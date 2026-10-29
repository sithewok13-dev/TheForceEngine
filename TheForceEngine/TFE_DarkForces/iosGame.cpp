#ifdef TFE_IOS
//////////////////////////////////////////////////////////////////////
// iOS: Dark Forces' side of the touch controls. See iosGame.h.
//////////////////////////////////////////////////////////////////////
#include "iosGame.h"
#include "mission.h"
#include "player.h"
#include "agent.h"
#include "weapon.h"
#include "cheats.h"
#include "sound.h"
#include "GameUI/escapeMenu.h"
#include "GameUI/pda.h"
#include "Landru/cutscene.h"
#include <TFE_Input/input.h>
#include <TFE_Input/inputMapping.h>
#include <TFE_FrontEndUI/frontEndUi.h>
#include <TFE_Settings/settings.h>
#include <TFE_Jedi/Renderer/virtualFramebuffer.h>
#include <TFE_Jedi/Renderer/rcommon.h>
#include <TFE_Jedi/Level/rtexture.h>
#include <TFE_Asset/spriteAsset_Jedi.h>
#include <SDL.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

#include <IOSTouch.h>
#include <IOSCheat.h>
#include <IOSCheatGame.h>
#include <IOSCapture.h>
#include <IOSSelfie.h>
#include <IOSFreeze.h>
#include <IOSCinema.h>

using namespace TFE_Input;
using namespace TFE_Jedi;

namespace TFE_DarkForces
{
	extern void pauseLevelSound();
	extern void resumeLevelSound();
	extern void giveAllInventoryAndHealth();	// (player.cpp: LAMAXOUT's)
	// (mission.cpp) a cheat, run as the game's next unpaused frame starts
	extern void mission_iosQueueCheat(s32 cheatId);

	// One count of mouse movement turns the view 8/16384 of a turn at the
	// default sensitivity (player.cpp: PLAYER_MOUSE_TURN_SPD); tilt aiming
	// comes in degrees.
	static const f32 c_degPerCount = 8.0f * 360.0f / 16384.0f;

	static s32  s_mode = IOSTOUCH_HIDDEN;
	static bool s_holding = false;		// the game held still for the overlay (the weapon wheel...)
	static bool s_typing = false;		// MENU's tray's keyboard: typing the game's cheats
	static u32  s_heldPrev = 0;			// the overlay's buttons held last frame
	static f32  s_lookX = 0.0f, s_lookY = 0.0f;	// look not handed over yet (fractions of a count)
	static u32  s_world = 1;			// changes with each level started or loaded
	static GameMissionMode s_missionModePrev = MISSION_MODE_LOADING;
	static unsigned long s_cheatPrev = 0;
	static char s_typed[16] = "";		// the last letters typed (the cheat's word)

	// The overlay's buttons as the game's actions
	struct ButtonAction { u32 bit; InputAction action; };
	static const ButtonAction c_buttonActions[] =
	{
		{ IOSTOUCH_FIRE,       IADF_PRIMARY_FIRE },
		{ IOSTOUCH_ZOOM,       IADF_SECONDARY_FIRE },	// ALT
		{ IOSTOUCH_USE,        IADF_USE },
		{ IOSTOUCH_JUMP,       IADF_JUMP },
		{ IOSTOUCH_CROUCH,     IADF_CROUCH },
		{ IOSTOUCH_NEXTWEAPON, IADF_CYCLEWPN_NEXT },
		{ IOSTOUCH_PREVWEAPON, IADF_CYCLEWPN_PREV },
	};

	static bool playerThere()
	{
		return s_playerObject != nullptr && s_missionMode == MISSION_MODE_MAIN;
	}

	// What the overlay shows: the controls while a mission is being played,
	// nothing in its menus (they take taps as mouse clicks)
	static s32 overlayMode(bool inMission)
	{
		if (!inMission || !playerThere() || escapeMenu_isOpen() || pda_isOpen() || cutscene_isPlaying() ||
			TFE_FrontEndUI::isConsoleOpen() || TFE_FrontEndUI::isConfigMenuOpen())
		{
			return IOSTOUCH_HIDDEN;
		}
		if (s_typing) { return IOSTOUCH_CONSOLE; }
		if (s_gamePaused && !s_holding) { return IOSTOUCH_PAUSED; }
		return IOSTOUCH_GAMEPLAY;
	}

	// The weapon wheel's view of the player's weapons (weapon numbers are
	// WeaponID + 1, as the overlay's Dark Forces map has them)
	static void getWeapons(IOSTouchWeapons* w)
	{
		memset(w, 0, sizeof(*w));
		for (s32 i = 0; i < IOSTOUCH_WPN_MAX; i++) { w->aiAmmo[i] = w->aiMaxAmmo[i] = -1; }
		if (!playerThere()) { return; }
		const PlayerInfo& p = s_playerInfo;
		w->bValid = 1;
		w->bCanSelect = !s_playerDying;
		w->iMap = IOSTOUCH_WHEEL_TSE;
		w->iWanted = p.newWeapon + 1;

		struct Slot { s32 id; JBool owned; s32 ammo; s32 maxAmmo; };
		const Slot slots[] =
		{
			{ WPN_FIST,        JTRUE,                    -1,              -1 },
			{ WPN_PISTOL,      p.itemPistol,             p.ammoEnergy,    s_ammoEnergyMax },
			{ WPN_RIFLE,       p.itemRifle,              p.ammoEnergy,    s_ammoEnergyMax },
			{ WPN_THERMAL_DET, p.ammoDetonator > 0,      p.ammoDetonator, s_ammoDetonatorMax },
			{ WPN_REPEATER,    p.itemAutogun,            p.ammoPower,     s_ammoPowerMax },
			{ WPN_FUSION,      p.itemFusion,             p.ammoPower,     s_ammoPowerMax },
			{ WPN_MORTAR,      p.itemMortar,             p.ammoShell,     s_ammoShellMax },
			{ WPN_MINE,        p.ammoMine > 0,           p.ammoMine,      s_ammoMineMax },
			{ WPN_CONCUSSION,  p.itemConcussion,         p.ammoPower,     s_ammoPowerMax },
			{ WPN_CANNON,      p.itemCannon,             p.ammoPlasma,    s_ammoPlasmaMax },
		};
		for (const Slot& s : slots)
		{
			const s32 n = s.id + 1;
			if (!s.owned) { continue; }
			w->ulOwned |= 1u << n;
			// (the assault cannon also fires its missiles)
			const bool ready = s.ammo < 0 || s.ammo > 0 || (s.id == WPN_CANNON && p.ammoMissile > 0);
			if (ready) { w->ulReady |= 1u << n; }
			w->aiAmmo[n] = s.ammo;
			w->aiMaxAmmo[n] = s.maxAmmo;
		}
	}

	// The weapon wheel's pictures: the game's own pickup sprites (the fists
	// and the pistol have none: their first-person view instead), in the
	// level's palette, cropped to what's drawn and scaled to fit 64 x 64
	// whole pixels (index 0 is see-through)
	static const char* c_weaponIcon[WPN_COUNT] =
	{
		"PUNCH1.BM", "PISTOL1.BM", "IST-GUNI.FME", "IDET.FME", "IAUTOGUN.FME",
		"IFUSION.FME", "IMORTAR.FME", "IMINE.FME", "ICONCUS.FME", "ICANNON.FME",
	};

	// An 8-bit picture, column by column, the bottom row first (as Dark
	// Forces keeps them)
	struct IconSource
	{
		s32 w = 0, h = 0;
		u8 pixels[256 * 256];
		u8 at(s32 x, s32 y) const { return pixels[x * h + y]; }
	};

	static bool loadIconSource(const char* name, IconSource* src)
	{
		const size_t len = strlen(name);
		if (len > 3 && strcasecmp(name + len - 3, ".BM") == 0)
		{
			TextureData* tex = bitmap_load(name, 1, POOL_GAME);
			if (!tex || !tex->image || tex->width > 256 || tex->height > 256) { return false; }
			src->w = tex->width;
			src->h = tex->height;
			memcpy(src->pixels, tex->image, (size_t)src->w * src->h);
			return true;
		}
		JediFrame* frame = TFE_Sprite_Jedi::getFrame(name, POOL_GAME);
		WaxCell* cell = frame ? WAX_CellPtr(frame, frame) : nullptr;
		if (!cell || cell->sizeX > 256 || cell->sizeY > 256 || cell->sizeY > WAX_DECOMPRESS_SIZE) { return false; }
		src->w = cell->sizeX;
		src->h = cell->sizeY;
		const u32* columns = (const u32*)((const u8*)frame + cell->columnOffset);
		const u8* image = (const u8*)cell + sizeof(WaxCell);
		u8 column[WAX_DECOMPRESS_SIZE];
		for (s32 x = 0; x < src->w; x++)
		{
			const u8* col = image + columns[x];
			if (cell->compressed == 1)
			{
				sprite_decompressColumn((const u8*)cell + columns[x], column, src->h);
				col = column;
			}
			memcpy(&src->pixels[x * src->h], col, src->h);
		}
		return true;
	}

	// (RGBA, not premultiplied, top row first; 0 x 0 if there is none)
	static void makeWeaponIcon(s32 weapon, u8* rgba, s32* outW, s32* outH)
	{
		*outW = *outH = 0;
		const u32* pal = vfb_getPalette();
		static IconSource src;
		if (weapon < 1 || weapon > WPN_COUNT || !pal || !loadIconSource(c_weaponIcon[weapon - 1], &src)) { return; }
		// what's drawn
		s32 x0 = src.w, y0 = src.h, x1 = -1, y1 = -1;
		for (s32 x = 0; x < src.w; x++)
		{
			for (s32 y = 0; y < src.h; y++)
			{
				if (!src.at(x, y)) { continue; }
				x0 = min(x0, x); x1 = max(x1, x);
				y0 = min(y0, y); y1 = max(y1, y);
			}
		}
		if (x1 < x0) { return; }
		const s32 cw = x1 - x0 + 1, ch = y1 - y0 + 1;
		// whole pixels up to fit, or evenly down
		const s32 up = max(1, 64 / max(cw, ch));
		const f32 down = max(1.0f, f32(max(cw, ch)) / 64.0f);
		const s32 w = (cw * up > 64) ? max(1, s32(cw / down)) : cw * up;
		const s32 h = (ch * up > 64) ? max(1, s32(ch / down)) : ch * up;
		for (s32 y = 0; y < h; y++)
		{
			for (s32 x = 0; x < w; x++)
			{
				const s32 sx = x0 + ((cw * up > 64) ? min(cw - 1, s32(x * down)) : x / up);
				const s32 sy = y0 + ((ch * up > 64) ? min(ch - 1, s32(y * down)) : y / up);
				const u8 index = src.at(sx, (y0 + y1) - sy);	// (top row first)
				u8* d = rgba + ((size_t)y * w + x) * 4;
				const u32 c = pal[index];
				d[0] = u8(c & 0xff); d[1] = u8((c >> 8) & 0xff); d[2] = u8((c >> 16) & 0xff);
				d[3] = index ? 255 : 0;
			}
		}
		*outW = w;
		*outH = h;
	}

	// The sithewok cheat, from the overlay's bits: god mode, everything given
	// as it comes on and as each level starts, ammo kept full; flying and
	// going through walls while those are on
	static void applyCheat(unsigned long bits, bool newWorld)
	{
		const bool on = (bits & IOSCHEAT_PLACT_ON) != 0;
		const bool wasOn = (s_cheatPrev & IOSCHEAT_PLACT_ON) != 0;
		if (on)
		{
			if (!wasOn || newWorld) { giveAllInventoryAndHealth(); }
			s_invincibility = -2;
			PlayerInfo& p = s_playerInfo;
			p.ammoEnergy = s_ammoEnergyMax;
			p.ammoPower = s_ammoPowerMax;
			p.ammoPlasma = s_ammoPlasmaMax;
			p.ammoDetonator = s_ammoDetonatorMax;
			p.ammoShell = s_ammoShellMax;
			p.ammoMine = s_ammoMineMax;
			p.ammoMissile = s_ammoMissileMax;
			const bool noclip = (bits & IOSCHEAT_PLACT_NOCLIP) != 0;
			s_noclip = noclip ? JTRUE : JFALSE;
			s_flyMode = (noclip || (bits & IOSCHEAT_PLACT_FLY)) ? JTRUE : JFALSE;
		}
		else if (wasOn)
		{
			// (what it gave is kept)
			if (s_invincibility == -2) { s_invincibility = 0; }
			s_flyMode = JFALSE;
			s_noclip = JFALSE;
		}
		s_cheatPrev = bits;
	}

	// The cheat's word, typed with MENU's tray's keyboard (as the game's own
	// cheats are typed)
	static void watchTyping()
	{
		const char* text = getBufferedText();
		for (const char* c = text; c && *c; c++)
		{
			const size_t len = strlen(s_typed);
			if (len + 1 >= sizeof(s_typed)) { memmove(s_typed, s_typed + 1, len); s_typed[len - 1] = 0; }
			const size_t n = strlen(s_typed);
			s_typed[n] = (char)tolower((unsigned char)*c);
			s_typed[n + 1] = 0;
			const size_t w = strlen(IOSCHEAT_WORD);
			if (strlen(s_typed) >= w && strcmp(s_typed + strlen(s_typed) - w, IOSCHEAT_WORD) == 0)
			{
				s_typed[0] = 0;
				IOSTouch_CheatTyped();
			}
		}
	}

	static void setTyping(bool typing)
	{
		if (typing == s_typing) { return; }
		s_typing = typing;
		setGameTextInput(typing);	// (TFE_Ui then keeps the iOS keyboard up)
		if (typing) { SDL_StartTextInput(); }
		else { SDL_StopTextInput(); }
	}

	static void setHold(bool hold)
	{
		if (hold == s_holding) { return; }
		if (hold)
		{
			if (s_gamePaused) { return; }	// (already held still)
			mission_pause(JTRUE);
			pauseLevelSound();
		}
		else
		{
			mission_pause(JFALSE);
			resumeLevelSound();
		}
		s_holding = hold;
	}

	void iosGame_frame(bool inMission, u32 framesDrawn)
	{
		const s32 mode = overlayMode(inMission);
		if (mode == IOSTOUCH_HIDDEN && s_typing) { setTyping(false); }

		// a level started or loaded
		bool newWorld = false;
		if (s_missionMode == MISSION_MODE_MAIN && s_missionModePrev != MISSION_MODE_MAIN)
		{
			s_world++;
			newWorld = true;
		}
		s_missionModePrev = s_missionMode;

		IOSTouchWeapons weapons;
		getWeapons(&weapons);
		IOSTouch_SetWeapons(&weapons);
		// the wheel's pictures (where there's none, it shows the name)
		const unsigned int wanted = playerThere() ? IOSTouch_WantedIcons(SDL_GetTicks() / 1000.0) : 0;
		for (s32 i = 0; i < IOSTOUCH_WPN_MAX; i++)
		{
			if (!(wanted & (1u << i))) { continue; }
			static u8 icon[64 * 64 * 4];
			s32 w, h;
			makeWeaponIcon(i, icon, &w, &h);
			IOSTouch_SetWeaponIcon(i, (w && h) ? icon : nullptr, w, h);
		}

		IOSTouchHud hud;
		memset(&hud, 0, sizeof(hud));
		hud.bValid = playerThere() ? 1 : 0;
		hud.bCheatAllowed = 1;
		hud.ctLevels = (s32)s_maxLevelIndex;
		hud.uWorld = s_world;

		SDL_Window* window = SDL_GetKeyboardFocus();
		if (!window) { window = SDL_GetMouseFocus(); }
		const int requests = IOSTouch_Update(window, mode, &hud, framesDrawn);
		s_mode = mode;

		// The HUD's corners inside the HUD frame the overlay gives (clear of
		// the screen's rounded corners, the notch or Dynamic Island and the
		// home bar), in the game's pixels
		{
			float frame[4];
			IOSTouch_GetHudFrame(frame);
			u32 w = 0, h = 0;
			vfb_getResolution(&w, &h);
			TFE_Settings_Hud* hudSettings = TFE_Settings::getHudSettings();
			hudSettings->pixelOffset[1] = (s32)lroundf(frame[0] * (f32)w);
			hudSettings->pixelOffset[0] = (s32)lroundf((1.0f - frame[2]) * (f32)w);
			hudSettings->pixelOffset[2] = -(s32)lroundf((1.0f - frame[3]) * (f32)h);
		}

		setHold(mode == IOSTOUCH_GAMEPLAY && IOSTouch_HoldsGame());

		if (mode == IOSTOUCH_HIDDEN)
		{
			s_heldPrev = 0;
			setAxis(AXIS_LEFT_X, 0.0f);
			setAxis(AXIS_LEFT_Y, 0.0f);
			return;
		}

		// what was asked for
		if (requests & IOSTOUCH_REQ_MENU)
		{
			if (s_typing) { setTyping(false); }	// (with the keyboard up, MENU only puts it away)
			else { inputMapping_setStatePress(IADF_MENU_TOGGLE); }
		}
		if (requests & IOSTOUCH_REQ_CONSOLE) { setTyping(!s_typing); }
		if (requests & IOSTOUCH_REQ_QUICKSAVE) { inputMapping_setStatePress(IAS_QUICK_SAVE); }
		else if (requests & IOSTOUCH_REQ_QUICKLOAD) { inputMapping_setStatePress(IAS_QUICK_LOAD); }
		if (requests & IOSTOUCH_REQ_RESUME) { inputMapping_setStatePress(IADF_PAUSE); }
		if (requests & IOSTOUCH_REQ_MAP) { inputMapping_setStatePress(IADF_AUTOMAP); }
		if (requests & IOSTOUCH_REQ_CHEATSOUND) { IOSCheatGame_PlaySound(); }
		if (requests & IOSTOUCH_REQ_LEVEL)
		{
			const s32 level = IOSTouch_TakeLevelPick();
			if (level >= 1 && level <= s_maxLevelIndex) { mission_iosQueueCheat(CHEAT_LASECBASE + level - 1); }
			else { IOSTouch_CheatLevelFailed(); }
		}
		if (s_typing) { watchTyping(); }

		// the stick, the buttons, the wheel's pick (the game's input mapping
		// has already run this frame: these go on top of it)
		IOSTouchInput in;
		IOSTouch_ReadInput(&in);
		setAxis(AXIS_LEFT_X, in.fMoveX);
		setAxis(AXIS_LEFT_Y, in.fMoveY);
		if (sqrtf(in.fMoveX * in.fMoveX + in.fMoveY * in.fMoveY) >= 0.99f)
		{
			inputMapping_setStateDown(IADF_RUN);	// (pushed all the way: running, as Dark Forces is played)
		}
		for (const ButtonAction& b : c_buttonActions)
		{
			if (!(in.ulButtons & b.bit)) { continue; }
			if (s_heldPrev & b.bit) { inputMapping_setStateDown(b.action); }
			else { inputMapping_setStatePress(b.action); }
		}
		s_heldPrev = in.ulButtons;
		if (in.iSelectWeapon >= 1 && in.iSelectWeapon <= WPN_COUNT)
		{
			inputMapping_setStatePress(InputAction(IADF_WEAPON_1 + in.iSelectWeapon - 1));
		}

		if (playerThere()) { applyCheat(IOSTouch_CheatBits(), newWorld); }
	}

	void iosGame_takeLook(s32* dx, s32* dy)
	{
		f32 lx = 0.0f, ly = 0.0f, yaw = 0.0f, pitch = 0.0f;
		IOSTouch_TakeLook(&lx, &ly);
		IOSTouch_TakeGyro(&yaw, &pitch);
		// tilt aiming turns the view as far as the phone turns (degrees, + left
		// and + up), whatever the mouse sensitivity
		const f32 sensH = fabsf(inputMapping_getHorzMouseSensitivity());
		const f32 sensV = fabsf(inputMapping_getVertMouseSensitivity());
		if (sensH > 0.0f) { lx -= yaw / (c_degPerCount * sensH); }
		if (sensV > 0.0f) { ly -= pitch / (c_degPerCount * sensV); }
		s_lookX += lx;
		s_lookY += ly;
		const s32 ix = (s32)s_lookX, iy = (s32)s_lookY;
		s_lookX -= (f32)ix;
		s_lookY -= (f32)iy;
		*dx = ix;
		*dy = iy;
	}

	void iosGame_hide()
	{
		setHold(false);
		setTyping(false);
		IOSTouch_Hide();
	}

	////////////////////////////////////////////////////
	// CAMERA's frame grabs: the game's 8-bit frame with its palette, scaled
	// up whole pixels to about the screen's size
	////////////////////////////////////////////////////
	static s32 s_grabScale = 1;

	void iosGame_frameBegin()
	{
		u32 w = 0, h = 0;
		vfb_getResolution(&w, &h);
		if (!w || !h) { return; }
		s_grabScale = (s32)ceilf(1600.0f / (f32)w);
		if (s_grabScale < 1) { s_grabScale = 1; }
		if (s_grabScale > 6) { s_grabScale = 6; }
		IOSCapture_FrameBegin((int)w * s_grabScale, (int)h * s_grabScale);
	}

	void iosGame_afterScene() { IOSCapture_AfterScene(); }
	void iosGame_afterHud() { IOSCapture_AfterHud(); }
}

using namespace TFE_DarkForces;

extern "C"
{
	int IOS_ReadFrameRGBA(int w, int h, unsigned char* p)
	{
		u32 fw = 0, fh = 0;
		vfb_getResolution(&fw, &fh);
		const u8* src = vfb_getCpuBuffer();
		const u32* pal = vfb_getPalette();
		const s32 k = s_grabScale;
		if (!src || !pal || !p || w != (int)fw * k || h != (int)fh * k) { return 0; }
		const u32 stride = vfb_getStride();
		// (bottom row first, as GL reads back)
		for (s32 y = 0; y < h; y++)
		{
			const u8* row = src + (size_t)((fh - 1) - y / k) * stride;
			u32* dst = (u32*)(p + (size_t)y * w * 4);
			for (s32 x = 0; x < w; x++)
			{
				dst[x] = pal[row[x / k]] | 0xff000000u;
			}
		}
		return 1;
	}

	// The cheat's game side (IOSCheatGame.h)
	int IOSCheatGame_Allowed(void) { return 1; }
	int IOSCheatGame_NumLevels(void) { return s_maxLevelIndex; }

	int IOSCheatGame_LevelInfo(int iLevel, char* strName, int ctName, char* strFile, int ctFile)
	{
		if (iLevel < 1 || iLevel > s_maxLevelIndex || !s_levelDisplayNames || !s_levelGamePaths) { return 0; }
		const char* name = s_levelDisplayNames[iLevel - 1];
		const char* file = s_levelGamePaths[iLevel - 1];
		if (strName && ctName > 0) { snprintf(strName, ctName, "%s", name ? name : ""); }
		if (strFile && ctFile > 0) { snprintf(strFile, ctFile, "%s.LEV", file ? file : ""); }
		return 1;
	}

	int IOSCheatGame_CurrentLevel(void)
	{
		const char* cur = agent_getLevelName();
		if (!cur || !s_levelGamePaths) { return 0; }
		for (s32 i = 0; i < s_maxLevelIndex; i++)
		{
			if (s_levelGamePaths[i] && strcasecmp(s_levelGamePaths[i], cur) == 0) { return i + 1; }
		}
		return 0;
	}

	int IOSCheatGame_GoToLevel(int iLevel)
	{
		if (iLevel < 1 || iLevel > s_maxLevelIndex) { return 0; }
		mission_iosQueueCheat(CHEAT_LASECBASE + iLevel - 1);
		return 1;
	}

	// One of Kyle's lines from the game's own sounds (a different one from
	// last time)
	void IOSCheatGame_PlaySound(void)
	{
		static const char* c_lines[] =
		{
			"M01KYL01.VOC",	// "This is too easy. Now to get to my ship."
			"M13KYL01.VOC",	// "Here we go..."
			"M16KYL04.VOC",	// "Jan would be proud..."
			"M16KYL06.VOC",	// "For freedom."
		};
		static s32 s_last = -1;
		const s32 count = s32(sizeof(c_lines) / sizeof(c_lines[0]));
		s32 i = s32(SDL_GetTicks() % u32(count));
		if (i == s_last) { i = (i + 1) % count; }
		s_last = i;
		SoundSourceId src = sound_load(c_lines[i], SOUND_PRIORITY_HIGH5);
		if (src) { sound_play(src); }
	}
	unsigned int IOSCheatGame_World(void) { return s_world; }
	unsigned int IOSCheatGame_Catches(void) { return 0; }

	// CAMERA's selfies, Action Freeze and the cinematic burst: not in this
	// build yet (the overlay says it can't take one here)
	int IOSSelfie_Begin(void) { return 0; }
	int IOSSelfie_IsActive(void) { return 0; }
	int IOSSelfie_IsPosed(void) { return 0; }
	int IOSSelfie_Failed(void) { return 1; }
	void IOSSelfie_Repose(void) {}
	void IOSSelfie_End(void) {}
	void IOSSelfie_SetMaxPeople(int) {}
	int IOSSelfie_NumPosed(void) { return 0; }

	int IOSFreeze_Begin(void) { return 0; }
	int IOSFreeze_Orbit(float, float) { return 0; }
	void IOSFreeze_Pan(float, float) {}
	void IOSFreeze_Zoom(float) {}
	void IOSFreeze_SetSmooth(int) {}
	void IOSFreeze_Frame(void) {}
	void IOSFreeze_End(void) {}
	int IOSFreeze_IsActive(void) { return 0; }

	int IOSCinema_Start(double) { return IOSCINEMA_CANT; }
	int IOSCinema_Tick(double) { return IOSCINEMA_IDLE; }
	void IOSCinema_Stop(void) {}
	int IOSCinema_IsActive(void) { return 0; }
	int IOSCinema_NumTaken(void) { return 0; }
}
#endif
