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
#include <TFE_Jedi/Renderer/rlimits.h>
#include <TFE_Jedi/Renderer/RClassic_Float/rclassicFloatSharedState.h>
#include <TFE_Jedi/Renderer/RClassic_GPU/modelGPU.h>
#include <TFE_Jedi/Renderer/virtualFramebuffer.h>
#include <TFE_Asset/modelAsset_jedi.h>
#include <TFE_Asset/spriteAsset_Jedi.h>
#include <TFE_Settings/settings.h>
#include <TFE_RenderBackend/renderBackend.h>
#include <TFE_Game/reticle.h>
#include <SDL.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <initializer_list>
#include <algorithm>

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
		// (the GPU renderer has the game's models in its buffers from when the
		// level was loaded: the figure joins them the first time it's needed)
		if (!model->drawId && TFE_Settings::getGraphicsSettings()->rendererIndex != 0) { model_loadGpuModels(); }
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

	// (moved: drawn somewhere else for the photo, in that sector's list)
	// Objects added to the photo only while it is drawn (fireworks)
	static const s32 c_maxExtras = 12;
	struct Extra { SecObject obj; RSector* sector; u32 dirty; };
	static Extra s_extras[c_maxExtras];
	static s32 s_extraCount = 0;

	struct Pose { SecObject* obj; angle14_32 yaw; s32 anim, frame; bool moved; vec3_fixed pos; RSector* sector; };
	struct Saved
	{
		SecObject* obj; angle14_32 yaw; s32 anim, frame; u32 flags;
		bool moved; vec3_fixed pos; RSector* oldSec; s32 oldIndex; RSector* newSec; s32 newIndex; u32 oldDirty, newDirty;
	};

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

	static bool s_reticleWasOn = false;	// (the crosshair, off while the photo view is drawn)

	static void pc_end()
	{
		s_extraCount = 0;
		if (s_reticleWasOn)
		{
			// (a menu that opened meanwhile turns it back on as it closes)
			if (!escapeMenu_isOpen() && !pda_isOpen()) { reticle_enable(true); }
			s_reticleWasOn = false;
		}
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
		p.moved = false;
		p.sector = nullptr;
	}

	static void pc_movePose(SecObject* obj, V3 feet, RSector* sector)
	{
		for (s32 i = 0; i < s_poseCount; i++)
		{
			if (s_poses[i].obj != obj) { continue; }
			s_poses[i].moved = true;
			s_poses[i].pos = { floatToFixed16(feet.x), floatToFixed16(feet.y), floatToFixed16(feet.z) };
			s_poses[i].sector = sector;
			return;
		}
		// (not posed yet: as they stand, until a pose is set)
		if (s_poseCount >= c_maxPoses) { return; }
		Pose& p = s_poses[s_poseCount++];
		p.obj = obj;
		p.yaw = obj->yaw;
		p.anim = -1;
		p.frame = 0;
		p.moved = true;
		p.pos = { floatToFixed16(feet.x), floatToFixed16(feet.y), floatToFixed16(feet.z) };
		p.sector = sector;
	}

	// Into a sector's list, in a free slot (only where there's none does
	// the list grow, as the game's own grows it)
	static void listAdd(RSector* sector, SecObject* obj)
	{
		for (s32 i = 0; i < sector->objectCapacity; i++)
		{
			if (!sector->objectList[i])
			{
				sector->objectList[i] = obj;
				obj->index = i;
				obj->sector = sector;
				sector->objectCount++;
				return;
			}
		}
		sector_addObjectDirect(sector, obj);
	}

	static void listRemove(SecObject* obj)
	{
		RSector* sector = obj->sector;
		if (sector && obj->index >= 0 && obj->index < sector->objectCapacity && sector->objectList[obj->index] == obj)
		{
			sector->objectList[obj->index] = nullptr;
			sector->objectCount--;
		}
		obj->sector = nullptr;
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

	static void pc_unhide(SecObject* obj)
	{
		for (s32 i = 0; i < s_hiddenCount; i++)
		{
			if (s_hidden[i] == obj) { s_hidden[i] = s_hidden[--s_hiddenCount]; return; }
		}
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
			s.moved = false;
			obj->yaw = s_poses[i].yaw;
			if (s_poses[i].anim >= 0)
			{
				obj->anim = s_poses[i].anim;
				obj->frame = s_poses[i].frame;
			}
			if (s_poses[i].moved && s_poses[i].sector)
			{
				s.moved = true;
				s.pos = obj->posWS;
				s.oldSec = obj->sector; s.oldIndex = obj->index; s.oldDirty = obj->sector->dirtyFlags;
				s.newSec = s_poses[i].sector; s.newDirty = s.newSec->dirtyFlags;
				listRemove(obj);
				listAdd(s.newSec, obj);
				s.newIndex = obj->index;
				obj->posWS = s_poses[i].pos;
			}
		}
		for (s32 i = 0; i < s_hiddenCount; i++)
		{
			SecObject* obj = s_hidden[i];
			if (!objValid(obj)) { continue; }
			Saved& s = saved[savedCount++];
			s.obj = obj; s.yaw = obj->yaw; s.anim = obj->anim; s.frame = obj->frame; s.flags = obj->flags;
			s.moved = false;
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
			s.moved = false;
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
			listAdd(figureSector, &s_figure);
		}
		for (s32 i = 0; i < s_extraCount; i++)
		{
			Extra& e = s_extras[i];
			e.obj.self = &e.obj;
			e.obj.sector = nullptr;
			e.dirty = e.sector->dirtyFlags;
			listAdd(e.sector, &e.obj);
		}
		// What the renderer marks for the automap
		const u32 sectorCount = s_levelState.sectorCount;
		s_savedSectorState.resize(sectorCount * 6);
		s_savedWallState.clear();
		RSector* sec = s_levelState.sectors;
		for (u32 i = 0; i < sectorCount; i++, sec++)
		{
			u32* st = &s_savedSectorState[i * 6];
			st[0] = sec->flags1; st[1] = u32(sec->prevDrawFrame); st[2] = u32(sec->prevDrawFrame2);
			st[3] = u32(sec->startWall); st[4] = u32(sec->drawWallCnt);
			st[5] = sec->dirtyFlags;	// (the GPU renderer's: what it still has to update, as first seen)
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
		// (what the renderer notes as drawn, for the game's auto-aim: as the
		// player's own view left it)
		SecObject* drawnObj[MAX_DRAWN_OBJ_STORE];
		const s32 drawnCount = s_drawnObjCount;
		memcpy(drawnObj, s_drawnObj, sizeof(drawnObj));
		drawWorld(display, s_camSector, colormap, lightSourceRamp);
		s_drawnObjCount = drawnCount;
		memcpy(s_drawnObj, drawnObj, sizeof(drawnObj));

		// Everything back as it was
		sec = s_levelState.sectors;
		size_t wi = 0;
		for (u32 i = 0; i < sectorCount; i++, sec++)
		{
			const u32* st = &s_savedSectorState[i * 6];
			sec->flags1 = (sec->flags1 & ~SEC_FLAGS1_RENDERED) | (st[0] & SEC_FLAGS1_RENDERED);
			sec->prevDrawFrame = s32(st[1]); sec->prevDrawFrame2 = s32(st[2]);
			sec->startWall = s32(st[3]); sec->drawWallCnt = s32(st[4]);
			sec->dirtyFlags = st[5];
			for (s32 w = 0; w < sec->wallCount && wi + 2 < s_savedWallState.size(); w++)
			{
				RWall* wall = &sec->walls[w];
				wall->seen = JBool(s_savedWallState[wi++]);
				wall->visible = s_savedWallState[wi++];
				wall->drawFrame = s_savedWallState[wi++];
			}
		}
		for (s32 i = s_extraCount - 1; i >= 0; i--)
		{
			Extra& e = s_extras[i];
			listRemove(&e.obj);
			e.sector->dirtyFlags = e.dirty;
		}
		if (figureSector)
		{
			// (out of the list as it went in: no messages, no other change)
			listRemove(&s_figure);
			figureSector->dirtyFlags = figureDirty;
		}
		for (s32 i = savedCount - 1; i >= 0; i--)
		{
			const Saved& s = saved[i];
			s.obj->yaw = s.yaw; s.obj->anim = s.anim; s.obj->frame = s.frame; s.obj->flags = s.flags;
			if (s.moved)
			{
				// (back into its own slot, where it was)
				listRemove(s.obj);
				s.oldSec->objectList[s.oldIndex] = s.obj;
				s.oldSec->objectCount++;
				s.obj->sector = s.oldSec;
				s.obj->index = s.oldIndex;
				s.obj->posWS = s.pos;
				s.newSec->dirtyFlags = s.newDirty;
				s.oldSec->dirtyFlags = s.oldDirty;
			}
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
	// Group photos (CAMERA's long press): the player's phone held out or up
	// where they stand, turned to the enemies nearest them, who turn to the
	// lens and fool about for it, each photo a different game (a party, a
	// photobomb, someone posing on the floor, a class photo, fireworks
	// overhead, a conga). The player isn't in it. Everything is posed,
	// moved or added only while the photo view is drawn (see pc_draw).
	//////////////////////////////////////////////////////////////////
	// Who: enemies within c_gpRange of the player, in their sight, the
	// nearest IOSSELFIE_MAX_PEOPLE (as set); nothing but a photobomber
	// nearer the lens than c_gpNear
	static const f32 c_gpRange = 64.0f;
	static const f32 c_gpNear = 7.5f;
	static const s32 c_gpMaxPersons = IOSSELFIE_MAX_PEOPLE_LIMIT;
	// The camera tips at most this much (the classic renderer shears, the
	// GPU one turns: this little, both frame alike)
	static const f32 c_gpPitchMax = degToRad(12.0f);
	// What fits (fractions of the half frame)
	static const f32 c_gpFitX = 0.86f, c_gpFitUp = 0.78f, c_gpFitDown = 0.80f;
	static const f32 c_gpHeadMargin = 0.02f;
	static const s32 c_gpShotsKept = 8;

	enum { POSE_ATTACK = 0, POSE_ANGER, POSE_IDLE, POSE_WOUND, POSE_MOVE, POSE_KINDS };
	enum { THEME_PARTY = 0, THEME_PHOTOBOMB, THEME_FLOOR, THEME_CLASS, THEME_FIREWORKS, THEME_CONGA, THEMES };
	static const s32 c_themeWeights[THEMES] = { 24, 18, 14, 14, 16, 14 };
	static const s32 c_themeLeast[THEMES] = { 1, 1, 2, 2, 1, 2 };	// (people it needs)
	enum { CAM_EYE = 0, CAM_RAISED, CAM_LOW, CAM_STYLES };
	static const s32 c_styleWeights[CAM_STYLES] = { 50, 32, 18 };

	// A choice at random by weight, but not `notThis` (the last photo's)
	static s32 pickWeighted(const s32* weights, s32 n, s32 notThis)
	{
		s32 total = 0;
		for (s32 i = 0; i < n; i++) { if (i != notThis) { total += weights[i]; } }
		s32 r = randInt(max(total, 1));
		for (s32 i = 0; i < n; i++)
		{
			if (i == notThis) { continue; }
			if (r < weights[i]) { return i; }
			r -= weights[i];
		}
		return 0;
	}

	struct Person
	{
		SecObject* obj;
		f32 dist;				// (from the player)
		V3 pos, head, mid;
		V3 homePos, homeHead, homeMid;	// (where they really stand: a photo's moves are its own)
		f32 height, headR, bodyR, width;
		bool inFrame;
	};

	static bool s_sfActive = false, s_sfPosed = false, s_sfFailed = false, s_sfNoOne = false;
	static Person s_people[c_gpMaxPersons];
	static s32 s_peopleCount = 0;
	static s32 s_maxPeople = IOSSELFIE_MAX_PEOPLE;
	static s32 s_gpTheme = -1, s_gpThemeBefore = -1, s_gpStyle = -1, s_gpShot = 0;
	static SecObject* s_gpBomber = nullptr;
	static V3 s_gpUsedCams[c_gpShotsKept];
	static f32 s_gpUsedYaws[c_gpShotsKept];
	static s32 s_gpUsedCount = 0;

	// (gpu: the GPU renderer turns the view up and down; the classic one shears it)
	struct View { V3 pos; f32 yaw, pitch, tanX, tanY, tanP; V3 fwd, right; bool gpu = false; };

	static void finishView(View& v)
	{
		viewTans(&v.tanX, &v.tanY);
		v.tanP = tanf(v.pitch);
		v.fwd = flatDir(v.yaw);
		v.right = v3(cosf(v.yaw), 0.0f, -sinf(v.yaw));
		v.gpu = TFE_Settings::getGraphicsSettings()->rendererIndex != 0;
	}

	static View viewAt(V3 cam, f32 yaw, f32 pitch)
	{
		View v;
		v.pos = cam;
		v.yaw = yaw;
		v.pitch = pitch;
		finishView(v);
		return v;
	}

	// Where a point is in the view (across, up: fractions of the half frame;
	// how far in front, <= 0 behind)
	static void project(const View& v, V3 p, f32* fx, f32* fy, f32* fz)
	{
		const V3 d = p - v.pos;
		if (v.gpu)
		{
			// (turned: forward and up tipped by the pitch)
			const f32 cp = cosf(v.pitch), sp = sinf(v.pitch);
			const V3 f3 = v3(v.fwd.x * cp, -sp, v.fwd.z * cp);
			const V3 u3 = v3(-v.fwd.x * sp, -cp, -v.fwd.z * sp);
			const f32 z = dot(d, f3);
			*fz = z;
			if (z <= 0.15f) { *fx = *fy = 99.0f; return; }
			*fx = dot(d, v.right) / z / v.tanX;
			*fy = dot(d, u3) / z / v.tanY;
			return;
		}
		const f32 z = dot(d, v.fwd);
		*fz = z;
		if (z <= 0.15f) { *fx = *fy = 99.0f; return; }
		*fx = dot(d, v.right) / z / v.tanX;
		*fy = (-d.y / z - v.tanP) / v.tanY;
	}

	// A point of the frame (-1..1 across, up) at a distance, in the level
	static V3 pointAt(const View& v, f32 fx, f32 depth)
	{
		return v.pos + v.fwd * depth + v.right * (fx * v.tanX * depth);
	}

	static bool headInFrame(const View& v, V3 head, f32* z)
	{
		f32 x, y;
		project(v, head, &x, &y, z);
		return *z > 0.15f && fabsf(x) <= c_gpFitX && y <= c_gpFitUp && y >= -c_gpFitDown;
	}

	// Whether person i's face is seen from the camera: no wall, nobody nearer
	// over it (but those too near the lens, who are left out)
	static bool faceClear(V3 cam, RSector* camSec, s32 i)
	{
		const Person& p = s_people[i];
		if (!clearLine(cam, camSec, p.head, p.headR)) { return false; }
		const V3 di = p.head - cam;
		const f32 fdi = len(di);
		if (fdi < 0.01f) { return false; }
		for (s32 j = 0; j < s_peopleCount; j++)
		{
			if (j == i) { continue; }
			const Person& b = s_people[j];
			if (len(b.mid - cam) < c_gpNear) { continue; }
			const V3 pv[2] = { b.head, b.mid };
			const f32 pr[2] = { b.headR, b.bodyR };
			for (s32 k = 0; k < 2; k++)
			{
				const V3 dj = pv[k] - cam;
				const f32 fdj = len(dj);
				if (fdj < 0.01f || fdj >= fdi - 0.3f) { continue; }
				if (angleBetween(di, dj) < pr[k] / fdj + c_gpHeadMargin) { return false; }
			}
		}
		return true;
	}

	// How far the view reaches (lines across the frame, out to 24 units:
	// 0 a wall right there .. 1 all open): into the room, not at a wall
	static f32 viewDepth(const View& v, RSector* camSec)
	{
		static const f32 across[5] = { -0.8f, -0.4f, 0.0f, 0.4f, 0.8f };
		const f32 want = 24.0f;
		f32 open = 0.0f;
		for (f32 a : across)
		{
			V3 d = v.fwd + v.right * (a * v.tanX) + up(v.tanP);
			d = d * (1.0f / len(d));
			open += clearDist(v.pos, camSec, v.pos + d * want) / want;
		}
		return open / 5.0f;
	}

	// The middle of the group: the nearest heads, the nearer counting more
	// (only those about the player's level, not on a walkway far above)
	static bool groupMiddle(V3* at)
	{
		V3 v = v3(0, 0, 0);
		f32 w = 0.0f;
		for (s32 i = 0; i < s_peopleCount && i < 10; i++)
		{
			const Person& p = s_people[i];
			if (fabsf(p.head.y - s_eye.y) > 7.0f) { continue; }
			const f32 wi = 1.0f / (1.0f + 0.3f * f32(i));
			v = v + p.head * wi;
			w += wi;
		}
		if (w <= 0.0f) { return false; }
		*at = v * (1.0f / w);
		return true;
	}

	// How good a group photo from here is: the people near the player in it
	// at a size that reads (more of them better), faces clear, none too near
	// the lens, a view into the room
	static f32 groupScore(const View& v, RSector* camSec)
	{
		f32 sc = 3.0f * viewDepth(v, camSec);
		for (s32 i = 0; i < s_peopleCount; i++)
		{
			const Person& p = s_people[i];
			const bool nearPlayer = i < 4 || p.dist < 24.0f;
			f32 z;
			if (!headInFrame(v, p.head, &z)) { if (nearPlayer) { sc -= 2.5f; } continue; }
			if (len(p.mid - v.pos) < c_gpNear) { sc -= 2.5f; continue; }
			if (!faceClear(v.pos, camSec, i)) { if (nearPlayer) { sc -= 1.2f; } continue; }
			const f32 size = p.height / (z * 2.0f * v.tanY);
			const f32 readable = (size >= 0.18f) ? (size <= 0.75f ? 1.0f : 0.6f) : maxf(size / 0.18f, 0.15f);
			sc += 1.5f * readable / (1.0f + 0.06f * f32(i));
		}
		return sc;
	}

	// The camera: where the player stands (held out a little, back a step,
	// to the side, or raised), turned to the group
	static bool placeGroupCamera()
	{
		V3 mid;
		if (!groupMiddle(&mid)) { mid = s_people[0].head; }
		const V3 eye = s_eye;
		const f32 toGroup = headingTo(eye, mid);
		const V3 fwd = flatDir(toGroup), right = v3(cosf(toGroup), 0.0f, -sinf(toGroup));
		static const f32 backs[] = { -3.0f, -1.5f, 0.0f, 1.0f };
		static const f32 sides[] = { -2.0f, 0.0f, 2.0f };
		struct Cand { V3 cam; RSector* sec; f32 yaw, pitch, score; };
		Cand best = { eye, nullptr, 0, 0, -1e9f };
		for (s32 attempt = 0; attempt < 2 && !best.sec; attempt++)
		{
			const s32 style = attempt ? CAM_EYE : s_gpStyle;
			for (f32 b : backs) for (f32 sd : sides) for (s32 t = 0; t < 2; t++)
			{
				f32 lift = 0.3f;
				if (style == CAM_RAISED) { lift = 1.4f + f32(t) * 1.0f; }
				if (style == CAM_LOW) { lift = -1.0f - f32(t) * 0.4f; }
				const V3 cam = eye + fwd * b + right * sd + up(lift);
				RSector* camSec = nullptr;
				if (!roomAt(eye, s_eyeSector, cam, 0.6f, &camSec)) { continue; }
				for (s32 a = -1; a <= 1; a++)
				{
					const f32 yaw = headingTo(cam, mid) + f32(a) * degToRad(9.0f);
					const V3 d = mid + up(-1.2f) - cam;
					f32 pitch = atan2f(-d.y, maxf(lenXZ(d), 0.1f));
					if (style == CAM_LOW) { pitch += degToRad(3.0f); }
					pitch = clampf(pitch, -c_gpPitchMax, c_gpPitchMax);
					const View v = viewAt(cam, yaw, pitch);
					f32 sc = groupScore(v, camSec) - 4.0f * (1.0f - viewDepth(v, camSec)) + rand01() * 0.7f;
					for (s32 u = 0; u < s_gpUsedCount; u++)
					{
						// (not the same picture as an earlier photo)
						const f32 nearBy = 1.0f - minf(len(cam - s_gpUsedCams[u]) / 3.0f, 1.0f);
						const f32 same = 1.0f - minf(fabsf(wrapAngle(yaw - s_gpUsedYaws[u])) / degToRad(25.0f), 1.0f);
						sc -= 2.5f * nearBy * same;
					}
					if (sc > best.score) { best = { cam, camSec, yaw, pitch, sc }; }
				}
			}
		}
		if (!best.sec)
		{
			// (crowded in on every side: from the player's own eyes, turned to them)
			best = { eye, s_eyeSector, headingTo(eye, mid), 0.0f, 0.0f };
			const V3 d = mid + up(-1.2f) - eye;
			best.pitch = clampf(atan2f(-d.y, maxf(lenXZ(d), 0.1f)), -c_gpPitchMax, c_gpPitchMax);
		}
		if (s_gpUsedCount < c_gpShotsKept)
		{
			s_gpUsedCams[s_gpUsedCount] = best.cam;
			s_gpUsedYaws[s_gpUsedCount] = best.yaw;
			s_gpUsedCount++;
		}
		const f32 tilt = chance(4) ? (rand01() - 0.5f) * degToRad(6.0f) : 0.0f;
		pc_place(best.cam, best.yaw, clampf(best.pitch + tilt, -c_gpPitchMax, c_gpPitchMax), best.sec);
		RIGDBG("group cam %.2f %.2f %.2f yaw %.0f pitch %.0f score %.2f style %d\n", best.cam.x, best.cam.y, best.cam.z,
			best.yaw * 180.0f / c_pi, best.pitch * 180.0f / c_pi, best.score, s_gpStyle);
		return true;
	}

	// ---- poses
	// A frame of one of the sprite's own animations for an action: any one
	// of it, or (last) the frame an attack fires on; -1: it has none
	static bool pickAction(SecObject* obj, s32 action, s32 frameWanted, s32* anim, s32* frame)
	{
		const s32 a = waxAnimFor(obj, action);
		if (a < 0) { return false; }
		const WaxAnim* w = WAX_AnimPtr(obj->wax, a);
		*anim = a;
		*frame = (frameWanted == -2) ? w->frameCount - 1 : (frameWanted >= 0 ? min(frameWanted, w->frameCount - 1) : randInt(w->frameCount));
		return true;
	}

	static bool pickPose(SecObject* obj, s32 kind, s32* anim, s32* frame)
	{
		static const s32 order[POSE_KINDS][5] =
		{
			{ ANIM_ATTACK1, ANIM_ATTACK2, ANIM_ATTACK1_END, ANIM_IDLE, ANIM_MOVE },
			{ ANIM_ATTACK2, ANIM_ATTACK1_END, ANIM_ATTACK1, ANIM_IDLE, ANIM_MOVE },
			{ ANIM_IDLE, ANIM_MOVE, ANIM_ATTACK1, -1, -1 },
			{ ANIM_PAIN, ANIM_IDLE, ANIM_MOVE, -1, -1 },
			{ ANIM_MOVE, ANIM_IDLE, ANIM_ATTACK1, -1, -1 },
		};
		for (s32 i = 0; i < 5; i++)
		{
			const s32 action = order[kind][i];
			if (action < 0) { break; }
			if (pickAction(obj, action, action == ANIM_IDLE ? 0 : -1, anim, frame)) { return true; }
		}
		return false;
	}

	static f32 sideways() { return (chance(2) ? 1.0f : -1.0f) * degToRad(70.0f + rand01() * 30.0f); }

	// Someone having fun: blasters fired off to the side (never at the lens),
	// a flinch that reads as a laugh or a dance, a leap in mid-stride, or
	// just standing about grinning
	static void partyPose(const View& v, const Person& p)
	{
		f32 yaw = headingTo(p.pos, v.pos);
		s32 anim = -1, frame = 0;
		const s32 r = randInt(100);
		if (r < 32)
		{
			// (a side view: the gun points away)
			yaw += sideways();
			if (!pickAction(p.obj, chance(3) ? ANIM_ATTACK2 : ANIM_ATTACK1, chance(2) ? -2 : -1, &anim, &frame)) { pickPose(p.obj, POSE_IDLE, &anim, &frame); }
		}
		else if (r < 58)
		{
			yaw += (rand01() - 0.5f) * degToRad(30.0f);
			if (!pickAction(p.obj, ANIM_PAIN, -1, &anim, &frame)) { pickPose(p.obj, POSE_IDLE, &anim, &frame); }
		}
		else if (r < 82)
		{
			yaw += (rand01() - 0.5f) * degToRad(70.0f);
			if (!pickAction(p.obj, ANIM_MOVE, -1, &anim, &frame)) { pickPose(p.obj, POSE_IDLE, &anim, &frame); }
		}
		else
		{
			yaw += (rand01() - 0.5f) * degToRad(12.0f);
			pickPose(p.obj, POSE_IDLE, &anim, &frame);
		}
		pc_setPose(p.obj, yaw, anim, frame);
	}

	static void standPose(const View& v, const Person& p, f32 turn)
	{
		s32 anim = -1, frame = 0;
		if (!pickPose(p.obj, POSE_IDLE, &anim, &frame)) { anim = -1; }
		pc_setPose(p.obj, headingTo(p.pos, v.pos) + turn, anim, frame);
	}

	// A place on the floor at this point of the frame (-1..1 across) and
	// distance, the camera's line to it clear: its feet and sector
	static bool floorSpot(const View& v, f32 fx, f32 depth, f32 height, V3* feet, RSector** sec)
	{
		const V3 at = pointAt(v, fx, depth);
		RSector* s = nullptr;
		if (walkLine(v.pos, s_camSector, at, 0.4f, &s) < len(at - v.pos) - 0.01f || !s) { return false; }
		*feet = v3(at.x, fixed16ToFloat(s->floorHeight), at.z);
		if (feet->y - height < fixed16ToFloat(s->ceilingHeight) + 0.3f) { return false; }
		if (fabsf(feet->y - fromFixed(s_playerObject->posWS).y) > 4.0f) { return false; }
		*sec = s;
		return true;
	}

	static void movePerson(Person& p, V3 feet, RSector* sec)
	{
		pc_movePose(p.obj, feet, sec);
		p.pos = feet;
		p.head = feet + up(p.height * 0.85f);
		p.mid = feet + up(p.height * 0.5f);
	}

	// The people in the photo (and the posable ones among them), the
	// nearest first; how far the group stands
	static s32 inFramePeople(const View& v, s32* list, s32 maxCount, f32* groupDepth)
	{
		s32 n = 0;
		f32 depthSum = 0.0f;
		for (s32 i = 0; i < s_peopleCount && n < maxCount; i++)
		{
			Person& p = s_people[i];
			f32 z;
			p.inFrame = headInFrame(v, p.head, &z) || headInFrame(v, p.mid, &z);
			if (!p.inFrame || pc_isHidden(p.obj) || !objValid(p.obj)) { p.inFrame = false; continue; }
			list[n++] = i;
			depthSum += z;
		}
		*groupDepth = n ? depthSum / f32(n) : 10.0f;
		return n;
	}

	// ---- added for the photo: fireworks (the game's own blasts and bolts)

	// Fireworks: the game's own blasts and bolts, added behind the group and
	// over their heads (the blasts' bottoms above the tallest head)
	static void addFirework(const View& v, f32 groupDepth, f32 topY)
	{
		// (named as the game loads them for its own blasts and shots, so these
		// are the very ones it has, ready for either renderer)
		static const char* blasts[] = { "genexp.wax", "detexp.wax", "missexp.wax", "mortexp.wax", "exptiny.wax", "plasexp.wax" };
		const s32 count = 3 + randInt(4);
		for (s32 k = 0, tries = 0; k < count && s_extraCount < c_maxExtras && tries < 24; tries++)
		{
			const bool bolt = (tries & 1) && chance(2);
			const f32 depth = groupDepth + (bolt ? 2.0f + rand01() * 6.0f : 5.0f + rand01() * 10.0f);
			V3 at = pointAt(v, (rand01() * 2.0f - 1.0f) * 0.85f, depth);
			Extra& e = s_extras[s_extraCount];
			memset(&e.obj, 0, sizeof(e.obj));
			e.obj.self = &e.obj;
			e.obj.worldWidth = e.obj.worldHeight = -1;
			f32 bottom;
			if (bolt)
			{
				JediModel* model = TFE_Model_Jedi::get(chance(3) ? "wgbolt.3do" : "wrbolt.3do", POOL_GAME);
				if (!model) { continue; }
				at.y = topY - 1.5f - rand01() * 4.0f;
				bottom = at.y;
				obj3d_setData(&e.obj, model);
				e.obj.flags = OBJ_FLAG_NEEDS_TRANSFORM | OBJ_FLAG_FULLBRIGHT;
				e.obj.yaw = toAngle14(v.yaw + sideways());
				e.obj.pitch = toAngle14(degToRad(10.0f + rand01() * 30.0f));
				obj3d_computeTransform(&e.obj);
			}
			else
			{
				JediWax* wax = TFE_Sprite_Jedi::getWax(blasts[randInt(6)], POOL_GAME);
				const WaxAnim* anim = wax ? WAX_AnimPtr(wax, 0) : nullptr;
				const WaxView* view = anim ? WAX_ViewPtr(wax, anim, 0) : nullptr;
				if (!view || anim->frameCount <= 0) { continue; }
				const s32 f = randInt(anim->frameCount);
				const WaxFrame* frame = WAX_FramePtr(wax, view, f);
				if (!frame) { continue; }
				// (drawn from its point up by its offset: its bottom over the heads)
				const f32 offsetY = fixed16ToFloat(frame->offsetY);
				at.y = topY - 1.0f - rand01() * 3.0f - offsetY;
				bottom = at.y + offsetY;
				e.obj.wax = wax;
				e.obj.type = OBJ_TYPE_SPRITE;
				e.obj.flags = OBJ_FLAG_NEEDS_TRANSFORM | OBJ_FLAG_FULLBRIGHT;
				e.obj.worldWidth = 0;
				e.obj.anim = 0;
				e.obj.frame = f;
			}
			RSector* sec = nullptr;
			const V3 low = v3(at.x, maxf(at.y, bottom), at.z);
			if (walkLine(v.pos, s_camSector, v3(low.x, v.pos.y, low.z), 0.3f, &sec) < lenXZ(low - v.pos) - 0.01f || !sec) { continue; }
			// (under the ceiling there, some of it, and in the frame)
			if (bottom < fixed16ToFloat(sec->ceilingHeight) + 2.0f) { continue; }
			f32 x, y, z;
			project(v, low, &x, &y, &z);
			if (z < 1.0f || fabsf(x) > 1.0f || y > 1.0f || y < -0.6f) { continue; }
			e.obj.posWS = { floatToFixed16(at.x), floatToFixed16(at.y), floatToFixed16(at.z) };
			e.sector = sec;
			s_extraCount++;
			k++;
			RIGDBG("firework %s at %.1f %.1f %.1f sector %d\n", bolt ? "bolt" : "blast", at.x, at.y, at.z, sec->id);
		}
	}

	// ---- the themes
	static void themeParty(const View& v, const s32* list, s32 n)
	{
		for (s32 k = 0; k < n; k++) { partyPose(v, s_people[list[k]]); }
	}

	// A class photo: a tidy line across the frame, everyone standing,
	// facing the lens
	static void themeClass(const View& v, const s32* list, s32 n, f32 groupDepth)
	{
		const s32 count = min(n, 7);
		const f32 depth = clampf(groupDepth, 8.0f, 14.0f);
		for (s32 k = 0; k < n; k++)
		{
			Person& p = s_people[list[k]];
			if (k < count)
			{
				const f32 fx = (count == 1) ? 0.0f : -0.62f + 1.24f * f32(k) / f32(count - 1);
				V3 feet;
				RSector* sec;
				if (floorSpot(v, fx, depth + (k & 1) * 0.6f, p.height, &feet, &sec)) { movePerson(p, feet, sec); }
			}
			standPose(v, p, (rand01() - 0.5f) * degToRad(4.0f));
		}
	}

	// A conga: everyone the same way, mid-stride, in a line across
	static void themeConga(const View& v, const s32* list, s32 n, f32 groupDepth)
	{
		const s32 count = min(n, 7);
		const f32 depth = clampf(groupDepth, 8.0f, 14.0f);
		const f32 dir = chance(2) ? 1.0f : -1.0f;
		const f32 yaw = v.yaw + c_pi + dir * degToRad(80.0f);	// (side on, all one way)
		for (s32 k = 0; k < n; k++)
		{
			Person& p = s_people[list[k]];
			if (k < count)
			{
				const f32 fx = (count == 1) ? 0.0f : dir * (-0.6f + 1.2f * f32(k) / f32(count - 1));
				V3 feet;
				RSector* sec;
				if (floorSpot(v, fx, depth + f32(k) * 0.5f, p.height, &feet, &sec)) { movePerson(p, feet, sec); }
			}
			s32 anim = -1, frame = 0;
			const s32 a = waxAnimFor(p.obj, ANIM_MOVE);
			if (a >= 0)
			{
				anim = a;
				const WaxAnim* w = WAX_AnimPtr(p.obj->wax, a);
				frame = (k * 2 + s_gpShot) % max(w->frameCount, 1);
			}
			pc_setPose(p.obj, yaw, anim, frame);
		}
	}

	// Someone posing on the floor in front of the others (lying on a frame
	// of their fall), the rest standing about or laughing
	static bool themeFloor(const View& v, const s32* list, s32 n, f32 groupDepth)
	{
		Person& p = s_people[list[0]];
		s32 anim = -1, frame = 0;
		if (!pickAction(p.obj, ANIM_DEAD, 0, &anim, &frame) && !pickAction(p.obj, ANIM_DIE1, -2, &anim, &frame) &&
			!pickAction(p.obj, ANIM_DIE2, -2, &anim, &frame)) { return false; }
		V3 feet;
		RSector* sec;
		bool placed = false;
		for (s32 t = 0; t < 6 && !placed; t++)
		{
			const f32 depth = maxf(groupDepth - 3.0f, c_gpNear + 1.0f) + f32(t) * 0.8f;
			if (!floorSpot(v, (rand01() - 0.5f) * 0.5f, depth, 1.0f, &feet, &sec)) { continue; }
			// (all of them in the photo, clear of the bottom edge)
			f32 x, y, z;
			project(v, feet, &x, &y, &z);
			placed = z > 0.15f && y >= -c_gpFitDown + 0.05f;
		}
		if (!placed) { return false; }
		movePerson(p, feet, sec);
		pc_setPose(p.obj, headingTo(feet, v.pos) + sideways(), anim, frame);
		for (s32 k = 1; k < n; k++)
		{
			if (chance(2)) { partyPose(v, s_people[list[k]]); }
			else { standPose(v, s_people[list[k]], (rand01() - 0.5f) * degToRad(20.0f)); }
		}
		return true;
	}

	// A photobomb: one of them right up at the edge of the frame peeking in,
	// or leaning in behind the others
	static bool themePhotobomb(const View& v, const s32* list, s32 n, f32 groupDepth)
	{
		Person& p = s_people[list[0]];
		const bool edge = n < 2 || chance(3) != 0;
		V3 feet;
		RSector* sec = nullptr;
		bool placed = false;
		const f32 side0 = chance(2) ? -1.0f : 1.0f;
		for (s32 t = 0; t < 8 && !placed; t++)
		{
			const f32 side = (t & 1) ? -side0 : side0;
			if (edge) { placed = floorSpot(v, side * (0.90f + rand01() * 0.10f), 3.6f + rand01() * 1.2f + (t >> 1) * 1.0f, p.height, &feet, &sec); }
			else { placed = floorSpot(v, side * rand01() * 0.4f, groupDepth + 2.5f + rand01() * 1.5f, p.height, &feet, &sec); }
			if (placed)
			{
				// (their face in the photo, not just a shoulder)
				f32 x, y, z;
				project(v, feet + up(p.height * 0.85f), &x, &y, &z);
				placed = z > 0.15f && y <= c_gpFitUp && y >= -c_gpFitDown;
			}
		}
		if (!placed) { return false; }
		pc_unhide(p.obj);
		movePerson(p, feet, sec);
		s_gpBomber = p.obj;
		s32 anim = -1, frame = 0;
		const s32 r = randInt(3);
		f32 yaw = headingTo(feet, v.pos);
		if (r == 0) { pickAction(p.obj, ANIM_PAIN, -1, &anim, &frame); yaw += (rand01() - 0.5f) * degToRad(20.0f); }
		else if (r == 1) { pickAction(p.obj, ANIM_ATTACK1, -1, &anim, &frame); yaw += sideways(); }
		else { pickPose(p.obj, POSE_IDLE, &anim, &frame); yaw += (rand01() - 0.5f) * degToRad(40.0f); }
		pc_setPose(p.obj, yaw, anim, frame);
		for (s32 k = 1; k < n; k++)
		{
			if (chance(3)) { partyPose(v, s_people[list[k]]); }
			else { standPose(v, s_people[list[k]], (rand01() - 0.5f) * degToRad(14.0f)); }
		}
		RIGDBG("photobomb %s at %.1f %.1f %.1f\n", edge ? "edge" : "behind", feet.x, feet.y, feet.z);
		return true;
	}

	// Shots and blasts: none near the lens or in front of a face; with
	// fireworks, those over their heads stay
	static void groupHideShots(const View& v, f32 groupDepth)
	{
		f32 topY = 1e9f;
		for (s32 i = 0; i < s_peopleCount; i++) { if (s_people[i].inFrame) { topY = minf(topY, s_people[i].head.y); } }
		forEachObject([&](SecObject* obj)
		{
			const s32 kind = objKind(obj);
			if (kind != KIND_SHOT && kind != KIND_EFFECT) { return; }
			const V3 pos = fromFixed(obj->posWS) + up(fixed16ToFloat(max(obj->worldHeight, 0)) * 0.5f);
			const V3 dp = pos - v.pos;
			const f32 fdp = len(dp);
			if (fdp < c_gpNear + 1.0f) { pc_hide(obj); return; }
			if (s_gpTheme == THEME_FIREWORKS && pos.y < topY - 0.5f && dot(dp, v.fwd) > groupDepth - 1.0f) { return; }
			// (none between the lens and the group, nor among them)
			if (dot(dp, v.fwd) < groupDepth + 1.5f) { pc_hide(obj); return; }
			for (s32 i = 0; i < s_peopleCount; i++)
			{
				const Person& p = s_people[i];
				if (!p.inFrame) { continue; }
				const V3 dh = p.head - v.pos, dm = p.mid - v.pos;
				const f32 fdh = len(dh);
				if (fdh < 0.01f || fdp >= fdh + 1.0f) { continue; }
				if (angleBetween(dp, dh) < p.headR * 2.0f / fdh + 0.03f || angleBetween(dp, dm) < p.bodyR / fdh) { pc_hide(obj); return; }
			}
		});
	}

	// One photo: a camera, a theme, everyone posed
	static bool groupPhoto()
	{
		s_poseCount = 0;
		s_hiddenCount = 0;
		s_extraCount = 0;
		s_gpBomber = nullptr;
		for (s32 i = 0; i < s_peopleCount; i++)
		{
			Person& p = s_people[i];
			p.pos = p.homePos; p.head = p.homeHead; p.mid = p.homeMid;
		}
		s_gpStyle = pickWeighted(c_styleWeights, CAM_STYLES, s_gpStyle);
		if (!placeGroupCamera()) { return false; }
		const View v = viewAt(s_camPos, s_camYaw, s_camPitch);
		// nobody nearer the lens than the group (the dying and the dead too)
		forEachObject([&](SecObject* obj)
		{
			if (obj == s_playerObject || !(obj->flags & OBJ_FLAG_NEEDS_TRANSFORM)) { return; }
			if (!(obj->entityFlags & (ETFLAG_AI_ACTOR | ETFLAG_CORPSE))) { return; }
			const Measure m = measure(obj);
			if (len(m.mid - v.pos) < c_gpNear) { pc_hide(obj); }
		});
		static s32 list[c_gpMaxPersons];
		f32 groupDepth = 10.0f;
		s32 n = inFramePeople(v, list, c_gpMaxPersons, &groupDepth);
		// (and anyone in front of the group with their head out of the frame:
		// only a big piece of them would be in it)
		for (s32 i = 0; i < s_peopleCount; i++)
		{
			const Person& p = s_people[i];
			if (p.inFrame) { continue; }
			f32 x, y, z;
			project(v, p.mid, &x, &y, &z);
			if (z > 0.15f && z < groupDepth + 1.0f && fabsf(x) < 1.6f) { pc_hide(p.obj); }
		}
		// (anyone well in front of the others joins them, or stays out of it)
		{
			s32 kept = 0;
			for (s32 k = 0; k < n; k++)
			{
				Person& p = s_people[list[k]];
				f32 x, y, z;
				project(v, p.mid, &x, &y, &z);
				if (n > 1 && z < groupDepth - 3.0f)
				{
					V3 feet;
					RSector* sec;
					if (floorSpot(v, clampf(x, -0.7f, 0.7f), groupDepth - 1.0f + rand01() * 2.0f, p.height, &feet, &sec)) { movePerson(p, feet, sec); }
					else { pc_hide(p.obj); p.inFrame = false; continue; }
				}
				list[kept++] = list[k];
			}
			n = kept;
		}
		// (and no bodies or things lying about in front of them)
		forEachObject([&](SecObject* obj)
		{
			if (obj == s_playerObject || !(obj->flags & OBJ_FLAG_NEEDS_TRANSFORM)) { return; }
			if (!(obj->entityFlags & (ETFLAG_CORPSE | ETFLAG_PICKUP)) || (obj->entityFlags & ETFLAG_AI_ACTOR)) { return; }
			if (dot(fromFixed(obj->posWS) - v.pos, v.fwd) < groupDepth - 1.0f) { pc_hide(obj); }
		});
		// a theme they are enough for, not one of the last two
		s32 theme = THEME_PARTY;
		for (s32 t = 0; t < 12; t++)
		{
			theme = pickWeighted(c_themeWeights, THEMES, s_gpTheme);
			if (theme != s_gpThemeBefore && n >= c_themeLeast[theme]) { break; }
			theme = THEME_PARTY;
		}
	#ifdef IOS_CAMERA_RIG_DEBUG
		if (getenv("CAMTEST_THEME")) { theme = atoi(getenv("CAMTEST_THEME")); }
	#endif
		s_gpThemeBefore = s_gpTheme;
		s_gpTheme = theme;
		f32 topY = 1e9f;
		for (s32 k = 0; k < n; k++) { topY = minf(topY, s_people[list[k]].head.y); }
		switch (theme)
		{
			case THEME_CLASS: themeClass(v, list, n, groupDepth); break;
			case THEME_CONGA: themeConga(v, list, n, groupDepth); break;
			case THEME_FLOOR: if (!themeFloor(v, list, n, groupDepth)) { s_gpTheme = THEME_PARTY; themeParty(v, list, n); } break;
			case THEME_PHOTOBOMB: if (!themePhotobomb(v, list, n, groupDepth)) { s_gpTheme = THEME_PARTY; themeParty(v, list, n); } break;
			case THEME_FIREWORKS: themeParty(v, list, n); addFirework(v, groupDepth, topY < 1e8f ? topY : s_eye.y); break;
			default: themeParty(v, list, n); break;
		}
		groupHideShots(v, groupDepth);
	#ifdef IOS_CAMERA_RIG_DEBUG
		for (s32 k = 0; k < n; k++)
		{
			const Person& p = s_people[list[k]];
			f32 x, y, z;
			project(v, p.mid, &x, &y, &z);
			RIGDBG("  person %d at %.1f %.1f %.1f proj %.2f %.2f %.2f hidden %d\n", k, p.pos.x, p.pos.y, p.pos.z, x, y, z, pc_isHidden(p.obj) ? 1 : 0);
		}
	#endif
		s_gpShot++;
		RIGDBG("group photo %d theme %d style %d people %d posed %d extras %d\n", s_gpShot, s_gpTheme, s_gpStyle, n, s_poseCount, s_extraCount);
		return true;
	}

	// Who is about: enemies near the player, in their sight, the nearest first
	static void findGroup()
	{
		struct Found { SecObject* obj; f32 dist; };
		static Found found[512];
		s32 foundCount = 0;
		const V3 pl = fromFixed(s_playerObject->posWS);
		forEachObject([&](SecObject* obj)
		{
			if (foundCount >= 512 || objKind(obj) != KIND_ENEMY) { return; }
			const f32 d = len(fromFixed(obj->posWS) - pl);
			if (d > c_gpRange) { return; }
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
		s_peopleCount = 0;
		for (s32 i = 0; i < foundCount && s_peopleCount < s_maxPeople && s_peopleCount < c_gpMaxPersons; i++)
		{
			const Measure m = measure(found[i].obj);
			// (in the player's sight)
			if (!clearLine(s_eye, s_eyeSector, m.head, m.headR) && !clearLine(s_eye, s_eyeSector, m.mid, m.bodyR)) { continue; }
			Person& p = s_people[s_peopleCount++];
			p.obj = found[i].obj;
			p.dist = found[i].dist;
			p.pos = fromFixed(found[i].obj->posWS);
			p.head = m.head; p.mid = m.mid; p.height = m.height; p.headR = m.headR; p.bodyR = m.bodyR; p.width = m.width;
			p.homePos = p.pos; p.homeHead = p.head; p.homeMid = p.mid;
			p.inFrame = false;
		}
	}

	static bool groupPrepare()
	{
		randSeed(u32(SDL_GetTicks()) * 2654435761u + 12345u);
		s_flash = true;
		s_hidePlayer = true;
		s_gpTheme = s_gpThemeBefore = s_gpStyle = -1;
		s_gpShot = 0;
		s_gpUsedCount = 0;
		findSolids(c_gpRange);
		findGroup();
		if (s_peopleCount == 0 || !groupPhoto())
		{
			s_sfNoOne = s_peopleCount == 0;
			s_sfFailed = true;
			return false;
		}
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
			// (no crosshair over the photos)
			if (reticle_enabled())
			{
				reticle_enable(false);
				s_reticleWasOn = true;
			}
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
	// ---------------------------------------------------------- group photos
	int IOSSelfie_Begin(void)
	{
		if (s_sfActive || s_pcActive) { return 0; }
		s_sfPosed = s_sfFailed = s_sfNoOne = false;
		s_peopleCount = 0;
		if (!pc_begin(groupPrepare)) { return 0; }
		// (with nobody about, no photo at all: the overlay says so)
		bool anyone = false;
		const V3 pl = fromFixed(s_playerObject->posWS);
		forEachObject([&](SecObject* obj)
		{
			if (anyone || objKind(obj) != KIND_ENEMY || len(fromFixed(obj->posWS) - pl) > c_gpRange) { return; }
			const Measure m = measure(obj);
			anyone = clearLine(s_eye, s_eyeSector, m.head, m.headR) || clearLine(s_eye, s_eyeSector, m.mid, m.bodyR);
		});
		if (!anyone)
		{
			pc_end();
			s_sfNoOne = true;
			return 0;
		}
		s_sfActive = true;
		return 1;
	}

	int IOSSelfie_IsActive(void) { return s_sfActive ? 1 : 0; }
	int IOSSelfie_IsPosed(void) { return (s_sfActive && s_sfPosed) ? 1 : 0; }
	int IOSSelfie_Failed(void) { return (s_sfActive && (s_sfFailed || s_pcFailed)) ? 1 : 0; }

	const char* IOSSelfie_CantMessage(void)
	{
		return s_sfNoOne ? "No one around for a group photo" : "Can't take a group photo here";
	}

	void IOSSelfie_Repose(void)
	{
		if (!s_sfActive || !s_sfPosed) { return; }
		// (a new photo; where no camera will do, the last one's, a new theme)
		const V3 cam = s_camPos;
		const f32 yaw = s_camYaw, pitch = s_camPitch;
		RSector* sec = s_camSector;
		if (!groupPhoto())
		{
			pc_place(cam, yaw, pitch, sec);
			s_poseCount = 0;
			s_extraCount = 0;
			const View v = viewAt(cam, yaw, pitch);
			static s32 list[c_gpMaxPersons];
			f32 depth;
			const s32 n = inFramePeople(v, list, c_gpMaxPersons, &depth);
			themeParty(v, list, n);
		}
	}

	void IOSSelfie_End(void)
	{
		if (!s_sfActive) { return; }
		s_peopleCount = 0;
		s_extraCount = 0;
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
		for (s32 i = 0; i < s_peopleCount; i++) { ct += s_people[i].inFrame ? 1 : 0; }
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
