# Tutorial I on native Linux

The scene reads the private GOG Gold 2.2 build 13718 installation. All asset paths below are relative to its game directory. The public tests generate their own pixel and tile data.

| Input | Observed in Tutorial I | Native use |
| --- | --- | --- |
| `maps.src/map.020/info` and `rhombs` | 128 × 128 grid with eight bytes per cell | Compose the ground texture in an 8192 × 4096 isometric canvas |
| `game_scheme1.sue` | Indexed rhomb pixels plus banks of 256 little-endian RGB565 colors | Decode and color the ground tiles |
| `maps.src/map.020/objects` | Text rows identify the class, U/V position and resource index | Place trees, cliffs, tracks, fences, bridge spans and other supported scenery |
| `maps.src/map.020/mis.000/mapunits` | Unit ID, group, HP, ammunition, direction and U/V fields. Passengers lack independent placement. | Place and pick 88 infantry with explicit coordinates |
| `desc_common.sue` and `game_common.sue` | The unit definition names the sprite file. The infantry RS2 file starts with a counted frame group. | Draw the first stored pose with the SOLD.PL base palette and the signed sprite origin |
| `maps/single/--- TUTORIAL I ---.SSM` | A gzip stream with an MS2S header, version 0x222, and records with a 12-byte padded name and a 32-bit payload length. I-JPG1024 contains dimensions, stride and a JPEG. | Independently load the original 1024 × 512 mission overview and check feature alignment |

The projection uses `x = 4096 + U - V` and `y = (U + V) / 2`. As a check, we projected 843 tree positions into the embedded mission overview. The surrounding canopy contrast was strongest at this orientation and an exact 1:8 scale. The opposite orientations produced a weaker match. This verifies orientation and scale across the scene. It leaves the exact pixel coverage and shading of individual tiles open.

The resource index gives the record boundaries for scenery. The first supported sprite starts at byte 29 in the sampled tree records, 8 for cliffs, 5 for lay, 4 for fences and 0 for fre, trop and vor. Tutorial I uses two bridge records with a record-local palette and a first sprite at byte 2004. The renderer checks the index, bounds, tag, dimensions, row lengths and encoded runs. WarStorm's N2CAD800/Renderer.cxx and WSX/BinAsset.hxx identify the compact and extended indexed run operations. The current view omits houses, stand objects, vehicles and artillery. Base infantry poses, palette variants, depth ordering and selection rectangles still need a retail frame comparison.

The private players file names owner 0 as the player. The camera opens on that owner’s placed officer. A click selects the topmost drawn infantry bounding box at the pointer. Left drag selects player infantry whose placement coordinates fall in the rectangle. Double-click uses type, owner and viewport bounds, Ctrl extends it map-wide, and Shift toggles membership. The manual’s “Selecting Units” section documents these controls. Single-clicking an enemy also lets the user inspect its recorded state. The resulting title reads fields from the mission source. Gold's tutorial text explicitly instructs the player to left-click units. The native rectangular hit test, camera speeds and Tab navigation are inspector controls. The view does not execute mission scripts, change unit state, or establish the original simulation tick. Rendering uploads the assembled image once and blits it each frame. A tile-based renderer is needed before extending this approach to larger maps.
