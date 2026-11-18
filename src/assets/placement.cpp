#include "assets/placement.hpp"
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace ss2::assets {
std::vector<Placement> readTutorialPlacements(const std::filesystem::path& gameDir) {
    const auto path = gameDir / "maps.src/map.020/mis.000/mapunits";
    if (std::filesystem::file_size(path) > (1u << 20)) throw std::runtime_error("Unit list exceeds size limit");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read Tutorial I unit list");
    std::vector<Placement> units;
    for (std::string line; std::getline(input, line);) {
        std::istringstream stream(line);
        std::map<std::string, std::string> fields;
        for (std::string token; stream >> token;) {
            const auto pos = token.find('=');
            if (pos != std::string::npos) fields.emplace(token.substr(0, pos), token.substr(pos + 1));
        }
        // Nested passengers have no independent map coordinates.
        if (!fields.contains("U") && !fields.contains("V")) continue;
        const auto number = [&](const char* key) {
            const auto it = fields.find(key);
            if (it == fields.end()) throw std::runtime_error("Missing unit field " + std::string(key));
            const auto& field = it->second;
            int value{};
            const auto [end, err] = std::from_chars(field.data(), field.data() + field.size(), value);
            if (err != std::errc{} || end != field.data() + field.size())
                throw std::runtime_error("Invalid unit field " + std::string(key));
            return value;
        };
        const int u = number("U"), v = number("V"), dir = number("Dir");
        if (u < 0 || v < 0 || u >= 4096 || v >= 4096 || dir < 0 || dir >= 32 || !fields.contains("ID"))
            throw std::runtime_error("Invalid Tutorial I unit placement");
        units.push_back({fields.at("ID"), 4096 + u - v, (u + v) / 2, dir, number("HP"),
                         number("Ammo"), number("Grp"), number("Owner")});
    }
    return units;
}
}
