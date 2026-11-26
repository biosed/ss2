#include "sim/command.hpp"
#include <array>
#include <charconv>
#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ss2::cmd {
namespace {
// Serialized field order. It must list every data member in declaration
// order, because parsing rebuilds the aggregate from these values.
auto fields(const assets::Infantry& u) {
    return std::tie(u.id, u.x, u.y, u.hp, u.ammo, u.group, u.owner, u.left, u.top, u.width, u.height, u.direction,
                    u.progress, u.walking, u.visible, u.expa);
}
auto fields(const assets::VehicleUnit& u) {
    return std::tie(u.id, u.x, u.y, u.hp, u.ammo, u.group, u.owner, u.hullDir, u.turretDir, u.left, u.top, u.width,
                    u.height, u.alive, u.visible, u.hasCrew, u.expa);
}
auto fields(const TutorialUnits::ReserveUnit& r) {
    return std::tie(r.id, r.owner, r.group, r.hp, r.ammo, r.expa, r.pool);
}
auto fields(const assets::PassengerPlacement& p) {
    return std::tie(p.id, p.group, p.hp, p.ammo, p.expa, p.isCrew);
}
auto fields(const assets::ReinforcementUnit& u) {
    return std::tie(u.id, u.group, u.hp, u.ammo, u.expa, u.passengers);
}
auto fields(const TutorialUnits::Snapshot::SoldierState& s) {
    return std::tie(s.cellX, s.cellY, s.hp, s.maxHp, s.ammo, s.expa, s.kills, s.atGrenades, s.mineAmmo, s.owner,
                    s.group, s.direction, s.crouched, s.holdFire, s.binoculars, s.embarked, s.garrisonBuilding,
                    s.suppressionTicks);
}
auto fields(const TutorialUnits::Snapshot::VehicleState& v) {
    return std::tie(v.x, v.y, v.hullDir, v.turretDir, v.hp, v.maxHp, v.ammo, v.ammo2, v.expa, v.kills, v.owner,
                    v.group, v.alive, v.holdFire, v.passengers, v.towedVehicle, v.towedBy, v.hasCrew, v.crewHp);
}
auto fields(const TutorialUnits::Snapshot::BuildingState& b) {
    return std::tie(b.hp, b.owner, b.destroyed, b.occupants);
}
auto fields(const TutorialUnits::Snapshot& s) {
    return std::tie(s.tick, s.playerTeams, s.mines, s.fog, s.soldiers, s.vehicles, s.buildings, s.airReady);
}

auto fields(const Move& c) { return std::tie(c.ids, c.x, c.y, c.append); }
auto fields(const AttackMove& c) { return std::tie(c.ids, c.x, c.y, c.append); }
auto fields(const Patrol& c) { return std::tie(c.ids, c.x, c.y); }
auto fields(const Scatter& c) { return std::tie(c.ids); }
auto fields(const Attack& c) { return std::tie(c.ids, c.target); }
auto fields(const Stop& c) { return std::tie(c.ids); }
auto fields(const ToggleHoldFire& c) { return std::tie(c.ids); }
auto fields(const ToggleCrouch& c) { return std::tie(c.ids); }
auto fields(const ToggleBinoculars& c) { return std::tie(c.ids); }
auto fields(const SpawnInfantry& c) { return std::tie(c.unit, c.expa); }
auto fields(const MoveVehicles& c) { return std::tie(c.ids, c.x, c.y, c.append); }
auto fields(const ReverseMoveVehicles& c) { return std::tie(c.ids, c.x, c.y); }
auto fields(const FaceVehicles& c) { return std::tie(c.ids, c.x, c.y); }
auto fields(const AttackMoveVehicles& c) { return std::tie(c.ids, c.x, c.y, c.append); }
auto fields(const PatrolVehicles& c) { return std::tie(c.ids, c.x, c.y); }
auto fields(const AttackWithVehicles& c) { return std::tie(c.ids, c.targetIsVehicle, c.target); }
auto fields(const AttackVehicle& c) { return std::tie(c.ids, c.targetVehicle); }
auto fields(const StopVehicles& c) { return std::tie(c.ids); }
auto fields(const SupportVehicle& c) { return std::tie(c.supporters, c.targetVehicle); }
auto fields(const SupportBuilding& c) { return std::tie(c.supporters, c.building); }
auto fields(const HealSoldier& c) { return std::tie(c.medics, c.ambulances, c.targetSoldier); }
auto fields(const EscortVehicle& c) { return std::tie(c.soldiers, c.vehicles, c.targetVehicle); }
auto fields(const EscortSoldier& c) { return std::tie(c.soldiers, c.vehicles, c.targetSoldier); }
auto fields(const ToggleVehicleHoldFire& c) { return std::tie(c.ids); }
auto fields(const ToggleMarchMode& c) { return std::tie(c.vehicles); }
auto fields(const BoardVehicle& c) { return std::tie(c.soldiers, c.vehicle); }
auto fields(const EmbarkImmediate& c) { return std::tie(c.soldier, c.vehicle); }
auto fields(const UnloadVehicles& c) { return std::tie(c.vehicles); }
auto fields(const CaptureVehicle& c) { return std::tie(c.soldiers, c.vehicle); }
auto fields(const AbandonVehicles& c) { return std::tie(c.vehicles); }
auto fields(const TowVehicle& c) { return std::tie(c.tractor, c.gun); }
auto fields(const UnhitchVehicles& c) { return std::tie(c.vehicles); }
auto fields(const SpawnVehicle& c) { return std::tie(c.unit); }
auto fields(const AddReserveUnit& c) { return std::tie(c.entry); }
auto fields(const TakeReserveUnits& c) { return std::tie(c.group, c.pool, c.maxCount); }
auto fields(const AttackGroundVehicles& c) { return std::tie(c.ids, c.x, c.y); }
auto fields(const AttackGroundSoldiers& c) { return std::tie(c.ids, c.x, c.y); }
auto fields(const LayMine& c) { return std::tie(c.soldiers); }
auto fields(const DisarmMine& c) { return std::tie(c.soldiers, c.x, c.y); }
auto fields(const GroupMove& c) { return std::tie(c.group, c.x, c.y, c.onlyNonPlayer); }
auto fields(const GarrisonBuilding& c) { return std::tie(c.soldiers, c.building); }
auto fields(const EvacuateBuilding& c) { return std::tie(c.building); }
auto fields(const ToggleBuildingHoldFire& c) { return std::tie(c.building); }
auto fields(const ToggleBuildingBinoculars& c) { return std::tie(c.building); }
auto fields(const BuildingAttack& c) { return std::tie(c.building, c.targetIsVehicle, c.target); }
auto fields(const BuildingAttackBuilding& c) { return std::tie(c.building, c.targetBuilding); }
auto fields(const AttackBuilding& c) { return std::tie(c.soldiers, c.building); }
auto fields(const AttackBuildingWithVehicles& c) { return std::tie(c.vehicles, c.building); }
auto fields(const CallAirSupport& c) { return std::tie(c.kind, c.x, c.y); }
auto fields(const LoadSnapshot& c) { return std::tie(c.snapshot); }
auto fields(const Acknowledge&) { return std::tuple<>{}; }
auto fields(const TogglePause&) { return std::tuple<>{}; }
auto fields(const Resign& c) { return std::tie(c.seat); }
auto fields(const SupportBridge& c) { return std::tie(c.supporters, c.bridge); }
auto fields(const UnloadSingleSoldier& c) { return std::tie(c.vehicle, c.soldier); }
auto fields(const EvacuateSingleSoldier& c) { return std::tie(c.building, c.soldier); }

template<class T> struct IsVector : std::false_type {};
template<class T> struct IsVector<std::vector<T>> : std::true_type {};
template<class T> struct IsArray : std::false_type {};
template<class T, std::size_t N> struct IsArray<std::array<T, N>> : std::true_type {};

template<class Tuple> struct DecayTuple;
template<class... Ts> struct DecayTuple<std::tuple<Ts...>> {
    using type = std::tuple<std::remove_cvref_t<Ts>...>;
};
template<class T> using FieldValues = typename DecayTuple<decltype(fields(std::declval<const T&>()))>::type;

template<std::size_t... I>
consteval bool tagsAreUniqueAndWellFormed(std::index_sequence<I...>) {
    const std::array<std::string_view, sizeof...(I)> tags{std::variant_alternative_t<I, Command>::tag...};
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].empty() || tags[i].find(' ') != std::string_view::npos) return false;
        for (std::size_t j = i + 1; j < tags.size(); ++j) {
            if (tags[i] == tags[j]) return false;
        }
    }
    return true;
}
static_assert(tagsAreUniqueAndWellFormed(std::make_index_sequence<std::variant_size_v<Command>>{}),
              "command tags must be unique, non-empty and free of spaces");

class Writer {
public:
    void token(std::string_view t) {
        if (!out_.empty()) out_ += ' ';
        out_ += t;
    }
    template<class T> void put(const T& v) {
        if constexpr (std::same_as<T, bool>) {
            token(v ? "1" : "0");
        } else if constexpr (std::is_enum_v<T>) {
            put(static_cast<std::underlying_type_t<T>>(v));
        } else if constexpr (std::is_integral_v<T>) {
            std::array<char, 24> buf{};
            const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
            token(std::string_view(buf.data(), static_cast<std::size_t>(res.ptr - buf.data())));
        } else if constexpr (std::same_as<T, std::string>) {
            putString(v);
        } else if constexpr (IsVector<T>::value) {
            put(v.size());
            for (const auto& e : v) put(e);
        } else if constexpr (IsArray<T>::value) {
            for (const auto& e : v) put(e);
        } else {
            std::apply([this](const auto&... f) {
                (void)this;
                (put(f), ...);
            }, fields(v));
        }
    }
    std::string take() { return std::move(out_); }
private:
    void putString(std::string_view s) {
        static constexpr std::string_view hex = "0123456789ABCDEF";
        std::string t = "'";
        for (const char ch : s) {
            const auto b = static_cast<unsigned char>(ch);
            if (b > 0x20u && b < 0x7fu && b != '%') {
                t += ch;
            } else {
                t += '%';
                t += hex[b >> 4u];
                t += hex[b & 0xfu];
            }
        }
        token(t);
    }
    std::string out_;
};

class Reader {
public:
    explicit Reader(std::string_view line) {
        for (;;) {
            const auto end = line.find(' ');
            tokens_.push_back(line.substr(0, end));
            if (end == std::string_view::npos) break;
            line.remove_prefix(end + 1);
        }
    }
    std::optional<std::string_view> token() {
        if (pos_ == tokens_.size()) return std::nullopt;
        return tokens_[pos_++];
    }
    bool atEnd() const { return pos_ == tokens_.size(); }

    template<class T> bool get(T& v) {
        if constexpr (std::same_as<T, bool>) {
            const auto t = token();
            if (!t || (*t != "0" && *t != "1")) return false;
            v = *t == "1";
            return true;
        } else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{};
            if (!get(raw)) return false;
            v = static_cast<T>(raw);
            return true;
        } else if constexpr (std::is_integral_v<T>) {
            const auto t = token();
            if (!t || t->empty()) return false;
            const auto res = std::from_chars(t->data(), t->data() + t->size(), v);
            return res.ec == std::errc{} && res.ptr == t->data() + t->size();
        } else if constexpr (std::same_as<T, std::string>) {
            const auto t = token();
            return t && getString(*t, v);
        } else if constexpr (IsVector<T>::value) {
            std::size_t n = 0;
            // Every element takes at least one token, which bounds bogus counts.
            if (!get(n) || n > tokens_.size() - pos_) return false;
            v.clear();
            v.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                if (!get(v.emplace_back())) return false;
            }
            return true;
        } else if constexpr (IsArray<T>::value) {
            for (auto& e : v) {
                if (!get(e)) return false;
            }
            return true;
        } else {
            FieldValues<T> values;
            const bool ok = std::apply([this](auto&... f) {
                (void)this;
                return (get(f) && ...);
            }, values);
            if (ok) v = std::make_from_tuple<T>(std::move(values));
            return ok;
        }
    }
private:
    static bool getString(std::string_view t, std::string& out) {
        if (t.empty() || t.front() != '\'') return false;
        t.remove_prefix(1);
        out.clear();
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] != '%') {
                out += t[i];
                continue;
            }
            unsigned b = 0;
            if (t.size() - i < 3) return false;
            const auto res = std::from_chars(t.data() + i + 1, t.data() + i + 3, b, 16);
            if (res.ec != std::errc{} || res.ptr != t.data() + i + 3) return false;
            out += static_cast<char>(b);
            i += 2;
        }
        return true;
    }
    std::vector<std::string_view> tokens_;
    std::size_t pos_{0};
};

template<class C> std::optional<Command> readAs(Reader& r) {
    C c{};
    if (!r.get(c)) return std::nullopt;
    return Command{std::move(c)};
}

template<std::size_t... I>
std::optional<Command> readCommand(std::string_view tag, Reader& r, std::index_sequence<I...>) {
    std::optional<Command> out;
    (void)((tag == std::variant_alternative_t<I, Command>::tag &&
            (out = readAs<std::variant_alternative_t<I, Command>>(r), true)) || ...);
    return out;
}
}

std::string_view tagOf(const Command& c) {
    return std::visit([](const auto& alt) { return std::remove_cvref_t<decltype(alt)>::tag; }, c);
}

std::string toLine(const RecordedCommand& rc) {
    Writer w;
    w.token(tagOf(rc.command));
    w.put(rc.tick);
    std::visit([&w](const auto& alt) { w.put(alt); }, rc.command);
    return w.take();
}

std::optional<RecordedCommand> fromLine(std::string_view line) {
    Reader r(line);
    const auto tag = r.token();
    if (!tag) return std::nullopt;
    RecordedCommand rc;
    if (!r.get(rc.tick)) return std::nullopt;
    auto command = readCommand(*tag, r, std::make_index_sequence<std::variant_size_v<Command>>{});
    if (!command || !r.atEnd()) return std::nullopt;
    rc.command = std::move(*command);
    return rc;
}

std::string toText(std::span<const RecordedCommand> log) {
    std::string out;
    for (const auto& rc : log) {
        out += toLine(rc);
        out += '\n';
    }
    return out;
}

std::optional<std::vector<RecordedCommand>> fromText(std::string_view text) {
    std::vector<RecordedCommand> log;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        auto rc = fromLine(line);
        if (!rc) return std::nullopt;
        log.push_back(std::move(*rc));
    }
    return log;
}

namespace {
std::string formatHex64(std::uint64_t v) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string s = "0x";
    s.reserve(18);
    for (int shift = 60; shift >= 0; shift -= 4) {
        s += hex[(v >> shift) & 0xfu];
    }
    return s;
}

std::string_view trimAsciiSpace(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
} // namespace

std::string toReplayFileText(const ReplayFile& file) {
    std::string out = "# ss2-replay v1\n";
    if (!file.mission.empty()) {
        std::string sanitized;
        sanitized.reserve(file.mission.size());
        for (const char c : file.mission) {
            if (c != '\r' && c != '\n') sanitized += c;
        }
        out += "# mission: " + sanitized + '\n';
    }
    if (file.difficulty) out += "# difficulty: " + std::to_string(*file.difficulty) + '\n';
    if (file.autoEngage) out += std::string("# auto_engage: ") + (*file.autoEngage ? "1\n" : "0\n");
    out += "# end_tick: " + std::to_string(file.endTick) + '\n';
    if (file.endStateHash) out += "# end_state_hash: " + formatHex64(*file.endStateHash) + '\n';
    for (const auto& [poolIdx, wave] : file.initialSavedPools) {
        if (wave.empty()) continue;
        Writer w;
        w.put(poolIdx);
        w.put(wave);
        out += "# saved_pool: " + w.take() + '\n';
    }
    out += toText(file.commands);
    return out;
}

std::optional<ReplayFile> fromReplayFileText(std::string_view text) {
    ReplayFile out;
    bool sawEndTick = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (line.front() == '#') {
            auto meta = trimAsciiSpace(line.substr(1));
            if (meta.starts_with("mission:")) {
                out.mission = std::string(trimAsciiSpace(meta.substr(8)));
            } else if (meta.starts_with("difficulty:")) {
                const auto val = trimAsciiSpace(meta.substr(11));
                int d = 0;
                const auto [p, ec] = std::from_chars(val.data(), val.data() + val.size(), d);
                if (ec != std::errc{} || p != val.data() + val.size() || d < 1 || d > 3) return std::nullopt;
                out.difficulty = d;
            } else if (meta.starts_with("auto_engage:")) {
                const auto val = trimAsciiSpace(meta.substr(12));
                if (val == "1") out.autoEngage = true;
                else if (val == "0") out.autoEngage = false;
                else return std::nullopt;
            } else if (meta.starts_with("end_tick:")) {
                const auto val = trimAsciiSpace(meta.substr(9));
                std::uint32_t t = 0;
                const auto [p, ec] = std::from_chars(val.data(), val.data() + val.size(), t);
                if (ec != std::errc{} || p != val.data() + val.size()) return std::nullopt;
                out.endTick = t;
                sawEndTick = true;
            } else if (meta.starts_with("end_state_hash:")) {
                auto val = trimAsciiSpace(meta.substr(15));
                if (val.starts_with("0x") || val.starts_with("0X")) val.remove_prefix(2);
                if (val.empty()) return std::nullopt;
                std::uint64_t h = 0;
                const auto [p, ec] = std::from_chars(val.data(), val.data() + val.size(), h, 16);
                if (ec != std::errc{} || p != val.data() + val.size()) return std::nullopt;
                out.endStateHash = h;
            } else if (meta.starts_with("saved_pool:")) {
                const auto val = trimAsciiSpace(meta.substr(11));
                if (val.empty()) return std::nullopt;
                Reader r(val);
                unsigned poolIdx = 0;
                std::vector<assets::ReinforcementUnit> wave;
                if (!r.get(poolIdx) || !r.get(wave) || !r.atEnd() || wave.empty() ||
                    out.initialSavedPools.contains(poolIdx)) {
                    return std::nullopt;
                }
                out.initialSavedPools.emplace(poolIdx, std::move(wave));
            }
            continue;
        }
        auto rc = fromLine(line);
        if (!rc) return std::nullopt;
        out.commands.push_back(std::move(*rc));
    }
    for (std::size_t i = 1; i < out.commands.size(); ++i) {
        if (out.commands[i].tick < out.commands[i - 1].tick) return std::nullopt;
    }
    if (!sawEndTick && !out.commands.empty()) {
        out.endTick = out.commands.back().tick;
    }
    if (!out.commands.empty() && out.endTick < out.commands.back().tick) return std::nullopt;
    return out;
}

std::string_view ReplayTransport::speedLabel() const noexcept {
    switch (std::clamp(speedIndex, 0, 4)) {
    case 0: return "0.5x";
    case 1: return "1x";
    case 2: return "2x";
    case 3: return "4x";
    default: return "8x";
    }
}

void ReplayTransport::cycleViewerSeat(int maxSeats) noexcept {
    const int clampedMax = std::clamp(maxSeats, 1, 4);
    if (viewerSeat < 0) {
        viewerSeat = 0;
    } else if (viewerSeat + 1 < clampedMax) {
        ++viewerSeat;
    } else {
        viewerSeat = -1;
    }
}

unsigned ReplayTransport::consumeTicks(double elapsedSeconds, double baseTickPeriodSec) noexcept {
    if (paused || finished || elapsedSeconds <= 0.0 || baseTickPeriodSec <= 0.0) return 0u;
    const double scaled = elapsedSeconds * (static_cast<double>(speedPercent()) / 100.0);
    tickAccumulator = std::min(tickAccumulator + scaled / baseTickPeriodSec, 64.0);
    const unsigned n = static_cast<unsigned>(tickAccumulator);
    tickAccumulator -= static_cast<double>(n);
    return n;
}

std::string ReplayTransport::formatBanner(std::uint32_t currentTick, std::uint32_t endTick) const {
    std::string out = "REPLAY ";
    if (finished) {
        out += "END";
        if (hashVerified) out += " OK";
        else if (hashMismatch) out += " HASH!";
    } else if (paused) {
        out += "PAUSED";
    } else {
        out += speedLabel();
    }
    out += " | ";
    out += std::to_string(std::min(currentTick, endTick)) + "/" + std::to_string(endTick) + "t | ";
    if (viewerSeat < 0) {
        out += "All Vision";
    } else {
        out += "P" + std::to_string(viewerSeat + 1);
    }
    return out;
}
}
