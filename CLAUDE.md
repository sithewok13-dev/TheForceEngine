# Dark Forces for iOS (The Force Engine): notes for Claude Code sessions

An unofficial iPhone/iPad build of The Force Engine (TFE) playing Dark Forces,
app name **Dark Forces**, bundle id `com.sithewok.darkforces`. The owner plays
it on an iPhone (iOS 18+), isn't a programmer, and reads everything on a phone.
They own both the original Dark Forces and Nightdive's Dark Forces Remaster
(Steam/GOG); the Remaster's files are the test priority.

- **Build branch:** `ios`. `master` is left as upstream (luciusDXL/TheForceEngine).
- **Stage 1 (done):** the app starts, finds the player's files, plays in TFE's
  software renderer at full screen with sound and music, menus by touch (SDL's
  touch-as-mouse, drag-to-scroll in ImGui windows). **Stage 2 (in progress):**
  touch game controls, following the standard below. The finger is the
  game's mouse in its menus (main.cpp, display pixels; the Agent menu and
  briefing map it onto their 4:3 screen), a tap skips cutscenes, the game
  starts at the Agent menu (no intro), the agent name box brings up the
  keyboard. While a mission is played, the controls are the Serious Sam
  port's overlay, copied: `ios/IOSTouch.m` / `.h` (C core + UIKit view), with
  Dark Forces' buttons (ALT in ZOOM's place, MAP top left) and weapon wheel;
  `TFE_DarkForces/iosGame.cpp` is the game side (mode, weapons, holding the
  game still, actions after the input mapping, look and tilt aiming into the
  mouse motion, HUD offsets from the overlay's HUD frame, the sithewok cheat
  on TFE's own cheats: god mode, LAMAXOUT, ammo kept full, fly, noclip,
  level warps; Kyle's lines). CAMERA: `ios/IOSCapture.c`, `IOSPhotos.m`,
  `IOSSelfieShots.c` (from Sam) with grabs of the 8-bit frame
  (`IOS_ReadFrameRGBA`); selfies, Action Freeze and the cinematic burst are
  `TFE_DarkForces/iosCamera.cpp` (a photo view drawn from its own camera
  through `iosCamera_drawWorld` in `mission.cpp`, no weapon or HUD; poses,
  hiding and moves applied only while that view is drawn and put back right
  after, every frame). In selfies Kyle is the cutscene art from the
  player's own BRIEF2.LFD (the arms-crossed bust), by the lens (true colour into the
  saved photo via `iosCamera_compositeRGBA`, nearest colours on the software
  renderer's screen; not on screen with the GPU renderer), with a mood per
  photo; Action Freeze and the cinematic burst draw the player as
  HUMAN3.3DO coloured as Kyle.
- **iOS code:**
  - `ios/`: the app around the engine. `ios_app.cmake` (bundle, Info.plist,
    icon, support files, ANGLE frameworks), `Info.plist.in`, `Assets.xcassets`,
    `IOSGameData.m` (first-launch screen, finding the game folder),
    `iosAngle.cpp/.h` (OpenGL ES 3.0 context from ANGLE on SDL's Metal view; the
    `SDL_GL_*` calls in the render backend become these), `iosApp.h`,
    `build-deps.sh` (SDL2 + SDL2_image for iOS), `toolchain.cmake`.
  - Engine changes are `#ifdef TFE_IOS` (app, paths, defaults, touch menus) or
    `#ifdef TFE_GLES` (OpenGL ES 3.0 path in `TFE_RenderBackend/Win32OpenGL`:
    shims at the end of `gl.h`, glad loads all entry points, shader header in
    `shader.cpp`, caps in `openGL_Caps.cpp`; ImGui's ES3 backend via `imconfig.h`).
  - Paths on iOS: game files anywhere in Documents (the folder with DARK.GOB, up
    to two levels down); settings, saves, screenshots and the log in
    `Documents/TheForceEngine/`; temp files in Application Support.
  - The GPU renderer (and with it true color and the Remaster's HD textures,
    sprites and HUD) runs on ES 3.0: its texture buffers are 2D textures there
    (`shaderBuffer.cpp`, read by `tfeBufferFetch()` from `shader.cpp`'s GLES
    header), its clip distances come from GL_ANGLE_clip_cull_distance (ANGLE on
    Metal) or GL_EXT_clip_cull_distance (Mesa), and `noperspective` is emulated
    (`TFE_GLES` in the model shader). Without the clip extension it falls back to
    the software renderer, which stays the iOS default. HD (OGV) cutscenes,
    system MIDI and the editor are off.
- **CI:** `.github/workflows/ios.yml` (macOS): ANGLE built once and cached
  (`ANGLE_REV`, same revision as the DOOM 3 and Jedi Knight ports), deps cached,
  artifacts `DarkForces-iOS-unsigned.ipa` and `build-log`; fails if game data is
  in the app. Upstream's `ubuntu-gcc.yml` only runs on `master`.
- **Docs:** `docs/ios/README.md` (install and where the game files go).

## Testing without a Mac

- Desktop build: `cmake -S . -B /tmp/tfe-linux -G Ninja` (from the build dir:
  upstream's CMake symlinks the support folders into the current directory).
- GLES test build (the iOS GL path on Linux, Mesa under Xvfb):
  `-DCMAKE_CXX_FLAGS=-DTFE_GLES=1 -DCMAKE_C_FLAGS=-DTFE_GLES=1
  -DCMAKE_CXX_STANDARD_LIBRARIES=-lGLESv2`, run with `TFE_DATA_HOME=<dir>` and
  `xvfb-run -s "-screen 0 852x393x24"` (an iPhone's screen in points).
- Shaders: check the software path's (blit, overlay, bloom) as GLSL ES 3.00 with
  `glslangValidator`, with the same `#version 300 es` + precision header.
- Objective-C and the ANGLE file only compile in CI. No game data is available
  in the cloud: gameplay is only tested by the owner on the device.

## Rules

- **Game data:** never commit, upload, bundle or publish game files (GOB, LFD,
  LAB, the Remaster's KPF, videos, installers, zips), here or anywhere public.
  Work on local copies only.
- **Commits:**
  - Identity `Claude <noreply@anthropic.com>` (already configured); never the
    owner's email or name. Subject `iOS: ...`, then a plain wrapped body.
  - End with the attribution trailer lines your session provides. Apart from
    those, no AI model names in commits, code or docs.
  - No PRs unless asked. Never stash, reset or check out over a working tree that
    another job may be editing. Push with `git push -u origin ios`.
- **Releases:** none yet. Before any public release or public text, build a
  preview and wait for the owner's explicit go.
- **Touch controls standard** (shared with the owner's other ports; keep it the
  same in stage 2):
  - Move stick where the left thumb lands; drag elsewhere to look.
  - QUICK SAVE / QUICK LOAD: a 0.3 s hold with a ring. Sliding off cancels;
    QUICK SAVE wins a tie.
  - MENU: tap = game menu; hold 0.45 s = tray with SENS, GYRO, FPS, keyboard.
  - GYRO: OFF (default) / TOUCH (aims while either thumb is down; lifting both
    freezes the view) / ALWAYS. SENS 1.0/1.5/2.0/3.0, default 1.5. Remembered.
  - NEXT WPN: tap = next; a 10 pt slide opens the weapon wheel at once; a 0.3 s
    hold opens it to tap. The gap at the bottom cancels. The game holds still
    while a wheel is open. A haptic tick per slice.
  - HUD and buttons stay clear of the corners, the notch or Dynamic Island, and
    the home bar.
- **Testing:** say plainly what was and wasn't tested. The owner tests on the
  device.
- **Lean mode:** small contexts, no full log or file dumps (grep/sed/head/tail),
  reuse builds.
- **Talking to the owner:** plain words, short messages, direct links, and
  honesty about mistakes and untested things. If they skip a question, choose a
  sensible default and say what you chose.
