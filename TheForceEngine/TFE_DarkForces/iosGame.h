#pragma once
//////////////////////////////////////////////////////////////////////
// iOS (TFE_IOS only): Dark Forces' side of the touch controls (the
// Serious Sam port's overlay, ios/IOSTouch.m): what the overlay shows and
// what its buttons, stick, look drag, tilt aiming, weapon wheel, cheat and
// CAMERA do in the game.
//////////////////////////////////////////////////////////////////////
#include <TFE_System/types.h>

struct RSector;
struct SecObject;
struct IOSAutoplayOptions;
struct IOSAutoplayDisplay;

namespace TFE_DarkForces
{
	// Once a frame, after SDL's events (the overlay's touches come in with
	// them) and the game's input mapping, before the game runs: shows the
	// overlay for what the game is doing (inMission: a mission is the game's
	// state), and hands the game what the touches asked for.
	void iosGame_frame(bool inMission, u32 framesDrawn);

	// (the input mapping) the look drag and tilt aiming since the last call,
	// in mouse counts
	void iosGame_takeLook(s32* dx, s32* dy);

	// The app going to the background, or the game ending: the overlay hides
	void iosGame_hide();

	// At startup, once the window (and so the GPU) is up: HD on, once per
	// install, where it can be
	void iosGame_hdByDefault();

	// (the mission's frame) CAMERA's frame grabs: the frame starts, the 3D
	// view and the weapon are drawn, the HUD is drawn
	void iosGame_frameBegin();
	void iosGame_afterScene();
	void iosGame_afterHud();
	// (the render backend) the game's frame is on the screen, before the UI
	void iosGame_screenDrawn();

	// (the mission's frame, iosCamera.cpp) Draws the 3D view: from the
	// player's eyes, or from CAMERA's photo view while a selfie, Action
	// Freeze or the cinematic burst has it (true: then no first-person
	// weapon and no HUD are drawn)
	bool iosCamera_drawWorld(u8* display, RSector* eyeSector, const u8* colormap, const u8* lightSourceRamp);
	bool iosCamera_photoView();

	// AUTOPLAY (iosAutoplay.cpp): the self-playing show from MENU's tray.
	// iosGame.cpp starts and stops it and, while it is on, hands the game
	// its controls in place of the touches'.
	void iosAutoplay_start(const IOSAutoplayOptions* options);
	// (restore: back to the game as it was when it started, from the save
	// it made then)
	void iosAutoplay_stop(bool restore);
	bool iosAutoplay_on();
	// (a double tap) a new fight: another level (SHUFFLE ALL), or another
	// group in this one (PICK ONE); nothing while a level loads
	void iosAutoplay_next();
	// The game's key for a weapon (WeaponID): keys 1 to 0 in the game's
	// order, which has the mines (7) before the mortar (8), as the WeaponIDs
	// don't
	s32 iosGame_weaponKey(s32 weapon);

	// (iosGame_frame, after the game's input mapping) its stick, buttons
	// and weapon picks into this frame's input
	void iosAutoplay_input();
	// (iosGame_takeLook) its turning, in mouse counts
	void iosAutoplay_takeLook(f32* dx, f32* dy);
	// (iosGame_frame, for the overlay) what it does, for CONTROLS ON to show
	void iosAutoplay_display(IOSAutoplayDisplay* display);
	// (the mission's main task, once a frame while the game runs, where the
	// cheats run) its scenes, cuts, crowds and alarms, and the autopilot
	void iosAutoplay_gameTick();
	// (the mission's frame) the HUD is drawn
	bool iosAutoplay_hudShown();
	// (obj_setEnemyLogic) an enemy of this kind (its KEYWORD) set up
	void iosAutoplay_noteEnemy(SecObject* obj, s32 logicId);
	// (the mission, as a level loads) the difficulty it loads at
	u8 iosAutoplay_levelDifficulty(u8 difficulty);
	// (the game, as a mission ends) the level AUTOPLAY goes on to (1..), or
	// 0 when it isn't playing
	s32 iosAutoplay_nextLevel();
}
