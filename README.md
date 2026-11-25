# Sudden Strike II Native Engine (Linux & Windows)

A native C++20 / SDL3 / Vulkan 1.3 reimplementation of *Sudden Strike II (Gold 2.2)* for **Linux** (x86-64 and ARM64, Wayland/X11, Bazzite/SteamOS) and **Windows** (x86-64). Requires user-supplied original or GOG *Sudden Strike II* game data.

The engine runs all 136 retail `.ssc`/`.ssm`/`.smm` missions (`0` unhandled CAOF opcodes), bit-exact retail visibility, combat, group AI, progressive `dom.res` building and `MOST.RES` bridge destruction, the Gold 2.2 front end, 2–4 player deterministic TCP lockstep multiplayer, 2× HD asset packs, and out-of-the-box community mods.

## Quick start

```sh
# Linux (auto-checks SS2_GAME_DIR, ~/.local/share/ss2/game, or executable folder)
./build/ss2 --game-dir "$HOME/ss2-private/game/app"

# Windows (drop ss2.exe + bundled DLLs into your SS2 folder, or pass --game-dir)
ss2.exe --game-dir "C:\Games\Sudden Strike II"
```

When launched without `--frames` or `--timing`, `ss2` searches in order:
1. `--game-dir DIR`
2. `SS2_GAME_DIR`
3. `$XDG_DATA_HOME/ss2/game` (default `~/.local/share/ss2/game` on Linux; `%LOCALAPPDATA%\ss2\game` on Windows)
4. The executable directory itself (`SDL_GetBasePath()`) if `sudtest.ini` or `game_common.sue` is present (so `ss2.exe` runs directly when copied into a retail/GOG folder)

Opening cinematics (`movies/logo1.BIK`, `movies/intro.BIK`, or HD `movies-hd/*.mkv`) play via FFmpeg before the main menu (`--no-movies` skips them).

## Features

- **Full Retail Single-Player & Campaign Parity**: `Single Player` multi-profile manager (`New` / `Rename` / `Delete`), all 5 retail campaigns + `Custom Campaigns` (`maps/custom_campaign/`, `~/.local/share/ss2/custom_campaigns/`, or `%LOCALAPPDATA%\ss2\custom_campaigns`), animated `I-BRF` briefings with OKI ADPCM commander voices and looping `backwav` / `briefing` music, Screen 5 `loadscr.dat`, post-mission `Ratings` / battle stats, persistent `SS2SAVE2` save/load (`F1–F8` quick-save / bookmark slots), `Replay Browser` (`--record`, `--replay`), and `Options`.
- **2–4 Player Lockstep Multiplayer**: Deterministic Star Host-Relay over TCP (`--host PORT`, `--join HOST:PORT`, `--mp-players`, `--mp-teams`, `--mp-nation`, `--list-multiplayer`), FFA / `2v2` / custom teams, Zeppelin capture economy, allied (`Shift+Enter` / `/a `) & global (`Enter`) chat, and `Alt+LeftClick` tactical flare beacons. Cross-ISA bit-for-bit simulation determinism is verified across ARM64 GCC, ARM64 Clang, x86-64, and x86-64-v3 (`-ffp-contract=off`).
- **Out-of-the-Box Mods**: Supports `sudtest.ini` `[Data]` `;`-chains (`Common=`, `Menu=`, `Game=`, `Sounds=`, `Movies=`, `BackWav=`, `Addons=`, `Scheme1..N=`) and `[Game]` keys (`TurnDelay`, prefixes), `Scheme3` `desc_scheme3.sue` / `lang_desc_scheme3.sue`, unpacked raw or RFC-1952 gzip folder mounts (`mycommon\`), `FZFS` and standalone `addons/*.ssx` mission packs, and automatic discovery under `<gameDir>/mods/*`, `<gameDir>/Run/*` (MultiModSwitcher layout), `<gameDir>/switcher/*`, `<gameDir>/sudtest_*.ini`, and user `mods/`. Switch mods live in `Options -> Mod` or via `--list-mods` / `--mod NAME|DIR|INI`.
- **Localization**: Full EN / FR / ES interface auto-detected from locale (`LC_ALL`, `LC_MESSAGES`, `LANG`), `--lang en|fr|es`, or `Options`.
- **Optional 2× HD Pack**: Pass `--hd-pack DIR` or place `infantry-art/`, `audio-hd/`, `movies-hd/` under `~/.local/share/ss2/hd-pack` (`%LOCALAPPDATA%\ss2\hd-pack` on Windows). Pass `--original-infantry` for vanilla sizing.

### In-game controls

| Input | Effect |
| --- | --- |
| Left-click / Left-drag | Select unit or box-select squad |
| Double-click / Ctrl-double-click | Select all matching types in view / across map |
| Shift | Add or toggle units in selection |
| Right-click | Move, board/enter, repair, attack spotted enemy, or issue command-bar action |
| Middle-drag / Arrow keys / Wheel / Home | Pan, zoom, fit entire map |
| `Tab` / `M` / `V` | Cycle infantry / jump to mission marker / cycle tanks & guns |
| `S` / `H` / `C` / `B` / `X` | Stop / Hold fire / Crouch / Officer binoculars / Hatch toggle |
| `Ctrl+0..9`, `0..9` | Assign / recall control group |
| `F1–F8` | Quick-save / bookmark slots |
| `Enter` / `Shift+Enter` / `Alt+Click` | Global chat / Allied chat / Tactical flare beacon (MP) |
| `Ctrl+P` or `Pause`, `L`, `O`, `N`, `Ctrl+M` | Pause, Message Log, Force Status, Next Music Track, Mute Music |
| `F11`, `Escape` | Toggle fullscreen, Open Pause Menu |

## Build & Install

### Linux (Fedora / Bazzite / Debian / Asahi ARM64)

Requirements: C++20 compiler (`g++` or `clang++`), CMake 3.24+, Ninja, SDL3 3.2+, PangoCairo, zlib 1.2.9+, libjpeg, libpng 1.6+, libvorbis (`libvorbisfile.so.3` loaded at runtime), FFmpeg (`libavformat`, `libavcodec`, `libswscale`, `libswresample`, `libavutil`), Vulkan 1.3 headers/loader + driver, and Python 3 (for CTest).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ss2 --help

# Optional desktop & binary install (e.g. ~/.local or /usr/local)
cmake --install build --prefix "$HOME/.local"
```

Desktop files installed by `cmake --install`: `bin/ss2`, `share/applications/io.github.biosed.ss2.desktop`, `share/icons/hicolor/scalable/apps/io.github.biosed.ss2.svg`, and `share/metainfo/io.github.biosed.ss2.metainfo.xml`.

### Windows (x86-64 via MSYS2 UCRT64)

Automated Windows builds (`ss2-windows-x86_64`) are built on every push by [`.github/workflows/windows.yml`](.github/workflows/windows.yml). To build locally in an MSYS2 `UCRT64` shell:

```sh
pacman -S --needed \
  mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,ccache,pkgconf,python,sdl3,pango,zlib,libjpeg-turbo,libpng,ffmpeg,libvorbis,vulkan-headers,vulkan-loader}

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist/ss2-windows-x86_64
```

(Pass `-DSS2_WITH_FFMPEG=OFF` if building a minimal headless/CI target without FFmpeg.)

## Tests & Verification

Fifty-nine tests are registered in [CMakeLists.txt](CMakeLists.txt):

```sh
# Standard asset-free test suite (33 tests on Linux & Windows)
ctest --test-dir build --output-on-failure

# Full Linux Vulkan + Khronos validation suite (45 tests without retail data, 59 with SS2_PRIVATE_GAME)
cmake --preset validation
cmake --build --preset validation
tools/ctest-validation.sh build-validation

# Clang ASan + UBSan + LSan sweep
cmake --preset sanitize -DSS2_PRIVATE_GAME="$HOME/ss2-private/game/app"
cmake --build --preset sanitize
tools/ctest-validation.sh build-sanitize

# Cross-ISA deterministic simulation verification (GCC, Clang, x86-64, x86-64-v3 FMA probe)
./tools/check-cross-isa-sim.sh
```

| Configuration | Tests | Notes |
| --- | --- | --- |
| Default / Windows (`--preset release`) | 33 | Sim determinism, `combat_parity`, `visibility_parity`, `tutorial_vm`, `lockstep` TCP, SUE/mission/addon/mod VFS, `save_file`, frontend/strings/movie, CLI |
| `-DSS2_TEST_WINDOW=ON -DSS2_TEST_VALIDATION=ON` (`--preset validation`) | 45 | Adds 12 Vulkan window/WSI/UI/VFX validation tests (Linux CI matrix in `.github/workflows/linux.yml` inside `fedora:45`, plus `cross-isa-determinism`) |
| `-DSS2_PRIVATE_GAME=DIR` (with window+validation) | 59 | Adds 12 retail trace/VM/scene/sound/movie tests plus `gold_cli_record_replay` & `gold_session_input` |

**Hardware acceptance (passed)**: Native x86-64 Fedora 44 container on Bazzite 45 (AMD BC-250 / RADV GFX1013, 3840×2160@120 Hz Wayland): `59/59` validation CTests, `8/8` live Wayland WSI tests, zero Vulkan validation errors, steady `p50=8.33 ms` frame pacing, and bit-identical simulation hashes with ARM64. See [docs/validation.md](docs/validation.md).

## Repository layout

| Path | Responsibility |
| --- | --- |
| `src/main.cpp` | CLI flags (`--mod`, `--list-mods`, `--host`, `--join`, `--record`, `--replay`), startup/game-dir discovery, Vulkan/SDL main loop |
| `src/assets/` | SUE/SSX archives, `sudtest.ini` VFS & loose/gzip mod mounts, missions, CAOF assets, PCK/ANI/RS2 sprites, terrains, catalogs, sound banks |
| `src/audio/` | SDL3 audio stream SFX mixer and dynamic `libvorbisfile` Ogg soundtrack player |
| `src/sim/` | Fixed-36 ms deterministic mission clock, infantry/vehicle/air/bridge/building/mine sim, CAOF VM, lockstep protocol |
| `src/net/` | Cross-platform non-blocking TCP (`Winsock2` + POSIX sockets) and 2–4p Star Host-Relay transport |
| `src/session/` | Playable session orchestrator, input/orders/menus, `SS2SAVE2` save/load, replay, and lockstep sync |
| `src/render/` | Vulkan 1.3 device selection, swapchain lifecycle, sprite/terrain shaders, validation reporting |
| `src/ui/` | Gold 2.2 front end, `Options -> Mod`, `I-BRF` briefings, tactical HUD, tooltips, EN/FR/ES strings |
| `src/video/` | FFmpeg `.BIK`/`.MKV` decoder and SDL3 opening cinematic player |
| `packaging/` | Freedesktop `.desktop`, vector icon, and AppStream metadata for Linux desktop/Bazzite installs |
| `.github/workflows/` | `linux.yml` (`fedora:45` x86-64/ARM64 Release+ASan/UBSan + `cross-isa-determinism`) & `windows.yml` (MSYS2 UCRT64 + standalone zip) |
| `docs/` | Reverse engineering (`gog-13718-pe.tsv`), source audit, determinism audit, implementation plan, and validation record |
