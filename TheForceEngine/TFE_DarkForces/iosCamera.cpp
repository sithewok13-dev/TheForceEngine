#ifdef TFE_IOS
//////////////////////////////////////////////////////////////////////
// iOS: CAMERA's selfies, Action Freeze and the cinematic burst (as the
// Serious Sam and Jedi Knight ports' IOSSelfie, IOSFreeze, IOSCinema and
// IOSPhotoCam), for Dark Forces. The C API is ios/IOSSelfie.h,
// ios/IOSFreeze.h and ios/IOSCinema.h; mission.cpp draws the 3D view
// through iosCamera_drawWorld (iosGame.h).
//
// The photo view. While one of them is on, the overlay holds the game
// still (mission_pause), and the 3D view is drawn from a camera of its own
// instead of the player's eyes: no first-person weapon, no HUD. The player
// is drawn too (Dark Forces has no player sprite: a human figure from the
// game's own files, HUMAN3.3DO, coloured as Kyle). Everything the photo
// view changes (enemies turned to the camera and posed on a frame of their
// own animations, shots in front of faces left out, the figure added to
// the player's sector, what the renderer marks as seen for the automap) is
// changed only while the view is being drawn, and put back exactly right
// after it, every frame: nothing else in the game ever sees it.
//
// Lengths are Dark Forces units (the player's eyes are 5.8 above their
// feet; about 3.2 units to a metre), angles radians unless named in
// degrees. World Y points down, as the game's: "up" is -Y.
//////////////////////////////////////////////////////////////////////
#include "iosGame.h"
#include "mission.h"
#include "player.h"
#include "logic.h"
#include "Actor/actor.h"
#include "Actor/actorModule.h"
#include "Actor/animTables.h"
#include "GameUI/escapeMenu.h"
#include "GameUI/pda.h"
#include "Landru/cutscene.h"
#include <TFE_Jedi/Level/rsector.h>
#include <TFE_Jedi/Level/rwall.h>
#include <TFE_Jedi/Level/robject.h>
#include <TFE_Jedi/Level/levelData.h>
#include <TFE_Jedi/Memory/allocator.h>
#include <TFE_Jedi/Renderer/jediRenderer.h>
#include <TFE_Jedi/Renderer/RClassic_Float/rclassicFloatSharedState.h>
#include <TFE_Jedi/Renderer/virtualFramebuffer.h>
#include <TFE_Asset/modelAsset_jedi.h>
#include <SDL.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include <initializer_list>

#include <IOSCapture.h>
#include <IOSPhotos.h>
#include <IOSSelfie.h>
#include <IOSFreeze.h>
#include <IOSCinema.h>

// (the Linux test rig's log)
#ifdef IOS_CAMERA_RIG_DEBUG
#define RIGDBG(...) printf("RIGDBG " __VA_ARGS__)
#else
#define RIGDBG(...)
#endif

using namespace TFE_Jedi;

namespace TFE_DarkForces
{
	static const f32 c_pi = 3.14159265f;
	static const f32 c_toAngle14 = 16384.0f / (2.0f * 3.14159265f);
	static inline f32 degToRad(f32 d) { return d * c_pi / 180.0f; }
	static inline f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
	static inline f32 minf(f32 a, f32 b) { return a < b ? a : b; }
	static inline f32 maxf(f32 a, f32 b) { return a > b ? a : b; }

	struct V3 { f32 x, y, z; };
	static inline V3 v3(f32 x, f32 y, f32 z) { V3 v = { x, y, z }; return v; }
	static inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
	static inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
	static inline V3 operator-(V3 a) { return v3(-a.x, -a.y, -a.z); }
	static inline V3 operator*(V3 a, f32 k) { return v3(a.x * k, a.y * k, a.z * k); }
	static inline f32 dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	static inline f32 len(V3 a) { return sqrtf(dot(a, a)); }
	static inline f32 lenXZ(V3 a) { return sqrtf(a.x * a.x + a.z * a.z); }
	static inline V3 up(f32 h) { return v3(0.0f, -h, 0.0f); }	// (h above)
	static inline V3 fromFixed(const vec3_fixed& p) { return v3(fixed16ToFloat(p.x), fixed16ToFloat(p.y), fixed16ToFloat(p.z)); }
	// Angle between two directions
	static inline f32 angleBetween(V3 a, V3 b)
	{
		const f32 la = len(a), lb = len(b);
		if (la < 1e-4f || lb < 1e-4f) { return 0.0f; }
		return acosf(clampf(dot(a, b) / (la * lb), -1.0f, 1.0f));
	}

	// Headings: the game's yaw, as radians; 0 looks along +Z, and it turns
	// right as it grows (towards +X)
	static inline f32 headingTo(V3 from, V3 to) { return atan2f(to.x - from.x, to.z - from.z); }
	static inline V3 flatDir(f32 yaw) { return v3(sinf(yaw), 0.0f, cosf(yaw)); }
	// The way a camera at this heading and pitch (up: > 0) looks
	static inline V3 lookDir(f32 yaw, f32 pitch) { return v3(sinf(yaw) * cosf(pitch), -sinf(pitch), cosf(yaw) * cosf(pitch)); }
	static inline angle14_32 toAngle14(f32 a) { return angle14_32(lroundf(a * c_toAngle14)) & 0x3fff; }
	static inline f32 fromAngle14(angle14_32 a)
	{
		a &= 0x3fff;
		if (a >= 8192) { a -= 16384; }
		return f32(a) / c_toAngle14;
	}
	static inline f32 wrapAngle(f32 a)
	{
		while (a > c_pi) { a -= 2.0f * c_pi; }
		while (a < -c_pi) { a += 2.0f * c_pi; }
		return a;
	}

	// The selfie's own random numbers (the game's stay as they were)
	static u32 s_rand = 1;
	static void randSeed(u32 seed) { s_rand = seed ? seed : 1; }
	static u32 randNext() { s_rand ^= s_rand << 13; s_rand ^= s_rand >> 17; s_rand ^= s_rand << 5; return s_rand; }
	static s32 randInt(s32 n) { return n > 0 ? s32(randNext() % u32(n)) : 0; }
	static f32 rand01() { return f32(randNext() & 0xffffff) / f32(0xffffff); }
	static bool chance(s32 n) { return randInt(n) == 0; }

	//////////////////////////////////////////////////////////////////
	// The level: sectors and lines through them
	//////////////////////////////////////////////////////////////////
	// Lines are walked in steps of c_lineStep, from sector to sector through
	// the openings of adjoining walls (not through mid-textured ones: bars,
	// fences, force fields, which a camera mustn't be behind), each point
	// c_lineMargin clear of the floor and ceiling at least
	static const f32 c_lineStep = 0.25f;

	static bool inSectorVol(RSector* s, V3 p, f32 margin)
	{
		if (!s) { return false; }
		const f32 floor = fixed16ToFloat(s->floorHeight), ceil = fixed16ToFloat(s->ceilingHeight);
		if (p.y > floor - margin || p.y < ceil + margin) { return false; }
		if (p.x < fixed16ToFloat(s->boundsMin.x) || p.x > fixed16ToFloat(s->boundsMax.x) ||
			p.z < fixed16ToFloat(s->boundsMin.z) || p.z > fixed16ToFloat(s->boundsMax.z)) { return false; }
		return sector_pointInsideDF(s, floatToFixed16(p.x), floatToFixed16(p.z)) != JFALSE;
	}

	static inline bool passable(const RWall* w)
	{
		return w->nextSector && !(w->flags1 & WF1_ADJ_MID_TEX) && w->nextSector->floorHeight > w->nextSector->ceilingHeight;
	}

	// The sector p is in, one or two openings from cur (null: a wall, a
	// closed door, the floor or ceiling between)
	static RSector* stepSector(RSector* cur, V3 p, f32 margin)
	{
		if (inSectorVol(cur, p, margin)) { return cur; }
		for (s32 i = 0; i < cur->wallCount; i++)
		{
			const RWall* w = &cur->walls[i];
			if (passable(w) && inSectorVol(w->nextSector, p, margin)) { return w->nextSector; }
		}
		// (a corner of a small sector cut across)
		for (s32 i = 0; i < cur->wallCount; i++)
		{
			const RWall* w = &cur->walls[i];
			if (!passable(w)) { continue; }
			RSector* n = w->nextSector;
			for (s32 k = 0; k < n->wallCount; k++)
			{
				const RWall* w2 = &n->walls[k];
				if (passable(w2) && w2->nextSector != cur && inSectorVol(w2->nextSector, p, margin)) { return w2->nextSector; }
			}
		}
		return nullptr;
	}

	// The sector a point is in (hint: the one it is most likely in)
	static RSector* sectorAt(V3 p, RSector* hint)
	{
		if (hint && inSectorVol(hint, p, 0.0f)) { return hint; }
		return sector_which3D(floatToFixed16(p.x), floatToFixed16(p.y), floatToFixed16(p.z));
	}

	// How far from `from` (in sector fromSec) towards `to` before the level
	// is in the way; the sector where it stops
	static f32 walkLine(V3 from, RSector* fromSec, V3 to, f32 margin, RSector** endSec)
	{
		const V3 d = to - from;
		const f32 l = len(d);
		RSector* s = fromSec;
		if (endSec) { *endSec = s; }
		if (!s) { return 0.0f; }
		if (l < 1e-4f) { return l; }
		const s32 n = s32(ceilf(l / c_lineStep));
		for (s32 i = 1; i <= n; i++)
		{
			const V3 p = from + d * (f32(i) / f32(n));
			RSector* ns = stepSector(s, p, margin);
			if (!ns) { return l * f32(i - 1) / f32(n); }
			s = ns;
			if (endSec) { *endSec = s; }
		}
		return l;
	}

	//////////////////////////////////////////////////////////////////
	// The game's objects
	//////////////////////////////////////////////////////////////////
	enum ObjKind { KIND_NONE = 0, KIND_ENEMY, KIND_SHOT, KIND_EFFECT, KIND_ITEM, KIND_SOLID };

	static ActorDispatch* dispatchOf(SecObject* obj)
	{
		if (!obj->logic) { return nullptr; }
		Logic** logicPtr = (Logic**)allocator_getHead((Allocator*)obj->logic);
		while (logicPtr)
		{
			Logic* logic = *logicPtr;
			if (logic && logic->type == LOGIC_DISPATCH) { return (ActorDispatch*)logic; }
			logicPtr = (Logic**)allocator_getNext((Allocator*)obj->logic);
		}
		return nullptr;
	}

	static DamageModule* damageOf(ActorDispatch* dispatch)
	{
		for (s32 i = 0; i < ACTOR_MAX_MODULES; i++)
		{
			ActorModule* module = dispatch->modules[i];
			if (module && module->type == ACTMOD_DAMAGE) { return (DamageModule*)module; }
		}
		return nullptr;
	}

	// The WAX animation an actor uses for one of its actions (-1: none)
	static s32 waxAnimFor(SecObject* obj, s32 action)
	{
		if (obj->type != OBJ_TYPE_SPRITE || !obj->wax) { return -1; }
		ActorDispatch* dispatch = dispatchOf(obj);
		s32 anim = action;
		if (dispatch && dispatch->animTable) { anim = dispatch->animTable[action]; }
		if (anim < 0 || anim >= WAX_MAX_ANIM || anim >= obj->wax->animCount) { return -1; }
		WaxAnim* a = WAX_AnimPtr(obj->wax, anim);
		if (!a || a->frameCount <= 0) { return -1; }
		return anim;
	}

	static bool isDying(SecObject* obj)
	{
		const s32 a = obj->anim;
		for (s32 action : { s32(ANIM_DIE1), s32(ANIM_DIE2), s32(ANIM_DEAD) })
		{
			if (waxAnimFor(obj, action) == a && a >= 0) { return true; }
		}
		return false;
	}

	static SecObject s_figure;	// the player's figure in the photo view (in a sector only while it is drawn)

	static s32 objKind(SecObject* obj)
	{
		if (!obj || obj == s_playerObject || obj == &s_figure || !obj->ptr) { return KIND_NONE; }
		if (!(obj->flags & OBJ_FLAG_NEEDS_TRANSFORM)) { return KIND_NONE; }	// (not drawn)
		const u32 ef = obj->entityFlags;
		if (ef & ETFLAG_PLAYER) { return KIND_NONE; }
		if (ef & ETFLAG_PROJECTILE) { return KIND_SHOT; }
		if (ef & ETFLAG_AI_ACTOR)
		{
			ActorDispatch* dispatch = dispatchOf(obj);
			if (dispatch)
			{
				DamageModule* damage = damageOf(dispatch);
				if (damage && damage->hp <= 0) { return KIND_NONE; }
			}
			if (obj->type == OBJ_TYPE_SPRITE && isDying(obj)) { return KIND_NONE; }
			return obj->type == OBJ_TYPE_SPRITE ? KIND_ENEMY : KIND_SOLID;
		}
		if (ef & ETFLAG_PICKUP) { return KIND_ITEM; }
		if (ef & ETFLAG_CORPSE) { return KIND_NONE; }
		if ((obj->type == OBJ_TYPE_SPRITE || obj->type == OBJ_TYPE_FRAME) && (obj->flags & OBJ_FLAG_FULLBRIGHT) && ef == 0)
		{
			return KIND_EFFECT;	// (a blast, a puff, sparks)
		}
		if (obj->type == OBJ_TYPE_3D)
		{
			const f32 r = obj->model ? fixed16ToFloat(obj->model->radius) : 0.0f;
			return (r > 0.5f && r < 20.0f) ? KIND_SOLID : KIND_NONE;
		}
		return obj->worldWidth > 0 ? KIND_SOLID : KIND_NONE;
	}

	template <class F> static void forEachObject(F f)
	{
		RSector* s = s_levelState.sectors;
		for (u32 i = 0; i < s_levelState.sectorCount; i++, s++)
		{
			if (!s->objectList) { continue; }
			for (s32 k = 0; k < s->objectCapacity; k++)
			{
				SecObject* obj = s->objectList[k];
				if (obj) { f(obj); }
			}
		}
	}

	// Where an object's head and middle are, how tall it is, how big its
	// head and body are (seen from anywhere, generous)
	struct Measure { V3 head, mid; f32 height, headR, bodyR, width; };
	static Measure measure(SecObject* obj)
	{
		Measure m;
		const V3 pos = fromFixed(obj->posWS);
		f32 h = obj->worldHeight > 0 ? fixed16ToFloat(obj->worldHeight) : 0.0f;
		f32 w = obj->worldWidth > 0 ? fixed16ToFloat(obj->worldWidth) : 0.0f;
		if (obj->type == OBJ_TYPE_3D && obj->model)
		{
			const f32 r = fixed16ToFloat(obj->model->radius);
			h = maxf(h, r);
			w = maxf(w, r * 0.7f);
		}
		if (h < 1.0f) { h = 1.0f; }
		m.height = h;
		m.width = w;
		m.mid = pos + up(h * 0.5f);
		m.head = pos + up(h * 0.85f);
		m.bodyR = maxf(0.5f * maxf(2.0f * w, 0.6f * h), 0.7f);
		m.headR = maxf(minf(0.7f * w, 0.2f * h), 0.4f);
		return m;
	}

	//////////////////////////////////////////////////////////////////
	// The player's figure: HUMAN3.3DO (a human of the game's own files),
	// made the player's size and coloured as Kyle, in the game pool (the
	// levels' copies of it are their own)
	//////////////////////////////////////////////////////////////////
	static const f32 c_figureHeight = 8.0f;		// (as tall as the game's people; the eyes are at 5.8)
	static JediModel* s_figureModel = nullptr;

	static s32 nearestColor(s32 r, s32 g, s32 b)
	{
		s32 best = 1, bestD = 0x7fffffff;
		for (s32 i = 1; i < 255; i++)
		{
			const s32 pr = s_levelPalette[i * 3] * 4, pg = s_levelPalette[i * 3 + 1] * 4, pb = s_levelPalette[i * 3 + 2] * 4;
			const s32 d = (pr - r) * (pr - r) * 3 + (pg - g) * (pg - g) * 4 + (pb - b) * (pb - b) * 2;
			if (d < bestD) { bestD = d; best = i; }
		}
		return best;
	}

	static JediModel* figureModel()
	{
		JediModel* model = TFE_Model_Jedi::get("HUMAN3.3DO", POOL_GAME);
		if (!model || model->vertexCount <= 0 || !model->vertices) { return nullptr; }
		s32 minY = 0;
		for (s32 i = 0; i < model->vertexCount; i++) { minY = min(minY, model->vertices[i].y); }
		const f32 height = -fixed16ToFloat(minY);
		if (height < 0.5f) { return nullptr; }
		if (fabsf(height - c_figureHeight) > 0.05f)
		{
			// (once: its own copy, made the player's size)
			const fixed16_16 k = floatToFixed16(c_figureHeight / height);
			for (s32 i = 0; i < model->vertexCount; i++)
			{
				model->vertices[i].x = mul16(model->vertices[i].x, k);
				model->vertices[i].y = mul16(model->vertices[i].y, k);
				model->vertices[i].z = mul16(model->vertices[i].z, k);
			}
			model->radius = mul16(model->radius, k);
		}
		// Kyle's colours, in this level's palette: boots, trousers, a tan
		// shirt, skin, dark hair (by where each triangle is on the figure;
		// it faces -Z, its arms out in front at the waist)
		const s32 cBoots = nearestColor(52, 36, 24), cTrousers = nearestColor(64, 60, 58), cShirt = nearestColor(150, 116, 76);
		const s32 cVest = nearestColor(92, 64, 40), cSkin = nearestColor(196, 146, 110), cHair = nearestColor(54, 38, 26);
		for (s32 p = 0; p < model->polygonCount; p++)
		{
			JmPolygon* poly = &model->polygons[p];
			if (poly->vertexCount <= 0 || !poly->indices) { continue; }
			V3 c = v3(0, 0, 0);
			for (s32 k = 0; k < poly->vertexCount; k++)
			{
				const vec3& v = model->vertices[poly->indices[k]];
				c = c + v3(fixed16ToFloat(v.x), fixed16ToFloat(v.y), fixed16ToFloat(v.z));
			}
			c = c * (1.0f / f32(poly->vertexCount));
			const f32 hgt = -c.y / c_figureHeight;	// (0: the feet, 1: the top of the head)
			s32 color;
			if (hgt < 0.07f) { color = cBoots; }
			else if (hgt < 0.52f) { color = cTrousers; }
			else if (hgt < 0.88f)
			{
				const bool arm = fabsf(c.x) > 1.0f * c_figureHeight / 8.43f || c.z < -0.9f * c_figureHeight / 8.43f;
				const bool hand = c.z < -1.5f * c_figureHeight / 8.43f;
				if (hand) { color = cSkin; }
				else if (arm) { color = cShirt; }
				else { color = (fabsf(c.x) > 0.45f * c_figureHeight / 8.43f) ? cVest : cShirt; }
			}
			else if (hgt < 0.95f && c.z < 0.15f) { color = cSkin; }
			else { color = cHair; }
			poly->color = color;
			// (it comes drawn as points only: solid faces, lit flat)
			poly->shading = PSHADE_FLAT;
		}
		model->flags &= ~(MFLAG_DRAW_VERTICES | MFLAG_VERTEX_LIT);
		s_figureModel = model;
		return model;
	}

	//////////////////////////////////////////////////////////////////
	// The photo view
	//////////////////////////////////////////////////////////////////
	static const s32 c_maxPoses = IOSSELFIE_MAX_PEOPLE_LIMIT + 8;
	static const s32 c_maxHidden = 256;
	// A selfie's flash: the faces near the camera lit, however the level is
	// lit behind them (the game's own light at the eye, this strong)
	static const s32 c_flashLight = 26;
	// Sprites nearer the lens than this are left out of a photo view
	static const f32 c_lensClear = 3.0f;

	struct Pose { SecObject* obj; angle14_32 yaw; s32 anim, frame; };
	struct Saved { SecObject* obj; angle14_32 yaw; s32 anim, frame; u32 flags; };

	static bool s_pcActive = false, s_pcPrepared = false, s_pcFailed = false;
	static bool (*s_pcPrepare)() = nullptr;
	static V3 s_camPos;
	static f32 s_camYaw = 0.0f, s_camPitch = 0.0f;
	static RSector* s_camSector = nullptr;
	static V3 s_eye;					// the player's eyes, and where they look
	static f32 s_lookYaw = 0.0f, s_lookPitch = 0.0f;	// (radians)
	static RSector* s_eyeSector = nullptr;
	static bool s_hidePlayer = true, s_flash = false;
	static f32 s_figureYaw = 0.0f;
	static Pose s_poses[c_maxPoses];
	static s32 s_poseCount = 0;
	static SecObject* s_hidden[c_maxHidden];
	static s32 s_hiddenCount = 0;
	// (what the renderer marks as seen, put back after each photo frame)
	static std::vector<u32> s_savedSectorState;
	static std::vector<s32> s_savedWallState;

	static bool playerCanPose()
	{
		return s_playerObject && s_playerEye == s_playerObject && s_playerObject->sector && s_missionMode == MISSION_MODE_MAIN &&
			!s_playerDying && !s_externalCameraMode && !escapeMenu_isOpen() && !pda_isOpen() && !cutscene_isPlaying() &&
			s_levelState.sectors && s_levelState.sectorCount > 0;
	}

	static void pc_readEye()
	{
		s_eye = fromFixed(s_eyePos);
		s_lookYaw = fromAngle14(TFE_DarkForces::s_eyeYaw);
		s_lookPitch = fromAngle14(s_eyePitch);
	}

	static bool pc_begin(bool (*prepare)())
	{
		if (s_pcActive || !playerCanPose()) { return false; }
		s_pcPrepare = prepare;
		s_pcPrepared = s_pcFailed = false;
		s_poseCount = 0;
		s_hiddenCount = 0;
		s_hidePlayer = true;
		s_flash = false;
		pc_readEye();
		s_figureYaw = fromAngle14(s_playerObject->yaw) + c_pi;	// (the figure faces -Z: the way the player faces)
		s_camPos = s_eye;
		s_camYaw = s_lookYaw;
		s_camPitch = s_lookPitch;
		s_eyeSector = s_camSector = s_playerObject->sector;
		s_pcActive = true;
		return true;
	}

	static void pc_end()
	{
		s_pcActive = s_pcPrepared = s_pcFailed = false;
		s_pcPrepare = nullptr;
		s_poseCount = 0;
		s_hiddenCount = 0;
		s_camSector = s_eyeSector = nullptr;
	}

	static void pc_place(V3 pos, f32 yaw, f32 pitch, RSector* sector)
	{
		s_camPos = pos;
		s_camYaw = yaw;
		s_camPitch = pitch;
		if (sector) { s_camSector = sector; }
	}

	static void pc_setPose(SecObject* obj, f32 yaw, s32 anim, s32 frame)
	{
		for (s32 i = 0; i < s_poseCount; i++)
		{
			if (s_poses[i].obj == obj) { s_poses[i].yaw = toAngle14(yaw); s_poses[i].anim = anim; s_poses[i].frame = frame; return; }
		}
		if (s_poseCount >= c_maxPoses) { return; }
		Pose& p = s_poses[s_poseCount++];
		p.obj = obj;
		p.yaw = toAngle14(yaw);
		p.anim = anim;
		p.frame = frame;
	}

	static bool pc_isHidden(SecObject* obj)
	{
		for (s32 i = 0; i < s_hiddenCount; i++) { if (s_hidden[i] == obj) { return true; } }
		return false;
	}

	static void pc_hide(SecObject* obj)
	{
		if (!obj || pc_isHidden(obj) || s_hiddenCount >= c_maxHidden) { return; }
		s_hidden[s_hiddenCount++] = obj;
	}

	// How far across and up the view reaches (tangents of half the view,
	// as the renderer draws it)
	static void viewTans(f32* tanX, f32* tanY)
	{
		u32 w = 0, h = 0;
		vfb_getResolution(&w, &h);
		if (w == 320 && h == 200)
		{
			*tanX = 1.0f;
			*tanY = 100.0f / 160.0f;
			return;
		}
		const RClassicFloatState& st = s_rcfltState;
		*tanX = (st.focalLength > 1.0f) ? st.projOffsetX / st.focalLength : 1.0f;
		*tanY = (st.focalLenAspect > 1.0f) ? st.projOffsetYBase / st.focalLenAspect : 0.6f;
		if (*tanX < 0.2f || *tanX > 5.0f) { *tanX = 1.0f; }
		if (*tanY < 0.2f || *tanY > 5.0f) { *tanY = 0.6f; }
	}

	// The renderer tips the view up and down by shearing it, at most 60
	// degrees; a photo view keeps within c_pitchMax
	static const f32 c_pitchMax = 0.96f;	// (55 degrees)

	static bool objValid(SecObject* obj)
	{
		return obj && obj->self == obj && obj->sector;
	}

	// Draws the photo view (everything posed, then put back)
	static void pc_draw(u8* display, const u8* colormap, const u8* lightSourceRamp)
	{
		static Saved saved[c_maxPoses + c_maxHidden];
		s32 savedCount = 0;
		for (s32 i = 0; i < s_poseCount; i++)
		{
			SecObject* obj = s_poses[i].obj;
			if (!objValid(obj)) { continue; }
			Saved& s = saved[savedCount++];
			s.obj = obj; s.yaw = obj->yaw; s.anim = obj->anim; s.frame = obj->frame; s.flags = obj->flags;
			obj->yaw = s_poses[i].yaw;
			if (s_poses[i].anim >= 0)
			{
				obj->anim = s_poses[i].anim;
				obj->frame = s_poses[i].frame;
			}
		}
		for (s32 i = 0; i < s_hiddenCount; i++)
		{
			SecObject* obj = s_hidden[i];
			if (!objValid(obj)) { continue; }
			Saved& s = saved[savedCount++];
			s.obj = obj; s.yaw = obj->yaw; s.anim = obj->anim; s.frame = obj->frame; s.flags = obj->flags;
			obj->flags &= ~OBJ_FLAG_NEEDS_TRANSFORM;
		}
		// Sprites right by the lens (a head filling half the photo) are left out
		forEachObject([&](SecObject* obj)
		{
			if (savedCount >= c_maxPoses + c_maxHidden || obj == s_playerObject || obj->type == OBJ_TYPE_3D) { return; }
			if (!(obj->flags & OBJ_FLAG_NEEDS_TRANSFORM) || !obj->ptr) { return; }
			const V3 pos = fromFixed(obj->posWS);
			const f32 h = obj->worldHeight > 0 ? fixed16ToFloat(obj->worldHeight) : 1.0f;
			if (lenXZ(pos - s_camPos) > c_lensClear || s_camPos.y > pos.y + 1.0f || s_camPos.y < pos.y - h - 1.0f) { return; }
			Saved& s = saved[savedCount++];
			s.obj = obj; s.yaw = obj->yaw; s.anim = obj->anim; s.frame = obj->frame; s.flags = obj->flags;
			obj->flags &= ~OBJ_FLAG_NEEDS_TRANSFORM;
		});
		// The player's figure, in their sector while it is drawn
		RSector* figureSector = nullptr;
		u32 figureDirty = 0;
		if (!s_hidePlayer && s_playerObject->sector && figureModel())
		{
			memset(&s_figure, 0, sizeof(s_figure));
			s_figure.self = &s_figure;
			s_figure.worldWidth = s_figure.worldHeight = -1;
			obj3d_setData(&s_figure, s_figureModel);
			s_figure.flags = OBJ_FLAG_NEEDS_TRANSFORM;
			s_figure.posWS = s_playerObject->posWS;
			s_figure.yaw = toAngle14(s_figureYaw);
			obj3d_computeTransform(&s_figure);
			figureSector = s_playerObject->sector;
			figureDirty = figureSector->dirtyFlags;
			sector_addObjectDirect(figureSector, &s_figure);
		}
		// What the renderer marks for the automap
		const u32 sectorCount = s_levelState.sectorCount;
		s_savedSectorState.resize(sectorCount * 5);
		s_savedWallState.clear();
		RSector* sec = s_levelState.sectors;
		for (u32 i = 0; i < sectorCount; i++, sec++)
		{
			u32* st = &s_savedSectorState[i * 5];
			st[0] = sec->flags1; st[1] = u32(sec->prevDrawFrame); st[2] = u32(sec->prevDrawFrame2);
			st[3] = u32(sec->startWall); st[4] = u32(sec->drawWallCnt);
			for (s32 w = 0; w < sec->wallCount; w++)
			{
				const RWall* wall = &sec->walls[w];
				s_savedWallState.push_back(s32(wall->seen));
				s_savedWallState.push_back(wall->visible);
				s_savedWallState.push_back(wall->drawFrame);
			}
		}

		RIGDBG("draw cam %.2f %.2f %.2f yaw %.1f pitch %.1f sector %d figure %d poses %d hidden %d\n", s_camPos.x, s_camPos.y, s_camPos.z,
			s_camYaw * 180.0f / c_pi, s_camPitch * 180.0f / c_pi, s_camSector ? s_camSector->id : -1, figureSector ? 1 : 0, s_poseCount, s_hiddenCount);
		renderer_computeCameraTransform(s_camSector, toAngle14(clampf(s_camPitch, -c_pitchMax, c_pitchMax)), toAngle14(s_camYaw),
			floatToFixed16(s_camPos.x), floatToFixed16(s_camPos.y), floatToFixed16(s_camPos.z));
		renderer_setWorldAmbient(s_flash ? max(s_playerLight, c_flashLight) : s_playerLight);
		drawWorld(display, s_camSector, colormap, lightSourceRamp);

		// Everything back as it was
		sec = s_levelState.sectors;
		size_t wi = 0;
		for (u32 i = 0; i < sectorCount; i++, sec++)
		{
			const u32* st = &s_savedSectorState[i * 5];
			sec->flags1 = (sec->flags1 & ~SEC_FLAGS1_RENDERED) | (st[0] & SEC_FLAGS1_RENDERED);
			sec->prevDrawFrame = s32(st[1]); sec->prevDrawFrame2 = s32(st[2]);
			sec->startWall = s32(st[3]); sec->drawWallCnt = s32(st[4]);
			for (s32 w = 0; w < sec->wallCount && wi + 2 < s_savedWallState.size(); w++)
			{
				RWall* wall = &sec->walls[w];
				wall->seen = JBool(s_savedWallState[wi++]);
				wall->visible = s_savedWallState[wi++];
				wall->drawFrame = s_savedWallState[wi++];
			}
		}
		if (figureSector)
		{
			// (out of the list as it went in: no messages, no other change)
			if (s_figure.sector == figureSector && s_figure.index >= 0 && s_figure.index < figureSector->objectCapacity &&
				figureSector->objectList[s_figure.index] == &s_figure)
			{
				figureSector->objectList[s_figure.index] = nullptr;
				figureSector->objectCount--;
			}
			s_figure.sector = nullptr;
			figureSector->dirtyFlags = figureDirty;
		}
		for (s32 i = savedCount - 1; i >= 0; i--)
		{
			const Saved& s = saved[i];
			s.obj->yaw = s.yaw; s.obj->anim = s.anim; s.obj->frame = s.frame; s.obj->flags = s.flags;
		}
		// (the camera the game's own drawing expects)
		player_setupCamera();
	}

	//////////////////////////////////////////////////////////////////
	// Lines and room, for the camera
	//////////////////////////////////////////////////////////////////
	// The models a camera mustn't be inside: enemies, scenery, small 3D
	// objects near the player, as boxes (shrunk, as Sam's: a sprite's box
	// is its widest)
	struct Solid { SecObject* obj; V3 lo, hi; };
	static const s32 c_maxSolids = 512;
	static Solid s_solids[c_maxSolids];
	static s32 s_solidCount = 0;
	static bool (*s_solidLeftOut)(SecObject* obj, V3 cam) = nullptr;

	static void findSolids(f32 range)
	{
		s_solidCount = 0;
		forEachObject([&](SecObject* obj)
		{
			if (s_solidCount >= c_maxSolids) { return; }
			const s32 kind = objKind(obj);
			if (kind != KIND_ENEMY && kind != KIND_SOLID) { return; }
			const Measure m = measure(obj);
			if (lenXZ(m.mid - s_eye) > range) { return; }
			// (people: a camera keeps out of arm's reach of them, too)
			const f32 r = maxf(m.width, 0.5f) * 0.8f + (kind == KIND_ENEMY ? 1.2f : 0.0f);
			const V3 pos = fromFixed(obj->posWS);
			Solid& s = s_solids[s_solidCount++];
			s.obj = obj;
			s.lo = v3(pos.x - r, pos.y - m.height * 0.9f, pos.z - r);
			s.hi = v3(pos.x + r, pos.y, pos.z + r);
		});
	}

	static bool inSolid(V3 v, f32 r)
	{
		for (s32 i = 0; i < s_solidCount; i++)
		{
			const Solid& s = s_solids[i];
			if (s_solidLeftOut && s_solidLeftOut(s.obj, v)) { continue; }
			if (v.x > s.lo.x - r && v.x < s.hi.x + r && v.y > s.lo.y - r && v.y < s.hi.y + r && v.z > s.lo.z - r && v.z < s.hi.z + r)
			{
				return true;
			}
		}
		// (nor in the player)
		const V3 feet = fromFixed(s_playerObject->posWS);
		const f32 pr = fixed16ToFloat(s_playerObject->worldWidth) * 0.6f + r;
		if (lenXZ(v - feet) < pr && v.y < feet.y + r && v.y > s_eye.y - 0.6f - r) { return true; }
		return false;
	}

	// How far from `from` towards `to` the level lets a line go
	static f32 clearDist(V3 from, RSector* fromSec, V3 to, RSector** endSec = nullptr)
	{
		return walkLine(from, fromSec, to, 0.05f, endSec);
	}

	static bool clearLine(V3 from, RSector* fromSec, V3 to, f32 slack)
	{
		return clearDist(from, fromSec, to) >= len(to - from) - slack;
	}

	// Whether a camera at v (r round it clear of the level and the models)
	// can be had, seen from `from`; its sector
	static bool roomAt(V3 from, RSector* fromSec, V3 v, f32 r, RSector** camSec)
	{
		if (inSolid(v, r)) { return false; }
		RSector* s = nullptr;
		if (walkLine(from, fromSec, v, r, &s) < len(v - from) - 0.001f || !s) { return false; }
		static const V3 dirs[4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		for (const V3& d : dirs)
		{
			if (walkLine(v, s, v + d * r, 0.0f, nullptr) < r - 0.001f) { return false; }
		}
		if (camSec) { *camSec = s; }
		return true;
	}

	//////////////////////////////////////////////////////////////////
	// Selfies
	//////////////////////////////////////////////////////////////////
	// Sam's selfie (IOSSelfie.cpp), in Dark Forces units (x3.2 the metres):
	// enemies within c_sfRange of the player in their sight, the nearest
	// IOSSELFIE_MAX_PEOPLE; one selfie in c_sfBombChance a photobomber from
	// further out
	static const f32 c_sfRange = 96.0f;
	static const f32 c_sfBombRange = 144.0f;
	static const s32 c_sfBombChance = 6;
	// The camera: at one of c_sfDirs headings round the player, from 5
	// units out (the figure has no face to see from nearer), as near as
	// c_sfCamNear (or c_sfCamMax where nothing near will do), a ball of
	// c_sfClearance round it clear; c_sfLift over the eyes (now and then up
	// or down a little, or held up high over a crowd)
	static const s32 c_sfDirs = 16;
	static const f32 c_sfCamNear = 12.0f;
	static const f32 c_sfCamMax = 38.0f;
	static const f32 c_sfClearance = 0.8f;
	static const f32 c_sfLift = 0.5f, c_sfLiftUp = 1.9f, c_sfLiftDown = -0.8f, c_sfLiftHigh = 3.8f, c_sfLiftHigher = 8.0f;
	static const f32 c_sfPitchDownMax = degToRad(35.0f);
	// Enemies nearer the camera than the player are left out of the photo
	static const f32 c_sfFrontMargin = 1.0f, c_sfBeside = 1.6f, c_sfLensClear = 4.8f;
	static const f32 c_sfCrowded = 0.8f;
	// What fits (fractions of the half frame), and how a photo is scored
	static const f32 c_sfFitX = 0.88f, c_sfFitUp = 0.80f, c_sfFitDown = 0.85f;
	static const f32 c_sfFitPlayerX = 0.60f, c_sfFitPlayerY = 0.55f;
	static const s32 c_sfFacesClear = 3;
	static const f32 c_sfDistCost = 0.078f, c_sfClutterCost = 12.0f;
	static const f32 c_sfWallCost = 8.0f;	// (a wall filling the frame by the camera)
	static const f32 c_sfCentreX = 0.35f, c_sfCentreY = 0.40f;
	static const f32 c_sfHeadMargin = 0.02f;
	// Poses
	static const f32 c_sfTurnSmall = degToRad(7.0f);
	static const s32 c_sfTurnChance = 45;
	static const f32 c_sfTurnMin = degToRad(20.0f), c_sfTurnMax = degToRad(35.0f);
	static const f32 c_sfPhoneTurn = degToRad(20.0f);
	static const s32 c_sfGroupChance = 5, c_sfRareChance = 20;
	static const s32 c_sfMaxPersons = IOSSELFIE_MAX_PEOPLE_LIMIT + 2;
	static const s32 c_sfMaxBlockers = 256;

	enum { POSE_ATTACK = 0, POSE_ANGER, POSE_IDLE, POSE_WOUND, POSE_MOVE, POSE_KINDS };
	enum { RARE_NONE = 0, RARE_STARE, RARE_OBLIVIOUS, RARE_ALLEYES };

	struct Person
	{
		SecObject* obj;		// (null: the player)
		bool player;
		f32 dist;
		V3 head, mid, pos;
		f32 height, headR, bodyR;
		bool inFrame, bomber;
	};
	struct Blocker { SecObject* obj; V3 head, mid; f32 headR, bodyR; };

	static bool s_sfActive = false, s_sfPosed = false, s_sfFailed = false;
	static Person s_people[c_sfMaxPersons];	// [0]: the player
	static s32 s_peopleCount = 0;
	static Blocker s_blockers[c_sfMaxBlockers];
	static s32 s_blockerCount = 0;
	static V3 s_sfCam;
	static RSector* s_sfCamSector = nullptr;
	static f32 s_sfLiftNow = c_sfLift;
	static bool s_phoneLeft = false;
	static s32 s_maxPeople = IOSSELFIE_MAX_PEOPLE;

	struct View { V3 pos; f32 yaw, pitch, tanX, tanY, tanP; V3 fwd, right; };

	static void makeView(View& v, V3 cam, V3 at, f32 tiltUp)
	{
		v.pos = cam;
		const V3 d = at - cam;
		v.yaw = (lenXZ(d) < 1e-3f) ? s_lookYaw : headingTo(cam, at);
		v.pitch = clampf(atan2f(-d.y, maxf(lenXZ(d), 1e-3f)) + tiltUp, -c_pitchMax, c_pitchMax);
		viewTans(&v.tanX, &v.tanY);
		v.tanP = tanf(v.pitch);
		v.fwd = flatDir(v.yaw);
		v.right = v3(cosf(v.yaw), 0.0f, -sinf(v.yaw));
	}

	// Where a point is in the view (across, up: fractions of the half frame;
	// how far in front, <= 0 behind)
	static void project(const View& v, V3 p, f32* fx, f32* fy, f32* fz)
	{
		const V3 d = p - v.pos;
		const f32 z = dot(d, v.fwd);
		*fz = z;
		if (z <= 0.15f) { *fx = *fy = 99.0f; return; }
		*fx = dot(d, v.right) / z / v.tanX;
		*fy = (-d.y / z - v.tanP) / v.tanY;
	}

	static bool personInFrame(const View& v, const Person& p)
	{
		f32 x, y, z;
		project(v, p.head, &x, &y, &z);
		if (z <= 0.15f) { return false; }
		if (p.player) { return fabsf(x) <= c_sfFitPlayerX && fabsf(y) <= c_sfFitPlayerY; }
		return fabsf(x) <= c_sfFitX && y <= c_sfFitUp && y >= -c_sfFitDown;
	}

	// Whether an enemy (its middle and size) is nearer a camera at cam than
	// the player, or level with them and over them: left out of the photo
	static bool inFront(V3 cam, V3 mid, f32 bodyR)
	{
		const Person& me = s_people[0];
		const V3 d = mid - cam;
		// (how far each is along the way to the player: one beside the
		// camera is nearer than the player, however far off to the side)
		const V3 toPlayer = me.mid - cam;
		const f32 fPlayer = len(toPlayer);
		if (fPlayer < 0.01f) { return true; }
		const f32 fd = maxf(dot(d, toPlayer) / fPlayer, 0.0f);
		const f32 dist = len(d);
		if (dist < c_sfLensClear || fd < fPlayer - c_sfFrontMargin) { return true; }
		if (fd >= fPlayer + c_sfBeside) { return false; }
		const V3 pv[2] = { me.head, me.mid };
		const f32 pr[2] = { me.headR, me.bodyR };
		for (s32 k = 0; k < 2; k++)
		{
			const V3 dp = pv[k] - cam;
			const f32 fdp = len(dp);
			if (fdp < 0.01f) { continue; }
			if (angleBetween(d, dp) < bodyR / dist + pr[k] / fdp) { return true; }
		}
		return false;
	}

	// Whether person i's face is seen from the camera: no wall, and nobody
	// nearer over it (but those left out)
	static bool faceClear(V3 cam, RSector* camSec, s32 i)
	{
		const Person& p = s_people[i];
		if (!clearLine(cam, camSec, p.head, p.headR)) { return false; }
		const V3 di = p.head - cam;
		const f32 fdi = len(di);
		if (fdi < 0.01f) { return false; }
		for (s32 j = 0; j <= s_blockerCount; j++)
		{
			Blocker bp;
			const Blocker* b = &bp;
			if (j < s_blockerCount) { b = &s_blockers[j]; }
			else
			{
				bp.obj = nullptr; bp.head = s_people[0].head; bp.mid = s_people[0].mid;
				bp.headR = s_people[0].headR; bp.bodyR = s_people[0].bodyR;
				if (p.player) { continue; }
			}
			if (j < s_blockerCount && b->obj == p.obj) { continue; }
			if (j < s_blockerCount && inFront(cam, b->mid, b->bodyR)) { continue; }
			const V3 pv[2] = { b->head, b->mid };
			const f32 pr[2] = { b->headR, b->bodyR };
			for (s32 k = 0; k < 2; k++)
			{
				const V3 dj = pv[k] - cam;
				const f32 fdj = len(dj);
				if (fdj < 0.01f || fdj >= fdi - 0.3f) { continue; }
				if (angleBetween(di, dj) < pr[k] / fdj + c_sfHeadMargin) { return false; }
			}
		}
		return true;
	}

	// How much of the frame things nearer than the player cover
	static f32 clutter(const View& v)
	{
		const f32 fPlayer = len(s_people[0].head - v.pos);
		f32 cover = 0.0f;
		for (s32 j = 0; j < s_blockerCount; j++)
		{
			const Blocker& b = s_blockers[j];
			if (inFront(v.pos, b.mid, b.bodyR)) { continue; }
			f32 x, y, z;
			project(v, b.mid, &x, &y, &z);
			if (z <= 0.15f || z >= fPlayer) { continue; }
			const f32 rx = b.bodyR / z / v.tanX, ry = b.bodyR / z / v.tanY;
			const f32 ox = clampf(minf(x + rx, 1.0f) - maxf(x - rx, -1.0f), 0.0f, 2.0f);
			const f32 oy = clampf(minf(y + ry, 1.0f) - maxf(y - ry, -1.0f), 0.0f, 2.0f);
			cover += ox * oy / 4.0f;
		}
		return cover;
	}

	// How much of the frame is a wall right by the camera (lines across the
	// frame, out as far as the player: 0 all clear .. 1 all wall)
	static f32 wallByCamera(const View& v, RSector* camSec)
	{
		static const f32 across[4] = { -0.85f, -0.45f, 0.45f, 0.85f };
		const f32 want = maxf(dot(s_people[0].head - v.pos, v.fwd), 2.0f);
		f32 blocked = 0.0f;
		for (f32 a : across)
		{
			const V3 to = v.pos + (v.fwd + v.right * (a * v.tanX)) * want + up(v.tanP * want);
			const f32 l = len(to - v.pos);
			blocked += 1.0f - clearDist(v.pos, camSec, to) / maxf(l, 0.01f);
		}
		return blocked / 4.0f;
	}

	static f32 score(const View& v, RSector* camSec, f32 dist, bool frameOnly)
	{
		if (!personInFrame(v, s_people[0])) { return -1e9f; }
		if (!frameOnly && !faceClear(v.pos, camSec, 0)) { return -1e9f; }
		f32 sc = 3.0f - c_sfDistCost * dist;
		if (!frameOnly) { sc -= c_sfClutterCost * clutter(v) + c_sfWallCost * wallByCamera(v, camSec); }
		for (s32 i = 1; i < s_peopleCount; i++)
		{
			const bool seen = personInFrame(v, s_people[i]) && !inFront(v.pos, s_people[i].mid, s_people[i].bodyR) &&
				(frameOnly || faceClear(v.pos, camSec, i));
			if (seen) { sc += 1.0f / (1.0f + 0.12f * f32(i - 1)); }
			else if (i <= c_sfFacesClear) { sc -= 4.0f; }
		}
		return sc;
	}

	static V3 aimPoint()
	{
		V3 v = s_people[0].head * 3.0f;
		f32 w = 3.0f;
		for (s32 i = 1; i < s_peopleCount; i++)
		{
			const f32 wi = 1.0f / (1.0f + 0.25f * f32(i - 1));
			v = v + s_people[i].head * wi;
			w += wi;
		}
		return v * (1.0f / w);
	}

	static bool selfieLeftOut(SecObject* obj, V3 cam)
	{
		for (s32 j = 0; j < s_blockerCount; j++)
		{
			if (s_blockers[j].obj == obj) { return inFront(cam, s_blockers[j].mid, s_blockers[j].bodyR); }
		}
		return false;
	}

	static f32 pitchDown(V3 cam, V3 v)
	{
		const V3 d = v - cam;
		return atan2f(d.y, maxf(lenXZ(d), 0.001f));	// (y down: looking down is > 0)
	}

	static bool placeSelfieCamera()
	{
		const V3 eye = s_people[0].head;
		V3 group = v3(0, 0, 0);
		f32 wg = 0.0f;
		for (s32 i = 1; i < s_peopleCount && i <= 8; i++) { group = group + s_people[i].head; wg += 1.0f; }
		V3 away = eye - (wg > 0.0f ? group * (1.0f / wg) : eye);
		away.y = 0.0f;
		const f32 awayYaw = (lenXZ(away) > 1.6f) ? atan2f(away.x, away.z) : s_lookYaw;
		static const s32 steps[c_sfDirs] = { 2, -2, 1, -1, 3, -3, 0, 4, -4, 5, -5, 6, -6, 7, -7, 8 };
		static const f32 dists[] = { 5.0f, 6.0f, 7.2f, 8.6f, 10.3f, 12.0f, 15.0f, 18.5f, 22.5f, 27.5f, 33.0f, 38.4f };
		const s32 distCount = s32(sizeof(dists) / sizeof(dists[0]));
		const V3 at = aimPoint();
		f32 best = -1e8f;
		V3 bestCam = eye;
		RSector* bestSec = nullptr;
		static const f32 liftCost[3] = { 0.0f, 0.4f, 0.8f };
		s_solidLeftOut = selfieLeftOut;
		for (s32 round = 0; round < 2 && best <= -1e7f; round++)
		{
			const f32 maxDist = round ? c_sfCamMax : c_sfCamNear;
			for (s32 pass = 0; pass < 3; pass++)
			{
				const f32 lift = (pass == 0) ? s_sfLiftNow : ((pass == 1) ? c_sfLiftHigh : c_sfLiftHigher);
				for (s32 k = 0; k < c_sfDirs; k++)
				{
					const f32 yaw = awayYaw + f32(steps[k]) * (2.0f * c_pi / f32(c_sfDirs)) * (s_phoneLeft ? -1.0f : 1.0f);
					const V3 dir = flatDir(yaw);
					const V3 far = eye + dir * c_sfCamMax + up(lift);
					const f32 free = walkLine(eye, s_eyeSector, far, c_sfClearance, nullptr) - c_sfClearance;
					for (s32 d = 0; d < distCount && dists[d] <= maxDist; d++)
					{
						const V3 cam = eye + dir * dists[d] + up(lift);
						if (len(cam - eye) > free) { break; }
						if (pitchDown(cam, at) > c_sfPitchDownMax || pitchDown(cam, eye + up(-1.0f)) > c_sfPitchDownMax) { continue; }
						View v;
						makeView(v, cam, at, 0.0f);
						if (score(v, nullptr, dists[d], true) - liftCost[pass] <= best + 0.001f) { continue; }
						RSector* camSec = nullptr;
						if (!roomAt(eye, s_eyeSector, cam, c_sfClearance, &camSec)) { continue; }
						const f32 sc = score(v, camSec, dists[d], false) - liftCost[pass];
						if (sc > best + 0.001f)
						{
							best = sc;
							bestCam = cam;
							bestSec = camSec;
						}
					}
				}
				if (best > -1e7f && s_peopleCount == 1) { break; }
			}
		}
		s_solidLeftOut = nullptr;
		if (best <= -1e7f || !bestSec) { return false; }
		s_sfCam = bestCam;
		s_sfCamSector = bestSec;
		RIGDBG("selfie cam %.2f %.2f %.2f score %.2f eye %.2f %.2f %.2f people %d\n", bestCam.x, bestCam.y, bestCam.z, best, eye.x, eye.y, eye.z, s_peopleCount);
		return true;
	}

	// A pose: an animation and frame of this sprite for a pose of this kind
	// (or the nearest kind it has)
	static s32 randomPoseKind()
	{
		const s32 r = randInt(100);
		if (r < 40) { return POSE_ATTACK; }
		if (r < 52) { return POSE_ANGER; }
		if (r < 77) { return POSE_IDLE; }
		if (r < 87) { return POSE_WOUND; }
		return POSE_MOVE;
	}

	static bool pickPose(SecObject* obj, s32 kind, s32* anim, s32* frame)
	{
		static const s32 order[POSE_KINDS][5] =
		{
			{ ANIM_ATTACK1, ANIM_ATTACK2, ANIM_ATTACK1_END, ANIM_IDLE, ANIM_MOVE },	// attack: aiming, firing
			{ ANIM_ATTACK2, ANIM_ATTACK1_END, ANIM_ATTACK1, ANIM_IDLE, ANIM_MOVE },	// anger: the other attack
			{ ANIM_IDLE, ANIM_MOVE, ANIM_ATTACK1, -1, -1 },							// standing about
			{ ANIM_PAIN, ANIM_IDLE, ANIM_MOVE, -1, -1 },								// a flinch
			{ ANIM_MOVE, ANIM_IDLE, ANIM_ATTACK1, -1, -1 },							// mid-stride
		};
		for (s32 i = 0; i < 5; i++)
		{
			const s32 action = order[kind][i];
			if (action < 0) { break; }
			const s32 a = waxAnimFor(obj, action);
			if (a < 0) { continue; }
			const WaxAnim* w = WAX_AnimPtr(obj->wax, a);
			*anim = a;
			// (standing: the first frame, as the game stands them; else any)
			*frame = (action == ANIM_IDLE) ? 0 : randInt(w->frameCount);
			return true;
		}
		return false;
	}

	// Shots and blasts in front of a face in the photo are left out
	static void hideInFront()
	{
		forEachObject([&](SecObject* obj)
		{
			const s32 kind = objKind(obj);
			if (kind != KIND_SHOT && kind != KIND_EFFECT) { return; }
			if (pc_isHidden(obj)) { return; }
			const V3 pos = fromFixed(obj->posWS) + up(fixed16ToFloat(max(obj->worldHeight, 0)) * 0.5f);
			const V3 dp = pos - s_sfCam;
			const f32 fdp = len(dp);
			if (fdp < 0.3f) { pc_hide(obj); return; }
			// (shots and blasts at the player: the figure stands for them, unhurt)
			const V3 me = fromFixed(s_playerObject->posWS);
			if (lenXZ(pos - me) < 3.5f && pos.y > me.y - c_figureHeight - 1.0f && pos.y < me.y + 1.0f) { pc_hide(obj); return; }
			// (nor any between the camera and the player: the player is the nearest in it)
			const V3 toPlayer = s_people[0].mid - s_sfCam;
			const f32 fPlayer = len(toPlayer);
			if (fPlayer > 0.01f && dot(dp, toPlayer) / fPlayer < fPlayer - 1.0f) { pc_hide(obj); return; }
			for (s32 i = 0; i < s_peopleCount; i++)
			{
				const Person& p = s_people[i];
				if (!p.inFrame) { continue; }
				const V3 dh = p.head - s_sfCam;
				const f32 fdh = len(dh);
				if (fdh < 0.01f || fdp >= fdh) { continue; }
				// (a face, and the body under it for the player)
				const f32 r = (p.player ? p.bodyR : p.headR * 1.5f);
				if (angleBetween(dp, dh) < r / fdh + 0.03f) { pc_hide(obj); return; }
				if (p.player && angleBetween(dp, p.mid - s_sfCam) < p.bodyR * 1.5f / fdh + 0.03f) { pc_hide(obj); return; }
			}
		});
	}

	static void selfiePose()
	{
		s_poseCount = 0;
		// The camera: aimed at the faces in the photo, the player near the
		// middle; now and then tilted a little
		V3 sum = v3(0, 0, 0);
		f32 cnt = 0.0f;
		for (s32 i = 0; i < s_peopleCount; i++)
		{
			if (i && !s_people[i].inFrame) { continue; }
			const f32 w = i ? 1.0f : 2.0f;
			sum = sum + s_people[i].head * w;
			cnt += w;
		}
		const V3 at = sum * (1.0f / cnt) + up(-0.5f);
		const f32 tilt = chance(3) ? (rand01() - 0.5f) * 2.0f * degToRad(4.0f) : 0.0f;
		View v;
		for (s32 t = 0; t <= 10; t++)
		{
			const f32 f = f32(t) * 0.1f;
			makeView(v, s_sfCam, at * (1.0f - f) + (s_people[0].head + up(-1.0f)) * f, tilt);
			f32 x, y, z;
			project(v, s_people[0].head, &x, &y, &z);
			if (z > 0.15f && fabsf(x) <= c_sfCentreX && fabsf(y) <= c_sfCentreY) { break; }
		}
		pc_place(s_sfCam, v.yaw, v.pitch, s_sfCamSector);
		for (s32 i = 1; i < s_peopleCount; i++)
		{
			if (!s_people[i].bomber)
			{
				s_people[i].inFrame = personInFrame(v, s_people[i]) && s_people[i].dist <= c_sfRange && !pc_isHidden(s_people[i].obj);
			}
		}

		s32 others = 0, bomber = -1;
		for (s32 i = 1; i < s_peopleCount; i++)
		{
			if (!s_people[i].inFrame) { continue; }
			others++;
			if (s_people[i].bomber) { bomber = i; }
		}
		const s32 groupKind = (others >= 2 && chance(c_sfGroupChance)) ? randomPoseKind() : -1;
		s32 rare = RARE_NONE, oblivious = -1;
		if (others >= 1 && chance(c_sfRareChance))
		{
			const s32 r = randInt(3);
			if (r == 0 && bomber >= 0) { rare = RARE_STARE; }
			else if (r == 1 && others >= 2) { rare = RARE_OBLIVIOUS; }
			else { rare = RARE_ALLEYES; }
			if (rare == RARE_OBLIVIOUS)
			{
				s32 k = randInt(others);
				for (s32 i = 1; i < s_peopleCount && oblivious < 0; i++)
				{
					if (s_people[i].inFrame && !s_people[i].bomber && k-- == 0) { oblivious = i; }
				}
			}
		}
		for (s32 i = 1; i < s_peopleCount; i++)
		{
			Person& p = s_people[i];
			if (!p.inFrame || i == oblivious || !objValid(p.obj)) { continue; }
			bool crowded = false;
			for (s32 j = 0; j < s_peopleCount && !crowded; j++)
			{
				const Person& o = s_people[j];
				if (j == i || !(j == 0 || o.inFrame)) { continue; }
				crowded = len(o.mid - p.mid) < c_sfCrowded * (o.bodyR + p.bodyR);
			}
			V3 look = s_sfCam;
			if (rare == RARE_STARE && i != bomber) { look = s_people[bomber].head; }
			if (rare == RARE_ALLEYES) { look = s_people[0].head; }
			f32 yaw = headingTo(p.pos, look) + (rand01() - 0.5f) * 2.0f * c_sfTurnSmall;
			if (!p.bomber && !crowded && rare == RARE_NONE && randInt(100) < c_sfTurnChance)
			{
				const f32 turn = c_sfTurnMin + rand01() * (c_sfTurnMax - c_sfTurnMin);
				yaw += chance(2) ? turn : -turn;
			}
			s32 kind = (groupKind >= 0) ? groupKind : randomPoseKind();
			if (crowded) { kind = POSE_IDLE; }
			if (p.bomber) { kind = chance(2) ? POSE_ATTACK : POSE_ANGER; }
			s32 anim = -1, frame = 0;
			if (!pickPose(p.obj, kind, &anim, &frame)) { anim = -1; }
			pc_setPose(p.obj, yaw, anim, frame);
		}
		// The player: turned to the camera, a little away from the phone's side
		const f32 toCam = headingTo(s_people[0].pos, s_sfCam);
		s_figureYaw = toCam + c_pi + (s_phoneLeft ? -c_sfPhoneTurn : c_sfPhoneTurn) + (rand01() - 0.5f) * 2.0f * c_sfTurnSmall;
		// (no one is over the player's face: those who would be were left
		// out as the camera was placed, and a sprite's pose doesn't move it)
		hideInFront();
	}

	static void findPeople()
	{
		s_peopleCount = 0;
		Person& me = s_people[s_peopleCount++];
		memset(&me, 0, sizeof(me));
		me.obj = nullptr;
		me.player = true;
		me.pos = fromFixed(s_playerObject->posWS);
		me.head = me.pos + up(c_figureHeight * 0.92f);	// (the figure's face)
		me.mid = me.pos + up(c_figureHeight * 0.52f);
		me.height = c_figureHeight;
		me.headR = 0.8f;
		me.bodyR = 1.6f;

		struct Found { SecObject* obj; f32 dist; };
		static Found found[512];
		s32 foundCount = 0;
		const V3 pl = me.pos;
		forEachObject([&](SecObject* obj)
		{
			if (foundCount >= 512 || objKind(obj) != KIND_ENEMY) { return; }
			const f32 d = len(fromFixed(obj->posWS) - pl);
			if (d > c_sfBombRange) { return; }
			found[foundCount].obj = obj;
			found[foundCount].dist = d;
			foundCount++;
		});
		for (s32 i = 1; i < foundCount; i++)
		{
			const Found f = found[i];
			s32 j = i;
			for (; j > 0 && found[j - 1].dist > f.dist; j--) { found[j] = found[j - 1]; }
			found[j] = f;
		}
		s_blockerCount = 0;
		for (s32 i = 0; i < foundCount && s_blockerCount < c_sfMaxBlockers; i++)
		{
			Blocker& b = s_blockers[s_blockerCount++];
			const Measure m = measure(found[i].obj);
			b.obj = found[i].obj; b.head = m.head; b.mid = m.mid; b.headR = m.headR; b.bodyR = m.bodyR;
		}
		bool wantBomber = chance(c_sfBombChance);
		for (s32 i = 0; i < foundCount && s_peopleCount < c_sfMaxPersons; i++)
		{
			const bool near = found[i].dist <= c_sfRange && s_peopleCount <= s_maxPeople;
			if (!near && !wantBomber) { continue; }
			Person& p = s_people[s_peopleCount];
			const Measure m = measure(found[i].obj);
			p.obj = found[i].obj;
			p.player = false;
			p.dist = found[i].dist;
			p.pos = fromFixed(found[i].obj->posWS);
			p.head = m.head; p.mid = m.mid; p.height = m.height; p.headR = m.headR; p.bodyR = m.bodyR;
			p.inFrame = false;
			// (in the player's sight)
			if (!clearLine(s_eye, s_eyeSector, p.head, p.headR) && !clearLine(s_eye, s_eyeSector, p.mid, p.bodyR)) { continue; }
			p.bomber = !near;
			if (!near) { wantBomber = false; }
			s_peopleCount++;
		}
	}

	static bool selfiePrepare()
	{
		randSeed(u32(SDL_GetTicks()) * 2654435761u + 12345u);
		s_flash = true;
		s_phoneLeft = randInt(10) < 4;
		const s32 r = randInt(100);
		s_sfLiftNow = (r < 20) ? c_sfLiftUp : ((r < 35) ? c_sfLiftDown : c_sfLift);
		findSolids(c_sfCamMax + 20.0f);
		findPeople();
		// the photobomber is only in it if they would be in the photo
		s32 bomberIdx = -1;
		for (s32 i = 1; i < s_peopleCount; i++) { if (s_people[i].bomber) { bomberIdx = i; } }
		Person bomber;
		if (bomberIdx >= 0)
		{
			bomber = s_people[bomberIdx];
			for (s32 i = bomberIdx; i < s_peopleCount - 1; i++) { s_people[i] = s_people[i + 1]; }
			s_peopleCount--;
		}
		if (!placeSelfieCamera())
		{
			s_sfFailed = true;
			return false;
		}
		// enemies between the camera and the player left out (posing or
		// not); and those nearer than the player with their heads out of the
		// frame (only a big piece of them would be in it)
		View v;
		makeView(v, s_sfCam, aimPoint(), 0.0f);
		const f32 fPlayer = len(s_people[0].mid - s_sfCam);
		for (s32 j = 0; j < s_blockerCount; j++)
		{
			const Blocker& b = s_blockers[j];
			f32 x, y, z;
			project(v, b.head, &x, &y, &z);
			const bool headOut = z <= 0.15f || fabsf(x) > 1.0f || fabsf(y) > 1.0f;
			if (inFront(s_sfCam, b.mid, b.bodyR) || (headOut && len(b.mid - s_sfCam) < fPlayer)) { pc_hide(b.obj); }
		}
		// (and the dying and the dead in front, as the living)
		forEachObject([&](SecObject* obj)
		{
			if (obj == s_playerObject || obj->type != OBJ_TYPE_SPRITE || !(obj->flags & OBJ_FLAG_NEEDS_TRANSFORM)) { return; }
			if (!(obj->entityFlags & (ETFLAG_AI_ACTOR | ETFLAG_CORPSE)) || objKind(obj) == KIND_ENEMY) { return; }
			const Measure m = measure(obj);
			if (len(m.mid - s_sfCam) < 60.0f && inFront(s_sfCam, m.mid, m.bodyR)) { pc_hide(obj); }
		});
		for (s32 i = 0; i < s_peopleCount; i++)
		{
			s_people[i].inFrame = i == 0 || (personInFrame(v, s_people[i]) && s_people[i].dist <= c_sfRange && !pc_isHidden(s_people[i].obj));
		}
		if (bomberIdx >= 0 && s_peopleCount < c_sfMaxPersons)
		{
			s_people[s_peopleCount] = bomber;
			if (personInFrame(v, bomber) && !pc_isHidden(bomber.obj) && faceClear(s_sfCam, s_sfCamSector, s_peopleCount))
			{
				s_people[s_peopleCount].inFrame = true;
				s_peopleCount++;
			}
		}
		s_hidePlayer = false;
		selfiePose();
		s_sfPosed = true;
		return true;
	}

	//////////////////////////////////////////////////////////////////
	// Action Freeze
	//////////////////////////////////////////////////////////////////
	// Sam's (IOSFreeze.cpp), in Dark Forces units: the camera turns round a
	// focus (what the player looks at, else a point ahead)
	static const f32 c_fzFocusMax = 48.0f, c_fzFocusMin = 2.6f, c_fzFocusAhead = 12.8f, c_fzFocusReach = 1.5f;
	static const f32 c_fzWallGap = 0.6f, c_fzDistMin = 1.0f, c_fzDistMax = 190.0f, c_fzCamRoom = 0.5f;
	static const f32 c_fzPitchMax = degToRad(55.0f);
	static const s32 c_fzStepsIn = 12, c_fzFitHalvings = 7, c_fzHoldIn = 12;
	static const f32 c_fzEaseOut = 0.25f, c_fzNearHead = 2.2f;

	static bool s_fzActive = false, s_fzReady = false, s_fzSmooth = false, s_fzInsideMe = false;
	static V3 s_fzFocus;
	static RSector* s_fzFocusSector = nullptr;
	static f32 s_fzYaw = 0.0f, s_fzPitch = 0.0f, s_fzWanted = 1.0f, s_fzShown = 1.0f, s_fzMinDist = c_fzDistMin;
	static s32 s_fzHeldIn = 0;

	static bool fzFits(V3 focus, RSector* focusSec, V3 cam, RSector** camSec)
	{
		return roomAt(focus, focusSec, cam, c_fzCamRoom, camSec);
	}

	// The camera at this orbit, as far out as the level lets it: how far
	// (-1: nowhere)
	static f32 fzProbeAt(V3 focus, RSector* focusSec, f32 yaw, f32 pitch, f32 dist, f32 minDist, V3* pos, RSector** camSec)
	{
		const V3 dir = lookDir(yaw, pitch);
		const f32 want = maxf(dist, minDist);
		f32 d0 = walkLine(focus, focusSec, focus - dir * want, c_fzCamRoom, nullptr);
		if (d0 < want - 0.01f) { d0 -= c_fzWallGap; }
		else { d0 = want; }
		if (d0 < minDist) { return -1.0f; }
		if (fzFits(focus, focusSec, focus - dir * d0, camSec)) { *pos = focus - dir * d0; return d0; }
		f32 fit = -1.0f, noFit = d0;
		for (s32 k = 1; k <= c_fzStepsIn; k++)
		{
			const f32 d = minDist + (d0 - minDist) * (1.0f - f32(k) / f32(c_fzStepsIn));
			if (fzFits(focus, focusSec, focus - dir * d, nullptr)) { fit = d; break; }
			noFit = d;
		}
		if (fit < 0.0f)
		{
			// (nowhere nearer: further out, past whoever is in the way)
			const f32 far = walkLine(focus, focusSec, focus - dir * c_fzDistMax, c_fzCamRoom, nullptr) - c_fzWallGap;
			for (s32 k = 1; k <= c_fzStepsIn; k++)
			{
				const f32 d = d0 + f32(k) * maxf(d0, 3.2f) * 0.25f;
				if (d > far) { break; }
				if (fzFits(focus, focusSec, focus - dir * d, camSec)) { *pos = focus - dir * d; return d; }
			}
			return -1.0f;
		}
		for (s32 h = 0; h < c_fzFitHalvings; h++)
		{
			const f32 d = (fit + noFit) * 0.5f;
			if (fzFits(focus, focusSec, focus - dir * d, nullptr)) { fit = d; }
			else { noFit = d; }
		}
		*pos = focus - dir * fit;
		fzFits(focus, focusSec, *pos, camSec);
		return fit;
	}

	static void fzPlace()
	{
		const V3 dir = lookDir(s_fzYaw, s_fzPitch);
		const V3 cam = s_fzFocus - dir * s_fzShown;
		RSector* camSec = nullptr;
		walkLine(s_fzFocus, s_fzFocusSector, cam, 0.0f, &camSec);
		pc_place(cam, s_fzYaw, s_fzPitch, camSec);
		s_hidePlayer = s_fzInsideMe || len(cam - s_eye) < c_fzNearHead;
	}

	static bool fzGo(V3 focus, RSector* focusSec, f32 yaw, f32 pitch, f32 wanted, bool exact)
	{
		V3 pos;
		RSector* camSec = nullptr;
		const f32 d = fzProbeAt(focus, focusSec, yaw, pitch, wanted, s_fzMinDist, &pos, &camSec);
		if (d < 0.0f) { return false; }
		s_fzFocus = focus;
		s_fzFocusSector = focusSec;
		s_fzYaw = yaw;
		s_fzPitch = pitch;
		s_fzWanted = wanted;
		if (exact || !s_fzSmooth || d < s_fzShown)
		{
			if (d < s_fzShown - 0.001f) { s_fzHeldIn = 0; }
			s_fzShown = d;
		}
		else
		{
			s_fzShown = minf(s_fzShown, d);
		}
		fzPlace();
		return true;
	}

	static bool freezePrepare()
	{
		findSolids(c_fzDistMax);
		const V3 fwd = lookDir(s_lookYaw, s_lookPitch);
		// the first wall ahead, and the nearest thing on the line of sight
		const f32 wall = walkLine(s_eye, s_eyeSector, s_eye + fwd * c_fzFocusMax, 0.1f, nullptr) - c_fzWallGap;
		f32 focus = minf(c_fzFocusAhead, wall);
		f32 best = 1e9f, bestR = 0.0f;
		forEachObject([&](SecObject* obj)
		{
			const s32 kind = objKind(obj);
			if (kind != KIND_ENEMY && kind != KIND_SHOT && kind != KIND_EFFECT && kind != KIND_ITEM) { return; }
			const Measure m = measure(obj);
			const f32 along = dot(m.mid - s_eye, fwd);
			if (along < c_fzFocusMin || along > wall || along >= best) { return; }
			const f32 off = len(m.mid - (s_eye + fwd * along));
			const f32 size = maxf(m.height * 0.5f, 0.6f);
			if (off > c_fzFocusReach * size) { return; }
			best = along;
			bestR = m.width;
		});
		if (best < 1e8f) { focus = best; }
		focus = maxf(focus, c_fzDistMin);
		s_fzMinDist = maxf(c_fzDistMin, bestR + c_fzCamRoom + 1.0f);
		s_fzFocus = s_eye + fwd * focus;
		s_fzFocusSector = nullptr;
		walkLine(s_eye, s_eyeSector, s_fzFocus, 0.0f, &s_fzFocusSector);
		if (!s_fzFocusSector) { s_fzFocusSector = s_eyeSector; }
		s_fzYaw = s_lookYaw;
		s_fzPitch = clampf(s_lookPitch, -c_fzPitchMax, c_fzPitchMax);
		s_fzWanted = s_fzShown = focus;
		s_fzHeldIn = c_fzHoldIn;
		s_fzInsideMe = true;
		// (exactly the player's view: from the eyes)
		pc_place(s_eye, s_lookYaw, s_lookPitch, s_eyeSector);
		s_hidePlayer = true;
		s_fzReady = true;
		return true;
	}

	static bool fzIsReady() { return s_fzActive && s_fzReady; }

	static bool fzSetOrbit(V3 focus, RSector* focusSec, f32 yaw, f32 pitch, f32 dist, f32 minDist)
	{
		if (!fzIsReady()) { return false; }
		s_fzMinDist = maxf(minDist, c_fzDistMin);
		pitch = clampf(pitch, -c_fzPitchMax, c_fzPitchMax);
		if (!fzGo(focus, focusSec, yaw, pitch, clampf(dist, s_fzMinDist, c_fzDistMax), true)) { return false; }
		s_fzInsideMe = false;
		fzPlace();
		return true;
	}

	//////////////////////////////////////////////////////////////////
	// The cinematic burst
	//////////////////////////////////////////////////////////////////
	// Sam's (IOSCinema.cpp), in Dark Forces units
	static const f32 c_cnSubjectRange = 80.0f, c_cnSubjectAngle = degToRad(50.0f), c_cnPairRange = 32.0f;
	static const f32 c_cnDistMin = 9.6f, c_cnDistMax = 25.6f, c_cnHeroDist = 9.6f;
	static const f32 c_cnPitchHigh = degToRad(-30.0f), c_cnPitchLow = degToRad(5.0f), c_cnPitchEnd = degToRad(-40.0f);
	static const f32 c_cnHeroPitch = degToRad(-12.0f);
	static const f32 c_cnSweep[] = { degToRad(90.0f), degToRad(70.0f), degToRad(50.0f), degToRad(35.0f), degToRad(20.0f) };
	static const f32 c_cnRise[] = { degToRad(-14.0f), degToRad(-6.0f) };
	static const f32 c_cnRoom = 0.7f, c_cnRoomLeast = 0.4f;
	static const s32 c_cnSeenLeast = 3;
	static const f32 c_cnZoomStep = 1.15f;
	static const f64 c_cnSettle = 0.1, c_cnShow = 0.5, c_cnGiveUp = 3.0;
	enum { SUBJECT_PAIR = 0, SUBJECT_ONE, SUBJECT_HERO, SUBJECT_KEPT };

	static s32 s_cnState = IOSCINEMA_IDLE;
	static bool s_cnPlanned = false, s_cnAsked = false, s_cnZoomOnly = false, s_cnOrbit = false;
	static s32 s_cnTaken = 0, s_cnDir = 1;
	static f64 s_cnStart = 0, s_cnStage = 0, s_cnLast = 0;
	static f32 s_cnSweepNow = 0, s_cnRiseNow = 0, s_cnArcMiddle = 0, s_cnDist0 = 0, s_cnDist1 = 0;
	static V3 s_cnFocus;
	static RSector* s_cnFocusSector = nullptr;
	static f32 s_cnBaseYaw = 0, s_cnBasePitch = 0, s_cnMinDist = 1.0f;
	static SecObject* s_cnSubject[2] = { nullptr, nullptr };	// (the player: &s_figure)

	struct CnPerson { SecObject* obj; V3 mid; f32 r; };
	static const s32 c_cnMaxPeople = 256;
	static CnPerson s_cnPeople[c_cnMaxPeople];
	static s32 s_cnPeopleCount = 0;

	static void cnMiddle(SecObject* obj, V3* mid, f32* r)
	{
		if (!obj || obj == &s_figure)
		{
			*mid = fromFixed(s_playerObject->posWS) + up(c_figureHeight * 0.5f);
			*r = 1.2f;
			return;
		}
		const Measure m = measure(obj);
		*mid = m.mid;
		*r = maxf(m.width, 1.0f);
	}

	static f32 cnArcAt(s32 k) { return s_cnOrbit ? f32(k - 1) / f32(IOSCINEMA_SHOTS - 2) : f32(k) / f32(IOSCINEMA_SHOTS - 1); }
	static f32 cnDistAt(f32 t) { return s_cnDist0 + (s_cnDist1 - s_cnDist0) * t; }
	static f32 cnHeading(V3 d)
	{
		if (lenXZ(d) < 0.001f) { return s_lookYaw; }
		return atan2f(d.x, d.z);
	}

	// Whether `target` is in clear view from pos: no wall, nobody but
	// skipA and skipB across the line (nor the camera inside anyone)
	static bool cnSeesPoint(V3 pos, RSector* posSec, V3 target, const SecObject* skipA, const SecObject* skipB)
	{
		const V3 v = target - pos;
		const f32 l = len(v);
		if (l < 0.01f) { return true; }
		if (clearDist(pos, posSec, target) < l - 1.0f) { return false; }
		const V3 vn = v * (1.0f / l);
		for (s32 i = 0; i < s_cnPeopleCount; i++)
		{
			const CnPerson& p = s_cnPeople[i];
			V3 d = p.mid - pos;
			if (p.obj == &s_figure && len(s_eye - pos) < c_fzNearHead) { continue; }
			if (len(d) < p.r * 1.2f) { return false; }
			if (p.obj == skipA || p.obj == skipB) { continue; }
			const f32 along = dot(d, vn);
			if (along < 1.0f || along > l - 1.0f) { continue; }
			d = d - vn * along;
			if (len(d) < p.r * 1.1f) { return false; }
		}
		return true;
	}

	// The focus in clear view, and for a pair each of the two as well
	static bool cnSees(V3 pos, RSector* posSec)
	{
		if (!cnSeesPoint(pos, posSec, s_cnFocus, s_cnSubject[0], s_cnSubject[1])) { return false; }
		if (!s_cnSubject[0] || !s_cnSubject[1]) { return true; }
		for (s32 k = 0; k < 2; k++)
		{
			V3 mid; f32 r;
			cnMiddle(s_cnSubject[k], &mid, &r);
			if (!cnSeesPoint(pos, posSec, mid, s_cnSubject[k], nullptr)) { return false; }
		}
		return true;
	}

	static SecObject* cnPickSubject(f32* dist, s32* kindOut)
	{
		const V3 fwd = lookDir(s_lookYaw, s_lookPitch);
		const f32 cosMax = cosf(c_cnSubjectAngle);
		SecObject* best = nullptr;
		f32 bestScore = 0.0f;
		forEachObject([&](SecObject* obj)
		{
			const s32 kind = objKind(obj);
			f32 bonus;
			if (kind == KIND_ENEMY) { bonus = 0.2f; }
			else if (kind == KIND_EFFECT) { bonus = 0.25f; }
			else if (kind == KIND_SHOT) { bonus = 0.15f; }
			else { return; }
			V3 mid; f32 r;
			cnMiddle(obj, &mid, &r);
			const V3 v = mid - s_eye;
			const f32 d = len(v);
			if (d < 1.0f || d > c_cnSubjectRange) { return; }
			const f32 c = dot(v, fwd) / d;
			if (c < cosMax) { return; }
			if (!clearLine(s_eye, s_eyeSector, mid, r)) { return; }
			const f32 ang = acosf(minf(c, 1.0f));
			const f32 sc = (1.0f - ang / c_cnSubjectAngle) * 0.55f + (1.0f - d / c_cnSubjectRange) * 0.25f + bonus;
			if (sc > bestScore) { bestScore = sc; best = obj; *dist = d; *kindOut = kind; }
		});
		return best;
	}

	static f32 cnTryArc(s32 dir, f32 sweep, f32 rise, s32* seen)
	{
		f32 least = 1.0f;
		*seen = 0;
		for (s32 k = 1; k < IOSCINEMA_SHOTS; k++)
		{
			const f32 t = cnArcAt(k);
			const f32 want = cnDistAt(t);
			V3 pos;
			RSector* posSec = nullptr;
			const f32 d = fzProbeAt(s_cnFocus, s_cnFocusSector, s_cnBaseYaw + f32(dir) * sweep * (t - s_cnArcMiddle), s_cnBasePitch + rise * t,
				want, s_cnMinDist, &pos, &posSec);
			if (d < 0.0f) { return -1.0f; }
			least = minf(least, d / want);
			if (cnSees(pos, posSec)) { (*seen)++; }
		}
		return least;
	}

	static void cnChoosePath()
	{
		const s32 sweeps = s32(sizeof(c_cnSweep) / sizeof(c_cnSweep[0]));
		const s32 rises = s32(sizeof(c_cnRise) / sizeof(c_cnRise[0]));
		f32 bestScore = -1.0f, bestSweep = 0.0f, bestRise = 0.0f, bestRoom = -1.0f;
		s32 bestDir = 1, bestSeen = 0;
		for (s32 s = 0; s < sweeps; s++)
		{
			f32 okRoom = -1.0f, okRise = 0.0f;
			s32 okDir = 1;
			for (s32 r = 0; r < rises; r++)
			{
				f32 rise = c_cnRise[r];
				if (s_cnOrbit && s_cnBasePitch + rise < c_cnPitchEnd) { rise = c_cnPitchEnd - s_cnBasePitch; }
				if (rise > 0.0f) { rise = 0.0f; }
				for (s32 dir = 1; dir >= -1; dir -= 2)
				{
					s32 seen = 0;
					const f32 room = cnTryArc(dir, c_cnSweep[s], rise, &seen);
					if (room < 0.0f) { continue; }
					if (room >= c_cnRoom && seen == IOSCINEMA_SHOTS - 1 && room > okRoom + 0.05f)
					{
						okRoom = room; okRise = rise; okDir = dir;
					}
					const f32 sc = f32(seen) * 0.5f + room + c_cnSweep[s] / degToRad(90.0f) * 0.3f;
					if (sc > bestScore)
					{
						bestScore = sc; bestSweep = c_cnSweep[s]; bestRise = rise; bestDir = dir; bestRoom = room; bestSeen = seen;
					}
				}
			}
			if (okRoom >= 0.0f)
			{
				s_cnSweepNow = c_cnSweep[s]; s_cnRiseNow = okRise; s_cnDir = okDir; s_cnZoomOnly = false;
				return;
			}
		}
		if (bestRoom >= c_cnRoomLeast && bestSeen >= c_cnSeenLeast)
		{
			s_cnSweepNow = bestSweep; s_cnRiseNow = bestRise; s_cnDir = bestDir; s_cnZoomOnly = false;
			return;
		}
		s_cnSweepNow = s_cnRiseNow = 0.0f;
		s_cnDir = 1;
		s_cnDist1 = s_cnDist0;
		s_cnZoomOnly = true;
	}

	static bool cnTryPlan(s32 kind, SecObject* obj, f32 dist, V3 keptFocus, RSector* keptSec, f32 keptYaw, f32 keptPitch, f32 keptDist, f32 keptMin)
	{
		f32 minDist = keptMin;
		V3 playerMid; f32 playerR;
		cnMiddle(&s_figure, &playerMid, &playerR);
		V3 focus = keptFocus;
		RSector* focusSec = keptSec;
		f32 dist1 = keptDist, yaw = keptYaw, pitch = keptPitch;
		s_cnSubject[0] = s_cnSubject[1] = nullptr;
		s_cnArcMiddle = 0.0f;
		s_cnOrbit = true;
		if (kind == SUBJECT_PAIR)
		{
			V3 mid; f32 r;
			cnMiddle(obj, &mid, &r);
			const V3 v = mid - playerMid;
			const f32 apart = len(v);
			focus = playerMid + v * 0.5f;
			f32 tanX, tanY;
			viewTans(&tanX, &tanY);
			dist1 = (apart * 0.5f + 2.6f) / (tanY * 1.4f);
			V3 side = v3(-v.z, 0.0f, v.x);
			if (dot(side, s_eye - focus) < 0.0f) { side = -side; }
			yaw = cnHeading(-side);
			pitch = degToRad(-10.0f);
			s_cnArcMiddle = 0.5f;
			s_cnSubject[0] = obj;
			s_cnSubject[1] = &s_figure;
			minDist = 1.0f;
		}
		else if (kind == SUBJECT_ONE)
		{
			f32 r;
			cnMiddle(obj, &focus, &r);
			minDist = r + 1.5f;
			dist1 = dist;
			const V3 v = focus - s_eye;
			if (len(v) > 0.01f)
			{
				yaw = cnHeading(v);
				pitch = atan2f(-v.y, maxf(lenXZ(v), 0.001f));
			}
			s_cnSubject[0] = obj;
		}
		else if (kind == SUBJECT_HERO)
		{
			const V3 fwd = flatDir(fromAngle14(s_playerObject->yaw));
			focus = playerMid;
			minDist = playerR + 1.5f;
			dist1 = c_cnHeroDist;
			yaw = cnHeading(-fwd);
			pitch = c_cnHeroPitch;
			s_cnArcMiddle = 0.5f;
			s_cnSubject[0] = &s_figure;
		}
		else
		{
			s_cnOrbit = false;
			s_cnSubject[0] = obj;
			s_cnSubject[1] = &s_figure;
		}
		if (kind != SUBJECT_KEPT)
		{
			dist1 = clampf(dist1, c_cnDistMin, c_cnDistMax);
			pitch = clampf(pitch, c_cnPitchHigh, c_cnPitchLow);
			focusSec = nullptr;
			walkLine(s_eye, s_eyeSector, focus, 0.0f, &focusSec);
			if (!focusSec || !inSectorVol(focusSec, focus, 0.0f)) { focusSec = sectorAt(focus, focusSec); }
			if (!focusSec) { return false; }
		}
		V3 pos;
		if (fzProbeAt(focus, focusSec, yaw, pitch, dist1, minDist, &pos, nullptr) < 0.0f) { return false; }
		s_cnMinDist = minDist;
		s_cnFocus = focus;
		s_cnFocusSector = focusSec;
		s_cnBaseYaw = yaw;
		s_cnBasePitch = pitch;
		s_cnDist0 = s_cnDist1 = dist1;
		cnChoosePath();
		return !s_cnZoomOnly;
	}

	static void cnPlan()
	{
		const V3 keptFocus = s_fzFocus;
		RSector* keptSec = s_fzFocusSector;
		const f32 keptYaw = s_fzYaw, keptPitch = s_fzPitch, keptDist = s_fzWanted, keptMin = s_fzMinDist;
		s_cnMinDist = keptMin;
		s_cnFocus = keptFocus;
		s_cnFocusSector = keptSec;
		s_cnBaseYaw = keptYaw;
		s_cnBasePitch = keptPitch;
		s_cnDist0 = s_cnDist1 = keptDist;
		s_cnZoomOnly = false;
		s_cnPeopleCount = 0;
		{
			CnPerson& me = s_cnPeople[s_cnPeopleCount++];
			me.obj = &s_figure;
			cnMiddle(&s_figure, &me.mid, &me.r);
		}
		forEachObject([&](SecObject* obj)
		{
			if (s_cnPeopleCount >= c_cnMaxPeople || objKind(obj) != KIND_ENEMY) { return; }
			CnPerson& p = s_cnPeople[s_cnPeopleCount++];
			p.obj = obj;
			cnMiddle(obj, &p.mid, &p.r);
		});
		f32 dist = 0.0f;
		s32 subjectKind = KIND_NONE;
		SecObject* obj = cnPickSubject(&dist, &subjectKind);
		s32 kinds[4], ct = 0;
		if (obj && subjectKind == KIND_ENEMY)
		{
			V3 mid, playerMid; f32 r;
			cnMiddle(obj, &mid, &r);
			cnMiddle(&s_figure, &playerMid, &r);
			if (len(mid - playerMid) < c_cnPairRange) { kinds[ct++] = SUBJECT_PAIR; }
		}
		if (obj) { kinds[ct++] = SUBJECT_ONE; }
		kinds[ct++] = SUBJECT_HERO;
		kinds[ct++] = SUBJECT_KEPT;
		for (s32 i = 0; i < ct; i++)
		{
			if (cnTryPlan(kinds[i], obj, dist, keptFocus, keptSec, keptYaw, keptPitch, keptDist, keptMin))
			{
				RIGDBG("cinema plan %d sweep %.0f dir %d focus %.1f %.1f %.1f dist %.1f\n", kinds[i], s_cnSweepNow * 180.0f / c_pi, s_cnDir,
					s_cnFocus.x, s_cnFocus.y, s_cnFocus.z, s_cnDist1);
				return;
			}
		}
		s_cnZoomOnly = true;
		s_cnSweepNow = s_cnRiseNow = 0.0f;
	}

	static void cnMoveTo(s32 k)
	{
		if (s_cnZoomOnly)
		{
			IOSFreeze_Zoom(c_cnZoomStep);
			return;
		}
		const f32 t = cnArcAt(k);
		fzSetOrbit(s_cnFocus, s_cnFocusSector, s_cnBaseYaw + f32(s_cnDir) * s_cnSweepNow * (t - s_cnArcMiddle), s_cnBasePitch + s_cnRiseNow * t,
			cnDistAt(t), s_cnMinDist);
	}

	//////////////////////////////////////////////////////////////////
	// The mission's frame
	//////////////////////////////////////////////////////////////////
	bool iosCamera_photoView()
	{
		return s_pcActive && s_pcPrepared && !s_pcFailed;
	}

	bool iosCamera_drawWorld(u8* display, RSector* eyeSector, const u8* colormap, const u8* lightSourceRamp)
	{
		if (s_pcActive && !playerCanPose())
		{
			// (the level ending, the player dying: the overlay ends it)
			s_pcFailed = true;
		}
		if (s_pcActive && !s_pcPrepared && !s_pcFailed)
		{
			// The first frame of the view: the game's ticks waiting for it
			// have run, the game holds still from now on
			s_pcPrepared = true;
			pc_readEye();
			s_eyeSector = sectorAt(s_eye, s_playerObject->sector);
			if (!s_eyeSector) { s_eyeSector = s_playerObject->sector; }
			s_camSector = s_eyeSector;
			s_camPos = s_eye; s_camYaw = s_lookYaw; s_camPitch = s_lookPitch;
		#ifdef IOS_CAMERA_RIG_DEBUG
			const u64 t0 = SDL_GetPerformanceCounter();
		#endif
			if (s_pcPrepare && !s_pcPrepare()) { s_pcFailed = true; }
			RIGDBG("prepare %.1f ms\n", f64(SDL_GetPerformanceCounter() - t0) * 1000.0 / f64(SDL_GetPerformanceFrequency()));
		}
		if (iosCamera_photoView() && s_camSector)
		{
			pc_draw(display, colormap, lightSourceRamp);
			return true;
		}
		if (eyeSector) { drawWorld(display, eyeSector, colormap, lightSourceRamp); }
		return false;
	}
}

using namespace TFE_DarkForces;

extern "C"
{
	// ---------------------------------------------------------- selfies
	int IOSSelfie_Begin(void)
	{
		if (s_sfActive || s_pcActive) { return 0; }
		s_sfPosed = s_sfFailed = false;
		s_peopleCount = 0;
		if (!pc_begin(selfiePrepare)) { return 0; }
		s_sfActive = true;
		return 1;
	}

	int IOSSelfie_IsActive(void) { return s_sfActive ? 1 : 0; }
	int IOSSelfie_IsPosed(void) { return (s_sfActive && s_sfPosed) ? 1 : 0; }
	int IOSSelfie_Failed(void) { return (s_sfActive && (s_sfFailed || s_pcFailed)) ? 1 : 0; }

	void IOSSelfie_Repose(void)
	{
		if (!s_sfActive || !s_sfPosed) { return; }
		selfiePose();
	}

	void IOSSelfie_End(void)
	{
		if (!s_sfActive) { return; }
		s_peopleCount = 0;
		s_blockerCount = 0;
		s_sfPosed = s_sfFailed = false;
		s_sfActive = false;
		pc_end();
	}

	void IOSSelfie_SetMaxPeople(int ct)
	{
		s_maxPeople = ct < 0 ? 0 : (ct > IOSSELFIE_MAX_PEOPLE_LIMIT ? IOSSELFIE_MAX_PEOPLE_LIMIT : ct);
	}

	int IOSSelfie_NumPosed(void)
	{
		int ct = 0;
		for (s32 i = 1; i < s_peopleCount; i++) { ct += s_people[i].inFrame ? 1 : 0; }
		return ct;
	}

	// ---------------------------------------------------------- Action Freeze
	int IOSFreeze_Begin(void)
	{
		if (s_fzActive || s_pcActive) { return 0; }
		s_fzReady = false;
		s_fzSmooth = false;
		if (!pc_begin(freezePrepare)) { return 0; }
		s_fzActive = true;
		return 1;
	}

	int IOSFreeze_Orbit(float yawDeg, float pitchDeg)
	{
		if (!fzIsReady()) { return 1; }
		// (+yaw: the view turns left; the game's heading grows to the right)
		const f32 pitch = clampf(s_fzPitch + degToRad(pitchDeg), -c_fzPitchMax, c_fzPitchMax);
		const f32 yaw = wrapAngle(s_fzYaw - degToRad(yawDeg));
		if (!fzGo(s_fzFocus, s_fzFocusSector, yaw, pitch, maxf(s_fzWanted, s_fzMinDist), false)) { return 0; }
		s_fzInsideMe = false;
		fzPlace();
		return 1;
	}

	void IOSFreeze_Pan(float right, float down)
	{
		if (!fzIsReady()) { return; }
		// as far across as the view's height at the focus
		f32 tanX, tanY;
		viewTans(&tanX, &tanY);
		const f32 h = 2.0f * maxf(s_fzShown, c_fzDistMin) * tanY;
		const V3 r = v3(cosf(s_fzYaw), 0.0f, -sinf(s_fzYaw));
		const V3 u = v3(sinf(s_fzYaw) * sinf(s_fzPitch), -cosf(s_fzPitch), cosf(s_fzYaw) * sinf(s_fzPitch));
		// (what is seen follows the fingers: the camera goes the other way)
		V3 move = (r * -right + u * down) * h;
		const f32 l = len(move);
		if (l < 0.0001f) { return; }
		RSector* endSec = nullptr;
		const f32 free = walkLine(s_fzFocus, s_fzFocusSector, s_fzFocus + move, c_fzCamRoom, nullptr) - c_fzWallGap;
		if (free <= 0.0f) { return; }
		if (free < l) { move = move * (free / l); }
		const V3 focus = s_fzFocus + move;
		walkLine(s_fzFocus, s_fzFocusSector, focus, 0.0f, &endSec);
		if (!endSec) { return; }
		if (fzGo(focus, endSec, s_fzYaw, s_fzPitch, maxf(s_fzWanted, s_fzMinDist), false)) { s_fzInsideMe = false; }
		fzPlace();
	}

	// (the game's view can't be narrowed: zooming moves the camera in and out)
	void IOSFreeze_Zoom(float factor)
	{
		if (!fzIsReady() || factor <= 0.0f) { return; }
		const f32 wanted = clampf(maxf(s_fzWanted, s_fzMinDist) / factor, s_fzMinDist, c_fzDistMax);
		if (fzGo(s_fzFocus, s_fzFocusSector, s_fzYaw, s_fzPitch, wanted, factor > 1.0f)) { s_fzInsideMe = false; }
		fzPlace();
	}

	void IOSFreeze_SetSmooth(int bOn) { s_fzSmooth = bOn != 0; }

	void IOSFreeze_Frame(void)
	{
		if (!fzIsReady() || !s_fzSmooth) { return; }
		if (s_fzHeldIn < c_fzHoldIn) { s_fzHeldIn++; return; }
		if (s_fzShown >= s_fzWanted - 0.005f) { return; }
		V3 pos;
		const f32 d = fzProbeAt(s_fzFocus, s_fzFocusSector, s_fzYaw, s_fzPitch, s_fzWanted, s_fzMinDist, &pos, nullptr);
		if (d <= s_fzShown + 0.005f) { return; }
		s_fzShown += (d - s_fzShown) * c_fzEaseOut;
		fzPlace();
	}

	void IOSFreeze_End(void)
	{
		if (!s_fzActive) { return; }
		s_fzActive = s_fzReady = false;
		pc_end();
	}

	int IOSFreeze_IsActive(void) { return s_fzActive ? 1 : 0; }

	// ---------------------------------------------------------- the cinematic burst
	int IOSCinema_Start(double now)
	{
		if (s_cnState != IOSCINEMA_IDLE) { return IOSCINEMA_CANT; }
		if (IOSCapture_IsPending() || !IOSCapture_HasRoom(IOSCAPTURE_CLEAN, IOSCINEMA_SHOTS)) { return IOSCINEMA_BUSY; }
		if (!IOSFreeze_Begin()) { return IOSCINEMA_CANT; }
		IOSFreeze_SetSmooth(0);
		s_cnPlanned = false;
		s_cnTaken = 0;
		s_cnAsked = false;
		s_cnStart = s_cnStage = s_cnLast = now;
		s_cnState = IOSCINEMA_TAKING;
		IOSPhotos_SetBatch(1);
		return IOSCINEMA_STARTED;
	}

	int IOSCinema_Tick(double now)
	{
		if (s_cnState == IOSCINEMA_IDLE) { return IOSCINEMA_IDLE; }
		s_cnLast = now;
		if (!IOSFreeze_IsActive() || s_pcFailed)
		{
			IOSCinema_Stop();
			return s_cnState;
		}
		if (!s_cnPlanned)
		{
			if (!fzIsReady())
			{
				if (now - s_cnStart >= c_cnGiveUp)
				{
					IOSCinema_Stop();
					IOS_ShowCameraMessage("Couldn't take the photos");
				}
				return s_cnState;
			}
			// As the freeze's first frame was drawn: the plan, then back to the
			// player's view for the first photo
			const V3 pos = s_camPos;
			const f32 yaw = s_camYaw, pitch = s_camPitch;
			RSector* sec = s_camSector;
			cnPlan();
			pc_place(pos, yaw, pitch, sec);
			s_hidePlayer = true;
			s_cnPlanned = true;
			s_cnStage = now;
		}
		if (s_cnState == IOSCINEMA_SHOWING)
		{
			if (now - s_cnStage >= c_cnShow) { IOSCinema_Stop(); }
			return s_cnState;
		}
		if (s_cnAsked && !IOSCapture_IsPending())
		{
			s_cnAsked = false;
			s_cnTaken++;
			s_cnStage = now;
			if (s_cnTaken >= IOSCINEMA_SHOTS)
			{
				s_cnState = IOSCINEMA_SHOWING;
				return s_cnState;
			}
			cnMoveTo(s_cnTaken);
		}
		if (!s_cnAsked)
		{
			const bool due = s_cnTaken > 0 || now - s_cnStage >= c_cnSettle;
			if (due && IOSCapture_Request(IOSCAPTURE_CLEAN, IOSCAPTURE_MODE_BURST))
			{
				s_cnAsked = true;
				s_cnStage = now;
			}
		}
		if (now - s_cnStage >= c_cnGiveUp + (s_cnTaken ? 0.0 : c_cnSettle))
		{
			const s32 taken = s_cnTaken;
			IOSCinema_Stop();
			char msg[64];
			if (taken) { snprintf(msg, sizeof(msg), "Took %d of %d photos", taken, IOSCINEMA_SHOTS); }
			else { snprintf(msg, sizeof(msg), "Couldn't take the photos"); }
			IOS_ShowCameraMessage(msg);
		}
		return s_cnState;
	}

	void IOSCinema_Stop(void)
	{
		if (s_cnState == IOSCINEMA_IDLE) { return; }
		if (s_cnAsked && IOSCapture_IsPending()) { IOSCapture_Cancel(); }
		s_cnAsked = false;
		s_cnState = IOSCINEMA_IDLE;
		s_cnSubject[0] = s_cnSubject[1] = nullptr;
		IOSFreeze_End();
		IOSCapture_NoteFreeze(s_cnLast - s_cnStart);
		IOSPhotos_SetBatch(0);
	}

	int IOSCinema_IsActive(void) { return s_cnState != IOSCINEMA_IDLE ? 1 : 0; }
	int IOSCinema_NumTaken(void) { return s_cnTaken; }
}
#endif
