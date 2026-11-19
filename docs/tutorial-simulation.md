# Gold 2.2 Tutorial I simulation evidence

The reference is the purchased build 13718, SHA-256 `aa3c36fad90a8fb39bc9aa0b55714c433a9d16df1f6d9490bd883c4abbb27f41` for n2Game_Dll.dll. Addresses below are virtual addresses at image base 0x10000000. The application reads data from the installation. Only the optional comparison tools execute isolated original instructions.

## Opening mission

The compressed MS2S mission contains 30 compiled triggers, a length-prefixed phrase table, a 128 × 128 byte location grid and 100 pairs of marker coordinates. The English text shown by the native executable comes from the player's private mission file. The dialog layout and marker outlines are native presentation choices.

Retail initialization calls its evaluator at 0x10091b40. The periodic dispatcher at 0x10090fe2 checks the low six bits of the mission tick for 3. Strict elapsed-time comparisons use 25 mission ticks per script second. The single-player dispatcher at 0x1007f8b0 gates updates against a millisecond deadline. Its fourth phase schedules from the current time and its preceding phases average the previous deadline and current time. The primary initialization reads Game/TurnDelay with a 40 ms fallback. This installation supplies 36 ms. Original modal display and dismissal set and clear the pause bit at 0x1006de55 and 0x100a94f8. The native simulation worker runs independently of Vulkan presentation. Speed changes, wall-clock stalls across system suspend, multiplayer, and whole-engine scheduling still need live comparison.

| Mission tick | Result in the native and original interpreters |
| --- | --- |
| 0, 67, 131, 195, 259, 387, 451, 515 | Opening dialogs with phrase indices 19, 0, 28, 1, 2, 20, 3, 29, respectively |
| 579 | A controlled zone count of one leaves the first movement objective pending |
| 643 | A controlled zone count of two fires trigger 10, displays ordinary phrase 4 and sets timer 0 for 150 ticks |
| 835 | First subsequent eligible scan after timer expiry. Trigger 11 displays modal phrase 5 |

Trigger 10 checks the original 16-bit count indexed by player 0 and location 2 at 0x10091e1b. The painted area has 254 cells. Its arithmetic center is tile (37,74). The program also evaluates the reminder trigger. Opening trigger 2 changes AI visibility and the later battle triggers require additional world state. Neither is implemented by the opening interpreter.

## Infantry

The placed unit definition and its RS2 sprite groups supply directional poses. Retail 0x1001227b selects the animation using state, stance and facing and chooses the frame by multiplying its count by the progress byte and shifting right eight bits. Slots within each tile are at (8,8), (24,8), (8,24), (24,24) in U/V units. The position function at 0x10021810 and RUN updater at 0x100200e0 operate on those slots, the eight directions and integer progress. All eight Tutorial I soldier definitions have the same standing and crouched RUN parameters and zero experience delta for this animation. Each update draws a bit from the shared linear congruential generator at 0x1007acc0. The two single-player initialization paths pass seed 0x76321f. Additional consumers of the same sequence in the full game affect lockstep replay.

The offline comparison runs the untouched RUN, RNG and positioning routines with three starting RNG states, two stances, four slots and eight directions. The native and original traces agree through step completion in all 192 cases, including RNG state, progress and both world coordinates. Both produce digest 0x1e0747cec323df2d. These are traces of accepted steps. Route choice, collision, individual order acceptance and mission completion require separate comparisons.

## Reproduce locally

The machine-code comparisons need Python with pefile and unicorn. No Win32 runtime is involved. Configure the private game directory for the optional licensed-data CTest. Public fixtures work without it.

```sh
cmake -S . -B build -DSS2_PRIVATE_GAME="$HOME/ss2-private/game/app"
cmake --build build
ctest --test-dir build --output-on-failure
python tools/verify_tutorial_vm.py "$HOME/ss2-private/game/app"
python tools/verify_infantry_step.py "$HOME/ss2-private/game/app"
```

## First playable objective

Normal input can now move one selected player soldier onto clear ground, animate the directional RUN frames and update actual location counts. Tutorial I advances to its next briefing when two player units enter location 2. The destination cursor changes to a crosshair over supported moves. The complete pathfinder, group destination assignment, enemy combat, AI, whole-game RNG consumption and subsequent combat objectives remain open. A rejected click leaves the soldier at its existing position.

**To reproduce the first objective by hand**, use the default 1280 × 720 window. Acknowledge the opening panels with Enter, including the panel after the camera pans to the objective. Press Home once to fit the whole map. The following positions are in pixels from the top-left of the game content, excluding the title bar. The Home view puts the two soldiers close together. They are separate front-rank submachine gunners.

| Order | Left-click soldier | Right-click ground |
| --- | --- | --- |
| First | (253, 404) | (405, 330) |
| Second | (258, 407) | (410, 333) |

You can also select at a closer zoom and then zoom out or pan to the destination. In map pixels the two starting points are (1616, 2336) and (1648, 2352). The destination tile centers are (2592, 1856) and (2624, 1872). Both lie inside the southern part of the original painted objective area. Allow about 70 seconds of unpaused time for the walk at this installation's speed. The soldiers can travel at the same time.

The original route wrapper at 0x10088700 searches tiles. The accepted-step function at 0x100218d0 expands a queued direction through the source slot and tests the new slot. A direction leaves the queue when the step crosses the tile boundary. The native direct-route branch tests the mission's P-FLAGS and P-OBSTACLES, the occupied slots and diagonal corners. It accepts clear rays of at most 63 tiles and rejects cases requiring unresolved detours or conflict handling. An additional comparison of 380 clear routes from the starting squad matched each accepted direction returned by the isolated original routines. Run the two objective-lane comparisons locally with the command below.

```sh
python tools/verify_infantry_route.py "$HOME/ss2-private/game/app"
```

The retail route wrapper and its original step-acceptance routine produced 60 north-going substeps for each of these clicks. The input scenario in the native simulation test produces the same directions and final coordinates. A separate normal SDL event-loop run exercised two selections, two right-clicks, actual rendered frames, location entry, and the two resulting script messages. That run used the installed 36 ms delay and reported zero Vulkan validation errors. A second run with a separate test profile accelerated the clock to exercise the same controls and state transitions. These comparisons cover separate engine routines and native input. A complete retail mission replay at matched world states remains outstanding.
