#ifdef TFE_IOS
//////////////////////////////////////////////////////////////////////
// iOS: AUTOPLAY (MENU's tray), a self-playing show of Dark Forces for
// screen recordings: Kyle fights on autopilot, cutting from one of a
// level's biggest fights to the next.
//
// START saves the game as it is ("Before AUTOPLAY"), then loads a level
// (the one picked, or one of them shuffled) at AUTOPLAY's difficulty,
// straight in: no briefing, no cutscene, nothing of the agent's progress
// saved. As a level starts, its enemies are grouped by where they stand
// (close together, in sight of each other) and the biggest groups go
// first. Each scene: Kyle appears near the next group, somewhere safe
// (solid floor with room round him, no water, pit, door or lift, out of
// their sight where there's such a place, a straight walk from them)
// and facing them; the group gets copies of its own kinds around it
// (CROWD), everyone about is woken (ALARM), and he fights until the
// scene's time is up or the group is down. Once every area has had its
// scene: another level (SHUFFLE ALL), or the level's areas again in
// another order (PICK ONE). God mode and full ammo throughout. A tap
// anywhere ends it and loads the save back: the game as it was.
//
// The autopilot plays as a player would: its stick, buttons and weapon
// picks go into the game's input mapping as the touch controls' do, and
// its turning into the mouse motion (iosGame.cpp). It turns to the
// nearest enemy in sight, walks to a fighting distance for the weapon in
// hand and strafes from side to side, firing once on target (the game's
// auto-aim does the rest); it changes weapon now and then among those
// ticked, throws thermal detonators at bunches, keeps off ledges, pits
// and water, and when it isn't getting anywhere it strafes the other way,
// turns or jumps.
//
// Lengths are Dark Forces units (the eyes 5.8 above the feet), angles
// radians; world Y points down ("up" is -Y), as the game's.
//////////////////////////////////////////////////////////////////////
#include "iosGame.h"
#include "mission.h"
#include "player.h"
#include "agent.h"
#include "weapon.h"
#include "logic.h"
#include "Actor/actor.h"
#include "Actor/actorModule.h"
#include "Actor/animTables.h"
#include <TFE_Jedi/Level/rsector.h>
#include <TFE_Jedi/Level/rwall.h>
#include <TFE_Jedi/Level/robject.h>
#include <TFE_Jedi/Level/levelData.h>
#include <TFE_Jedi/Memory/allocator.h>
#include <TFE_Jedi/InfSystem/infTypesInternal.h>
#include <TFE_Input/inputMapping.h>
#include <TFE_Game/saveSystem.h>
#include <SDL.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <initializer_list>

#include <IOSTouch.h>
#include <IOSCheatGame.h>

// (the Linux test rig's log)
#ifdef IOS_AUTOPLAY_RIG_DEBUG
#define APDBG(...) printf("APDBG " __VA_ARGS__)
#else
#define APDBG(...)
#endif

using namespace TFE_Input;
using namespace TFE_Jedi;

namespace TFE_DarkForces
{
	extern void giveAllInventoryAndHealth();	// (player.cpp: LAMAXOUT's)
	extern void player_iosPlace(RSector* sector, fixed16_16 x, fixed16_16 z, angle14_32 yaw);	// (player.cpp)

	static const f32 c_pi = 3.14159265f;
	static const f32 c_toAngle14 = 16384.0f / (2.0f * 3.14159265f);
	static const f32 c_eyeHeight = 5.8f;
	static const char* c_saveFile = "autoplay.tfe";	// (in the game's saves: "Before AUTOPLAY")

	// The scenes
	static const f32 c_alarmRange = 70.0f;	// woken around a group, and counted for CROWD's cap
	static const f32 c_downRange = 55.0f;	// a group is down when no one is left this close to where it was
	static const s32 c_crowdCap = 36;		// CROWD adds copies only while fewer enemies than this are about
	static const s32 c_copiesPerLevel = 160;	// ...and no more than this many in one level
	static const f32 c_visitedRange = 40.0f;	// an area that has had its scene

	static inline f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
	static inline f32 minf(f32 a, f32 b) { return a < b ? a : b; }
	static inline f32 maxf(f32 a, f32 b) { return a > b ? a : b; }

	struct V3 { f32 x, y, z; };
	static inline V3 v3(f32 x, f32 y, f32 z) { V3 v = { x, y, z }; return v; }
	static inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
	static inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
	static inline V3 operator*(V3 a, f32 k) { return v3(a.x * k, a.y * k, a.z * k); }
	static inline f32 dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	static inline f32 len(V3 a) { return sqrtf(dot(a, a)); }
	static inline f32 lenXZ(V3 a) { return sqrtf(a.x * a.x + a.z * a.z); }
	static inline V3 up(f32 h) { return v3(0.0f, -h, 0.0f); }
	static inline V3 fromFixed(const vec3_fixed& p) { return v3(fixed16ToFloat(p.x), fixed16ToFloat(p.y), fixed16ToFloat(p.z)); }
	// Headings: the game's yaw as radians; 0 looks along +Z, and it grows
	// turning right (towards +X)
	static inline f32 headingTo(V3 from, V3 to) { return atan2f(to.x - from.x, to.z - from.z); }
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

	// AUTOPLAY's own random numbers (the game's stay as they were)
	static u32 s_rand = 1;
	static u32 randNext() { s_rand ^= s_rand << 13; s_rand ^= s_rand >> 17; s_rand ^= s_rand << 5; return s_rand; }
	static s32 randInt(s32 n) { return n > 0 ? s32(randNext() % u32(n)) : 0; }
	static f32 rand01() { return f32(randNext() & 0xffffff) / f32(0xffffff); }
	static f32 randRange(f32 lo, f32 hi) { return lo + (hi - lo) * rand01(); }

	//////////////////////////////////////////////////////////////////
	// The level: lines of sight, and walking the floor
	//////////////////////////////////////////////////////////////////
	static inline f32 floorOf(const RSector* s) { return fixed16ToFloat(s->floorHeight); }
	static inline f32 ceilOf(const RSector* s) { return fixed16ToFloat(s->ceilingHeight); }

	static bool inSectorXZ(RSector* s, f32 x, f32 z)
	{
		if (x < fixed16ToFloat(s->boundsMin.x) || x > fixed16ToFloat(s->boundsMax.x) ||
			z < fixed16ToFloat(s->boundsMin.z) || z > fixed16ToFloat(s->boundsMax.z)) { return false; }
		return sector_pointInsideDF(s, floatToFixed16(x), floatToFixed16(z)) != JFALSE;
	}

	static bool inSectorVol(RSector* s, V3 p)
	{
		if (!s || p.y > floorOf(s) || p.y < ceilOf(s)) { return false; }
		return inSectorXZ(s, p.x, p.z);
	}

	// (a sight line passes openings, not bars, fences or force fields)
	static inline bool seeThrough(const RWall* w)
	{
		return w->nextSector && !(w->flags1 & WF1_ADJ_MID_TEX) && w->nextSector->floorHeight > w->nextSector->ceilingHeight;
	}

	// The sector p is in, one or two openings from cur (null: in the way)
	static RSector* sightStep(RSector* cur, V3 p)
	{
		if (inSectorVol(cur, p)) { return cur; }
		for (s32 i = 0; i < cur->wallCount; i++)
		{
			const RWall* w = &cur->walls[i];
			if (seeThrough(w) && inSectorVol(w->nextSector, p)) { return w->nextSector; }
		}
		for (s32 i = 0; i < cur->wallCount; i++)
		{
			const RWall* w = &cur->walls[i];
			if (!seeThrough(w)) { continue; }
			RSector* n = w->nextSector;
			for (s32 k = 0; k < n->wallCount; k++)
			{
				const RWall* w2 = &n->walls[k];
				if (seeThrough(w2) && w2->nextSector != cur && inSectorVol(w2->nextSector, p)) { return w2->nextSector; }
			}
		}
		return nullptr;
	}

	// Whether `to` can be seen from `from` (in fromSec)
	static bool sees(V3 from, RSector* fromSec, V3 to)
	{
		if (!fromSec) { return false; }
		const V3 d = to - from;
		const f32 l = len(d);
		const s32 n = s32(ceilf(l / 0.5f));
		RSector* s = fromSec;
		for (s32 i = 1; i <= n; i++)
		{
			s = sightStep(s, from + d * (f32(i) / f32(n)));
			if (!s) { return false; }
		}
		return true;
	}

	// The sector whose floor is nearest under height y at (x, z) (the
	// game's own search logs points that lie on a wall)
	static RSector* sectorUnder(f32 x, f32 y, f32 z)
	{
		RSector* best = nullptr;
		f32 bestGap = 1e9f;
		RSector* s = s_levelState.sectors;
		for (u32 i = 0; i < s_levelState.sectorCount; i++, s++)
		{
			const f32 gap = floorOf(s) - y;	// (how far the floor is below y)
			if (gap < 0.0f || gap >= bestGap || ceilOf(s) > y || !inSectorXZ(s, x, z)) { continue; }
			best = s;
			bestGap = gap;
		}
		return best;
	}

	static RSector* sectorAt(V3 p, RSector* hint)
	{
		if (hint && inSectorVol(hint, p)) { return hint; }
		RSector* s = sectorUnder(p.x, p.y, p.z);
		return (s && inSectorVol(s, p)) ? s : nullptr;
	}

	static inline bool isWater(const RSector* s)
	{
		return s->secHeight > 0;	// (the second height, below the floor: the depth of the water, as the player's)
	}

	// A sector whose floor, ceiling or walls the level moves (lifts, doors,
	// crushers; strict: conveyors and every trigger too)
	static bool moves(const RSector* s, bool strict)
	{
		if (!s->infLink) { return false; }
		Allocator* links = s->infLink;
		for (InfLink* link = (InfLink*)allocator_getHead(links); link; link = (InfLink*)allocator_getNext(links))
		{
			if (strict) { return true; }
			if (link->type != LTYPE_SECTOR || !link->elev) { continue; }
			switch (link->elev->type)
			{
				case IELEV_MOVE_CEILING: case IELEV_MOVE_FLOOR: case IELEV_MOVE_OFFSET: case IELEV_MOVE_FC:
				case IELEV_MOVE_WALL: case IELEV_ROTATE_WALL:
					return true;
				default:
					break;
			}
		}
		return false;
	}

	// Somewhere to walk (strict: somewhere to stand at a cut, too: nothing
	// there the level moves or triggers)
	static bool safeSector(const RSector* s, bool strict)
	{
		const u32 bad = SEC_FLAGS1_DOOR | SEC_FLAGS1_PIT | SEC_FLAGS1_CRUSHING | SEC_FLAGS1_LOW_DAMAGE | SEC_FLAGS1_HIGH_DAMAGE;
		if ((s->flags1 & bad) || isWater(s)) { return false; }
		if (floorOf(s) - ceilOf(s) < 7.0f) { return false; }
		return !moves(s, strict);
	}

	// Whether the player could walk from sector a into b through wall w: a
	// step up of at most maxStep (the player's is 3.5; a jump makes more),
	// down of at most maxDrop, head room, no solid wall
	static bool walkable(const RSector* a, const RWall* w, const RSector* b, f32 maxDrop, f32 maxStep)
	{
		if (w->flags3 & WF3_SOLID_WALL) { return false; }
		const f32 fa = floorOf(a), fb = floorOf(b);
		if (fa - fb > maxStep || fb - fa > maxDrop) { return false; }
		const f32 opening = minf(fa, fb) - maxf(ceilOf(a), ceilOf(b));
		return opening >= 7.0f && safeSector(b, false);
	}

	// Walks the floor straight from `from` (feet, in sec) to (tx, tz), as
	// the player could; where it ends. False: something's in the way (a
	// wall, a ledge, a pit, water, a door).
	static bool walkFloor(V3 from, RSector* sec, f32 tx, f32 tz, f32 maxDrop, RSector** endSec, f32 maxStep = 3.5f)
	{
		if (!sec) { return false; }
		const f32 dx = tx - from.x, dz = tz - from.z;
		const f32 l = sqrtf(dx * dx + dz * dz);
		const s32 n = max(1, s32(ceilf(l / 0.5f)));
		RSector* s = sec;
		for (s32 i = 1; i <= n; i++)
		{
			const f32 x = from.x + dx * f32(i) / f32(n), z = from.z + dz * f32(i) / f32(n);
			if (inSectorXZ(s, x, z)) { continue; }
			RSector* next = nullptr;
			for (s32 k = 0; k < s->wallCount && !next; k++)
			{
				const RWall* w = &s->walls[k];
				if (!w->nextSector || !inSectorXZ(w->nextSector, x, z)) { continue; }
				if (!walkable(s, w, w->nextSector, maxDrop, maxStep)) { return false; }
				next = w->nextSector;
			}
			// (a corner of a small sector cut across)
			for (s32 k = 0; k < s->wallCount && !next; k++)
			{
				const RWall* w = &s->walls[k];
				if (!w->nextSector || !walkable(s, w, w->nextSector, maxDrop, maxStep)) { continue; }
				RSector* m = w->nextSector;
				for (s32 j = 0; j < m->wallCount && !next; j++)
				{
					const RWall* w2 = &m->walls[j];
					if (w2->nextSector && w2->nextSector != s && inSectorXZ(w2->nextSector, x, z) && walkable(m, w2, w2->nextSector, maxDrop, maxStep))
					{
						next = w2->nextSector;
					}
				}
			}
			if (!next) { return false; }
			s = next;
		}
		if (endSec) { *endSec = s; }
		return true;
	}

	//////////////////////////////////////////////////////////////////
	// The enemies
	//////////////////////////////////////////////////////////////////
	// What kind each enemy of the level is (its logic's KEYWORD), as it was
	// set up: copies are made of the same kind
	static std::unordered_map<SecObject*, s32> s_kinds;

	void iosAutoplay_noteEnemy(SecObject* obj, s32 logicId)
	{
		if (obj) { s_kinds[obj] = logicId; }
	}

	// Barrels, mines, scenery, the mouse droids and welders aren't fought
	static bool isFoeKind(s32 kind)
	{
		switch (kind)
		{
			case KW_BARREL: case KW_LAND_MINE: case KW_SCENERY: case KW_MOUSEBOT: case KW_WELDER:
				return false;
		}
		return true;
	}

	// The kind a copy of this one is (-1: none: the bosses, turrets; an
	// officer's copy carries no key)
	static s32 copyKind(s32 kind)
	{
		switch (kind)
		{
			case KW_TROOP: case KW_STORM1: case KW_COMMANDO: case KW_INT_DROID: case KW_PROBE_DROID:
			case KW_G_GUARD: case KW_REE_YEES: case KW_REE_YEES2: case KW_BOSSK: case KW_SEWER1: case KW_REMOTE:
				return kind;
			case KW_I_OFFICER: case KW_I_OFFICER1: case KW_I_OFFICER2: case KW_I_OFFICER3: case KW_I_OFFICER4:
			case KW_I_OFFICER5: case KW_I_OFFICER6: case KW_I_OFFICER7: case KW_I_OFFICER8: case KW_I_OFFICER9:
			case KW_I_OFFICERR: case KW_I_OFFICERY: case KW_I_OFFICERB:
				return KW_I_OFFICER;
		}
		return -1;
	}

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

	static bool isDying(SecObject* obj)
	{
		if (obj->type != OBJ_TYPE_SPRITE || !obj->wax) { return false; }
		ActorDispatch* dispatch = dispatchOf(obj);
		for (s32 action : { s32(ANIM_DIE1), s32(ANIM_DIE2), s32(ANIM_DEAD) })
		{
			s32 anim = action;
			if (dispatch && dispatch->animTable) { anim = dispatch->animTable[action]; }
			if (anim >= 0 && anim < obj->wax->animCount && anim == obj->anim) { return true; }
		}
		return false;
	}

	struct Foe
	{
		SecObject* obj;
		V3 pos, mid, head;
		s32 kind;	// (-1: not known)
	};
	static std::vector<Foe> s_foes;

	// An enemy still fighting (its kind: kindOut)
	static bool livingFoe(SecObject* obj, s32* kindOut)
	{
		if (!obj || obj == s_playerObject || !obj->sector || !(obj->entityFlags & ETFLAG_AI_ACTOR)) { return false; }
		if (!(obj->flags & OBJ_FLAG_NEEDS_TRANSFORM)) { return false; }	// (not drawn)
		const auto it = s_kinds.find(obj);
		const s32 kind = (it != s_kinds.end()) ? it->second : -1;
		if (kind >= 0 && !isFoeKind(kind)) { return false; }
		if (kind < 0 && obj->type != OBJ_TYPE_SPRITE) { return false; }	// (not known: only people and droids)
		ActorDispatch* dispatch = dispatchOf(obj);
		if (dispatch)
		{
			DamageModule* damage = damageOf(dispatch);
			if (damage && damage->hp <= 0) { return false; }
		}
		if (isDying(obj)) { return false; }
		*kindOut = kind;
		return true;
	}

	static void collectFoes()
	{
		s_foes.clear();
		RSector* s = s_levelState.sectors;
		for (u32 i = 0; i < s_levelState.sectorCount; i++, s++)
		{
			if (!s->objectList) { continue; }
			for (s32 k = 0; k < s->objectCapacity; k++)
			{
				SecObject* obj = s->objectList[k];
				s32 kind = -1;
				if (!obj || !livingFoe(obj, &kind)) { continue; }
				Foe f;
				f.obj = obj;
				f.kind = kind;
				f.pos = fromFixed(obj->posWS);
				const f32 h = obj->worldHeight > 0 ? maxf(fixed16ToFloat(obj->worldHeight), 2.0f) : 6.0f;
				f.mid = f.pos + up(h * 0.5f);
				f.head = f.pos + up(h * 0.85f);
				s_foes.push_back(f);
			}
		}
	}

	//////////////////////////////////////////////////////////////////
	// Groups: the enemies by where they stand
	//////////////////////////////////////////////////////////////////
	struct Group
	{
		V3 center;		// (on the floor of its middle one)
		RSector* sector;	// its middle one's
		V3 anchor;		// its middle one's feet
		s32 count;
	};

	static s32 findRoot(std::vector<s32>& parent, s32 i)
	{
		while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
		return i;
	}

	// Enemies close together and in sight of each other make a group;
	// biggest first
	static void makeGroups(std::vector<Group>& groups)
	{
		groups.clear();
		const s32 n = s32(s_foes.size());
		std::vector<s32> parent(n);
		for (s32 i = 0; i < n; i++) { parent[i] = i; }
		for (s32 i = 0; i < n; i++)
		{
			for (s32 j = i + 1; j < n; j++)
			{
				const Foe& a = s_foes[i];
				const Foe& b = s_foes[j];
				if (lenXZ(a.pos - b.pos) > 34.0f || fabsf(a.pos.y - b.pos.y) > 14.0f) { continue; }
				if (findRoot(parent, i) == findRoot(parent, j)) { continue; }
				if (!sees(a.mid, a.obj->sector, b.mid)) { continue; }
				parent[findRoot(parent, i)] = findRoot(parent, j);
			}
		}
		std::vector<s32> rootGroup(n, -1);
		std::vector<V3> sums;
		for (s32 i = 0; i < n; i++)
		{
			const s32 r = findRoot(parent, i);
			if (rootGroup[r] < 0)
			{
				rootGroup[r] = s32(groups.size());
				Group g;
				g.count = 0;
				g.sector = nullptr;
				g.center = g.anchor = v3(0, 0, 0);
				groups.push_back(g);
				sums.push_back(v3(0, 0, 0));
			}
			const s32 gi = rootGroup[r];
			groups[gi].count++;
			sums[gi] = sums[gi] + s_foes[i].pos;
		}
		for (size_t g = 0; g < groups.size(); g++) { groups[g].center = sums[g] * (1.0f / f32(groups[g].count)); }
		// the middle one of each (the nearest the middle), standing on a floor
		std::vector<f32> best(groups.size(), 1e9f);
		for (s32 i = 0; i < n; i++)
		{
			const s32 gi = rootGroup[findRoot(parent, i)];
			const f32 d = lenXZ(s_foes[i].pos - groups[gi].center);
			if (d < best[gi])
			{
				best[gi] = d;
				groups[gi].sector = s_foes[i].obj->sector;
				groups[gi].anchor = v3(s_foes[i].pos.x, floorOf(s_foes[i].obj->sector), s_foes[i].pos.z);
			}
		}
		for (Group& g : groups) { g.center.y = g.anchor.y; }
		std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.count > b.count; });
	}

	//////////////////////////////////////////////////////////////////
	// Where Kyle appears near a group
	//////////////////////////////////////////////////////////////////
	struct Spot { V3 feet; RSector* sector; f32 yaw; };

	// Room around: the floor walkable a little way every way, level
	static bool roomAround(V3 feet, RSector* sec)
	{
		for (s32 k = 0; k < 8; k++)
		{
			const f32 a = f32(k) * c_pi * 0.25f;
			RSector* end = nullptr;
			if (!walkFloor(feet, sec, feet.x + sinf(a) * 2.8f, feet.z + cosf(a) * 2.8f, 1.0f, &end)) { return false; }
			if (fabsf(floorOf(end) - feet.y) > 1.0f) { return false; }
		}
		return true;
	}

	// Somewhere near the group, a straight walk from it, standing safe, and
	// seen by as few of them as can be; facing them
	static bool findSpot(const Group& g, Spot* out)
	{
		f32 bestScore = 1e9f;
		s32 why[5] = { 0, 0, 0, 0, 0 };
		for (s32 pass = 0; pass < 2 && bestScore >= 1e8f; pass++)
		{
			const bool strict = pass == 0;
			for (f32 r : { 12.0f, 16.0f, 20.0f, 25.0f, 30.0f, 36.0f, 44.0f })
			{
				for (s32 k = 0; k < 24; k++)
				{
					const f32 a = (f32(k) + rand01() * 0.6f) * (2.0f * c_pi / 24.0f);
					const f32 x = g.center.x + sinf(a) * r, z = g.center.z + cosf(a) * r;
					// (the floor there nearest the group's height)
					RSector* sec = sectorUnder(x, g.center.y - 7.0f, z);
					if (!sec || !safeSector(sec, strict)) { why[0]++; continue; }
					const V3 feet = v3(x, floorOf(sec), z);
					if (fabsf(feet.y - g.center.y) > 8.0f) { why[1]++; continue; }
					if (!roomAround(feet, sec)) { why[2]++; continue; }
					// (a straight walk to them, if with a jump or a drop: the
					// same place, no wall between)
					if (!walkFloor(feet, sec, g.anchor.x, g.anchor.z, 10.0f, nullptr, 6.0f)) { why[3]++; continue; }
					const V3 eye = feet + up(c_eyeHeight);
					RSector* eyeSec = sectorAt(eye, sec);
					s32 seenBy = 0;
					f32 nearest = 1e9f;
					for (const Foe& f : s_foes)
					{
						const f32 d = lenXZ(f.pos - feet);
						if (d > 80.0f) { continue; }
						nearest = minf(nearest, d);
						if (eyeSec && sees(eye, eyeSec, f.head)) { seenBy++; }
					}
					if (nearest < 7.0f) { why[4]++; continue; }
					const f32 score = f32(seenBy) * 10.0f + fabsf(r - 24.0f) * 0.25f + rand01();
					if (score < bestScore)
					{
						bestScore = score;
						out->feet = feet;
						out->sector = sec;
						out->yaw = headingTo(feet, g.center);
					}
				}
			}
		}
		APDBG("spot for %d: %s (rejected: walk %d, height %d, room %d, back %d, near %d)\n", g.count, bestScore < 1e8f ? "found" : "none",
			why[0], why[1], why[2], why[3], why[4]);
		return bestScore < 1e8f;
	}

	//////////////////////////////////////////////////////////////////
	// The show
	//////////////////////////////////////////////////////////////////
	enum Stage { ST_OFF = 0, ST_TRAVEL, ST_ARRIVE, ST_SCENE };
	static bool s_on = false;
	static IOSAutoplayOptions s_opt;
	static Stage s_stage = ST_OFF;
	static f64 s_tStage = 0.0;
	static s32 s_requested = 0;		// the level asked for (1..), as the mission ends
	static u32 s_world = 0;			// (IOSCheatGame_World) the level being played
	static bool s_saved = false;		// the game was saved as it started
	static s32 s_agentLevel = 0, s_agentSelected = 0;
	static std::vector<s32> s_bag;		// SHUFFLE ALL's levels still to come
	static std::vector<V3> s_visited;	// the areas that have had their scene
	static s32 s_copies = 0;		// CROWD's copies in this level
	static f64 s_tPrev = 0.0;

	// The scene
	static V3 s_center;
	static f64 s_tScene = 0.0, s_tDown = 0.0, s_tCheck = 0.0;

	// The autopilot's controls, handed to the game each frame
	struct Controls
	{
		f32 forward, strafe;
		bool fire, alt, jump;
		s32 weapon;		// a weapon to take (WeaponID; -1: none)
		f32 lookX, lookY;	// mouse counts not handed over yet
	};
	static Controls s_ctl;
	static bool s_firePrev = false, s_altPrev = false;

	// The autopilot
	static SecObject* s_target = nullptr;
	static V3 s_aim;
	static f64 s_tScan = 0.0, s_tSeen = 0.0, s_tStrafe = 0.0, s_tBurst = 0.0, s_tPause = 0.0;
	static f32 s_strafeDir = 1.0f;
	static bool s_burstAlt = false;		// this burst on the other trigger
	static V3 s_lastPos;
	static f64 s_tMoved = 0.0, s_tEvade = 0.0;
	static f32 s_evadeYaw = 0.0f;
	static s32 s_stuck = 0;
	static std::vector<s32> s_seen;		// s_foes in sight
	// Weapons
	static std::vector<s32> s_rotation;	// the weapons it goes round
	static bool s_fists = false, s_dets = false;	// punches up close; throws detonators at bunches
	static s32 s_weapon = WPN_PISTOL;	// the one it wants
	static s32 s_before = WPN_PISTOL;	// ...and wanted before a punch or a throw
	static f64 s_tSwitch = 0.0, s_tAsked = 0.0, s_tDetsNext = 0.0, s_tThrow = 0.0, s_tRelease = 0.0;
	static s32 s_throws = 0;
	static V3 s_bunch;
	static bool s_bunchSeen = false;

	static void clearControls()
	{
		memset(&s_ctl, 0, sizeof(s_ctl));
		s_ctl.weapon = -1;
	}

	bool iosAutoplay_on() { return s_on; }
	bool iosAutoplay_hudShown() { return !s_on || s_opt.bHud; }

	u8 iosAutoplay_levelDifficulty(u8 difficulty)
	{
		s_kinds.clear();	// (a new level's enemies are noted as it loads)
		return s_on ? u8(s_opt.iDifficulty) : difficulty;
	}

	// SHUFFLE ALL: each level once, in a shuffled order, then again (never
	// the one just played twice in a row)
	static s32 pickLevel()
	{
		const s32 count = s_maxLevelIndex;
		if (count <= 0) { return 1; }
		const s32 cur = IOSCheatGame_CurrentLevel();
		if (s_bag.empty())
		{
			for (s32 i = 1; i <= count; i++) { if (i != cur || count == 1) { s_bag.push_back(i); } }
			for (s32 i = s32(s_bag.size()) - 1; i > 0; i--) { std::swap(s_bag[i], s_bag[randInt(i + 1)]); }
		}
		const s32 level = s_bag.back();
		s_bag.pop_back();
		return level;
	}

	static void goToLevel(s32 level)
	{
		APDBG("level %d\n", level);
		s_requested = level;
		s_stage = ST_TRAVEL;
		s_tStage = SDL_GetTicks() / 1000.0;
		clearControls();
		mission_setExitLevel(JTRUE);
	}

	static void nextLevel()
	{
		goToLevel(s_opt.iLevel > 0 ? s_opt.iLevel : pickLevel());
	}

	s32 iosAutoplay_nextLevel()
	{
		if (!s_on) { return 0; }
		// (asked for; or the level was finished by walking into its exit)
		const s32 level = (s_requested > 0) ? s_requested : (s_opt.iLevel > 0 ? s_opt.iLevel : pickLevel());
		s_requested = 0;
		s_stage = ST_TRAVEL;
		s_tStage = SDL_GetTicks() / 1000.0;
		return (level >= 1 && level <= s_maxLevelIndex) ? level : 1;
	}

	static void makeRotation()
	{
		const u32 ticked = s_opt.ulWeapons;
		s_rotation.clear();
		for (s32 id : { WPN_PISTOL, WPN_RIFLE, WPN_REPEATER, WPN_FUSION, WPN_MORTAR, WPN_CONCUSSION, WPN_CANNON })
		{
			if (ticked & (1u << (id + 1))) { s_rotation.push_back(id); }
		}
		const bool fists = (ticked & (1u << (WPN_FIST + 1))) != 0;
		const bool dets = (ticked & (1u << (WPN_THERMAL_DET + 1))) != 0;
		const bool mines = (ticked & (1u << (WPN_MINE + 1))) != 0;
		// (punches up close only when picked: with ALL it keeps its guns)
		s_fists = fists && !s_rotation.empty() && ticked != IOSAUTOPLAY_WEAPONS_ALL;
		s_dets = dets && !s_rotation.empty();
		if (s_rotation.empty())
		{
			// (only fists, detonators or mines ticked: it goes round those)
			if (dets) { s_rotation.push_back(WPN_THERMAL_DET); }
			if (mines) { s_rotation.push_back(WPN_MINE); }
			if (fists) { s_rotation.push_back(WPN_FIST); }
			if (s_rotation.empty()) { s_rotation.push_back(WPN_PISTOL); }
		}
	}

	void iosAutoplay_start(const IOSAutoplayOptions* options)
	{
		if (s_on || !options || !s_playerObject) { return; }
		s_opt = *options;
		s_opt.iDifficulty = max(0, min(2, s_opt.iDifficulty));
		s_opt.iCrowd = max(1, min(3, s_opt.iCrowd));
		s_opt.iSceneSeconds = max(5, min(600, s_opt.iSceneSeconds));
		s_opt.ulWeapons &= IOSAUTOPLAY_WEAPONS_ALL;
		if (!s_opt.ulWeapons) { s_opt.ulWeapons = IOSAUTOPLAY_WEAPONS_ALL; }
		if (s_opt.iLevel < 0 || s_opt.iLevel > s_maxLevelIndex) { s_opt.iLevel = 0; }
		makeRotation();
		s_rand = SDL_GetTicks() * 2654435761u + 1u;

		// the game as it is, to come back to
		s_saved = TFE_SaveSystem::saveGame(c_saveFile, "Before AUTOPLAY");
		s_agentLevel = agent_getLevelIndex();
		s_agentSelected = s_agentData[s_agentId].selectedMission;

		s_on = true;
		s_bag.clear();
		s_visited.clear();
		s_world = IOSCheatGame_World();
		s_tPrev = SDL_GetTicks() / 1000.0;
		IOSTouch_ShowMessage("AUTOPLAY: tap anywhere to stop", 2.5);
		nextLevel();
	}

	void iosAutoplay_stop(bool restore)
	{
		if (!s_on) { return; }
		s_on = false;
		s_stage = ST_OFF;
		s_requested = 0;
		s_target = nullptr;
		clearControls();
		setAxis(AXIS_LEFT_X, 0.0f);
		setAxis(AXIS_LEFT_Y, 0.0f);
		// (the cheat, if it's on, gives these back)
		if (s_invincibility == -2) { s_invincibility = 0; }
		s_flyMode = JFALSE;
		s_noclip = JFALSE;
		if (restore && s_saved)
		{
			agent_setNextLevelByIndex(s_agentLevel);
			s_agentData[s_agentId].selectedMission = s_agentSelected;
			TFE_SaveSystem::postLoadRequest(c_saveFile);
			IOSTouch_ShowMessage("AUTOPLAY off: back to your game", 2.5);
		}
		else if (restore)
		{
			IOSTouch_ShowMessage("AUTOPLAY off", 2.5);
		}
	}

	//////////////////////////////////////////////////////////////////
	// Scenes
	//////////////////////////////////////////////////////////////////
	static s32 foesAround(V3 at, f32 range)
	{
		s32 n = 0;
		for (const Foe& f : s_foes)
		{
			if (lenXZ(f.pos - at) <= range && fabsf(f.pos.y - at.y) <= 40.0f) { n++; }
		}
		return n;
	}

	// CROWD: copies of the group's own kinds around each of them, while the
	// area has fewer than c_crowdCap
	static void addCopies(V3 center, V3 kyle)
	{
		if (s_opt.iCrowd <= 1) { return; }
		s32 around = foesAround(center, c_alarmRange);
		std::vector<V3> taken;
		for (const Foe& f : s_foes) { taken.push_back(f.pos); }
		const size_t originals = s_foes.size();
		for (size_t i = 0; i < originals; i++)
		{
			const Foe f = s_foes[i];
			if (lenXZ(f.pos - center) > 30.0f || fabsf(f.pos.y - center.y) > 16.0f) { continue; }
			const s32 kind = copyKind(f.kind);
			if (kind < 0 || f.obj->type != OBJ_TYPE_SPRITE || !f.obj->wax) { continue; }
			RSector* fsec = f.obj->sector;
			const V3 feet = v3(f.pos.x, floorOf(fsec), f.pos.z);
			const f32 lift = f.pos.y - feet.y;	// (droids hover)
			for (s32 c = 1; c < s_opt.iCrowd; c++)
			{
				if (around >= c_crowdCap || s_copies >= c_copiesPerLevel) { return; }
				for (s32 tries = 0; tries < 14; tries++)
				{
					const f32 a = rand01() * 2.0f * c_pi, r = randRange(3.2f, 7.5f);
					const f32 x = feet.x + sinf(a) * r, z = feet.z + cosf(a) * r;
					RSector* sec = nullptr;
					if (!walkFloor(feet, fsec, x, z, 2.0f, &sec) || !safeSector(sec, false)) { continue; }
					const V3 p = v3(x, floorOf(sec), z);
					if (fabsf(p.y - feet.y) > 2.0f || floorOf(sec) - ceilOf(sec) < 9.0f || lenXZ(p - kyle) < 9.0f) { continue; }
					bool clear = true;
					for (const V3& t : taken) { if (lenXZ(t - p) < 3.0f && fabsf(t.y - p.y) < 8.0f) { clear = false; break; } }
					if (!clear) { continue; }

					SecObject* copy = allocateObject();
					if (!copy) { return; }
					copy->posWS.x = floatToFixed16(p.x);
					copy->posWS.y = floatToFixed16(p.y + lift);
					copy->posWS.z = floatToFixed16(p.z);
					copy->yaw = toAngle14(rand01() * 2.0f * c_pi);
					sector_addObject(sec, copy);
					sprite_setData(copy, f.obj->wax);
					obj_setEnemyLogic(copy, KEYWORD(kind));
					taken.push_back(p);
					around++;
					s_copies++;
					break;
				}
			}
		}
	}

	// ALARM: everyone around woken, and hunting
	static void soundAlarm(V3 center)
	{
		for (const Foe& f : s_foes)
		{
			if (lenXZ(f.pos - center) <= c_alarmRange && fabsf(f.pos.y - center.y) <= 40.0f)
			{
				actor_sendWakeupMsg(f.obj);
			}
		}
	}

	static bool visited(V3 c)
	{
		for (const V3& v : s_visited)
		{
			if (lenXZ(v - c) < c_visitedRange && fabsf(v.y - c.y) < 24.0f) { return true; }
		}
		return false;
	}

	static void pickWeapon(s32 notThis)
	{
		s32 id = s_rotation[randInt(s32(s_rotation.size()))];
		if (id == notThis && s_rotation.size() > 1)
		{
			while (id == notThis) { id = s_rotation[randInt(s32(s_rotation.size()))]; }
		}
		s_weapon = id;
	}

	// The next scene: the biggest group whose area hasn't had one, Kyle
	// near it, its crowd and its alarm
	static void startScene(f64 now)
	{
		collectFoes();
		if (s_foes.empty()) { nextLevel(); return; }
		std::vector<Group> groups;
		makeGroups(groups);
		APDBG("groups: %d foes, %d groups, biggest %d %d %d %d\n", (int)s_foes.size(), (int)groups.size(),
			groups.size() > 0 ? groups[0].count : 0, groups.size() > 1 ? groups[1].count : 0, groups.size() > 2 ? groups[2].count : 0, groups.size() > 3 ? groups[3].count : 0);
		std::vector<s32> order;
		for (s32 i = 0; i < s32(groups.size()); i++) { if (!visited(groups[i].center)) { order.push_back(i); } }
		if (order.empty())
		{
			if (s_opt.iLevel == 0) { nextLevel(); return; }
			// PICK ONE: the level's areas again, in another order
			s_visited.clear();
			for (s32 i = 0; i < s32(groups.size()); i++) { order.push_back(i); }
			for (s32 i = s32(order.size()) - 1; i > 0; i--) { std::swap(order[i], order[randInt(i + 1)]); }
		}
		for (s32 gi : order)
		{
			const Group& g = groups[gi];
			s_visited.push_back(g.center);
			Spot spot;
			if (!g.sector || !findSpot(g, &spot)) { continue; }

			APDBG("scene found in %.1f ms\n", f64(SDL_GetTicks()) - now * 1000.0);
			APDBG("scene: group of %d at %.1f %.1f %.1f, Kyle at %.1f %.1f %.1f\n", g.count, g.center.x, g.center.y, g.center.z,
				spot.feet.x, spot.feet.y, spot.feet.z);
			player_iosPlace(spot.sector, floatToFixed16(spot.feet.x), floatToFixed16(spot.feet.z), toAngle14(spot.yaw));
			addCopies(g.center, spot.feet);
			if (s_opt.bAlarm)
			{
				collectFoes();
				soundAlarm(g.center);
			}
			s_center = g.center;
			s_stage = ST_SCENE;
			s_tScene = now;
			s_tDown = 0.0;
			s_tCheck = now + 1.0;
			// the autopilot starts afresh
			clearControls();
			s_target = nullptr;
			s_seen.clear();
			s_tScan = 0.0;
			s_tSeen = now;
			s_lastPos = spot.feet;
			s_tMoved = now;
			s_tEvade = 0.0;
			s_stuck = 0;
			s_throws = 0;
			s_tBurst = s_tPause = now;
			s_tStrafe = now + randRange(0.8f, 1.8f);
			s_strafeDir = randInt(2) ? 1.0f : -1.0f;
			pickWeapon(-1);
			s_before = s_weapon;
			s_tSwitch = now + randRange(7.0f, 12.0f);
			s_tAsked = 0.0;
			s_tDetsNext = now + 3.0;
			return;
		}
		nextLevel();	// (nowhere to stand near any of them)
	}

	//////////////////////////////////////////////////////////////////
	// The autopilot
	//////////////////////////////////////////////////////////////////
	// How far from its target it fights with each weapon
	static f32 fightRange(s32 weapon)
	{
		switch (weapon)
		{
			case WPN_FIST:        return 3.0f;
			case WPN_PISTOL:      return 18.0f;
			case WPN_RIFLE:       return 22.0f;
			case WPN_THERMAL_DET: return 24.0f;
			case WPN_REPEATER:    return 20.0f;
			case WPN_FUSION:      return 18.0f;
			case WPN_MORTAR:      return 32.0f;
			case WPN_MINE:        return 10.0f;
			case WPN_CONCUSSION:  return 28.0f;
			case WPN_CANNON:      return 22.0f;
		}
		return 20.0f;
	}

	// The enemies in sight, the target (the nearest, kept while it stays
	// about as near), and a bunch for a detonator
	static void scan(V3 eye, RSector* eyeSec, f64 now)
	{
		collectFoes();
		s_seen.clear();
		s32 nearest = -1;
		f32 nearestD = 1e9f, targetD = 1e9f;
		for (s32 i = 0; i < s32(s_foes.size()); i++)
		{
			const Foe& f = s_foes[i];
			const f32 d = len(f.mid - eye);
			if (d > 100.0f) { continue; }
			if (!sees(eye, eyeSec, f.mid) && !sees(eye, eyeSec, f.head)) { continue; }
			s_seen.push_back(i);
			if (d < nearestD) { nearestD = d; nearest = i; }
			if (f.obj == s_target) { targetD = d; }
		}
		if (nearest >= 0)
		{
			// (kept while in sight, unless another is much nearer: no flitting
			// from one to the next)
			if (!s_target || targetD > 1e8f || targetD > nearestD * 1.8f + 4.0f) { s_target = s_foes[nearest].obj; targetD = nearestD; }
			for (s32 i : s_seen) { if (s_foes[i].obj == s_target) { s_aim = s_foes[i].mid; } }
			s_tSeen = now;
		}
		else if (now - s_tSeen > 1.5)
		{
			s_target = nullptr;
		}
		// a bunch: three or more in sight close together, not too near
		s_bunchSeen = false;
		s32 bestCount = 0;
		for (s32 i : s_seen)
		{
			const f32 d = lenXZ(s_foes[i].pos - eye);
			if (d < 14.0f || d > 48.0f) { continue; }
			s32 count = 0;
			V3 sum = v3(0, 0, 0);
			for (s32 j : s_seen)
			{
				if (lenXZ(s_foes[j].pos - s_foes[i].pos) < 9.0f) { count++; sum = sum + s_foes[j].mid; }
			}
			if (count >= 3 && count > bestCount)
			{
				bestCount = count;
				s_bunch = sum * (1.0f / f32(count));
				s_bunchSeen = true;
			}
		}
	}

	// Whether the player can go that way (dir, flat) for a little: no
	// wall, ledge, pit or water
	static bool canGo(V3 feet, RSector* sec, f32 dx, f32 dz)
	{
		RSector* end = nullptr;
		if (!walkFloor(feet, sec, feet.x + dx * 3.0f, feet.z + dz * 3.0f, 4.0f, &end)) { return false; }
		return safeSector(end, false);
	}

	// Turns the view towards a heading and a pitch (up: > 0), smoothly, as
	// a player's thumb would: mouse counts for the next frames
	static void turnTowards(f32 yawWanted, f32 pitchWanted, f32 dt, f32 maxRate)
	{
		const f32 yawErr = wrapAngle(yawWanted - fromAngle14(s_playerYaw));
		f32 step = clampf(yawErr * 10.0f, -maxRate, maxRate) * dt;
		if (fabsf(step) > fabsf(yawErr)) { step = yawErr; }
		const f32 sensH = inputMapping_getHorzMouseSensitivity();
		const f32 sensV = inputMapping_getVertMouseSensitivity();
		if (fabsf(sensH) > 1e-3f) { s_ctl.lookX = step * c_toAngle14 / (8.0f * sensH); }
		// (mouse counts down look down)
		const f32 pitchErr = clampf(pitchWanted, -0.9f, 0.9f) - fromAngle14(s_eyePitch);
		const f32 pstep = clampf(pitchErr * 5.0f, -2.0f, 2.0f) * dt;
		if (fabsf(sensV) > 1e-3f) { s_ctl.lookY = -pstep * c_toAngle14 / (8.0f * sensV); }
	}

	static void takeWeapon(s32 id)
	{
		s_weapon = id;
		s_tAsked = 0.0;
	}

	static void autopilot(f64 now, f32 dt)
	{
		const V3 feet = fromFixed(s_playerObject->posWS);
		RSector* sec = s_playerObject->sector;
		const V3 eye = fromFixed(s_eyePos);
		RSector* eyeSec = sectorAt(eye, sec);
		if (!eyeSec) { eyeSec = sec; }
		const f32 yaw = fromAngle14(s_playerYaw);
		const PlayerInfo& p = s_playerInfo;

		if (now >= s_tScan)
		{
			s_tScan = now + 0.15;
			scan(eye, eyeSec, now);
		}

		// the weapon: round the ticked ones now and then; a punch up close,
		// a detonator at a bunch
		const f32 targetD = s_target ? len(s_aim - eye) : 1e9f;
		if (s_throws == 0)
		{
			if (s_fists && s_target && targetD < 6.0f && s_weapon != WPN_FIST) { s_before = s_weapon; takeWeapon(WPN_FIST); }
			else if (s_weapon == WPN_FIST && s_fists && (!s_target || targetD > 10.0f)) { takeWeapon(s_before); }
			else if (s_dets && s_bunchSeen && now >= s_tDetsNext && s_weapon != WPN_FIST)
			{
				s_before = s_weapon;
				takeWeapon(WPN_THERMAL_DET);
				s_throws = 1 + randInt(2);
				s_tThrow = now + 0.5;
				s_tRelease = 0.0;
			}
			else if (now >= s_tSwitch && s_weapon != WPN_FIST)
			{
				pickWeapon(s_weapon);
				s_tSwitch = now + randRange(7.0f, 12.0f);
			}
		}
		if (p.curWeapon != s_weapon && p.newWeapon != s_weapon && now - s_tAsked > 0.8)
		{
			s_ctl.weapon = s_weapon;	// (asked again if it didn't take)
			s_tAsked = now;
		}
		const bool holding = p.curWeapon == s_weapon;

		// where it looks: the target, the bunch for a throw, the scene, or
		// away from what it's stuck on
		f32 yawWanted = yaw, pitchWanted = 0.0f, turnRate = 5.0f;
		f32 forward = 0.0f, strafe = 0.0f;
		const bool evading = now < s_tEvade;
		const bool throwing = holding && (s_weapon == WPN_THERMAL_DET) && (s_throws > 0 || s_target);
		V3 aim = (throwing && s_bunchSeen) ? s_bunch : s_aim;
		if (evading)
		{
			yawWanted = s_evadeYaw;
			forward = 1.0f;
		}
		else if (s_target)
		{
			yawWanted = headingTo(eye, aim);
			const f32 dxz = maxf(lenXZ(aim - eye), 0.1f);
			pitchWanted = atan2f(eye.y - aim.y, dxz) * 0.6f;
			const f32 range = fightRange(holding ? s_weapon : p.curWeapon);
			forward = clampf((dxz - range) / 6.0f, -1.0f, 1.0f);
			if (fabsf(wrapAngle(yawWanted - yaw)) > 1.0f) { forward *= 0.3f; }
			strafe = s_strafeDir * 0.85f;
			if (now >= s_tStrafe)
			{
				s_strafeDir = -s_strafeDir;
				s_tStrafe = now + randRange(0.9f, 2.2f);
			}
		}
		else
		{
			// nothing in sight: hunting the nearest one left about, else back
			// to where the group was, then a look round
			V3 goal = s_center;
			f32 nearest = c_downRange;
			for (const Foe& f : s_foes)
			{
				const f32 d = lenXZ(f.pos - feet);
				if (d < nearest && fabsf(f.pos.y - feet.y) < 12.0f && lenXZ(f.pos - s_center) < c_downRange) { nearest = d; goal = f.pos; }
			}
			const f32 d = lenXZ(goal - feet);
			if (d > 8.0f)
			{
				yawWanted = headingTo(feet, goal);
				forward = fabsf(wrapAngle(yawWanted - yaw)) < 0.6f ? 1.0f : 0.25f;
			}
			else
			{
				yawWanted = yaw + 1.0f;
				turnRate = 1.2f;
			}
		}

		// keep off ledges, pits and water, and out of walls (what it wanted
		// to do counts for being stuck)
		const f32 wanted = fabsf(forward) + fabsf(strafe);
		const f32 sy = sinf(yaw), cy = cosf(yaw);
		if (forward > 0.05f && !canGo(feet, sec, sy, cy)) { forward = 0.0f; }
		if (forward < -0.05f && !canGo(feet, sec, -sy, -cy)) { forward = 0.0f; }
		if (fabsf(strafe) > 0.05f)
		{
			// (right is +X at heading 0)
			const f32 rx = cy * (strafe > 0.0f ? 1.0f : -1.0f), rz = -sy * (strafe > 0.0f ? 1.0f : -1.0f);
			if (!canGo(feet, sec, rx, rz))
			{
				s_strafeDir = -s_strafeDir;
				strafe = -strafe;
				if (!canGo(feet, sec, -rx, -rz)) { strafe = 0.0f; }
				s_tStrafe = now + randRange(0.9f, 2.2f);
			}
		}

		// not getting anywhere: strafe the other way, then turn away and jump
		if (now - s_tMoved >= 0.5)
		{
			const f32 moved = lenXZ(feet - s_lastPos);
			const bool trying = wanted > 0.6f;
			if (trying && moved < 0.5f)
			{
				s_stuck++;
				s_strafeDir = -s_strafeDir;
				// (with someone in sight it keeps facing them, strafing)
				if (s_stuck >= 3 && s_target)
				{
					s_stuck = 0;
					s_ctl.jump = true;
				}
				else if (s_stuck >= 3)
				{
					s_stuck = 0;
					s_evadeYaw = yaw + (randInt(2) ? 1.0f : -1.0f) * randRange(1.6f, 2.8f);
					s_tEvade = now + 1.5;
					s_ctl.jump = true;
				}
			}
			else if (moved >= 0.5f)
			{
				s_stuck = 0;
			}
			s_lastPos = feet;
			s_tMoved = now;
		}

#ifdef IOS_AUTOPLAY_RIG_DEBUG
		static f64 s_tDbg = 0.0;
		if (now >= s_tDbg)
		{
			s_tDbg = now + 0.5;
			APDBG("ap t %.1f target %d seen %d err %.2f fwd %.2f str %.2f evade %d stuck %d wpn %d/%d dist %.1f\n", now - s_tScene, s_target ? 1 : 0, (int)s_seen.size(),
				wrapAngle(yawWanted - yaw), forward, strafe, evading ? 1 : 0, s_stuck, p.curWeapon, s_weapon, s_target ? lenXZ(aim - eye) : -1.0f);
		}
#endif
		turnTowards(yawWanted, pitchWanted, dt, turnRate);
		s_ctl.forward = forward;
		s_ctl.strafe = strafe;

		// fire: on target, in bursts; a detonator held to throw as far as the
		// bunch, mines dropped now and then
		bool fire = false, alt = false;
		if (s_target && holding && !evading)
		{
			const f32 dxz = maxf(lenXZ(aim - eye), 0.1f);
			const f32 err = fabsf(wrapAngle(headingTo(eye, aim) - yaw));
			const bool onTarget = err < maxf(0.06f, atan2f(1.8f, dxz)) * 1.5f;
			if (s_weapon == WPN_THERMAL_DET)
			{
				if (s_tRelease > 0.0)
				{
					fire = now < s_tRelease;
					if (!fire)
					{
						s_tRelease = 0.0;
						s_tThrow = now + 1.0;
						if (s_throws > 0 && --s_throws == 0)
						{
							takeWeapon(s_before);
							s_tDetsNext = now + randRange(6.0f, 10.0f);
						}
					}
				}
				else if (onTarget && now >= s_tThrow && dxz < 60.0f)
				{
					// (held 0.4 s to 0.8 s: the throw from short to far)
					s_tRelease = now + clampf(0.4f + (dxz - 12.0f) / 40.0f * 0.4f, 0.4f, 0.8f);
					fire = true;
				}
			}
			else if (s_weapon == WPN_MINE)
			{
				fire = onTarget && fmod(now, 2.5) < 0.3;
			}
			else if (s_weapon == WPN_FIST)
			{
				fire = dxz < 5.0f;
			}
			else if (onTarget && dxz < 90.0f)
			{
				if (now >= s_tPause)
				{
					// a new burst, now and then the other trigger (the
					// repeater's three, the fusion cutter's four, the
					// cannon's missiles)
					const bool hasAlt = s_weapon == WPN_REPEATER || s_weapon == WPN_FUSION || s_weapon == WPN_CANNON;
					s_tBurst = now + randRange(0.7f, 2.0f);
					s_tPause = s_tBurst + randRange(0.15f, 0.5f);
					s_burstAlt = hasAlt && randInt(10) < 3;
				}
				fire = now < s_tBurst;
				alt = fire && s_burstAlt;
				if (alt) { fire = false; }
			}
		}
		s_ctl.fire = fire;
		s_ctl.alt = alt;
	}

	// God mode, full ammo, everything given; no flying
	static void keepPlayer(bool give)
	{
		if (give) { giveAllInventoryAndHealth(); }
		s_invincibility = -2;
		PlayerInfo& p = s_playerInfo;
		p.ammoEnergy = s_ammoEnergyMax;
		p.ammoPower = s_ammoPowerMax;
		p.ammoPlasma = s_ammoPlasmaMax;
		p.ammoDetonator = s_ammoDetonatorMax;
		p.ammoShell = s_ammoShellMax;
		p.ammoMine = s_ammoMineMax;
		p.ammoMissile = s_ammoMissileMax;
		s_flyMode = JFALSE;
		s_noclip = JFALSE;
	}

	void iosAutoplay_gameTick()
	{
		if (!s_on || !s_playerObject || !s_playerObject->sector || s_missionMode != MISSION_MODE_MAIN) { return; }
		const f64 now = SDL_GetTicks() / 1000.0;
		const f32 dt = f32(clampf(f32(now - s_tPrev), 0.0f, 0.1f));
		s_tPrev = now;

		// a level started
		const u32 world = IOSCheatGame_World();
		const bool arrived = world != s_world;
		s_world = world;
		if (arrived)
		{
			s_stage = ST_ARRIVE;
			s_tStage = now;
			s_visited.clear();
			s_copies = 0;
			clearControls();
		}
		if (s_stage == ST_TRAVEL)
		{
			// (the level never came: AUTOPLAY gives up)
			if (now - s_tStage > 30.0) { iosAutoplay_stop(false); }
			return;
		}
		keepPlayer(arrived);
		if (s_playerDying) { clearControls(); return; }

		if (s_stage == ST_ARRIVE)
		{
			if (now - s_tStage >= 0.3) { startScene(now); }
			return;
		}
		if (s_stage != ST_SCENE) { return; }

		autopilot(now, dt);

		// the scene's end: its time up, or no one left where the group was
		// (for a moment), or Kyle long lost far from it
		if (now >= s_tCheck)
		{
			s_tCheck = now + 0.25;
			const V3 feet = fromFixed(s_playerObject->posWS);
			const s32 left = foesAround(s_center, c_downRange) + s32(s_seen.size());
			if (left == 0)
			{
				if (s_tDown <= 0.0) { s_tDown = now; }
			}
			else
			{
				s_tDown = 0.0;
			}
			APDBG("check t %.1f left %d seen %d target %p total %d\n", now - s_tScene, left, (int)s_seen.size(), (void*)s_target, (int)s_foes.size());
			const bool down = s_tDown > 0.0 && now - s_tDown >= 1.2;
			// (or fallen far below it: a shaft, a lift gone down)
			const bool lost = (!s_target && now - s_tSeen > 6.0 && lenXZ(feet - s_center) > 100.0f) || feet.y - s_center.y > 24.0f;
			if (down || lost || now - s_tScene >= f64(s_opt.iSceneSeconds))
			{
				APDBG("scene over: %s (Kyle %.1f %.1f %.1f, scene %.1f %.1f %.1f)\n", down ? "down" : (lost ? "lost" : "time"), feet.x, feet.y, feet.z, s_center.x, s_center.y, s_center.z);
				startScene(now);
			}
		}
	}

	void iosAutoplay_input()
	{
		if (!s_on) { return; }
		setAxis(AXIS_LEFT_X, s_ctl.strafe);
		setAxis(AXIS_LEFT_Y, s_ctl.forward);
		if (s_ctl.forward != 0.0f || s_ctl.strafe != 0.0f) { inputMapping_setStateDown(IADF_RUN); }
		if (s_ctl.fire)
		{
			if (s_firePrev) { inputMapping_setStateDown(IADF_PRIMARY_FIRE); }
			else { inputMapping_setStatePress(IADF_PRIMARY_FIRE); }
		}
		s_firePrev = s_ctl.fire;
		if (s_ctl.alt)
		{
			if (s_altPrev) { inputMapping_setStateDown(IADF_SECONDARY_FIRE); }
			else { inputMapping_setStatePress(IADF_SECONDARY_FIRE); }
		}
		s_altPrev = s_ctl.alt;
		if (s_ctl.jump)
		{
			inputMapping_setStatePress(IADF_JUMP);
			s_ctl.jump = false;
		}
		if (s_ctl.weapon >= 0 && s_ctl.weapon < WPN_COUNT)
		{
			inputMapping_setStatePress(InputAction(IADF_WEAPON_1 + s_ctl.weapon));
		}
		s_ctl.weapon = -1;
	}

	void iosAutoplay_takeLook(f32* dx, f32* dy)
	{
		*dx = *dy = 0.0f;
		if (!s_on) { return; }
		*dx = s_ctl.lookX;
		*dy = s_ctl.lookY;
		s_ctl.lookX = s_ctl.lookY = 0.0f;
	}
}
#endif
