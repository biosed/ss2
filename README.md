# Sudden Strike II native Linux engine

The native Linux executable opens the Tutorial I map from a private Gold 2.2 installation. It draws the terrain, scenery and placed infantry, and lets you inspect individual soldiers. Unit orders, movement, combat, AI and mission execution are still open. The simulation interface accepts a single step independently of display timing.

## Run Tutorial I

```sh
./build/ss2 --game-dir "$HOME/ss2-private/game/app"
```

Left-click infantry to inspect their ID, HP, ammunition, group and owner. Drag a box around your squad to select multiple soldiers. Double-click selects matching infantry in view. Ctrl-double-click extends that selection across the map, and Shift toggles units. Tab focuses the next soldier. Right-drag, middle-drag or use the arrow keys to pan. The wheel zooms, Home fits the map, 1 restores one map pixel per drawable pixel, and Escape closes the window.

For interactive runs, `./build/ss2` also checks `SS2_GAME_DIR` and then `$XDG_DATA_HOME/ss2/game` (default `~/.local/share/ss2/game`). A directory or symlink there can point to the privately extracted game. A launch without game data still provides the original renderer smoke test. `--frames` and `--timing` only load retail data when a path is supplied explicitly.

Use `--mission FILE.ssm` to view an individual single-player mission's embedded overview. The overview is the image stored in the mission file. `--game-dir` reconstructs Tutorial I from individual assets. [Format evidence and remaining gaps](docs/tutorial-map.md)

## Build and run

Use a C++20 toolchain, CMake 3.24+, SDL3 3.2+ development files, zlib 1.2.9+ and libjpeg development files, Vulkan 1.3 headers and loader, and a Vulkan-capable Linux driver. Python 3 runs the asset fixtures when building with tests. On this Apple M1 Pro, the same CMake commands build native ARM64 Linux and SDL selects the Wayland/Honeykrisp path. On Bazzite, build in a development environment with these packages and run in the desktop session with Mesa RADV. On this Mac, run `./build/ss2` from the checkout. To close automatically after 32 frames, run `./build/ss2 --frames 32`. The executable uses the system SDL3 and Vulkan libraries. Tutorial I uses the purchased game data and runs entirely in the Linux executable.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ss2
```

For a bounded smoke run, use `./build/ss2 --frames 120`. For a timing run, select the desired desktop display mode first, then run `./build/ss2 --4k --fullscreen --timing`. Inspect the logged **actual swapchain extent**, device, driver, format and present mode. The timing run warms up for one second and measures at least ten seconds of intervals between successful application present returns. Display scanout timing requires compositor or display instrumentation.

## Validation

Install the Khronos validation layer and configure a build with window tests enabled. A requested validation run fails if the layer is unavailable. SDL's offscreen driver plus Mesa lavapipe provides a headless test path on systems with headless surface support.

```sh
cmake -S . -B build -DSS2_TEST_WINDOW=ON -DSS2_TEST_VALIDATION=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/ss2 --validate --frames 16
```

On the original ARM64 development host the Fedora layer was staged privately. To use that copy, set `VK_LAYER_PATH="$HOME/ss2-private/tools/fedora-aarch64/usr/share/vulkan/explicit_layer.d"` and add `$HOME/ss2-private/tools/fedora-aarch64/usr/lib64` to `LD_LIBRARY_PATH`. The default build requires the layer only when validation is requested.

## Repository layout

| Path | Responsibility |
| --- | --- |
| src/main.cpp | SDL application lifetime, input, presentation loop and optional frame statistics |
| src/assets/ | Read SUE, PCK, mission and tutorial scene assets |
| src/render/ | Vulkan device selection, presentation, swapchain lifecycle and validation callbacks |
| include/ss2/simulation.hpp | Platform-independent step, tick accumulator and snapshot boundary |
| tests/ | Clock behavior, CLI diagnostics, real window resizing and injected WSI recovery |
| tools/ | Private installation inventory and repeatable Ghidra analysis scripts |
| docs/ | Version-pinned upstream audit, original module evidence, validation results and milestone plan |
| .github/workflows/ | x86-64 GCC Release and Clang sanitizer build/test definitions |

The `ss2_sprite` executable runs the [SUE/PCK reference slice](docs/asset-reference.md) against a private game installation. Reserve `src/simulation/` for behavior supported by original observations. Keep private installations, captures, save files, and Ghidra databases in a directory outside this project.

See the [validation record](docs/validation.md), [source audit](docs/source-audit.md), [original binary workflow](docs/reverse-engineering.md) and [implementation plan](docs/implementation-plan.md).
