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

The first bounded SUE/PCK comparison passed on three retail sprite frames. The next unresolved reference is the original simulation clock and state progression. [Asset results](asset-reference.md) Run the RADV hardware test when the BC-250 is available. The project has no configured Git remote or hosted CI result.


Tutorial I now provides a native visual reference and infantry picking. The next gameplay milestone is one original move order. Identify the retail order handler, establish the tick source and capture a starting unit state, destination and successive positions. That trace will provide an acceptance test for the first command under multiple render schedules. The current map inspector leaves all mission state at its recorded starting values. [Tutorial I evidence](tutorial-map.md)
