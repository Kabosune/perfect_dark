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
 * WeightyAim.PlayerN.Enabled = 0 the original code path runs untouched.
 */

#include <PR/ultratypes.h>
#include "types.h"

struct weightyaimcfg {
	s32 enabled;         // 0 = original PD behaviour, 1 = Weighty Aim free-aim
	f32 deadzonex;       // half-width of the free-aim zone, degrees
	f32 deadzoney;       // half-height of the free-aim zone, degrees
	f32 stickaimspeed;   // how fast the stick moves the gun inside the zone (1 = same as turning)
	f32 mouseaimspeed;   // same for the mouse
	f32 recenterspeed;   // how fast the camera catches up to the gun when idle (per second, 0 = never)
	f32 recenterdelay;   // seconds without look input before catch-up starts
	f32 gunresponse;     // weapon inertia spring frequency in Hz (lower = heavier gun)
	f32 gundamping;      // spring damping ratio (1 = no overshoot, lower = more overshoot)
	f32 turndrag;        // how much the gun lags behind when the camera turns (0..1)
};

extern struct weightyaimcfg g_WeightyAimCfg[4];
extern s32 g_WeightyAimDebugLog;       // write per-frame telemetry to weightyaim_log.csv
extern s32 g_WeightyAimDebugPattern;   // replace look input with a scripted test pattern

void weightyAimResetDefaults(s32 cfgindex);

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

#endif
