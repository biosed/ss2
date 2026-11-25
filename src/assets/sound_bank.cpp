#include "assets/sound_bank.hpp"
#include "assets/sprite.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace ss2::assets {
namespace {
std::uint32_t u32(std::span<const std::uint8_t> data, std::size_t pos) {
    if (pos > data.size() || data.size() - pos < 4)
        throw std::runtime_error("Truncated sound archive");
    return std::uint32_t(data[pos]) | (std::uint32_t(data[pos + 1]) << 8) |
           (std::uint32_t(data[pos + 2]) << 16) | (std::uint32_t(data[pos + 3]) << 24);
}

std::string normalizeName(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c == 0) break;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() &&
           (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view stripInlineComment(std::string_view line) {
    const std::size_t semi = line.find(';');
    if (semi != std::string_view::npos) line = line.substr(0, semi);
    return trimView(line);
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

bool parseIntToken(std::string_view tok, int& out) {
    tok = trimView(tok);
    if (tok.empty()) return false;
    int val = 0;
    const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), val);
    if (ec != std::errc{} || ptr != tok.data() + tok.size()) return false;
    out = val;
    return true;
}

bool parseFloatToken(std::string_view tok, float& out) {
    const std::string buf(trimView(tok));
    if (buf.empty()) return false;
    char* end = nullptr;
    const float val = std::strtof(buf.c_str(), &end);
    if (end == buf.c_str() || *end != '\0') return false;
    out = val;
    return true;
}

std::string canonicalNation(std::string_view raw) {
    const std::string low = normalizeName(trimView(raw));
    if (low == "german" || low == "deu" || low == "d" || low == "axis") return "german";
    if (low == "russian" || low == "rus" || low == "soviet" || low == "r") return "russian";
    if (low == "british" || low == "uk" || low == "english" || low == "ally" || low == "b" || low == "e") {
        return "british";
    }
    if (low == "american" || low == "us" || low == "usa" || low == "a") return "american";
    if (low == "japanese" || low == "japan" || low == "jpn" || low == "j") return "japanese";
    if (low == "french" || low == "fra" || low == "f") return "french";
    if (low == "italian" || low == "ita" || low == "i") return "italian";
    return "german";
}

std::string canonicalRole(std::string_view raw) {
    const std::string low = normalizeName(trimView(raw));
    if (low.empty() || low == "none") return "none";
    if (low == "sold" || low == "soldier" || low == "infantry" || low == "rifleinf" ||
        low == "smginf" || low == "mginf") {
        return "soldier";
    }
    if (low == "off" || low == "officer" || low == "general" || low == "commander") return "officer";
    if (low == "sap" || low == "sapper" || low == "sappers" || low == "miner") return "sapper";
    if (low == "med" || low == "medic" || low == "medics" || low == "hospital") return "medic";
    if (low == "art" || low == "gun" || low == "artillery" || low == "howitzer" ||
        low == "atg" || low == "mortar") {
        return "gun";
    }
    if (low == "trans" || low == "transport" || low == "truck" || low == "apc" || low == "jeep") {
        return "transport";
    }
    if (low == "tank" || low == "lighttank" || low == "medtank" || low == "heavytank" || low == "spg") {
        return "tank";
    }
    if (low == "pilot" || low == "para" || low == "paratrooper") return "pilot";
    if (low == "plane" || low == "air") return "plane";
    if (low == "spec" || low == "specforce") return "spec";
    if (low == "fsold" || low == "ftank" || low == "isold" || low == "itank") return low;
    return low;
}

bool isVehicleRole(std::string_view canonicalSt) {
    return canonicalSt == "tank" || canonicalSt == "transport" || canonicalSt == "gun" ||
           canonicalSt == "plane" || canonicalSt == "ftank" || canonicalSt == "itank";
}

std::string normalizeSuePath(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : trimView(raw)) {
        if (c == '/') out.push_back('\\');
        else out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

void parseSimpleTokenList(std::string_view text, std::set<std::string>& dst, bool keepNone) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view clean = stripInlineComment(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (clean.empty() || clean.front() == '-') continue;
        const auto toks = splitTokens(clean);
        if (toks.empty()) continue;
        const std::string name = normalizeName(toks[0]);
        if (name.empty() || (!keepNone && name == "none")) continue;
        dst.insert(name);
    }
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open sound file: " + path.string());
    const auto size = file.tellg();
    if (size < 0) throw std::runtime_error("Cannot determine size of: " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Cannot read sound file: " + path.string());
    return bytes;
}

void appendUniquePair(std::vector<std::pair<std::string, std::string>>& list,
                      std::string_view nat, std::string_view st) {
    for (const auto& [en, es] : list) {
        if (en == nat && es == st) return;
    }
    list.emplace_back(std::string(nat), std::string(st));
}

std::vector<std::pair<std::string, std::string>> buildRoleCandidates(
    std::string_view rawNation, std::string_view rawRole) {
    std::string nat = canonicalNation(rawNation);
    const std::string st = canonicalRole(rawRole);
    if (st == "fsold" || st == "ftank") nat = "french";
    else if (st == "isold" || st == "itank") nat = "italian";

    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(8);

    if (nat == "french") {
        // Retail `MISC\french` stores French speech under `(british, fsold | ftank)` and falls
        // back to British Allied clips for roles/verbs without dedicated French recordings.
        const bool veh = isVehicleRole(st);
        if (veh) {
            appendUniquePair(out, "british", "ftank");
            if (st != "ftank" && st != "tank") appendUniquePair(out, "british", st);
            appendUniquePair(out, "british", "tank");
            appendUniquePair(out, "british", "fsold");
            appendUniquePair(out, "british", "soldier");
            appendUniquePair(out, "british", "none");
        } else {
            appendUniquePair(out, "british", "fsold");
            if (st != "fsold" && st != "soldier" && st != "medic" && st != "none") {
                appendUniquePair(out, "british", st);
            }
            appendUniquePair(out, "british", "soldier");
            appendUniquePair(out, "british", "ftank");
            appendUniquePair(out, "british", "none");
        }
        return out;
    }

    if (nat == "italian") {
        // Retail `MISC\italian` stores Italian speech under `(german, isold | itank)` and falls
        // back to German Axis clips for roles/verbs without dedicated Italian recordings.
        const bool veh = isVehicleRole(st);
        if (veh) {
            appendUniquePair(out, "german", "itank");
            appendUniquePair(out, "german", "isold");
            if (st != "itank" && st != "tank") appendUniquePair(out, "german", st);
            appendUniquePair(out, "german", "tank");
            appendUniquePair(out, "german", "soldier");
            appendUniquePair(out, "german", "none");
        } else {
            appendUniquePair(out, "german", "isold");
            appendUniquePair(out, "german", "itank");
            if (st != "isold" && st != "soldier" && st != "medic" && st != "none") {
                appendUniquePair(out, "german", st);
            }
            appendUniquePair(out, "german", "soldier");
            appendUniquePair(out, "german", "none");
        }
        return out;
    }

    appendUniquePair(out, nat, st);
    if (st == "spec") {
        appendUniquePair(out, nat, "officer");
        appendUniquePair(out, nat, "soldier");
    } else if (st == "officer" || st == "medic" || st == "pilot") {
        appendUniquePair(out, nat, "soldier");
    } else if (st == "sapper") {
        appendUniquePair(out, nat, "soldier");
        appendUniquePair(out, nat, "tank");
    } else if (st == "gun") {
        appendUniquePair(out, nat, "soldier");
        appendUniquePair(out, nat, "tank");
    } else if (st == "transport") {
        appendUniquePair(out, nat, "tank");
        appendUniquePair(out, nat, "soldier");
    } else if (st == "tank") {
        appendUniquePair(out, nat, "transport");
        appendUniquePair(out, nat, "soldier");
    } else if (st == "plane") {
        appendUniquePair(out, nat, "pilot");
        appendUniquePair(out, nat, "tank");
        appendUniquePair(out, nat, "soldier");
    }
    appendUniquePair(out, nat, "none");
    return out;
}

enum class VerbCategory { Select, Ack, Scream };

struct VerbMapping {
    VerbCategory category{VerbCategory::Ack};
    std::vector<std::string_view> candidates;
    bool suppressGenericAck{false};
};

VerbMapping classifyVerb(std::string_view rawVerb, bool vehicleRole) {
    const std::string v = normalizeName(trimView(rawVerb));
    if (v == "select" || v == "sayselect") {
        return {VerbCategory::Select, {}, false};
    }
    if (v == "underattack" || v == "under_attack") {
        return {VerbCategory::Scream, {"underattack", "enemyinsight"}, false};
    }
    if (v == "close" || v == "closetodeath" || v == "enemy_close" || v == "enemyclose") {
        return {VerbCategory::Scream, {"closetodeath"}, true};
    }
    if (v == "noammo" || v == "outofammo" || v == "out_of_ammo") {
        return {VerbCategory::Scream, {"outofammo"}, true};
    }
    if (v == "enemy" || v == "enemyinsight" || v == "enemy_in_sight") {
        return {VerbCategory::Scream, {"enemyinsight"}, true};
    }
    if (v == "startattack" || v == "start_attack") {
        return {VerbCategory::Scream, {"startattack"}, true};
    }
    if (v == "attack_move" || v == "attackmove" || v == "assault") {
        return {VerbCategory::Ack, {"attack_move", "attack_unit", "move"}, false};
    }
    if (v == "attack_ground" || v == "attackground" || v == "bombard") {
        return {VerbCategory::Ack, {"attack_ground", "attack_unit", "attack_move"}, false};
    }
    if (v == "attack" || v == "attack_unit" || v == "attackunit") {
        return {VerbCategory::Ack, {"attack_unit", "attack_ground", "attack_move", "move"}, false};
    }
    if (v == "stop") {
        return {VerbCategory::Ack, {"stop", "rotate", "unload", "move"}, false};
    }
    if (v == "hold" || v == "holdfire" || v == "hold_fire") {
        return {VerbCategory::Ack, {"stop", "rotate", "move"}, false};
    }
    if (v == "crouch" || v == "stand" || v == "stance") {
        return {VerbCategory::Ack, {"stop", "move"}, false};
    }
    if (v == "binoculars" || v == "recon" || v == "binoc") {
        return {VerbCategory::Ack, {"stop", "rotate", "move"}, false};
    }
    if (v == "garrison" || v == "goin" || v == "enter") {
        return {VerbCategory::Ack, {"goin", "move"}, false};
    }
    if (v == "embark" || v == "board" || v == "pickup") {
        if (vehicleRole) {
            return {VerbCategory::Ack, {"pickup", "goin", "move"}, false};
        }
        return {VerbCategory::Ack, {"goin", "pickup", "move"}, false};
    }
    if (v == "unload" || v == "disembark" || v == "evacuate") {
        return {VerbCategory::Ack, {"unload", "move", "stop"}, false};
    }
    if (v == "pricep" || v == "tow" || v == "hitch") {
        return {VerbCategory::Ack, {"pricep", "pickup", "move"}, false};
    }
    if (v == "rotate" || v == "face") {
        return {VerbCategory::Ack, {"rotate", "move", "stop"}, false};
    }
    if (v == "laymine" || v == "lay_mine") {
        return {VerbCategory::Ack, {"lay_mine", "build1", "fixmost", "get_mine", "stop"}, true};
    }
    if (v == "getmine" || v == "get_mine" || v == "disarm" || v == "minesweep" || v == "searchmine") {
        return {VerbCategory::Ack, {"get_mine", "fixmost", "stop"}, true};
    }
    if (v == "repair" || v == "fixmost" || v == "build" || v == "build1" || v == "build2" ||
        v == "build3" || v == "build4") {
        return {VerbCategory::Ack, {"fixmost", "reload", "build1", "move"}, true};
    }
    if (v == "resupply" || v == "reload") {
        return {VerbCategory::Ack, {"reload", "fixmost", "move"}, true};
    }
    if (v == "heal" || v == "cure") {
        return {VerbCategory::Ack, {"reload", "goin", "move", "stop"}, false};
    }
    // Default (`"move"`, `"patrol"`, `"scatter"`, `"reverse"`, etc.).
    return {VerbCategory::Ack, {"move", "unload", "stop"}, false};
}
} // namespace

SoundBank SoundBank::load(const std::filesystem::path& gameDir) {
    const auto sbFiles = resolveSoundBankFiles(gameDir);
    const auto hdrPath = sbFiles.hdr.empty() ? resolveCaseInsensitivePath(gameDir, "SOUNDS.HDR") : sbFiles.hdr;
    const auto rusPath = sbFiles.rus.empty() ? resolveCaseInsensitivePath(gameDir, "SOUNDS.RUS") : sbFiles.rus;
    const auto hdr = readFileBytes(hdrPath);
    const auto rus = readFileBytes(rusPath);
    SoundBank bank = fromMemory(hdr, rus);

    const auto descArchives = resolveDescArchives(gameDir, 0);
    if (!descArchives.empty()) {
        auto readEntryText = [&](std::string_view entry) -> std::string {
            try {
                if (const auto bytes = tryReadSueEntryChain(descArchives, entry)) {
                    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                }
            } catch (const std::exception&) {
            }
            return {};
        };
        const std::string soundTypesTxt = readEntryText("soundtypes");
        const std::string soundSchemesTxt = readEntryText("soundschemes");
        const std::string voiceTxt = readEntryText("voice");
        std::string miscSoundsTxt = readEntryText("MISC\\sounds");
        for (const char* natFile : {"MISC\\american", "MISC\\british", "MISC\\french",
                                    "MISC\\german", "MISC\\italian", "MISC\\japanese", "MISC\\russian"}) {
            miscSoundsTxt += "\n#";
            miscSoundsTxt += natFile;
        }
        bank.loadVoiceTablesFromText(miscSoundsTxt, soundTypesTxt, soundSchemesTxt, voiceTxt,
                                     [&](std::string_view inc) { return readEntryText(inc); });
    }
    return bank;
}

SoundBank SoundBank::fromMemory(std::span<const std::uint8_t> hdr, std::span<const std::uint8_t> rus) {
    SoundBank bank;
    const std::uint32_t numGroups = u32(hdr, 0);
    std::size_t pos = 4;
    for (std::uint32_t g = 0; g < numGroups; ++g) {
        if (pos + 68 > hdr.size()) throw std::runtime_error("Truncated SOUNDS.HDR group");
        const std::string name = normalizeName(
            std::string_view(reinterpret_cast<const char*>(hdr.data() + pos), 64));
        pos += 64;
        const std::uint32_t count = u32(hdr, pos);
        pos += 4;
        if (pos + std::size_t(count) * 4 > hdr.size())
            throw std::runtime_error("Truncated SOUNDS.HDR indices");
        std::vector<std::uint32_t> indices;
        indices.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            indices.push_back(u32(hdr, pos));
            pos += 4;
        }
        bank.groups_.emplace(name, std::move(indices));
    }

    const std::uint32_t clipCount = u32(rus, 0);
    if (4 + std::size_t(clipCount + 1) * 4 > rus.size())
        throw std::runtime_error("Truncated SOUNDS.RUS offset table");
    bank.clips_.reserve(clipCount);
    for (std::uint32_t i = 0; i < clipCount; ++i) {
        const std::uint32_t start = u32(rus, 4 + std::size_t(i) * 4);
        const std::uint32_t end = u32(rus, 4 + std::size_t(i + 1) * 4);
        if (end < start || end > rus.size() || end - start < 12)
            throw std::runtime_error("Invalid SOUNDS.RUS clip bounds");
        SoundClip clip;
        clip.sampleRate = u32(rus, start);
        clip.bitsPerSample = u32(rus, start + 4);
        clip.loop = u32(rus, start + 8) != 0;
        const std::size_t pcmBytes = end - start - 12;
        clip.pcm.assign(rus.begin() + start + 12, rus.begin() + start + 12 + pcmBytes);
        bank.clips_.push_back(std::move(clip));
    }
    return bank;
}

std::size_t SoundBank::variantCount(const std::string& group) const {
    const auto it = groups_.find(normalizeName(group));
    return it == groups_.end() ? 0 : it->second.size();
}

const SoundClip* SoundBank::clip(const std::string& group, std::size_t variant) const {
    const auto it = groups_.find(normalizeName(group));
    if (it == groups_.end() || it->second.empty()) return nullptr;
    const std::uint32_t idx = it->second[variant % it->second.size()];
    if (idx >= clips_.size()) return nullptr;
    return &clips_[idx];
}

bool SoundBank::hasPlayableGroup(std::string_view group) const {
    if (group.empty()) return false;
    const auto it = groups_.find(normalizeName(group));
    if (it == groups_.end() || it->second.empty()) return false;
    for (const std::uint32_t idx : it->second) {
        if (idx < clips_.size() && !clips_[idx].pcm.empty()) return true;
    }
    return false;
}

bool SoundBank::hasSoundScheme(std::string_view scheme) const {
    return soundSchemes_.contains(normalizeName(trimView(scheme)));
}

bool SoundBank::hasSoundType(std::string_view soundType) const {
    return soundTypes_.contains(normalizeName(trimView(soundType)));
}

bool SoundBank::hasDeclaredVoice(std::string_view voiceName) const {
    return declaredVoices_.contains(normalizeName(trimView(voiceName)));
}

void SoundBank::loadVoiceTablesFromText(
    std::string_view miscSoundsText,
    std::string_view soundTypesText,
    std::string_view soundSchemesText,
    std::string_view voiceText,
    const std::function<std::string(std::string_view)>& resolveInclude) {
    soundTypes_.clear();
    soundSchemes_.clear();
    declaredVoices_.clear();
    saySelect_.clear();
    sayAck_.clear();
    scream_.clear();
    dieSound_.clear();
    defaultVoicePriority_ = 0.33f;
    defaultVoiceDelaySec_ = 8;

    parseSimpleTokenList(soundTypesText, soundTypes_, true);
    parseSimpleTokenList(soundSchemesText, soundSchemes_, false);
    parseSimpleTokenList(voiceText, declaredVoices_, false);

    std::set<std::string> visited;
    parseMiscVoiceStream(miscSoundsText, resolveInclude, visited);
    finalizeVoiceInheritance();
    hasVoiceTables_ = !saySelect_.empty() || !sayAck_.empty() || !scream_.empty() || !dieSound_.empty();
}

void SoundBank::parseMiscVoiceStream(
    std::string_view text,
    const std::function<std::string(std::string_view)>& resolveInclude,
    std::set<std::string>& visitedIncludes) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view clean = stripInlineComment(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (clean.empty() || clean.front() == '-') continue;

        if (clean.front() == '#') {
            if (resolveInclude) {
                const std::string incKey = normalizeSuePath(clean.substr(1));
                if (!incKey.empty() && visitedIncludes.insert(incKey).second) {
                    const std::string incText = resolveInclude(incKey);
                    if (!incText.empty()) {
                        parseMiscVoiceStream(incText, resolveInclude, visitedIncludes);
                    }
                }
            }
            continue;
        }

        const auto toks = splitTokens(clean);
        if (toks.empty()) continue;
        const std::string kw = normalizeName(toks[0]);

        if (kw == "defaultpriority" && toks.size() >= 2) {
            float p = 0.33f;
            if (parseFloatToken(toks[1], p)) defaultVoicePriority_ = std::clamp(p, 0.0f, 1.0f);
            continue;
        }
        if (kw == "defaultdelay" && toks.size() >= 2) {
            int d = 8;
            if (parseIntToken(toks[1], d) && d >= 0) defaultVoiceDelaySec_ = d;
            continue;
        }
        if (kw == "sayselect" && toks.size() >= 5) {
            const std::string st = canonicalRole(toks[1]);
            const std::string nat = canonicalNation(toks[2]);
            int rank = 1;
            (void)parseIntToken(toks[3], rank);
            const std::string grp = normalizeName(toks[4]);
            auto& slots = saySelect_[{nat, st}];
            std::size_t vetIdx = rank >= 2 ? 1u : 0u;
            // Retail `MISC\german` lists `sayselect soldier german 1 d_select` followed immediately
            // by `sayselect soldier german 1 d_vet_select` (a rank-column typo next to rank-2
            // `d_vet_attack` / `d_vet_move` / `d_vet_ack`). Route `_vet_` to slot 1 so regular
            // soldiers retain `d_select` and veterans get `d_vet_select`.
            if (vetIdx == 0u && slots[0].defined && !slots[0].explicitNone &&
                slots[0].group.find("_vet_") == std::string::npos &&
                grp.find("_vet_") != std::string::npos && !slots[1].defined) {
                vetIdx = 1u;
            }
            const bool isNone = (grp == "none");
            slots[vetIdx] = VoiceEntry{isNone ? std::string{} : grp, isNone, true};
            continue;
        }
        if (kw == "sayack" && toks.size() >= 6) {
            const std::string st = canonicalRole(toks[1]);
            const std::string nat = canonicalNation(toks[2]);
            int rank = 1;
            (void)parseIntToken(toks[3], rank);
            const std::string orderVerb = normalizeName(toks[4]);
            const std::string grp = normalizeName(toks[5]);
            const std::size_t vetIdx = rank >= 2 ? 1u : 0u;
            const bool isNone = (grp == "none");
            sayAck_[{nat, st, orderVerb}][vetIdx] =
                VoiceEntry{isNone ? std::string{} : grp, isNone, true};
            continue;
        }
        if (kw == "scream" && toks.size() >= 7) {
            const std::string st = canonicalRole(toks[1]);
            const std::string nat = canonicalNation(toks[2]);
            int rank = 1;
            (void)parseIntToken(toks[3], rank);
            const std::string screamVerb = normalizeName(toks[5]);
            const std::string grp = normalizeName(toks[6]);
            const std::size_t vetIdx = rank >= 2 ? 1u : 0u;
            const bool isNone = (grp == "none");
            scream_[{nat, st, screamVerb}][vetIdx] =
                VoiceEntry{isNone ? std::string{} : grp, isNone, true};
            continue;
        }
        if (kw == "diesound" && toks.size() >= 4) {
            const std::string st = canonicalRole(toks[1]);
            const std::string nat = canonicalNation(toks[2]);
            const std::string grp = normalizeName(toks[3]);
            const bool isNone = (grp == "none");
            dieSound_[{nat, st}] = VoiceEntry{isNone ? std::string{} : grp, isNone, true};
            continue;
        }
    }
}

void SoundBank::finalizeVoiceInheritance() {
    for (auto& [key, slots] : saySelect_) {
        (void)key;
        if (!slots[1].defined && slots[0].defined) slots[1] = slots[0];
    }
    for (auto& [key, slots] : sayAck_) {
        (void)key;
        if (!slots[1].defined && slots[0].defined) slots[1] = slots[0];
    }
    for (auto& [key, slots] : scream_) {
        (void)key;
        if (!slots[1].defined && slots[0].defined) slots[1] = slots[0];
    }
}

std::string SoundBank::resolveDeathSound(std::string_view nation, std::string_view soundType) const {
    const auto candidates = buildRoleCandidates(nation, soundType);
    for (const auto& [nat, st] : candidates) {
        const auto it = dieSound_.find({nat, st});
        if (it == dieSound_.end() || !it->second.defined) continue;
        if (it->second.explicitNone) return {};
        if (groups_.empty() || hasPlayableGroup(it->second.group)) return it->second.group;
    }
    if (hasPlayableGroup("screamofpain")) return "screamofpain";
    return {};
}

std::string SoundBank::resolveUnitVoice(std::string_view nation, std::string_view soundType,
                                        std::string_view verb, bool isVeteran) const {
    const std::string canonNat = canonicalNation(nation);
    const std::string canonRole = canonicalRole(soundType);
    const bool vehRole = isVehicleRole(canonRole);
    const VerbMapping mapping = classifyVerb(verb, vehRole);
    const std::size_t vetIdx = isVeteran ? 1u : 0u;
    const auto acceptGroup = [&](std::string_view g) -> bool {
        if (g.empty()) return false;
        // Verify against loaded SOUNDS.HDR+SOUNDS.RUS whenever the bank has clips/groups.
        return groups_.empty() ? false : hasPlayableGroup(g);
    };

    // Real recordings packed in retail SOUNDS.HDR that were omitted from MISC\german text:
    // German field medics (`d_med_select`, `d_med_move`, `d_med_ack`), German sapper mine cues
    // (`d_sap_mine`, `d_sap_search`), and German aircraft select (`d_plane_select`).
    if (canonNat == "german") {
        if (canonRole == "medic") {
            const std::string lowVerb = normalizeName(trimView(verb));
            if (mapping.category == VerbCategory::Select && acceptGroup("d_med_select")) {
                return "d_med_select";
            }
            if ((lowVerb == "move" || lowVerb == "garrison" || lowVerb == "goin" ||
                 lowVerb == "embark" || lowVerb == "board" || lowVerb == "unload") &&
                acceptGroup("d_med_move")) {
                return "d_med_move";
            }
            if ((lowVerb == "heal" || lowVerb == "cure" || lowVerb == "stop" ||
                 lowVerb == "hold" || lowVerb == "holdfire" || lowVerb == "hold_fire" ||
                 lowVerb == "crouch" || lowVerb == "stand" || lowVerb == "attack" ||
                 lowVerb == "attack_move") &&
                acceptGroup("d_med_ack")) {
                return "d_med_ack";
            }
        } else if (canonRole == "sapper" && mapping.category == VerbCategory::Ack &&
                   !mapping.candidates.empty()) {
            if (mapping.candidates.front() == "lay_mine" && acceptGroup("d_sap_mine")) {
                return "d_sap_mine";
            }
            if (mapping.candidates.front() == "get_mine" && acceptGroup("d_sap_search")) {
                return "d_sap_search";
            }
        } else if (canonRole == "plane" && mapping.category == VerbCategory::Select &&
                   acceptGroup("d_plane_select")) {
            return "d_plane_select";
        }
    }

    if (hasVoiceTables_) {
        const auto candidates = buildRoleCandidates(canonNat, canonRole);
        if (mapping.category == VerbCategory::Select) {
            for (const auto& [nat, st] : candidates) {
                const auto it = saySelect_.find({nat, st});
                if (it == saySelect_.end()) continue;
                const auto& entry = it->second[vetIdx];
                if (!entry.defined) continue;
                if (entry.explicitNone) return {};
                if (acceptGroup(entry.group)) return entry.group;
                if (vetIdx == 1u && it->second[0].defined && !it->second[0].explicitNone &&
                    acceptGroup(it->second[0].group)) {
                    return it->second[0].group;
                }
            }
            // Plane select fallback to `sayack` (e.g. Russian `MISC\russian` defines `sayack plane`
            // `r_plane_ack` / `r_plane_move` without `sayselect plane`).
            for (const auto& [nat, st] : candidates) {
                for (std::string_view fbOrder : {"stop", "move", "attack_unit"}) {
                    const auto it = sayAck_.find({nat, st, std::string(fbOrder)});
                    if (it == sayAck_.end()) continue;
                    const auto& entry = it->second[vetIdx];
                    if (entry.defined && !entry.explicitNone && acceptGroup(entry.group)) {
                        return entry.group;
                    }
                }
            }
        } else if (mapping.category == VerbCategory::Ack) {
            // Check each mapped retail order verb in priority order (exact primary verb first across
            // role/fallback candidates so e.g. `stop` never plays a `move` line when a parent role
            // has a real `stop` clip, then secondary synonym verbs).
            for (std::string_view candVerb : mapping.candidates) {
                for (const auto& [nat, st] : candidates) {
                    const auto it = sayAck_.find({nat, st, std::string(candVerb)});
                    if (it == sayAck_.end()) continue;
                    const auto& entry = it->second[vetIdx];
                    if (!entry.defined) continue;
                    if (entry.explicitNone) return {};
                    if (acceptGroup(entry.group)) return entry.group;
                    if (vetIdx == 1u && it->second[0].defined && !it->second[0].explicitNone &&
                        acceptGroup(it->second[0].group)) {
                        return it->second[0].group;
                    }
                }
            }
        } else {
            for (std::string_view candScream : mapping.candidates) {
                for (const auto& [nat, st] : candidates) {
                    const auto it = scream_.find({nat, st, std::string(candScream)});
                    if (it == scream_.end()) continue;
                    const auto& entry = it->second[vetIdx];
                    if (!entry.defined) continue;
                    if (entry.explicitNone) return {};
                    if (acceptGroup(entry.group)) return entry.group;
                    if (vetIdx == 1u && it->second[0].defined && !it->second[0].explicitNone &&
                        acceptGroup(it->second[0].group)) {
                        return it->second[0].group;
                    }
                }
            }
        }
    }

    // Naming-convention lookup for synthetic SoundBanks constructed via `fromMemory(hdr, rus)`
    // without `desc_common.sue` descriptor tables.
    if (!hasVoiceTables_ && !groups_.empty()) {
        const std::string pfx = canonNat == "russian" ? "r_"
                              : canonNat == "british" ? "b_"
                              : canonNat == "american" ? "a_"
                              : canonNat == "japanese" ? "j_"
                              : (canonNat == "french" || canonRole == "fsold" || canonRole == "ftank") ? "f_"
                              : (canonNat == "italian" || canonRole == "isold" || canonRole == "itank") ? "i_"
                              : "d_";
        const std::string shortRole = (canonRole == "officer" || canonRole == "spec") ? "off"
                                    : canonRole == "sapper" ? "sap"
                                    : canonRole == "medic" ? "med"
                                    : canonRole == "gun" ? "art"
                                    : canonRole == "transport" ? "trans"
                                    : canonRole == "pilot" ? "pilot"
                                    : canonRole == "plane" ? "plane"
                                    : vehRole ? "tank"
                                    : "sold";
        const std::string lowVerb = normalizeName(trimView(verb));
        if (isVeteran && pfx == "d_" && shortRole == "sold") {
            if (lowVerb == "select" && acceptGroup("d_vet_select")) return "d_vet_select";
            if ((lowVerb == "move" || lowVerb == "garrison" || lowVerb == "goin") &&
                acceptGroup("d_vet_move")) return "d_vet_move";
            if ((lowVerb == "attack" || lowVerb == "attack_unit" || lowVerb == "attack_ground") &&
                acceptGroup("d_vet_attack")) return "d_vet_attack";
            if ((lowVerb == "stop" || lowVerb == "hold" || lowVerb == "attack_move") &&
                acceptGroup("d_vet_ack")) return "d_vet_ack";
            if ((lowVerb == "getmine" || lowVerb == "get_mine") &&
                acceptGroup("d_vet_search")) return "d_vet_search";
        }
        const std::string baseVerb = (lowVerb == "attack_unit" || lowVerb == "attack_ground" ||
                                      lowVerb == "attack_move")
                                         ? "attack"
                                         : (lowVerb == "garrison" || lowVerb == "goin" ||
                                            lowVerb == "embark" || lowVerb == "board" ||
                                            lowVerb == "unload")
                                               ? "move"
                                               : (lowVerb == "hold" || lowVerb == "holdfire" ||
                                                  lowVerb == "crouch" || lowVerb == "stand" ||
                                                  lowVerb == "binoculars")
                                                     ? "stop"
                                                     : lowVerb;
        if (pfx == "d_" && shortRole == "sold" && acceptGroup("d_" + baseVerb)) {
            return "d_" + baseVerb;
        }
        if (pfx == "b_" && shortRole == "off" && baseVerb == "select" && acceptGroup("b_off_sele")) {
            return "b_off_sele";
        }
        if (acceptGroup(pfx + shortRole + "_" + baseVerb)) return pfx + shortRole + "_" + baseVerb;
        if ((baseVerb == "move" || baseVerb == "attack" || baseVerb == "stop") &&
            acceptGroup(pfx + shortRole + "_ack")) {
            return pfx + shortRole + "_ack";
        }
        if (shortRole != "sold") {
            if (pfx == "d_" && acceptGroup("d_" + baseVerb)) return "d_" + baseVerb;
            if (acceptGroup(pfx + "sold_" + baseVerb)) return pfx + "sold_" + baseVerb;
            if ((baseVerb == "move" || baseVerb == "attack" || baseVerb == "stop") &&
                acceptGroup(pfx + "sold_ack")) {
                return pfx + "sold_ack";
            }
        }
    }

    if (!mapping.suppressGenericAck) {
        const char* genericAck = canonNat == "russian"
                                     ? "ack_rus"
                                     : (canonNat == "german" || canonNat == "italian")
                                           ? "ack_deu"
                                           : "ack_ally";
        if (acceptGroup(genericAck)) return genericAck;
    }
    return {};
}

} // namespace ss2::assets
