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
#include "lib/mtx.h"
#include "game/camera.h"
#include "game/gunfx.h"
#include "game/game_0b0fd0.h"
#include "game/bondgun.h"
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
#define bc_sinf  __builtin_sinf
#define bc_fabsf __builtin_fabsf

#define DEG_PER_SPEED_TICK 3.5f
#define SPRING_STEP (1.f / 240.f)

struct weightyaimcfg g_WeightyAimCfg[4];
struct weightyaimstickcfg g_WeightyAimStickCfg[4];

const char *g_WeightyAimCurveNames[WEIGHTYAIM_NUM_CURVES] = {
	"Original",
	"Linear",
	"Balanced",
	"Custom",
};

const char *g_WeightyAimBoostNames[WEIGHTYAIM_NUM_BOOSTS] = {
	"Off",
	"Instant",
	"Ramped",
};

static const struct weightyaimstickcfg g_WeightyAimStickDefaults = {
	.curve = WEIGHTYAIM_CURVE_BALANCED,
	.innerdeadzone = 0.08f,
	.outerdeadzone = 0.95f,
	.bezier = { 0.4f, 0.0f, 0.75f, 1.0f },
	.turnspeed = 1.f,
	.boostmode = WEIGHTYAIM_BOOST_RAMPED,
	.boostamount = 1.8f,
	.boostthreshold = 0.9f,
	.boostdelay = 0.12f,
	.boosttime = 0.3f,
	.boostvertical = 0.35f,
};

void weightyAimResetStickDefaults(s32 cfgindex)
{
	// curve, deadzones and max speed only; boost has its own reset
	struct weightyaimstickcfg *sc = &g_WeightyAimStickCfg[cfgindex & 3];
	const struct weightyaimstickcfg *d = &g_WeightyAimStickDefaults;

	sc->curve = d->curve;
	sc->innerdeadzone = d->innerdeadzone;
	sc->outerdeadzone = d->outerdeadzone;
	memcpy(sc->bezier, d->bezier, sizeof(sc->bezier));
	sc->turnspeed = d->turnspeed;
}

void weightyAimResetBoostDefaults(s32 cfgindex)
{
	struct weightyaimstickcfg *sc = &g_WeightyAimStickCfg[cfgindex & 3];
	const struct weightyaimstickcfg *d = &g_WeightyAimStickDefaults;

	sc->boostmode = d->boostmode;
	sc->boostamount = d->boostamount;
	sc->boostthreshold = d->boostthreshold;
	sc->boostdelay = d->boostdelay;
	sc->boosttime = d->boosttime;
	sc->boostvertical = d->boostvertical;
}
s32 g_WeightyAimDebugLog = 0;
s32 g_WeightyAimDebugPattern = 0;

struct weightyaimstate {
	f32 target[2];   // where the gun wants to point (yaw, pitch), degrees from camera centre
	f32 display[2];  // where the gun actually points after inertia
	f32 vel[2];      // spring velocity, degrees/second
	f32 over[2];     // how far the gun is pushed past the edge of the zone, degrees (drains into camera turn)
	f32 idletime;    // seconds since the last look input
	f32 boostheld;   // seconds the stick has been held past the boost threshold
	f32 boostlevel;  // current turn boost, 0..1
	bool adsheld;    // aim button held with aim-down-sights on, this frame
	f32 adsblend;    // 0 = hip, 1 = fully aimed down sights (linear, see weightyAimAdsAmount)
	f32 gunpos[3];   // where the gun model was placed last frame (camera space)
	bool gunposvalid;
	f32 swayphase[3];// camera sway oscillator phases (breath, drift, footsteps)
	f32 sway[2];     // camera sway offset applied last frame, degrees
	bool active;     // Weighty Aim drove this player's crosshair on the last update
};

static struct weightyaimstate g_WeightyAimState[MAX_PLAYERS];

// telemetry
static FILE *g_WeightyAimLogFile = NULL;
static f32 g_WeightyAimLogTime = 0.f;
static f32 g_WeightyAimPatternTime = 0.f;

const char *g_WeightyAimPresetNames[WEIGHTYAIM_NUM_PRESETS] = {
	"Weighty",
	"Immersive",
	"Boring",
	"Classic",
	"Custom",
};

// Weighty: free-aim with a crosshair, a gun with some heft, a hint of sway
static const struct weightyaimcfg g_WeightyAimPresetWeighty = {
	.preset = WEIGHTYAIM_PRESET_WEIGHTY,
	.deadzonex = 8.5f,
	.deadzoney = 5.f,
	.camerashare = 0.35f,
	.cameralead = 0.9f,
	.recenterspeed = 1.f,
	.recenterdelay = 0.25f,
	.gunresponse = 5.5f,
	.gundamping = 0.55f,
	.turndrag = 0.65f,
	.edgesmoothing = 0.12f,
	.camerasway = 0.12f,
	.walksway = 0.35f,
	.crosshair = WEIGHTYAIM_CROSSHAIR_ALWAYS,
	.laser = 0,
	.ads = 1,
	.adszoom = 1.3f,
	.adstime = 0.18f,
	.adssway = 0.35f,
	.adszone = 0.25f,
	.adsheight = 0.f,
};

// Immersive: wide zone, heavy flowing gun, a camera that follows and never sits still
static const struct weightyaimcfg g_WeightyAimPresetImmersive = {
	.preset = WEIGHTYAIM_PRESET_IMMERSIVE,
	.deadzonex = 11.f,
	.deadzoney = 6.5f,
	.camerashare = 0.4f,
	.cameralead = 1.4f,
	.recenterspeed = 1.3f,
	.recenterdelay = 0.15f,
	.gunresponse = 3.6f,
	.gundamping = 0.45f,
	.turndrag = 0.9f,
	.edgesmoothing = 0.2f,
	.camerasway = 0.45f,
	.walksway = 1.1f,
	.crosshair = WEIGHTYAIM_CROSSHAIR_AIMONLY,
	.laser = 1,
	.ads = 1,
	.adszoom = 1.25f,
	.adstime = 0.26f,
	.adssway = 0.5f,
	.adszone = 0.35f,
	.adsheight = 0.f,
};

// Boring: what most shooters do. Crosshair locked to the centre, the camera
// follows the stick directly, no gun lag, no sway.
static const struct weightyaimcfg g_WeightyAimPresetBoring = {
	.preset = WEIGHTYAIM_PRESET_BORING,
	.deadzonex = 0.f,
	.deadzoney = 0.f,
	.camerashare = 1.f,
	.cameralead = 0.f,
	.recenterspeed = 0.f,
	.recenterdelay = 0.f,
	.gunresponse = 20.f,
	.gundamping = 1.f,
	.turndrag = 0.f,
	.edgesmoothing = 0.f,
	.camerasway = 0.f,
	.walksway = 0.f,
	.crosshair = WEIGHTYAIM_CROSSHAIR_ALWAYS,
	.laser = 0,
	.ads = 1,
	.adszoom = 1.35f,
	.adstime = 0.12f,
	.adssway = 0.f,
	.adszone = 0.f,
	.adsheight = 0.f,
};

void weightyAimApplyPreset(s32 cfgindex, s32 preset)
{
	struct weightyaimcfg *cfg = &g_WeightyAimCfg[cfgindex & 3];

	switch (preset) {
	case WEIGHTYAIM_PRESET_WEIGHTY:
		*cfg = g_WeightyAimPresetWeighty;
		break;
	case WEIGHTYAIM_PRESET_IMMERSIVE:
		*cfg = g_WeightyAimPresetImmersive;
		break;
	case WEIGHTYAIM_PRESET_BORING:
		*cfg = g_WeightyAimPresetBoring;
		break;
	case WEIGHTYAIM_PRESET_CLASSIC:
	case WEIGHTYAIM_PRESET_CUSTOM:
		// keep the current values; Classic just switches the mod off
		cfg->preset = preset;
		break;
	}
}

void weightyAimResetDefaults(s32 cfgindex)
{
	weightyAimApplyPreset(cfgindex, WEIGHTYAIM_PRESET_WEIGHTY);
}

static inline bool weightyAimCfgEnabled(const struct weightyaimcfg *cfg)
{
	return cfg->preset != WEIGHTYAIM_PRESET_CLASSIC;
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
 * Cubic bezier through (0,0), (x1,y1), (x2,y2), (1,1): find t where x(t) = x
 * by bisection (x(t) is monotonic while x1, x2 are within 0..1), return y(t).
 */
static f32 weightyAimBezier(const f32 *p, f32 x)
{
	const f32 x1 = clampf(p[0], 0.f, 1.f), y1 = clampf(p[1], 0.f, 1.f);
	const f32 x2 = clampf(p[2], 0.f, 1.f), y2 = clampf(p[3], 0.f, 1.f);
	f32 lo = 0.f, hi = 1.f, t = x, u;

	for (s32 i = 0; i < 24; i++) {
		u = 1.f - t;
		const f32 xt = 3.f * u * u * t * x1 + 3.f * u * t * t * x2 + t * t * t;

		if (xt < x) {
			lo = t;
		} else {
			hi = t;
		}

		t = (lo + hi) * 0.5f;
	}

	u = 1.f - t;
	return 3.f * u * u * t * y1 + 3.f * u * t * t * y2 + t * t * t;
}

/**
 * Turn the look stick into turn rates in speed units (-1..1 per axis).
 *
 * Original: the game's own per-axis squared response.
 * Others: radial deadzone (inner/outer) so diagonals behave like straight
 * lines, then the chosen curve applied to how far the stick is pushed.
 */
/**
 * Turn boost: extra turn speed while the stick is held near full deflection.
 * Instant jumps straight to full boost; Ramped waits a moment and then builds
 * up, so quick flicks to the edge stay precise but long holds turn fast.
 * Looking up/down gets only part of the boost.
 */
static void weightyAimApplyBoost(const struct weightyaimstickcfg *sc, struct weightyaimstate *st,
		f32 deflection, f32 dtsec, f32 *out)
{
	f32 extra;

	if (sc->boostmode == WEIGHTYAIM_BOOST_OFF) {
		st->boostheld = 0.f;
		st->boostlevel = 0.f;
		return;
	}

	if (deflection >= clampf(sc->boostthreshold, 0.3f, 1.f) - 0.0001f) {
		f32 goal = 1.f;

		st->boostheld += dtsec;

		if (sc->boostmode == WEIGHTYAIM_BOOST_RAMPED) {
			goal = (st->boostheld - sc->boostdelay) / (sc->boosttime > 0.01f ? sc->boosttime : 0.01f);
			goal = clampf(goal, 0.f, 1.f);
			goal = goal * goal * (3.f - 2.f * goal); // ease in and out
		}

		st->boostlevel = goal > st->boostlevel ? goal : st->boostlevel;
	} else {
		// let go of the edge: drop the boost quickly but not in a single frame
		st->boostheld = 0.f;
		st->boostlevel = clampf(st->boostlevel - dtsec / 0.1f, 0.f, 1.f);
	}

	extra = (clampf(sc->boostamount, 1.f, 4.f) - 1.f) * st->boostlevel;
	out[0] *= 1.f + extra;
	out[1] *= 1.f + extra * clampf(sc->boostvertical, 0.f, 1.f);
}

/**
 * Turn the look stick into turn rates in speed units (-1..1 per axis, more with boost).
 *
 * Original: the game's own per-axis squared response.
 * Others: radial deadzone (inner/outer) so diagonals behave like straight
 * lines, then the chosen curve applied to how far the stick is pushed.
 */
static void weightyAimStickRates(const struct weightyaimstickcfg *sc, struct weightyaimstate *st,
		s32 turn, s32 pitch, f32 dtsec, f32 *out)
{
	f32 rx, ry, mag, n, o, lo, hi;

	// PD already subtracted a small 5-unit safe zone; put it back so our own
	// deadzone is the only one (full deflection is about 127)
	rx = turn == 0 ? 0.f : (turn + (turn > 0 ? 5 : -5)) / 127.f;
	ry = pitch == 0 ? 0.f : (pitch + (pitch > 0 ? 5 : -5)) / 127.f;
	mag = bc_sqrtf(rx * rx + ry * ry);

	if (sc->curve == WEIGHTYAIM_CURVE_ORIGINAL) {
		out[0] = weightyAimStickCurve(turn);
		out[1] = weightyAimStickCurve(pitch);
		weightyAimApplyBoost(sc, st, clampf(mag, 0.f, 1.f), dtsec, out);
		return;
	}

	if (mag < 0.0001f) {
		out[0] = out[1] = 0.f;
		weightyAimApplyBoost(sc, st, 0.f, dtsec, out);
		return;
	}

	lo = clampf(sc->innerdeadzone, 0.f, 0.9f);
	hi = clampf(sc->outerdeadzone, lo + 0.05f, 1.f);
	n = clampf((mag - lo) / (hi - lo), 0.f, 1.f);

	switch (sc->curve) {
	case WEIGHTYAIM_CURVE_LINEAR:
		o = n;
		break;
	case WEIGHTYAIM_CURVE_CUSTOM:
		o = weightyAimBezier(sc->bezier, n);
		break;
	case WEIGHTYAIM_CURVE_BALANCED:
	default:
		o = n * bc_sqrtf(n); // n^1.5, halfway between linear and squared
		break;
	}

	o *= clampf(sc->turnspeed, 0.1f, 3.f);
	out[0] = rx / mag * o;
	out[1] = ry / mag * o;
	weightyAimApplyBoost(sc, st, n, dtsec, out);
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
			"time,dt,preset,active,in_yaw,in_pitch,target_yaw,target_pitch,"
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
			const f32 acc = k * (st->target[i] + st->over[i] - st->display[i]) - c * st->vel[i];
			st->vel[i] += acc * h;
			st->display[i] += st->vel[i] * h;
		}
	}
}

/**
 * How far into aiming down sights we are, eased (0 = hip, 1 = aimed).
 */
static inline f32 weightyAimAdsAmount(const struct weightyaimstate *st)
{
	const f32 b = st->adsblend;
	return b * b * (3.f - 2.f * b);
}

static void weightyAimResetState(struct weightyaimstate *st)
{
	st->target[0] = st->target[1] = 0.f;
	st->display[0] = st->display[1] = 0.f;
	st->vel[0] = st->vel[1] = 0.f;
	st->over[0] = st->over[1] = 0.f;
	st->idletime = 0.f;
	st->sway[0] = st->sway[1] = 0.f;
}

void weightyAimFilterLook(s32 *analogturn, s32 *analogpitch, f32 *freelookdx, f32 *freelookdy,
		bool canlook, f32 mlookscale)
{
	struct weightyaimcfg *cfg = weightyAimCurCfg();
	struct weightyaimstate *st = weightyAimCurState();
	const struct weightyaimstickcfg *sc = &g_WeightyAimStickCfg[g_Vars.currentplayerstats->mpindex & 3];
	const f32 dt60 = g_Vars.lvupdate60freal;
	const f32 dtsec = dt60 / 60.f;
	const f32 fovscale = viGetFovY() / PLAYER_DEFAULT_FOV;
	const bool isfirstplayer = (g_Vars.currentplayernum == 0);
	f32 stickrate[2], stickdeg[2], mousedeg[2], reqdeg[2], camdeg[2] = { 0.f, 0.f };
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
	weightyAimStickRates(sc, st, *analogturn, *analogpitch, dtsec, stickrate);
	stickdeg[0] = stickrate[0] * fovscale * DEG_PER_SPEED_TICK * dt60;
	mousedeg[0] = *freelookdx * mlookscale * fovscale * DEG_PER_SPEED_TICK * dt60;
	stickdeg[1] = -stickrate[1] * fovscale * DEG_PER_SPEED_TICK * dt60;
	mousedeg[1] = -*freelookdy * mlookscale * fovscale * DEG_PER_SPEED_TICK * dt60;
	reqdeg[0] = stickdeg[0] + mousedeg[0];
	reqdeg[1] = stickdeg[1] + mousedeg[1];

	// Raise or lower the gun for aiming down sights
	{
		const f32 goal = st->adsheld ? 1.f : 0.f;
		const f32 step = dtsec / (cfg->adstime > 0.01f ? cfg->adstime : 0.01f);

		if (st->adsblend < goal) {
			st->adsblend = clampf(st->adsblend + step, 0.f, goal);
		} else if (st->adsblend > goal) {
			st->adsblend = clampf(st->adsblend - step, goal, 1.f);
		}
	}

	active = weightyAimCfgEnabled(cfg)
		&& canlook
		&& g_Vars.currentplayer->bondmovemode == MOVEMODE_WALK
		&& (!g_Vars.currentplayer->insightaimmode || st->adsheld)
		&& !g_Vars.currentplayer->isdead
		&& g_Vars.tickmode == TICKMODE_NORMAL;

	if (!active) {
		weightyAimResetState(st);
		st->active = false;

		// Aim mod off (Classic) but a custom stick response chosen: still apply it.
		// With analog zeroed, PD computes speed = (freelook * mlookscale) * fovscale.
		if (canlook && (sc->curve != WEIGHTYAIM_CURVE_ORIGINAL || st->boostlevel > 0.f)) {
			*analogturn = 0;
			*analogpitch = 0;
			*freelookdx += stickrate[0] / mlookscale;
			*freelookdy += stickrate[1] / mlookscale;
		}
	} else {
		// While aiming down sights the zone shrinks, the camera takes over more of
		// the turning, sway calms down and the gun steadies.
		const f32 ads = weightyAimAdsAmount(st);
		struct weightyaimcfg effcfg = *cfg;
		struct weightyaimcfg *ec = &effcfg;

		ec->deadzonex *= 1.f + (clampf(cfg->adszone, 0.f, 1.f) - 1.f) * ads;
		ec->deadzoney *= 1.f + (clampf(cfg->adszone, 0.f, 1.f) - 1.f) * ads;
		ec->camerashare += (1.f - ec->camerashare) * ads * (1.f - clampf(cfg->adszone, 0.f, 1.f));
		ec->camerasway *= 1.f + (clampf(cfg->adssway, 0.f, 1.f) - 1.f) * ads;
		ec->walksway *= 1.f + (clampf(cfg->adssway, 0.f, 1.f) - 1.f) * ads;
		ec->turndrag *= 1.f - 0.6f * ads;
		ec->gunresponse *= 1.f + 0.8f * ads;

		const f32 zx = clampf(ec->deadzonex, 0.05f, 45.f);
		const f32 zy = clampf(ec->deadzoney, 0.05f, 45.f);
		f32 reqlen, ellipse;

		// 1. Every look movement is split: part turns the camera right away (so the
		//    view always answers the stick), the rest moves the gun inside the zone.
		//    The two always add up to exactly the input, so aim speed never changes.
		const f32 share = clampf(ec->camerashare, 0.f, 1.f);
		const f32 drag = clampf(ec->turndrag, 0.f, 1.f);

		reqlen = bc_sqrtf(reqdeg[0] * reqdeg[0] + reqdeg[1] * reqdeg[1]);

		if (reqlen > 0.0001f) {
			for (s32 i = 0; i < 2; i++) {
				const f32 direct = reqdeg[i] * share;

				camdeg[i] += direct;
				st->display[i] -= direct * drag; // the gun lags a touch behind the turn
				st->target[i] += reqdeg[i] - direct;
			}

			// 2. Whatever pushes the gun past the edge of the zone goes into the
			//    overflow, and the camera eases into it below (edge smoothing)
			ellipse = (st->target[0] / zx) * (st->target[0] / zx) + (st->target[1] / zy) * (st->target[1] / zy);

			if (ellipse > 1.f) {
				const f32 scale = 1.f / bc_sqrtf(ellipse);

				st->over[0] += st->target[0] - st->target[0] * scale;
				st->over[1] += st->target[1] - st->target[1] * scale;
				st->target[0] *= scale;
				st->target[1] *= scale;
			}
		}

		// If the zone shrank (aiming down sights, or a menu change), the part of the
		// gun that's now outside it becomes overflow, so the camera eases toward
		// it and the gun keeps pointing at the same spot
		ellipse = (st->target[0] / zx) * (st->target[0] / zx) + (st->target[1] / zy) * (st->target[1] / zy);

		if (ellipse > 1.f) {
			const f32 scale = 1.f / bc_sqrtf(ellipse);

			st->over[0] += st->target[0] - st->target[0] * scale;
			st->over[1] += st->target[1] - st->target[1] * scale;
			st->target[0] *= scale;
			st->target[1] *= scale;
		}

		// 3. Edge smoothing: the camera turns toward the overflow over a short time
		//    instead of instantly, so hitting the edge eases in and out. The more
		//    the gun is pushed past the edge, the harder the camera follows, so
		//    fast turns still keep up. The gun holds its aim in the world while
		//    the camera turns, and lags a little extra (turn drag).
		{
			const f32 softcap = 0.25f * (zx + zy);
			const f32 overlen = bc_sqrtf(st->over[0] * st->over[0] + st->over[1] * st->over[1]);
			f32 k = 1.f;

			if (ec->edgesmoothing > 0.001f && overlen > 0.f) {
				const f32 pressure = overlen / softcap;
				const f32 rate = (1.f / ec->edgesmoothing) * (1.f + pressure * pressure);
				k = 1.f - bc_expf(-rate * dtsec);
			}

			for (s32 i = 0; i < 2; i++) {
				const f32 c = st->over[i] * k;
				st->over[i] -= c;
				camdeg[i] += c;
				st->display[i] -= c * (1.f + drag);
			}

			// finish off tiny leftovers so catch-up and lead can take over again
			if (bc_fabsf(st->over[0]) + bc_fabsf(st->over[1]) < 0.01f) {
				for (s32 i = 0; i < 2; i++) {
					camdeg[i] += st->over[i];
					st->display[i] -= st->over[i] * (1.f + drag);
					st->over[i] = 0.f;
				}
			}

			// never let the gun run away past the edge
			if (overlen > softcap * 3.f) {
				const f32 keep = softcap * 3.f / overlen;

				for (s32 i = 0; i < 2; i++) {
					const f32 excess = st->over[i] * (1.f - keep);
					st->over[i] -= excess;
					camdeg[i] += excess;
					st->display[i] -= excess * (1.f + drag);
				}
			}
		}

		// 4. The camera drifts toward where the gun points, so it never sits dead still.
		//    While aiming it leads gently (stronger near the edge of the zone), and
		//    after a moment idle it catches up. The gun stays on the same spot in the
		//    world while the camera moves toward it.
		if (reqlen > 0.0001f) {
			st->idletime = 0.f;
		} else {
			st->idletime += dtsec;
		}

		{
			const f32 edgeness = bc_sqrtf(clampf((st->target[0] / zx) * (st->target[0] / zx)
						+ (st->target[1] / zy) * (st->target[1] / zy), 0.f, 1.f));
			f32 rate = 0.f;

			if (st->over[0] != 0.f || st->over[1] != 0.f) {
				// already turning from the edge; leading as well would slow that turn down
				rate = 0.f;
			} else if (st->idletime > ec->recenterdelay) {
				rate = ec->recenterspeed;
			} else if (reqlen > 0.0001f || st->idletime > 0.f) {
				rate = ec->cameralead * edgeness;
			}

			if (rate > 0.f) {
				const f32 k = 1.f - bc_expf(-rate * dtsec);
				const f32 ry = st->target[0] * k;
				const f32 rp = st->target[1] * k;

				st->target[0] -= ry;
				st->target[1] -= rp;
				st->display[0] -= ry;
				st->display[1] -= rp;
				camdeg[0] += ry;
				camdeg[1] += rp;
			}
		}

		// 4b. Raising the gun to your eye: the camera swings to where the gun
		//     points, so aiming down sights keeps your aim instead of jumping
		if (ads > 0.f) {
			const f32 rate = ads * 5.f / (cfg->adstime > 0.05f ? cfg->adstime : 0.05f);
			const f32 k = 1.f - bc_expf(-rate * dtsec);

			for (s32 i = 0; i < 2; i++) {
				const f32 r = st->target[i] * k;
				st->target[i] -= r;
				st->display[i] -= r;
				camdeg[i] += r;
			}
		}

		// 5. Camera sway: slow breathing drift plus a footstep rhythm while moving.
		//    Applied as a bounded offset (this frame's value minus last frame's) so
		//    the view wobbles around where you point it and never drifts away.
		if (ec->camerasway > 0.f || ec->walksway > 0.f) {
			const struct player *pl = g_Vars.currentplayer;
			const f32 movefrac = clampf(bc_sqrtf(pl->speedforwards * pl->speedforwards
						+ pl->speedsideways * pl->speedsideways), 0.f, 1.f);
			const f32 tau = 2.f * (f32)M_PI;
			f32 sway[2];

			st->swayphase[0] = bc_fmodf(st->swayphase[0] + dtsec * 0.27f * tau, tau);             // breathing
			st->swayphase[1] = bc_fmodf(st->swayphase[1] + dtsec * 0.61f * tau, tau);             // slow drift
			st->swayphase[2] = bc_fmodf(st->swayphase[2] + dtsec * (0.9f + movefrac) * tau, tau); // footsteps

			sway[0] = ec->camerasway * (0.55f * bc_sinf(st->swayphase[1]) + 0.25f * bc_sinf(st->swayphase[0] * 0.5f + 1.3f))
				+ ec->walksway * movefrac * 0.6f * bc_sinf(st->swayphase[2]);
			sway[1] = ec->camerasway * (0.6f * bc_sinf(st->swayphase[0]) + 0.2f * bc_sinf(st->swayphase[1] * 1.7f + 0.7f))
				- ec->walksway * movefrac * 0.5f * bc_fabsf(bc_sinf(st->swayphase[2]));

			for (s32 i = 0; i < 2; i++) {
				const f32 delta = sway[i] - st->sway[i];
				camdeg[i] += delta;
				st->display[i] -= delta * clampf(ec->turndrag, 0.f, 1.f) * 0.5f; // hands lag the body a little
				st->sway[i] = sway[i];
			}
		}

		weightyAimStepSpring(st, ec, dtsec);

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
				g_WeightyAimLogTime, dtsec, cfg->preset, st->active,
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
	return weightyAimCfgEnabled(weightyAimCurCfg()) && weightyAimCurState()->active;
}

bool weightyAimHideCrosshair(void)
{
	return weightyAimCurCfg()->crosshair == WEIGHTYAIM_CROSSHAIR_AIMONLY
		&& weightyAimIsActive()
		&& !g_Vars.currentplayer->insightaimmode;
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

/*
 * Aim down sights
 */

static bool weightyAimIsFirearm(s32 weaponnum);

static bool weightyAimAdsWeapon(s32 weaponnum)
{
	// the Farsight keeps its own auto-seeking aim mode
	return weightyAimIsFirearm(weaponnum) && weaponnum != WEAPON_FARSIGHT;
}

bool weightyAimPrepareMove(struct movedata *movedata)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	struct weightyaimstate *st = weightyAimCurState();

	st->adsheld = weightyAimCfgEnabled(cfg)
		&& cfg->ads
		&& g_Vars.currentplayer->insightaimmode
		&& g_Vars.currentplayer->bondmovemode == MOVEMODE_WALK
		&& g_Vars.tickmode == TICKMODE_NORMAL
		&& weightyAimAdsWeapon(bgunGetWeaponNum(HAND_RIGHT));

	if (st->adsheld) {
		// keep looking around normally instead of the game's aim mode, where
		// the camera freezes and the stick moves the crosshair
		movedata->cannaturalturn = true;
		movedata->cannaturalpitch = true;
		movedata->canswivelgun = true;
		movedata->canmanualaim = false;
		movedata->aimturnleftspeed = 0.f;
		movedata->aimturnrightspeed = 0.f;
		movedata->speedvertaup = 0.f;
		movedata->speedvertadown = 0.f;
	}

	return st->adsheld;
}

f32 weightyAimAdjustZoomFov(f32 zoomfov)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	const struct weightyaimstate *st = weightyAimCurState();
	const f32 ads = weightyAimAdsAmount(st);

	// only zoom guns without their own scope zoom
	if (!weightyAimCfgEnabled(cfg) || !cfg->ads || ads <= 0.f || zoomfov < PLAYER_DEFAULT_FOV - 0.01f) {
		return zoomfov;
	}

	return zoomfov / (1.f + (clampf(cfg->adszoom, 1.f, 3.f) - 1.f) * ads);
}

/**
 * Line the gun up with the centre of the screen while aiming down sights.
 *
 * Instead of hand-tuning every weapon, this measures where the gun's barrel
 * ended up relative to the gun's origin last frame, and moves the gun so the
 * barrel sits just below the centre of the screen, where the sights are.
 */
void weightyAimAdjustGunPos(struct hand *hand, s32 handnum, struct coord *pos)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	struct weightyaimstate *st = weightyAimCurState();
	const f32 ads = weightyAimAdsAmount(st);
	const f32 gunzoom = currentPlayerGetGunZoomFov();

	if (handnum != HAND_RIGHT) {
		return;
	}

	// scoped weapons keep their scope view; dual wielding keeps both guns apart
	if (ads > 0.f && !(gunzoom > 0.f && gunzoom < PLAYER_DEFAULT_FOV - 0.01f)
			&& !g_Vars.currentplayer->hands[HAND_LEFT].inuse) {
		f32 rel[3];
		f32 target[2];
		f32 depth;
		bool relok = false;

		if (st->gunposvalid) {
			rel[0] = hand->muzzlemat.m[3][0] - st->gunpos[0];
			rel[1] = hand->muzzlemat.m[3][1] - st->gunpos[1];
			rel[2] = hand->muzzlemat.m[3][2] - st->gunpos[2];
			relok = bc_fabsf(rel[0]) < 60.f && bc_fabsf(rel[1]) < 60.f && bc_fabsf(rel[2]) < 120.f;
		}

		if (!relok) {
			// no barrel to measure: a reasonable guess for a pistol-sized gun
			rel[0] = 0.f;
			rel[1] = 3.f;
			rel[2] = -12.f;
		}

		// the barrel sits a little below the line of sight
		depth = -(pos->z + rel[2]);

		if (depth < 5.f) {
			depth = 5.f;
		}

		target[0] = -rel[0];
		target[1] = -rel[1] - depth * 0.021f + cfg->adsheight;

		pos->x += (target[0] - pos->x) * ads;
		pos->y += (target[1] - pos->y) * ads;
	}

	st->gunpos[0] = pos->x;
	st->gunpos[1] = pos->y;
	st->gunpos[2] = pos->z;
	st->gunposvalid = true;
}

/*
 * RE4-style laser sight
 */

bool weightyAimLaserEnhanced(void)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	return weightyAimCfgEnabled(cfg) && cfg->laser;
}

static bool weightyAimIsFirearm(s32 weaponnum)
{
	return (weaponnum >= WEAPON_FALCON2 && weaponnum <= WEAPON_SLAYER)
		|| (weaponnum >= WEAPON_CROSSBOW && weaponnum <= WEAPON_LASER)
		|| (weaponnum >= WEAPON_PP9I && weaponnum <= WEAPON_PSYCHOSISGUN);
}

bool weightyAimLaserWanted(struct hand *hand, s32 handnum, s32 weaponnum)
{
	return handnum == HAND_RIGHT
		&& PLAYERCOUNT() == 1 && IS8MB() // the game only draws laser sights in single player
		&& hand->visible
		&& hand->animmode != HANDANIMMODE_BUSY // hide it while reloading or switching
		&& weightyAimIsFirearm(weaponnum)
		&& weightyAimLaserEnhanced();
}

/**
 * Beam from the gun's muzzle to where the shot would land, with a dot there.
 * The game traces the aim every frame in single player (hand->dotpos).
 */
void weightyAimUpdateLaser(struct hand *hand, s32 handnum)
{
	struct coord beamnear, beamfar;

	beamnear.x = hand->muzzlepos.x;
	beamnear.y = hand->muzzlepos.y;
	beamnear.z = hand->muzzlepos.z;

	if (hand->hasdotinfo) {
		beamfar.x = hand->dotpos.x;
		beamfar.y = hand->dotpos.y;
		beamfar.z = hand->dotpos.z;
	} else {
		// nothing in range: aim far along the crosshair direction
		cam0f0b4c3c(g_Vars.currentplayer->crosspos, &beamfar, 1);
		beamfar.x *= 5000.0f;
		beamfar.y *= 5000.0f;
		beamfar.z *= 5000.0f;
		mtx4TransformVecInPlace(camGetProjectionMtxF(), &beamfar);
	}

	lasersightSetBeam(handnum, 1, &beamnear, &beamfar);

	if (hand->hasdotinfo) {
		struct coord dotpos = hand->dotpos;
		struct coord dotrot = hand->dotrot;
		lasersightSetDot(handnum, &dotpos, &dotrot);
	}
}

PD_CONSTRUCTOR static void weightyAimConfigInit(void)
{
	for (s32 j = 0; j < MAX_PLAYERS; ++j) {
		const s32 i = j + 1;
		weightyAimResetDefaults(j);
		g_WeightyAimStickCfg[j] = g_WeightyAimStickDefaults;
		configRegisterInt(strFmt("WeightyAim.Player%d.Preset", i), &g_WeightyAimCfg[j].preset, 0, WEIGHTYAIM_NUM_PRESETS - 1);
		configRegisterFloat(strFmt("WeightyAim.Player%d.DeadzoneX", i), &g_WeightyAimCfg[j].deadzonex, 0.f, 45.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.DeadzoneY", i), &g_WeightyAimCfg[j].deadzoney, 0.f, 45.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.CameraShare", i), &g_WeightyAimCfg[j].camerashare, 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.CameraLead", i), &g_WeightyAimCfg[j].cameralead, 0.f, 10.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.RecenterSpeed", i), &g_WeightyAimCfg[j].recenterspeed, 0.f, 10.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.RecenterDelay", i), &g_WeightyAimCfg[j].recenterdelay, 0.f, 5.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GunResponse", i), &g_WeightyAimCfg[j].gunresponse, 0.5f, 40.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GunDamping", i), &g_WeightyAimCfg[j].gundamping, 0.05f, 3.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.TurnDrag", i), &g_WeightyAimCfg[j].turndrag, 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.EdgeSmoothing", i), &g_WeightyAimCfg[j].edgesmoothing, 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.CameraSway", i), &g_WeightyAimCfg[j].camerasway, 0.f, 5.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.WalkSway", i), &g_WeightyAimCfg[j].walksway, 0.f, 5.f);
		configRegisterInt(strFmt("WeightyAim.Player%d.Crosshair", i), &g_WeightyAimCfg[j].crosshair, 0, 1);
		configRegisterInt(strFmt("WeightyAim.Player%d.LaserSight", i), &g_WeightyAimCfg[j].laser, 0, 1);
		configRegisterInt(strFmt("WeightyAim.Player%d.AimDownSights", i), &g_WeightyAimCfg[j].ads, 0, 1);
		configRegisterFloat(strFmt("WeightyAim.Player%d.AdsZoom", i), &g_WeightyAimCfg[j].adszoom, 1.f, 3.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.AdsTime", i), &g_WeightyAimCfg[j].adstime, 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.AdsSway", i), &g_WeightyAimCfg[j].adssway, 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.AdsZone", i), &g_WeightyAimCfg[j].adszone, 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.AdsHeight", i), &g_WeightyAimCfg[j].adsheight, -10.f, 10.f);
		configRegisterInt(strFmt("WeightyAim.Player%d.StickCurve", i), &g_WeightyAimStickCfg[j].curve, 0, WEIGHTYAIM_NUM_CURVES - 1);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickInnerDeadzone", i), &g_WeightyAimStickCfg[j].innerdeadzone, 0.f, 0.9f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickOuterDeadzone", i), &g_WeightyAimStickCfg[j].outerdeadzone, 0.1f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickBezierX1", i), &g_WeightyAimStickCfg[j].bezier[0], 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickBezierY1", i), &g_WeightyAimStickCfg[j].bezier[1], 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickBezierX2", i), &g_WeightyAimStickCfg[j].bezier[2], 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickBezierY2", i), &g_WeightyAimStickCfg[j].bezier[3], 0.f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickTurnSpeed", i), &g_WeightyAimStickCfg[j].turnspeed, 0.1f, 3.f);
		configRegisterInt(strFmt("WeightyAim.Player%d.BoostMode", i), &g_WeightyAimStickCfg[j].boostmode, 0, WEIGHTYAIM_NUM_BOOSTS - 1);
		configRegisterFloat(strFmt("WeightyAim.Player%d.BoostAmount", i), &g_WeightyAimStickCfg[j].boostamount, 1.f, 4.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.BoostThreshold", i), &g_WeightyAimStickCfg[j].boostthreshold, 0.3f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.BoostDelay", i), &g_WeightyAimStickCfg[j].boostdelay, 0.f, 2.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.BoostRampTime", i), &g_WeightyAimStickCfg[j].boosttime, 0.f, 2.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.BoostVertical", i), &g_WeightyAimStickCfg[j].boostvertical, 0.f, 1.f);
	}

	configRegisterInt("WeightyAim.Debug.Log", &g_WeightyAimDebugLog, 0, 1);
	configRegisterInt("WeightyAim.Debug.TestPattern", &g_WeightyAimDebugPattern, 0, 1);
}
