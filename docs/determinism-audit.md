# Cross-ISA Simulation Determinism Audit (`src/sim/`)

Audited on 22 November 2026 ahead of native-to-native lockstep work. All findings below cover the entire `ss2_sim` library (`CMakeLists.txt` target `ss2_sim`).

## Static audit of non-determinism vectors

1. **Containers, iteration order, and addresses**:
   - Zero `std::unordered_map`, `std::unordered_set`, `std::hash`, or `std::random_device` containers/calls exist inside `src/sim/`. All entity tables use dense index-addressed vectors/deques (`soldiers_`, `vehicles_`, `buildings_`, `sorties_`), `lockstep.{hpp,cpp}` uses ordered `std::map`/`std::vector` state without pointers or wall clocks, and `TutorialUnits::stateHash()` hashes canonical little-endian serialized state (`src/sim/state_hash.hpp`).
   - `MissionRunner::run` uses `std::chrono::steady_clock` strictly for real-time 25 Hz pacing when running its background worker thread in non-manual, non-lockstep single-player mode; `ManualClock`, replay, and lockstep stepping never consult wall clocks inside simulation state or `stateHash()`.
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

Tested after hardening and lockstep integration across four compiler/libc/ISA targets (`-ffp-contract=off`), plus an active x86-64-v3 FMA negative-control probe (`-ffp-contract=off` vs `-ffp-contract=fast`):
- **ARM64 GCC** (`g++ -O3`, glibc aarch64)
- **ARM64 Clang 22.1.8** (`clang++ -O2`, glibc aarch64)
- **x86-64 Zig / Clang** (`zig c++ -target x86_64-linux-musl -O2`, musl libc, executed under `qemu-x86_64-static`)
- **x86-64-v3 Zig / Clang** (`zig c++ -target x86_64-linux-musl -mcpu=x86_64_v3 -O2`, FMA-capable musl libc, executed under `qemu-x86_64-static`)

Observed bit-identical outputs on all four targets (and active FMA probe separation):
- `combat_parity_test`: `combat_parity: PASS (120 ticks x 13 fields, 21 events, final_rng=0xe06870c1, 550 regression rows)`
- `sim_determinism_test`: `SIM_DETERMINISM_HASHES t0=0x30877be0aac2d46d t125=0x68bbbf2d7f830c31 t250=0x890bb188bb2b3a64`
- `lockstep_test` (250-tick twin-runner multi-seat determinism + live loopback TCP): `LOCKSTEP_DETERMINISM_HASH 0x2c8728cb9cdbb7f0 verifiedTick=246` (updated from `0xdb1c5caa1787fcc7` after fixing `TutorialUnits::isHostile(3, 0/1)` under `multiplayerSymmetry_` so active Seat 4 cannon muzzle-flash spotting bits `0x08` are set symmetrically without altering single-player)
- `lockstep_test` (skirmish `OP(52/53/54/56)` nation waves, Zeppelin capture/contest, anti-forged `cmd::Resign`): `LOCKSTEP_SKIRMISH_HASH 0x653b9a007be75c29`
- `lockstep_test` (4-player `2v2` `StarRelayTransport` lockstep session with team-only chat/beacon isolation, shared allied vision, Zeppelin team capture, and partial-resign continuation): `LOCKSTEP_4P_2V2_HASH 0x6b4089b2cbf8ac84`
- x86-64-v3 FMA negative-control probe: `0x6610475e5345ba17` strict (`-ffp-contract=off` on ARM64 GCC and x86-64-v3) vs `0x5c3022d25bc09199` contracted (`-ffp-contract=fast` on x86-64-v3)
- Full Tutorial I P[7] route replay (`gold_tutorial_replay`): `10 dialogs, 12 commands, 3862 ticks, stateHash 0x1627b0e8d45be6ea PASSED`
