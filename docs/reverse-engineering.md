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
- **Sight truncation, moving penalty, Easy bonus, and `0x1f` cap**: Infantry (`0x100212c0..0x100213b3`), base vehicles (`0x10035260..0x10035381`, with occupied `bonusSight` at `[spec + 0x537]`), and unbuttoned vehicle march sight (`0x1001d3e0..0x1001d474`, adding `marchSightBonus` at `[spec + 0x66d]` when `byte [unit + 0x112] == 0`) all apply moving penalty (`infantry -1` when `byte [unit + 0x1a] & 2`, set at `0x10021bd9` in `0x10021b70` and cleared at `0x10021aa6` in `0x10021a50`) **before** integer-truncating `(r * visPct) / 100` with a floor of `1`, then multiply player sight by `1.3` (`0x100eb2b0`) on Easy difficulty (`[0x1106f6e8]+8 <= 1`), and clamp at `31` (`0x1f`) tiles. Map initialization also runs `0x1006b2ec..0x1006b448` across the outer 4-tile border of `P-FLAGS` (`0x1034239c`), masking `(flags & 0xc0001fff) | 0x4000000f` (clearing `0x2000` elevation on the 4-tile border) and setting `0x0800` LOS block across the outer 3-tile perimeter.
- **`scandelay`**: Ground-unit descriptors initialize `[spec + 0x281]` to `-1` (`0x10060ca7`) and parse `scandelay` raw (`0x100614ae`). `module_scaner.cpp` (`0x100046a0..0x10004720`) sets scan cooldown to `scandelay + rand(0..15)` when a target is within `2 * range` and `2 * scandelay + rand(0..31)` when none is near (`0x100635e0` default `0x5a=90` at `+0xa9e` belongs to a separate descriptor class).
- **Commander hatch, suppression button-up, and `tanksonarmorprotection`**: `marchenabled` (`[spec + 0x669]`) enables player order `0x2a` (`0x1001cd64`, `0x10085b2c`), invoking vtable slot `+0xb8` (`0x1001c710`) on `byte [unit + 0x112]` (`0` = open, `1` = buttoned; spawn initializes `1` at `0x1001d275`). Per-tick unit update `0x1001cf50` (`0x1001d0b7`) buttons up whenever morale `word [unit + 0x21] < 5` (without a separate 125-tick combat timer). Crew damage in `module_cont_health.cpp` (`0x100014c9`) and `0x1001ceb0` uses `MISC\MISC` `tanksonarmorprotection` (`0x101101f4`, stored as `256 - val` at `0x10060ef0`, default `0x100`): unbuttoned `SNIPER` uses the default `0x100` (`(256 - 0) / 256` = 100% raw damage), whereas `PIERCE` and `EXPLOSIVE` use protection level `3` (`253 / 256`).
