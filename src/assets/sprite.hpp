#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ss2::assets {
using Bytes = std::vector<std::uint8_t>;

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b);
std::filesystem::path resolveCaseInsensitivePath(const std::filesystem::path& base,
                                                 const std::filesystem::path& relative);

std::optional<Bytes> tryReadSueEntry(const std::filesystem::path& archive, std::string_view entry);
Bytes readSueEntry(const std::filesystem::path& archive, std::string_view entry);
Bytes readSueEntryFallback(const std::vector<std::filesystem::path>& archives,
                           std::string_view entry);
std::optional<std::size_t> sueEntrySize(const std::filesystem::path& archive, std::string_view entry);
Bytes readSueEntryRange(const std::filesystem::path& archive, std::string_view entry,
                        std::size_t byteOffset, std::size_t maxBytes);

std::optional<Bytes> tryReadSueEntryChain(std::span<const std::filesystem::path> archives,
                                          std::string_view entry);
Bytes readSueEntryChain(std::span<const std::filesystem::path> archives, std::string_view entry);
std::optional<std::size_t> sueChainEntrySize(std::span<const std::filesystem::path> archives,
                                             std::string_view entry);
Bytes readSueChainRange(std::span<const std::filesystem::path> archives, std::string_view entry,
                        std::size_t byteOffset, std::size_t maxBytes);
std::vector<std::string> listSueEntries(const std::filesystem::path& archive);

struct ModInfo {
    std::string id;
    std::string name;
    std::filesystem::path rootDir;
    std::filesystem::path iniPath;
};

struct SoundBankPaths {
    std::filesystem::path hdr;
    std::filesystem::path rus;
};

void setActiveMod(std::string_view modSpec);
std::string activeMod();
std::vector<ModInfo> listInstalledMods(const std::filesystem::path& gameDir,
                                       const std::filesystem::path& userModsDir = {});
std::optional<ModInfo> resolveMod(const std::filesystem::path& gameDir,
                                  std::string_view modSpec,
                                  const std::filesystem::path& userModsDir = {});

std::vector<std::filesystem::path> resolveGameArchives(const std::filesystem::path& gameDir);
std::vector<std::filesystem::path> resolveDescArchives(const std::filesystem::path& gameDir,
                                                       unsigned scheme = 0);
std::vector<std::filesystem::path> resolveLangArchives(const std::filesystem::path& gameDir,
                                                       unsigned scheme = 0);
std::vector<std::filesystem::path> resolveMenuArchives(const std::filesystem::path& gameDir);
std::vector<std::filesystem::path> resolveSchemeArchives(const std::filesystem::path& gameDir,
                                                         unsigned scheme);
SoundBankPaths resolveSoundBankFiles(const std::filesystem::path& gameDir);
std::string resolveBackWavEntry(const std::filesystem::path& gameDir);
std::filesystem::path resolveMusicDir(const std::filesystem::path& gameDir);
std::vector<std::filesystem::path> resolveMovieDirs(const std::filesystem::path& gameDir);
std::vector<std::filesystem::path> resolveAddonsDirs(const std::filesystem::path& gameDir);
std::vector<std::filesystem::path> resolveSingleMapDirs(const std::filesystem::path& gameDir);
std::vector<std::filesystem::path> resolveMultiMapDirs(const std::filesystem::path& gameDir);
std::vector<std::filesystem::path> resolveCampaignMapDirs(const std::filesystem::path& gameDir);
std::vector<std::filesystem::path> resolveCustomCampaignDirs(const std::filesystem::path& gameDir);
std::optional<unsigned> resolveIniTurnDelay(const std::filesystem::path& gameDir);
std::string schemeDisplayName(unsigned scheme);

struct SpriteFrame {
    int x, y, width, height;
    // Original 16-bit pixel words serialized little-endian in top-to-bottom row order.
    Bytes pixels;
};
SpriteFrame decodePckFrame(std::span<const std::uint8_t> file, std::size_t index);
std::size_t pckFrameCount(std::span<const std::uint8_t> file);
SpriteFrame decodeUiPckFrame(std::span<const std::uint8_t> file, std::size_t index);
}
