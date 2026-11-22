# Implementation plan after the platform review

Codex and Claude agreed on the platform milestone on 18 November 2026. Claude reviewed the renderer, build, clock tests and CI, and Codex owns the follow-up implementation when work resumes. Claude declined the game-specific review. The original-game investigation below is Codex's plan. The review is recorded on the shared ss2 board in messages 2–6.

## First milestone — a verifiable Linux Vulkan window

The validation-layer gap from the review has been closed. The application now explicitly enables the requested layer and counts validation errors through Vulkan teardown. Repeated resize, WSI recovery, the extended clock fixtures and sanitizer coverage are implemented. See the [validation record](validation.md) for exact environments and pending hardware checks.

| Priority | Agreed work | Acceptance |
| --- | --- | --- |
| 1 | Make Vulkan validation observable in the validation test configuration. Enumerate and explicitly enable the Khronos layer, install a debug-utils callback, and collect validation errors through teardown. | A missing layer fails the validation run with a diagnostic. With the layer active, initialization, presentation, resize and clean exit produce zero validation errors. Ordinary users can run the application without development layers. |
| 2 | Choose a supported surface format and color space deliberately. Log the selected pair and actual swapchain extent. Exercise normal SDL resize event handling. | Repeated resizes report the expected drawable pixel extents and resume presentation. Exercise minimize/restore, zero drawable extent and out-of-date recovery. Record which cases were observed live or driven by a fixture. |
| 3 | Run the platform tests on x86-64. Add a Debug ASan/UBSan CI run alongside the existing build. Tighten the malformed-argument test to require its intended diagnostic. | Record build, CTest, sanitizer and validation results. Record the hosted CI result once a repository and workflow run exist. Check direct standard-library includes and compiler warnings on GCC and Clang. |
| 4 | Extend the generic clock tests using synthetic periods. Compare one long delta against many small deltas, exact tick boundaries, zero elapsed and fractional remainders over long runs. | Each partition of the same elapsed time yields the same step count and remainder. Keep stall behavior explicit. Defer any production policy that drops ticks or limits catch-up until the original's behavior is measured. |
| 5 | Run the visible window on BC-250/Bazzite with RADV and a 120 Hz display mode. Capture at least ten seconds after warm-up. | Record SDL driver, compositor, GPU and driver version, active refresh, actual 3840×2160 extent, format/color space, present mode, and p50/p95/p99/max frame interval. Identify the timing source. Application present-call intervals and measured display presentation intervals answer different questions. Keep hardware acceptance pending until this run. |

The clear-only measurement establishes a presentation baseline. Assess the 8.33 ms frame budget again with representative terrain, sprites and simulation load.

## Original-game investigation — Codex owns this work

1. **Completed the pinned source audit.** Both recursive checkouts and source findings are recorded. [Current audit](source-audit.md)
2. **Establish two independent reference tests.** Trace the hash-identified primary Gold 2.2 game DLL and observe tick advances, input order, pause, speed changes and long frames in the original. In parallel, read one SUE entry and decode one PCK sprite with synthetic fixtures and a private retail pixel comparison. Keep the purchased files and captures outside the repository. [Binary evidence and Ghidra workflow](reverse-engineering.md)
3. **Run the smallest comparable scenario.** Use a recorded input sequence and a single observable state transition. Compare the native and original state at matched ticks across different render schedules, then compare the corresponding scene. Record the first divergent tick or pixel region before expanding to unit behavior, AI and campaigns.

The first bounded SUE/PCK comparison passed on three retail sprite frames. The single-player dispatch clock, opening script sequence and accepted infantry steps now have reference evidence. [Simulation results](tutorial-simulation.md) [Asset results](asset-reference.md) Run the RADV hardware test when the BC-250 is available. The project has no configured Git remote or hosted CI result.


## Current state, 22 November 2026

Tutorial I plays end to end on the native build: the normal-input playthrough reaches the P[7] village briefing with the original orders, phrases and casualties, and the wreck leg matches the Gold reference. Infantry combat, fog of war and Gold 2.2 spotting are implemented and covered by `combat_parity` and the retail spotting rows in `gold_infantry_combat`; Phases 5T–5V aligned `I-MISXINF` per-mission weather (`0/1/2/3`), `0x1f` (`31`-tile) sight caps, Easy `1.3x` vehicle/march sight, `scandelay`, `tanksonarmorprotection` unbuttoned sniper damage (`100%`), and suppression-induced auto-button-up with Gold 2.2 disassembly; and the controlled Gold visibility comparison at 1× zoom and matched ticks (`tools/verify_visibility.py`, `visibility_parity`, `gold_visibility_parity`, `52/52` CTest passing) verified moving `-1` soldier sight, weather-before-Easy ordering, `FogGrid::applyMapPerimeterFlags()`, and negative-delta fog brightness shifts bit-for-bit across all 11 Tutorial I checkpoints (`t=0..3200`) and Group-2 first spotting (`tick 1414`). `src/sim/` is hardened for cross-ISA floating-point determinism (`det_math.hpp`, `-ffp-contract=off`, canonicalized `-0.0`/`NaN` hashing in `StateHasher`, `determinism-audit.md`), verified bit-for-bit identical across ARM64 GCC, ARM64 Clang, and x86-64 Zig/musl via `tools/check-cross-isa-sim.sh` and CTest `sim_determinism`. The Gold front end (`Single Player`, `Campaigns`, `Single Missions`, `Mission Briefing`, `Options`, `Ratings`, `Replay Game`, `Credits`), Bink movies and localized UI are in place. Every simulation order passes through a recorded `cmd::Command`, `MissionRunner` replays command logs on a manual clock verified by a full-state 64-bit simulation hash (`stateHash()`), `--record FILE` and `--replay FILE` are wired in the CLI (`gold_cli_record_replay`), `gold_session_input` verifies headless `GameSession::handleInputEvent` -> `MissionRunner` -> replay parity, `gold_tutorial_replay` verifies the full Tutorial I route through the P[7] village briefing (`3862 ticks, stateHash 0x1627b0e8d45be6ea`), and all three former monoliths (`TutorialUnits`, `GameSession` in `src/session/`, and `tutorial_vm_test`) are split into modular translation units with `CMakePresets.json` (`release`, `validation`, `sanitize`).

Next work, in order:

1. Front End `Load Game` (`.ss2save`) browser and persistent mission/campaign ratings integration.
2. Native-to-native lockstep multiplayer prototype built on deterministic `ss2_sim` (`stateHash()` + `cmd::Command`).
3. Physical BC-250/RADV 4K/120 Hz hardware acceptance.

Physical BC-250/RADV 4K/120 Hz performance remains open. The project still has no Git remote, so the CI workflow has never run. [Tutorial I results](tutorial-simulation.md)
