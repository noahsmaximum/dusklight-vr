## Install

**Windows (PC VR)**
1. You need **Dusklight v2.0** on Windows, using the **D3D12** graphics backend (the default).
2. Download `dusklight_vr.dusk` below and copy it into your Dusklight `mods` folder (e.g. `%APPDATA%\TwilitRealm\Dusklight\mods`), replacing any older version.
3. Start your VR runtime (SteamVR, Virtual Desktop, Quest Link, ...), then launch Dusklight.
4. Enable **Dusklight VR** in the Mods window. Pick a **preset** in its panel, then use **Recenter view**.

**Quest 3 (standalone, experimental)**
1. Enable developer mode and sideload `dusklight-vr-edition-arm64.apk` (SideQuest or `adb install`). The VR mod is already inside. It installs beside the official Dusklight app as **Dusklight VR** and keeps its own saves.
2. Copy your Twilight Princess (USA) disc image to the headset (e.g. its `Download` folder).
3. Launch **Dusklight VR** from the library (Unknown Sources). Dusklight's launch menu shows in the headset: select your disc image there and start the game. It's remembered for later launches.

## What's new in 1.0

- **Tabletop x-ray**: when the level hides Link, a cylinder of clear view opens from your eyes to him (roofs, walls, trees). The ground he stands on, Link himself, NPCs and enemies are never cut; scenery right in front of your eyes fades out
- **Tabletop HUD** lies flat on the table around the diorama
- **Quick item wheel** in tabletop: opens around Link without pausing; steer it with the right stick while Link keeps walking. The pausing wheel is still an option
- **Aim lines**: a dashed, glowing line in the item's colour from the item to where the shot lands (bow, slingshot, clawshot, dominion rod, boomerang), replacing the flat crosshair. Lock-on arrows are drawn around targets in 3D
- **Hawkeye** shows its zoomed view on a 3D screen in stereo and tabletop
- **Stereo aiming** stays third person, over Link's shoulder
- **Camera turns only when you turn it** (option, on): no more automatic swinging in stereo; Z-targeting, cutscenes and fixed-angle spots still move it
- Scene transitions fade through **black** instead of white flashes
- Stereo: real-time shadows are back (the widened culling frustum was culling them), and small objects' shadows no longer show through walls in the right eye
- **VR settings window** with tabs (General, View, Tabletop, HUD & menus, Advanced), opened from **VR** in Dusklight's top bar

## Known issues

- Quest: the new tabletop features (x-ray, aim lines, quick wheel, Hawkeye screen) are tested on Windows only so far
- Quest: stereo runs at 36 fps (head rotation is reprojected at 72 Hz); motion-vector frame generation (SpaceWarp) is not in yet
- Quest: pause/resume is untested; tabletop runs at about 22 fps
- Virtual Desktop doesn't offer passthrough to apps; use a chroma-key background colour instead
