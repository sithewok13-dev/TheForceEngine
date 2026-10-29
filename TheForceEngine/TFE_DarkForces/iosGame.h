#pragma once
//////////////////////////////////////////////////////////////////////
// iOS (TFE_IOS only): Dark Forces' side of the touch controls (the
// Serious Sam port's overlay, ios/IOSTouch.m): what the overlay shows and
// what its buttons, stick, look drag, tilt aiming, weapon wheel, cheat and
// CAMERA do in the game.
//////////////////////////////////////////////////////////////////////
#include <TFE_System/types.h>

struct RSector;

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
}
