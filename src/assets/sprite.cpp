#include "assets/sprite.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <zlib.h>

namespace ss2::assets {
namespace {
using View = std::span<const std::uint8_t>;
constexpr size_t tableLimit = 16u << 20;
constexpr size_t assetLimit = 64u << 20;

constexpr unsigned char asciiLower(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

View slice(View bytes, size_t offset, size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset)
        throw std::runtime_error("Truncated asset record");
    return bytes.subspan(offset, count);
}

unsigned u16(View bytes, size_t at) {
    const auto v = slice(bytes, at, 2);
    return v[0] | (unsigned(v[1]) << 8);
}
std::uint32_t u32(View bytes, size_t at) {
    return u16(bytes, at) | (std::uint32_t(u16(bytes, at + 2)) << 16);
}

Bytes read(std::ifstream& file, size_t length) {
    Bytes bytes(length);
    if (length && !file.read(reinterpret_cast<char*>(bytes.data()), length))
        throw std::runtime_error("Truncated SUE file");
    return bytes;
}

Bytes inflateExact(View input, size_t expected) {
    Bytes output(std::max<size_t>(expected, 1));
    uLongf outSize = output.size();
    uLong inSize = input.size();
    if (uncompress2(output.data(), &outSize, input.data(), &inSize) != Z_OK ||
        outSize != expected || inSize != input.size())
        throw std::runtime_error("Invalid SUE zlib stream or declared length");
    output.resize(expected);
    return output;
}

Bytes table(std::ifstream& file, unsigned recordWidth) {
    const auto header = read(file, 12);
    if (recordWidth && u32(header, 8) != recordWidth)
        throw std::runtime_error("Unexpected SUE table record width");
    const auto packed = u32(header, 0);
    const std::uint64_t length = std::uint64_t(u32(header, 4)) * u32(header, 8);
    if (length > tableLimit || packed > compressBound(tableLimit))
        throw std::runtime_error("SUE index exceeds reader limit");
    return inflateExact(read(file, packed), length);
}

Bytes payload(std::ifstream& file, size_t start, size_t length, size_t indexStart) {
    if (start < 8 || start > indexStart || length > indexStart - start)
        throw std::runtime_error("SUE payload outside data section");
    file.seekg(start);
    return read(file, length);
}

struct SueIndex {
    std::filesystem::path key;
    std::uintmax_t fileSize{};
    std::filesystem::file_time_type mtime{};
    size_t indexStart{};
    std::uint64_t indexHash{};
    Bytes items;
    Bytes offsets;
    std::unordered_map<std::string, size_t> exactPos;
    std::unordered_map<std::string, size_t> lowerPos;
};

std::uint64_t hashIndexPrefix(std::ifstream& file, size_t indexStart, std::uintmax_t fileSize) {
    if (fileSize == static_cast<std::uintmax_t>(-1) || indexStart >= fileSize) return 0;
    const size_t n = static_cast<size_t>(std::min<std::uintmax_t>(fileSize - indexStart, 256));
    Bytes buf(n);
    file.seekg(static_cast<std::streamoff>(indexStart));
    if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n))) return 0;
    std::uint64_t h = 14695981039346656037ull;
    for (std::uint8_t b : buf) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

std::shared_ptr<const SueIndex> loadSueIndex(const std::filesystem::path& archive, std::ifstream& file, size_t indexStart) {
    static std::mutex cacheMutex;
    static std::vector<std::shared_ptr<const SueIndex>> cache;

    std::error_code ec;
    const auto normKey = archive.lexically_normal();
    const auto fsize = std::filesystem::file_size(archive, ec);
    const auto fileSize = ec ? static_cast<std::uintmax_t>(-1) : fsize;
    ec.clear();
    const auto mtime = std::filesystem::last_write_time(archive, ec);
    const bool hasStat = !ec && fileSize != static_cast<std::uintmax_t>(-1);
    const std::uint64_t prefixHash = hasStat ? hashIndexPrefix(file, indexStart, fileSize) : 0;

    if (hasStat) {
        std::lock_guard lock(cacheMutex);
        for (const auto& entry : cache) {
            if (entry->key == normKey && entry->fileSize == fileSize &&
                entry->mtime == mtime && entry->indexStart == indexStart &&
                entry->indexHash == prefixHash) {
                return entry;
            }
        }
    }

    file.clear();
    file.seekg(static_cast<std::streamoff>(indexStart));
    Bytes items = table(file, 20);
    const Bytes names = table(file, 0);
    Bytes offsets = table(file, 4);
    if (items.size() % 20 || offsets.size() % 4) throw std::runtime_error("Invalid SUE index layout");

    auto idx = std::make_shared<SueIndex>();
    idx->key = normKey;
    idx->fileSize = fileSize;
    idx->mtime = mtime;
    idx->indexStart = indexStart;
    idx->indexHash = prefixHash;
    idx->items = std::move(items);
    idx->offsets = std::move(offsets);
    idx->exactPos.reserve(idx->items.size() / 20 + 1);
    idx->lowerPos.reserve(idx->items.size() / 20 + 1);

    for (size_t pos = 0; pos < idx->items.size(); pos += 20) {
        const auto info = slice(idx->items, pos, 20);
        const size_t n = u32(info, 0);
        if (n >= names.size()) throw std::runtime_error("Invalid SUE name offset");
        const auto* startPtr = reinterpret_cast<const char*>(names.data() + n);
        const size_t maxLen = names.size() - n;
        const auto* nul = static_cast<const char*>(std::memchr(startPtr, '\0', maxLen));
        if (!nul) continue;
        std::string name(startPtr, std::size_t(nul - startPtr));
        idx->exactPos.try_emplace(name, pos);
        for (char& ch : name) ch = static_cast<char>(asciiLower(static_cast<unsigned char>(ch)));
        idx->lowerPos.try_emplace(std::move(name), pos);
    }

    if (hasStat) {
        std::lock_guard lock(cacheMutex);
        for (auto& existing : cache) {
            if (existing->key == normKey) {
                existing = idx;
                return idx;
            }
        }
        if (cache.size() >= 16) cache.erase(cache.begin());
        cache.push_back(idx);
    }
    return idx;
}

struct OpenedSue {
    std::ifstream file;
    size_t indexStart{};
    std::shared_ptr<const SueIndex> idx;
};

OpenedSue openSueArchive(const std::filesystem::path& archive) {
    OpenedSue out;
    out.file.open(archive, std::ios::binary);
    if (!out.file) throw std::runtime_error("Cannot open SUE archive");
    const auto header = read(out.file, 8);
    if (u32(header, 0) != 0x53465a46) throw std::runtime_error("Missing FZFS signature");
    out.indexStart = 8ull + u32(header, 4);
    out.idx = loadSueIndex(archive, out.file, out.indexStart);
    return out;
}

std::optional<size_t> findSueEntryPos(const SueIndex& idx, std::string_view entry) {
    const std::string key(entry);
    if (const auto it = idx.exactPos.find(key); it != idx.exactPos.end()) {
        return it->second;
    }
    std::string lower = key;
    for (char& ch : lower) ch = static_cast<char>(asciiLower(static_cast<unsigned char>(ch)));
    if (const auto lit = idx.lowerPos.find(lower); lit != idx.lowerPos.end()) {
        return lit->second;
    }
    return std::nullopt;
}

Bytes readSueRangeImpl(OpenedSue& opened, size_t entryPos, size_t byteOffset, size_t maxBytes) {
    const auto info = slice(opened.idx->items, entryPos, 20);
    const size_t kind = u32(info, 4), first = u32(info, 8), size = u32(info, 12);
    if (size > assetLimit) throw std::runtime_error("SUE entry exceeds reader limit");
    if (kind != 2 && kind != 8) throw std::runtime_error("Unsupported SUE storage type");
    size_t chunk = 0;
    if (kind == 8) {
        chunk = u32(info, 16);
        if (!chunk) throw std::runtime_error("Zero SUE chunk size");
        const size_t count = (size + chunk - 1) / chunk;
        if (first >= opened.idx->offsets.size() / 4 || count >= opened.idx->offsets.size() / 4 - first)
            throw std::runtime_error("Invalid SUE chunk range");
    }
    if (byteOffset >= size || maxBytes == 0) return {};
    const size_t want = std::min(maxBytes, size - byteOffset);
    if (kind == 2) return payload(opened.file, first + byteOffset, want, opened.indexStart);

    const size_t firstChunk = byteOffset / chunk;
    const size_t lastChunk = (byteOffset + want - 1) / chunk;
    Bytes result;
    result.reserve(want);
    for (size_t i = firstChunk; i <= lastChunk; ++i) {
        const size_t start = u32(opened.idx->offsets, 4 * (first + i));
        const size_t end = u32(opened.idx->offsets, 4 * (first + i + 1));
        const size_t chunkOff = i * chunk;
        const size_t expected = std::min(chunk, size - chunkOff);
        if (end < start || end - start > compressBound(expected))
            throw std::runtime_error("Invalid SUE chunk length");
        const auto decoded = inflateExact(payload(opened.file, start, end - start, opened.indexStart), expected);
        const size_t sliceBegin = (byteOffset > chunkOff) ? (byteOffset - chunkOff) : 0;
        const size_t sliceEnd = std::min(decoded.size(), (byteOffset + want) - chunkOff);
        result.insert(result.end(), decoded.begin() + static_cast<std::ptrdiff_t>(sliceBegin),
                      decoded.begin() + static_cast<std::ptrdiff_t>(sliceEnd));
    }
    return result;
}

std::string toLowerAsciiStr(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char ch : sv) out.push_back(static_cast<char>(asciiLower(static_cast<unsigned char>(ch))));
    return out;
}

std::string_view trimAsciiSv(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n'))
        sv.remove_suffix(1);
    return sv;
}

std::filesystem::path resolvePathIgnoreCase(const std::filesystem::path& p) {
    if (p.empty()) return p;
    if (p.is_absolute()) {
        return resolveCaseInsensitivePath(p.root_path(), p.relative_path());
    }
    return resolveCaseInsensitivePath(std::filesystem::path{}, p);
}

std::optional<std::filesystem::path> resolveLooseMountEntry(const std::filesystem::path& dirRoot,
                                                            std::string_view entry) {
    std::string norm;
    norm.reserve(entry.size());
    for (char ch : entry) norm.push_back(ch == '\\' ? '/' : ch);

    std::filesystem::path rel;
    std::size_t pos = 0;
    while (pos <= norm.size()) {
        const std::size_t slash = norm.find('/', pos);
        const std::string_view seg = (slash == std::string_view::npos)
            ? std::string_view(norm).substr(pos)
            : std::string_view(norm).substr(pos, slash - pos);
        if (seg == "..") return std::nullopt;
        if (!seg.empty() && seg != ".") {
            rel /= std::filesystem::path(seg);
        }
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
    if (rel.empty()) return std::nullopt;
    const auto resolved = resolveCaseInsensitivePath(dirRoot, rel);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(resolved, ec) || ec) return std::nullopt;
    return resolved;
}

bool hasGzipMagic(const std::filesystem::path& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) return false;
    unsigned char hdr[2]{};
    if (!in.read(reinterpret_cast<char*>(hdr), 2)) return false;
    return hdr[0] == 0x1fu && hdr[1] == 0x8bu;
}

Bytes readRawBoundedFile(const std::filesystem::path& filePath, std::size_t limit) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(filePath, ec);
    if (ec || sz > limit) throw std::runtime_error("Loose asset exceeds reader limit");
    std::ifstream in(filePath, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open loose asset file");
    Bytes out(static_cast<std::size_t>(sz));
    if (!out.empty() && !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())))
        throw std::runtime_error("Truncated loose asset file");
    return out;
}

Bytes inflateGzipBytes(View compressed) {
    struct ZGuard {
        z_stream zs{};
        bool active{false};
        ~ZGuard() {
            if (active) inflateEnd(&zs);
        }
    } g;
    if (inflateInit2(&g.zs, 15 + 32) != Z_OK)
        throw std::runtime_error("Failed to initialize gzip decoder");
    g.active = true;
    g.zs.next_in = const_cast<Bytef*>(compressed.data());
    g.zs.avail_in = static_cast<uInt>(std::min<std::size_t>(compressed.size(), std::numeric_limits<uInt>::max()));

    Bytes out;
    std::uint8_t buf[65536];
    for (;;) {
        g.zs.next_out = buf;
        g.zs.avail_out = sizeof(buf);
        const int rc = inflate(&g.zs, Z_NO_FLUSH);
        const std::size_t produced = sizeof(buf) - g.zs.avail_out;
        if (out.size() + produced > assetLimit)
            throw std::runtime_error("Gzip loose asset exceeds reader limit");
        out.insert(out.end(), buf, buf + produced);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) throw std::runtime_error("Corrupt gzip loose asset");
        if (g.zs.avail_in == 0 && produced == 0)
            throw std::runtime_error("Truncated gzip loose asset");
    }
    return out;
}

Bytes readLooseFileOrGzip(const std::filesystem::path& filePath) {
    Bytes raw = readRawBoundedFile(filePath, assetLimit);
    if (raw.size() >= 2 && raw[0] == 0x1fu && raw[1] == 0x8bu) {
        return inflateGzipBytes(raw);
    }
    return raw;
}

std::string normalizeForwardEntry(const std::filesystem::path& rel) {
    std::string out = rel.generic_string();
    return out;
}

struct ParsedIni {
    struct DataToken {
        std::string virtualPrefix;
        std::string rawTarget;
    };
    std::vector<DataToken> commonTokens;
    std::vector<DataToken> menuTokens;
    std::vector<DataToken> gameTokens;
    std::vector<DataToken> addonsTokens;
    std::vector<DataToken> movieTokens;
    std::string soundsStem;
    std::string backWav;
    std::map<unsigned, std::vector<DataToken>> schemeTokens;
    std::string singlePrefix{"_single\\"};
    std::string campaignsPrefix{"_camp"};
    std::string multiPrefix{"maps\\mplay\\"};
    std::optional<std::string> turnDelayRaw;
    bool loaded{false};
};

std::vector<ParsedIni::DataToken> parseIniList(std::string_view val) {
    std::vector<ParsedIni::DataToken> out;
    std::size_t pos = 0;
    while (pos <= val.size()) {
        const std::size_t semi = val.find(';', pos);
        std::string_view tok = trimAsciiSv(semi == std::string_view::npos ? val.substr(pos) : val.substr(pos, semi - pos));
        if (!tok.empty()) {
            ParsedIni::DataToken item;
            if (tok.front() == '[') {
                if (const std::size_t rb = tok.find(']'); rb != std::string_view::npos) {
                    item.virtualPrefix = std::string(tok.substr(1, rb - 1));
                    tok = trimAsciiSv(tok.substr(rb + 1));
                }
            }
            item.rawTarget = std::string(tok);
            if (!item.rawTarget.empty()) out.push_back(std::move(item));
        }
        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }
    return out;
}

ParsedIni parseSudtestIni(const std::filesystem::path& iniPath) {
    ParsedIni ini;
    std::error_code ec;
    if (iniPath.empty() || !std::filesystem::is_regular_file(iniPath, ec) || ec) return ini;
    std::ifstream in(iniPath, std::ios::binary);
    if (!in) return ini;
    ini.loaded = true;

    std::string section;
    for (std::string line; std::getline(in, line);) {
        std::string_view sv = trimAsciiSv(line);
        if (sv.empty() || sv.front() == ';' || sv.front() == '#') continue;
        if (sv.front() == '[') {
            if (const std::size_t rb = sv.find(']'); rb != std::string_view::npos) {
                section = toLowerAsciiStr(trimAsciiSv(sv.substr(1, rb - 1)));
            } else {
                section.clear();
            }
            continue;
        }
        const std::size_t eq = sv.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key = toLowerAsciiStr(trimAsciiSv(sv.substr(0, eq)));
        std::string_view val = trimAsciiSv(sv.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }

        if (section == "data") {
            if (key == "common") ini.commonTokens = parseIniList(val);
            else if (key == "menu") ini.menuTokens = parseIniList(val);
            else if (key == "game") ini.gameTokens = parseIniList(val);
            else if (key == "addons") ini.addonsTokens = parseIniList(val);
            else if (key == "movies") ini.movieTokens = parseIniList(val);
            else if (key == "sounds") ini.soundsStem = std::string(val);
            else if (key == "backwav") ini.backWav = std::string(val);
            else if (key.starts_with("scheme")) {
                unsigned num = 0;
                const auto* p0 = key.data() + 6;
                const auto* p1 = key.data() + key.size();
                const auto [ptr, ferr] = std::from_chars(p0, p1, num);
                if (ferr == std::errc{} && ptr == p1 && num >= 1u && num <= 64u) {
                    ini.schemeTokens[num] = parseIniList(val);
                }
            }
        } else if (section == "game") {
            if (key == "singleprefix") ini.singlePrefix = std::string(val);
            else if (key == "campaignsprefix") ini.campaignsPrefix = std::string(val);
            else if (key == "multiprefix") ini.multiPrefix = std::string(val);
            else if (key == "turndelay") ini.turnDelayRaw = std::string(val);
        }
    }
    return ini;
}

std::filesystem::path normalizeNativeTarget(std::string_view raw) {
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (char ch : raw) cleaned.push_back(ch == '\\' ? '/' : ch);
    while (!cleaned.empty() && cleaned.back() == '/') cleaned.pop_back();
    if (cleaned.empty()) return {};
    return std::filesystem::path(cleaned);
}

std::filesystem::path defaultUserModsRoot() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "ss2" / "mods";
    }
#ifdef _WIN32
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData) {
        return std::filesystem::path(localAppData) / "ss2" / "mods";
    }
    if (const char* appData = std::getenv("APPDATA"); appData && *appData) {
        return std::filesystem::path(appData) / "ss2" / "mods";
    }
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile) {
        return std::filesystem::path(profile) / "AppData" / "Local" / "ss2" / "mods";
    }
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / "ss2" / "mods";
    }
#endif
    return {};
}

bool isValidModDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return false;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name.front() == '.') continue;
        std::error_code itemEc;
        if (it->is_regular_file(itemEc)) {
            if (equalsIgnoreCaseAscii(name, "sudtest.ini")) return true;
            const std::string ext = toLowerAsciiStr(it->path().extension().string());
            if (ext == ".sue" || ext == ".ssx" || ext == ".hdr" || ext == ".col" ||
                ext == ".pck" || ext == ".rs2" || ext == ".ani" || ext == ".hot") {
                return true;
            }
        } else if (it->is_directory(itemEc)) {
            const std::string low = toLowerAsciiStr(name);
            if (low == "maps" || low == "addons" || low == "music" || low == "movies" ||
                low == "units" || low == "shots" || low == "explosions" || low == "avia" ||
                low == "misc" || low == "land" || low == "game_common" || low == "desc_common" ||
                low == "menu_common" || low == "mycommon") {
                return true;
            }
        }
    }
    return false;
}

std::mutex& activeModMutex() {
    static std::mutex m;
    return m;
}

std::string& activeModStorage() {
    static std::string s;
    return s;
}

bool directoryHasLooseAssets(const std::filesystem::path& dir, bool includeSubdirs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return false;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code itemEc;
        const std::string nm = toLowerAsciiStr(it->path().filename().string());
        if (it->is_regular_file(itemEc)) {
            const std::string ext = toLowerAsciiStr(it->path().extension().string());
            if (ext == ".col" || ext == ".pck" || ext == ".rs2" || ext == ".ani" ||
                ext == ".hot" || ext == ".pl" || ext == ".dat" || ext == ".jpg") {
                return true;
            }
        } else if (includeSubdirs && it->is_directory(itemEc)) {
            if (nm == "units" || nm == "shots" || nm == "explosions" ||
                nm == "avia" || nm == "misc" || nm == "land") {
                return true;
            }
        }
    }
    return false;
}

bool directoryHasAnyRegularFile(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return false;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fEc;
        if (it->is_regular_file(fEc)) return true;
    }
    return false;
}

void appendUniqueExistingPath(std::vector<std::filesystem::path>& out,
                              std::vector<std::filesystem::path>& seen,
                              const std::filesystem::path& cand) {
    if (cand.empty()) return;
    const auto resolved = resolvePathIgnoreCase(cand);
    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec) || ec) return;
    std::filesystem::path key = std::filesystem::weakly_canonical(resolved, ec);
    if (ec || key.empty()) key = resolved.lexically_normal();
    if (std::ranges::find(seen, key) != seen.end()) return;
    seen.push_back(std::move(key));
    out.push_back(resolved);
}

void resolveIniTokenPaths(std::vector<std::filesystem::path>& out,
                          std::vector<std::filesystem::path>& seen,
                          const std::filesystem::path& primaryRoot,
                          const std::filesystem::path& fallbackRoot,
                          std::string_view rawTarget) {
    const auto rel = normalizeNativeTarget(rawTarget);
    if (rel.empty()) return;
    if (rel.is_absolute()) {
        appendUniqueExistingPath(out, seen, rel);
        return;
    }
    if (!primaryRoot.empty()) appendUniqueExistingPath(out, seen, primaryRoot / rel);
    if (!fallbackRoot.empty()) appendUniqueExistingPath(out, seen, fallbackRoot / rel);
}

struct ModVfsContext {
    std::filesystem::path gameDir;
    std::optional<ModInfo> mod;
    ParsedIni modIni;
    ParsedIni baseIni;
};

ModVfsContext buildModVfsContext(const std::filesystem::path& gameDir) {
    ModVfsContext ctx;
    ctx.gameDir = gameDir;
    const std::string spec = activeMod();
    if (!spec.empty()) {
        ctx.mod = resolveMod(gameDir, spec);
        if (ctx.mod && !ctx.mod->iniPath.empty()) {
            ctx.modIni = parseSudtestIni(ctx.mod->iniPath);
        }
    }
    if (!gameDir.empty()) {
        const auto baseIniPath = resolveCaseInsensitivePath(gameDir, "sudtest.ini");
        ctx.baseIni = parseSudtestIni(baseIniPath);
    }
    return ctx;
}

std::vector<std::filesystem::path> listSortedModSueFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec) || ec) return out;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fEc;
        if (!it->is_regular_file(fEc)) continue;
        if (equalsIgnoreCaseAscii(it->path().extension().string(), ".sue")) {
            out.push_back(it->path());
        }
    }
    std::ranges::sort(out, [](const auto& a, const auto& b) {
        return toLowerAsciiStr(a.filename().string()) < toLowerAsciiStr(b.filename().string());
    });
    return out;
}

} // namespace

std::optional<Bytes> tryReadSueEntry(const std::filesystem::path& archive, std::string_view entry) {
    std::error_code ec;
    if (std::filesystem::is_directory(archive, ec) && !ec) {
        const auto filePath = resolveLooseMountEntry(archive, entry);
        if (!filePath) return std::nullopt;
        return readLooseFileOrGzip(*filePath);
    }
    auto opened = openSueArchive(archive);
    const auto matchedPos = findSueEntryPos(*opened.idx, entry);
    if (!matchedPos) return std::nullopt;
    return readSueRangeImpl(opened, *matchedPos, 0, std::numeric_limits<size_t>::max());
}

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(static_cast<unsigned char>(a[i])) !=
            asciiLower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::filesystem::path resolveCaseInsensitivePath(const std::filesystem::path& base,
                                                 const std::filesystem::path& relative) {
    const auto direct = base / relative;
    std::error_code ec;
    if (relative.empty() || std::filesystem::exists(direct, ec)) return direct;

    std::filesystem::path current = base;
    for (const auto& part : relative) {
        if (part.empty() || part == ".") continue;
        if (part == ".." || part == relative.root_name() || part == relative.root_directory()) {
            current /= part;
            continue;
        }
        const auto exact = current / part;
        ec.clear();
        if (std::filesystem::exists(exact, ec)) {
            current = exact;
            continue;
        }
        const auto dir = current.empty() ? std::filesystem::path(".") : current;
        ec.clear();
        if (!std::filesystem::is_directory(dir, ec) || ec) return direct;

        const auto target = part.string();
        bool matched = false;
        ec.clear();
        std::filesystem::directory_iterator it(
            dir, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) return direct;
        for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename();
            if (equalsIgnoreCaseAscii(name.string(), target)) {
                current /= name;
                matched = true;
                break;
            }
        }
        if (!matched) return direct;
    }
    return current;
}

Bytes readSueEntry(const std::filesystem::path& archive, std::string_view entry) {
    if (auto bytes = tryReadSueEntry(archive, entry)) return *bytes;
    throw std::runtime_error("SUE entry not found");
}

Bytes readSueEntryFallback(const std::vector<std::filesystem::path>& archives,
                           std::string_view entry) {
    for (const auto& archive : archives) {
        const auto resolved = resolvePathIgnoreCase(archive);
        std::error_code ec;
        if (!std::filesystem::exists(resolved, ec)) continue;
        if (auto bytes = tryReadSueEntry(resolved, entry)) return *bytes;
    }
    throw std::runtime_error("SUE entry not found in fallback archives: " + std::string(entry));
}

std::optional<std::size_t> sueEntrySize(const std::filesystem::path& archive, std::string_view entry) {
    try {
        std::error_code ec;
        if (std::filesystem::is_directory(archive, ec) && !ec) {
            const auto filePath = resolveLooseMountEntry(archive, entry);
            if (!filePath) return std::nullopt;
            if (hasGzipMagic(*filePath)) {
                return readLooseFileOrGzip(*filePath).size();
            }
            const auto sz = std::filesystem::file_size(*filePath, ec);
            if (ec || sz > assetLimit) return std::nullopt;
            return static_cast<std::size_t>(sz);
        }
        const auto opened = openSueArchive(archive);
        const auto matchedPos = findSueEntryPos(*opened.idx, entry);
        if (!matchedPos) return std::nullopt;
        const auto info = slice(opened.idx->items, *matchedPos, 20);
        const size_t size = u32(info, 12);
        if (size > assetLimit) return std::nullopt;
        return size;
    } catch (...) {
        return std::nullopt;
    }
}

Bytes readSueEntryRange(const std::filesystem::path& archive, std::string_view entry,
                        std::size_t byteOffset, std::size_t maxBytes) {
    std::error_code ec;
    if (std::filesystem::is_directory(archive, ec) && !ec) {
        const auto filePath = resolveLooseMountEntry(archive, entry);
        if (!filePath) throw std::runtime_error("SUE entry not found");
        if (hasGzipMagic(*filePath)) {
            const Bytes all = readLooseFileOrGzip(*filePath);
            if (byteOffset >= all.size() || maxBytes == 0) return {};
            const size_t want = std::min(maxBytes, all.size() - byteOffset);
            return Bytes(all.begin() + static_cast<std::ptrdiff_t>(byteOffset),
                         all.begin() + static_cast<std::ptrdiff_t>(byteOffset + want));
        }
        const auto sz = std::filesystem::file_size(*filePath, ec);
        if (ec || sz > assetLimit) throw std::runtime_error("Loose asset exceeds reader limit");
        const size_t total = static_cast<size_t>(sz);
        if (byteOffset >= total || maxBytes == 0) return {};
        const size_t want = std::min(maxBytes, total - byteOffset);
        std::ifstream in(*filePath, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open loose asset file");
        in.seekg(static_cast<std::streamoff>(byteOffset));
        return read(in, want);
    }
    auto opened = openSueArchive(archive);
    const auto matchedPos = findSueEntryPos(*opened.idx, entry);
    if (!matchedPos) throw std::runtime_error("SUE entry not found");
    return readSueRangeImpl(opened, *matchedPos, byteOffset, maxBytes);
}

std::optional<Bytes> tryReadSueEntryChain(std::span<const std::filesystem::path> archives,
                                          std::string_view entry) {
    for (const auto& archive : archives) {
        if (archive.empty()) continue;
        const auto resolved = resolvePathIgnoreCase(archive);
        std::error_code ec;
        if (!std::filesystem::exists(resolved, ec) || ec) continue;
        try {
            if (auto bytes = tryReadSueEntry(resolved, entry)) return bytes;
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

Bytes readSueEntryChain(std::span<const std::filesystem::path> archives, std::string_view entry) {
    if (auto bytes = tryReadSueEntryChain(archives, entry)) return *bytes;
    throw std::runtime_error("SUE entry not found");
}

std::optional<std::size_t> sueChainEntrySize(std::span<const std::filesystem::path> archives,
                                             std::string_view entry) {
    for (const auto& archive : archives) {
        if (archive.empty()) continue;
        const auto resolved = resolvePathIgnoreCase(archive);
        std::error_code ec;
        if (!std::filesystem::exists(resolved, ec) || ec) continue;
        if (auto sz = sueEntrySize(resolved, entry)) return sz;
    }
    return std::nullopt;
}

Bytes readSueChainRange(std::span<const std::filesystem::path> archives, std::string_view entry,
                        std::size_t byteOffset, std::size_t maxBytes) {
    for (const auto& archive : archives) {
        if (archive.empty()) continue;
        const auto resolved = resolvePathIgnoreCase(archive);
        std::error_code ec;
        if (!std::filesystem::exists(resolved, ec) || ec) continue;
        if (sueEntrySize(resolved, entry).has_value()) {
            return readSueEntryRange(resolved, entry, byteOffset, maxBytes);
        }
    }
    throw std::runtime_error("SUE entry not found");
}

std::vector<std::string> listSueEntries(const std::filesystem::path& archive) {
    std::vector<std::string> out;
    if (archive.empty()) return out;
    std::error_code ec;
    if (std::filesystem::is_directory(archive, ec) && !ec) {
        for (std::filesystem::recursive_directory_iterator it(
                 archive, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code fEc;
            if (!it->is_regular_file(fEc)) continue;
            const auto rel = std::filesystem::relative(it->path(), archive, fEc);
            if (!fEc && !rel.empty()) out.push_back(normalizeForwardEntry(rel));
        }
        std::ranges::sort(out);
        return out;
    }
    try {
        auto opened = openSueArchive(archive);
        opened.file.clear();
        opened.file.seekg(static_cast<std::streamoff>(opened.indexStart));
        const Bytes items = table(opened.file, 20);
        const Bytes names = table(opened.file, 0);
        for (size_t pos = 0; pos < items.size(); pos += 20) {
            const auto info = slice(items, pos, 20);
            const size_t n = u32(info, 0);
            if (n >= names.size()) continue;
            const auto* startPtr = reinterpret_cast<const char*>(names.data() + n);
            const auto* nul = static_cast<const char*>(std::memchr(startPtr, '\0', names.size() - n));
            if (!nul) continue;
            out.emplace_back(startPtr, static_cast<std::size_t>(nul - startPtr));
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

void setActiveMod(std::string_view modSpec) {
    const std::string_view trimmed = trimAsciiSv(modSpec);
    std::lock_guard lock(activeModMutex());
    if (trimmed.empty() || equalsIgnoreCaseAscii(trimmed, "none") ||
        equalsIgnoreCaseAscii(trimmed, "vanilla") || equalsIgnoreCaseAscii(trimmed, "original")) {
        activeModStorage().clear();
    } else {
        activeModStorage() = std::string(trimmed);
    }
}

std::string activeMod() {
    std::lock_guard lock(activeModMutex());
    return activeModStorage();
}

std::vector<ModInfo> listInstalledMods(const std::filesystem::path& gameDir,
                                       const std::filesystem::path& userModsDir) {
    std::vector<ModInfo> out;
    std::vector<std::filesystem::path> seenRoots;
    auto pushUniqueMod = [&](ModInfo info) {
        std::error_code ec;
        const auto target = !info.iniPath.empty() && info.rootDir == gameDir ? info.iniPath : info.rootDir;
        std::filesystem::path can = std::filesystem::weakly_canonical(target, ec);
        if (ec || can.empty()) can = target.lexically_normal();
        if (std::ranges::find(seenRoots, can) != seenRoots.end()) return;
        for (const auto& existing : out) {
            if (equalsIgnoreCaseAscii(existing.id, info.id)) {
                info.id = info.rootDir.filename().string() + ":" + info.id;
                break;
            }
        }
        seenRoots.push_back(std::move(can));
        out.push_back(std::move(info));
    };

    auto scanParentDir = [&](const std::filesystem::path& parent) {
        std::error_code ec;
        if (parent.empty() || !std::filesystem::is_directory(parent, ec) || ec) return;
        std::vector<std::filesystem::path> dirs;
        for (std::filesystem::directory_iterator it(parent, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code dEc;
            if (!it->is_directory(dEc)) continue;
            const std::string name = it->path().filename().string();
            if (name.empty() || name.front() == '.') continue;
            if (isValidModDirectory(it->path())) dirs.push_back(it->path());
        }
        std::ranges::sort(dirs);
        for (const auto& d : dirs) {
            const auto ini = resolveCaseInsensitivePath(d, "sudtest.ini");
            std::error_code fEc;
            std::filesystem::path foundIni = std::filesystem::is_regular_file(ini, fEc) ? ini : std::filesystem::path{};
            const std::string nm = d.filename().string();
            pushUniqueMod(ModInfo{nm, nm, d, std::move(foundIni)});
        }
    };

    if (!gameDir.empty()) {
        for (const char* sub : {"mods", "Run", "switcher"}) {
            const auto dir = resolveCaseInsensitivePath(gameDir, sub);
            scanParentDir(dir);
        }
        std::error_code ec;
        std::vector<std::filesystem::path> iniProfiles;
        for (std::filesystem::directory_iterator it(gameDir, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code fEc;
            if (!it->is_regular_file(fEc)) continue;
            const std::string fn = it->path().filename().string();
            const std::string low = toLowerAsciiStr(fn);
            if (low.starts_with("sudtest_") && low.ends_with(".ini") && fn.size() > 12) {
                iniProfiles.push_back(it->path());
            }
        }
        std::ranges::sort(iniProfiles);
        for (const auto& ip : iniProfiles) {
            const std::string fn = ip.stem().string();
            const std::string id = fn.substr(8);
            pushUniqueMod(ModInfo{id, id, gameDir, ip});
        }
    }

    const auto uRoot = userModsDir.empty() ? defaultUserModsRoot() : userModsDir;
    if (!uRoot.empty()) scanParentDir(uRoot);

    std::ranges::sort(out, [](const ModInfo& a, const ModInfo& b) {
        const auto la = toLowerAsciiStr(a.name);
        const auto lb = toLowerAsciiStr(b.name);
        if (la != lb) return la < lb;
        return a.id < b.id;
    });
    return out;
}

std::optional<ModInfo> resolveMod(const std::filesystem::path& gameDir,
                                  std::string_view modSpec,
                                  const std::filesystem::path& userModsDir) {
    const std::string_view trimmed = trimAsciiSv(modSpec);
    if (trimmed.empty() || equalsIgnoreCaseAscii(trimmed, "none") ||
        equalsIgnoreCaseAscii(trimmed, "vanilla") || equalsIgnoreCaseAscii(trimmed, "original")) {
        return std::nullopt;
    }

    const std::filesystem::path specPath(trimmed);
    const auto resolvedPath = resolvePathIgnoreCase(specPath);
    std::error_code ec;
    if (std::filesystem::is_regular_file(resolvedPath, ec) &&
        equalsIgnoreCaseAscii(resolvedPath.extension().string(), ".ini")) {
        const auto root = resolvedPath.parent_path().empty() ? gameDir : resolvedPath.parent_path();
        const std::string nm = resolvedPath.stem().string();
        return ModInfo{nm, nm, root, resolvedPath};
    }
    if (std::filesystem::is_directory(resolvedPath, ec)) {
        const auto ini = resolveCaseInsensitivePath(resolvedPath, "sudtest.ini");
        std::filesystem::path foundIni = std::filesystem::is_regular_file(ini, ec) ? ini : std::filesystem::path{};
        const std::string nm = resolvedPath.filename().empty() ? std::string(trimmed) : resolvedPath.filename().string();
        return ModInfo{nm, nm, resolvedPath, std::move(foundIni)};
    }

    for (const auto& m : listInstalledMods(gameDir, userModsDir)) {
        if (equalsIgnoreCaseAscii(m.id, trimmed) || equalsIgnoreCaseAscii(m.name, trimmed)) {
            return m;
        }
    }

    if (!gameDir.empty() && !specPath.is_absolute()) {
        const auto relDir = resolveCaseInsensitivePath(gameDir, specPath);
        if (std::filesystem::is_directory(relDir, ec)) {
            const auto ini = resolveCaseInsensitivePath(relDir, "sudtest.ini");
            std::filesystem::path foundIni = std::filesystem::is_regular_file(ini, ec) ? ini : std::filesystem::path{};
            const std::string nm = relDir.filename().string();
            return ModInfo{nm, nm, relDir, std::move(foundIni)};
        }
    }
    return std::nullopt;
}

namespace {

bool isDescArchiveToken(const std::filesystem::path& p) {
    const std::string low = toLowerAsciiStr(p.filename().string());
    if (low.find("lang") != std::string::npos || low.find("lng") != std::string::npos ||
        low.find("edit") != std::string::npos) {
        return false;
    }
    if (low.find("desc") != std::string::npos) return true;
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        for (const char* sub : {"UNITS", "SHOTS", "EXPLOSIONS", "AVIA", "MISC", "LAND"}) {
            if (std::filesystem::is_directory(resolveCaseInsensitivePath(p, sub), ec)) return true;
        }
    }
    return false;
}

bool isLangArchiveToken(const std::filesystem::path& p) {
    const std::string low = toLowerAsciiStr(p.filename().string());
    if (low.find("edit") != std::string::npos) return false;
    return low.find("lang") != std::string::npos || low.find("lng") != std::string::npos;
}

bool isSchemeTerrainArchiveToken(const std::filesystem::path& p) {
    const std::string low = toLowerAsciiStr(p.filename().string());
    if (low.find("desc") != std::string::npos || low.find("lang") != std::string::npos ||
        low.find("lng") != std::string::npos || low.find("edit") != std::string::npos) {
        return false;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) return true;
    return equalsIgnoreCaseAscii(p.extension().string(), ".sue");
}

void collectDropInModArchives(const std::filesystem::path& modRoot,
                              const std::filesystem::path& gameDir,
                              std::vector<std::filesystem::path>& out,
                              std::vector<std::filesystem::path>& seen,
                              int kind, unsigned scheme = 0) {
    std::error_code ec;
    if (modRoot.empty() || !std::filesystem::is_directory(modRoot, ec) || ec) return;
    const bool sameAsGameDir = !gameDir.empty() &&
        std::filesystem::weakly_canonical(modRoot, ec) == std::filesystem::weakly_canonical(gameDir, ec);
    if (sameAsGameDir) return;

    const auto sues = listSortedModSueFiles(modRoot);
    const std::string schemeTag = "scheme" + std::to_string(scheme + 1u);

    if (kind == 0) { // Game
        for (const char* sub : {"game_common", "mycommon", "common"}) {
            const auto d = resolveCaseInsensitivePath(modRoot, sub);
            if (std::filesystem::is_directory(d, ec)) appendUniqueExistingPath(out, seen, d);
        }
        if (directoryHasLooseAssets(modRoot, false)) appendUniqueExistingPath(out, seen, modRoot);
        for (const auto& s : sues) {
            const std::string low = toLowerAsciiStr(s.stem().string());
            if ((low.find("common") != std::string::npos || low.find("game") != std::string::npos) &&
                low.find("menu") == std::string::npos && low.find("desc") == std::string::npos &&
                low.find("edit") == std::string::npos && low.find("scheme") == std::string::npos) {
                appendUniqueExistingPath(out, seen, s);
            }
        }
    } else if (kind == 1) { // Desc
        const std::string descSchemeDir = "desc_scheme" + std::to_string(scheme + 1u);
        for (const auto& sub : {descSchemeDir, std::string("desc_common"), std::string("desc")}) {
            const auto d = resolveCaseInsensitivePath(modRoot, sub);
            if (std::filesystem::is_directory(d, ec)) appendUniqueExistingPath(out, seen, d);
        }
        if (directoryHasLooseAssets(modRoot, true)) {
            for (const char* sub : {"UNITS", "SHOTS", "EXPLOSIONS", "AVIA", "MISC", "LAND"}) {
                if (std::filesystem::is_directory(resolveCaseInsensitivePath(modRoot, sub), ec)) {
                    appendUniqueExistingPath(out, seen, modRoot);
                    break;
                }
            }
        }
        for (const auto& s : sues) {
            const std::string low = toLowerAsciiStr(s.stem().string());
            if (low.find("desc") != std::string::npos && low.find(schemeTag) != std::string::npos &&
                low.find("lang") == std::string::npos && low.find("edit") == std::string::npos) {
                appendUniqueExistingPath(out, seen, s);
            }
        }
        for (const auto& s : sues) {
            const std::string low = toLowerAsciiStr(s.stem().string());
            if (low.find("desc") != std::string::npos && low.find("scheme") == std::string::npos &&
                low.find("lang") == std::string::npos && low.find("edit") == std::string::npos) {
                appendUniqueExistingPath(out, seen, s);
            }
        }
    } else if (kind == 2) { // Lang
        const std::string langSchemeDir = "lang_desc_scheme" + std::to_string(scheme + 1u);
        for (const auto& sub : {langSchemeDir, std::string("lang"), std::string("lng")}) {
            const auto d = resolveCaseInsensitivePath(modRoot, sub);
            if (std::filesystem::is_directory(d, ec)) appendUniqueExistingPath(out, seen, d);
        }
        for (const auto& s : sues) {
            const std::string low = toLowerAsciiStr(s.stem().string());
            if ((low.find("lang") != std::string::npos || low.find("lng") != std::string::npos) &&
                low.find(schemeTag) != std::string::npos && low.find("edit") == std::string::npos) {
                appendUniqueExistingPath(out, seen, s);
            }
        }
        for (const auto& s : sues) {
            const std::string low = toLowerAsciiStr(s.stem().string());
            if ((low.find("lang") != std::string::npos || low.find("lng") != std::string::npos) &&
                low.find("scheme") == std::string::npos && low.find("edit") == std::string::npos) {
                appendUniqueExistingPath(out, seen, s);
            }
        }
    } else if (kind == 3) { // Menu
        for (const char* sub : {"menu_common", "menu", "lang", "lng"}) {
            const auto d = resolveCaseInsensitivePath(modRoot, sub);
            if (std::filesystem::is_directory(d, ec)) appendUniqueExistingPath(out, seen, d);
        }
        for (const auto& s : sues) {
            const std::string low = toLowerAsciiStr(s.stem().string());
            if ((low.find("menu") != std::string::npos || low.find("lang") != std::string::npos ||
                 low.find("lng") != std::string::npos) &&
                low.find("edit") == std::string::npos) {
                appendUniqueExistingPath(out, seen, s);
            }
        }
    } else if (kind == 4) { // Scheme
        const std::string gDir = "game_scheme" + std::to_string(scheme + 1u);
        const auto d = resolveCaseInsensitivePath(modRoot, gDir);
        if (std::filesystem::is_directory(d, ec)) appendUniqueExistingPath(out, seen, d);
        for (const auto& s : sues) {
            const std::string low = toLowerAsciiStr(s.stem().string());
            if (low.find(schemeTag) != std::string::npos && low.find("desc") == std::string::npos &&
                low.find("lang") == std::string::npos && low.find("edit") == std::string::npos) {
                appendUniqueExistingPath(out, seen, s);
            }
        }
    }
}

void appendTokensWithFilter(std::vector<std::filesystem::path>& out,
                            std::vector<std::filesystem::path>& seen,
                            const std::vector<ParsedIni::DataToken>& tokens,
                            const std::filesystem::path& primaryRoot,
                            const std::filesystem::path& fallbackRoot,
                            bool (*pred)(const std::filesystem::path&)) {
    for (const auto& t : tokens) {
        std::vector<std::filesystem::path> tmp;
        std::vector<std::filesystem::path> tmpSeen;
        resolveIniTokenPaths(tmp, tmpSeen, primaryRoot, fallbackRoot, t.rawTarget);
        for (const auto& c : tmp) {
            if (!pred || pred(c)) appendUniqueExistingPath(out, seen, c);
        }
    }
}

bool matchVirtualPrefix(std::string_view tokenPrefix, std::string_view gamePrefix) {
    if (tokenPrefix.empty() || gamePrefix.empty()) return false;
    const std::string tp = toLowerAsciiStr(tokenPrefix);
    const std::string gp = toLowerAsciiStr(gamePrefix);
    return tp.starts_with(gp);
}

void appendMapDirTokens(std::vector<std::filesystem::path>& out,
                        std::vector<std::filesystem::path>& seen,
                        const std::vector<ParsedIni::DataToken>& commonTokens,
                        std::string_view wantedPrefix,
                        const std::filesystem::path& primaryRoot,
                        const std::filesystem::path& fallbackRoot) {
    for (const auto& t : commonTokens) {
        if (!matchVirtualPrefix(t.virtualPrefix, wantedPrefix)) continue;
        std::vector<std::filesystem::path> cands;
        std::vector<std::filesystem::path> dummySeen;
        resolveIniTokenPaths(cands, dummySeen, primaryRoot, fallbackRoot, t.rawTarget);
        for (const auto& c : cands) {
            std::error_code ec;
            if (std::filesystem::is_directory(c, ec)) appendUniqueExistingPath(out, seen, c);
        }
    }
}

} // namespace

std::vector<std::filesystem::path> resolveGameArchives(const std::filesystem::path& gameDir) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);

    if (ctx.mod) {
        appendTokensWithFilter(out, seen, ctx.modIni.gameTokens, ctx.mod->rootDir, ctx.gameDir, nullptr);
        collectDropInModArchives(ctx.mod->rootDir, ctx.gameDir, out, seen, 0);
    }
    appendTokensWithFilter(out, seen, ctx.baseIni.gameTokens, ctx.gameDir, ctx.gameDir, nullptr);
    appendUniqueExistingPath(out, seen, ctx.gameDir / "game_common.sue");
    return out;
}

std::vector<std::filesystem::path> resolveDescArchives(const std::filesystem::path& gameDir, unsigned scheme) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);
    const unsigned schemeKey = scheme + 1u;

    if (ctx.mod) {
        if (const auto it = ctx.modIni.schemeTokens.find(schemeKey); it != ctx.modIni.schemeTokens.end()) {
            appendTokensWithFilter(out, seen, it->second, ctx.mod->rootDir, ctx.gameDir, isDescArchiveToken);
        }
        if (schemeKey != 1u) {
            if (const auto it1 = ctx.modIni.schemeTokens.find(1u); it1 != ctx.modIni.schemeTokens.end()) {
                appendTokensWithFilter(out, seen, it1->second, ctx.mod->rootDir, ctx.gameDir, isDescArchiveToken);
            }
        }
        collectDropInModArchives(ctx.mod->rootDir, ctx.gameDir, out, seen, 1, scheme);
    }
    if (const auto it = ctx.baseIni.schemeTokens.find(schemeKey); it != ctx.baseIni.schemeTokens.end()) {
        appendTokensWithFilter(out, seen, it->second, ctx.gameDir, ctx.gameDir, isDescArchiveToken);
    }
    if (schemeKey != 1u) {
        if (const auto it1 = ctx.baseIni.schemeTokens.find(1u); it1 != ctx.baseIni.schemeTokens.end()) {
            appendTokensWithFilter(out, seen, it1->second, ctx.gameDir, ctx.gameDir, isDescArchiveToken);
        }
    }
    if (scheme > 0u) {
        appendUniqueExistingPath(out, seen, ctx.gameDir / ("desc_scheme" + std::to_string(schemeKey) + ".sue"));
    }
    appendUniqueExistingPath(out, seen, ctx.gameDir / "desc_common.sue");
    return out;
}

std::vector<std::filesystem::path> resolveLangArchives(const std::filesystem::path& gameDir, unsigned scheme) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);
    const unsigned schemeKey = scheme + 1u;

    auto collectIniLang = [&](const ParsedIni& ini, const std::filesystem::path& primary) {
        if (const auto it = ini.schemeTokens.find(schemeKey); it != ini.schemeTokens.end()) {
            appendTokensWithFilter(out, seen, it->second, primary, ctx.gameDir, isLangArchiveToken);
        }
        appendTokensWithFilter(out, seen, ini.commonTokens, primary, ctx.gameDir, isLangArchiveToken);
        appendTokensWithFilter(out, seen, ini.menuTokens, primary, ctx.gameDir, isLangArchiveToken);
        if (schemeKey != 1u) {
            if (const auto it1 = ini.schemeTokens.find(1u); it1 != ini.schemeTokens.end()) {
                appendTokensWithFilter(out, seen, it1->second, primary, ctx.gameDir, isLangArchiveToken);
            }
        }
    };

    if (ctx.mod) {
        collectIniLang(ctx.modIni, ctx.mod->rootDir);
        collectDropInModArchives(ctx.mod->rootDir, ctx.gameDir, out, seen, 2, scheme);
    }
    collectIniLang(ctx.baseIni, ctx.gameDir);
    if (scheme > 0u) {
        appendUniqueExistingPath(out, seen, ctx.gameDir / ("lang_desc_scheme" + std::to_string(schemeKey) + ".sue"));
    }
    appendUniqueExistingPath(out, seen, ctx.gameDir / "lang.sue");
    appendUniqueExistingPath(out, seen, ctx.gameDir / "lng.sue");
    return out;
}

std::vector<std::filesystem::path> resolveMenuArchives(const std::filesystem::path& gameDir) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);

    if (ctx.mod) {
        appendTokensWithFilter(out, seen, ctx.modIni.menuTokens, ctx.mod->rootDir, ctx.gameDir, nullptr);
        collectDropInModArchives(ctx.mod->rootDir, ctx.gameDir, out, seen, 3);
    }
    appendTokensWithFilter(out, seen, ctx.baseIni.menuTokens, ctx.gameDir, ctx.gameDir, nullptr);
    appendUniqueExistingPath(out, seen, ctx.gameDir / "lang.sue");
    appendUniqueExistingPath(out, seen, ctx.gameDir / "lng.sue");
    appendUniqueExistingPath(out, seen, ctx.gameDir / "menu_common.sue");
    return out;
}

std::vector<std::filesystem::path> resolveSchemeArchives(const std::filesystem::path& gameDir, unsigned scheme) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);
    const unsigned schemeKey = scheme + 1u;

    if (ctx.mod) {
        if (const auto it = ctx.modIni.schemeTokens.find(schemeKey); it != ctx.modIni.schemeTokens.end()) {
            appendTokensWithFilter(out, seen, it->second, ctx.mod->rootDir, ctx.gameDir, isSchemeTerrainArchiveToken);
        }
        collectDropInModArchives(ctx.mod->rootDir, ctx.gameDir, out, seen, 4, scheme);
    }
    if (const auto it = ctx.baseIni.schemeTokens.find(schemeKey); it != ctx.baseIni.schemeTokens.end()) {
        appendTokensWithFilter(out, seen, it->second, ctx.gameDir, ctx.gameDir, isSchemeTerrainArchiveToken);
    }
    if (scheme == 2u) {
        appendUniqueExistingPath(out, seen, ctx.gameDir / "game_scheme3.sue");
    }
    if (ctx.mod) {
        if (const auto it1 = ctx.modIni.schemeTokens.find(1u); it1 != ctx.modIni.schemeTokens.end()) {
            appendTokensWithFilter(out, seen, it1->second, ctx.mod->rootDir, ctx.gameDir, isSchemeTerrainArchiveToken);
        }
        if (scheme != 0u) {
            collectDropInModArchives(ctx.mod->rootDir, ctx.gameDir, out, seen, 4, 0u);
        }
    }
    if (const auto it1 = ctx.baseIni.schemeTokens.find(1u); it1 != ctx.baseIni.schemeTokens.end()) {
        appendTokensWithFilter(out, seen, it1->second, ctx.gameDir, ctx.gameDir, isSchemeTerrainArchiveToken);
    }
    appendUniqueExistingPath(out, seen, ctx.gameDir / "game_scheme1.sue");
    return out;
}

SoundBankPaths resolveSoundBankFiles(const std::filesystem::path& gameDir) {
    SoundBankPaths out{};
    if (gameDir.empty()) return out;
    const auto ctx = buildModVfsContext(gameDir);
    std::error_code ec;

    auto tryStemPair = [&](const std::filesystem::path& root, std::string_view stem) -> std::optional<SoundBankPaths> {
        if (root.empty() || stem.empty()) return std::nullopt;
        std::string norm;
        for (char ch : stem) norm.push_back(ch == '\\' ? '/' : ch);
        if (norm.back() != '.') norm.push_back('.');
        const auto hdr = resolveCaseInsensitivePath(root, norm + "hdr");
        const auto rus = resolveCaseInsensitivePath(root, norm + "rus");
        if (std::filesystem::is_regular_file(hdr, ec) && std::filesystem::is_regular_file(rus, ec)) {
            return SoundBankPaths{hdr, rus};
        }
        return std::nullopt;
    };

    if (ctx.mod) {
        if (!ctx.modIni.soundsStem.empty()) {
            if (auto p = tryStemPair(ctx.mod->rootDir, ctx.modIni.soundsStem)) return *p;
            if (auto p = tryStemPair(ctx.gameDir, ctx.modIni.soundsStem)) return *p;
        }
        if (auto p = tryStemPair(ctx.mod->rootDir, "SOUNDS.")) return *p;
    }
    if (!ctx.baseIni.soundsStem.empty()) {
        if (auto p = tryStemPair(ctx.gameDir, ctx.baseIni.soundsStem)) return *p;
    }
    if (auto p = tryStemPair(ctx.gameDir, "SOUNDS.")) return *p;
    return out;
}

std::string resolveBackWavEntry(const std::filesystem::path& gameDir) {
    if (gameDir.empty()) return "backwav";
    const auto ctx = buildModVfsContext(gameDir);
    if (!ctx.modIni.backWav.empty()) return ctx.modIni.backWav;
    if (!ctx.baseIni.backWav.empty()) return ctx.baseIni.backWav;
    return "backwav";
}

std::filesystem::path resolveMusicDir(const std::filesystem::path& gameDir) {
    if (gameDir.empty()) return {};
    const auto ctx = buildModVfsContext(gameDir);
    if (ctx.mod) {
        const auto mDir = resolveCaseInsensitivePath(ctx.mod->rootDir, "MUSIC");
        if (directoryHasAnyRegularFile(mDir)) return mDir;
    }
    return resolveCaseInsensitivePath(ctx.gameDir, "MUSIC");
}

std::vector<std::filesystem::path> resolveMovieDirs(const std::filesystem::path& gameDir) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);
    std::error_code ec;
    auto pushDir = [&](const std::filesystem::path& p) {
        const auto res = resolvePathIgnoreCase(p);
        if (std::filesystem::is_directory(res, ec)) appendUniqueExistingPath(out, seen, res);
    };
    if (ctx.mod) {
        for (const auto& t : ctx.modIni.movieTokens) {
            const auto rel = normalizeNativeTarget(t.rawTarget);
            if (!rel.empty()) pushDir(rel.is_absolute() ? rel : (ctx.mod->rootDir / rel));
        }
        pushDir(ctx.mod->rootDir / "movies");
    }
    for (const auto& t : ctx.baseIni.movieTokens) {
        const auto rel = normalizeNativeTarget(t.rawTarget);
        if (!rel.empty()) pushDir(rel.is_absolute() ? rel : (ctx.gameDir / rel));
    }
    pushDir(ctx.gameDir / "movies");
    return out;
}

std::vector<std::filesystem::path> resolveAddonsDirs(const std::filesystem::path& gameDir) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);
    std::error_code ec;
    auto pushDir = [&](const std::filesystem::path& p) {
        const auto res = resolvePathIgnoreCase(p);
        if (std::filesystem::is_directory(res, ec)) appendUniqueExistingPath(out, seen, res);
    };
    if (ctx.mod) {
        for (const auto& t : ctx.modIni.addonsTokens) {
            const auto rel = normalizeNativeTarget(t.rawTarget);
            if (!rel.empty()) pushDir(rel.is_absolute() ? rel : (ctx.mod->rootDir / rel));
        }
        pushDir(ctx.mod->rootDir / "addons");
    }
    for (const auto& t : ctx.baseIni.addonsTokens) {
        const auto rel = normalizeNativeTarget(t.rawTarget);
        if (!rel.empty()) pushDir(rel.is_absolute() ? rel : (ctx.gameDir / rel));
    }
    pushDir(ctx.gameDir / "addons");
    return out;
}

std::vector<std::filesystem::path> resolveSingleMapDirs(const std::filesystem::path& gameDir) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);
    if (ctx.mod) {
        appendMapDirTokens(out, seen, ctx.modIni.commonTokens, ctx.modIni.singlePrefix, ctx.mod->rootDir, ctx.gameDir);
        appendUniqueExistingPath(out, seen, ctx.mod->rootDir / "maps" / "single");
    }
    appendMapDirTokens(out, seen, ctx.baseIni.commonTokens, ctx.baseIni.singlePrefix, ctx.gameDir, ctx.gameDir);
    appendUniqueExistingPath(out, seen, ctx.gameDir / "maps" / "single");
    return out;
}

std::vector<std::filesystem::path> resolveMultiMapDirs(const std::filesystem::path& gameDir) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);
    if (ctx.mod) {
        appendMapDirTokens(out, seen, ctx.modIni.commonTokens, ctx.modIni.multiPrefix, ctx.mod->rootDir, ctx.gameDir);
        if (const auto mp = normalizeNativeTarget(ctx.modIni.multiPrefix); !mp.empty()) {
            appendUniqueExistingPath(out, seen, mp.is_absolute() ? mp : (ctx.mod->rootDir / mp));
        }
        appendUniqueExistingPath(out, seen, ctx.mod->rootDir / "maps" / "mplay");
    }
    appendMapDirTokens(out, seen, ctx.baseIni.commonTokens, ctx.baseIni.multiPrefix, ctx.gameDir, ctx.gameDir);
    if (const auto mp = normalizeNativeTarget(ctx.baseIni.multiPrefix); !mp.empty()) {
        appendUniqueExistingPath(out, seen, mp.is_absolute() ? mp : (ctx.gameDir / mp));
    }
    appendUniqueExistingPath(out, seen, ctx.gameDir / "maps" / "mplay");
    return out;
}

std::vector<std::filesystem::path> resolveCampaignMapDirs(const std::filesystem::path& gameDir) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);
    if (ctx.mod) {
        appendMapDirTokens(out, seen, ctx.modIni.commonTokens, ctx.modIni.campaignsPrefix, ctx.mod->rootDir, ctx.gameDir);
        appendUniqueExistingPath(out, seen, ctx.mod->rootDir / "maps" / "campaign");
    }
    appendMapDirTokens(out, seen, ctx.baseIni.commonTokens, ctx.baseIni.campaignsPrefix, ctx.gameDir, ctx.gameDir);
    appendUniqueExistingPath(out, seen, ctx.gameDir / "maps" / "campaign");
    return out;
}

std::vector<std::filesystem::path> resolveCustomCampaignDirs(const std::filesystem::path& gameDir) {
    std::vector<std::filesystem::path> out;
    if (gameDir.empty()) return out;
    std::vector<std::filesystem::path> seen;
    const auto ctx = buildModVfsContext(gameDir);
    if (ctx.mod) {
        appendUniqueExistingPath(out, seen, ctx.mod->rootDir / "maps" / "custom_campaign");
    }
    appendUniqueExistingPath(out, seen, ctx.gameDir / "maps" / "custom_campaign");
    return out;
}

std::optional<unsigned> resolveIniTurnDelay(const std::filesystem::path& gameDir) {
    if (gameDir.empty()) return std::nullopt;
    const auto ctx = buildModVfsContext(gameDir);
    const std::optional<std::string>& raw = ctx.modIni.turnDelayRaw.has_value()
        ? ctx.modIni.turnDelayRaw : ctx.baseIni.turnDelayRaw;
    if (!raw.has_value()) return std::nullopt;
    unsigned value{};
    const std::string_view sv = trimAsciiSv(*raw);
    const auto [ptr, err] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (err != std::errc{} || ptr != sv.data() + sv.size() || !value) {
        throw std::runtime_error("Invalid Gold Game/TurnDelay");
    }
    return value;
}

std::string schemeDisplayName(unsigned scheme) {
    switch (scheme) {
        case 0u: return "Summer";
        case 1u: return "Winter";
        case 2u: return "Beach";
        case 3u: return "Desert";
        default: return "Scheme " + std::to_string(scheme + 1u);
    }
}

namespace {
SpriteFrame decodePckFrameImpl(View file, size_t index, bool allowTruncatedRows) {
    const size_t first = u32(file, 0);
    if (!first || first % 4 || first > file.size() || index >= first / 4)
        throw std::runtime_error("Invalid PCK frame table or index");
    const size_t start = u32(file, index * 4);
    const size_t end = index + 1 < first / 4 ? u32(file, (index + 1) * 4) : file.size();
    if (start < first || end < start) throw std::runtime_error("Invalid PCK frame range");
    const auto frame = slice(file, start, end - start);
    const auto header = slice(frame, 0, 9);
    SpriteFrame image{std::int16_t(u16(header, 0)), std::int16_t(u16(header, 2)),
                      std::int16_t(u16(header, 4)), std::int16_t(u16(header, 6)), {}};
    if (header[8] != 0xa5) throw std::runtime_error("Unsupported PCK frame tag");
    if (image.width <= 0 || image.height <= 0 || 2ull * image.width * image.height > assetLimit)
        throw std::runtime_error("Invalid PCK dimensions");
    image.pixels.reserve(2ull * image.width * image.height);
    size_t pos = 9;
    for (int y = 0; y < image.height; ++y) {
        const auto row = slice(frame, pos + 2, u16(frame, pos));
        pos += 2 + row.size();
        int remaining = image.width;
        for (size_t i = 0; i < row.size();) {
            const auto opcode = row[i++];
            const int count = opcode & 127;
            if (!count || count > remaining) throw std::runtime_error("Invalid PCK run width");
            const bool repeat = (opcode & 128) != 0;
            const auto words = slice(row, i, repeat ? 2 : 2 * count);
            i += words.size();
            if (repeat) {
                for (int j = 0; j < count; ++j) image.pixels.insert(image.pixels.end(), words.begin(), words.end());
            } else image.pixels.insert(image.pixels.end(), words.begin(), words.end());
            remaining -= count;
        }
        if (remaining) {
            if (!allowTruncatedRows) throw std::runtime_error("Incomplete PCK row");
            // UI PCKs omit trailing 0xF81F transparent key words at the right edge of rows.
            for (int j = 0; j < remaining; ++j) {
                image.pixels.push_back(0x1f);
                image.pixels.push_back(0xf8);
            }
        }
    }
    return image;
}
}

SpriteFrame decodePckFrame(View file, size_t index) {
    return decodePckFrameImpl(file, index, false);
}

std::size_t pckFrameCount(View file) {
    if (file.size() < 4) return 0;
    const size_t first = u32(file, 0);
    if (!first || first % 4 || first > file.size()) return 0;
    return first / 4;
}

SpriteFrame decodeUiPckFrame(View file, size_t index) {
    return decodePckFrameImpl(file, index, true);
}
}
