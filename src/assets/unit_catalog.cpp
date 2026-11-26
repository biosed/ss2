#include "assets/unit_catalog.hpp"
#include "assets/mission.hpp"
#include "assets/sprite.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <system_error>

namespace ss2::assets {
namespace {

std::string asciiLower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view stripQuotes(std::string_view s) {
    s = trimView(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return trimView(s);
}

std::vector<std::string_view> splitTokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r')) {
            ++pos;
        }
        if (pos >= line.size()) break;
        const std::size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t' && line[pos] != '\r') {
            ++pos;
        }
        tokens.push_back(line.substr(start, pos - start));
    }
    return tokens;
}

bool parseInt(std::string_view tok, int& out) {
    tok = trimView(tok);
    if (tok.empty()) return false;
    int val{};
    const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), val);
    if (ec != std::errc{} || ptr != tok.data() + tok.size()) return false;
    out = val;
    return true;
}

bool parseIntPair(const std::vector<std::string_view>& toks, int& aOut, int& bOut) {
    if (toks.size() < 2) return false;
    int a = 0;
    if (!parseInt(toks[1], a)) return false;
    int b = a;
    if (toks.size() >= 3) {
        int bVal = 0;
        if (parseInt(toks[2], bVal)) b = bVal;
    }
    aOut = a;
    bOut = b;
    return true;
}

bool parseDouble(std::string_view tok, double& out) {
    const std::string buf(trimView(tok));
    if (buf.empty()) return false;
    char* end = nullptr;
    const double val = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str() || *end != '\0') return false;
    out = val;
    return true;
}

std::string_view restAfterKey(std::string_view line, std::string_view key) {
    const std::size_t keyPos = line.find(key);
    if (keyPos == std::string_view::npos) return {};
    return trimView(line.substr(keyPos + key.size()));
}

void parseWeaponField(WeaponSpec& w, std::string_view suffix, const std::vector<std::string_view>& toks,
                      const std::function<HitType(std::string_view)>& resolveShotHit) {
    if (toks.size() < 2) return;
    if (suffix == "id") {
        w.shotId = asciiLower(toks[1]);
        if (resolveShotHit && !w.shotId.empty() && w.shotId != "none") {
            w.hitType = resolveShotHit(w.shotId);
        }
    } else if (suffix == "damage") {
        int a = 0, b = 0;
        if (parseInt(toks[1], a)) {
            w.damage = a;
            w.damageMax = (toks.size() >= 3 && parseInt(toks[2], b)) ? b : a;
        }
    } else if (suffix == "range") {
        int a = 0, b = 0;
        if (parseInt(toks[1], a)) {
            w.range = a;
            w.rangeMax = (toks.size() >= 3 && parseInt(toks[2], b)) ? b : a;
        }
    } else if (suffix == "deadzone") {
        (void)parseInt(toks[1], w.deadzone);
    } else if (suffix == "reloadtime") {
        (void)parseInt(toks[1], w.reloadTicks);
    } else if (suffix == "burstshots") {
        (void)parseInt(toks[1], w.burstShots);
    } else if (suffix == "burstreloadtime") {
        (void)parseInt(toks[1], w.burstReloadTicks);
    } else if (suffix == "accuracy") {
        int a = 0, b = 0;
        if (parseInt(toks[1], a)) {
            w.accuracyBase = a;
            w.accuracyMin = (toks.size() >= 3 && parseInt(toks[2], b)) ? b : a;
        }
    } else if (suffix == "expa") {
        (void)parseInt(toks[1], w.expaPerShot);
    } else if (suffix == "sound") {
        const std::string snd = asciiLower(toks[1]);
        if (snd != "none") w.soundGroup = std::string(toks[1]);
    } else if (suffix == "endburstsound") {
        const std::string snd = asciiLower(toks[1]);
        if (!snd.empty() && snd != "none") w.endBurstSound = snd;
    } else if (suffix == "useammo") {
        int amt = 1;
        if (toks.size() >= 3 && parseInt(toks[2], amt) && amt >= 0) {
            w.ammoPerShot = amt;
        } else if (parseInt(toks[1], amt) && amt >= 0) {
            w.ammoPerShot = amt;
        }
    }
}

void parseTextBlock(UnitSpec& spec, std::string_view text, bool isLangOverride,
                    const std::function<HitType(std::string_view)>& resolveShotHit) {
    std::string quotedName;
    bool hasShot1Sound = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view line = trimView(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty()) continue;

        const auto toks = splitTokens(line);
        if (toks.empty()) continue;
        const std::string key = asciiLower(toks[0]);

        if (key == "*name") {
            const std::string decoded = decodeMissionText(restAfterKey(line, toks[0]));
            if (!decoded.empty() && (!isLangOverride || spec.displayName.empty())) {
                spec.displayName = decoded;
            }
            continue;
        }
        if (key == "name") {
            const std::string_view raw = stripQuotes(restAfterKey(line, toks[0]));
            if (!raw.empty() && raw.front() != '$' && quotedName.empty()) {
                quotedName = decodeMissionText(raw);
            }
            continue;
        }
        if (key == "shortname") {
            const std::string_view raw = stripQuotes(restAfterKey(line, toks[0]));
            if (!raw.empty() && raw.front() != '$') {
                if (!isLangOverride || spec.shortName.empty()) {
                    spec.shortName = decodeMissionText(raw);
                }
            }
            continue;
        }
        if (isLangOverride) continue;

        if (key == "health" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.health);
        } else if (key == "mech" && toks.size() >= 2) {
            int m = 0;
            if (parseInt(toks[1], m)) spec.isVehicle = (m != 0);
        } else if (key == "ammo1" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.ammo1);
        } else if (key == "ammo2" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.ammo2);
        } else if (key == "crew_number" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.crewNumber);
        } else if (key == "crew_unit" && toks.size() >= 2) {
            const std::string cu = asciiLower(toks[1]);
            if (cu != "none") spec.crewUnit = cu;
        } else if (key == "*guncrewunit" && toks.size() >= 2) {
            const std::string cu = asciiLower(toks[1]);
            if (spec.crewUnit.empty() && !cu.empty() && cu != "none") spec.crewUnit = cu;
        } else if (key == "turret_type" && toks.size() >= 2) {
            const std::string tt = asciiLower(toks[1]);
            if (!tt.empty() && tt != "none") spec.turretType = tt;
        } else if (key == "repair" && toks.size() >= 2) {
            double repVal = 0.0;
            if (parseDouble(toks[1], repVal) && repVal > 0.0) {
                spec.canSelfRepair = true;
                spec.repairCost = std::max(1, static_cast<int>(std::lround(repVal)));
            }
        } else if (key == "dieexplosion" && toks.size() >= 2) {
            const std::string de = asciiLower(toks[1]);
            if (de != "none") spec.dieExplosion = de;
        } else if (key == "scorevalue" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.scoreValue);
        } else if (key == "scoretype" && toks.size() >= 2) {
            spec.scoreType = asciiLower(toks[1]);
        } else if (key == "attackcrew" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.attackCrew);
        } else if (key == "movecrew" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.moveCrew);
        } else if (key == "walkonground" && toks.size() >= 2) {
            int b = 1;
            if (parseInt(toks[1], b)) spec.walkOnGround = (b != 0);
        } else if (key == "walkonwater" && toks.size() >= 2) {
            int b = 0;
            if (parseInt(toks[1], b)) spec.walkOnWater = (b != 0);
        } else if (key == "walkonshallows" && toks.size() >= 2) {
            int b = 1;
            if (parseInt(toks[1], b)) spec.walkOnShallows = (b != 0);
        } else if (key == "stock" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.stock);
        } else if (key == "sight" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.sight);
        } else if (key == "bonus_sight" && toks.size() >= 2 && !isLangOverride) {
            (void)parseInt(toks[1], spec.bonusSight);
        } else if (key == "marchsightbonus" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.marchSightBonus);
        } else if (key == "marchenabled" && toks.size() >= 2) {
            int b = 0;
            if (parseInt(toks[1], b)) spec.marchEnabled = (b != 0);
        } else if (key == "binocular" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.binocularSight);
        } else if (key == "kamikazepctg" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.kamikazePct);
        } else if (key == "movespeed" && toks.size() >= 2) {
            if (parseDouble(toks[1], spec.moveSpeed)) {
                double msMax = spec.moveSpeed;
                if (toks.size() >= 3 && parseDouble(toks[2], msMax) && msMax >= spec.moveSpeed) {
                    spec.moveSpeedMax = msMax;
                } else {
                    spec.moveSpeedMax = spec.moveSpeed;
                }
            }
        } else if (key == "crouchmovespeed" && toks.size() >= 2) {
            if (parseDouble(toks[1], spec.crouchMoveSpeed)) {
                double cmsMax = spec.crouchMoveSpeed;
                if (toks.size() >= 3 && parseDouble(toks[2], cmsMax) && cmsMax >= spec.crouchMoveSpeed) {
                    spec.crouchMoveSpeedMax = cmsMax;
                } else {
                    spec.crouchMoveSpeedMax = spec.crouchMoveSpeed;
                }
            }
        } else if (key == "seltype" && toks.size() >= 2) {
            spec.selType = asciiLower(toks[1]);
        } else if (key == "targettype" && toks.size() >= 2) {
            spec.targetType = asciiLower(toks[1]);
        } else if (key == "attackpref" && toks.size() >= 3) {
            const int idx = UnitSpec::targetClassIndex(toks[1]);
            int prefVal = 0;
            if (idx >= 0 && parseInt(toks[2], prefVal)) {
                spec.attackPref[static_cast<std::size_t>(idx)] = std::clamp(prefVal, 0, 255);
                spec.hasAttackPref = true;
            }
        } else if (key == "engine" && toks.size() >= 2) {
            spec.engineType = asciiLower(toks[1]);
        } else if (key == "*nationtype" && toks.size() >= 3) {
            spec.nation = asciiLower(toks[1]);
            spec.roleClass = asciiLower(toks[2]);
        } else if ((key == "native" || key == "camouflage") && toks.size() >= 2 && spec.nation.empty()) {
            const std::string val = asciiLower(toks[1]);
            if (!val.empty() && val != "none" && std::isalpha(static_cast<unsigned char>(val.front()))) {
                spec.nation = val;
            }
        } else if (key == "idlesnd_move" && toks.size() >= 2) {
            const std::string snd = asciiLower(toks[1]);
            if (snd != "none") spec.moveSound = std::string(toks[1]);
        } else if (key == "idlesnd_idle" && toks.size() >= 2) {
            const std::string snd = asciiLower(toks[1]);
            if (snd != "none") spec.idleSound = std::string(toks[1]);
        } else if (key == "movedamagehp" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.moveDamageHp);
        } else if (key == "ammoregendelay" && toks.size() >= 2) {
            (void)parseIntPair(toks, spec.ammoRegenDelay, spec.ammoRegenDelayMax);
        } else if (key == "healdelay" && toks.size() >= 2) {
            (void)parseIntPair(toks, spec.healDelay, spec.healDelayMax);
        } else if (key == "expa_reload" && toks.size() >= 2) {
            (void)parseIntPair(toks, spec.expaReload, spec.expaReloadMax);
        } else if (key == "mineammo" && toks.size() >= 2) {
            int v = 0;
            if (parseInt(toks[1], v) && v >= 0) spec.mineAmmo = v;
        } else if (key == "layminetime" && toks.size() >= 2) {
            int a = 0, b = 0;
            if (parseIntPair(toks, a, b)) {
                spec.layMineTime = a;
                spec.layMineTimeMax = (b > 0) ? b : a;
            }
        } else if (key == "getminetime" && toks.size() >= 2) {
            int a = 0, b = 0;
            if (parseIntPair(toks, a, b)) {
                spec.getMineTime = a;
                spec.getMineTimeMax = (b > 0) ? b : a;
            }
        } else if (key == "canmovebackward" && toks.size() >= 2) {
            int b = 0;
            if (parseInt(toks[1], b)) spec.canMoveBackward = (b != 0);
        } else if (key == "backmovespeed" && toks.size() >= 2) {
            (void)parseDouble(toks[1], spec.backMoveSpeed);
        } else if (key == "turndelay" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.turnDelay);
        } else if (key == "gunturndelay" && toks.size() >= 2) {
            if (parseInt(toks[1], spec.gunTurnDelay)) spec.hasGunTurnDelay = true;
        } else if (key == "canbecrushed" && toks.size() >= 2) {
            int b = 0;
            if (parseInt(toks[1], b)) spec.canBeCrushed = (b != 0);
        } else if (key == "davirange" && toks.size() >= 2) {
            spec.hasCrushRange = true;
        } else if (key == "passcanfire" && toks.size() >= 2) {
            int b = 0;
            if (parseInt(toks[1], b)) spec.passengersCanFire = (b != 0);
        } else if (key == "bonus_shotrange" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.bonusShotRange);
        } else if (key == "soldonarmor" && toks.size() >= 2) {
            const std::string val = asciiLower(toks[1]);
            if (!val.empty() && val != "none") spec.soldiersOnArmor = true;
        } else if (key == "officerradius" && toks.size() >= 2) {
            int rMin = 0, rMax = 0;
            if (toks.size() >= 3 && parseInt(toks[2], rMax) && rMax > 0) {
                spec.officerRadius = rMax;
            } else if (parseInt(toks[1], rMin) && rMin > 0) {
                spec.officerRadius = rMin;
            }
        } else if (key == "moraleautoincrease" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.moraleAutoIncrease);
        } else if (key == "moraleresist" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.moraleResist);
        } else if (key == "moralenoattack" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.moraleNoAttack);
        } else if (key == "moralerndmove" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.moraleRndMove);
        } else if (key == "moralerage" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.moraleRage);
        } else if (key == "reload1" && toks.size() >= 2) {
            int cost = 0;
            if (parseInt(toks[1], cost) && cost > 0) spec.shot1.resupplyCost = cost;
        } else if (key == "reload2" && toks.size() >= 2) {
            int cost = 0;
            if (parseInt(toks[1], cost) && cost > 0) spec.shot2.resupplyCost = cost;
        } else if (key == "gunshotwait" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.gunShotWait);
        } else if (key == "canbecrew" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.canBeCrew);
        } else if (key == "radius" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.unitRadius);
        } else if (key == "canfirefrominside" && toks.size() >= 2) {
            int b = 1;
            if (parseInt(toks[1], b)) {
                spec.canFireFromInside = (b != 0);
                spec.hasExplicitInsideRule = true;
            }
        } else if (key == "crouchtofire" && toks.size() >= 2) {
            int b = 0;
            if (parseInt(toks[1], b)) spec.crouchToFire = (b != 0);
        } else if (key == "crouchsight" && toks.size() >= 2) {
            int a = 0, b = 0;
            if (parseInt(toks[1], a) && a > 0) {
                spec.crouchSight = a;
                spec.crouchSightMax = (toks.size() >= 3 && parseInt(toks[2], b) && b >= a) ? b : a;
            }
        } else if (key == "crew_ver" && toks.size() >= 2) {
            double cv = 0.0;
            if (parseDouble(toks[1], cv) && cv > 0.0) spec.crewVulnerability = cv;
        } else if (key == "expa_loosehp" && toks.size() >= 2) {
            int xp = 0;
            if (parseInt(toks[1], xp) && xp > 0) spec.expaOnHit = xp;
        } else if (key == "soldtomove" && toks.size() >= 2) {
            int n = 0;
            if (parseInt(toks[1], n) && n > 0) spec.soldToMove = n;
        } else if (key == "movesmoke" && toks.size() >= 2) {
            const std::string ms = asciiLower(toks[1]);
            if (!ms.empty() && ms != "none") spec.hasMoveSmoke = true;
        } else if (key == "soundtype" && toks.size() >= 2) {
            const std::string st = asciiLower(toks[1]);
            if (!st.empty() && st != "none") spec.soundType = st;
        } else if (key == "crewcanbehealed" && toks.size() >= 2) {
            int b = 0;
            if (parseInt(toks[1], b)) spec.crewCanBeHealed = (b != 0);
        } else if (key == "canbepara" && toks.size() >= 2) {
            int b = 0;
            if (parseInt(toks[1], b)) spec.canBeParadropped = (b != 0);
        } else if (key == "canattackpoint" && toks.size() >= 2) {
            (void)parseInt(toks[1], spec.canAttackPoint);
        } else if (key == "bonus_shotdeadzone" && toks.size() >= 2) {
            int v = 0;
            if (parseInt(toks[1], v) && v >= 0) spec.bonusShotDeadZone = v;
        } else if (key == "scandelay" && toks.size() >= 2) {
            int v = 0;
            if (parseInt(toks[1], v) && v > 0) spec.scanDelay = v;
        } else if (key == "expa_crush" && toks.size() >= 2) {
            int v = 0;
            if (parseInt(toks[1], v) && v > 0) spec.expaCrush = v;
        } else if (key == "cancrouchexpa") {
            int v = 1;
            if (toks.size() >= 2) (void)parseInt(toks[1], v);
            spec.canCrouchExpa = (v != 0);
        } else if (key == "canbetowed" && toks.size() >= 2) {
            spec.towWeightClass = asciiLower(toks[1]);
        } else if (key == "havepricep" && toks.size() >= 2) {
            static constexpr std::array<double, 6> kDefaultTowSpeed{1.0, 0.70, 0.60, 0.52, 0.45, 0.35};
            for (std::size_t ti = 1; ti < toks.size(); ++ti) {
                const int idx = UnitSpec::towClassIndex(asciiLower(toks[ti]));
                if (idx >= 0) {
                    auto& slot = spec.towSpeedByClass[static_cast<std::size_t>(idx)];
                    if (slot <= 0.0) slot = kDefaultTowSpeed[static_cast<std::size_t>(idx)];
                }
            }
        } else if (key == "pricepspeed" && toks.size() >= 3) {
            const int idx = UnitSpec::towClassIndex(asciiLower(toks[1]));
            double mult = 0.0;
            if (idx >= 0 && parseDouble(toks[2], mult) && mult > 0.0) {
                spec.towSpeedByClass[static_cast<std::size_t>(idx)] = std::clamp(mult, 0.15, 1.0);
            }
        } else if (key.starts_with("shot_")) {
            const std::string_view suffix = std::string_view(key).substr(5);
            if (suffix == "sound" && hasShot1Sound) {
                continue;
            }
            parseWeaponField(spec.shot1, suffix, toks, resolveShotHit);
        } else if (key.starts_with("shot1_")) {
            const std::string_view suffix = std::string_view(key).substr(6);
            parseWeaponField(spec.shot1, suffix, toks, resolveShotHit);
            if (suffix == "sound" && !spec.shot1.soundGroup.empty()) {
                hasShot1Sound = true;
            }
        } else if (key.starts_with("shot2_")) {
            parseWeaponField(spec.shot2, std::string_view(key).substr(6), toks, resolveShotHit);
        } else if (key == "armor" && toks.size() >= 8) {
            const HitType ht = UnitCatalog::parseHitTypeName(toks[1]);
            if (ht != HitType::None) {
                int f0 = 0, s0 = 0, r0 = 0;
                if (parseInt(toks[2], f0) && parseInt(toks[4], s0) && parseInt(toks[6], r0)) {
                    spec.armor[static_cast<std::size_t>(ht)] = DirectionalArmor{f0, s0, r0};
                }
            }
        } else if (key == "protection" && toks.size() >= 8) {
            const HitType ht = UnitCatalog::parseHitTypeName(toks[1]);
            if (ht != HitType::None) {
                int f0 = 0, s0 = 0, r0 = 0;
                if (parseInt(toks[2], f0) && parseInt(toks[4], s0) && parseInt(toks[6], r0)) {
                    spec.crewProtection[static_cast<std::size_t>(ht)] = DirectionalArmor{f0, s0, r0};
                    spec.hasCrewProtection = true;
                }
            }
        }
    }
    if (spec.displayName.empty() && !quotedName.empty()) {
        spec.displayName = std::move(quotedName);
    }
}

} // namespace

HitType UnitCatalog::parseHitTypeName(std::string_view token) {
    const std::string low = asciiLower(trimView(token));
    if (low == "pierce") return HitType::Pierce;
    if (low == "transpierce") return HitType::Transpierce;
    if (low == "explosive") return HitType::Explosive;
    if (low == "machine") return HitType::Machine;
    if (low == "sniper") return HitType::Sniper;
    if (low == "fire") return HitType::Fire;
    if (low == "mine") return HitType::Mine;
    if (low == "abjective") return HitType::Abjective;
    if (low == "air") return HitType::Air;
    return HitType::None;
}

UnitSpec UnitCatalog::parseUnitText(std::string_view unitId, std::string_view descText,
                                    std::string_view langText,
                                    const std::function<HitType(std::string_view)>& resolveShotHit) {
    UnitSpec spec;
    spec.id = asciiLower(trimView(unitId));
    parseTextBlock(spec, descText, false, resolveShotHit);
    if (!langText.empty()) {
        parseTextBlock(spec, langText, true, resolveShotHit);
    }
    if (spec.displayName.empty()) spec.displayName = spec.shortName;
    if (spec.shortName.empty()) spec.shortName = spec.displayName;
    return spec;
}

std::shared_ptr<const UnitCatalog> UnitCatalog::load(const std::filesystem::path& gameDir,
                                                   unsigned scheme) {
    auto catalog = std::make_shared<UnitCatalog>();
    if (gameDir.empty()) return catalog;

    catalog->descArchives_ = resolveDescArchives(gameDir, scheme);
    catalog->langArchives_ = resolveLangArchives(gameDir, scheme);
    catalog->hasDesc_ = !catalog->descArchives_.empty();
    catalog->hasLang_ = !catalog->langArchives_.empty();

    if (catalog->hasDesc_) {
        bool parsedAnyDef = false;
        bool sawTranspierceArmor = false;
        bool sawTranspierceProt = false;
        std::map<int, int> parsedStarsByLevel;
        bool parsedPara = false;
        bool parsedMine = false;

        for (std::size_t idx = catalog->descArchives_.size(); idx-- > 0;) {
            const auto& descPath = catalog->descArchives_[idx];
            if (const auto defBytes = tryReadSueEntry(descPath, "LAND\\DEFDOM")) {
                const std::string_view defText(reinterpret_cast<const char*>(defBytes->data()), defBytes->size());
                std::size_t pos = 0;
                while (pos < defText.size()) {
                    std::size_t eol = defText.find('\n', pos);
                    if (eol == std::string_view::npos) eol = defText.size();
                    const auto toks = splitTokens(defText.substr(pos, eol - pos));
                    pos = eol + 1;
                    if (toks.size() < 4) continue;
                    const std::string k = asciiLower(toks[0]);
                    if ((k == "armor" || k == "protection") && asciiLower(toks[1]) == "default") {
                        const HitType ht = parseHitTypeName(toks[2]);
                        int val = 0;
                        if (ht != HitType::None && parseInt(toks[3], val)) {
                            if (k == "armor") {
                                catalog->buildingDefense_.armor[static_cast<std::size_t>(ht)] = val;
                                if (ht == HitType::Transpierce) sawTranspierceArmor = true;
                            } else {
                                catalog->buildingDefense_.protection[static_cast<std::size_t>(ht)] = val;
                                if (ht == HitType::Transpierce) sawTranspierceProt = true;
                            }
                            parsedAnyDef = true;
                        }
                    }
                }
            }
            if (const auto domBytes = tryReadSueEntry(descPath, "LAND\\DOM")) {
                const std::string_view domText(reinterpret_cast<const char*>(domBytes->data()), domBytes->size());
                std::size_t pos = 0;
                while (pos < domText.size()) {
                    std::size_t eol = domText.find('\n', pos);
                    if (eol == std::string_view::npos) eol = domText.size();
                    const auto toks = splitTokens(domText.substr(pos, eol - pos));
                    pos = eol + 1;
                    if (toks.size() < 3) continue;
                    const std::string k = asciiLower(toks[0]);
                    if (asciiLower(toks[1]) != "default") continue;
                    int val = 0;
                    if (k == "unloadhp" && parseInt(toks[2], val) && val > 0) {
                        catalog->buildingDefense_.unloadHp = val;
                    } else if (k == "bonus_shotrange" && parseInt(toks[2], val) && val > 0) {
                        catalog->buildingDefense_.bonusShotRange = val;
                    } else if (k == "bonus_shotdeadzone" && parseInt(toks[2], val) && val > 0) {
                        catalog->buildingDefense_.bonusShotDeadZone = val;
                    }
                }
            }
            if (const auto miscBytes = tryReadSueEntry(descPath, "MISC\\MISC")) {
                const std::string_view miscText(reinterpret_cast<const char*>(miscBytes->data()), miscBytes->size());
                std::size_t pos = 0;
                while (pos < miscText.size()) {
                    std::size_t eol = miscText.find('\n', pos);
                    if (eol == std::string_view::npos) eol = miscText.size();
                    const auto toks = splitTokens(miscText.substr(pos, eol - pos));
                    pos = eol + 1;
                    if (toks.size() >= 3 && asciiLower(toks[0]) == "starscore") {
                        int star = 0;
                        int thresh = 0;
                        if (parseInt(toks[1], star) && parseInt(toks[2], thresh) && star > 0) {
                            parsedStarsByLevel[star] = thresh;
                        }
                    } else if (toks.size() >= 3 && asciiLower(toks[0]) == "tanksonarmorprotection") {
                        const HitType ht = parseHitTypeName(toks[1]);
                        int prot = 0;
                        if (parseInt(toks[2], prot) && prot >= 0) {
                            if (ht == HitType::Pierce) catalog->tankArmorProtectionPierce_ = prot;
                            else if (ht == HitType::Explosive) catalog->tankArmorProtectionExplosive_ = prot;
                        }
                    }
                }
            }
            if (const auto paraBytes = tryReadSueEntry(descPath, "UNITS\\para")) {
                const std::string_view paraText(reinterpret_cast<const char*>(paraBytes->data()), paraBytes->size());
                std::size_t pos = 0;
                while (pos < paraText.size()) {
                    std::size_t eol = paraText.find('\n', pos);
                    if (eol == std::string_view::npos) eol = paraText.size();
                    const auto toks = splitTokens(paraText.substr(pos, eol - pos));
                    pos = eol + 1;
                    if (toks.size() < 2) continue;
                    const std::string k = asciiLower(toks[0]);
                    int iv = 0;
                    double dv = 0.0;
                    if (k == "fallspeed" && parseDouble(toks[1], dv) && dv > 0.0) {
                        catalog->paraSpec_.fallSpeed = dv;
                        parsedPara = true;
                    } else if (k == "paraanicenter" && parseInt(toks[1], iv) && iv >= 0) {
                        catalog->paraSpec_.paraAniCenter = iv;
                        parsedPara = true;
                    } else if (k == "paraframedelay" && parseInt(toks[1], iv) && iv > 0) {
                        catalog->paraSpec_.paraFrameDelay = iv;
                        parsedPara = true;
                    } else if (k == "sight" && parseInt(toks[1], iv) && iv > 0) {
                        catalog->paraSpec_.sight = iv;
                        parsedPara = true;
                    }
                }
            }
            if (const auto mineBytes = tryReadSueEntry(descPath, "MISC\\MINES")) {
                const std::string_view mineText(reinterpret_cast<const char*>(mineBytes->data()), mineBytes->size());
                std::size_t pos = 0;
                while (pos < mineText.size()) {
                    std::size_t eol = mineText.find('\n', pos);
                    if (eol == std::string_view::npos) eol = mineText.size();
                    const auto toks = splitTokens(mineText.substr(pos, eol - pos));
                    pos = eol + 1;
                    if (toks.size() < 2) continue;
                    const std::string k = asciiLower(toks[0]);
                    int val = 0;
                    if (k == "damage" && parseInt(toks[1], val) && val > 0) {
                        catalog->mineSpec_.damage = val;
                        parsedMine = true;
                    } else if (k == "radius" && parseInt(toks[1], val) && val > 0) {
                        catalog->mineSpec_.radius = val;
                        parsedMine = true;
                    } else if (k == "expldelay" && parseInt(toks[1], val) && val > 0) {
                        catalog->mineSpec_.explDelayTicks = val;
                        parsedMine = true;
                    } else if (k == "shot") {
                        const std::string s = asciiLower(toks[1]);
                        if (!s.empty() && s != "none") {
                            catalog->mineSpec_.atShot = s;
                            parsedMine = true;
                        }
                    } else if (k == "shotsold") {
                        const std::string s = asciiLower(toks[1]);
                        if (!s.empty() && s != "none") {
                            catalog->mineSpec_.apShot = s;
                            parsedMine = true;
                        }
                    }
                }
            }
            if (const auto ifaceBytes = tryReadSueEntry(descPath, "MISC\\INTERFACE")) {
                const std::string_view ifaceText(reinterpret_cast<const char*>(ifaceBytes->data()), ifaceBytes->size());
                std::size_t pos = 0;
                while (pos < ifaceText.size()) {
                    std::size_t eol = ifaceText.find('\n', pos);
                    if (eol == std::string_view::npos) eol = ifaceText.size();
                    const auto toks = splitTokens(ifaceText.substr(pos, eol - pos));
                    pos = eol + 1;
                    if (toks.size() >= 2 && asciiLower(toks[0]) == "underattackinterval") {
                        int val = 0;
                        if (parseInt(toks[1], val) && val > 0) {
                            catalog->underAttackIntervalSec_ = std::clamp(val, 1, 120);
                        }
                    }
                }
            }
            if (const auto weatherBytes = tryReadSueEntry(descPath, "MISC\\WEATHER")) {
                const std::string_view weatherText(reinterpret_cast<const char*>(weatherBytes->data()),
                                                     weatherBytes->size());
                std::size_t pos = 0;
                while (pos < weatherText.size()) {
                    std::size_t eol = weatherText.find('\n', pos);
                    if (eol == std::string_view::npos) eol = weatherText.size();
                    const auto toks = splitTokens(weatherText.substr(pos, eol - pos));
                    pos = eol + 1;
                    if (toks.size() < 2) continue;
                    const std::string k = asciiLower(toks[0]);
                    const auto parseTint = [&](WeatherTint& dst) {
                        if (toks.size() < 5) return;
                        int r = 0, g = 0, b = 0;
                        double a = 0.0;
                        if (parseInt(toks[1], r) && parseInt(toks[2], g) && parseInt(toks[3], b) && parseDouble(toks[4], a)) {
                            dst.r = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
                            dst.g = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
                            dst.b = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
                            dst.alpha = static_cast<float>(std::clamp(a, 0.0, 1.0));
                            catalog->weather_.loaded = true;
                        }
                    };
                    int val = 0;
                    if (k == "rainvisibility" && parseInt(toks[1], val)) {
                        catalog->weather_.rainVisibilityPct = std::clamp(val, 0, 100);
                        catalog->weather_.loaded = true;
                    } else if (k == "fogvisibility" && parseInt(toks[1], val)) {
                        catalog->weather_.fogVisibilityPct = std::clamp(val, 0, 100);
                        catalog->weather_.loaded = true;
                    } else if (k == "snowvisibility" && parseInt(toks[1], val)) {
                        catalog->weather_.snowVisibilityPct = std::clamp(val, 0, 100);
                        catalog->weather_.loaded = true;
                    } else if (k == "showalways" && parseInt(toks[1], val)) {
                        catalog->weather_.showAlways = (val != 0);
                        catalog->weather_.loaded = true;
                    } else if (k == "fogrgbc") {
                        parseTint(catalog->weather_.fogTint);
                    } else if (k == "rainrgbc") {
                        parseTint(catalog->weather_.rainTint);
                    } else if (k == "snowrgbc") {
                        parseTint(catalog->weather_.snowTint);
                    }
                }
            }
        }

        if (parsedAnyDef) {
            if (!sawTranspierceArmor) {
                catalog->buildingDefense_.armor[static_cast<std::size_t>(HitType::Transpierce)] =
                    catalog->buildingDefense_.armorFor(HitType::Pierce);
            }
            if (!sawTranspierceProt) {
                catalog->buildingDefense_.protection[static_cast<std::size_t>(HitType::Transpierce)] =
                    catalog->buildingDefense_.protectionFor(HitType::Pierce);
            }
            catalog->buildingDefense_.loaded = true;
        }
        if (!parsedStarsByLevel.empty()) {
            std::vector<std::pair<int, int>> sortedStars;
            sortedStars.reserve(parsedStarsByLevel.size());
            for (const auto& [st, th] : parsedStarsByLevel) {
                sortedStars.emplace_back(st, th);
            }
            std::sort(sortedStars.begin(), sortedStars.end(),
                      [](const auto& a, const auto& b) { return a.second < b.second; });
            catalog->starScores_ = std::move(sortedStars);
        }
        if (parsedPara) catalog->paraSpec_.loaded = true;
        if (parsedMine) {
            const auto atInfo = catalog->resolveShotInfoLocked(catalog->mineSpec_.atShot);
            if (!atInfo.impactSound.empty()) catalog->mineSpec_.atSound = atInfo.impactSound;
            const auto apInfo = catalog->resolveShotInfoLocked(catalog->mineSpec_.apShot);
            if (!apInfo.impactSound.empty()) catalog->mineSpec_.apSound = apInfo.impactSound;
            catalog->mineSpec_.loaded = true;
        }
    }

    auto parseDescdomNames = [&](const std::vector<std::filesystem::path>& archives) {
        for (std::size_t idx = archives.size(); idx-- > 0;) {
            const auto bytes = tryReadSueEntry(archives[idx], "LAND\\DESCDOM");
            if (!bytes) continue;
            const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            std::size_t pos = 0;
            while (pos < text.size()) {
                std::size_t eol = text.find('\n', pos);
                if (eol == std::string_view::npos) eol = text.size();
                const std::string_view line = trimView(text.substr(pos, eol - pos));
                pos = eol + 1;
                const auto toks = splitTokens(line);
                if (toks.size() < 4 || asciiLower(toks[0]) != "name" || asciiLower(toks[1]) != "obj") continue;
                int objIdx = -1;
                if (!parseInt(toks[2], objIdx) || objIdx < 0) continue;
                const auto afterObj = restAfterKey(line, toks[2]);
                const std::string name = decodeMissionText(stripQuotes(afterObj));
                if (!name.empty() && name.front() != '$') {
                    catalog->buildingNames_[static_cast<unsigned>(objIdx)] = name;
                }
            }
        }
    };
    parseDescdomNames(catalog->descArchives_);
    parseDescdomNames(catalog->langArchives_);
    return catalog;
}

std::shared_ptr<const UnitCatalog> UnitCatalog::fromSpecs(std::vector<UnitSpec> specs,
                                                          std::vector<PlaneSpec> planes,
                                                          bool retailCatalog) {
    auto catalog = std::make_shared<UnitCatalog>();
    catalog->hasDesc_ = retailCatalog;
    for (auto& spec : specs) {
        std::string key = asciiLower(trimView(spec.id));
        if (key.empty()) continue;
        spec.id = key;
        auto entry = std::make_unique<UnitSpec>(std::move(spec));
        catalog->cache_.insert_or_assign(std::move(key), std::move(entry));
    }
    for (auto& plane : planes) {
        std::string key = asciiLower(trimView(plane.id));
        if (key.empty()) continue;
        plane.id = key;
        auto entry = std::make_unique<PlaneSpec>(std::move(plane));
        catalog->planeCache_.insert_or_assign(std::move(key), std::move(entry));
    }
    return catalog;
}

PlaneSpec UnitCatalog::parsePlaneText(std::string_view planeId, std::string_view descText) {
    PlaneSpec spec;
    spec.id = asciiLower(trimView(planeId));
    std::size_t pos = 0;
    while (pos < descText.size()) {
        std::size_t eol = descText.find('\n', pos);
        if (eol == std::string_view::npos) eol = descText.size();
        const auto toks = splitTokens(descText.substr(pos, eol - pos));
        pos = eol + 1;
        if (toks.size() < 2) continue;
        const std::string key = asciiLower(toks[0]);
        if (key == "health") {
            (void)parseInt(toks[1], spec.health);
        } else if (key == "scorevalue") {
            (void)parseInt(toks[1], spec.scoreValue);
        } else if (key == "sight") {
            (void)parseInt(toks[1], spec.sight);
        } else if (key == "flyspeed") {
            (void)parseDouble(toks[1], spec.flySpeed);
        } else if (key == "dropflyspeed") {
            double dfs = 0.0;
            if (parseDouble(toks[1], dfs) && dfs > 0.0) spec.dropFlySpeed = dfs;
        } else if (key == "bombdamage") {
            (void)parseInt(toks[1], spec.bombDamage);
        } else if (key == "bombsnumber") {
            (void)parseInt(toks[1], spec.bombsNumber);
        } else if (key == "shot_grn_damage") {
            (void)parseInt(toks[1], spec.groundDamage);
        } else if (key == "shot_air_damage") {
            (void)parseInt(toks[1], spec.airDamage);
        } else if (key == "idlesound") {
            const std::string snd = asciiLower(toks[1]);
            if (!snd.empty() && snd != "none") spec.idleSound = snd;
        } else if (key == "bombsound") {
            const std::string snd = asciiLower(toks[1]);
            if (!snd.empty() && snd != "none") spec.bombSound = snd;
        } else if (key == "fallsound") {
            const std::string snd = asciiLower(toks[1]);
            if (!snd.empty() && snd != "none") spec.fallSound = snd;
        } else if (key == "shot_grn_sound") {
            const std::string snd = asciiLower(toks[1]);
            if (!snd.empty() && snd != "none") spec.groundShotSound = snd;
        } else if (key == "shot_air_sound") {
            const std::string snd = asciiLower(toks[1]);
            if (!snd.empty() && snd != "none") spec.airShotSound = snd;
        } else if (key == "bombid") {
            const std::string bid = asciiLower(toks[1]);
            if (!bid.empty() && bid != "none") spec.bombId = bid;
        } else if (key == "fallsmoke" || key == "flysmoke") {
            const std::string sm = asciiLower(toks[1]);
            if (!sm.empty() && sm != "none") spec.hasFallSmoke = true;
        } else if (key == "altitude") {
            int alt = 0;
            if (parseInt(toks[1], alt) && alt > 0) spec.altitude = alt;
        } else if (key == "shot_grn_ammo") {
            int ga = 0;
            if (parseInt(toks[1], ga) && ga > 0) spec.groundAmmo = ga;
        } else if (key == "shot_grn_id") {
            const std::string sid = asciiLower(toks[1]);
            if (!sid.empty() && sid != "none") spec.groundShotId = sid;
        } else if (key == "actiontime" || key == "action_time") {
            int v = 0;
            if (parseInt(toks[1], v) && v > 0) spec.actionTime = v;
        } else if (key == "bombaccuracy" || key == "bomb_accuracy") {
            int v = 0;
            if (parseInt(toks[1], v) && v > 0) spec.bombAccuracy = v;
        } else if (key == "bombreloadtime" || key == "bomb_reload_time") {
            int v = 0;
            if (parseInt(toks[1], v) && v > 0) spec.bombReloadTime = v;
        }
    }
    return spec;
}

int UnitCatalog::starRatingForScore(int score) const {
    int stars = 0;
    for (const auto& [s, threshold] : starScores_) {
        if (score >= threshold) stars = std::max(stars, s);
    }
    return stars;
}

UnitCatalog::ShotInfo UnitCatalog::resolveShotInfoLocked(std::string_view shotId) const {
    const std::string key = asciiLower(trimView(shotId));
    if (key.empty() || key == "none" || !hasDesc_) return {};
    if (const auto it = shotInfoCache_.find(key); it != shotInfoCache_.end()) {
        return it->second;
    }
    ShotInfo info{};
    std::string defaultExpl;
    std::string fallbackExpl;
    try {
        if (const auto bytes = tryReadSueEntryChain(descArchives_, "SHOTS\\" + key)) {
            const std::string_view shotText(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            std::size_t pos = 0;
            while (pos < shotText.size()) {
                std::size_t eol = shotText.find('\n', pos);
                if (eol == std::string_view::npos) eol = shotText.size();
                const auto toks = splitTokens(shotText.substr(pos, eol - pos));
                pos = eol + 1;
                if (toks.size() < 2) continue;
                const std::string k = asciiLower(toks[0]);
                if (k == "hitid") {
                    info.hitType = parseHitTypeName(toks[1]);
                } else if (k == "useh") {
                    double val = 0.0;
                    if (parseDouble(toks[1], val) && val >= 2.0) info.indirectArc = true;
                } else if (k == "explosion" && toks.size() >= 3) {
                    const std::string surf = asciiLower(toks[1]);
                    const std::string expl = asciiLower(toks[2]);
                    if (!expl.empty() && expl != "none") {
                        if (surf == "default") defaultExpl = expl;
                        else if (fallbackExpl.empty()) fallbackExpl = expl;
                    }
                }
            }
        }
    } catch (const std::exception&) {}

    const std::string chosenExpl = !defaultExpl.empty() ? defaultExpl : fallbackExpl;
    if (!chosenExpl.empty() && chosenExpl != "none") {
        try {
            if (const auto eBytes = tryReadSueEntryChain(descArchives_, "EXPLOSIONS\\" + chosenExpl)) {
                const std::string_view explText(reinterpret_cast<const char*>(eBytes->data()), eBytes->size());
                std::size_t pos = 0;
                while (pos < explText.size()) {
                    std::size_t eol = explText.find('\n', pos);
                    if (eol == std::string_view::npos) eol = explText.size();
                    const auto toks = splitTokens(explText.substr(pos, eol - pos));
                    pos = eol + 1;
                    if (toks.size() < 2) continue;
                    const std::string k = asciiLower(toks[0]);
                    if (k == "vor") {
                        int v = 0;
                        if (asciiLower(toks[1]) != "none" && parseInt(toks[1], v) && v > 0) {
                            info.craterScale = v;
                        }
                    } else if (k == "sound") {
                        const std::string snd = asciiLower(toks[1]);
                        if (!snd.empty() && snd != "none") info.impactSound = snd;
                    }
                }
            }
        } catch (const std::exception&) {}
    }

    shotInfoCache_.emplace(key, info);
    return info;
}

HitType UnitCatalog::resolveShotHitLocked(std::string_view shotId) const {
    return resolveShotInfoLocked(shotId).hitType;
}

const UnitSpec* UnitCatalog::find(std::string_view unitId) const {
    const std::string key = asciiLower(trimView(unitId));
    if (key.empty()) return nullptr;

    std::lock_guard lock(mutex_);
    if (const auto it = cache_.find(key); it != cache_.end()) {
        return it->second.get();
    }
    if (misses_.contains(key) || !hasDesc_) {
        return nullptr;
    }

    std::optional<Bytes> descOpt;
    try {
        descOpt = tryReadSueEntryChain(descArchives_, "UNITS\\" + key);
    } catch (const std::exception&) {
    }
    if (!descOpt) {
        misses_.insert(key);
        return nullptr;
    }
    Bytes descBytes = std::move(*descOpt);

    Bytes langBytes;
    if (hasLang_) {
        try {
            if (auto lb = tryReadSueEntryChain(langArchives_, "UNITS\\" + key)) {
                langBytes = std::move(*lb);
            }
        } catch (const std::exception&) {}
    }

    const std::string_view descText(reinterpret_cast<const char*>(descBytes.data()), descBytes.size());
    const std::string_view langText(reinterpret_cast<const char*>(langBytes.data()), langBytes.size());
    auto spec = std::make_unique<UnitSpec>(parseUnitText(
        key, descText, langText,
        [this](std::string_view shotId) { return resolveShotHitLocked(shotId); }));
    auto populateWeaponExtra = [&](WeaponSpec& w) {
        if (w.shotId.empty() || w.shotId == "none") return;
        const auto sInfo = resolveShotInfoLocked(w.shotId);
        if (w.hitType == HitType::None) w.hitType = sInfo.hitType;
        w.indirectArc = sInfo.indirectArc;
        w.craterScale = sInfo.craterScale;
        w.impactSound = sInfo.impactSound;
    };
    populateWeaponExtra(spec->shot1);
    populateWeaponExtra(spec->shot2);
    const UnitSpec* ptr = spec.get();
    cache_.emplace(key, std::move(spec));
    return ptr;
}

const PlaneSpec* UnitCatalog::findPlane(std::string_view planeId) const {
    const std::string key = asciiLower(trimView(planeId));
    if (key.empty()) return nullptr;

    std::lock_guard lock(mutex_);
    if (const auto it = planeCache_.find(key); it != planeCache_.end()) {
        return it->second.get();
    }
    if (planeMisses_.contains(key) || !hasDesc_) {
        return nullptr;
    }

    std::optional<Bytes> descOpt;
    try {
        descOpt = tryReadSueEntryChain(descArchives_, "AVIA\\" + key);
    } catch (const std::exception&) {
    }
    if (!descOpt) {
        planeMisses_.insert(key);
        return nullptr;
    }
    Bytes descBytes = std::move(*descOpt);

    const std::string_view descText(reinterpret_cast<const char*>(descBytes.data()), descBytes.size());
    auto spec = std::make_unique<PlaneSpec>(parsePlaneText(key, descText));
    if (!spec->groundShotId.empty()) {
        spec->groundHitType = resolveShotInfoLocked(spec->groundShotId).hitType;
    }
    const PlaneSpec* ptr = spec.get();
    planeCache_.emplace(key, std::move(spec));
    return ptr;
}

std::string UnitCatalog::displayName(std::string_view unitId) const {
    if (const auto* spec = find(unitId); spec && !spec->displayName.empty()) {
        return spec->displayName;
    }
    return {};
}

std::string UnitCatalog::shortName(std::string_view unitId) const {
    if (const auto* spec = find(unitId); spec && !spec->shortName.empty()) {
        return spec->shortName;
    }
    return {};
}

std::string UnitCatalog::buildingName(unsigned objIndex) const {
    if (const auto it = buildingNames_.find(objIndex); it != buildingNames_.end()) {
        return it->second;
    }
    return {};
}

} // namespace ss2::assets
