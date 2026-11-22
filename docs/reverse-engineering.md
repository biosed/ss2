# Gold 2.2 binary evidence and Ghidra workflow

## Original material

Inspected the offline installer `setup_sudden_strike_2_2.2_(13718).exe`. Its SHA-256 is `b78b6c41678352babb5eb15a855c3ea3e78dcb10309cfc973a8bbe653f5f8109`. The separate GOG Galaxy downloader is unnecessary. The selected application files came from Inno Setup 5.5.7 Unicode records. A private reconstruction checked metadata-block CRCs and matched each selected file against its per-file SHA-1 after decompression and reversal of the Inno executable filter where applicable. The manifest records SHA-256 and LLVM PE metadata for 19 verified application, support and editor/tool images. Installer helpers and game assets are outside this inventory. [PE manifest](gog-13718-pe.tsv)

| Module or edge | Verified evidence |
| --- | --- |
| Startup | game_exe.exe is i386 PE32, image base 0x400000, entry RVA 0x36F8. Its imports include LoadLibraryA, GetProcAddress and DirectSound. The original sudtest.ini selects module 1 at startup and names n2Menu_Dll.dll, n2Game_Dll.dll and n2Text_Dll.dll. |
| Game | Primary n2Game_Dll.dll has base 0x10000000. Its exports include VModule_Init at RVA 0x79B80, VModule_Play at 0x79C10, VModule_Done at 0x79C50 and VModule_Handle at 0x79C80. It imports DirectInput, DirectSound, WINMM, GetTickCount, Sleep, SetTimer and timeGetTime. The imports alone do not establish which timer advances simulation. |
| Menu | n2Menu_dll.dll exports the same four module entry names and GetGeckVersion. VModule_Play has RVA 0x2D360. The DLL imports WSOCK32, DirectInput and binkw32. |
| Graphics and text | The INI names n2Cad640.dll, n2Cad800.dll and n2Cad1024.dll. All three import DDRAW. The 1024 module exports CADraw_Init at RVA 0x1970. The shipped ddraw.dll imports d3d9.dll. n2Text_dll.dll has no static DLL imports and entry RVA zero. |
| Audio variants | The primary game DLL imports WINMM.dll. The copy in __support/winmm_override imports WIN32.dll. The bundled win32.dll imports WINMM.DLL and libvorbisfile-3.dll, which imports the Vorbis and Ogg libraries. Both game DLLs have the same entry RVA and distinct hashes. Installer selection and runtime load order require a trace. |
| Editor and tools | edit3.exe imports scridll.dll. The packaged SUE utilities are separately invocable. Their presence does not establish an editor dependency for the main game. |

The manifest lists direct imports for each inspected image. INI-based selection, dynamic library lookup and Windows DLL search order require separate evidence. The native binary links SDL3 and the Linux Vulkan loader.

## Reproduce privately

Use Python 3.11+, LLVM tools, innoextract and a compatible Ghidra/JDK installation. Set `SS2_PRIVATE` to a directory outside this checkout. Keep the Inno installer, extracted assets, decompiler listings, save files and Ghidra project there. The normal game build uses none of them.

```sh
export SS2_PRIVATE="$HOME/ss2-private"
install -d -m 700 "$SS2_PRIVATE" "$SS2_PRIVATE/game" "$SS2_PRIVATE/ghidra"
installer="$HOME/Downloads/setup_sudden_strike_2_2.2_(13718).exe"
sha256sum "$installer"
innoextract --output-dir "$SS2_PRIVATE/game" "$installer"
game_root=$(dirname "$(find "$SS2_PRIVATE/game" -iname game_exe.exe -print -quit)")
python3 tools/pe_inventory.py "$game_root" > "$SS2_PRIVATE/pe-inventory.tsv"
llvm-readobj --file-headers --coff-imports --coff-exports "$game_root/n2Game_Dll.dll"
```

Match the primary DLL and launcher hashes against the checked-in manifest. Use the local casing of extracted filenames. For a fresh Ghidra project, import each module as its own PE program, allowing the loader to select x86 little-endian 32-bit and its preferred image base. Use a separate project or folder for the override DLL with the same filename. Standard analysis and the import/export tables provide the initial labels.

```sh
for module in game_exe.exe n2Game_Dll.dll n2Menu_dll.dll n2Cad1024.dll; do
    "$GHIDRA_HOME/support/analyzeHeadless" "$SS2_PRIVATE/ghidra" Gold13718 \
        -import "$game_root/$module" -analysisTimeoutPerFile 600
done
```

1. In the launcher, trace calls from INI reads through LoadLibraryA and GetProcAddress. Record the call sites and arguments for VModule_Init and VModule_Play. Inspect arguments and stack cleanup on both sides before declaring a calling convention or struct layout.
2. In the primary game DLL, start at VModule_Play and trace references to timeGetTime, GetTickCount and timer callbacks. Distinguish animation, input, audio, network turns and simulation writes. Record counter width, units, speed scaling, pauses and accumulated backlog as open questions until there is code and trace evidence.
3. On a machine running the original, repeat a short recorded input sequence on the same mission at two display rates. Capture elapsed time, simulation-update counts, unit positions or another stable state observation, and pause/speed transitions. Repeat after a long frame. Check whether the same tick-indexed inputs yield the same state and find the first divergence. RNG state and update order may require additional probes.
4. For each recovered fact, record build/SHA-256, module, RVA, code references, input and observation, competing interpretations, confidence and a regression fixture. A source annotation such as 0x1002D360 can match a known menu export while other functions still need independent verification. Keep public fixtures synthetic and public notes limited to metadata and behavioral descriptions.

## Executed locally

Innoextract independently extracted the launcher, primary and override game DLLs, menu DLL and 1024 renderer. Their SHA-256 values matched the inventory. Ghidra 12.2 imported and analyzed the four primary PE programs in a private Gold13718 project. The host is ARM64. The release initially lacked an ARM64 decompiler executable, so the bundled Decompiler source was built locally and installed under its linux_arm_64 directory. The timing script then successfully decompiled VModule_Play at RVA 0x79C10.

```sh
"$GHIDRA_HOME/support/analyzeHeadless" "$SS2_PRIVATE/ghidra" Gold13718 \
    -process n2Game_Dll.dll -scriptPath "$PWD/tools/ghidra" -postScript TimingReferences.java
```

On the primary DLL, the script reported 106 instruction references to timeGetTime, one to GetTickCount (site RVA 0xD03DB), one to Sleep (0xDD772) and zero to SetTimer under the script's direct-or-one-pointer search. SetTimer is present in the import table. Counts describe static references found by this procedure. They cannot identify which function advances simulation. The next evidence required is a trace connecting a time read to a simulation state write, repeated at different presentation rates, game speeds and across pause/resume. [Ghidra project](https://github.com/NationalSecurityAgency/ghidra)

### Timing reference triage

On the verified primary DLL, VModule_Play calls the active list node's function pointer at RVA 0x79C1E and follows the link at node offset 8. The export dispatches multiple handlers. Counting calls to the export alone would conflate dispatch and simulation work. The function at RVA 0x7AC70 reads timeGetTime and stores a value used by the linear congruential recurrence at RVA 0x7ACC0. This provides a concrete seed candidate to control for replay comparisons. Neither observation establishes a tick period. Both were checked against the i386 instructions in the original PE. A mission-level dynamic trace, pause/speed experiment and matched state observation remain open.


### Tutorial I command reference

The purchased editor source pairs a player-owned officer and riflemen in group 30 with named tutorial script records in `maps.src/map.020/mis.000/scripts2`. The labels cover selection, a send-unit prompt, a timer and a first-zone or enemy-group condition. The manual describes group selection and the native test exercises it. Original-module traces now establish the accepted steps, formation assignments, collision retries and bytecode callbacks for the first movement objective. A fixed-seed convoy replay compares each of the starting squad’s positions at matched mission ticks. The subsequent enemy engagement needs a separate combat trace. [Movement evidence and offline checks](tutorial-simulation.md)

## Gold 2.2 weather, sight, scan-delay, and hatch disassembly

Static x86 disassembly of the pinned `n2Menu_dll.dll` and `n2Game_Dll.dll` binaries (base `0x10000000`) established the remaining runtime consumers and formulas:

- **`I-MISXINF` per-mission weather pipeline**: `n2Menu_dll.dll` `0x10025d66..0x10025df4` reads the mission's `I-MISXINF` record (392 bytes in retail single/campaign missions) and writes its `dword[0]` into `dword[5]` of `XCHNG\TOGAME\map_info`. `n2Game_Dll.dll` `0x1006ae6c..0x1006ae90` loads `dword[5]` into global `0x1014238c` (`g_weatherKind`), clamping values `< 0` or `>= 4` to `0`. `WeatherSpec` getter `0x10099430` indexes `*(this + 11 * kind + 0x1d)`, mapping `0` → Clear (`0x1d` = 100), `1` → Fog (`0x28`), `2` → Rain (`0x33`), and `3` → Snow (`0x3e`). A census of 101 retail `SSM`/`SSC` archives observed `dword[0]` values `{0: 90, 2: 5, 3: 6}`, matching Clear/Rain/Snow.
- **Sight truncation, moving penalty, Easy bonus, and `0x1f` cap**: Infantry (`0x100212c0..0x100213b3`, scaling `[spec + 0xd5] + ((f * [spec + 0xd9]) >> 15)` where `0x1004a270` aggregates squad member strength/sight `word[unit+0x5e]` + `word[unit+0x41]` via `0x10001941`/`0x10002ba1`/`0x10004ea0`, or `0x7b8`/`0x7bc` when `[unit + 0xe6]` binoculars mode is active), buildings/garrisons (`0x100290e0..0x100291fc`, scaling best occupant sight + `[buildingSpec + 0xfb1]`), base vehicles (`0x10035260..0x10035381`, with occupied `bonusSight` at `[spec + 0x537]`), and unbuttoned vehicle march sight (`0x1001d3e0..0x1001d474`, adding `marchSightBonus` at `[spec + 0x66d]` when `byte [unit + 0x112] == 0`) all apply moving penalty (`infantry -1` when `byte [unit + 0x1a] & 2`, set at `0x10021bd9` in `0x10021b70` and cleared at `0x10021aa6` in `0x10021a50`) **before** integer-truncating `(r * visPct) / 100` via `0x10099430` with a floor of `1`, then multiply player sight by `1.3` (`0x100eb2b0`) on Easy difficulty (`[0x1106f6b0] == 0 && [0x1106f6e8]+8 <= 1 && [unit+0x1c] == [0x1106f05c]`), and clamp at `31` (`0x1f`) tiles. Four native-only approximations remain documented vs retail: (1) `TutorialUnits::refreshSpotting` (`tutorial_units.cpp:1508-1521`) adds flat garrison `+3`, binocular `+binoSightBonus`, and high-ground `+2` post-`effectiveSightRadius` (with high-ground `+2` unscaled by weather/Easy, whereas retail has only the pre-weather squad/spec/building terms); (2) garrison/binocular bonuses are weather-scaled separately without re-applying Easy `1.3x` (whereas vehicle march sight re-applies `1.3x`); (3) vehicle high-ground `+2` (`tutorial_units.cpp:1532-1534`) enters pre-weather while infantry `+2` enters post-weather; and (4) `effectiveSightRadius` (`infantry_combat.cpp:316-318`) grants AI units (`owner != 0 && owner != 3`) a `1.15x` (`* 115 / 100`) Hard-mode sight boost with no counterpart in `0x100212c0`. Map initialization also runs `0x1006b2ec..0x1006b448` across the outer 4-tile border of the single retail `P-FLAGS` array (`0x1034239c`), masking `(flags & 0xc0001fff) | 0x4000000f` (intentionally clearing bit 13 `0x2000` High Ground and bit 14 `0x4000` blocked on that border ring) and setting `0x0800` LOS block across the outer 3-tile perimeter. On the native side, `FogGrid::applyMapPerimeterFlags()` applies this exact transform to the fog/LOS flags copy while `InfantryPathGrid::inBounds` enforces `borderMargin_ = 4` across the entire 4-tile border ring.
- **`scandelay`**: Ground-unit descriptors initialize `[spec + 0x281]` to `-1` (`0x10060ca7`) and parse `scandelay` raw (`0x100614ae`). `module_scaner.cpp` (`0x100046a0..0x10004720`) sets scan cooldown to `scandelay + rand(0..15)` when a target is within `2 * range` and `2 * scandelay + rand(0..31)` when none is near (`0x100635e0` default `0x5a=90` at `+0xa9e` belongs to a separate descriptor class).
- **Commander hatch, suppression button-up, and `tanksonarmorprotection`**: `marchenabled` (`[spec + 0x669]`) enables player order `0x2a` (`0x1001cd64`, `0x10085b2c`), invoking vtable slot `+0xb8` (`0x1001c710`) on `byte [unit + 0x112]` (`0` = open, `1` = buttoned; spawn initializes `1` at `0x1001d275`). Per-tick unit update `0x1001cf50` (`0x1001d0b7`) buttons up whenever morale `word [unit + 0x21] < 5` (without a separate 125-tick combat timer). Crew damage in `module_cont_health.cpp` (`0x100014c9`) and `0x1001ceb0` uses `MISC\MISC` `tanksonarmorprotection` (`0x101101f4`, stored as `256 - val` at `0x10060ef0`, default `0x100`): unbuttoned `SNIPER` uses the default `0x100` (`(256 - 0) / 256` = 100% raw damage), whereas `PIERCE` and `EXPLOSIVE` use protection level `3` (`253 / 256`).

### Retail `MM2S` multiplayer opcodes and Zeppelin capture controller

Static disassembly of `n2Game_Dll.dll` (`base 0x10000000`) and a census of all 35 retail `maps/MPLAY/*.smm` archives versus the 101 single-player/campaign missions (`62` `.ssc` + `39` `.ssm`) established the retail multiplayer skirmish script contract:

- **CAOF VM multiplayer opcodes (`0x10091b52..0x100938f4`)**: `OP(52)` (`0x100930ae`), `OP(53)` (`0x1009310f`), `OP(54)` (`0x10093181`), `OP(55)` (`0x100931ac`), and `OP(56)` (`0x100931ce`) dispatch to the multiplayer controller (`0x100b4730`, vtable `0x100f00a4`). Across the retail corpus, only four MP-specific opcodes appear in `maps/MPLAY/*.smm` (`OP(52)`: 127, `OP(53)`: 244, `OP(54)`: 97, `OP(56)`: 96), with zero occurrences of `OP(52..56)` across all 101 SP/campaign scripts.
- **`OP(52)` nation starting/timed waves (`0x100b4c70`, vtable `+0x4c`)**: Pops `(delaySec, wJp, wUs, wGb, wDe, wRu)` with nation enum `0 = Russian, 1 = German, 2 = British, 3 = USA, 4 = Japanese`, scheduling per-seat `I-SUPPORT` waves at `delaySec * 25` through each player's assigned active `I-FLAGS` gate; match initialization (`0x100b4be7..0x100b4c36`) centers the local camera on that player's assigned gate.
- **`OP(53/54/56)` Zeppelin rewards (`0x100b4fb0..0x100b50d0`, vtable `+0x50/+0x54/+0x58`)**: `OP(53)` pops `(periodSec, wJp, wUs, wGb, wDe, wRu, zeppelinMask)` to attach repeating nation reinforcement waves to `I-OBJECTS[0..31]` bitmasks (`0x10892d80`). `OP(54)` invokes `0x1006ebc0` (identical to `OP(24)` air-planes add) and `OP(56)` invokes `0x1006ec30` (identical to `OP(25)` air-sorties add).
- **16-tick Zeppelin scan and elimination check (`0x100b51a0`, vtable `+0x18`)**: Every 16 ticks at `(tick & 0xf) == 0xe`, computes team presence inside the Zeppelin capture radius via octagonal distance `max(dx, dy) + min(dx, dy) / 2` (`0x100b5386..0x100b53bb`), clears contested markers when multiple teams are present, increments hold counters when uncontested, emits wave/air rewards once captured, and evaluates elimination victory/defeat (`0x100b5873..0x100b5a5c`).
- **Remaining 17 CAOF opcodes and air-route table (`n2Game_Dll.dll` `0x10091ee6..0x100937b2`, 136 retail `.ssc`/`.ssm`/`.smm` corpus)**: Scanning all 136 retail missions (`62` campaign `.ssc`, `39` single `.ssm`, `35` multiplayer `.smm`; `2939` triggers total, `0` occurrences of `OP(55)`) completed the remaining VM handlers:
  - **`OP(43/44/45/46)` air-route helpers & zone airstrike (`0x11039cb0`)**: `OP(43/44/45)` build/reset the 60-entry `0x11039cb0` air-route waypoint table, and `OP(46)` launches air-support sorties along that route (or falls back to the target zone center when empty).
  - **`OP(39)` reserve deploy (`0x10092b2f` -> `0x100543b0`)**: Schedules delayed reserve deployment into a target zone/group.
  - **`OP(83)` & `OP(91)` group stats & ammo query (`vtable +0x13c`, `0x10048cd0`)**: `OP(83)` floors/caps a group stat (`0 = HP`, `1 = Ammo`, `2 = Expa` `0..0x8000`, `3 = Morale`); `OP(91)` compares primary/secondary group ammo percentage.
  - **`OP(62/63/71/72/84/90)` saved-unit pool system (`0x10101f44`, `0x100c91e0`)**: Clears a pool (`OP(62)`), saves surviving group/player units (`OP(63/84)`), materializes pooled units into dynamic reinforcement waves preserving or overriding group IDs (`OP(71/72)` for `OP(40)`), and queries saved pool counts (`OP(90)`) across campaign missions.
  - **`OP(85/86)` evacuation (`0x100917c0`/`0x100944f0`, order `0x28`), `OP(8/9/31/33/75)`**: Orders group (`OP(85)`) or zone (`OP(86)`) evacuations to map edge / reserve, executes `OP(8)` group airfield orders, queries active/unmanned group counts/percentages (`OP(9/31/33)`), and dispatches distance-limited moves (`OP(75)`).

### Retail `MOST.RES` destructible & repairable bridges

Static disassembly of `n2Game_Dll.dll` (`0x10070b50`, `0x100715c0`) and inspection of `MOST.RES` (`game_scheme1.sue` 32 records, `game_scheme3.sue` 19 records), `LAND\MOST`, and `UNITS\ss` established the retail bridge format and mechanics:

- **`MOST.RES` record layout**: Bytes `0..511` store the 256-entry RGB565 palette; bytes `516..1959` store a `38x38` subcell mask grid (`baseTile = (u >> 5) - 9`, `(v >> 5) - 9`, where bit `0x01` marks bridge passable footprint, bit `0x04` marks hit/splash footprint, and `(0x10 << stage)` indicates passability at stage `0..3` while stage `4` blocks all bridge cells); bytes `1960..1999` store 10 relative frame offsets (5 stages x body/shadow pair, relative to `1960`); and bytes `2004..` begin stage 0 sprite data. Across all 51 summer and beach bridge records, 0 bad stage frames were observed.
- **`LAND\MOST` & `UNITS\ss`**: `LAND\MOST` specifies `HP default 1000 1500 500 500` (4 damage transitions totaling `3500` HP across stages `0..4`: stage 0 `>2500`, stage 1 `1001..2500`, stage 2 `501..1000`, stage 3 `1..500`, stage 4 `<=0` destroyed). `UNITS\ss` supply trucks carry `5000` spare-parts `ammo2` and use `idlesnd_fixmost` / `cmd::SupportBridge` to repair damaged or collapsed bridges, clearing submerged units on collapse (`OP(41)` detects destroyed status at bridge markers) and reopening `InfantryPathGrid` subcells on repair.

### Retail `I-GROUPS` initial AI group table & auto-reserve
- **Mission `I-GROUPS` section**: Present in all `101` retail single-player and campaign `.ssc`/`.ssm` missions (`2800` bytes = `100` fixed `28`-byte entries; `0` bytes across all `35` multiplayer `.smm` maps, `1742` non-default entries across behaviors `0..13`).
- **`n2Game_Dll.dll` (`0x10090850` / `0x10091040` / `0x10091350` / `0x10091440` / `0x10092278..0x10092353`) & `edit3.exe`**: `0x10090850` loads `XCHNG\TOGAME\mis_groups`, and `GroupAI::loadFromDisk28` (`0x10091040`) initializes each group's AI behavior (`0..13`: `ai_none`, `ai_sold_guard`, `ai_sold_art`, `ai_*_follow`, `ai_recon`, `ai_furg_help`, `ai_gruz_reload`, `ai_katya_move`, `ai_train_move`, ..., `ai_furg_transport`), 24-bit `aif_*` flags (`aif_holdfire`, `aif_useobjects`, `aif_domambush`, `aif_domhide`, `aif_domnouse`, `aif_zonehaveown`, `aif_zonenoenemy`), zone assignments, and auto-reserve thresholds (`minUnits`, `minDelay`, `forceDelay`).
- **Auto-reserve & packed `OP(19)`**: `DeployReserve` (`0x10091350`) is polled every 16 ticks at `(tick_ & 0xfu) == 9u` (`0x10091440`), releasing reserves when `elapsed >= minDelay && (active <= minUnits || elapsed >= forceDelay)`. CAOF `OP(19)` (`0x10092278..0x10092353`) packs `behavior = w0 & 0xf` and `aiFlags = w0 >> 4`, forwarded alongside `OP(20/21/80)` (and `Tutorial I` Group `13` loads retail `I-GROUPS` `behavior = 4` `ai_sold_art`, `aiFlags = 1`).
