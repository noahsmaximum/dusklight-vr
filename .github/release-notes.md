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

## What's new in 0.3.1

- Quest: Dusklight's launch menu (disc selection, settings, mods) now shows in the headset from the first launch. Before, a first launch only showed loading dots
- Quest: tabletop now shows your room around the diorama (passthrough is enabled in the app)

## What's new in 0.3.0

- **Quest 3 standalone support** (experimental) through the Dusklight VR edition APK: stereo, cinema, tabletop and Dusklight's own menus in the headset
- **Water**: the last "portal" water (the river in Ordon Village) is fixed
- Quest performance: stereo holds 36 fps at 60% render scale (the Quest default), cinema mostly 72 fps
- Windows: a few shared-code fixes from the Android work; behaviour otherwise unchanged

## Known issues

- Quest: stereo runs at 36 fps (head rotation is reprojected at 72 Hz); motion-vector frame generation (SpaceWarp) is not in yet
- Quest: pause/resume is untested; tabletop runs at about 22 fps
- Virtual Desktop doesn't offer passthrough to apps; use a chroma-key background colour instead
- Frosted-glass blur behind Dusklight menus is off while in VR
- Pause-menu 3D item models look flat
- Depth of field is off by default; motion blur is always off in VR
