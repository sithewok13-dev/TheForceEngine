#ifdef TFE_IOS
#include "touchControls.h"
#include "input.h"
#include "inputMapping.h"
#include <TFE_Ui/imGUI/imgui.h>
#include <SDL.h>
#include <math.h>

using namespace TFE_Input;

namespace TFE_TouchControls
{
	// The screen's edges kept clear: the notch or Dynamic Island at the sides
	// in landscape, the home bar at the bottom.
	static const f32 c_sideInset   = 56.0f;
	static const f32 c_bottomInset = 24.0f;
	static const f32 c_topInset    = 12.0f;

	static const f32 c_stickRadius = 56.0f;		// a full push
	static const f32 c_stickRun    = 0.85f;		// pushed this far, run
	static const f32 c_lookSens    = 1.5f;		// SENS default
	static const u32 c_holdMs      = 300;		// QUICK SAVE / QUICK LOAD
	static const f32 c_pi          = 3.14159265f;

	enum ButtonKind
	{
		BTN_HOLD = 0,	// held while the finger is down
		BTN_TAP,		// pressed as the finger lands
		BTN_TIMED,		// pressed after a hold (with a ring); sliding off cancels
	};

	enum ButtonId
	{
		B_FIRE = 0,
		B_ALT,
		B_JUMP,
		B_CROUCH,
		B_USE,
		B_NEXT_WPN,
		B_MENU,
		B_QUICK_SAVE,
		B_QUICK_LOAD,
		B_MAP,
		B_COUNT
	};

	struct Button
	{
		const char* label;
		InputAction action;
		ButtonKind kind;
		bool round;
		// placed by layout()
		f32 x, y;		// center
		f32 w, h;		// size (a round button: w = h = diameter)
		// state
		s32 finger;		// the finger index holding it, or -1
		bool pressed;	// a press waiting for the game's input frame
		u32 downTime;
		bool fired;		// a timed button's action went off
	};

	static Button s_buttons[B_COUNT] =
	{
		{ "FIRE",       IADF_PRIMARY_FIRE,   BTN_HOLD,  true },
		{ "ALT",        IADF_SECONDARY_FIRE, BTN_HOLD,  true },
		{ "JUMP",       IADF_JUMP,           BTN_HOLD,  true },
		{ "CROUCH",     IADF_CROUCH,         BTN_HOLD,  true },
		{ "USE",        IADF_USE,            BTN_HOLD,  true },
		{ "NEXT WPN",   IADF_CYCLEWPN_NEXT,  BTN_TAP,   false },
		{ "MENU",       IADF_MENU_TOGGLE,    BTN_TAP,   false },
		{ "QUICK SAVE", IAS_QUICK_SAVE,      BTN_TIMED, false },
		{ "QUICK LOAD", IAS_QUICK_LOAD,      BTN_TIMED, false },
		{ "MAP",        IADF_AUTOMAP,        BTN_TAP,   false },
	};

	enum FingerRole { ROLE_NONE = 0, ROLE_STICK, ROLE_LOOK, ROLE_BUTTON };
	struct Finger
	{
		SDL_FingerID id;
		FingerRole role;
		s32 button;
		f32 x, y;
	};
	enum { MAX_FINGERS = 10 };
	static Finger s_fingers[MAX_FINGERS] = {};

	static bool s_active = false;
	static f32 s_width = 0.0f, s_height = 0.0f;	// the screen, in points
	static f32 s_stickX = 0.0f, s_stickY = 0.0f;	// where the stick started
	static f32 s_stickDx = 0.0f, s_stickDy = 0.0f;	// -1..1
	static f32 s_lookX = 0.0f, s_lookY = 0.0f;		// look drag not yet taken

	static void layout()
	{
		const f32 r = s_width - c_sideInset;
		const f32 b = s_height - c_bottomInset;
		const f32 top = c_topInset;
		struct Place { f32 x, y, w, h; };
		const Place place[B_COUNT] =
		{
			{ r - 60.0f,  b - 115.0f, 92.0f, 92.0f },	// FIRE (above the ammo display)
			{ r - 60.0f,  b - 205.0f, 52.0f, 52.0f },	// ALT
			{ r - 160.0f, b - 50.0f,  62.0f, 62.0f },	// JUMP
			{ r - 235.0f, b - 34.0f,  50.0f, 50.0f },	// CROUCH
			{ r - 165.0f, b - 140.0f, 56.0f, 56.0f },	// USE
			{ r - 50.0f,  top + 18.0f, 96.0f, 36.0f },	// NEXT WPN
			{ c_sideInset + 40.0f,  top + 18.0f, 76.0f, 36.0f },	// MENU
			{ c_sideInset + 130.0f, top + 18.0f, 96.0f, 36.0f },	// QUICK SAVE
			{ c_sideInset + 236.0f, top + 18.0f, 96.0f, 36.0f },	// QUICK LOAD
			{ c_sideInset + 326.0f, top + 18.0f, 64.0f, 36.0f },	// MAP
		};
		for (s32 i = 0; i < B_COUNT; i++)
		{
			s_buttons[i].x = place[i].x;
			s_buttons[i].y = place[i].y;
			s_buttons[i].w = place[i].w;
			s_buttons[i].h = place[i].h;
		}
	}

	static bool inButton(const Button& btn, f32 x, f32 y, f32 slop)
	{
		if (btn.round)
		{
			const f32 dx = x - btn.x, dy = y - btn.y;
			const f32 r = btn.w * 0.5f + slop;
			return dx*dx + dy*dy <= r*r;
		}
		return fabsf(x - btn.x) <= btn.w * 0.5f + slop && fabsf(y - btn.y) <= btn.h * 0.5f + slop;
	}

	static void releaseAll()
	{
		for (s32 i = 0; i < MAX_FINGERS; i++) { s_fingers[i].role = ROLE_NONE; }
		for (s32 i = 0; i < B_COUNT; i++) { s_buttons[i].finger = -1; s_buttons[i].fired = false; }
		s_stickDx = s_stickDy = 0.0f;
		s_lookX = s_lookY = 0.0f;
		setAxis(AXIS_LEFT_X, 0.0f);
		setAxis(AXIS_LEFT_Y, 0.0f);
	}

	void setActive(bool active)
	{
		if (active == s_active) { return; }
		s_active = active;
		releaseAll();	// (also sets up the buttons as free)
	}

	bool isActive()
	{
		return s_active;
	}

	bool handleFinger(const SDL_Event* event)
	{
		const SDL_TouchFingerEvent& tf = event->tfinger;
		// a finger that came down on the controls stays theirs until it lifts
		s32 index = -1;
		for (s32 i = 0; i < MAX_FINGERS; i++)
		{
			if (s_fingers[i].role != ROLE_NONE && s_fingers[i].id == tf.fingerId) { index = i; break; }
		}
		if (!s_active && index < 0) { return false; }
		if (s_width <= 0.0f || s_height <= 0.0f) { return s_active; }

		const f32 x = tf.x * s_width;
		const f32 y = tf.y * s_height;
		if (event->type == SDL_FINGERDOWN)
		{
			if (!s_active) { return false; }
			for (s32 i = 0; i < MAX_FINGERS && index < 0; i++)
			{
				if (s_fingers[i].role == ROLE_NONE) { index = i; }
			}
			if (index < 0) { return true; }

			Finger& f = s_fingers[index];
			f.id = tf.fingerId;
			f.x = x;
			f.y = y;
			f.button = -1;
			for (s32 b = 0; b < B_COUNT; b++)
			{
				if (s_buttons[b].finger < 0 && inButton(s_buttons[b], x, y, 6.0f))
				{
					f.button = b;
					break;
				}
			}
			if (f.button >= 0)
			{
				Button& btn = s_buttons[f.button];
				f.role = ROLE_BUTTON;
				btn.finger = index;
				btn.pressed = true;
				btn.fired = false;
				btn.downTime = SDL_GetTicks();
			}
			else
			{
				// the left side moves (one stick at a time), anywhere else looks
				bool stickTaken = false;
				for (s32 i = 0; i < MAX_FINGERS; i++) { stickTaken |= (s_fingers[i].role == ROLE_STICK); }
				f.role = (x < s_width * 0.4f && !stickTaken) ? ROLE_STICK : ROLE_LOOK;
			}
			if (f.role == ROLE_STICK)
			{
				s_stickX = x;
				s_stickY = y;
				s_stickDx = s_stickDy = 0.0f;
			}
			return true;
		}
		// a finger that came down before the controls were up stays the mouse's
		if (index < 0) { return false; }

		Finger& f = s_fingers[index];
		if (event->type == SDL_FINGERMOTION)
		{
			if (f.role == ROLE_STICK)
			{
				f32 dx = (x - s_stickX) / c_stickRadius;
				f32 dy = (y - s_stickY) / c_stickRadius;
				const f32 len = sqrtf(dx*dx + dy*dy);
				if (len > 1.0f) { dx /= len; dy /= len; }
				s_stickDx = dx;
				s_stickDy = dy;
			}
			else if (f.role == ROLE_LOOK)
			{
				s_lookX += (x - f.x) * c_lookSens;
				s_lookY += (y - f.y) * c_lookSens;
			}
			else if (f.role == ROLE_BUTTON && s_buttons[f.button].kind == BTN_TIMED &&
				!inButton(s_buttons[f.button], x, y, 12.0f))
			{
				// sliding off a QUICK SAVE / QUICK LOAD cancels it
				s_buttons[f.button].finger = -1;
				f.role = ROLE_LOOK;
			}
			f.x = x;
			f.y = y;
		}
		else if (event->type == SDL_FINGERUP)
		{
			if (f.role == ROLE_STICK)
			{
				s_stickDx = s_stickDy = 0.0f;
			}
			else if (f.role == ROLE_BUTTON)
			{
				s_buttons[f.button].finger = -1;
			}
			f.role = ROLE_NONE;
		}
		return true;
	}

	void apply()
	{
		if (!s_active) { return; }

		// the stick (a controller's left stick: up is +1)
		setAxis(AXIS_LEFT_X, s_stickDx);
		setAxis(AXIS_LEFT_Y, -s_stickDy);
		if (sqrtf(s_stickDx*s_stickDx + s_stickDy*s_stickDy) >= c_stickRun)
		{
			inputMapping_setStateDown(IADF_RUN);
		}

		// QUICK SAVE / QUICK LOAD go off after the hold (QUICK SAVE wins a tie)
		const u32 now = SDL_GetTicks();
		bool savedNow = false;
		for (s32 b = B_QUICK_SAVE; b <= B_QUICK_LOAD; b++)
		{
			Button& btn = s_buttons[b];
			if (btn.finger >= 0 && !btn.fired && now - btn.downTime >= c_holdMs)
			{
				btn.fired = true;
				if (b == B_QUICK_LOAD && savedNow) { continue; }
				inputMapping_setStatePress(btn.action);
				savedNow |= (b == B_QUICK_SAVE);
			}
		}

		for (s32 b = 0; b < B_COUNT; b++)
		{
			const Button& btn = s_buttons[b];
			if (btn.kind == BTN_TIMED) { continue; }
			if (btn.pressed)
			{
				inputMapping_setStatePress(btn.action);
			}
			else if (btn.kind == BTN_HOLD && btn.finger >= 0)
			{
				inputMapping_setStateDown(btn.action);
			}
		}
	}

	void endFrame()
	{
		for (s32 b = 0; b < B_COUNT; b++) { s_buttons[b].pressed = false; }
	}

	void takeLook(s32* dx, s32* dy)
	{
		const s32 ix = s32(s_lookX), iy = s32(s_lookY);
		s_lookX -= f32(ix);
		s_lookY -= f32(iy);
		*dx = ix;
		*dy = iy;
	}

	void draw(ImDrawList* drawList, f32 width, f32 height)
	{
		if (width != s_width || height != s_height)
		{
			s_width = width;
			s_height = height;
			layout();
		}
		if (!s_active) { return; }

		const ImU32 fill     = IM_COL32(20, 20, 20, 90);
		const ImU32 fillDown = IM_COL32(200, 200, 200, 110);
		const ImU32 edge     = IM_COL32(255, 255, 255, 120);
		const ImU32 text     = IM_COL32(255, 255, 255, 200);
		const ImU32 ring     = IM_COL32(255, 210, 60, 230);
		const u32 now = SDL_GetTicks();

		for (s32 b = 0; b < B_COUNT; b++)
		{
			const Button& btn = s_buttons[b];
			const bool down = btn.finger >= 0;
			const ImVec2 c(btn.x, btn.y);
			if (btn.round)
			{
				drawList->AddCircleFilled(c, btn.w * 0.5f, down ? fillDown : fill, 32);
				drawList->AddCircle(c, btn.w * 0.5f, edge, 32, 1.5f);
			}
			else
			{
				const ImVec2 p0(btn.x - btn.w * 0.5f, btn.y - btn.h * 0.5f), p1(btn.x + btn.w * 0.5f, btn.y + btn.h * 0.5f);
				drawList->AddRectFilled(p0, p1, down ? fillDown : fill, 8.0f);
				drawList->AddRect(p0, p1, edge, 8.0f, 0, 1.5f);
			}
			const ImVec2 size = ImGui::CalcTextSize(btn.label);
			drawList->AddText(ImVec2(btn.x - size.x * 0.5f, btn.y - size.y * 0.5f), text, btn.label);

			// the hold's ring
			if (btn.kind == BTN_TIMED && down && !btn.fired)
			{
				const f32 t = fminf(1.0f, f32(now - btn.downTime) / f32(c_holdMs));
				drawList->PathArcTo(c, btn.h * 0.5f + 5.0f, -c_pi * 0.5f, -c_pi * 0.5f + t * 2.0f * c_pi, 40);
				drawList->PathStroke(ring, 0, 3.0f);
			}
		}

		// the stick, while the left thumb is down
		for (s32 i = 0; i < MAX_FINGERS; i++)
		{
			if (s_fingers[i].role != ROLE_STICK) { continue; }
			drawList->AddCircleFilled(ImVec2(s_stickX, s_stickY), c_stickRadius, fill, 32);
			drawList->AddCircle(ImVec2(s_stickX, s_stickY), c_stickRadius, edge, 32, 1.5f);
			drawList->AddCircleFilled(ImVec2(s_stickX + s_stickDx * c_stickRadius, s_stickY + s_stickDy * c_stickRadius), 22.0f, fillDown, 24);
			break;
		}
	}
}
#endif
