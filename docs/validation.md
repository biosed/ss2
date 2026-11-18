# First milestone validation

Runs on 18–19 November 2026 used the Fedora Asahi Remix ARM64 development machine. The x86-64 ELF was cross-built with CMake and Zig's C++ driver, using Linux SDL3 and Vulkan shared libraries. It ran under FEX in a muvm guest with X11 presentation. Emulation was used only to validate that artifact on the ARM machine.

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

## Local desktop timing observation

On KDE Wayland with a display reporting 120 Hz and pixel density 1.5, the compositor granted a 3024×1791 swapchain for the 4K window request. Honeykrisp 26.2.7 used FIFO, four images, B8G8R8A8Srgb and SrgbNonlinear. With validation active the ten-second sample contained 1,196 intervals between successful present returns.

| p50 | p95 | p99 | Maximum | Intervals above 12.5 ms |
| --- | --- | --- | --- | --- |
| 8.335 ms | 8.810 ms | 12.276 ms | 13.666 ms | 6 |

The program clears one color per frame. Application timestamps describe the presentation loop on this desktop. Hardware scanout, sprite workload cost, and BC-250 frame pacing are unmeasured.

## BC-250 run

From the Bazzite desktop with the target mode active, run the validation-enabled CTest suite and `./build/ss2 --4k --fullscreen --validate --frames 120`. Confirm the log identifies RADV and 3840×2160. Record the desktop/compositor, refresh rate and pixel density. Then run `./build/ss2 --4k --fullscreen --timing` in the Release build with validation disabled to measure presentation intervals. Retain the printed format, mode, percentiles and long-interval count, and repeat under representative game content in a later milestone.

## M1 Asahi verification with the asset reader

On 19 November 2026, the ARM64 Wayland/Honeykrisp suite passed 8/8 with the privately staged validation layer. A separate 32-frame smoke run reported zero validation errors. The Clang sanitizer/offscreen suite also passed 8/8. Claude reviewed Asahi portability and found no M1-specific blocker. Gemini independently configured a fresh RelWithDebInfo build, confirmed an AArch64 ELF and visible Honeykrisp presentation, and passed 7/7 tests without validation. Its validation-required run correctly failed the three graphics tests when the staged layer was outside its search path.

The x86-64/FEX X11 suite passed 8/8 on a full rerun. An earlier resize test aborted in Xlib/XCB with a queue sequence assertion, followed by a retry losing the X connection. The intermittent guest-display failure remains unexplained. Native M1 validation passed. Physical BC-250/RADV testing remains pending.

## Native Tutorial I scene

The ARM64 release and Clang ASan/UBSan suites exercised terrain decoding, indexed sprite runs, map upload, WSI recovery, map picking after resize, camera bounds and infantry selection. The selection fixture verifies click, drag, double-click, Ctrl and Shift through SDL events. Vulkan synchronization validation passed on the overlapping selection outlines.

The M1 Pro desktop run used the native aarch64 ELF, SDL's X11 backend on the Plasma Wayland desktop, and Mesa Honeykrisp. The scene contains 1,360 decoded scenery placements and 88 individually placed infantry. All 39 retail single-player mission overviews also decoded and presented in the sanitizer build. The 3840 × 2160 offscreen scene smoke used lavapipe. Neither that smoke nor the M1 panel establishes the physical BC-250's 4K/120 performance.

The rebuilt x86-64 executable presented the tutorial on FEX/X11 with Honeykrisp and zero Vulkan validation errors. The FEX/offscreen lavapipe configuration failed during swapchain allocation with VK_ERROR_OUT_OF_DEVICE_MEMORY. Native ARM64 offscreen lavapipe succeeded. The public repository contains no retail data or captures.
