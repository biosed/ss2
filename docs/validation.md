# Validation record

Runs on 18–19 November 2026 used the Fedora Asahi Remix ARM64 development machine, an Apple M1 Pro. The x86-64 ELF was cross-built with CMake and Zig's C++ driver, using Linux SDL3 and Vulkan shared libraries. It ran under FEX in a muvm guest with X11 presentation. Emulation was used only to validate that artifact on the ARM machine.

This document has two parts. The first part is the current state: what is verified today, what the test suite covers, and which gates remain open. The second part is the dated run log, kept as it was recorded, with the measurements intact. Where a run log entry states something as present tense that has since changed, the entry is left as it was and corrected with a dated note rather than rewritten.

## Current status

Verified on 19 November 2026, ARM64 on the M1 Pro, Mesa Honeykrisp.

| Environment | Result |
| --- | --- |
| ARM64 GCC Release, SDL Wayland and X11, Apple M1 Pro | 23/23 tests pass with the private installation enabled, including every retail-trace test |
| ARM64 Clang Debug, ASan/UBSan, SDL offscreen and lavapipe | 23/23 tests pass with the private installation enabled, no sanitizer diagnostics |
| Hosted CI configuration: no private installation, no window tests | 14/14 tests pass; this is the set a hosted run covers |
| x86-64 Linux Release, FEX/X11, Apple M1 Pro, Mesa Honeykrisp 26.0.3 | Full suite passed on a full rerun. An earlier resize test aborted in Xlib/XCB with a queue sequence assertion, followed by a retry losing the X connection. The intermittent guest-display failure remains unexplained. |
| ARM64, SDL offscreen, lavapipe, 3840×2160 | 120 presented frames, zero validation errors. No display timing inferred from this run. |
| BC-250/Bazzite/RADV, native x86-64, physical 4K 120 Hz display | Pending hardware availability. |
| Hosted GitHub Actions | Workflow defined with two matrix legs. No remote configured and no workflow ever dispatched. |

## Coverage of the combat path

The first movement objective is covered by the movement and order tests in `tests/infantry_path_test.cpp`, `tests/tutorial_orders_test.cpp` and `tests/tutorial_vm_test.cpp`. The original fog of war, spotting and infantry combat are covered by `tests/infantry_combat_test.cpp` and `tests/combat_parity_test.cpp`.

`combat_parity` replays the matched-fields encounter and compares the result against the Gold 2.2 oracle fixture:

- 120 ticks × 13 fields, that is 1560 compared values, plus all 21 discrete events, compared field by field.
- The final RNG state is compared, and the end state of the attacker and the target.
- The asset-free fog grid reproduces the real Tutorial I P-FLAGS and `circ64.dat` trace bit for bit, so the test runs in CI without the private Gold installation.
- Any divergence is reported with the first differing tick and field, and the test exits non-zero.

A second, clearly labelled section of the same table pins native behaviour for the two paths no oracle fixture reaches: a two-sided duel with return fire, and a six-soldier squad that pauses to engage a stationary enemy before resuming its march. That section is a native baseline and not an oracle parity claim. It is regenerated with one command, `combat_parity_test --emit`, when a change to combat timing, auto-engage or pathing is intentional; the regenerated rows must then be called out in the board post.

Mutating the simulation rather than the table confirms that the net bites. In a scratch copy of the tree outside the repository, changing the attacker's reload base produced 414 differences and changing the march-pause gating produced 84, and the pause-gating change left the oracle trace and all 21 events untouched. The second case is the reason the regression section exists.

## Test suite and hosted CI

Thirty tests are registered in `CMakeLists.txt`. Which of them run depends on the configuration:

| Configuration | Tests |
| --- | --- |
| Default | 14 |
| `-DSS2_PRIVATE_GAME=DIR` | 23 |
| `-DSS2_TEST_WINDOW=ON` | 20 |
| `-DSS2_TEST_WINDOW=ON -DSS2_TEST_VALIDATION=ON` | 21 |
| `-DSS2_PRIVATE_GAME=DIR -DSS2_TEST_WINDOW=ON` | 29 |
| All three | 30 |

The nine retail-trace tests need the purchased installation and are skipped without `-DSS2_PRIVATE_GAME`; the private installation is never committed. The six window and WSI tests need a Vulkan WSI surface, and the seventh, the missing-layer test, is only registered when `-DSS2_TEST_VALIDATION=ON` asks for the Khronos layer.

`.github/workflows/linux.yml` defines two matrix legs, `g++` Release and `clang++` Debug with AddressSanitizer and UndefinedBehaviorSanitizer, and runs the plain `ctest --test-dir build --output-on-failure`. Because that configure sets neither `SS2_PRIVATE_GAME` nor `SS2_TEST_WINDOW`, a hosted run covers exactly the fourteen tests that need no private data and no window. The workflow has never been dispatched, because no Git remote is configured for this repository.

The default build requires the Khronos validation layer only when validation is requested. On the ARM64 development host the Fedora layer was staged privately; set `VK_LAYER_PATH="$HOME/ss2-private/tools/fedora-aarch64/usr/share/vulkan/explicit_layer.d"` and add `$HOME/ss2-private/tools/fedora-aarch64/usr/lib64` to `LD_LIBRARY_PATH` to use that copy.

## Hardware acceptance (BC-250)

From the Bazzite desktop with the target mode active, run the validation-enabled CTest suite and `./build/ss2 --4k --fullscreen --validate --frames 120`. Confirm the log identifies RADV and 3840×2160. Record the desktop/compositor, refresh rate and pixel density. Then run `./build/ss2 --4k --fullscreen --timing` in the Release build with validation disabled to measure presentation intervals. Retain the printed format, mode, percentiles and long-interval count, and repeat under representative game content in a later milestone.

## Comparison with the purchased Gold executable

On the M1, the Windows Gold 2.2 executable reached Tutorial I through Single Player, Single Missions and the mission briefing. The reference ran in a private 1024 × 768 Wine virtual desktop under Proton 10 and FEX. The native game ran separately on Asahi Linux. Both used the purchased mission, its installed 36 ms delay, a box selection around the starting formation and a right-button press on the same point on the wreck. The native framebuffer was downsampled by two for the camera comparison. Stationary map features aligned within three original pixels. The reference installation for this run listed only Tutorial I in the Single Missions screen. The mission file and executable were unchanged.

Both showed "Good." and then the fields briefing. In this paired run Gold first showed "Good." about 73 seconds after the order. The native build showed it at 64.7 seconds. This wall-clock comparison includes the Wine/FEX reference host, the original simulation and combat, and the different renderers. It does not isolate the cause of the timing gap. Controlled tests of the opening movement routines still match all 92,800 sampled positions. Under the original fog of war the nearby enemy infantry became visible when the squad approached.

The cause was later separated into two parts. A pure wall-clock stretch of about 1.10 times was measured in the Gold run: 39.5 ms per tick against the native 36 ms, caused by poll lateness in the original scheduler under Wine/FEX. The remaining difference was a real simulation divergence, not a uniform stretch. Gold stopped the marching squad and killed the four Group-2 enemies lying in the road in front of the wreck, because the original fog of war and spotting were active; native walked past unimpeded.

**Correction dated 19 November 2026.** The statement in the run log below that the native game shows the enemy before the squad approaches, and that original firing, damage and spotting remain gameplay differences, described the engine at the time of that run. The native engine has since implemented the original fog of war and the Gold 2.2 spotting rules, infantry combat in both directions and the Group-3 elimination phrase, and the matched encounter reproduces the oracle tick by tick. A fresh Gold-versus-native capture of the same march has not been repeated since those changes, so the 73 second against 64.7 second gap is not re-measured and the two figures above remain the record of that run.

The paired capture also identified an incorrect intact truck in the earlier optional HD pack. The updated private pack renders Gold's STAND 166 wreck silhouette at its original anchor. It adds Gemini-generated road, soil, grass and field detail. The packed map keeps the original rhomb layout, lighting inputs and collision data. Both HD builds issued the same squad order and reached the fields briefing at the same installed simulation speed.

## Playback without desktop windows

On Linux, SDL's offscreen driver runs the CTest suite without showing windows.

```sh
SDL_VIDEO_DRIVER=offscreen ctest --test-dir build --output-on-failure
```

For a real pointer-and-keyboard replay, use an isolated display. This example requires Xvfb with the xvfb-run wrapper, xdotool, ImageMagick, the Vulkan validation layer and a Vulkan driver that supports Xvfb presentation, such as Mesa lavapipe. The replay uses the installed mission clock and records screenshots, order coordinates and mission callbacks in a fresh directory.

```sh
results=$(mktemp -d)
xvfb-run -a -s "-screen 0 1920x1080x24 -nolisten tcp" \
  python3 tools/playtest_tutorial_x11.py --engine build/ss2 \
  --game "$HOME/.local/share/ss2/game" --scenario wreck-body --selection squad \
  --size 1920x1080 --marker-navigation --pause-order --output "$results/wreck"
```

The driver also accepts `--fields` to march all 29 soldiers to the fields and inspect the group 3 combat objective, `--probe-attack` to inspect the first enemy interaction after the objective, `--picking` for click-target tests near the wreck, `--original` for the original infantry size and `--hd DIR` for a private HD pack.

## Timing measurements

On KDE Wayland with a display reporting 120 Hz and pixel density 1.5, the compositor granted a 3024×1791 swapchain for the 4K window request. Honeykrisp 26.2.7 used FIFO, four images, B8G8R8A8Srgb and SrgbNonlinear. With validation active the ten-second sample contained 1,196 intervals between successful present returns.

| p50 | p95 | p99 | Maximum | Intervals above 12.5 ms |
| --- | --- | --- | --- | --- |
| 8.335 ms | 8.810 ms | 12.276 ms | 13.666 ms | 6 |

The program clears one color per frame. Application timestamps describe the presentation loop on this desktop. Hardware scanout, sprite workload cost, and BC-250 frame pacing are unmeasured.

During the march between order acceptance and the "Good." message, two 3840×2160 FIFO runs on M1 Pro/Honeykrisp averaged 117.9 presented frames/s with validation and 117.7 without. The median time between successful returns from `vkQueuePresentKHR` was 8.34 ms and 8.33 ms respectively. The 95th percentiles were 12.13 ms and 12.23 ms. These measurements include the active painted infantry, selection overlays and normal gameplay loop in a nested Xwayland window. They characterize application presentation on this machine. A sustained 120 FPS and physical BC-250 scanout remain unverified.

A native Wayland replay used the physical M1 render node and a private 3840 × 2160 output configured for 120 Hz. During the 63.6-second march, the release build averaged 123.0 successful Vulkan presents/s with an 8.26 ms 95th percentile. A validated run averaged 121.6 with an 8.52 ms 95th percentile. The headless output supplied the clock, and these figures measure application presentation. A physical BC-250 display test remains outstanding.

## Run log

The entries below are the dated runs as they were recorded. Each is a statement about the engine at the time of that run.

### First milestone validation

| Executable and environment | Result |
| --- | --- |
| ARM64 GCC Release, SDL Wayland, Apple M1 Pro, Mesa Honeykrisp 26.2.7 | 7/7 CTest passed with validation requested. Visible frames and two requested size changes presented. |
| ARM64 GCC Release, SDL X11, Apple M1 Pro | 7/7 passed, including a window manager-acknowledged minimize and restore. |
| ARM64 Clang Debug, ASan/UBSan, SDL offscreen and lavapipe | 7/7 passed with the Khronos validation layer, with no sanitizer diagnostics. |
| x86-64 Linux Release, FEX/X11, Apple M1 Pro, Mesa Honeykrisp 26.0.3 | 7/7 passed with validation requested. The resize test observed minimize/restore. An earlier QEMU-user run failed swapchain allocation. The FEX run exercised the full executable with working WSI. |
| ARM64, SDL offscreen, lavapipe, 3840×2160 | 120 presented frames, zero validation errors. No display timing inferred from this run. |
| BC-250/Bazzite/RADV, native x86-64, physical 4K 120 Hz display | Pending hardware availability. |
| Hosted GitHub Actions | Workflow defined. No remote configured or workflow dispatched. |

The seven tests cover simulation time partitioning, CLI help, an invalid argument, a deliberately missing validation layer, basic presentation, resize, and recovery from WSI errors. The Vulkan tests enable the layer and count error-severity callback messages through teardown. For the missing-layer test, success means that the child program refused to run. The resize test compares the actual swapchain extent against SDL's pixel extent, and reports a skip if the compositor forces its own window size. No tests were skipped in the runs above.

The recovery test injects a zero-sized surface and out-of-date acquire and present results at the Vulkan API boundary in the test executable. It requires a subsequent successful presentation after each fault. These three cases were exercised by fixtures. The ordinary presentation, resize, minimize and restore observations above were live.

### M1 Asahi verification with the asset reader

On 19 November 2026, the ARM64 Wayland/Honeykrisp suite passed 8/8 with the privately staged validation layer. A separate 32-frame smoke run reported zero validation errors. The Clang sanitizer/offscreen suite also passed 8/8. Claude reviewed Asahi portability and found no M1-specific blocker. Gemini independently configured a fresh RelWithDebInfo build, confirmed an AArch64 ELF and visible Honeykrisp presentation, and passed 7/7 tests without validation. Its validation-required run correctly failed the three graphics tests when the staged layer was outside its search path.

The x86-64/FEX X11 suite passed 8/8 on a full rerun. An earlier resize test aborted in Xlib/XCB with a queue sequence assertion, followed by a retry losing the X connection. The intermittent guest-display failure remains unexplained. Native M1 validation passed. Physical BC-250/RADV testing remains pending.

### Native Tutorial I scene

The ARM64 release and Clang ASan/UBSan suites exercised terrain decoding, indexed sprite runs, map upload, WSI recovery, map picking after resize, camera bounds and infantry selection. The selection fixture verifies click, drag, double-click, Ctrl and Shift through SDL events. Vulkan synchronization validation passed on the overlapping selection outlines.

The M1 Pro desktop run used the native aarch64 ELF, SDL's X11 backend on the Plasma Wayland desktop, and Mesa Honeykrisp. The scene contains 1,360 decoded scenery placements, 88 infantry and eight tanks and guns. All 39 retail single-player mission overviews also decoded and presented in the sanitizer build. The 3840 × 2160 offscreen scene smoke used lavapipe. Neither that smoke nor the M1 panel establishes the physical BC-250's 4K/120 performance.

The rebuilt x86-64 executable presented the tutorial on FEX/X11 with Honeykrisp and zero Vulkan validation errors. The FEX/offscreen lavapipe configuration failed during swapchain allocation with `VK_ERROR_OUT_OF_DEVICE_MEMORY`. Native ARM64 offscreen lavapipe succeeded. The public repository contains no retail data or captures.

### First movement objective

The earlier native M1 build, Clang ASan/UBSan build and x86-64/FEX build exercised 18 tests with the private Tutorial I installation enabled. These cover the opening bytecode, accepted infantry steps, the two movement orders, final zone counts, control groups, briefing panels and Vulkan map-region readback. The rendered mission contains 1,368 nonempty scenery placements, including the damaged vehicle that marks the first objective.

A real-time SDL input run used the installed 36 ms delay, two left-click selections and two right-click orders. It reached the original success callback and next briefing with zero Vulkan validation errors. A separate run used integer mouse coordinates and a faster test profile. The routes, input coordinates, checks against the original x86 routines and remaining simulation gaps are recorded in the [playable opening](tutorial-simulation.md). The hardware acceptance target remains 3840 × 2160 at 120 Hz on a BC-250 with RADV.

#### Reproducing the abandoned-vehicle report

The original clear-ground control missed two player actions. Right-clicking the vehicle with a single front-rank soldier required a detour, and right-clicking it with a box-selected squad required group destination assignment. Both failed with normal X11 input before the fixes. After the fixes the 29-soldier selection accepted the wreck order and triggered both the original success phrase and the next briefing at the installed clock speed, with zero Vulkan validation errors. Separate runs exercised the body of the sprite and a 2419 × 1360 window. Two soldiers inside location 2 satisfy the script. This test does not measure arrival of the whole squad. [Reproduction and evidence](tutorial-simulation.md#first-playable-objective)

The movement test also samples the whole convoy. All 29 reach the marked area and their assigned tiles. Native coordinates match all 92,800 coordinate pairs from the isolated original movement callbacks in the fixed-order, fixed-seed case. Window tests separately cover recalling a saved squad, zooming and panning before clicking the wreck, and redirecting a squad already walking toward another destination. The 2419 × 1360 HD test completed both mission messages at the installed speed with zero Vulkan validation errors. [Convoy replay and scope](tutorial-simulation.md#formation-and-route-evidence)

With the convoy and formation regressions enabled, all 23 private-data tests passed on native AArch64 Release, native Clang ASan/UBSan with Vulkan synchronization validation, and the x86-64 Linux build under FEX/X11. The silicon and Linux GPU driver in the FEX run were still the M1 Pro and Honeykrisp. The simulation translation units also cross-compiled to Windows x86-64 COFF and macOS ARM64 Mach-O objects. Full Windows and macOS executables remain untested.

A separate input replay ran in a dedicated Xwayland window using the physical M1 Pro and Honeykrisp. At 1600 × 900 it loaded all 88 painted sprites, recalled a saved squad, zoomed and panned, redirected all 29 soldiers mid-walk to the body of the wreck, and reached the combat briefing. The Vulkan layer reported zero errors. Another window test split the eight submachine gunners from the other 21, sent them to the objective, then recalled and issued a second order to all 29 from their dispersed positions.

The marker-navigation test centers the selected squad with Tab, returns to the original marker with M and clicks the vehicle in the zoomed view. A separate test issues orders while paused, compares two captures of the map for identical pixels, resumes with Ctrl+P and requires both mission callbacks.

### November 19 gameplay checks

On the M1 Pro a native Vulkan window completed Tutorial I's first objective on Honeykrisp using both X11 and Wayland. At 3840×2160 on X11, one right-click on the wreck body was accepted by all 29 selected soldiers. The original script showed both subsequent phrases with zero validation errors. On Wayland, real pointer drags and right-clicks delivered through a dedicated Sway seat completed the same objective at 100% and 150% display scale. From the full-map view, the marker shortcut now zooms in before centering. The click at 150% landed at map pixel (2959,1759), within one original pixel of the intended (2960,1760).

A 34-minute continuous HD session completed the objective, then accepted 32 further orders for the same 29 soldiers. The native process reported zero Vulkan validation errors at exit. The x86-64 suite also passed 24/24 in FEX using offscreen presentation and on a separate guest X server. The latter exposed a device-selection error on systems with multiple Vulkan drivers. The renderer now confirms that a queue can present to the surface before querying its formats or capabilities. Running through the forwarded guest X11 display occasionally produced an XCB sequence error in the selection test. Dedicated guest X11 and offscreen runs of that test passed five consecutive times each. BC-250/RADV performance requires the physical machine.

On a Wayland compositor that accepts only shared-memory buffers, the M1 GPU reported a present-capable queue for the surface but failed the capability query with `VK_ERROR_SURFACE_LOST_KHR`. The SDL presentation check for that same device and queue returned false. Device selection now checks both the active SDL display and the Vulkan surface before querying capabilities. With both drivers installed, the same compositor then presented 32 validated frames using llvmpipe. The private hardware test compositor uses the M1 render node and a headless output, so its gameplay windows never appear on the desktop.

At 125% Wayland scaling on the private M1 output, a floating 760 × 430 logical window at (156,80) landed the right-click on the wreck at original pixel (2960,1760). All 29 accepted. F11 during the march switched to 1280 × 720 logical fullscreen. A second press restored the original size, position and selection, and the squad completed the objective with zero validation errors.

A second installed-speed replay continued from the wreck to the fields. It inspected an enemy soldier with group 3, owner 1, 100 HP and 100 ammunition, recalled the 29 infantry, issued a second order to (3600,1600) and box-selected all 29 inside the fields approach. Right-clicking the enemy displayed the combat limitation. The mission remained at phrase 5 and Vulkan validation reported zero errors. Run the X11 replay with `--selection squad --fields` to check this sequence.

**Correction dated 19 November 2026.** The two paragraphs above describe the engine before infantry combat existed. Right-clicking the enemy now issues an attack order on a spotted enemy; a live check on the private display accepted an attack on unit 12, eliminated it and reached phrase 5. The mission still holds at phrase 5 for a target that is not spotted, which is the intended behaviour.

The x86-64 Linux executable also completed the wreck objective under FEX on the M1, on an isolated guest X server using lavapipe. At the installed 36 ms turn delay, a right-click while paused issued the order for all 29. The captured map stayed frozen until Ctrl+P. The native mission delivered phrases 4 and 5 and validation reported zero errors. A second run selected all 29 at the fields and confirmed the group 3 combat boundary. The guest test used a line-buffering helper built for x86-64 to capture live output from the emulated executable.

A native Wayland replay used the physical M1 render node and a private 3840 × 2160 output configured for 120 Hz. During the 63.6-second march, the release build averaged 123.0 successful Vulkan presents/s with an 8.26 ms 95th percentile. A validated run averaged 121.6 with an 8.52 ms 95th percentile. The headless output supplied the clock, and these figures measure application presentation. A physical BC-250 display test remains outstanding.
