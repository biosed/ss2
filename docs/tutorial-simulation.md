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

### The fields after the wreck

Trigger 11 displays phrase 5 and moves the camera to location 3, near tile (41,53). The mission's `mapunits` places five enemy `aguner` infantry in group 3 across tiles (41,53) and (42,53), each with 100 hit points. The four enemies near the wreck belong to group 2. Defeating those four would leave the fields objective pending.

Trigger 12's internal name mentions group 2, but its compiled condition pushes group index 3. Script operation 3 at 0x10091cac calls 0x10091ab0 and checks whether the group's live on-map count has dropped below one. The group records start at 0x110395a0 with stride 0x8a and are counted by 0x10090cd0. On the first eligible scan after the last of those five enemies dies, trigger 12 displays phrase 6 and starts timer 1 for five script seconds. Trigger 13 then displays phrase 7.

The original default-command handler at 0x10087b40 calls 0x10049320 to test whether the target is alive, hostile and spotted. For a spotted enemy the branch at 0x10087cdf dispatches an attack on that unit through 0x100867f0 (order 3). An unspotted enemy falls through to the ground-move branch at 0x100866c0. The native tutorial currently stops at the fields briefing. Continuing requires verified spotting, firing, damage, death and group accounting.

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

Acknowledge the opening panels with Enter. After the briefing asks you to move to the abandoned vehicle, drag-select the starting squad and right-click the wreck. Tab centers the selected squad and M returns to the marker at the original zoom. The squad has 29 player-controlled soldiers. The original mission script displays “Good” after two player units enter location 2. A later briefing asks you to eliminate the enemy. Combat and vehicle boarding remain unimplemented.

Gold's map viewport handles a right-button press at 0x1009dbd3. With an active left selection box, it cancels the box. Otherwise it dispatches the press coordinates to the command handler. The input translator sends event 0x20 on press and 0x40 on release. The viewport dispatch table at 0x1009da00 handles 0x20 and ignores 0x40. The native tutorial uses the press position immediately, including when the pointer moves before release. Middle-drag and arrow keys pan the native camera.

**Two-soldier control case.** To reproduce the earlier clear-ground test by hand, launch with `--windowed` for the 1280 × 720 coordinate guide. Acknowledge the opening panels, including the panel after the camera pans, then press Home once to fit the whole map. The following positions are in pixels from the top-left of the game content, excluding the title bar. The Home view puts the two soldiers close together. They are separate front-rank submachine gunners.

| Order | Left-click soldier | Right-click ground |
| --- | --- | --- |
| First | (253, 403) | (405, 330) |
| Second | (258, 405) | (410, 333) |

You can also select at a closer zoom and then zoom out or pan to the destination. In map pixels the two starting points are (1616, 2336) and (1648, 2352). The destination tile centers are (2592, 1856) and (2624, 1872). Both lie inside the southern part of the original painted objective area. Allow about 70 seconds of unpaused time for the walk at this installation's speed. The soldiers can travel at the same time.

### Formation and route evidence

The wreck is scenery at world (1208, 2344), tile (37, 73). The ordinary ground-order dispatcher at 0x100866c0 calls the group destination helpers at 0x10084500 through 0x10084640. The starting squad uses the compact formation branch. Each soldier receives the clicked tile plus its offset from the squad’s integer tile centroid, (34, 112). All 29 primary assignments for each of four clicks matched Gold, including the wreck. A widely dispersed squad uses the alternate formation branch. Blocked compact assignments and dispersed selections scan successive rings about the clicked tile and reserve up to four infantry per fallback tile. For the 254 objective tiles, the 7,366 starting-squad assignments and another 7,366 with half the formation displaced by 30 tiles matched the original. The test preserves selection order. The two fingerprints are `c843ae03` and `72fc5094`.

The infantry wrapper at 0x1008bda0 writes its route queue at unit offset 0xa9. It uses the ray and wall-following search at 0x1008be20. The accepted-step routine at 0x100218d0 uses the source slot, queued direction and live obstacles to select a substep. Crossing a tile boundary consumes that queue entry. The original tile occupancy grid holds four slot bits per tile. A comparison with opening occupancy held fixed matched the complete native output of accepted directions for all 29 starting soldiers against all 254 location-2 tiles (7,366 cases). The comparison includes partial and empty routes. In the public check, sorting starts and separating each sequence with byte 255 gives CRC32 `c67b6a1b` in both engines. When an advancing soldier meets another formation member, the original scheduler at 0x10058570 compares live occupancy with the footprints of lower-priority units. It can retain the queue, search for a detour, back off or terminate a blocked order. The native and original tests compare each of these branches. Changing a destination during a RUN substep preserves its current progress. Repeating the same tile retains the existing queue.

Run the retail formation and route checks against your own installation.

```sh
python tools/verify_infantry_route.py "$HOME/ss2-private/game/app" --all-zone
python tools/verify_tutorial_convoy.py "$HOME/ss2-private/game/app" --long-marches --inflight-redirect
```

For the wreck click at map (2960, 1776), an isolated replay sampled all 29 soldiers for 3,200 unpaused mission ticks. All 92,800 rendered positions matched the native simulation, CRC32 `0489743f`. Counting from tick zero after the order, the second soldier entered the area on tick 1,758 and the last on tick 2,087. All 29 ended on their assigned tiles. The replay fixes the seed at `0x76321f`, processes soldiers in the verified initial live-list order and represents the other initial placements as occupied slots. It calls the original RUN, movement, retry, position and scheduler routines. Fog rendering, full-world indexing, other consumers of the shared RNG and experience events are outside the fixture. The full retail frame loop can therefore produce different timings.

Three more orders send the front-rank infantryman at world (1096,3576) beyond one 61-entry route queue. Over 6,300 ticks each, the native positions matched the original callbacks for goals (75,35), (42,42) and (34,35). The last goal is inaccessible and Gold settles at (32,32). In this test the original map loader's four-tile perimeter pass at 0x1006b2ec runs before path search. The wall-following branches sample bit 30 of the first candidate tile before trying alternate directions. Omitting that test changes the course of the long fallback even when it ends on the same tile. An additional fixed-seed sample compared 900 initial queues across the map on a four-tile grid, selecting an opening soldier with a seeded random generator. All 900 matched with the opening occupancy held fixed.

Both clear-ground controls produced 60 northbound substeps. The private-data CTest also orders all 29 soldiers to the wreck through the native group-order interface and checks both subsequent original phrases. The X11 playtest exercises real window input, confirms the number of recipients and clicked coordinates, and checks those phrases on the installed 36 ms clock with Vulkan validation enabled.

```sh
python tools/playtest_tutorial_x11.py --game "$HOME/ss2-private/game/app" \
    --scenario wreck --selection squad --output /tmp/ss2-wreck-playtest
```

The X11 playtest requires xdotool, ImageMagick and an available X display. Use a dedicated display when testing unattended. Set `--walk-timeout 150` on a slower machine. Frame-for-frame comparison of a complete retail mission, including scheduler and crowd timing, remains outstanding.

## Pointer selection and second orders

Gold's single-selection, default-order and cursor paths call 0x1009b880. Away from a house, it tests an infantryman's foot against the pointer in U/V coordinates. The distance is the larger absolute delta plus half the smaller delta, rounded down. All eight Tutorial I infantry types have radius 5. Their ordinary-ground cutoff is distance 25 inclusive. A headless original-code fixture at the wreck returned the unit at 25 and clear ground at 26. The wreck never enters the house hit-test branch. The native picker applies the foot rule to both sprite sizes. In a live input replay at (2768,1857) the southern enemy near the wreck could be inspected. Clicking (2768,1858) selected clear ground and dispatched a move for 29. House candidates and spotting-dependent default orders still need native world state.

The compiled I-UNITS section stores the 96 on-map placements in the exact reverse of the editor's mapunits file. The loader at 0x10057db3 reads compiled records forward and 0x10055e76 prepends each spawned unit. Traversing the live list therefore follows the editor's file order. The earliest editor placement wins an exact distance tie. The simulation loop at 0x10055ff0 follows the same list. The path scheduler at 0x10058570 prepends its entries to priority buckets and visits equal priorities in reverse. Selection at 0x100b33bb leaves the list intact, and 0x10087b40 reverses the eligible units twice before formation dispatch at 0x10086470. The native scene preserves file order for these operations. Only drawing sorts by foot height. The single-click release at 0x100b31a0 uses the midpoint of its inclusive rectangle when both sides are at most eight screen pixels. Larger rectangles test the units' feet. The native port measures gestures in SDL logical pixels.

Right-click conversion at 0x1009d7b6 shifts the signed horizontal coordinate right by one before adding it to and subtracting it from map Y. At the right side of the wreck, (2961,1768) consequently resolves to tile (37,73). Halving the two coordinates separately sends this boundary click to the neighboring tile (37,72).

A new group order clears the pending route request at 0x100369d0. A soldier who is already in a RUN substep requests the replacement route at 0x10021a50 after finishing that substep. Requesting it immediately changes the order of draws from the shared random generator. Five controlled two-leg replays issued the fields order at ticks 350, 1700, 1872, 2108 and 3200. Across 797,500 sampled positions, each native coordinate matched the isolated original routines. With all 29 in mid-stride at tick 1872, the full 5,500-tick checksum is `3fbc7902`. The fixed seed, update ordering and occupancy assumptions are the same as for the first wreck convoy.
