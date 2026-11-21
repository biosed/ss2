#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ss2::assets {

enum class HitType : std::uint8_t {
    None = 0,
    Pierce,
    Transpierce,
    Explosive,
    Machine,
    Sniper,
    Fire,
    Mine,
    Abjective,
    Air,
    Count
};

struct DirectionalArmor {
    int front{0};
    int side{0};
    int rear{0};
};

inline constexpr std::array<DirectionalArmor, static_cast<std::size_t>(HitType::Count)>
makeDefaultCrewProtection() {
    std::array<DirectionalArmor, static_cast<std::size_t>(HitType::Count)> out{};
    for (auto& d : out) d = DirectionalArmor{256, 256, 256};
    return out;
}

enum class TargetClass : std::uint8_t {
    Apc = 0,
    Boat,
    Building,
    Car,
    Cruiser,
    Gun,
    HeavyTank,
    LightTank,
    Man,
    MedTank,
    SmallGear,
    Soft,
    Train,
    Turret,
    Count
};

inline constexpr std::array<int, static_cast<std::size_t>(TargetClass::Count)>
makeDefaultAttackPref() {
    return std::array<int, static_cast<std::size_t>(TargetClass::Count)>{};
}

struct WeaponSpec {
    std::string shotId;
    HitType hitType{HitType::None};
    int damage{0};
    int damageMax{0};
    int range{0};
    int rangeMax{0};
    int deadzone{0};
    int reloadTicks{0};
    int burstShots{1};
    int burstReloadTicks{0};
    std::string soundGroup;
    int accuracyBase{0};
    int accuracyMin{0};
    int expaPerShot{0};
    int resupplyCost{0};
    std::string endBurstSound;
    int ammoPerShot{1};
    bool indirectArc{false};
    int craterScale{0};
    std::string impactSound;

    bool isFlamethrower() const {
        return hitType == HitType::Fire || soundGroup == "fmw" || shotId.find("flame") != std::string::npos;
    }
};

struct UnitSpec {
    std::string id;
    std::string displayName;
    std::string shortName;
    std::string selType;
    std::string targetType;
    bool isVehicle{false};
    int health{100};
    int ammo1{0};
    int ammo2{0};
    int sight{12};
    int bonusSight{0};
    int marchSightBonus{0};
    bool marchEnabled{false};
    int binocularSight{0};
    int kamikazePct{0};
    int crewNumber{0};
    int stock{0};
    double moveSpeed{0.0};
    double moveSpeedMax{0.0};
    double crouchMoveSpeed{0.0};
    double crouchMoveSpeedMax{0.0};
    WeaponSpec shot1{};
    WeaponSpec shot2{};
    std::array<DirectionalArmor, static_cast<std::size_t>(HitType::Count)> armor{};
    std::array<DirectionalArmor, static_cast<std::size_t>(HitType::Count)> crewProtection{makeDefaultCrewProtection()};
    bool hasCrewProtection{false};
    std::string crewUnit;
    bool canSelfRepair{false};
    int repairCost{0};
    std::string dieExplosion;
    int scoreValue{0};
    std::string scoreType;
    int attackCrew{0};
    int moveCrew{0};
    bool walkOnGround{true};
    bool walkOnWater{false};
    bool walkOnShallows{true};
    std::string nation;
    std::string roleClass;
    std::string turretType;
    std::string moveSound;
    std::string idleSound;
    std::string soundType;
    std::string engineType;
    std::array<int, static_cast<std::size_t>(TargetClass::Count)> attackPref{makeDefaultAttackPref()};
    bool hasAttackPref{false};
    int moveDamageHp{0};
    int ammoRegenDelay{0};
    int ammoRegenDelayMax{0};
    int healDelay{0};
    int healDelayMax{0};
    int expaReload{0};
    int expaReloadMax{0};
    int mineAmmo{0};
    int layMineTime{0};
    int layMineTimeMax{0};
    int getMineTime{0};
    int getMineTimeMax{0};
    bool canMoveBackward{false};
    double backMoveSpeed{0.0};
    int turnDelay{8};
    int gunTurnDelay{4};
    bool hasGunTurnDelay{false};
    bool canBeCrushed{false};
    bool hasCrushRange{false};
    bool passengersCanFire{false};
    int bonusShotRange{0};
    bool soldiersOnArmor{false};
    int officerRadius{0};
    int moraleAutoIncrease{0};
    int moraleResist{0};
    int moraleNoAttack{0};
    int moraleRndMove{0};
    int moraleRage{0};
    int gunShotWait{0};
    int canBeCrew{0};
    int unitRadius{0};
    bool canFireFromInside{true};
    bool hasExplicitInsideRule{false};
    bool crouchToFire{false};
    int crouchSight{0};
    int crouchSightMax{0};
    double crewVulnerability{0.0};
    int expaOnHit{0};
    int soldToMove{0};
    bool hasMoveSmoke{false};
    bool crewCanBeHealed{false};
    bool canBeParadropped{false};
    int canAttackPoint{0};
    int bonusShotDeadZone{0};
    int scanDelay{0};
    int expaCrush{0};
    std::string towWeightClass;
    std::array<double, 6> towSpeedByClass{};

    static bool iequalsAscii(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char c = a[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != b[i]) return false;
        }
        return true;
    }

    static int towClassIndex(std::string_view cls) {
        static constexpr std::array<std::string_view, 6> kClasses{
            "weightless", "light", "handy", "medium", "heavy", "veryheavy"
        };
        for (std::size_t i = 0; i < kClasses.size(); ++i) {
            if (iequalsAscii(cls, kClasses[i])) return static_cast<int>(i);
        }
        return -1;
    }

    static int targetClassIndex(std::string_view cls) {
        static constexpr std::array<std::string_view, static_cast<std::size_t>(TargetClass::Count)> kTargets{
            "apc", "boat", "building", "car", "cruiser", "gun", "heavytank",
            "lighttank", "man", "medtank", "smallgear", "soft", "train", "turret"
        };
        for (std::size_t i = 0; i < kTargets.size(); ++i) {
            if (iequalsAscii(cls, kTargets[i])) return static_cast<int>(i);
        }
        return -1;
    }

    int attackPreferenceFor(std::string_view tgtType) const {
        const int idx = targetClassIndex(tgtType);
        return idx >= 0 ? attackPref[static_cast<std::size_t>(idx)] : 0;
    }
    bool isTracked() const { return engineType == "track"; }
    bool isWheeled() const { return engineType == "whell" || engineType == "wheel"; }
    bool isTrain() const { return selType == "train" || targetType == "train"; }

    bool isSupplyTruck() const { return ammoRegenDelay > 0 || selType == "sappers" || selType == "crate"; }
    bool isSupplyCrate() const { return selType == "crate"; }
    bool isAmbulance() const { return healDelay > 0; }
    bool hasFlamethrower() const {
        return shot1.isFlamethrower() || shot2.isFlamethrower() || selType == "flameinf";
    }
    bool isSniper() const {
        return selType == "sniper" || shot1.hitType == HitType::Sniper || id.find("sniper") != std::string::npos;
    }
    bool isRocketArtillery() const {
        auto containsLower = [](std::string_view hay, std::string_view needle) {
            if (hay.size() < needle.size()) return false;
            for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                bool ok = true;
                for (std::size_t j = 0; j < needle.size(); ++j) {
                    char c = hay[i + j];
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    if (c != needle[j]) { ok = false; break; }
                }
                if (ok) return true;
            }
            return false;
        };
        if (selType == "rl") return true;
        for (const auto* n : {"katy", "nebel", "bm-31", "bm-13", "calliope", "werfer", "rocket"}) {
            if (containsLower(id, n) || containsLower(shot1.shotId, n) || containsLower(shot1.soundGroup, n)) {
                return true;
            }
        }
        return false;
    }
    bool isArtillery() const {
        return roleClass == "at" || roleClass == "mortar" || selType == "howitzer" ||
               selType == "mortar" || selType == "mobmortar" || selType == "atg" ||
               selType == "spg" || turretType == "gaub" || turretType == "atg_gaub" ||
               turretType == "atg" || isRocketArtillery();
    }
    bool isCasemate() const {
        return roleClass == "stug" || selType == "spg" || selType == "rl";
    }
    bool isAntiAir() const {
        return roleClass == "antiaircraft" || roleClass == "aa" ||
               selType == "aagun" || selType == "mobairdef" ||
               turretType == "zenit" || turretType == "atg_zenit" ||
               shot2.shotId == "zenith" || shot2.shotId == "zenith_ac";
    }
    bool isHeavyAntiAir() const {
        return isAntiAir() &&
               (selType == "mobairdef" || shot1.soundGroup == "flak" ||
                shot2.soundGroup == "flak_37mm" || shot1.damage >= 180 ||
                shot2.damage >= 180);
    }
    bool isFearless() const { return moraleNoAttack <= -32767 || moraleResist >= 10000; }
    bool isPropaganda() const { return selType == "agit"; }
    bool isGunboat() const { return selType == "gunboat"; }
    bool isFortification() const { return roleClass == "fortification"; }
    bool isMedicInfantry() const { return selType == "medic" || selType == "medics"; }
    bool isOfficerInfantry() const { return selType == "officer" || selType == "general"; }
    bool isAntiTankInfantry() const { return selType == "atinf"; }
    bool isMortarInfantry() const { return selType == "mortar"; }
    bool isHeavyMgInfantry() const { return selType == "hmg"; }
    bool hasBinoculars() const { return binocularSight > 0 || isOfficerInfantry() || isSniper(); }
    bool isKamikaze() const { return kamikazePct > 0 || selType == "kamikaze"; }
    bool isQualifiedVehicleCrew() const { return canBeCrew > 0; }
    bool isTowable() const { return towClassIndex(towWeightClass) >= 0; }
    bool hasExposedCrew() const {
        if (!hasCrewProtection) return false;
        const auto& sn = crewProtection[static_cast<std::size_t>(HitType::Sniper)];
        const auto& mg = crewProtection[static_cast<std::size_t>(HitType::Machine)];
        return sn.front < 100 || sn.side < 100 || sn.rear < 100 ||
               mg.front < 100 || mg.side < 100 || mg.rear < 100;
    }
    int cookOffDamage() const {
        if (dieExplosion == "highexplosive" || dieExplosion == "mine6-7k" ||
            dieExplosion == "selfdestruct" ||
            dieExplosion.starts_with("dead_train") || dieExplosion.starts_with("dead_khassan")) {
            return 280;
        }
        if (dieExplosion == "dead_flamer") {
            return 180;
        }
        if (dieExplosion == "dead_tank" || dieExplosion == "mine4-5k" ||
            dieExplosion.starts_with("dead_short")) {
            return 160;
        }
        if (dieExplosion == "crate" || dieExplosion == "mine05-1k" || dieExplosion == "pontoon") {
            return 90;
        }
        return 0;
    }
    double cookOffRadius() const {
        const int d = cookOffDamage();
        if (d >= 280) return 72.0;
        if (d >= 160) return 56.0;
        if (d > 0) return 44.0;
        return 0.0;
    }
    int effectiveScoreValue() const {
        return scoreValue > 0 ? scoreValue : (isVehicle ? 250 : 100);
    }
    bool isNavalCraft() const { return walkOnWater && !walkOnGround; }
    std::uint16_t forbiddenTerrainMask() const {
        std::uint16_t m = 0u;
        if (!walkOnGround) m |= 0x000Fu;
        if (!walkOnWater) m |= 0x00F0u;
        if (!walkOnShallows) m |= 0x0F00u;
        return m;
    }
    double towSpeedFactor(std::string_view cls) const {
        const int idx = towClassIndex(cls);
        return (idx >= 0) ? towSpeedByClass[static_cast<std::size_t>(idx)] : 0.0;
    }
    bool canTowAny() const {
        for (double f : towSpeedByClass) {
            if (f > 0.0) return true;
        }
        return false;
    }
};

struct PlaneSpec {
    std::string id;
    int health{220};
    int scoreValue{500};
    int sight{18};
    double flySpeed{6.0};
    int bombDamage{0};
    int bombsNumber{0};
    int groundDamage{0};
    int airDamage{0};
    std::string idleSound;
    std::string bombSound;
    std::string fallSound;
    std::string groundShotSound;
    std::string airShotSound;
    std::string bombId;
    bool hasFallSmoke{false};
    int altitude{280};
    int groundAmmo{0};
    std::string groundShotId;
    HitType groundHitType{HitType::None};

    bool isLeafletBomber() const { return bombId == "fsheet"; }
    bool isRocketAttacker() const { return groundShotSound == "rocket" || groundShotId == "rs"; }
    HitType effectiveGroundHitType() const {
        if (groundHitType != HitType::None) return groundHitType;
        return isRocketAttacker() ? HitType::Explosive : HitType::Machine;
    }
    int effectiveScoreValue() const { return scoreValue > 0 ? scoreValue : 500; }
};

struct BuildingDefense {
    std::array<int, static_cast<std::size_t>(HitType::Count)> armor{};
    std::array<int, static_cast<std::size_t>(HitType::Count)> protection{};
    bool loaded{false};
    int unloadHp{150};
    int bonusShotRange{150};
    int bonusShotDeadZone{150};

    static BuildingDefense retailDefault() {
        BuildingDefense d{};
        d.armor[static_cast<std::size_t>(HitType::Explosive)] = 38;
        d.armor[static_cast<std::size_t>(HitType::Mine)] = 38;
        d.armor[static_cast<std::size_t>(HitType::Pierce)] = 38;
        d.armor[static_cast<std::size_t>(HitType::Transpierce)] = 38;
        d.armor[static_cast<std::size_t>(HitType::Fire)] = 0;
        d.armor[static_cast<std::size_t>(HitType::Machine)] = 50;
        d.armor[static_cast<std::size_t>(HitType::Sniper)] = 50;
        d.protection[static_cast<std::size_t>(HitType::Explosive)] = 44;
        d.protection[static_cast<std::size_t>(HitType::Mine)] = 32;
        d.protection[static_cast<std::size_t>(HitType::Pierce)] = 40;
        d.protection[static_cast<std::size_t>(HitType::Transpierce)] = 40;
        d.protection[static_cast<std::size_t>(HitType::Fire)] = 0;
        d.protection[static_cast<std::size_t>(HitType::Sniper)] = 40;
        d.protection[static_cast<std::size_t>(HitType::Machine)] = 37;
        return d;
    }
    int armorFor(HitType ht) const { return armor[static_cast<std::size_t>(ht)]; }
    int protectionFor(HitType ht) const { return protection[static_cast<std::size_t>(ht)]; }
};

struct MineSpec {
    int damage{50};
    int radius{29};
    int explDelayTicks{1000};
    std::string atShot{"mine_at"};
    std::string apShot{"mine_claymore"};
    std::string atSound{"defaultexplosion"};
    std::string apSound{"smallexplosion"};
    bool loaded{false};
};

struct WeatherTint {
    std::uint8_t r{0}, g{0}, b{0};
    float alpha{0.0f};
};

struct WeatherSpec {
    int rainVisibilityPct{60};
    int fogVisibilityPct{70};
    int snowVisibilityPct{60};
    WeatherTint fogTint{128, 128, 128, 0.5f};
    WeatherTint rainTint{32, 32, 32, 0.5f};
    WeatherTint snowTint{32, 32, 32, 0.3f};
    bool showAlways{true};
    bool loaded{false};
};

/// Lazy, thread-safe parser and cache for Gold 2.2 unit & weapon descriptors (`desc_common.sue`, `lang.sue`).
class UnitCatalog {
public:
    static std::shared_ptr<const UnitCatalog> load(const std::filesystem::path& gameDir);
    static std::shared_ptr<const UnitCatalog> fromSpecs(std::vector<UnitSpec> specs,
                                                        std::vector<PlaneSpec> planes = {});
    static HitType parseHitTypeName(std::string_view token);
    static UnitSpec parseUnitText(std::string_view unitId, std::string_view descText,
                                  std::string_view langText = {},
                                  const std::function<HitType(std::string_view)>& resolveShotHit = {});
    static PlaneSpec parsePlaneText(std::string_view planeId, std::string_view descText);

    const UnitSpec* find(std::string_view unitId) const;
    const PlaneSpec* findPlane(std::string_view planeId) const;
    std::string displayName(std::string_view unitId) const;
    std::string shortName(std::string_view unitId) const;
    const BuildingDefense& buildingDefense() const { return buildingDefense_; }
    const MineSpec& mineSpec() const { return mineSpec_; }
    const WeatherSpec& weather() const noexcept { return weather_; }
    int tankArmorProtection(HitType ht) const {
        if (ht == HitType::Pierce || ht == HitType::Transpierce) return tankArmorProtectionPierce_;
        if (ht == HitType::Explosive || ht == HitType::Mine) return tankArmorProtectionExplosive_;
        return 0;
    }
    const std::vector<std::pair<int, int>>& starScores() const { return starScores_; }
    int starRatingForScore(int score) const;
    int underAttackIntervalSec() const { return underAttackIntervalSec_; }

private:
    struct ShotInfo {
        HitType hitType{HitType::None};
        bool indirectArc{false};
        int craterScale{0};
        std::string impactSound;
    };
    ShotInfo resolveShotInfoLocked(std::string_view shotId) const;
    HitType resolveShotHitLocked(std::string_view shotId) const;

    std::filesystem::path descArchive_;
    std::filesystem::path langArchive_;
    bool hasDesc_{false};
    bool hasLang_{false};
    BuildingDefense buildingDefense_{BuildingDefense::retailDefault()};
    MineSpec mineSpec_{};
    WeatherSpec weather_{};
    int tankArmorProtectionPierce_{3};
    int tankArmorProtectionExplosive_{3};
    int underAttackIntervalSec_{15};
    std::vector<std::pair<int, int>> starScores_{
        {1, 25001}, {2, 60001}, {3, 100001}, {4, 150001}, {5, 200001},
        {6, 250001}, {7, 275001}, {8, 300001}, {9, 375001}, {10, 400001}
    };

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, std::unique_ptr<UnitSpec>> cache_;
    mutable std::unordered_set<std::string> misses_;
    mutable std::unordered_map<std::string, std::unique_ptr<PlaneSpec>> planeCache_;
    mutable std::unordered_set<std::string> planeMisses_;
    mutable std::unordered_map<std::string, ShotInfo> shotInfoCache_;
};

} // namespace ss2::assets
