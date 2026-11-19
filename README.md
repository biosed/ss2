# Sudden Strike II native Linux engine

A native Linux executable that plays the first movement objective of Tutorial I from a private Gold 2.2 installation. Drag-select the starting squad and right-click the wreck; the original mission script advances when the units reach its marked area. The SDL3/Vulkan renderer and the single-player mission clock run independently of each other.

The native engine now also reproduces the original fog of war, infantry spotting, infantry combat and the Group-3 elimination phrase for that scenario, each pinned by an automated test. Later campaign objectives, the original AI and full parity beyond the matched encounter are still open. [Play the verified opening](docs/tutorial-simulation.md#first-playable-objective)

## Run Tutorial I

```sh
./build/ss2 --game-dir "$HOME/ss2-private/game/app"
```

The tutorial opens in a large window sized to your desktop. F11 switches between fullscreen and windowed play, and `--fullscreen` or `--windowed` selects the same two modes from the command line. Press Enter or click a briefing panel to acknowledge it. After the movement briefing, drag a box around the starting soldiers and right-click the abandoned vehicle. You can also give individual soldiers separate orders. A short message shows how many units accepted the order.

Enemies are hidden until the squad spots them, and they fire back. Right-clicking a spotted enemy issues an attack order. Pass `--no-auto-engage` to disable automatic firing on spotted enemies so that every attack is a manual order.

### Controls

| Input | Effect |
| --- | --- |
| Left-click | Select one soldier and show its ID, HP, ammunition, group and owner |
| Left-drag | Box-select a squad |
| Double-click | Select all matching infantry in view |
| Ctrl-double-click | Extend that selection across the map |
| Shift | Toggle units in the selection |
| Right-click | Order the selection to move, or to attack a spotted enemy |
| Middle-drag, arrow keys | Pan the camera |
| Wheel | Zoom |
| Home | Fit the map |
| Tab | Centre the camera on the selected squad; with one or no soldiers selected, cycle through the infantry |
| M | Return to the latest mission marker, keeping the selection; zooms in first from the whole-map view |
| V | Visit the eight placed tanks and guns |
| S | Stop: cancel movement, attack and auto-target for the selection |
| H | Toggle hold fire for the selection |
| C | Toggle crouch stance for the selection (cover bonus, shorter sight) |
| Ctrl+0..9, 0..9 | Assign the selection to a group number, recall it |
| Ctrl+P or Pause | Pause and resume the mission clock |
| F11 | Toggle fullscreen and windowed |
| L | Open the message log with every briefing and order notice |
| N | Play the next music track |
| Ctrl+M | Mute or unmute the music |
| Escape | Open or close the pause menu |

### Optional HD pack

A private HD pack can supply painted infantry for Tutorial I. Pass `--hd-pack DIR`, or install the pack under `$XDG_DATA_HOME/ss2/hd-pack` (default `~/.local/share/ss2/hd-pack`) for interactive launches. The renderer reads the separate ground layer `tutorial1_ground_2x.ss2hd` and the directional standing and walking sheets in `infantry-art/UNITID.png`. Each 512×320 RGBA sheet has eight columns (SE, S, SW, W, NW, N, NE, E), one standing row and four walking rows. Each 64×64 cell shares a foot anchor at (32, 58). Types without a painted sheet use their enlarged Gold frames. To use the original infantry size and artwork, pass `--original-infantry`.

The same pack may also carry audio. `audio-hd/` holds 48 kHz 16-bit or 24-bit WAV overrides for the sound groups (`<group>_<variant>.wav`), and `audio-hd/music/` holds the soundtrack tracks. Both are optional: without them the retail sound bank and the retail `MUSIC` folder are used. `--hd-pack DIR` selects the pack for all of them at once.

### Where the game data comes from

For interactive runs, `./build/ss2` also checks `SS2_GAME_DIR` and then `$XDG_DATA_HOME/ss2/game` (default `~/.local/share/ss2/game`). A directory or symlink there can point to the privately extracted game. A launch without game data still provides the original renderer smoke test. `--frames` and `--timing` only load retail data when a path is supplied explicitly.

Use `--mission FILE.ssm` to view an individual single-player mission's embedded overview. The overview is the image stored in the mission file. `--game-dir` reconstructs Tutorial I from individual assets. [Format evidence and remaining gaps](docs/tutorial-map.md)

## Build and run

Use a C++20 toolchain, CMake 3.24+, SDL3 3.2+ and PangoCairo development files, zlib 1.2.9+ and libjpeg and libpng 1.6 development files, Vulkan 1.3 headers and loader, and a Vulkan-capable Linux driver. Python 3 runs the asset fixtures when building with tests. On this Apple M1 Pro, the same CMake commands build native ARM64 Linux and SDL selects the Wayland/Honeykrisp path. On Bazzite, build in a development environment with these packages and run in the desktop session with Mesa RADV. On this Mac, run `./build/ss2` from the checkout. To close automatically after 32 frames, run `./build/ss2 --frames 32`. The executable uses the system SDL3 and Vulkan libraries. Tutorial I uses the purchased game data and runs entirely in the Linux executable.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ss2
```

`./build/ss2 --help` lists every option.

For a bounded smoke run, use `./build/ss2 --frames 120`. For a timing run, select the desired desktop display mode first, then run `./build/ss2 --4k --fullscreen --timing`. Inspect the logged **actual swapchain extent**, device, driver, format and present mode. The timing run warms up for one second and measures at least ten seconds of intervals between successful application present returns. Display scanout timing requires compositor or display instrumentation.

## Tests

Install the Khronos validation layer and configure a build with window tests enabled. A requested validation run fails if the layer is unavailable. SDL's offscreen driver plus Mesa lavapipe provides a headless test path on systems with headless surface support.

```sh
cmake -S . -B build -DSS2_TEST_WINDOW=ON -DSS2_TEST_VALIDATION=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/ss2 --validate --frames 16
```

On the original ARM64 development host the Fedora layer was staged privately. To use that copy, set `VK_LAYER_PATH="$HOME/ss2-private/tools/fedora-aarch64/usr/share/vulkan/explicit_layer.d"` and add `$HOME/ss2-private/tools/fedora-aarch64/usr/lib64` to `LD_LIBRARY_PATH`. The default build requires the layer only when validation is requested.

### What the suite contains

Thirty tests are registered. Which of them run depends on the configuration:

| Configuration | Tests | Notes |
| --- | --- | --- |
| Default | 14 | Asset fixtures, movement, combat, the parity test, clock and CLI checks |
| `-DSS2_PRIVATE_GAME=DIR` | 23 | Adds the nine retail-trace tests that need the private Gold installation |
| `-DSS2_TEST_WINDOW=ON` | 20 | Adds the six window and WSI tests |
| `-DSS2_TEST_WINDOW=ON -DSS2_TEST_VALIDATION=ON` | 21 | Adds the missing-layer test, which needs the Khronos layer |
| `-DSS2_PRIVATE_GAME=DIR -DSS2_TEST_WINDOW=ON` | 29 | Both of the above |
| All three | 30 | Everything, on a host with the validation layer |

The retail-trace tests are skipped without `-DSS2_PRIVATE_GAME` because they need the purchased installation, which is never committed. The window tests need a Vulkan WSI surface, and the missing-layer test needs the validation layer to be installed.

### Hosted CI

`.github/workflows/linux.yml` defines two matrix legs: `g++` Release and `clang++` Debug with AddressSanitizer and UndefinedBehaviorSanitizer. It runs the default configure, the full `ctest` and no further steps, so the fourteen tests that need no private data and no window are exactly what a hosted run covers. The workflow has never been dispatched: no Git remote is configured for this repository. [Validation record](docs/validation.md)

### Coverage of the combat path

`combat_parity` replays the matched-fields encounter from the Gold 2.2 oracle fixture and compares every tick and every event: 120 ticks × 13 fields and all 21 events, down to the final RNG state. A second, clearly labelled section of the same table pins native behaviour for the two-sided duel and the march-pause engage, which no oracle fixture covers. Both sections fail the build on the first divergent tick, and the regression section can be regenerated with one command when a change is intentional. [Parity and regression coverage](docs/validation.md#coverage-of-the-combat-path)

## Hardware acceptance

The acceptance target is a BC-250 running Bazzite with RADV at 3840×2160 and 120 Hz. It is the one gate that no software-only run can close: the presentation figures in the validation record characterise the application on an M1 Pro, not the physical scanout of that machine. [BC-250 run](docs/validation.md#hardware-acceptance-bc-250)

## Repository layout

| Path | Responsibility |
| --- | --- |
| src/main.cpp | SDL application lifetime, input, presentation loop and optional frame statistics |
| src/assets/ | Read SUE, PCK, mission and tutorial scene assets, the optional HD pack, painted infantry atlases and the sound bank |
| src/audio/ | SDL3 audio stream playback for the decoded sound bank |
| src/sim/ | Platform-independent mission clock, movement, unit orders, fog of war and infantry combat |
| src/render/ | Vulkan device selection, presentation, swapchain lifecycle and validation callbacks |
| src/ui/ | Briefing panel and infantry selection state |
| include/ss2/simulation.hpp | Platform-independent step, tick accumulator and snapshot boundary |
| tests/ | Clock behavior, CLI diagnostics, real window resizing, injected WSI recovery, movement, combat and the parity test |
| tools/ | Private installation inventory, repeatable Ghidra analysis scripts and the X11 playtest driver |
| docs/ | Version-pinned upstream audit, original module evidence, validation results and milestone plan |
| .github/workflows/ | x86-64 GCC Release and Clang sanitizer build/test definitions |

The `ss2_sprite` executable runs the [SUE/PCK reference slice](docs/asset-reference.md) against a private game installation. Reserve `src/simulation/` for behavior supported by original observations. Keep private installations, captures, save files, and Ghidra databases in a directory outside this project.

## Documents

| Document | Contents |
| --- | --- |
| [Validation record](docs/validation.md) | Current status, test coverage, timing measurements, the Gold comparison and the dated run log |
| [Tutorial simulation evidence](docs/tutorial-simulation.md) | The playable opening objective, movement orders and convoy replay |
| [Tutorial I map](docs/tutorial-map.md) | Scene reconstruction, decoded placements, tanks and guns |
| [Reverse engineering](docs/reverse-engineering.md) | Gold 2.2 binary evidence and the Ghidra workflow |
| [Source audit](docs/source-audit.md) | Version-pinned audit of the upstream references |
| [Implementation plan](docs/implementation-plan.md) | Milestone plan and division of work |
| [Asset reference](docs/asset-reference.md) | Supported SUE/PCK subset and retail comparison |
