#pragma once
#include "sim/tutorial.hpp"
#include "sim/mission_runner.hpp"
#include "sim/tutorial_units.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ss2::vm_test {

using Event = ss2::MissionEvent;

inline std::pair<int, int> tc(int tx, int ty) {
    return {4096 + (tx - ty) * 32, (tx + ty) * 16 + 16};
}

inline std::vector<std::uint32_t> emptyFlags128() {
    return std::vector<std::uint32_t>(128u * 128u, 0u);
}

inline std::vector<std::uint16_t> emptyObstacles128() {
    return std::vector<std::uint16_t>(128u * 128u, 0u);
}

inline ss2::assets::UnitSpec makePhase4ETigerSpec() {
    ss2::assets::UnitSpec spec{};
    spec.id = "tiger";
    spec.isVehicle = true;
    spec.health = 800;
    spec.ammo1 = 30;
    spec.moveSpeed = 1.5;
    spec.sight = 16;
    spec.shot1 = {"ap88", ss2::assets::HitType::Pierce, 220, 220, 750, 750, 0, 15, 1, 15, "gunshot"};
    return spec;
}

inline std::shared_ptr<const ss2::assets::UnitCatalog> makePhase4ECatalog() {
    ss2::assets::UnitSpec flakSpec{};
    flakSpec.id = "88mmflak";
    flakSpec.isVehicle = true;
    flakSpec.roleClass = "aa";
    flakSpec.health = 220;
    flakSpec.ammo1 = 40;
    flakSpec.moveSpeed = 0.0;
    flakSpec.sight = 14;
    flakSpec.shot1 = {"aa88", ss2::assets::HitType::Explosive, 120, 120, 600, 600, 0, 20, 1, 20, "gunshot"};

    ss2::assets::UnitSpec carSpec{};
    carSpec.id = "kubel";
    carSpec.isVehicle = true;
    carSpec.health = 180;
    carSpec.moveSpeed = 1.6;
    carSpec.sight = 10;

    ss2::assets::UnitSpec pTankSpec = makePhase4ETigerSpec();

    ss2::assets::UnitSpec eTankSpec{};
    eTankSpec.id = "t-34";
    eTankSpec.isVehicle = true;
    eTankSpec.health = 600;
    eTankSpec.ammo1 = 30;
    eTankSpec.moveSpeed = 1.6;
    eTankSpec.sight = 8; // 256px sight < 430px attack distance
    eTankSpec.shot1 = {"ap76", ss2::assets::HitType::Pierce, 140, 140, 450, 450, 0, 20, 1, 20, "tankgun"};

    return ss2::assets::UnitCatalog::fromSpecs({flakSpec, carSpec, pTankSpec, eTankSpec});
}

void runRetailChecks(const std::filesystem::path& gameDir);
int runCoreAnd4Ato4E();
void runPhase4Fto4O();
void runPhase4Pto4W();
void runPhase4Xto5B();
void runPhase5Cto5F();
void runPhase5Gto5J();
void runPhase5Kto5O();
void runPhase5Pto5U();
void runCaofCompleteOpcodes();
void runRetailCaofCorpusCheck(const std::filesystem::path& gameDir);

}  // namespace ss2::vm_test
