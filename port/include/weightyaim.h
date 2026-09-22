#ifndef _IN_WEIGHTYAIM_H
#define _IN_WEIGHTYAIM_H

/*
 * Weighty Aim: weighted free-aim for Perfect Dark.
 *
 * Replaces Perfect Dark's "crosshair offset = turn speed" sway with a
 * free-aim model: look input moves the gun inside a deadzone first and only
 * turns the camera once the gun reaches the edge. The gun follows its target
 * through a spring (weapon inertia), so it stays where you put it instead of
 * rubber-banding back to the centre when you let go of the stick.
 *
 * Everything lives in port/src/weightyaim.c and port/src/weightyaimmenu.c. The
 * game code only calls into it from a couple of marked hooks, and with
 * the Classic preset the original code path runs untouched.
 */

#include <PR/ultratypes.h>
#include "types.h"

#define WEIGHTYAIM_PRESET_WEIGHTY 0 // free-aim with a crosshair (default)
#define WEIGHTYAIM_PRESET_IMMERSIVE 1 // heavier, more flowing gun and camera, crosshair only when aiming
#define WEIGHTYAIM_PRESET_CLASSIC 2 // the game's original crosshair sway (mod off)
#define WEIGHTYAIM_PRESET_CUSTOM  3 // any slider changed by hand
#define WEIGHTYAIM_NUM_PRESETS    4

#define WEIGHTYAIM_CROSSHAIR_ALWAYS   0
#define WEIGHTYAIM_CROSSHAIR_AIMONLY  1 // hidden while hip-firing, shown when holding aim

struct weightyaimcfg {
	s32 preset;          // WEIGHTYAIM_PRESET_*; CLASSIC turns the mod off
	f32 deadzonex;       // half-width of the free-aim zone, degrees
	f32 deadzoney;       // half-height of the free-aim zone, degrees
	f32 camerashare;     // share of every look movement that turns the camera directly (0..1);
	                     // the rest moves the gun, so gun + camera always add up to your input
	f32 cameralead;      // camera drifts toward the gun while aiming inside the zone (per second at the edge)
	f32 recenterspeed;   // how fast the camera catches up to the gun when idle (per second, 0 = never)
	f32 recenterdelay;   // seconds without look input before catch-up starts
	f32 gunresponse;     // weapon inertia spring frequency in Hz (lower = heavier gun)
	f32 gundamping;      // spring damping ratio (1 = no overshoot, lower = more overshoot)
	f32 turndrag;        // how much the gun lags behind when the camera turns (0..1)
	f32 edgesmoothing;   // seconds for the camera to ease in when the gun pushes past the edge (0 = rigid)
	f32 camerasway;      // idle breathing sway of the camera, degrees
	f32 walksway;        // extra camera sway while moving at full speed, degrees
	s32 crosshair;       // WEIGHTYAIM_CROSSHAIR_*
	s32 laser;           // RE4-style laser sight on every gun: full beam to a bigger, brighter dot
};

#define WEIGHTYAIM_CURVE_ORIGINAL 0 // the game's response: squared, maxes out early (ignores the deadzones below)
#define WEIGHTYAIM_CURVE_LINEAR   1
#define WEIGHTYAIM_CURVE_BALANCED 2 // between linear and the original
#define WEIGHTYAIM_CURVE_CUSTOM   3 // cubic bezier from (0,0) to (1,1), like DS4Windows
#define WEIGHTYAIM_NUM_CURVES     4

/*
 * Look-stick response. Kept separate from the aim presets so switching
 * presets never resets your stick tuning.
 */
struct weightyaimstickcfg {
	s32 curve;           // WEIGHTYAIM_CURVE_*
	f32 innerdeadzone;   // 0..1 of full deflection; below this the stick reads zero
	f32 outerdeadzone;   // 0..1; at or past this the stick reads full
	f32 bezier[4];       // custom curve control points: x1, y1, x2, y2 (0..1)
	f32 turnspeed;       // look speed at full deflection (1 = the game's max turn rate)
};

extern struct weightyaimcfg g_WeightyAimCfg[4];
extern struct weightyaimstickcfg g_WeightyAimStickCfg[4];
extern const char *g_WeightyAimCurveNames[WEIGHTYAIM_NUM_CURVES];
extern s32 g_WeightyAimDebugLog;       // write per-frame telemetry to weightyaim_log.csv
extern s32 g_WeightyAimDebugPattern;   // replace look input with a scripted test pattern

extern const char *g_WeightyAimPresetNames[WEIGHTYAIM_NUM_PRESETS];

void weightyAimResetDefaults(s32 cfgindex);
void weightyAimApplyPreset(s32 cfgindex, s32 preset);
void weightyAimResetStickDefaults(s32 cfgindex);

/*
 * Hook 1 (bondmove.c, before the look code runs): takes this frame's look
 * input, moves the gun inside the deadzone, and rewrites the input so that
 * only the leftover turning reaches the camera.
 */
void weightyAimFilterLook(s32 *analogturn, s32 *analogpitch, f32 *freelookdx, f32 *freelookdy,
		bool canlook, f32 mlookscale);

/*
 * Hook 2 (bondmove.c, crosshair swivel): true when Weighty Aim is driving the
 * crosshair this frame; weightyAimGetCrosshair() then gives its screen position
 * in the same -1..1 units bgunSwivel() uses.
 */
bool weightyAimIsActive(void);
void weightyAimGetCrosshair(f32 *x, f32 *y);

/*
 * Hook 3 (sight.c): true when the crosshair should be hidden right now
 * (crosshair set to "only when aiming" and the player is hip-firing).
 */
bool weightyAimHideCrosshair(void);

/*
 * Hook 4 (bondgun.c, per hand each frame): true if Weighty Aim wants a laser
 * sight on this gun; weightyAimUpdateLaser() then places the beam and dot.
 */
struct hand;
bool weightyAimLaserWanted(struct hand *hand, s32 handnum, s32 weaponnum);
void weightyAimUpdateLaser(struct hand *hand, s32 handnum);

/*
 * Hook 5 (gunfx.c, laser rendering): true when the enhanced laser look is on
 * for the current player (also upgrades the Falcon 2's built-in laser).
 */
bool weightyAimLaserEnhanced(void);

#endif
