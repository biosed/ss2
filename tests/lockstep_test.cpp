#include "assets/mission.hpp"
#include "assets/unit_catalog.hpp"
#include "net/tcp_transport.hpp"
#include "sim/command.hpp"
#include "sim/lockstep.hpp"
#include "sim/mission_runner.hpp"
#include "sim/tutorial_units.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace cmd = ss2::cmd;

void require(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

ss2::assets::MissionData makeArenaData(std::uint16_t width = 128, std::uint16_t height = 128) {
    ss2::assets::MissionData data;
    data.width = width;
    data.height = height;
    data.scheme = 0;
    data.flags.assign(std::size_t(width) * std::size_t(height), 0);
    data.obstacles.assign(std::size_t(width) * std::size_t(height), 0);
    data.zones.assign(std::size_t(width) * std::size_t(height), 0);
    data.phrases.resize(1);
    data.scripts.resize(1, {0x7fffffffu});
    return data;
}

struct LockstepWorld {
    ss2::assets::MissionData data;
    std::vector<ss2::assets::Infantry> infantry;
    std::vector<ss2::assets::VehicleUnit> vehicles;
    std::shared_ptr<const ss2::assets::UnitCatalog> catalog;

    LockstepWorld() : data(makeArenaData()) {
        const auto weapon = [](std::string id, ss2::assets::HitType hit, int dmg, int range,
                               int reload, int burst, int burstReload, std::string sound) {
            ss2::assets::WeaponSpec w{};
            w.shotId = std::move(id);
            w.hitType = hit;
            w.damage = w.damageMax = dmg;
            w.range = w.rangeMax = range;
            w.reloadTicks = reload;
            w.burstShots = burst;
            w.burstReloadTicks = burstReload;
            w.soundGroup = std::move(sound);
            return w;
        };

        ss2::assets::UnitSpec rifle{};
        rifle.id = "dguner";
        rifle.isVehicle = false;
        rifle.health = 100;
        rifle.ammo1 = 40;
        rifle.sight = 6;
        rifle.moveSpeed = 0.85;
        rifle.shot1 = weapon("rifle", ss2::assets::HitType::Machine, 22, 320, 4, 3, 12, "rifle_burst");

        ss2::assets::UnitSpec allyRifle = rifle;
        allyRifle.id = "aguner";

        ss2::assets::UnitSpec sherman{};
        sherman.id = "sherman.v";
        sherman.isVehicle = true;
        sherman.health = 660;
        sherman.ammo1 = 71;
        sherman.ammo2 = 400;
        sherman.sight = 8;
        sherman.moveSpeed = 1.15;
        sherman.armor[static_cast<std::size_t>(ss2::assets::HitType::Pierce)] = {63, 45, 38};
        sherman.armor[static_cast<std::size_t>(ss2::assets::HitType::Machine)] = {256, 256, 256};
        sherman.shot1 = weapon("pierce6-7k", ss2::assets::HitType::Pierce, 450, 560, 5, 1, 5, "gunshot");
        sherman.shot2 = weapon("man_machine", ss2::assets::HitType::Machine, 46, 480, 2, 4, 12, "mg_tank_burst");

        ss2::assets::UnitSpec panzer = sherman;
        panzer.id = "pz4";
        panzer.health = 620;

        catalog = ss2::assets::UnitCatalog::fromSpecs({rifle, allyRifle, sherman, panzer});

        // Seat 0 force around x=3000 (west), Seat 1 force around x=4100 (east).
        // Separated by >1100 px so each side initially only sees its own units under fog.
        infantry = {
            {"dguner", 3000, 2000, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},
            {"dguner", 3040, 2060, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},
            {"aguner", 4120, 2000, 100, 40, 2, 1, 0, 0, 0, 0, 4u, 0u, false, true},
            {"aguner", 4160, 2060, 100, 40, 2, 1, 0, 0, 0, 0, 4u, 0u, false, true},
        };
        vehicles = {
            {"pz4", 2960, 2032, 100, 100, 1, 0, 0u, 0u, 0, 0, 0, 0, true, true},
            {"sherman.v", 4160, 2032, 100, 100, 2, 1, 0u, 0u, 0, 0, 0, 0, true, true},
        };
    }

    ss2::TutorialUnits units() const {
        ss2::FogGrid fog(data.width, data.height, data.flags);
        return ss2::TutorialUnits(
            ss2::InfantryPathGrid::fromSubCellMasks(data.width, data.height, data.flags, data.obstacles),
            infantry, {}, std::move(fog), 2, true, vehicles, catalog);
    }

    std::unique_ptr<ss2::MissionRunner> makeRunner(int viewerSeat) const {
        auto r = std::make_unique<ss2::MissionRunner>(ss2::MissionRunner::ManualClock{}, data, units());
        r->setMultiplayerSymmetry(true);
        r->setViewerOwner(viewerSeat);
        return r;
    }
};

void testWireCodec() {
    using namespace ss2;

    std::vector<LockstepPacket> packets;
    packets.push_back(HelloPacket{kLockstepProtocolVersion, 1, "Commander Two", 2});
    packets.push_back(MatchSetupPacket{
        1, 2, 2, 2, 5, 0x0123456789abcdefULL, "skirmish_2p", {0, 1, 0, 1}, false,
        {1, 2, 0, 3}, {"Commander One", "Commander Two", "", ""}});
    packets.push_back(TurnBundlePacket{
        17u,
        1u,
        {
            cmd::Move{{0, 2, 5}, 3450, 2180, true},
            cmd::AttackVehicle{{0}, 1},
            cmd::TogglePause{},
        },
        0xfedcba9876543210ULL,
        34u,
    });
    packets.push_back(TurnBundlePacket{18u, 0u, {}, std::nullopt, 0u});
    packets.push_back(DesyncPacket{40u, 0x1111u, 0x2222u});
    packets.push_back(ByePacket{1u, "match ended"});
    packets.push_back(ChatPacket{1u, "Commander Two", "GLHF!"});
    packets.push_back(PingPacket{7u, 123456ULL});
    packets.push_back(PongPacket{7u, 123456ULL});

    // Fragmented 1-byte feeding + trailing frame preservation
    std::vector<std::uint8_t> stream;
    for (const auto& p : packets) {
        const auto enc = encodeLockstepPacket(p);
        stream.insert(stream.end(), enc.begin(), enc.end());
    }
    const std::vector<std::uint8_t> extra{0xaa, 0xbb};
    std::vector<std::uint8_t> rx;
    std::vector<LockstepPacket> decoded;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        rx.push_back(stream[i]);
        for (;;) {
            std::optional<LockstepPacket> pkt;
            const auto st = tryDecodeLockstepPacket(rx, pkt);
            require(st != LockstepDecodeStatus::Invalid, "valid packet fragment marked invalid");
            if (st == LockstepDecodeStatus::NeedMore) break;
            require(pkt.has_value(), "decoded empty optional on Ok");
            decoded.push_back(std::move(*pkt));
        }
    }
    require(rx.empty(), "rx buffer should be drained cleanly");
    require(decoded.size() == packets.size(), "decoded packet count mismatch");

    const auto& p0 = std::get<HelloPacket>(decoded[0]);
    require(p0.nation == 2u && p0.playerName == "Commander Two", "HelloPacket nation round-trip mismatch");
    const auto& p1 = std::get<MatchSetupPacket>(decoded[1]);
    require(p1.mapHash == 0x0123456789abcdefULL && p1.missionSpec == "skirmish_2p" &&
            !p1.autoEngage && p1.playerTeams[2] == 0 && p1.playerTeams[3] == 1 &&
            p1.playerNations[0] == 1u && p1.playerNations[1] == 2u &&
            p1.playerNames[0] == "Commander One" && p1.playerNames[1] == "Commander Two",
            "MatchSetupPacket fields round-trip mismatch");
    const auto& p2 = std::get<TurnBundlePacket>(decoded[2]);
    require(p2.turn == 17u && p2.seat == 1u && p2.commands.size() == 3u &&
            p2.stateHash == std::optional<std::uint64_t>(0xfedcba9876543210ULL) && p2.hashTick == 34u,
            "TurnBundlePacket fields round-trip mismatch");
    const auto& p5 = std::get<ByePacket>(decoded[5]);
    require(p5.seat == 1u && p5.reason == "match ended", "ByePacket round-trip mismatch");
    const auto& p6 = std::get<ChatPacket>(decoded[6]);
    require(p6.seat == 1u && p6.sender == "Commander Two" && p6.text == "GLHF!", "ChatPacket round-trip mismatch");
    const auto& p7 = std::get<PingPacket>(decoded[7]);
    require(p7.seq == 7u && p7.sentTimeMs == 123456ULL, "PingPacket round-trip mismatch");
    const auto& p8 = std::get<PongPacket>(decoded[8]);
    require(p8.seq == 7u && p8.echoedTimeMs == 123456ULL, "PongPacket round-trip mismatch");

    // Trailing partial bytes
    {
        std::vector<std::uint8_t> tail = encodeLockstepPacket(ByePacket{0u, "ok"});
        tail.insert(tail.end(), extra.begin(), extra.end());
        std::optional<LockstepPacket> pkt;
        require(tryDecodeLockstepPacket(tail, pkt) == LockstepDecodeStatus::Ok, "leading packet should decode");
        require(tail == extra, "trailing bytes were not preserved");
    }

    // Invalid frames: corrupted magic, zero length, unknown tag, oversized frame
    {
        std::vector<std::uint8_t> badMagic = encodeLockstepPacket(HelloPacket{kLockstepProtocolVersion, 1, "P2"});
        badMagic[5] ^= 0xffu;
        std::optional<LockstepPacket> pkt;
        require(tryDecodeLockstepPacket(badMagic, pkt) == LockstepDecodeStatus::Invalid,
                "corrupted magic must fail cleanly");

        std::vector<std::uint8_t> zeroLen{0u, 0u, 0u, 0u};
        require(tryDecodeLockstepPacket(zeroLen, pkt) == LockstepDecodeStatus::Invalid,
                "zero payload length must fail cleanly");

        std::vector<std::uint8_t> badTag{0u, 0u, 0u, 1u, 0x99u};
        require(tryDecodeLockstepPacket(badTag, pkt) == LockstepDecodeStatus::Invalid,
                "unknown packet tag must fail cleanly");

        std::vector<std::uint8_t> huge{0x01u, 0x00u, 0x00u, 0x00u};
        require(tryDecodeLockstepPacket(huge, pkt) == LockstepDecodeStatus::Invalid,
                "oversized payload length must fail cleanly");

        std::vector<std::uint8_t> badHelloNation =
            encodeLockstepPacket(HelloPacket{kLockstepProtocolVersion, 1, "BadNat", 5u});
        require(tryDecodeLockstepPacket(badHelloNation, pkt) == LockstepDecodeStatus::Invalid,
                "HelloPacket with nation > 4 must fail cleanly");
    }

    // computeMissionMapHash determinism & sensitivity
    auto d1 = makeArenaData();
    auto d2 = makeArenaData();
    const std::uint64_t h1 = computeMissionMapHash(d1);
    require(h1 == computeMissionMapHash(d2), "computeMissionMapHash not deterministic");
    d2.flags[17] ^= 1u;
    require(h1 != computeMissionMapHash(d2), "computeMissionMapHash must change when flags change");
    d2 = d1;
    d2.scripts[0][0] ^= 2u;
    require(h1 != computeMissionMapHash(d2), "computeMissionMapHash must change when scripts change");

    std::cout << "lockstep wire codec & map hash checks PASSED\n";
}

void testTwinRunnerLockstep() {
    using namespace ss2;
    const LockstepWorld world;
    auto runner0 = world.makeRunner(0);
    auto runner1 = world.makeRunner(1);

    const std::uint64_t initHash = runner0->stateHash();
    require(initHash == runner1->stateHash(),
            "Initial stateHash differs between viewer seats 0 and 1");

    // Viewer fog isolation check on initial tick: neither seat can target unspotted opponent units.
    require(!runner0->isValidAttackTarget(2) && !runner0->isValidVehicleAttackTarget(1) &&
            !runner1->isValidAttackTarget(0) && !runner1->isValidVehicleAttackTarget(0),
            "Distant enemy units should be hidden under per-seat fog at tick 0");

    LockstepCoordinator coord0(LockstepConfig{0u, 2u, 2u, 2u, 5u});
    LockstepCoordinator coord1(LockstepConfig{1u, 2u, 2u, 2u, 5u});

    std::size_t sunk0 = 0;
    std::size_t sunk1 = 0;
    runner0->setCommandSink([&](cmd::Command c) {
        ++sunk0;
        coord0.queueLocalCommand(std::move(c));
    });
    runner1->setCommandSink([&](cmd::Command c) {
        ++sunk1;
        coord1.queueLocalCommand(std::move(c));
    });

    const auto flushBundles = [&] {
        for (const auto& b : coord0.produceOutboundBundles()) {
            require(coord1.receiveTurnBundle(b), "coord1 rejected seat 0 turn bundle");
        }
        for (const auto& b : coord1.produceOutboundBundles()) {
            require(coord0.receiveTurnBundle(b), "coord0 rejected seat 1 turn bundle");
        }
    };

    // Issue commands through commandSink_-intercepted MissionRunner methods AND direct queueLocalCommand across seats,
    // including cross-seat hijack attempts and a Pause/Unpause cycle across different seats.
    const auto initSnap = runner0->takeSnapshot();
    require(runner1->canMove(std::array<std::size_t, 2>{2, 3}, 3620, 2030),
            "Seat 1 canMove should accept Seat 1 soldiers");
    require(!runner1->canMove(std::array<std::size_t, 2>{0, 1}, 3620, 2030),
            "Seat 1 canMove must reject Seat 0 soldiers");
    const auto moved0 = runner0->move(std::array<std::size_t, 2>{0, 1}, 3560, 2030, false);
    require(moved0.has_value() && sunk0 == 1u && runner0->stateHash() == initHash,
            "Player command on local runner with commandSink should be intercepted without mutating state");
    runner0->moveVehicles(std::array<std::size_t, 1>{0}, 3520, 2032, false);
    // Seat 0 tries illegally to move Seat 1's vehicle #1:
    coord0.queueLocalCommand(cmd::MoveVehicles{{1}, 100, 100, false});

    runner1->move(std::array<std::size_t, 2>{2, 3}, 3620, 2030, false);
    require(sunk1 == 1u, "Seat 1 move should be intercepted by commandSink");
    runner1->moveVehicles(std::array<std::size_t, 1>{1}, 3660, 2032, false);
    // Seat 1 tries illegally to move Seat 0's vehicle #0:
    coord1.queueLocalCommand(cmd::MoveVehicles{{0}, 100, 100, false});

    bool checkedPausedGap = false;
    bool checkedUnpaused = false;

    for (std::uint32_t step = 0; step < 250u; ++step) {
        if (step == 18u) {
            coord0.queueLocalCommand(cmd::TogglePause{});
        }
        if (step == 28u) {
            require(runner0->isPaused() && runner1->isPaused(),
                    "TogglePause from seat 0 must pause both lockstep runners");
            checkedPausedGap = true;
            coord1.queueLocalCommand(cmd::TogglePause{});
        }
        if (step == 40u) {
            require(!runner0->isPaused() && !runner1->isPaused(),
                    "TogglePause from seat 1 must unpause both lockstep runners without deadlock");
            checkedUnpaused = true;
        }
        if (step == 110u) {
            runner0->attackMove(std::array<std::size_t, 2>{0, 1}, 3700, 2032, true);
            runner1->attackWithVehicles(std::array<std::size_t, 1>{1}, true, 0);
        }

        flushBundles();
        const auto st0 = coord0.stepTick(*runner0);
        const auto st1 = coord1.stepTick(*runner1);
        require(st0 == LockstepCoordinator::StepStatus::Stepped, "coord0 unexpectedly stalled or desynced");
        require(st1 == LockstepCoordinator::StepStatus::Stepped, "coord1 unexpectedly stalled or desynced");
        require(runner0->stateHash() == runner1->stateHash(),
                "Twin runner stateHash diverged at lockstepTick " + std::to_string(coord0.lockstepTick()));
    }
    flushBundles();

    require(checkedPausedGap && checkedUnpaused, "Pause/unpause lifecycle checks did not execute");
    require(!coord0.desync().has_value() && !coord1.desync().has_value(),
            "Unexpected desync info recorded");
    require(coord0.lastVerifiedTick().has_value() && *coord0.lastVerifiedTick() >= 230u,
            "Periodic lockstep stateHash verification did not advance");

    // Verify ownership isolation & Seat 1 infantry execution: neither vehicle moved to (100, 100) from opponent hijack commands,
    // and Seat 1 soldiers executed cmd::Move via formationTargets().
    {
        const auto snap = runner0->takeSnapshot();
        require(snap.vehicles.size() == 2u, "Expected 2 vehicles in snapshot");
        require(snap.vehicles[0].x >= 2950 && snap.vehicles[1].x >= 3500,
                "Cross-seat hijack command must be rejected");
        require(snap.soldiers.size() == 4u &&
                (snap.soldiers[2].cellX != initSnap.soldiers[2].cellX ||
                 snap.soldiers[2].cellY != initSnap.soldiers[2].cellY),
                "Seat 1 infantry cmd::Move must execute via formationTargets()");
    }

    constexpr std::uint64_t kExpectedTwinLockstepHash = 0xdb1c5caa1787fcc7ULL;
    const std::uint64_t finalHash = runner0->stateHash();
    require(finalHash == kExpectedTwinLockstepHash,
            "LOCKSTEP_DETERMINISM_HASH drifted from pinned baseline");
    std::ostringstream oss;
    oss << "0x" << std::hex << std::nouppercase << std::setw(16) << std::setfill('0') << finalHash;
    std::cout << "LOCKSTEP_DETERMINISM_HASH " << oss.str()
              << " verifiedTick=" << *coord0.lastVerifiedTick() << '\n';

    // Explicit desync detection test
    TurnBundlePacket bogusHash{coord0.lockstepTick() / 2u + 5u, 1u, {}, finalHash ^ 0xdeadbeefULL, *coord0.lastVerifiedTick()};
    require(coord0.receiveTurnBundle(bogusHash), "receiveTurnBundle rejected hash packet");
    require(coord0.desync().has_value() &&
            coord0.desync()->localHash != coord0.desync()->remoteHash,
            "Desync detector failed to flag mismatched remote stateHash");
    require(coord0.stepTick(*runner0) == LockstepCoordinator::StepStatus::Desynced,
            "stepTick must report Desynced once mismatch is detected");
}

void testLoopbackTcpSession() {
    using namespace ss2;
    const LockstepWorld world;
    const std::uint64_t mapHash = computeMissionMapHash(world.data);

    // 1. Handshake rejects mismatched mapHash
    {
        MultiplayerHandshake host;
        MultiplayerHandshake joiner;
        MatchSetupPacket setup{};
        setup.assignedSeat = 1;
        setup.playerCount = 2;
        setup.mapHash = mapHash;
        setup.missionSpec = "skirmish_2p";
        require(host.startHost(0, setup, "127.0.0.1"), "Host listener failed: " + host.errorText());
        const std::uint16_t port = host.listeningPort();
        require(port > 0, "Ephemeral listening port was 0");
        require(joiner.startJoin("127.0.0.1", port, "P2", mapHash ^ 1ULL),
                "Joiner connect failed: " + joiner.errorText());

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (joiner.state() != MultiplayerHandshake::State::Failed &&
               std::chrono::steady_clock::now() < deadline) {
            host.poll();
            joiner.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(joiner.state() == MultiplayerHandshake::State::Failed,
                "Joiner did not fail on mapHash mismatch");
        require(joiner.errorText().find("mismatch") != std::string::npos,
                "Expected mapHash mismatch error text, got: " + joiner.errorText());
    }

    // 2. Matching handshake + 150-tick live POSIX TCP session + clean ByePacket
    {
        MultiplayerHandshake host;
        MultiplayerHandshake joiner;
        MatchSetupPacket setup{};
        setup.assignedSeat = 1;
        setup.playerCount = 2;
        setup.turnTicks = 2;
        setup.inputDelayTurns = 2;
        setup.hashIntervalTurns = 5;
        setup.mapHash = mapHash;
        setup.missionSpec = "skirmish_2p";
        require(host.startHost(0, setup, "127.0.0.1"), "Host listener failed: " + host.errorText());
        const std::uint16_t port = host.listeningPort();
        require(joiner.startJoin("127.0.0.1", port, "Player 2", mapHash),
                "Joiner failed: " + joiner.errorText());

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((host.state() != MultiplayerHandshake::State::Ready ||
                joiner.state() != MultiplayerHandshake::State::Ready) &&
               std::chrono::steady_clock::now() < deadline) {
            require(host.state() != MultiplayerHandshake::State::Failed,
                    "Host handshake failed: " + host.errorText());
            require(joiner.state() != MultiplayerHandshake::State::Failed,
                    "Joiner handshake failed: " + joiner.errorText());
            host.poll();
            joiner.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(host.state() == MultiplayerHandshake::State::Ready &&
                joiner.state() == MultiplayerHandshake::State::Ready,
                "TCP handshake timed out");
        require(joiner.matchSetup().mapHash == mapHash && joiner.matchSetup().assignedSeat == 1,
                "Joiner received unexpected MatchSetupPacket");

        auto conn0 = host.takeConnection();
        auto conn1 = joiner.takeConnection();
        require(conn0 && conn0->isConnected() && conn1 && conn1->isConnected(),
                "Handshake Ready did not yield connected TcpConnections");

        auto runner0 = world.makeRunner(0);
        auto runner1 = world.makeRunner(1);
        LockstepCoordinator coord0(LockstepConfig{0u, 2u, 2u, 2u, 5u});
        LockstepCoordinator coord1(LockstepConfig{1u, 2u, 2u, 2u, 5u});
        runner0->setCommandSink([&](cmd::Command c) { coord0.queueLocalCommand(std::move(c)); });
        runner1->setCommandSink([&](cmd::Command c) { coord1.queueLocalCommand(std::move(c)); });

        const auto pumpNetwork = [&] {
            for (auto& b : coord0.produceOutboundBundles()) {
                require(conn0->sendPacket(LockstepPacket{std::move(b)}), "conn0 send failed");
            }
            for (auto& b : coord1.produceOutboundBundles()) {
                require(conn1->sendPacket(LockstepPacket{std::move(b)}), "conn1 send failed");
            }
            conn0->flush();
            conn1->flush();

            std::vector<LockstepPacket> pkts;
            require(conn0->pollPackets(pkts), "conn0 pollPackets failed: " + conn0->lastError());
            for (auto& p : pkts) {
                if (auto* b = std::get_if<TurnBundlePacket>(&p)) {
                    require(coord0.receiveTurnBundle(std::move(*b)), "coord0 rejected peer bundle");
                }
            }
            pkts.clear();
            require(conn1->pollPackets(pkts), "conn1 pollPackets failed: " + conn1->lastError());
            for (auto& p : pkts) {
                if (auto* b = std::get_if<TurnBundlePacket>(&p)) {
                    require(coord1.receiveTurnBundle(std::move(*b)), "coord1 rejected peer bundle");
                }
            }
        };

        runner0->move(std::array<std::size_t, 2>{0, 1}, 3450, 2030, false);
        runner1->moveVehicles(std::array<std::size_t, 1>{1}, 3650, 2032, false);

        std::uint32_t ticks0 = 0;
        std::uint32_t ticks1 = 0;
        const auto simDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while ((ticks0 < 150u || ticks1 < 150u) && std::chrono::steady_clock::now() < simDeadline) {
            pumpNetwork();
            if (ticks0 < 150u) {
                const auto st = coord0.stepTick(*runner0);
                require(st != LockstepCoordinator::StepStatus::Desynced, "TCP runner0 desynced");
                if (st == LockstepCoordinator::StepStatus::Stepped) ++ticks0;
            }
            if (ticks1 < 150u) {
                const auto st = coord1.stepTick(*runner1);
                require(st != LockstepCoordinator::StepStatus::Desynced, "TCP runner1 desynced");
                if (st == LockstepCoordinator::StepStatus::Stepped) ++ticks1;
            }
            if (ticks0 < 150u || ticks1 < 150u) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        require(ticks0 == 150u && ticks1 == 150u, "Loopback TCP session timed out before 150 ticks");
        require(runner0->stateHash() == runner1->stateHash(),
                "Loopback TCP final stateHash diverged between host and joiner");
        require(coord0.lastVerifiedTick().has_value() && *coord0.lastVerifiedTick() >= 130u,
                "Loopback TCP periodic hash verification did not trigger");

        require(conn1->sendPacket(ByePacket{1u, "exit"}), "send ByePacket failed");
        conn1->flush();
        bool sawBye = false;
        const auto byeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!sawBye && std::chrono::steady_clock::now() < byeDeadline) {
            std::vector<LockstepPacket> pkts;
            conn0->pollPackets(pkts);
            for (const auto& p : pkts) {
                if (const auto* bye = std::get_if<ByePacket>(&p); bye && bye->seat == 1u) {
                    sawBye = true;
                }
            }
            if (!sawBye) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(sawBye, "Host did not receive ByePacket from disconnecting peer");
        std::cout << "Loopback POSIX TCP lockstep session (150 ticks) PASSED\n";
    }
}

void testMultiplayerSkirmishAndZeppelins() {
    using namespace ss2;
    LockstepWorld w;
    // gates[0] at tile (20, 20), gates[3] at tile (90, 90); marker[0] Zeppelin at tile (40, 40) = (4096, 1296)
    const int originX = 128 * 32;
    w.data.gates[0] = {originX + 32 * (20 - 20), 16 * (20 + 20 + 1)};
    w.data.gates[3] = {originX + 32 * (90 - 90), 16 * (90 + 90 + 1)};
    w.data.markers[0] = {originX + 32 * (40 - 40), 16 * (40 + 40 + 1)};
    w.data.reinforcements[2] = {{"dguner", 0, 100, 40, 0, {}}};
    w.data.reinforcements[4] = {{"aguner", 0, 100, 40, 0, {}}};
    w.data.reinforcements[10] = {{"dguner", 0, 100, 40, 0, {}}};
    w.data.reinforcements[11] = {{"aguner", 0, 100, 40, 0, {}}};
    // Trigger 0 (CAOF): OP(51) OP(7) OP(11) + OP(52) starting waves + OP(53/54/56) Zeppelin 0 reward + END
    const auto lit = [](std::uint32_t v) { return 0x80000000u | v; };
    w.data.scripts = {{
        51u, 7u, 11u,
        lit(4u), lit(2u), lit(6u), lit(0u), lit(3u), lit(0u), 52u,
        lit(1u), lit(11u), lit(10u), lit(10u), lit(10u), lit(10u), lit(60u), 53u,
        lit(1u), lit(2u), lit(0u), 54u,
        lit(1u), lit(3u), lit(0u), 56u,
        0x7fffffffu,
    }};
    // Place Seat 0 near Zeppelin (40,40) = (4096, 1296), Seat 1 at tile (44, 44) = (4096, 1424) -> outside radius 4 until moved
    w.infantry = {
        {"dguner", 4096, 1296, 100, 40, 1, 0, 0, 0, 0, 0, 0u, 0u, false, true},
        {"aguner", 4096, 1424, 100, 40, 2, 1, 0, 0, 0, 0, 0u, 0u, false, true},
    };
    w.vehicles.clear();

    auto runner0 = w.makeRunner(0);
    auto runner1 = w.makeRunner(1);
    const MultiplayerSkirmishConfig cfg{2, {1, 0, 2, 3}, {0, 1, 2, 3}, 12u, 0u, 4};
    runner0->configureMultiplayerSkirmish(cfg);
    runner1->configureMultiplayerSkirmish(cfg);
    require(runner0->stateHash() == runner1->stateHash(), "skirmish initial hash mismatch");
    require(runner0->assignedGateForSeat(0) == 0 && runner0->assignedGateForSeat(1) == 3,
            "active gates 0 & 3 must map to seats 0 & 1");

    // Initial tick-0 OP(52) nation waves: Seat 0 (German=1 -> wave 2), Seat 1 (Russian=0 -> wave 4)
    const auto ev0Init = runner0->poll();
    const auto ev1Init = runner1->poll();
    require(ev0Init.size() == 2u && ev1Init.size() == 2u, "OP(52) must queue nation start waves for both seats");
    require(ev0Init[0].kind == MissionEvent::Reinforcement && ev0Init[0].index == 2u &&
            ev0Init[0].owner == 0u && ev0Init[0].gate == 0u,
            "OP(52) German wave index/gate mismatch");
    require(ev0Init[1].kind == MissionEvent::Reinforcement && ev0Init[1].index == 4u &&
            ev0Init[1].owner == 1u && ev0Init[1].gate == 3u,
            "OP(52) Russian wave index/gate mismatch");

    LockstepCoordinator coord0(LockstepConfig{0u, 2u, 2u, 2u, 4u});
    LockstepCoordinator coord1(LockstepConfig{1u, 2u, 2u, 2u, 4u});
    const auto syncStep = [&](int n) {
        for (int s = 0; s < n; ++s) {
            for (auto& b : coord0.produceOutboundBundles()) {
                require(coord1.receiveTurnBundle(b), "coord1 rejected skirmish bundle");
                coord0.receiveTurnBundle(std::move(b));
            }
            for (auto& b : coord1.produceOutboundBundles()) {
                require(coord0.receiveTurnBundle(b), "coord0 rejected skirmish bundle");
                coord1.receiveTurnBundle(std::move(b));
            }
            const auto st0 = coord0.stepTick(*runner0);
            const auto st1 = coord1.stepTick(*runner1);
            require(st0 == LockstepCoordinator::StepStatus::Stepped &&
                    st1 == LockstepCoordinator::StepStatus::Stepped,
                    "skirmish twin runner stalled or desynced");
        }
    };

    coord0.queueLocalCommand(cmd::ToggleHoldFire{{0}});
    coord1.queueLocalCommand(cmd::ToggleHoldFire{{1}});
    syncStep(40);
    const auto zepAfterCap = runner0->zeppelinStatuses();
    require(zepAfterCap.size() == 1u && zepAfterCap[0].holderTeam == 0 && !zepAfterCap[0].contested,
            "Seat 0 holding near Zeppelin 0 must capture it");
    require(runner0->airSupportFor(0)[0].ready == 2 && runner0->airSupportFor(0)[0].total == 3 &&
            runner1->airSupportFor(0)[0].ready == 2 && runner1->airSupportFor(0)[0].total == 3,
            "Zeppelin OP(54)/OP(56) must grant identical air planes and sorties on both peers");
    bool sawZepWave = false;
    for (const auto& ev : runner0->poll()) {
        if (ev.kind == MissionEvent::Reinforcement && ev.index == 10u && ev.owner == 0u) sawZepWave = true;
    }
    require(sawZepWave, "Zeppelin capture must emit OP(53) wave 10 for Seat 0");
    (void)runner1->poll();

    // Anti-forged Resign from Seat 1 attempting to resign Seat 0 must be rejected.
    runner0->applyAs(1, cmd::Resign{0});
    runner1->applyAs(1, cmd::Resign{0});
    require(runner0->poll().empty() && runner1->poll().empty(),
            "Forged Resign(seat=0) by actor=1 must be ignored");

    // Move Seat 1 soldier into Zeppelin capture radius -> contested state at next 16-tick scan
    coord1.queueLocalCommand(cmd::Move{{1}, 4096, 1296, false});
    syncStep(60);
    const auto zepContested = runner0->zeppelinStatuses();
    require(zepContested.size() == 1u && zepContested[0].contested && zepContested[0].holderTeam == -1,
            "Enemy entering held Zeppelin must mark it contested");

    // Genuine resign from Seat 1 -> Seat 0 Victory, Seat 1 Defeat, identical stateHash()
    coord1.queueLocalCommand(cmd::Resign{1});
    syncStep(10);
    bool r0Victory = false, r1Defeat = false;
    for (const auto& ev : runner0->poll()) if (ev.kind == MissionEvent::Victory) r0Victory = true;
    for (const auto& ev : runner1->poll()) if (ev.kind == MissionEvent::Defeat) r1Defeat = true;
    require(r0Victory && r1Defeat, "Valid Resign(1) must emit Victory for Seat 0 and Defeat for Seat 1");
    require(runner0->stateHash() == runner1->stateHash(), "Skirmish final stateHash diverged");

    constexpr std::uint64_t kExpectedSkirmishHash = 0x653b9a007be75c29ULL;
    const std::uint64_t skirmishHash = runner0->stateHash();
    require(skirmishHash == kExpectedSkirmishHash,
            "LOCKSTEP_SKIRMISH_HASH drifted from pinned baseline");
    std::ostringstream oss;
    oss << "0x" << std::hex << std::nouppercase << std::setw(16) << std::setfill('0') << skirmishHash;
    std::cout << "LOCKSTEP_SKIRMISH_HASH " << oss.str() << '\n';
}

}  // namespace

int main(int argc, char** argv) try {
    bool simOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--sim-only") simOnly = true;
    }

    testWireCodec();
    testTwinRunnerLockstep();
    testMultiplayerSkirmishAndZeppelins();
    if (!simOnly) {
        testLoopbackTcpSession();
    }

    std::cout << "lockstep_test PASSED\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "lockstep_test FAILED: " << e.what() << '\n';
    return 1;
}
