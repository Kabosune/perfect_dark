#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include <math.h>
#include "lib/vi.h"
#include "config.h"
#include "system.h"
#include "utils.h"
#include "fs.h"
#include "weightyaim.h"

/*
 * Units used in this file
 * -----------------------
 * PD's look code works in "speed units": the camera yaws by
 * speedtheta * 3.5 degrees per 60Hz tick (see bwalkUpdateTheta) and pitches by
 * speedverta * 3.5 degrees per tick (see bmoveProcessInput). We convert the
 * requested look input into degrees for this frame, run the free-aim model in
 * degrees, and convert the leftover back into speed units for the camera.
 *
 * Gun offsets (target/display) are yaw/pitch angles in degrees relative to the
 * camera: +yaw = right, +pitch = up.
 */

// PD's own math.h shadows the C library one, so use the compiler builtins
#define bc_fmodf __builtin_fmodf
#define bc_ceilf __builtin_ceilf
#define bc_expf  __builtin_expf
#define bc_tanf  __builtin_tanf
#define bc_sqrtf __builtin_sqrtf

#define DEG_PER_SPEED_TICK 3.5f
#define SPRING_STEP (1.f / 240.f)

struct weightyaimcfg g_WeightyAimCfg[4];
s32 g_WeightyAimDebugLog = 0;
s32 g_WeightyAimDebugPattern = 0;

struct weightyaimstate {
	f32 target[2];   // where the gun wants to point (yaw, pitch), degrees from camera centre
	f32 display[2];  // where the gun actually points after inertia
	f32 vel[2];      // spring velocity, degrees/second
	f32 idletime;    // seconds since the last look input
	bool active;     // Weighty Aim drove this player's crosshair on the last update
};

static struct weightyaimstate g_WeightyAimState[MAX_PLAYERS];

// telemetry
static FILE *g_WeightyAimLogFile = NULL;
static f32 g_WeightyAimLogTime = 0.f;
static f32 g_WeightyAimPatternTime = 0.f;

static const struct weightyaimcfg g_WeightyAimDefaults = {
	.enabled = 1,
	.deadzonex = 7.f,
	.deadzoney = 4.5f,
	.stickaimspeed = 0.4f,
	.mouseaimspeed = 1.f,
	.recenterspeed = 0.8f,
	.recenterdelay = 0.35f,
	.gunresponse = 7.f,
	.gundamping = 0.65f,
	.turndrag = 0.5f,
};

void weightyAimResetDefaults(s32 cfgindex)
{
	g_WeightyAimCfg[cfgindex & 3] = g_WeightyAimDefaults;
}

static inline f32 clampf(f32 v, f32 lo, f32 hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline struct weightyaimcfg *weightyAimCurCfg(void)
{
	return &g_WeightyAimCfg[g_Vars.currentplayerstats->mpindex & 3];
}

static inline struct weightyaimstate *weightyAimCurState(void)
{
	return &g_WeightyAimState[g_Vars.currentplayernum & 3];
}

/**
 * PD squares the analog stick (keeping its sign) before using it as a turn rate.
 */
static inline f32 weightyAimStickCurve(s32 analog)
{
	f32 v = clampf(analog / 70.f, -1.f, 1.f);
	return v >= 0.f ? v * v : -v * v;
}

/**
 * Scripted look input for repeatable testing, in speed units (-1..1).
 * One cycle is 4.8 seconds:
 *   fast turn right, stop, fast turn left, stop, slow sweep inside the zone, stop.
 */
static f32 weightyAimPatternYaw(f32 t)
{
	t = bc_fmodf(t, 4.8f);

	if (t < 0.4f) return 0.5f;
	if (t < 1.6f) return 0.f;
	if (t < 1.8f) return -1.f;
	if (t < 3.0f) return 0.f;
	if (t < 3.6f) return 0.12f;
	return 0.f;
}

static void weightyAimLogOpen(void)
{
	if (g_WeightyAimLogFile) {
		return;
	}

	g_WeightyAimLogFile = fopen(fsFullPath("$E/weightyaim_log.csv"), "w");

	if (g_WeightyAimLogFile) {
		fprintf(g_WeightyAimLogFile,
			"time,dt,enabled,active,in_yaw,in_pitch,target_yaw,target_pitch,"
			"gun_yaw,gun_pitch,cam_yaw,cam_pitch,cross_x,cross_y,theta,verta,fovy\n");
		g_WeightyAimLogTime = 0.f;
		sysLogPrintf(LOG_NOTE, "weightyaim: logging to %s", fsFullPath("$E/weightyaim_log.csv"));
	} else {
		sysLogPrintf(LOG_WARNING, "weightyaim: could not open log file");
		g_WeightyAimDebugLog = 0;
	}
}

static void weightyAimLogClose(void)
{
	if (g_WeightyAimLogFile) {
		fclose(g_WeightyAimLogFile);
		g_WeightyAimLogFile = NULL;
	}
}

/**
 * Advance the gun's inertia spring towards its target.
 * Fixed substeps keep it identical at any frame rate.
 */
static void weightyAimStepSpring(struct weightyaimstate *st, const struct weightyaimcfg *cfg, f32 dtsec)
{
	const f32 w = 2.f * (f32)M_PI * clampf(cfg->gunresponse, 0.5f, 40.f);
	const f32 k = w * w;
	const f32 c = 2.f * clampf(cfg->gundamping, 0.05f, 3.f) * w;
	s32 steps = (s32)bc_ceilf(dtsec / SPRING_STEP);
	f32 h;

	if (steps < 1) {
		steps = 1;
	} else if (steps > 64) {
		steps = 64;
	}

	h = dtsec / steps;

	for (s32 s = 0; s < steps; s++) {
		for (s32 i = 0; i < 2; i++) {
			const f32 acc = k * (st->target[i] - st->display[i]) - c * st->vel[i];
			st->vel[i] += acc * h;
			st->display[i] += st->vel[i] * h;
		}
	}
}

static void weightyAimResetState(struct weightyaimstate *st)
{
	st->target[0] = st->target[1] = 0.f;
	st->display[0] = st->display[1] = 0.f;
	st->vel[0] = st->vel[1] = 0.f;
	st->idletime = 0.f;
}

void weightyAimFilterLook(s32 *analogturn, s32 *analogpitch, f32 *freelookdx, f32 *freelookdy,
		bool canlook, f32 mlookscale)
{
	struct weightyaimcfg *cfg = weightyAimCurCfg();
	struct weightyaimstate *st = weightyAimCurState();
	const f32 dt60 = g_Vars.lvupdate60freal;
	const f32 dtsec = dt60 / 60.f;
	const f32 fovscale = viGetFovY() / PLAYER_DEFAULT_FOV;
	const bool isfirstplayer = (g_Vars.currentplayernum == 0);
	f32 stickdeg[2], mousedeg[2], reqdeg[2], camdeg[2] = { 0.f, 0.f };
	bool active;

	if (dt60 <= 0.f || mlookscale <= 0.f || fovscale <= 0.f) {
		// paused or no time passed; keep the crosshair where it is
		return;
	}

	// Debug: scripted input replaces the player's look input
	if (g_WeightyAimDebugPattern && isfirstplayer && canlook) {
		g_WeightyAimPatternTime += dtsec;
		*analogturn = 0;
		*analogpitch = 0;
		*freelookdx = weightyAimPatternYaw(g_WeightyAimPatternTime) / mlookscale;
		*freelookdy = 0.f;
	}

	// Requested rotation this frame in degrees, exactly as PD would apply it.
	// Pitch: PD sets speedverta = -(stick + mouse), and +speedverta looks up.
	stickdeg[0] = weightyAimStickCurve(*analogturn) * fovscale * DEG_PER_SPEED_TICK * dt60;
	mousedeg[0] = *freelookdx * mlookscale * fovscale * DEG_PER_SPEED_TICK * dt60;
	stickdeg[1] = -weightyAimStickCurve(*analogpitch) * fovscale * DEG_PER_SPEED_TICK * dt60;
	mousedeg[1] = -*freelookdy * mlookscale * fovscale * DEG_PER_SPEED_TICK * dt60;
	reqdeg[0] = stickdeg[0] + mousedeg[0];
	reqdeg[1] = stickdeg[1] + mousedeg[1];

	active = cfg->enabled
		&& canlook
		&& g_Vars.currentplayer->bondmovemode == MOVEMODE_WALK
		&& !g_Vars.currentplayer->insightaimmode
		&& !g_Vars.currentplayer->isdead
		&& g_Vars.tickmode == TICKMODE_NORMAL;

	if (!active) {
		weightyAimResetState(st);
		st->active = false;
	} else {
		const f32 zx = clampf(cfg->deadzonex, 0.05f, 45.f);
		const f32 zy = clampf(cfg->deadzoney, 0.05f, 45.f);
		f32 move[2];
		f32 reqlen, movelen, ellipse;

		// 1. Look input moves the gun first, at its own speed
		move[0] = stickdeg[0] * cfg->stickaimspeed + mousedeg[0] * cfg->mouseaimspeed;
		move[1] = stickdeg[1] * cfg->stickaimspeed + mousedeg[1] * cfg->mouseaimspeed;
		reqlen = bc_sqrtf(reqdeg[0] * reqdeg[0] + reqdeg[1] * reqdeg[1]);
		movelen = bc_sqrtf(move[0] * move[0] + move[1] * move[1]);

		if (reqlen > 0.0001f && movelen < 0.0001f) {
			// gun can't move on its own: everything turns the camera
			camdeg[0] = reqdeg[0];
			camdeg[1] = reqdeg[1];
		} else if (reqlen > 0.0001f) {
			const f32 ratio = movelen / reqlen; // gun degrees per requested degree

			st->target[0] += move[0];
			st->target[1] += move[1];

			// 2. Anything that pushes past the edge of the zone turns the camera instead
			ellipse = (st->target[0] / zx) * (st->target[0] / zx) + (st->target[1] / zy) * (st->target[1] / zy);

			if (ellipse > 1.f) {
				const f32 scale = 1.f / bc_sqrtf(ellipse);
				const f32 overx = st->target[0] - st->target[0] * scale;
				const f32 overy = st->target[1] - st->target[1] * scale;

				st->target[0] *= scale;
				st->target[1] *= scale;
				camdeg[0] = overx / ratio;
				camdeg[1] = overy / ratio;
			}
		}

		// If the zone was shrunk from the menu, pull the target back inside
		ellipse = (st->target[0] / zx) * (st->target[0] / zx) + (st->target[1] / zy) * (st->target[1] / zy);

		if (ellipse > 1.f) {
			const f32 scale = 1.f / bc_sqrtf(ellipse);
			st->target[0] *= scale;
			st->target[1] *= scale;
		}

		// 3. When idle, the camera slowly catches up to where the gun points.
		//    The gun stays pointed at the same spot in the world while this happens.
		if (reqlen > 0.0001f) {
			st->idletime = 0.f;
		} else {
			st->idletime += dtsec;
		}

		if (cfg->recenterspeed > 0.f && st->idletime > cfg->recenterdelay) {
			const f32 k = 1.f - bc_expf(-cfg->recenterspeed * dtsec);
			const f32 ry = st->target[0] * k;
			const f32 rp = st->target[1] * k;

			st->target[0] -= ry;
			st->target[1] -= rp;
			st->display[0] -= ry;
			st->display[1] -= rp;
			camdeg[0] += ry;
			camdeg[1] += rp;
		} else {
			// 4. When the camera turns, the gun lags behind for a moment
			st->display[0] -= camdeg[0] * clampf(cfg->turndrag, 0.f, 1.f);
			st->display[1] -= camdeg[1] * clampf(cfg->turndrag, 0.f, 1.f);
		}

		weightyAimStepSpring(st, cfg, dtsec);

		// keep the gun on screen even after a violent turn
		st->display[0] = clampf(st->display[0], -zx * 2.f - 10.f, zx * 2.f + 10.f);
		st->display[1] = clampf(st->display[1], -zy * 2.f - 10.f, zy * 2.f + 10.f);

		// 5. Hand only the leftover turning to PD's camera code.
		//    With analog zeroed, PD computes speed = freelook * mlookscale * fovscale.
		*analogturn = 0;
		*analogpitch = 0;
		*freelookdx = camdeg[0] / (DEG_PER_SPEED_TICK * dt60 * mlookscale * fovscale);
		*freelookdy = -camdeg[1] / (DEG_PER_SPEED_TICK * dt60 * mlookscale * fovscale);

		st->active = true;
	}

	// Telemetry (first player only)
	if (g_WeightyAimDebugLog && isfirstplayer) {
		weightyAimLogOpen();

		if (g_WeightyAimLogFile) {
			const struct player *pl = g_Vars.currentplayer;
			const f32 sw = viGetViewWidth() * 0.5f;
			const f32 sh = viGetViewHeight() * 0.5f;

			g_WeightyAimLogTime += dtsec;
			fprintf(g_WeightyAimLogFile,
				"%.4f,%.4f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.2f\n",
				g_WeightyAimLogTime, dtsec, cfg->enabled, st->active,
				reqdeg[0], reqdeg[1], st->target[0], st->target[1],
				st->display[0], st->display[1], camdeg[0], camdeg[1],
				sw > 0.f ? (pl->crosspos[0] - sw) / sw : 0.f,
				sh > 0.f ? (pl->crosspos[1] - sh) / sh : 0.f,
				pl->vv_theta, pl->vv_verta, viGetFovY());
		}
	} else if (!g_WeightyAimDebugLog && g_WeightyAimLogFile) {
		weightyAimLogClose();
	}
}

bool weightyAimIsActive(void)
{
	return g_WeightyAimCfg[g_Vars.currentplayerstats->mpindex & 3].enabled && weightyAimCurState()->active;
}

void weightyAimGetCrosshair(f32 *x, f32 *y)
{
	const struct weightyaimstate *st = weightyAimCurState();
	const f32 fovy = viGetFovY();
	const f32 aspect = viGetAspect();
	const f32 tanhalfy = bc_tanf(DEG2RAD(fovy * 0.5f));
	const f32 tanhalfx = tanhalfy * (aspect > 0.f ? aspect : 1.333f);

	// project the gun direction onto the screen; +y is down in bgunSwivel units
	*x = clampf(bc_tanf(DEG2RAD(st->display[0])) / tanhalfx, -0.97f, 0.97f);
	*y = clampf(-bc_tanf(DEG2RAD(st->display[1])) / tanhalfy, -0.97f, 0.97f);
}

PD_CONSTRUCTOR static void weightyAimConfigInit(void)
{
	for (s32 j = 0; j < MAX_PLAYERS; ++j) {
		const s32 i = j + 1;
		weightyAimResetDefaults(j);
		configRegisterInt(strFmt("WeightyAim.Player%d.Enabled", i), &g_WeightyAimCfg[j].enabled, 0, 1);
		configRegisterFloat(strFmt("WeightyAim.Player%d.DeadzoneX", i), &g_WeightyAimCfg[j].deadzonex, 0.f, 45.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.DeadzoneY", i), &g_WeightyAimCfg[j].deadzoney, 0.f, 45.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickAimSpeed", i), &g_WeightyAimCfg[j].stickaimspeed, 0.f, 3.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.MouseAimSpeed", i), &g_WeightyAimCfg[j].mouseaimspeed, 0.f, 3.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.RecenterSpeed", i), &g_WeightyAimCfg[j].recenterspeed, 0.f, 10.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.RecenterDelay", i), &g_WeightyAimCfg[j].recenterdelay, 0.f, 5.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GunResponse", i), &g_WeightyAimCfg[j].gunresponse, 0.5f, 40.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GunDamping", i), &g_WeightyAimCfg[j].gundamping, 0.05f, 3.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.TurnDrag", i), &g_WeightyAimCfg[j].turndrag, 0.f, 1.f);
	}

	configRegisterInt("WeightyAim.Debug.Log", &g_WeightyAimDebugLog, 0, 1);
	configRegisterInt("WeightyAim.Debug.TestPattern", &g_WeightyAimDebugPattern, 0, 1);
}
