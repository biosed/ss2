#!/usr/bin/env bash
set -euo pipefail

# Build and run sim_determinism_test and combat_parity_test across:
#   1) Native ARM64 GCC
#   2) Native ARM64 Clang
#   3) Cross-compiled baseline x86-64 (zig c++ -target x86_64-linux-musl) under qemu-x86_64-static
#   4) Cross-compiled FMA-capable x86-64-v3 (zig c++ -target x86_64-linux-musl -mcpu=x86_64_v3)
# Plus an explicit x86-64-v3 FMA negative-control probe verifying -ffp-contract=off
# actively prevents FMA contraction divergence relative to -ffp-contract=fast.
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
std::optional<Bytes> tryReadSueEntry(const std::filesystem::path&, std::string_view) {
    return std::nullopt;
}
std::optional<Bytes> tryReadSueEntryChain(std::span<const std::filesystem::path>, std::string_view) {
    return std::nullopt;
}
Bytes readSueEntryChain(std::span<const std::filesystem::path>, std::string_view) {
    throw std::runtime_error("stub readSueEntryChain");
}
std::vector<std::filesystem::path> resolveGameArchives(const std::filesystem::path&) {
    return {};
}
std::vector<std::filesystem::path> resolveDescArchives(const std::filesystem::path&, unsigned) {
    return {};
}
std::vector<std::filesystem::path> resolveLangArchives(const std::filesystem::path&, unsigned) {
    return {};
}
std::optional<unsigned> resolveIniTurnDelay(const std::filesystem::path&) {
    return std::nullopt;
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
    "$ROOT/src/sim/lockstep.cpp"
    "$ROOT/src/sim/mission_runner.cpp"
    "$ROOT/src/sim/infantry_step.cpp"
    "$ROOT/src/sim/tutorial_units.cpp"
    "$ROOT/src/sim/tutorial_units_ai.cpp"
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
    "${cxx[@]}" -std=c++20 -ffp-contract=off -Wall -Wextra -Wpedantic -UNDEBUG \
        -I"$ROOT/src" "${SIM_SRCS[@]}" "$ROOT/src/net/tcp_transport.cpp" "$ROOT/tests/lockstep_test.cpp" \
        -lpthread -o "$TMP/lockstep_$label"

    echo "=== Running $label ==="
    local out lock_out
    if [ -n "$runner" ]; then
        out="$("$runner" "$TMP/sim_det_$label")"
        "$runner" "$TMP/combat_$label" "$ROOT/tests/combat_parity_golden.txt"
        lock_out="$("$runner" "$TMP/lockstep_$label")"
    else
        out="$("$TMP/sim_det_$label")"
        "$TMP/combat_$label" "$ROOT/tests/combat_parity_golden.txt"
        lock_out="$("$TMP/lockstep_$label")"
    fi
    printf '%s\n' "$out"
    printf '%s\n' "$lock_out"
    printf '%s\n' "$out" | grep '^SIM_DETERMINISM_HASHES ' > "$TMP/hash_$label"
    printf '%s\n' "$lock_out" | grep -E '^(LOCKSTEP_DETERMINISM_HASH|LOCKSTEP_SKIRMISH_HASH|LOCKSTEP_4P_2V2_HASH) ' > "$TMP/lockhash_$label"
}

run_toolchain "arm64_gcc" "" g++ -O3
run_toolchain "arm64_clang" "" clang++ -O2
run_toolchain "x86_64_musl" "qemu-x86_64-static" zig c++ -target x86_64-linux-musl -O2
run_toolchain "x86_64_v3_musl" "qemu-x86_64-static" zig c++ -target x86_64-linux-musl -mcpu=x86_64_v3 -O2

h_gcc="$(cat "$TMP/hash_arm64_gcc")"
h_clang="$(cat "$TMP/hash_arm64_clang")"
h_x86="$(cat "$TMP/hash_x86_64_musl")"
h_x86_v3="$(cat "$TMP/hash_x86_64_v3_musl")"
lh_gcc="$(cat "$TMP/lockhash_arm64_gcc")"
lh_clang="$(cat "$TMP/lockhash_arm64_clang")"
lh_x86="$(cat "$TMP/lockhash_x86_64_musl")"
lh_x86_v3="$(cat "$TMP/lockhash_x86_64_v3_musl")"

echo "=== Cross-ISA Comparison ==="
echo "ARM64 GCC     : $h_gcc"
echo "ARM64 Clang   : $h_clang"
echo "x86-64 Zig    : $h_x86"
echo "x86-64-v3 Zig : $h_x86_v3"
echo "Lockstep GCC  : $lh_gcc"
echo "Lockstep Clang: $lh_clang"
echo "Lockstep x86  : $lh_x86"
echo "Lockstep v3   : $lh_x86_v3"

if [ "$h_gcc" != "$h_clang" ] || [ "$h_gcc" != "$h_x86" ] || [ "$h_gcc" != "$h_x86_v3" ]; then
    echo "ERROR: Cross-ISA simulation stateHash mismatch!" >&2
    exit 1
fi
if [ "$lh_gcc" != "$lh_clang" ] || [ "$lh_gcc" != "$lh_x86" ] || [ "$lh_gcc" != "$lh_x86_v3" ]; then
    echo "ERROR: Cross-ISA lockstep stateHash mismatch!" >&2
    exit 1
fi

cat > "$TMP/fma_probe.cpp" << 'EOF'
#include "sim/det_math.hpp"
#include "sim/state_hash.hpp"
#include <cstdint>
#include <iostream>

int main() {
    ss2::StateHasher h;
    for (int i = 0; i < 2048; ++i) {
        const double angle = (static_cast<double>(i) * 6.28318530717958647692) / 2048.0;
        const auto [s, c] = ss2::det::detSinCos(angle);
        h.add(s);
        h.add(c);
        const double dx = s * 137.3 + 0.125;
        const double dy = c * 91.7 - 0.375;
        h.add(ss2::det::detHypot(dx, dy));
    }
    std::cout << "0x" << std::hex << h.value() << '\n';
    return 0;
}
EOF

g++ -std=c++20 -O3 -ffp-contract=off -I"$ROOT/src" "$TMP/fma_probe.cpp" -o "$TMP/fma_arm64_off"
zig c++ -target x86_64-linux-musl -mcpu=x86_64_v3 -O2 -ffp-contract=off -std=c++20 -I"$ROOT/src" "$TMP/fma_probe.cpp" -o "$TMP/fma_x86_v3_off"
zig c++ -target x86_64-linux-musl -mcpu=x86_64_v3 -O2 -ffp-contract=fast -std=c++20 -I"$ROOT/src" "$TMP/fma_probe.cpp" -o "$TMP/fma_x86_v3_fast"

p_arm="$("$TMP/fma_arm64_off")"
p_v3_off="$(qemu-x86_64-static "$TMP/fma_x86_v3_off")"
p_v3_fast="$(qemu-x86_64-static "$TMP/fma_x86_v3_fast")"

echo "=== x86-64-v3 FMA Negative-Control Probe ==="
echo "ARM64 GCC (-ffp-contract=off)     : $p_arm"
echo "x86-64-v3 (-ffp-contract=off)     : $p_v3_off"
echo "x86-64-v3 (-ffp-contract=fast)    : $p_v3_fast"

if [ "$p_arm" != "$p_v3_off" ]; then
    echo "ERROR: x86-64-v3 strict hash diverged from ARM64 GCC!" >&2
    exit 1
fi
if [ "$p_v3_fast" = "$p_v3_off" ]; then
    echo "ERROR: x86-64-v3 -ffp-contract=fast did not trigger FMA contraction divergence!" >&2
    exit 1
fi
echo "Cross-ISA bit-for-bit determinism PASSED"
