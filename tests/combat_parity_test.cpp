// Tick-level parity test for the SS2 matched-fields encounter.
//
// Replays the encounter (dguner -> Group-3 aguner, difficulty 2, rng seed
// 0x12345678) through the native CombatWorld and compares EVERY tick snapshot
// and EVERY discrete event against the expected table in
// tests/combat_parity_golden.txt.
//
// The expected table comes from the Gold 2.2 oracle fixture
// (matched_fields_encounter.json) and holds simulation state only - no retail
// asset bytes and no game data - so this test runs in CI without the private
// Gold installation.
//
// tests/infantry_combat_test.cpp asserts about ten checkpoint ticks. This one
// asserts all 120 ticks and all 21 events, so a drift in an intermediate tick
// (reload countdown, burst counter, fire animation, expa between checkpoints)
// fails the build instead of hiding between two checkpoints.
//
// The file also carries a REGRESSION section: native-vs-native expectations for
// the two-sided duel and the march-pause engage. Those two paths have no Gold
// 2.2 oracle fixture, so the section is explicitly NOT an oracle parity claim -
// it is a native baseline that catches drift in return fire, in the march-pause
// gating and in the combat timing.
//
// Usage: combat_parity_test <path to combat_parity_golden.txt>
//        combat_parity_test --emit      prints the REGRESSION rows

#include "assets/infantry.hpp"
#include "sim/infantry_combat.hpp"
#include "sim/infantry_path.hpp"
#include "sim/tutorial_units.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace ss2;

constexpr int kDgCtrlU = 1328, kDgCtrlV = 2128; // tile (41, 66), dy=13
constexpr int kDgSpotU = 1328, kDgSpotV = 2096; // tile (41, 65), dy=12
constexpr int kAgU = 1336, kAgV = 1720;         // tile (41, 53)

// Window sizes for the native-vs-native regression scenarios. They are fixed so
// that a change in timing shows up as a difference inside the window instead of
// as a different number of rows.
constexpr int kDuelTicks = 250;
constexpr int kMarchTicks = 300;

struct GoldenTick {
    int tick{};
    int orderId{};
    int orderQ{};
    int dir{};
    int act{};
    int anim{};
    int reload{};
    int burst{};
    int ammo{};
    int expa{};
    int tgtSpotted{};
    int tgtHp{};
    std::uint32_t rng{};
};

struct GoldenEvent {
    int tick{};
    std::string kind;
    std::vector<long long> values;
};

// One native-vs-native expectation: a scenario, a tick and a set of named
// values. Named fields keep the table readable and let each scenario carry
// exactly the state it wants to pin down.
struct RegressionRow {
    std::string scenario;
    int tick{};
    std::vector<std::pair<std::string, long long>> fields;
};

// Collects failures so the test can report every divergence, not just the
// first, and still returns a non-zero exit code in a build where assert()
// would be compiled out.
class Report {
public:
    void fail(const std::string& what) {
        ++count_;
        if (count_ == 1) first_ = what;
        if (count_ <= 20) std::cerr << "  - " << what << "\n";
    }
    bool ok() const { return count_ == 0; }
    int count() const { return count_; }
    const std::string& first() const { return first_; }

private:
    int count_{};
    std::string first_;
};

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string token;
    while (in >> token) out.push_back(token);
    return out;
}

struct GoldenTable {
    std::vector<GoldenTick> ticks;
    std::vector<GoldenEvent> events;
    std::vector<RegressionRow> regression;
};

enum class Section { Trace, Events, Regression };

bool loadGolden(const std::string& path, GoldenTable& table, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open golden table: " + path;
        return false;
    }

    Section section = Section::Trace;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.rfind("#", 0) == 0) {
            if (line.find("Section EVENTS") != std::string::npos) section = Section::Events;
            else if (line.find("Section REGRESSION") != std::string::npos) section = Section::Regression;
            continue;
        }

        const auto tokens = split(line);
        if (tokens.empty()) continue;

        if (section == Section::Trace) {
            if (tokens.size() != 13) {
                error = "golden trace row must have 13 fields, got " +
                        std::to_string(tokens.size()) + ": " + line;
                return false;
            }
            GoldenTick tick{};
            tick.tick = std::stoi(tokens[0]);
            tick.orderId = std::stoi(tokens[1]);
            tick.orderQ = std::stoi(tokens[2]);
            tick.dir = std::stoi(tokens[3]);
            tick.act = std::stoi(tokens[4]);
            tick.anim = std::stoi(tokens[5]);
            tick.reload = std::stoi(tokens[6]);
            tick.burst = std::stoi(tokens[7]);
            tick.ammo = std::stoi(tokens[8]);
            tick.expa = std::stoi(tokens[9]);
            tick.tgtSpotted = std::stoi(tokens[10]);
            tick.tgtHp = std::stoi(tokens[11]);
            tick.rng = static_cast<std::uint32_t>(std::stoul(tokens[12]));
            table.ticks.push_back(tick);
        } else if (section == Section::Events) {
            if (tokens.size() < 2) {
                error = "golden event row needs at least a tick and a kind: " + line;
                return false;
            }
            GoldenEvent event{};
            event.tick = std::stoi(tokens[0]);
            event.kind = tokens[1];
            for (std::size_t i = 2; i < tokens.size(); ++i) {
                event.values.push_back(std::stoll(tokens[i]));
            }
            table.events.push_back(event);
        } else {
            if (tokens.size() < 2) {
                error = "golden regression row needs a scenario and a tick: " + line;
                return false;
            }
            RegressionRow row{};
            row.scenario = tokens[0];
            row.tick = std::stoi(tokens[1]);
            for (std::size_t i = 2; i < tokens.size(); ++i) {
                const auto& token = tokens[i];
                const auto eq = token.find('=');
                if (eq == std::string::npos) {
                    error = "golden regression field must be key=value: " + token;
                    return false;
                }
                row.fields.emplace_back(token.substr(0, eq), std::stoll(token.substr(eq + 1)));
            }
            table.regression.push_back(row);
        }
    }

    if (table.ticks.empty()) {
        error = "golden table has no trace rows: " + path;
        return false;
    }
    if (table.events.empty()) {
        error = "golden table has no event rows: " + path;
        return false;
    }
    return true;
}

// Maps one native event onto the same semantic fields the golden table stores,
// so the comparison is against the oracle meaning and not against the native
// field packing.
bool nativeEventValues(const CombatEvent& e, std::vector<long long>& out) {
    out.clear();
    if (e.kind == "ORDER_ENQUEUE" || e.kind == "ORDER_ACCEPT") {
        out = {e.a};
    } else if (e.kind == "FIRE_START") {
        out = {static_cast<long long>(e.rng)};
    } else if (e.kind == "PROJ_LAUNCH") {
        out = {e.a, e.b, e.c, e.d, static_cast<long long>(e.rng)};
    } else if (e.kind == "PROJ_IMPACT") {
        out = {e.a, e.b, e.c, e.d};
    } else if (e.kind == "HIT_ROLL") {
        out = {e.a, e.b, e.c};
    } else if (e.kind == "HP_APPLY") {
        out = {e.a, e.b, e.c, e.d};
    } else {
        return false;
    }
    return true;
}

const char* kValueNames[] = {"v1", "v2", "v3", "v4", "v5"};

// Replays the encounter on an open fog grid. The asset-free grid reproduces the
// real Tutorial I P-FLAGS + circ64.dat trace bit for bit, so the test needs no
// private game data.
std::vector<CombatTickSnapshot> replayEncounter() {
    FogGrid fog(128, 128, std::vector<std::uint32_t>(128 * 128, 0));
    CombatWorld world(std::move(fog), 2, 0x12345678u);
    world.addUnit(CombatUnit{1, 0, kDgCtrlU, kDgCtrlV, 100, 40, 0, false, InfantryCombatDesc::dguner()});
    world.addUnit(CombatUnit{2, 1, kAgU, kAgV, 100, 64, 80, false, InfantryCombatDesc::aguner()});

    world.refreshSpotting();
    world.setUnitPosition(1, kDgSpotU, kDgSpotV);
    world.refreshSpotting();
    world.issueAttackOrder(1, 2);

    std::vector<CombatTickSnapshot> trace;
    trace.push_back(world.snapshot(-1, 1, 2));
    for (int t = 0; t < 200; ++t) {
        world.step(1);
        const auto snap = world.snapshot(t, 1, 2);
        trace.push_back(snap);
        if (snap.targetHp <= 0) break;
    }
    return trace;
}

// ---------------------------------------------------------------------------
// Native-vs-native regression scenarios.
//
// These have NO Gold 2.2 oracle fixture, so they are not an oracle parity
// claim: they are a native baseline that pins down the two combat paths the
// oracle table does not reach - two-sided return fire and the march-pause
// engage - so that a change in them fails CI instead of passing unnoticed.
// ---------------------------------------------------------------------------

assets::Infantry makeInfantry(const std::string& id, int u, int v, int hp, int ammo, int group, int owner) {
    assets::Infantry unit{};
    unit.id = id;
    unit.x = 4096 + u - v;
    unit.y = (u + v) / 2;
    unit.hp = hp;
    unit.ammo = ammo;
    unit.group = group;
    unit.owner = owner;
    return unit;
}

// TutorialUnits::step() returns a pose only for the units that changed this
// tick: the ones that moved, the ones that fired and the ones that were hit.
// A per-tick digest therefore keeps a running state per unit and folds the
// sparse poses into it, while the alive counts come from
// TutorialUnits::countAliveInGroup, which is exact.
struct UnitState {
    int hp{100};
    bool alive{true};
};

struct Digest {
    std::vector<UnitState> units;
    long long alivePlayer{0};
    long long aliveEnemy{0};
    long long hpPlayer{0};
    long long hpEnemy{0};
    long long shots{0};
    long long moving{0};
    long long progress{0};
};

void foldPoses(Digest& digest, const std::vector<SoldierPose>& poses) {
    // Per-tick counters are reset here; progress stays cumulative so it reads
    // as "how far the squad has advanced in total".
    digest.moving = 0;
    digest.shots = 0;
    for (const auto& p : poses) {
        if (p.index < digest.units.size()) {
            digest.units[p.index].hp = p.hp;
            digest.units[p.index].alive = p.alive;
        }
        if (p.owner == 0) {
            if (p.walking) ++digest.moving;
            digest.progress += p.progress;
        }
        if (p.fired) ++digest.shots;
    }
}

void writeDigest(const Digest& digest, const std::vector<int>& owners, RegressionRow& row) {
    long long hpPlayer = 0, hpEnemy = 0;
    for (std::size_t i = 0; i < digest.units.size(); ++i) {
        if (owners[i] == 0) hpPlayer += digest.units[i].hp;
        else hpEnemy += digest.units[i].hp;
    }
    row.fields.emplace_back("alive_player", digest.alivePlayer);
    row.fields.emplace_back("alive_enemy", digest.aliveEnemy);
    row.fields.emplace_back("hp_player", hpPlayer);
    row.fields.emplace_back("hp_enemy", hpEnemy);
    row.fields.emplace_back("shots", digest.shots);
    row.fields.emplace_back("moving", digest.moving);
    row.fields.emplace_back("progress", digest.progress);
}

// Two-sided duel: both sides auto-engage, so return fire and the loser's state
// are exercised. No oracle fixture covers this path.
std::vector<RegressionRow> emitDuel() {
    auto grid = InfantryPathGrid::fromWalkable(256, 256, std::vector<std::uint8_t>(256 * 256, 1));
    FogGrid fog(128, 128, std::vector<std::uint32_t>(128 * 128, 0));

    std::vector<assets::Infantry> units;
    units.push_back(makeInfantry("dguner", kDgSpotU, kDgSpotV, 100, 40, 0, 0));
    units.push_back(makeInfantry("aguner", kAgU, kAgV, 100, 64, 3, 1));

    TutorialUnits duel(std::move(grid), units, {}, std::move(fog), 2, true);

    Digest digest;
    digest.units.resize(units.size());
    const std::vector<int> owners{0, 1};

    std::vector<RegressionRow> rows;
    for (int t = 0; t < kDuelTicks; ++t) {
        const auto poses = duel.step();
        foldPoses(digest, poses);
        digest.alivePlayer = duel.countAliveInGroup(0, 0);
        digest.aliveEnemy = duel.countAliveInGroup(1, 3);

        RegressionRow row{};
        row.scenario = "duel";
        row.tick = t;
        writeDigest(digest, owners, row);
        rows.push_back(std::move(row));
    }
    return rows;
}

// March-pause engage: a six-soldier squad marches past a stationary enemy,
// pauses to engage it and resumes. The gating of that pause lives in
// src/sim/tutorial_units.cpp and has no oracle fixture.
std::vector<RegressionRow> emitMarch() {
    auto grid = InfantryPathGrid::fromWalkable(256, 256, std::vector<std::uint8_t>(256 * 256, 1));
    FogGrid fog(128, 128, std::vector<std::uint32_t>(128 * 128, 0));

    std::vector<assets::Infantry> squad;
    for (int k = 0; k < 6; ++k) {
        const int u = kDgCtrlU + (k % 3) * 32;
        const int v = kDgCtrlV + 96 + (k / 3) * 32;
        squad.push_back(makeInfantry("dguner", u, v, 100, 40, 0, 0));
    }
    squad.push_back(makeInfantry("aguner", kAgU, kAgV, 100, 64, 3, 1));

    TutorialUnits march(std::move(grid), squad, {}, std::move(fog), 2, true);
    const std::array<std::size_t, 6> squadIds{0, 1, 2, 3, 4, 5};
    const int destX = 4096 + kAgU - (kAgV - 64);
    const int destY = (kAgU + (kAgV - 64)) / 2;
    march.orderSelection(squadIds, destX, destY);

    Digest digest;
    digest.units.resize(squad.size());
    const std::vector<int> owners{0, 0, 0, 0, 0, 0, 1};

    std::vector<RegressionRow> rows;
    for (int t = 0; t < kMarchTicks; ++t) {
        const auto poses = march.step();
        foldPoses(digest, poses);
        digest.alivePlayer = march.countAliveInGroup(0, 0);
        digest.aliveEnemy = march.countAliveInGroup(1, 3);

        RegressionRow row{};
        row.scenario = "march";
        row.tick = t;
        writeDigest(digest, owners, row);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<RegressionRow> emitRegression() {
    std::vector<RegressionRow> rows = emitDuel();
    std::vector<RegressionRow> marchRows = emitMarch();
    rows.insert(rows.end(), marchRows.begin(), marchRows.end());
    return rows;
}

void compareRegression(const GoldenTable& golden, const std::vector<RegressionRow>& native, Report& report) {
    if (golden.regression.empty()) {
        report.fail("golden table has no REGRESSION section");
        return;
    }
    if (golden.regression.size() != native.size()) {
        report.fail("regression row count: golden=" + std::to_string(golden.regression.size()) +
                    " native=" + std::to_string(native.size()));
    }

    const std::size_t common = std::min(golden.regression.size(), native.size());
    for (std::size_t i = 0; i < common; ++i) {
        const auto& g = golden.regression[i];
        const auto& n = native[i];
        if (g.scenario != n.scenario || g.tick != n.tick) {
            report.fail("regression[" + std::to_string(i) + "] row: golden=" + g.scenario + ":" +
                        std::to_string(g.tick) + " native=" + n.scenario + ":" + std::to_string(n.tick));
            continue;
        }
        for (const auto& gf : g.fields) {
            const auto found = std::find_if(n.fields.begin(), n.fields.end(),
                                            [&](const auto& nf) { return nf.first == gf.first; });
            if (found == n.fields.end()) {
                report.fail("regression " + g.scenario + " tick " + std::to_string(g.tick) +
                            " field " + gf.first + ": missing in native");
                continue;
            }
            if (found->second != gf.second) {
                report.fail("regression " + g.scenario + " tick " + std::to_string(g.tick) +
                            " field " + gf.first + ": golden=" + std::to_string(gf.second) +
                            " native=" + std::to_string(found->second));
            }
        }
    }
}

void compareTrace(const GoldenTable& golden, const std::vector<CombatTickSnapshot>& trace, Report& report) {
    if (golden.ticks.size() != trace.size()) {
        report.fail("trace length: golden=" + std::to_string(golden.ticks.size()) +
                    " native=" + std::to_string(trace.size()));
    }

    const std::size_t common = std::min(golden.ticks.size(), trace.size());
    for (std::size_t i = 0; i < common; ++i) {
        const auto& g = golden.ticks[i];
        const auto& n = trace[i];

        const struct {
            const char* name;
            long long golden;
            long long native;
        } fields[] = {
            {"tick", g.tick, n.tick},
            {"order_id", g.orderId, n.orderId},
            {"order_q", g.orderQ, n.orderQueue},
            {"dir", g.dir, n.direction},
            {"act", g.act, n.actionState},
            {"anim", g.anim, n.animTicks},
            {"reload", g.reload, n.reload},
            {"burst", g.burst, n.burst},
            {"ammo", g.ammo, n.ammo},
            {"expa", g.expa, n.expa},
            {"tgt_spotted", g.tgtSpotted, n.targetSpotted},
            {"tgt_hp", g.tgtHp, n.targetHp},
            {"rng", static_cast<long long>(g.rng), static_cast<long long>(n.rng)},
        };

        for (const auto& f : fields) {
            if (f.golden != f.native) {
                report.fail("tick " + std::to_string(g.tick) + " field " + f.name +
                            ": golden=" + std::to_string(f.golden) +
                            " native=" + std::to_string(f.native));
            }
        }
    }
}

void compareEvents(const GoldenTable& golden, const std::vector<CombatEvent>& events, Report& report) {
    if (golden.events.size() != events.size()) {
        report.fail("event count: golden=" + std::to_string(golden.events.size()) +
                    " native=" + std::to_string(events.size()));
    }

    const std::size_t common = std::min(golden.events.size(), events.size());
    for (std::size_t i = 0; i < common; ++i) {
        const auto& g = golden.events[i];
        const auto& e = events[i];

        if (g.kind != e.kind) {
            report.fail("event[" + std::to_string(i) + "] kind: golden=" + g.kind +
                        " native=" + e.kind);
            continue;
        }
        if (g.tick != e.tick) {
            report.fail("event[" + std::to_string(i) + "] " + g.kind + " tick: golden=" +
                        std::to_string(g.tick) + " native=" + std::to_string(e.tick));
        }

        std::vector<long long> native;
        if (!nativeEventValues(e, native)) {
            report.fail("event[" + std::to_string(i) + "] unknown native event kind: " + e.kind);
            continue;
        }
        if (native.size() != g.values.size()) {
            report.fail("event[" + std::to_string(i) + "] " + g.kind + " field count: golden=" +
                        std::to_string(g.values.size()) + " native=" + std::to_string(native.size()));
            continue;
        }

        for (std::size_t f = 0; f < native.size(); ++f) {
            const long long expected = g.values[f];
            // Strict equality on every field, including the raw HP of the last
            // HP_APPLY event: the oracle and the native simulation both report
            // the unclamped value (-47), so no leniency is warranted here. A
            // native that started clamping the event value would fail loudly.
            if (expected != native[f]) {
                report.fail("event[" + std::to_string(i) + "] " + g.kind + " tick=" +
                            std::to_string(e.tick) + " field " + kValueNames[f] +
                            ": golden=" + std::to_string(expected) +
                            " native=" + std::to_string(native[f]));
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    bool emit = false;
    std::string goldenPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--emit") {
            emit = true;
        } else if (goldenPath.empty()) {
            goldenPath = arg;
        }
    }

    if (emit) {
        // Prints the native-vs-native REGRESSION rows. Used to regenerate the
        // REGRESSION section of the golden table, so the test stays the single
        // source of truth for both capture and verification.
        for (const auto& row : emitRegression()) {
            std::cout << row.scenario << " " << row.tick;
            for (const auto& f : row.fields) std::cout << " " << f.first << "=" << f.second;
            std::cout << "\n";
        }
        return 0;
    }

    if (goldenPath.empty()) {
        std::cerr << "usage: combat_parity_test <combat_parity_golden.txt>\n"
                     "       combat_parity_test --emit\n";
        return 2;
    }

    GoldenTable golden;
    std::string error;
    if (!loadGolden(goldenPath, golden, error)) {
        std::cerr << "combat_parity: " << error << "\n";
        return 1;
    }

    const auto trace = replayEncounter();
    Report report;
    compareTrace(golden, trace, report);

    // The event log lives in the CombatWorld, so replay once more to reach it.
    {
        FogGrid fog(128, 128, std::vector<std::uint32_t>(128 * 128, 0));
        CombatWorld world(std::move(fog), 2, 0x12345678u);
        world.addUnit(CombatUnit{1, 0, kDgCtrlU, kDgCtrlV, 100, 40, 0, false, InfantryCombatDesc::dguner()});
        world.addUnit(CombatUnit{2, 1, kAgU, kAgV, 100, 64, 80, false, InfantryCombatDesc::aguner()});
        world.refreshSpotting();
        world.setUnitPosition(1, kDgSpotU, kDgSpotV);
        world.refreshSpotting();
        world.issueAttackOrder(1, 2);
        for (int t = 0; t < 200; ++t) {
            world.step(1);
            if (world.snapshot(t, 1, 2).targetHp <= 0) break;
        }
        compareEvents(golden, world.events(), report);
    }

    // Native-vs-native regression baseline (no oracle claim).
    compareRegression(golden, emitRegression(), report);

    // End-state invariants from the oracle combat summary.
    if (!trace.empty()) {
        const auto& last = trace.back();
        if (last.tick != 118) report.fail("final tick: expected 118, native=" + std::to_string(last.tick));
        if (last.ammo != 36) report.fail("final ammo: expected 36, native=" + std::to_string(last.ammo));
        if (last.expa != 25) report.fail("final expa: expected 25, native=" + std::to_string(last.expa));
        if (last.targetHp != 0) report.fail("final target hp: expected 0, native=" + std::to_string(last.targetHp));
        if (last.rng != 0xe06870c1u) {
            report.fail("final rng: expected 0xe06870c1, native=0x" +
                        std::to_string(static_cast<unsigned>(last.rng)));
        }
    }

    if (!report.ok()) {
        std::cerr << "combat_parity: DIVERGENT (" << report.count() << " differences)\n";
        std::cerr << "FIRST: " << report.first() << "\n";
        return 1;
    }

    std::cout << "combat_parity: PASS (" << golden.ticks.size() << " ticks x 13 fields, "
              << golden.events.size() << " events, final_rng=0xe06870c1, "
              << golden.regression.size() << " regression rows)\n";
    assert(true);
    return 0;
}
