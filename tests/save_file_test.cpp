#include "session/save_file.hpp"
#include "sim/infantry_combat.hpp"
#include "sim/infantry_path.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool cond, const std::string& msg) {
    if (!cond) throw std::runtime_error(msg);
}

ss2::TutorialUnits::Snapshot makeSyntheticSnapshot() {
    ss2::TutorialUnits::Snapshot snap{};
    snap.tick = 1425u;
    snap.playerTeams = {0, 1, 2, 0};
    snap.mines = {0u, 1u, 2u, 0u};
    snap.fog = {0u, 1u, 3u, 1u};

    ss2::TutorialUnits::Snapshot::SoldierState s0{};
    s0.cellX = 12;
    s0.cellY = 18;
    s0.hp = 90;
    s0.maxHp = 100;
    s0.ammo = 35;
    s0.expa = 4096;
    s0.kills = 2;
    s0.atGrenades = 1;
    s0.mineAmmo = 3;
    s0.owner = 0;
    s0.group = 30;
    s0.direction = 5;
    s0.crouched = true;
    s0.holdFire = false;
    s0.binoculars = true;
    s0.embarked = false;
    s0.garrisonBuilding = -1;
    s0.suppressionTicks = 12;

    ss2::TutorialUnits::Snapshot::SoldierState s1{};
    s1.cellX = 40;
    s1.cellY = 44;
    s1.hp = 100;
    s1.maxHp = 100;
    s1.ammo = 20;
    s1.owner = 1;
    s1.group = 12;
    s1.direction = 1;

    ss2::TutorialUnits::Snapshot::SoldierState sDead{};
    sDead.cellX = 20;
    sDead.cellY = 22;
    sDead.hp = 0;
    sDead.maxHp = 100;
    sDead.owner = 0;

    snap.soldiers = {s0, s1, sDead};

    ss2::TutorialUnits::Snapshot::VehicleState v0{};
    v0.x = 640;
    v0.y = 480;
    v0.hullDir = 8;
    v0.turretDir = 10;
    v0.hp = 450;
    v0.maxHp = 500;
    v0.ammo = 40;
    v0.ammo2 = 120;
    v0.expa = 8192;
    v0.kills = 4;
    v0.owner = 0;
    v0.group = 5;
    v0.alive = true;
    v0.holdFire = true;
    v0.passengers = {};
    v0.towedVehicle = -1;
    v0.towedBy = -1;
    v0.hasCrew = true;
    v0.crewHp = 180;

    ss2::TutorialUnits::Snapshot::VehicleState vDead{};
    vDead.x = 700;
    vDead.y = 500;
    vDead.hp = 0;
    vDead.maxHp = 300;
    vDead.owner = 0;
    vDead.alive = false;
    vDead.hasCrew = false;
    vDead.crewHp = 0;

    snap.vehicles = {v0, vDead};

    ss2::TutorialUnits::Snapshot::BuildingState b0{};
    b0.hp = 850;
    b0.owner = 0;
    b0.destroyed = false;
    b0.occupants = {};
    snap.buildings = {b0};

    snap.airReady[0][0] = 2;
    snap.airReady[0][2] = 1;
    return snap;
}

struct TempDirGuard {
    std::filesystem::path dir;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

int main() try {
    const auto snap = makeSyntheticSnapshot();

    // 1. SS2SAVE1 binary round-trip with full metadata and debrief counters.
    {
        ss2::SaveFileMetadata meta{};
        meta.hasMetadata = true;
        meta.mission = "maps/campaign/023000.ssc";
        meta.missionTitle = "1. Starting Position";
        meta.difficulty = 3;
        meta.savedEpochSec = 1795250000;
        meta.enemyInfKilled = 14;
        meta.enemyVehDestroyed = 3;
        meta.enemyTanksDestroyed = 2;
        meta.enemyGunsDestroyed = 1;
        meta.enemyPlanesShotDown = 1;
        meta.buildingsDemolished = 4;
        meta.airSortiesFlown = 2;
        meta.enemyScoreEarned = 5200;
        meta.friendlyScoreLost = 300;

        const auto bytes = ss2::encodeSaveFile(meta, snap);
        check(bytes.size() > 32 &&
                  std::string_view(reinterpret_cast<const char*>(bytes.data()), 8u) == "SS2SAVE1",
              "encodeSaveFile emits SS2SAVE1 header");

        const auto dec = ss2::decodeSaveFile(bytes);
        check(dec.has_value(), "decodeSaveFile decodes SS2SAVE1 payload");
        check(dec->meta.hasMetadata, "decoded save marks hasMetadata");
        check(dec->meta.mission == meta.mission &&
                  dec->meta.missionTitle == meta.missionTitle &&
                  dec->meta.difficulty == 3 &&
                  dec->meta.savedEpochSec == meta.savedEpochSec,
              "SS2SAVE1 metadata header round-trips");
        check(dec->meta.tick == 1425u &&
                  dec->meta.playerSoldiers == 1u &&
                  dec->meta.playerVehicles == 1u &&
                  dec->meta.totalSoldiers == 3u &&
                  dec->meta.totalVehicles == 2u &&
                  dec->meta.fileBytes == bytes.size(),
              "SS2SAVE1 force/tick summary counts alive owner==0 units");
        check(dec->meta.enemyInfKilled == 14u &&
                  dec->meta.enemyVehDestroyed == 3u &&
                  dec->meta.enemyTanksDestroyed == 2u &&
                  dec->meta.enemyGunsDestroyed == 1u &&
                  dec->meta.enemyPlanesShotDown == 1u &&
                  dec->meta.buildingsDemolished == 4u &&
                  dec->meta.airSortiesFlown == 2u &&
                  dec->meta.enemyScoreEarned == 5200 &&
                  dec->meta.friendlyScoreLost == 300,
              "SS2SAVE1 debrief counters round-trip");
        check(dec->snapshot == snap, "SS2SAVE1 inner snapshot round-trips");

        // Corruption checks: empty, bad magic, truncated header, truncated payload, trailing junk, huge string length.
        check(!ss2::decodeSaveFile({}).has_value(), "reject empty buffer");
        auto badMagic = bytes;
        badMagic[0] = 'X';
        check(!ss2::decodeSaveFile(badMagic).has_value(), "reject bad magic");
        check(!ss2::decodeSaveFile(std::span<const std::uint8_t>(bytes.data(), 12)).has_value(),
              "reject truncated header");
        check(!ss2::decodeSaveFile(std::span<const std::uint8_t>(bytes.data(), bytes.size() - 1)).has_value(),
              "reject truncated payload");
        auto trailing = bytes;
        trailing.push_back(0);
        check(!ss2::decodeSaveFile(trailing).has_value(), "reject trailing extra bytes");
        auto hugeLen = bytes;
        hugeLen[8] = 0xffu;
        hugeLen[9] = 0xffu;
        hugeLen[10] = 0xffu;
        hugeLen[11] = 0x00u;
        check(!ss2::decodeSaveFile(hugeLen).has_value(), "reject oversized mission string length");
    }

    // 2. Legacy SS2SNAP3 / SS2SNAP2 / SS2SNAP1 transparent decode.
    {
        const auto snap3Bytes = ss2::TutorialUnits::serializeSnapshot(snap);
        const auto dec3 = ss2::decodeSaveFile(snap3Bytes);
        check(dec3.has_value() && !dec3->meta.hasMetadata && dec3->meta.difficulty == 2 &&
                  dec3->meta.tick == 1425u && dec3->meta.playerSoldiers == 1u &&
                  dec3->meta.playerVehicles == 1u && dec3->snapshot == snap,
              "decodeSaveFile handles legacy raw SS2SNAP3");

        const std::size_t v3ExtSize = snap.soldiers.size() * 4u + snap.vehicles.size() * 5u;
        std::vector<std::uint8_t> snap2Bytes(snap3Bytes.begin(), snap3Bytes.end() - static_cast<std::ptrdiff_t>(v3ExtSize));
        snap2Bytes[7] = '2';
        const auto dec2 = ss2::decodeSaveFile(snap2Bytes);
        check(dec2.has_value() && !dec2->meta.hasMetadata && dec2->meta.tick == 1425u &&
                  dec2->snapshot.airReady[0][0] == 2,
              "decodeSaveFile handles legacy raw SS2SNAP2");

        const std::size_t v2ExtSize = 4u * 5u * 4u;
        std::vector<std::uint8_t> snap1Bytes(snap2Bytes.begin(), snap2Bytes.end() - static_cast<std::ptrdiff_t>(v2ExtSize));
        snap1Bytes[7] = '1';
        const auto dec1 = ss2::decodeSaveFile(snap1Bytes);
        check(dec1.has_value() && !dec1->meta.hasMetadata && dec1->meta.tick == 1425u &&
                  dec1->meta.playerSoldiers == 1u,
              "decodeSaveFile handles legacy raw SS2SNAP1");
    }

    // 3. Disk writeSaveFile / readSaveFile round-trip + mtime fallback on legacy file.
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        TempDirGuard guard{std::filesystem::temp_directory_path() / ("ss2_save_file_test_" + std::to_string(nonce))};
        const auto nestedSave = guard.dir / "sub" / "023000_slot2.ss2save";

        ss2::SaveFileMetadata meta{};
        meta.hasMetadata = true;
        meta.mission = "maps/campaign/023000.ssc";
        meta.missionTitle = "1. Starting Position";
        meta.difficulty = 1;
        meta.savedEpochSec = 1795251234;
        meta.enemyScoreEarned = 900;
        check(ss2::writeSaveFile(nestedSave, meta, snap), "writeSaveFile writes nested file");

        const auto diskDec = ss2::readSaveFile(nestedSave);
        check(diskDec.has_value() && diskDec->meta.hasMetadata &&
                  diskDec->meta.missionTitle == "1. Starting Position" &&
                  diskDec->meta.difficulty == 1 &&
                  diskDec->meta.savedEpochSec == 1795251234 &&
                  diskDec->meta.enemyScoreEarned == 900 &&
                  diskDec->snapshot == snap,
              "readSaveFile reloads written save identically");

        // Legacy file written raw should populate savedEpochSec from filesystem mtime.
        const auto legacyFile = guard.dir / "tutorial1_autosave.ss2save";
        const auto rawSnap = ss2::TutorialUnits::serializeSnapshot(snap);
        {
            std::ofstream out(legacyFile, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(rawSnap.data()), static_cast<std::streamsize>(rawSnap.size()));
            check(static_cast<bool>(out), "write legacy raw snapshot file");
        }
        const auto legacyDisk = ss2::readSaveFile(legacyFile);
        check(legacyDisk.has_value() && !legacyDisk->meta.hasMetadata &&
                  legacyDisk->meta.savedEpochSec > 0 &&
                  !ss2::formatSaveTimestamp(legacyDisk->meta.savedEpochSec).empty(),
              "readSaveFile fills mtime timestamp for legacy raw saves");

        check(!ss2::readSaveFile(guard.dir / "missing.ss2save").has_value(),
              "readSaveFile rejects missing file");
    }

    // 4. Slot label, suffix stripping, and timestamp helpers.
    check(ss2::inferSaveSlotLabel("tutorial1_autosave") == "Auto-Save", "infer Auto-Save");
    check(ss2::inferSaveSlotLabel("--- TUTORIAL I ---_AUTOSAVE") == "Auto-Save", "infer uppercase AUTOSAVE");
    check(ss2::inferSaveSlotLabel("023000_slot3") == "Slot 3", "infer Slot 3");
    check(ss2::inferSaveSlotLabel("023000_SLOT12") == "Slot 12", "infer Slot 12");
    check(ss2::inferSaveSlotLabel("V2") == "Quick Save", "infer Quick Save");
    check(ss2::stripSaveSlotSuffix("tutorial1_autosave") == "tutorial1", "strip _autosave");
    check(ss2::stripSaveSlotSuffix("023000_slot3") == "023000", "strip _slot3");
    check(ss2::stripSaveSlotSuffix("--- TUTORIAL I ---") == "--- TUTORIAL I ---", "keep bare stem");
    check(ss2::formatSaveTimestamp(0).empty(), "epoch 0 yields empty timestamp");
    check(ss2::formatSaveTimestamp(1795250000).size() == 16u, "positive epoch yields YYYY-MM-DD HH:MM");

    std::cout << "save_file_test PASSED\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "save_file_test FAILED: " << e.what() << '\n';
    return 1;
}
