#!/usr/bin/env bash
set -euo pipefail

# Build and run sim_determinism_test and combat_parity_test across:
#   1) Native ARM64 GCC
#   2) Native ARM64 Clang
#   3) Cross-compiled x86-64 (zig c++ -target x86_64-linux-musl) under qemu-x86_64-static
# Verifies bit-for-bit identical 64-bit simulation stateHash() across compilers and ISAs.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/assets_stub.cpp" << 'EOF'
#include "assets/mission.hpp"
#include "assets/sprite.hpp"
#include <stdexcept>

namespace ss2::assets {
bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto lo = [](unsigned char c) -> unsigned char {
            return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
        };
        if (lo(static_cast<unsigned char>(a[i])) != lo(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}
std::filesystem::path resolveCaseInsensitivePath(const std::filesystem::path& base,
                                                 const std::filesystem::path& relative) {
    return base / relative;
}
Bytes readSueEntry(const std::filesystem::path&, std::string_view) {
    throw std::runtime_error("stub readSueEntry");
}
Bytes readSueEntryFallback(const std::vector<std::filesystem::path>&, std::string_view) {
    throw std::runtime_error("stub readSueEntryFallback");
}
std::string decodeMissionText(std::string_view raw) {
    return std::string(raw);
}
MissionData readMissionData(const std::filesystem::path&) {
    throw std::runtime_error("stub readMissionData");
}
}  // namespace ss2::assets
EOF

SIM_SRCS=(
    "$ROOT/src/assets/unit_catalog.cpp"
    "$TMP/assets_stub.cpp"
    "$ROOT/src/sim/tutorial.cpp"
    "$ROOT/src/sim/command.cpp"
    "$ROOT/src/sim/mission_runner.cpp"
    "$ROOT/src/sim/infantry_step.cpp"
    "$ROOT/src/sim/tutorial_units.cpp"
    "$ROOT/src/sim/tutorial_units_combat.cpp"
    "$ROOT/src/sim/tutorial_units_vehicles.cpp"
    "$ROOT/src/sim/tutorial_units_support.cpp"
    "$ROOT/src/sim/tutorial_units_air.cpp"
    "$ROOT/src/sim/tutorial_units_mines.cpp"
    "$ROOT/src/sim/tutorial_units_buildings.cpp"
    "$ROOT/src/sim/tutorial_units_snapshot.cpp"
    "$ROOT/src/sim/tutorial_units_hash.cpp"
    "$ROOT/src/sim/mission_state_hash.cpp"
    "$ROOT/src/sim/infantry_path.cpp"
    "$ROOT/src/sim/infantry_combat.cpp"
)

run_toolchain() {
    local label="$1"
    local runner="$2"
    shift 2
    local cxx=("$@")
    echo "=== Building $label ==="
    "${cxx[@]}" -std=c++20 -ffp-contract=off -Wall -Wextra -Wpedantic -UNDEBUG \
        -I"$ROOT/src" "${SIM_SRCS[@]}" "$ROOT/tests/sim_determinism_test.cpp" \
        -lpthread -o "$TMP/sim_det_$label"
    "${cxx[@]}" -std=c++20 -ffp-contract=off -Wall -Wextra -Wpedantic -UNDEBUG \
        -I"$ROOT/src" "${SIM_SRCS[@]}" "$ROOT/tests/combat_parity_test.cpp" \
        -lpthread -o "$TMP/combat_$label"

    echo "=== Running $label ==="
    local out
    if [ -n "$runner" ]; then
        out="$("$runner" "$TMP/sim_det_$label")"
        "$runner" "$TMP/combat_$label" "$ROOT/tests/combat_parity_golden.txt"
    else
        out="$("$TMP/sim_det_$label")"
        "$TMP/combat_$label" "$ROOT/tests/combat_parity_golden.txt"
    fi
    printf '%s\n' "$out"
    printf '%s\n' "$out" | grep '^SIM_DETERMINISM_HASHES ' > "$TMP/hash_$label"
}

run_toolchain "arm64_gcc" "" g++ -O3
run_toolchain "arm64_clang" "" clang++ -O2
run_toolchain "x86_64_musl" "qemu-x86_64-static" zig c++ -target x86_64-linux-musl -O2

h_gcc="$(cat "$TMP/hash_arm64_gcc")"
h_clang="$(cat "$TMP/hash_arm64_clang")"
h_x86="$(cat "$TMP/hash_x86_64_musl")"

echo "=== Cross-ISA Comparison ==="
echo "ARM64 GCC  : $h_gcc"
echo "ARM64 Clang: $h_clang"
echo "x86-64 Zig : $h_x86"

if [ "$h_gcc" != "$h_clang" ] || [ "$h_gcc" != "$h_x86" ]; then
    echo "ERROR: Cross-ISA simulation stateHash mismatch!" >&2
    exit 1
fi
echo "Cross-ISA bit-for-bit determinism PASSED"
