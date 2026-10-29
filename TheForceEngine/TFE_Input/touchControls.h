#pragma once
//////////////////////////////////////////////////////////////////////
// iOS touch controls for playing (TFE_IOS only): a move stick where the
// left thumb lands, a drag elsewhere to look, and buttons, drawn over
// the game while a mission is being played (not in its menus).
//////////////////////////////////////////////////////////////////////
#include <TFE_System/types.h>

union SDL_Event;
struct ImDrawList;

namespace TFE_TouchControls
{
	// Whether the controls are up (a mission is being played, no menu open).
	void setActive(bool active);
	bool isActive();

	// A finger event; true when the controls took it (while they're up).
	bool handleFinger(const SDL_Event* event);

	// After the game's input mapping each frame: the stick, buttons and
	// actions they hold or pressed.
	void apply();
	// When the game's input frame ends (presses last one input frame).
	void endFrame();

	// The look drag since the last call, in mouse units.
	void takeLook(s32* dx, s32* dy);

	// Draws the controls (in points, the size of the screen).
	void draw(ImDrawList* drawList, f32 width, f32 height);
}
