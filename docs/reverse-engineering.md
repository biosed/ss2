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
