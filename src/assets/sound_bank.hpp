#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace ss2::assets {

struct SoundClip {
    std::uint32_t sampleRate{22050};
    std::uint32_t bitsPerSample{16};
    bool loop{false};
    std::vector<std::uint8_t> pcm;
};

// Decodes Gold 2.2 SOUNDS.HDR + SOUNDS.RUS uncompressed mono PCM archives and
// optional `desc_common.sue` unit voice / soundscheme descriptor tables.
class SoundBank {
public:
    static SoundBank load(const std::filesystem::path& gameDir);
    static SoundBank fromMemory(std::span<const std::uint8_t> hdr, std::span<const std::uint8_t> rus);

    const SoundClip* clip(const std::string& group, std::size_t variant = 0) const;
    std::size_t variantCount(const std::string& group) const;
    std::size_t groupCount() const { return groups_.size(); }
    std::size_t clipCount() const { return clips_.size(); }

    // Parses Gold 2.2 `MISC\sounds` + `MISC\<nation>` rules (`sayselect`, `sayack`, `scream`,
    // `diesound`, `defaultpriority`, `defaultdelay`, `#misc\<name>` includes) plus optional
    // `soundtypes`, `soundschemes`, and `voice` descriptor lists.
    void loadVoiceTablesFromText(
        std::string_view miscSoundsText,
        std::string_view soundTypesText = {},
        std::string_view soundSchemesText = {},
        std::string_view voiceText = {},
        const std::function<std::string(std::string_view)>& resolveInclude = {});

    bool hasVoiceTables() const noexcept { return hasVoiceTables_; }
    bool hasSoundScheme(std::string_view scheme) const;
    bool hasSoundType(std::string_view soundType) const;
    bool hasDeclaredVoice(std::string_view voiceName) const;
    std::size_t soundSchemeCount() const noexcept { return soundSchemes_.size(); }
    std::size_t soundTypeCount() const noexcept { return soundTypes_.size(); }
    float defaultVoicePriority() const noexcept { return defaultVoicePriority_; }
    int defaultVoiceDelaySec() const noexcept { return defaultVoiceDelaySec_; }

    // Resolves `(nation, soundType/role, verb, isVeteran)` to a verified sound group name in
    // this bank (or synthetic fallback `ack_deu` / `ack_rus` / `ack_ally` when present).
    // Returns an empty string when explicitly silenced (`none` in descriptor) or unresolvable.
    std::string resolveUnitVoice(std::string_view nation, std::string_view soundType,
                                 std::string_view verb, bool isVeteran = false) const;

    // Resolves `diesound <soundType> <nation> <group>` (returns empty string if `none`).
    std::string resolveDeathSound(std::string_view nation, std::string_view soundType) const;

private:
    struct VoiceEntry {
        std::string group;
        bool explicitNone{false};
        bool defined{false};
    };
    using VetPair = std::array<VoiceEntry, 2>;

    bool hasPlayableGroup(std::string_view group) const;
    void parseMiscVoiceStream(
        std::string_view text,
        const std::function<std::string(std::string_view)>& resolveInclude,
        std::set<std::string>& visitedIncludes);
    void finalizeVoiceInheritance();

    std::map<std::string, std::vector<std::uint32_t>> groups_;
    std::vector<SoundClip> clips_;

    bool hasVoiceTables_{false};
    float defaultVoicePriority_{0.33f};
    int defaultVoiceDelaySec_{8};
    std::set<std::string> soundTypes_;
    std::set<std::string> soundSchemes_;
    std::set<std::string> declaredVoices_;
    std::map<std::pair<std::string, std::string>, VetPair> saySelect_;
    std::map<std::tuple<std::string, std::string, std::string>, VetPair> sayAck_;
    std::map<std::tuple<std::string, std::string, std::string>, VetPair> scream_;
    std::map<std::pair<std::string, std::string>, VoiceEntry> dieSound_;
};

} // namespace ss2::assets
