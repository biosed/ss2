# Cross-ISA Simulation Determinism Audit (`src/sim/`)

Audited on 22 November 2026 ahead of native-to-native lockstep work. All findings below cover the entire `ss2_sim` library (`CMakeLists.txt` target `ss2_sim`).

## Static audit of non-determinism vectors

1. **Containers, iteration order, and addresses**:
   - Zero `std::unordered_map`, `std::unordered_set`, `std::hash`, wall clocks (`std::chrono::steady_clock` / `system_clock`), `std::random_device`, or pointer-ordered iteration exist inside `src/sim/`. All entity tables use dense index-addressed vectors/deques (`soldiers_`, `vehicles_`, `buildings_`, `sorties_`), and `TutorialUnits::stateHash()` hashes canonical little-endian serialized state without pointers (`src/sim/state_hash.hpp`).
   - All PRNG state (`MissionRunner::random_`, `TutorialUnits::randomState_`, `TutorialUnits::combatRandomState_`, `CombatWorld::rngState_`) is deterministically seeded and explicitly included in `stateHash()`.
2. **Compiler FMA contraction (`SS2_STRICT_FP`)**:
   - `CMakeLists.txt` adds `SS2_STRICT_FP` (`-ffp-contract=off` on GCC/Clang, `/fp:precise` on MSVC) to `ss2_sim` and `sim_determinism_test`, preventing ARM64/x86-64 AVX2/FMA instruction fusion (`fmadd`) from silently changing intermediate `double` rounding versus non-FMA targets.
3. **`StateHasher` floating-point canonicalization**:
   - `src/sim/state_hash.hpp` normalizes `-0.0f` / `-0.0` to `+0.0f` / `+0.0` and folds all `NaN` payloads to a single canonical quiet `NaN` bit pattern (`0x7fc00000u` / `0x7ff8000000000000ull`) before FNV-1a hashing.
4. **Replacement of platform `libm` calls (`src/sim/det_math.hpp`)**:
   - Replaced all 152 `std::hypot`, `std::lround`, `std::llround`, `std::sin`, `std::cos`, and `std::atan2` call sites across `src/sim/` (`infantry_combat.cpp`, `tutorial_units_internal.hpp`, `tutorial_units*.cpp`) with deterministic primitives in `src/sim/det_math.hpp`:
     - `det::detHypot`: IEEE-754 `std::sqrt(dx * dx + dy * dy)` (correctly rounded to 0.5 ULP in hardware on ARM64 `fsqrt` and x86-64 `sqrtsd`, overflow-safe scaling above `1e150`), avoiding platform `libm` `hypot` differences.
     - `det::detRoundToInt64` / `det::detRoundToInt`: exact IEEE-754 `std::trunc`-based half-away-from-zero rounding to fixed-width `int64_t` / `int`, avoiding `long` ABI width differences and `x + 0.5` double rounding.
     - `det::detSinCos`: Cody-Waite 3-part `pi/2` range reduction and Horner degree-15/16 Taylor evaluation on `[-pi/4, pi/4]` (max error `< 1e-15` across all 32,768 scatter angles, zero `libm` transcendental calls).
     - `det::detDir8` and `det::detDir32`: exact 64-bit integer and 4-constant `tan(k * pi / 32)` sector comparisons matching the original 8-way and 32-way angle quantizers without `std::atan2`.
   - Remaining IEEE-754 operations in `src/sim/`: hardware `+`, `-`, `*`, `/`, `<cmath>` `std::sqrt`, `std::trunc`, `std::floor` (`tutorial_coordinates.hpp` comment only — actual integer code uses bit/arithmetic ops), and `std::ceil` (`flightTicks` integer-ish division in `infantry_combat.cpp` / `tutorial_units_combat.cpp`), all single-instruction IEEE-754 rounded on ARMv8 and x86-64 SSE2.
   - Audit grep confirmation: `grep -rn -E '\b(sin|cos|tan|atan2|atan|asin|acos|hypot|lround|llround|pow|exp|log|fmod)\s*\(' src/sim/ | grep -v 'det_math.hpp'` returns zero matches.

## Cross-ISA verification (`tools/check-cross-isa-sim.sh`)

Tested after hardening across three compiler/libc/ISA triples (`-ffp-contract=off`):
- **ARM64 GCC 16** (`g++ -O3`, glibc aarch64)
- **ARM64 Clang 22.1.8** (`clang++ -O2`, glibc aarch64)
- **x86-64 Zig / Clang** (`zig c++ -target x86_64-linux-musl -O2`, musl libc, executed under `qemu-x86_64-static`)

Observed bit-identical outputs on all three:
- `combat_parity_test`: `combat_parity: PASS (120 ticks x 13 fields, 21 events, final_rng=0xe06870c1, 550 regression rows)`
- `sim_determinism_test`: `SIM_DETERMINISM_HASHES t0=0xe8ec69fe9ef2f3d5 t125=0x1c957edf9fac66ef t250=0x5386e9986a328979`
- Full Tutorial I P[7] route replay (`gold_tutorial_replay`): `10 dialogs, 12 commands, 3734 ticks, stateHash 0xa596a2f447164378 PASSED`
