#include <stddef.h>
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
#include "game/bondmove.h"
#include "game/options.h"
#include "game/prop.h"
#include "input.h"
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
#define bc_atanf __builtin_atanf

#define DEG_PER_SPEED_TICK 3.5f
#define SPRING_STEP (1.f / 240.f)

struct weightyaimcfg g_WeightyAimCfg[4];
struct weightyaimcfg g_WeightyAimCustomCfg[4][3];
f32 g_WeightyAimAssistStrength[4];
struct weightyaimgyrocfg g_WeightyAimGyroCfg[4];

const char *g_WeightyAimGyroModeNames[WEIGHTYAIM_NUM_GYROMODES] = {
	"Off",
	"Always On",
	"While Aiming",
};

const char *g_WeightyAimGyroSpaceNames[WEIGHTYAIM_NUM_GYROSPACES] = {
	"Player",
	"Local",
};

static const struct weightyaimgyrocfg g_WeightyAimGyroDefaults = {
	.mode = WEIGHTYAIM_GYRO_OFF,
	.sensitivity = 2.5f,
	.vertical = 0.9f,
	.space = WEIGHTYAIM_GYROSPACE_PLAYER,
	.inverty = 0,
	.acceleration = 1.f,
	.accelthreshold = 75.f,
	.tightening = 0.f,
	.smoothing = 4.f,
	.autocalibrate = 1,
	.pausewithstick = 0,
};

void weightyAimResetGyroDefaults(s32 cfgindex)
{
	const s32 mode = g_WeightyAimGyroCfg[cfgindex & 3].mode;
	g_WeightyAimGyroCfg[cfgindex & 3] = g_WeightyAimGyroDefaults;
	g_WeightyAimGyroCfg[cfgindex & 3].mode = mode; // resetting the tuning shouldn't switch gyro off
}

/*
 * Gyro state, per player
 */
struct weightyaimgyrostate {
	bool available;      // the controller reported a gyro last time we asked
	f32 bias[3];         // calibration offset, deg/s
	bool calibrated;
	f32 calibtime;       // > 0 while calibrating
	f32 calibsum[3];
	s32 calibcount;
	f32 stilltime;       // how long the controller has been held still (auto-calibration)
	f32 grav[3];         // smoothed "up" direction from the accelerometer
	f32 smooth[2];       // smoothed small movements
	u64 lastmenutick;
};

static struct weightyaimgyrostate g_WeightyAimGyroState[MAX_PLAYERS];
s32 g_WeightyAimLastCustom[4];
struct weightyaimstickcfg g_WeightyAimStickCfg[4];

const char *g_WeightyAimCurveNames[WEIGHTYAIM_NUM_CURVES] = {
	"Original",
	"Linear",
	"Balanced",
	"Custom 1",
	"Custom 2",
	"Custom 3",
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
	.bezierprofile = {
		{ 0.4f, 0.0f, 0.75f, 1.0f },
		{ 0.4f, 0.0f, 0.75f, 1.0f },
		{ 0.4f, 0.0f, 0.75f, 1.0f },
	},
	.lastcustomcurve = 0,
	.turnspeed = 1.f,
	.verticalsens = 0.85f,
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
	sc->turnspeed = d->turnspeed;
	sc->verticalsens = d->verticalsens;
	// custom curve profiles are kept; resetting shouldn't wipe your curves
}

/**
 * Pick a look curve. Choosing Custom 1-3 loads that profile's points.
 */
void weightyAimSelectCurve(s32 cfgindex, s32 curve)
{
	struct weightyaimstickcfg *sc = &g_WeightyAimStickCfg[cfgindex & 3];

	sc->curve = curve;

	if (WEIGHTYAIM_IS_CUSTOM_CURVE(curve)) {
		const s32 slot = curve - WEIGHTYAIM_CURVE_CUSTOM1;
		memcpy(sc->bezier, sc->bezierprofile[slot], sizeof(sc->bezier));
		sc->lastcustomcurve = slot;
	}
}

/**
 * A curve point slider moved: make sure a custom curve is selected (the one
 * last used if a built-in curve was on) and save the points into it.
 */
void weightyAimCurvePointsChanged(s32 cfgindex)
{
	struct weightyaimstickcfg *sc = &g_WeightyAimStickCfg[cfgindex & 3];
	s32 slot;

	if (!WEIGHTYAIM_IS_CUSTOM_CURVE(sc->curve)) {
		sc->curve = WEIGHTYAIM_CURVE_CUSTOM1 + (sc->lastcustomcurve % 3);
	}

	slot = sc->curve - WEIGHTYAIM_CURVE_CUSTOM1;
	memcpy(sc->bezierprofile[slot], sc->bezier, sizeof(sc->bezier));
	sc->lastcustomcurve = slot;
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
	bool aiming;     // aim button held and Weighty Aim drives the look (Modern Classic / Mobile), this frame
	bool adsheld;    // aim button held with aim down sights on (any aim mode), this frame
	bool held;       // aim button held on a gun Weighty Aim applies to (any aim mode), this frame
	f32 adsblend;    // 0 = hip, 1 = fully aiming (linear, see weightyAimAdsAmount)
	f32 gunpos[3];   // where the gun model was placed last frame (camera space)
	bool assistlock; // the game's auto-aim has a target this frame and may pull the crosshair
	f32 assist[2];   // extra gun offset from auto-aim, degrees
	bool gunposvalid;
	f32 swayphase[3];// camera sway oscillator phases (breath, drift, footsteps)
	f32 sway[2];     // camera sway offset applied last frame, degrees
	bool active;     // Weighty Aim drove this player's crosshair on the last update
	f32 barrelbias[3]; // laser: how far the barrel normally points off the aim line (camera space)
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
	"Custom 1",
	"Custom 2",
	"Custom 3",
};

// Weighty: free-aim with a crosshair, a gun with some heft, a hint of sway.
// Leans a little toward Boring: a steadier, stiffer gun for gameplay, while
// keeping the free-aim crosshair that isn't locked to the centre.
static const struct weightyaimcfg g_WeightyAimPresetWeighty = {
	.preset = WEIGHTYAIM_PRESET_WEIGHTY,
	.deadzonex = 7.5f,
	.deadzoney = 4.5f,
	.camerashare = 0.4f,
	.cameralead = 0.8f,
	.recenterspeed = 1.f,
	.recenterdelay = 0.25f,
	.recentersmooth = 0.4f,
	.gunresponse = 8.5f,
	.gundamping = 0.7f,
	.turndrag = 0.35f,
	.edgesmoothing = 0.1f,
	.camerasway = 0.08f,
	.walksway = 0.25f,
	.crosshair = WEIGHTYAIM_CROSSHAIR_ALWAYS,
	.laser = 0,
	.laserdot = 0,
	.laserpersist = 1,
	.aimmode = WEIGHTYAIM_AIMMODE_MODERN,
	.aimdpadmove = 1,
	.aimcrosshair = 1,
	.aimlaserdot = 0,
	.aimlaserbeam = 0,
	.ads = 1,
	.adszoom = 1.3f,
	.adstime = 0.18f,
	.adssway = 0.35f,
	.adszone = 1.f,
	.adsheight = -4.f,
	.adsmovespeed = 0.6f,
	.adssens = 0.65f,
};

// Immersive: wide zone, heavy flowing gun, a camera that follows and never sits still
// (the gun itself is kept a little steadier so it isn't too shaky)
static const struct weightyaimcfg g_WeightyAimPresetImmersive = {
	.preset = WEIGHTYAIM_PRESET_IMMERSIVE,
	.deadzonex = 11.f,
	.deadzoney = 6.5f,
	.camerashare = 0.45f,
	.cameralead = 1.4f,
	.recenterspeed = 1.3f,
	.recenterdelay = 0.15f,
	.recentersmooth = 0.6f,
	.gunresponse = 4.6f,
	.gundamping = 0.52f,
	.turndrag = 0.6f,
	.edgesmoothing = 0.2f,
	.camerasway = 0.32f,
	.walksway = 0.8f,
	.crosshair = WEIGHTYAIM_CROSSHAIR_AIMONLY,
	.laser = 1,
	.laserdot = 1,
	.laserpersist = 1,
	.aimmode = WEIGHTYAIM_AIMMODE_MOBILE,
	.aimdpadmove = 1,
	.aimcrosshair = 0,
	.aimlaserdot = 1,
	.aimlaserbeam = 1,
	.ads = 1,
	.adszoom = 1.25f,
	.adstime = 0.26f,
	.adssway = 0.5f,
	.adszone = 1.f,
	.adsheight = -4.f,
	.adsmovespeed = 0.55f,
	.adssens = 0.65f,
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
	.recentersmooth = 0.f,
	.gunresponse = 20.f,
	.gundamping = 1.f,
	.turndrag = 0.f,
	.edgesmoothing = 0.f,
	.camerasway = 0.f,
	.walksway = 0.f,
	.crosshair = WEIGHTYAIM_CROSSHAIR_ALWAYS,
	.laser = 0,
	.laserdot = 0,
	.laserpersist = 1,
	.aimmode = WEIGHTYAIM_AIMMODE_MOBILE,
	.aimdpadmove = 1,
	.aimcrosshair = 1,
	.aimlaserdot = 0,
	.aimlaserbeam = 0,
	.ads = 1,
	.adszoom = 1.35f,
	.adstime = 0.12f,
	.adssway = 0.f,
	.adszone = 1.f,
	.adsheight = -4.f,
	.adsmovespeed = 0.65f,
	.adssens = 0.65f,
};

void weightyAimApplyPreset(s32 cfgindex, s32 preset)
{
	const s32 idx = cfgindex & 3;
	struct weightyaimcfg *cfg = &g_WeightyAimCfg[idx];

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
		// keep the current values; Classic just switches the mod off
		cfg->preset = preset;
		break;
	default:
		if (WEIGHTYAIM_IS_CUSTOM(preset) && preset < WEIGHTYAIM_NUM_PRESETS) {
			const s32 slot = preset - WEIGHTYAIM_PRESET_CUSTOM1;
			*cfg = g_WeightyAimCustomCfg[idx][slot];
			cfg->preset = preset;
			g_WeightyAimLastCustom[idx] = slot;
		}
		break;
	}
}

/**
 * Something on the aim pages changed by hand. On a custom profile, save it
 * there. On a built-in preset (or Classic), carry the current values over to
 * the last custom profile you used, switch to it and save the change there.
 */
void weightyAimAimSettingsChanged(s32 cfgindex)
{
	const s32 idx = cfgindex & 3;
	struct weightyaimcfg *cfg = &g_WeightyAimCfg[idx];
	s32 slot;

	if (!WEIGHTYAIM_IS_CUSTOM(cfg->preset)) {
		cfg->preset = WEIGHTYAIM_PRESET_CUSTOM1 + (g_WeightyAimLastCustom[idx] % 3);
	}

	slot = cfg->preset - WEIGHTYAIM_PRESET_CUSTOM1;
	g_WeightyAimCustomCfg[idx][slot] = *cfg;
	g_WeightyAimLastCustom[idx] = slot;
}

void weightyAimResetDefaults(s32 cfgindex)
{
	weightyAimApplyPreset(cfgindex, WEIGHTYAIM_PRESET_WEIGHTY);
}

void weightyAimInit(void)
{
	for (s32 j = 0; j < MAX_PLAYERS; j++) {
		const s32 preset = g_WeightyAimCfg[j].preset;

		// pd.ini keeps the live values of whichever preset was on, so a retuned
		// built-in preset would otherwise never reach existing players
		if (preset == WEIGHTYAIM_PRESET_WEIGHTY
				|| preset == WEIGHTYAIM_PRESET_IMMERSIVE
				|| preset == WEIGHTYAIM_PRESET_BORING) {
			weightyAimApplyPreset(j, preset);
		}
	}
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
		// the game's own response, but sensitivity still applies
		out[0] = weightyAimStickCurve(turn) * clampf(sc->turnspeed, 0.1f, 3.f);
		out[1] = weightyAimStickCurve(pitch) * clampf(sc->turnspeed, 0.1f, 3.f) * clampf(sc->verticalsens, 0.1f, 3.f);
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
	case WEIGHTYAIM_CURVE_CUSTOM1:
	case WEIGHTYAIM_CURVE_CUSTOM2:
	case WEIGHTYAIM_CURVE_CUSTOM3:
		o = weightyAimBezier(sc->bezier, n);
		break;
	case WEIGHTYAIM_CURVE_BALANCED:
	default:
		o = n * bc_sqrtf(n); // n^1.5, halfway between linear and squared
		break;
	}

	o *= clampf(sc->turnspeed, 0.1f, 3.f);
	out[0] = rx / mag * o;
	out[1] = ry / mag * o * clampf(sc->verticalsens, 0.1f, 3.f);
	weightyAimApplyBoost(sc, st, n, dtsec, out);
}

/**
 * The curve on its own (no boost, no state): stick pushed straight right.
 */
f32 weightyAimCurveOutput(const struct weightyaimstickcfg *sc, f32 deflection)
{
	f32 n, o, lo, hi;
	s32 analog;

	deflection = clampf(deflection, 0.f, 1.f);

	if (sc->curve == WEIGHTYAIM_CURVE_ORIGINAL) {
		// the game's response, including its small 5-unit safe zone
		analog = (s32)(deflection * 127.f + 0.5f) - 5;
		return analog > 0 ? weightyAimStickCurve(analog) * clampf(sc->turnspeed, 0.1f, 3.f) : 0.f;
	}

	lo = clampf(sc->innerdeadzone, 0.f, 0.9f);
	hi = clampf(sc->outerdeadzone, lo + 0.05f, 1.f);
	n = clampf((deflection - lo) / (hi - lo), 0.f, 1.f);

	switch (sc->curve) {
	case WEIGHTYAIM_CURVE_LINEAR:
		o = n;
		break;
	case WEIGHTYAIM_CURVE_CUSTOM1:
	case WEIGHTYAIM_CURVE_CUSTOM2:
	case WEIGHTYAIM_CURVE_CUSTOM3:
		o = weightyAimBezier(sc->bezier, n);
		break;
	case WEIGHTYAIM_CURVE_BALANCED:
	default:
		o = n * bc_sqrtf(n);
		break;
	}

	return o * clampf(sc->turnspeed, 0.1f, 3.f);
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
			const f32 acc = k * (st->target[i] + st->over[i] + st->assist[i] - st->display[i]) - c * st->vel[i];
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

static void weightyAimGyroDegrees(s32 cfgindex, bool aiming, bool stickmoving, f32 dtsec, f32 *out);

static void weightyAimResetState(struct weightyaimstate *st)
{
	st->target[0] = st->target[1] = 0.f;
	st->display[0] = st->display[1] = 0.f;
	st->vel[0] = st->vel[1] = 0.f;
	st->over[0] = st->over[1] = 0.f;
	st->assist[0] = st->assist[1] = 0.f;
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
	f32 gyrodeg[2] = { 0.f, 0.f };
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
	// Gyro counts like the mouse: it goes through the free-aim zone too
	{
		const s32 cfgindex = g_Vars.currentplayerstats->mpindex & 3;
		const bool aiming = g_Vars.currentplayer->insightaimmode != 0;
		const bool stickmoving = bc_fabsf(stickrate[0]) + bc_fabsf(stickrate[1]) > 0.02f;

		weightyAimGyroDegrees(cfgindex, aiming, stickmoving, dtsec, gyrodeg);

		if (!canlook) {
			gyrodeg[0] = gyrodeg[1] = 0.f;
		}

		mousedeg[0] += gyrodeg[0];
		mousedeg[1] += gyrodeg[1];
	}

	reqdeg[0] = stickdeg[0] + mousedeg[0];
	reqdeg[1] = stickdeg[1] + mousedeg[1];

	// Ease into and out of aiming (and bring the gun up, with aim down sights)
	{
		const f32 goal = st->held ? 1.f : 0.f;
		const f32 step = dtsec / (cfg->adstime > 0.01f ? cfg->adstime : 0.01f);

		if (st->adsblend < goal) {
			st->adsblend = clampf(st->adsblend + step, 0.f, goal);
		} else if (st->adsblend > goal) {
			st->adsblend = clampf(st->adsblend - step, goal, 1.f);
		}
	}

	// Lower sensitivity while aiming (stick, mouse and gyro alike), eased in.
	// Classic aim mode uses the game's own aiming, so it's left alone there.
	if (weightyAimCfgEnabled(cfg) && cfg->aimmode != WEIGHTYAIM_AIMMODE_CLASSIC) {
		const f32 mult = 1.f + (clampf(cfg->adssens, 0.1f, 1.f) - 1.f) * weightyAimAdsAmount(st);

		reqdeg[0] *= mult;
		reqdeg[1] *= mult;
	}

	active = weightyAimCfgEnabled(cfg)
		&& canlook
		&& g_Vars.currentplayer->bondmovemode == MOVEMODE_WALK
		&& (!g_Vars.currentplayer->insightaimmode || st->aiming)
		&& !g_Vars.currentplayer->isdead
		&& g_Vars.tickmode == TICKMODE_NORMAL;

	if (!active) {
		weightyAimResetState(st);
		st->active = false;

		// Aim mod off (Classic) but a custom stick response chosen (a different
		// curve, turn boost, or a sensitivity other than 1x): still apply it.
		// With analog zeroed, PD computes speed = (freelook * mlookscale) * fovscale.
		if (canlook && (sc->curve != WEIGHTYAIM_CURVE_ORIGINAL || st->boostlevel > 0.f
					|| bc_fabsf(sc->turnspeed - 1.f) > 0.001f || bc_fabsf(sc->verticalsens - 1.f) > 0.001f)) {
			*analogturn = 0;
			*analogpitch = 0;
			*freelookdx += stickrate[0] / mlookscale;
			*freelookdy += stickrate[1] / mlookscale;
		}

		// gyro works with the Classic preset too
		if (canlook && (gyrodeg[0] != 0.f || gyrodeg[1] != 0.f)) {
			*freelookdx += gyrodeg[0] / (DEG_PER_SPEED_TICK * dt60 * mlookscale * fovscale);
			*freelookdy -= gyrodeg[1] / (DEG_PER_SPEED_TICK * dt60 * mlookscale * fovscale);
		}
	} else {
		// While aiming the zone shrinks, the camera takes over more of the turning,
		// sway calms down and the gun steadies. Aim Feel While Aiming (adszone)
		// sets how much of the hip-fire feel is kept: at 1 aiming feels the same.
		const f32 ads = cfg->aimmode != WEIGHTYAIM_AIMMODE_CLASSIC ? weightyAimAdsAmount(st) : 0.f;
		const f32 align = clampf(cfg->adszone, 0.f, 1.f);
		struct weightyaimcfg effcfg = *cfg;
		struct weightyaimcfg *ec = &effcfg;

		ec->deadzonex *= 1.f + (clampf(cfg->adszone, 0.f, 1.f) - 1.f) * ads;
		ec->deadzoney *= 1.f + (clampf(cfg->adszone, 0.f, 1.f) - 1.f) * ads;
		ec->camerashare += (1.f - ec->camerashare) * ads * (1.f - clampf(cfg->adszone, 0.f, 1.f));
		ec->camerasway *= 1.f + (clampf(cfg->adssway, 0.f, 1.f) - 1.f) * ads;
		ec->walksway *= 1.f + (clampf(cfg->adssway, 0.f, 1.f) - 1.f) * ads;
		ec->turndrag *= 1.f - 0.6f * ads * (1.f - align);
		ec->gunresponse *= 1.f + 0.8f * ads * (1.f - align);

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
				// catch-up eases in instead of kicking in at full speed
				rate = ec->recenterspeed;

				if (ec->recentersmooth > 0.001f) {
					f32 e = clampf((st->idletime - ec->recenterdelay) / ec->recentersmooth, 0.f, 1.f);
					rate *= e * e * (3.f - 2.f * e);
				}
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

		// 4b. Starting to aim: the camera swings toward where the gun points, so
		//     your aim doesn't jump. The more aim feel is kept, the less it pulls
		//     the crosshair to the centre (none at all at 100%).
		if (ads > 0.f && align < 0.999f) {
			const f32 rate = ads * (1.f - align) * 5.f / (cfg->adstime > 0.05f ? cfg->adstime : 0.05f);
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

		// 6. Aim assist: the game's auto-aim moves the crosshair (not the camera)
		//    onto the target it picked, just like it does without the mod, so
		//    it's never stronger than the game and difficulty allow
		{
			f32 want[2] = { 0.f, 0.f };

			if (st->assistlock) {
				const f32 tanhalfy = bc_tanf(DEG2RAD(viGetFovY() * 0.5f));
				const f32 tanhalfx = tanhalfy * (viGetAspect() > 0.f ? viGetAspect() : 1.333f);
				const f32 tx = clampf(g_Vars.currentplayer->autoaimx, -1.f, 1.f);
				const f32 ty = clampf(g_Vars.currentplayer->autoaimy, -1.f, 1.f);
				const f32 targetyaw = RAD2DEG(bc_atanf(tx * tanhalfx));
				const f32 targetpitch = -RAD2DEG(bc_atanf(ty * tanhalfy));

				want[0] = targetyaw - (st->target[0] + st->over[0]);
				want[1] = targetpitch - (st->target[1] + st->over[1]);
			}

			// eases on and off at about the speed the game's own auto-aim moves
			const f32 k = 1.f - bc_expf(-dtsec / 0.35f);
			st->assist[0] += (want[0] - st->assist[0]) * k;
			st->assist[1] += (want[1] - st->assist[1]) * k;
		}

		weightyAimStepSpring(st, ec, dtsec);

		// Never let the gun trail your aim by more than a little: gun weight and
		// turn drag should be felt, but on a fast spin the lag otherwise grows
		// with turn speed and the crosshair falls well behind where you aim.
		{
			const f32 maxlag = 0.35f * (zx < zy ? zx : zy) + 1.5f;
			f32 lag[2], laglen;

			for (s32 i = 0; i < 2; i++) {
				lag[i] = st->display[i] - (st->target[i] + st->over[i] + st->assist[i]);
			}

			laglen = bc_sqrtf(lag[0] * lag[0] + lag[1] * lag[1]);

			if (laglen > maxlag) {
				const f32 keep = maxlag / laglen;

				for (s32 i = 0; i < 2; i++) {
					st->display[i] -= lag[i] * (1.f - keep);
					st->vel[i] *= keep;
				}
			}
		}

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

/**
 * Holding aim with Weighty Aim on (any aim mode, any gun it applies to).
 */
static bool weightyAimHoldingAim(const struct weightyaimcfg *cfg)
{
	return weightyAimCfgEnabled(cfg) && g_Vars.currentplayer->insightaimmode;
}

bool weightyAimHideCrosshair(void)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();

	if (!weightyAimCfgEnabled(cfg)) {
		return false;
	}

	// aiming: Crosshair While Aiming
	if (weightyAimHoldingAim(cfg)) {
		return !cfg->aimcrosshair;
	}

	// hip-fire: Crosshair
	return cfg->crosshair == WEIGHTYAIM_CROSSHAIR_AIMONLY && weightyAimIsActive();
}

bool weightyAimForceCrosshair(void)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	bool wanted;

	if (!weightyAimCfgEnabled(cfg)
			|| g_Vars.tickmode != TICKMODE_NORMAL
			|| g_Vars.currentplayer->bondmovemode != MOVEMODE_WALK
			|| g_Vars.currentplayer->isdead
			|| weightyAimHideCrosshair()) {
		return false;
	}

	// The hip-fire crosshair is the game's "target", which it only draws with its
	// own Always Show Target option on, and the aiming crosshair only while
	// every sight flag is clear (taking damage clears it for a moment, for one).
	// When the setting says on, it's on.
	wanted = weightyAimHoldingAim(cfg) ? cfg->aimcrosshair : cfg->crosshair == WEIGHTYAIM_CROSSHAIR_ALWAYS;

	return wanted;
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
 * Aim assist limits
 */

bool weightyAimAssistAllowed(void)
{
	return g_WeightyAimAssistStrength[g_Vars.currentplayerstats->mpindex & 3] > 0.001f;
}

f32 weightyAimAssistScale(void)
{
	// capped at 1: can weaken the game's aim assist, never strengthen it
	return clampf(g_WeightyAimAssistStrength[g_Vars.currentplayerstats->mpindex & 3], 0.f, 1.f);
}

/*
 * Gyro aiming
 *
 * Follows JibbSmart's GyroWiki (and what Steam Input does):
 * - sensitivity 1 = 1 real degree turns you 1 in-game degree
 * - "player space": turning the controller flat or leaning it left/right both
 *   turn you, whichever way you naturally hold it; up/down is always pitch
 * - optional acceleration, tightening and soft tiered smoothing, all applied
 *   only where the GyroWiki says they belong (smoothing only on small moves)
 * - calibration on demand, plus automatic calibration while held still
 */

#define GYRO_CALIB_SECONDS 1.5f
#define GYRO_STILL_DEG     2.0f   // deg/s: slower than this counts as "held still"
#define GYRO_YAW_RELAX     1.41f  // player space: ~45 degrees of freedom

static inline struct weightyaimgyrostate *weightyAimGyroState(s32 cfgindex)
{
	return &g_WeightyAimGyroState[cfgindex & 3];
}

void weightyAimGyroStartCalibration(s32 cfgindex)
{
	struct weightyaimgyrostate *gs = weightyAimGyroState(cfgindex);

	gs->calibtime = GYRO_CALIB_SECONDS;
	gs->calibsum[0] = gs->calibsum[1] = gs->calibsum[2] = 0.f;
	gs->calibcount = 0;
}

/**
 * Read the controller and update calibration. Returns false without a gyro.
 * rate[3] gets calibrated angular velocity in deg/s (pitch, yaw, roll).
 */
static bool weightyAimGyroSample(s32 cfgindex, f32 dtsec, f32 *rate)
{
	const struct weightyaimgyrocfg *gc = &g_WeightyAimGyroCfg[cfgindex & 3];
	struct weightyaimgyrostate *gs = weightyAimGyroState(cfgindex);
	const s32 pad = optionsGetContpadNum1(cfgindex & 3);
	f32 gyro[3], accel[3], raw[3], alen;

	gs->available = inputControllerGetMotion(pad, gyro, accel) != 0;

	if (!gs->available) {
		return false;
	}

	for (s32 i = 0; i < 3; i++) {
		raw[i] = gyro[i] * (180.f / (f32)M_PI);
	}

	// smoothed gravity (points up) for player space
	alen = bc_sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);

	if (alen > 0.1f) {
		const f32 k = 1.f - bc_expf(-dtsec / 0.1f);

		for (s32 i = 0; i < 3; i++) {
			gs->grav[i] += (accel[i] / alen - gs->grav[i]) * k;
		}
	}

	// manual calibration: average while the player holds still
	if (gs->calibtime > 0.f) {
		for (s32 i = 0; i < 3; i++) {
			gs->calibsum[i] += raw[i];
		}

		gs->calibcount++;
		gs->calibtime -= dtsec;

		if (gs->calibtime <= 0.f && gs->calibcount > 0) {
			for (s32 i = 0; i < 3; i++) {
				gs->bias[i] = gs->calibsum[i] / gs->calibcount;
			}

			gs->calibrated = true;
		}
	} else if (gc->autocalibrate) {
		// automatic: after a second of being held still, slowly settle the bias
		const f32 dx = raw[0] - gs->bias[0], dy = raw[1] - gs->bias[1], dz = raw[2] - gs->bias[2];

		if (bc_fabsf(dx) < GYRO_STILL_DEG && bc_fabsf(dy) < GYRO_STILL_DEG && bc_fabsf(dz) < GYRO_STILL_DEG) {
			gs->stilltime += dtsec;
		} else {
			gs->stilltime = 0.f;
		}

		if (gs->stilltime > 1.f) {
			const f32 k = 1.f - bc_expf(-dtsec / 1.f);

			for (s32 i = 0; i < 3; i++) {
				gs->bias[i] += (raw[i] - gs->bias[i]) * k;
			}

			gs->calibrated = true;
		}
	}

	for (s32 i = 0; i < 3; i++) {
		rate[i] = raw[i] - gs->bias[i];
	}

	return true;
}

/**
 * Degrees to turn this frame from the gyro (+yaw = right, +pitch = up).
 */
static void weightyAimGyroDegrees(s32 cfgindex, bool aiming, bool stickmoving, f32 dtsec, f32 *out)
{
	const struct weightyaimgyrocfg *gc = &g_WeightyAimGyroCfg[cfgindex & 3];
	struct weightyaimgyrostate *gs = weightyAimGyroState(cfgindex);
	f32 rate[3], yaw, pitch, speed, sens;

	out[0] = out[1] = 0.f;

	if (gc->mode == WEIGHTYAIM_GYRO_OFF || !weightyAimGyroSample(cfgindex, dtsec, rate)) {
		return;
	}

	if (gs->calibtime > 0.f
			|| (gc->mode == WEIGHTYAIM_GYRO_AIMING && !aiming)
			|| (gc->pausewithstick && stickmoving)) {
		gs->smooth[0] = gs->smooth[1] = 0.f;
		return;
	}

	// SDL: x = pitch (+ = tilt the far edge up), y = yaw (+ = turn left), z = roll
	pitch = rate[0];

	if (gc->space == WEIGHTYAIM_GYROSPACE_PLAYER && (gs->grav[0] != 0.f || gs->grav[1] != 0.f || gs->grav[2] != 0.f)) {
		const f32 worldyaw = rate[1] * gs->grav[1] + rate[2] * gs->grav[2];
		const f32 len = bc_sqrtf(rate[1] * rate[1] + rate[2] * rate[2]);
		const f32 mag = bc_fabsf(worldyaw) * GYRO_YAW_RELAX;

		yaw = (worldyaw < 0.f ? -1.f : 1.f) * (mag < len ? mag : len);
	} else {
		yaw = rate[1];
	}

	speed = bc_sqrtf(yaw * yaw + pitch * pitch);

	// tightening: scale down tiny movements (hand shake) without a hard cutoff
	if (gc->tightening > 0.f && speed < gc->tightening) {
		const f32 k = speed / gc->tightening;
		yaw *= k;
		pitch *= k;
	}

	// soft tiered smoothing: only small movements are smoothed, big ones pass straight through
	if (gc->smoothing > 0.f) {
		const f32 half = gc->smoothing * 0.5f;
		const f32 direct = clampf((speed - half) / half, 0.f, 1.f);
		const f32 k = 1.f - bc_expf(-dtsec / 0.1f);

		gs->smooth[0] += (yaw - gs->smooth[0]) * k;
		gs->smooth[1] += (pitch - gs->smooth[1]) * k;
		yaw = yaw * direct + gs->smooth[0] * (1.f - direct);
		pitch = pitch * direct + gs->smooth[1] * (1.f - direct);
	}

	// acceleration: faster motion gets more sensitivity
	sens = gc->sensitivity;

	if (gc->acceleration > 1.f) {
		sens *= 1.f + (gc->acceleration - 1.f) * clampf(speed / (gc->accelthreshold > 1.f ? gc->accelthreshold : 1.f), 0.f, 1.f);
	}

	out[0] = -yaw * sens * dtsec;
	out[1] = pitch * sens * clampf(gc->vertical, 0.f, 2.f) * dtsec * (gc->inverty ? -1.f : 1.f);
}

/**
 * Keeps calibration running while the Gyro Aim page is open (the game is
 * paused then, so the aiming code isn't running).
 */
void weightyAimGyroMenuTick(s32 cfgindex)
{
	struct weightyaimgyrostate *gs = weightyAimGyroState(cfgindex);
	const u64 now = sysGetMicroseconds();
	f32 dt = gs->lastmenutick ? (now - gs->lastmenutick) / 1000000.f : 0.f;
	f32 rate[3];

	gs->lastmenutick = now;

	if (dt <= 0.f || dt > 0.25f) {
		dt = 1.f / 60.f;
	}

	weightyAimGyroSample(cfgindex, dt, rate);
}

const char *weightyAimGyroStatusText(s32 cfgindex)
{
	const struct weightyaimgyrostate *gs = weightyAimGyroState(cfgindex);

	if (!gs->available) {
		return "No gyro found on this controller\n";
	}

	if (gs->calibtime > 0.f) {
		return "Calibrating... keep the controller still\n";
	}

	return gs->calibrated ? "Gyro ready (calibrated)\n" : "Gyro found - calibrate for best results\n";
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

/**
 * Can this player use Weighty Aim's aiming right now (walking, in control,
 * holding a gun that aiming applies to)?
 */
static bool weightyAimCanAim(const struct weightyaimcfg *cfg)
{
	return weightyAimCfgEnabled(cfg)
		&& g_Vars.currentplayer->bondmovemode == MOVEMODE_WALK
		&& g_Vars.tickmode == TICKMODE_NORMAL
		&& weightyAimAdsWeapon(bgunGetWeaponNum(HAND_RIGHT));
}

bool weightyAimPrepareMove(struct movedata *movedata)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	struct weightyaimstate *st = weightyAimCurState();
	const bool held = g_Vars.currentplayer->insightaimmode && weightyAimCanAim(cfg);

	// Classic aim mode keeps the game's own aiming (the camera stops and the
	// stick moves the crosshair); Modern Classic and Mobile keep looking around
	// with Weighty Aim. Aim down sights works on top of any of them.
	st->held = held;
	st->aiming = held && cfg->aimmode != WEIGHTYAIM_AIMMODE_CLASSIC;
	st->adsheld = held && cfg->ads;

	// Aim assist: the same conditions under which the game itself would pull the
	// crosshair onto a target (auto-aim on for this weapon, not in aim mode, a
	// target inside the game's difficulty-scaled window)
	{
		const s32 weaponnum = bgunGetWeaponNum(HAND_RIGHT);
		const struct player *pl = g_Vars.currentplayer;

		st->assistlock = movedata->canautoaim
			&& (bmoveIsAutoAimXEnabledForCurrentWeapon() || bmoveIsAutoAimYEnabledForCurrentWeapon())
			&& pl->autoxaimprop && pl->autoyaimprop
			&& weaponHasAimFlag(weaponnum, INVAIMFLAG_AUTOAIM);
	}

	if (st->aiming) {
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

	return st->aiming;
}

bool weightyAimAdsMoveWanted(void)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();

	return cfg->aimmode == WEIGHTYAIM_AIMMODE_MOBILE && weightyAimCanAim(cfg);
}

bool weightyAimAimDpadMoveWanted(void)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();

	return cfg->aimmode == WEIGHTYAIM_AIMMODE_MODERN && cfg->aimdpadmove && weightyAimCanAim(cfg);
}

bool weightyAimAimStickMoveWanted(void)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();

	return cfg->aimmode == WEIGHTYAIM_AIMMODE_MODERN && !cfg->aimdpadmove && weightyAimCanAim(cfg);
}

void weightyAimApplyMoveSpeed(void)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	const struct weightyaimstate *st = weightyAimCurState();
	f32 mult;

	if (!st->aiming) {
		return;
	}

	mult = 1.f + (clampf(cfg->adsmovespeed, 0.1f, 1.f) - 1.f) * weightyAimAdsAmount(st);

	g_Vars.currentplayer->speedforwards *= mult;
	g_Vars.currentplayer->speedsideways *= mult;

	// no building up the sprint boost while aiming
	g_Vars.currentplayer->speedmaxtime60 = 0;
}

/**
 * How far the gun is brought up for aiming down sights (0..1, eased).
 */
static f32 weightyAimSightsAmount(const struct weightyaimcfg *cfg, const struct weightyaimstate *st)
{
	return weightyAimCfgEnabled(cfg) && cfg->ads && st->adsheld ? weightyAimAdsAmount(st) : 0.f;
}

f32 weightyAimAdjustZoomFov(f32 zoomfov)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	const struct weightyaimstate *st = weightyAimCurState();
	f32 amount, zoom;

	// only zoom guns without their own scope zoom
	if (!weightyAimCfgEnabled(cfg) || zoomfov < PLAYER_DEFAULT_FOV - 0.01f) {
		return zoomfov;
	}

	// Sights Zoom applies to any aiming, with or without Aim Down Sights
	amount = weightyAimAdsAmount(st);
	zoom = cfg->adszoom;

	if (amount <= 0.f) {
		return zoomfov;
	}

	return zoomfov / (1.f + (clampf(zoom, 1.f, 3.f) - 1.f) * amount);
}

/**
 * Aim down sights: bring the gun in low and centred on its line of fire,
 * Doom style.
 *
 * The gun is moved onto the line from the eye to wherever you're aiming (the
 * crosshair doesn't have to be in the middle of the screen), then dropped by
 * Gun Height. The game turns the gun model to point at the spot it will hit
 * from wherever it ends up, so it keeps pointing where the shots go.
 *
 * Instead of hand-tuning every weapon, the barrel's offset from the gun's
 * origin is measured from last frame.
 */
void weightyAimAdjustGunPos(struct hand *hand, s32 handnum, struct coord *pos)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	struct weightyaimstate *st = weightyAimCurState();
	const f32 ads = weightyAimSightsAmount(cfg, st);
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
		struct coord aimdir;
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

		depth = -(pos->z + rel[2]);

		if (depth < 5.f) {
			depth = 5.f;
		}

		// where the line of fire is at the barrel's depth (camera space, -z forward)
		cam0f0b4c3c(g_Vars.currentplayer->crosspos, &aimdir, 1.f);

		if (aimdir.z < -0.05f) {
			target[0] = aimdir.x / -aimdir.z * depth;
			target[1] = aimdir.y / -aimdir.z * depth;
		} else {
			target[0] = target[1] = 0.f;
		}

		// barrel on that line, a little below it, then Gun Height
		target[0] += -rel[0];
		target[1] += -rel[1] - depth * 0.021f + clampf(cfg->adsheight, -10.f, 10.f);

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

bool weightyAimLaserBeamShown(void)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();

	if (!weightyAimCfgEnabled(cfg)) {
		return false;
	}

	return weightyAimHoldingAim(cfg) ? cfg->aimlaserbeam : cfg->laser;
}

bool weightyAimLaserDotShown(void)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();

	if (!weightyAimCfgEnabled(cfg)) {
		return false;
	}

	return weightyAimHoldingAim(cfg) ? cfg->aimlaserdot : cfg->laserdot;
}

bool weightyAimLaserEnhanced(void)
{
	return weightyAimLaserBeamShown() || weightyAimLaserDotShown();
}

static bool weightyAimIsFirearm(s32 weaponnum)
{
	return (weaponnum >= WEAPON_FALCON2 && weaponnum <= WEAPON_SLAYER)
		|| (weaponnum >= WEAPON_CROSSBOW && weaponnum <= WEAPON_LASER)
		|| (weaponnum >= WEAPON_PP9I && weaponnum <= WEAPON_PSYCHOSISGUN);
}

/**
 * The gun is busy with something other than shooting: reloading, switching
 * weapons or modes. Firing animations (a shotgun pump, a bolt) don't count.
 */
static bool weightyAimHandBusyNotFiring(const struct hand *hand)
{
	switch (hand->state) {
	case HANDSTATE_RELOAD:
	case HANDSTATE_CHANGEGUN:
	case HANDSTATE_CHANGEFUNC:
	case HANDSTATE_AUTOSWITCH:
		return true;
	}

	return false;
}

bool weightyAimLaserWanted(struct hand *hand, s32 handnum, s32 weaponnum)
{
	const struct weightyaimcfg *cfg = weightyAimCurCfg();
	bool hidden;

	if (cfg->laserpersist) {
		// only hide it while reloading or switching, not after every shot
		hidden = weightyAimHandBusyNotFiring(hand);
	} else {
		// hide it while any gun animation plays, firing included
		hidden = hand->animmode == HANDANIMMODE_BUSY;
	}

	return handnum == HAND_RIGHT
		&& PLAYERCOUNT() == 1 && IS8MB() // the game only draws laser sights in single player
		&& hand->visible
		&& !hidden
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

	// Each shot re-traces the aim with the gun's random spread and leaves the
	// dot where that bullet went, or nowhere if it flew into open space, so the
	// dot flickers and blinks out while firing. Trace the steady aim line again
	// so the dot stays where you're pointing.
	if (weightyAimCurCfg()->laserpersist && (hand->firing || hand->state == HANDSTATE_ATTACK)) {
		propFindAimingAt(HAND_RIGHT, false, FINDPROPCONTEXT_QUERY);
	}

	beamnear.x = hand->muzzlepos.x;
	beamnear.y = hand->muzzlepos.y;
	beamnear.z = hand->muzzlepos.z;

	if (hand->hasdotinfo) {
		// The beam runs from the muzzle to the dot, but kicks up with the gun's
		// recoil and settles back, while the dot stays on the target. The kick is
		// how far the barrel turns away from its usual line while a shot's
		// animation plays (its usual offset is learned the rest of the time).
		struct weightyaimstate *st = weightyAimCurState();
		const Mtxf *mm = &hand->muzzlemat;
		const f32 mc[3] = { mm->m[3][0], mm->m[3][1], mm->m[3][2] };
		struct coord dotworld = hand->dotpos, dotcam;
		f32 line[3], barrel[3], dir[3], len, blen, dlen;
		const bool kicking = hand->animmode == HANDANIMMODE_BUSY && hand->state == HANDSTATE_ATTACK;

		mtx4TransformVec(camGetWorldToScreenMtxf(), &dotworld, &dotcam);

		line[0] = dotcam.x - mc[0];
		line[1] = dotcam.y - mc[1];
		line[2] = dotcam.z - mc[2];
		len = bc_sqrtf(line[0] * line[0] + line[1] * line[1] + line[2] * line[2]);
		blen = bc_sqrtf(mm->m[2][0] * mm->m[2][0] + mm->m[2][1] * mm->m[2][1] + mm->m[2][2] * mm->m[2][2]);

		beamfar = dotworld;

		if (len > 1.f && blen > 0.0001f) {
			for (s32 i = 0; i < 3; i++) {
				line[i] /= len;
				barrel[i] = mm->m[2][i] / blen; // the gun model's forward axis, camera space
			}

			if (!kicking) {
				const f32 k = 1.f - bc_expf(-g_Vars.lvupdate60freal / 60.f / 0.1f);

				for (s32 i = 0; i < 3; i++) {
					st->barrelbias[i] += (barrel[i] - line[i] - st->barrelbias[i]) * k;
					dir[i] = line[i];
				}
			} else {
				for (s32 i = 0; i < 3; i++) {
					dir[i] = barrel[i] - st->barrelbias[i];
				}
			}

			dlen = bc_sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);

			if (kicking && dlen > 0.0001f) {
				beamfar.x = mc[0] + dir[0] / dlen * len;
				beamfar.y = mc[1] + dir[1] / dlen * len;
				beamfar.z = mc[2] + dir[2] / dlen * len;
				mtx4TransformVecInPlace(camGetProjectionMtxF(), &beamfar);
			}
		}
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
		const struct coord *campos = &g_Vars.currentplayer->cam_pos;
		const f32 dx = campos->x - dotpos.x, dy = campos->y - dotpos.y, dz = campos->z - dotpos.z;
		const f32 dist = bc_sqrtf(dx * dx + dy * dy + dz * dz);

		// lift the dot a touch off the surface, toward the eye, so bullet holes
		// and other wall marks at the same spot can't cover it
		if (dist > 10.f) {
			const f32 lift = 2.f / dist;
			dotpos.x += dx * lift;
			dotpos.y += dy * lift;
			dotpos.z += dz * lift;
		}

		lasersightSetDot(handnum, &dotpos, &dotrot);
	}
}

/*
 * Settings file (pd.ini). The aim settings are registered once for the live
 * values and once per custom profile, from one table.
 */

struct weightyaimcfgfield {
	const char *name;
	size_t offset;
	s32 isint;
	f32 min;
	f32 max;
};

#define WA_FLOAT(name, field, lo, hi) { name, offsetof(struct weightyaimcfg, field), 0, lo, hi }
#define WA_INT(name, field, lo, hi)   { name, offsetof(struct weightyaimcfg, field), 1, lo, hi }

static const struct weightyaimcfgfield g_WeightyAimCfgFields[] = {
	WA_FLOAT("DeadzoneX",     deadzonex,     0.f, 45.f),
	WA_FLOAT("DeadzoneY",     deadzoney,     0.f, 45.f),
	WA_FLOAT("CameraShare",   camerashare,   0.f, 1.f),
	WA_FLOAT("CameraLead",    cameralead,    0.f, 10.f),
	WA_FLOAT("RecenterSpeed", recenterspeed, 0.f, 10.f),
	WA_FLOAT("RecenterDelay", recenterdelay, 0.f, 5.f),
	WA_FLOAT("RecenterSmooth", recentersmooth, 0.f, 2.f),
	WA_FLOAT("GunResponse",   gunresponse,   0.5f, 40.f),
	WA_FLOAT("GunDamping",    gundamping,    0.05f, 3.f),
	WA_FLOAT("TurnDrag",      turndrag,      0.f, 1.f),
	WA_FLOAT("EdgeSmoothing", edgesmoothing, 0.f, 1.f),
	WA_FLOAT("CameraSway",    camerasway,    0.f, 5.f),
	WA_FLOAT("WalkSway",      walksway,      0.f, 5.f),
	WA_INT  ("Crosshair",     crosshair,     0, 1),
	WA_INT  ("LaserSight",    laser,         0, 1),
	WA_INT  ("LaserDot",      laserdot,      0, 1),
	WA_INT  ("LaserPersist",  laserpersist,  0, 1),
	WA_INT  ("AimMode",       aimmode,       0, WEIGHTYAIM_NUM_AIMMODES - 1),
	WA_INT  ("AimDpadMove",   aimdpadmove,   0, 1),
	WA_INT  ("AimCrosshair",  aimcrosshair,  0, 1),
	WA_INT  ("AimLaserDot",   aimlaserdot,   0, 1),
	WA_INT  ("AimLaserBeam",  aimlaserbeam,  0, 1),
	WA_INT  ("AimDownSights", ads,           0, 1),
	WA_FLOAT("AdsZoom",       adszoom,       1.f, 3.f),
	WA_FLOAT("AdsTime",       adstime,       0.f, 1.f),
	WA_FLOAT("AdsSway",       adssway,       0.f, 1.f),
	WA_FLOAT("AdsZone",       adszone,       0.f, 1.f),
	WA_FLOAT("AdsHeight",     adsheight,     -10.f, 10.f),
	WA_FLOAT("AdsMoveSpeed",  adsmovespeed,  0.1f, 1.f),
	WA_FLOAT("AdsSensitivity", adssens,      0.1f, 1.f),
};

static void weightyAimRegisterCfg(const char *prefix, struct weightyaimcfg *cfg)
{
	for (s32 f = 0; f < (s32)ARRAYCOUNT(g_WeightyAimCfgFields); f++) {
		const struct weightyaimcfgfield *fd = &g_WeightyAimCfgFields[f];
		void *ptr = (u8 *)cfg + fd->offset;

		if (fd->isint) {
			configRegisterInt(strFmt("%s.%s", prefix, fd->name), (s32 *)ptr, (s32)fd->min, (s32)fd->max);
		} else {
			configRegisterFloat(strFmt("%s.%s", prefix, fd->name), (f32 *)ptr, fd->min, fd->max);
		}
	}
}

PD_CONSTRUCTOR static void weightyAimConfigInit(void)
{
	static const char *pointnames[4] = { "X1", "Y1", "X2", "Y2" };
	char prefix[64];

	for (s32 j = 0; j < MAX_PLAYERS; ++j) {
		const s32 i = j + 1;

		weightyAimResetDefaults(j);
		g_WeightyAimStickCfg[j] = g_WeightyAimStickDefaults;
		g_WeightyAimLastCustom[j] = 0;

		for (s32 c = 0; c < WEIGHTYAIM_NUM_CUSTOM; c++) {
			g_WeightyAimCustomCfg[j][c] = g_WeightyAimPresetWeighty;
			g_WeightyAimCustomCfg[j][c].preset = WEIGHTYAIM_PRESET_CUSTOM1 + c;
		}

		configRegisterInt(strFmt("WeightyAim.Player%d.Preset", i), &g_WeightyAimCfg[j].preset, 0, WEIGHTYAIM_NUM_PRESETS - 1);
		snprintf(prefix, sizeof(prefix), "WeightyAim.Player%d", i);
		weightyAimRegisterCfg(prefix, &g_WeightyAimCfg[j]);

		configRegisterInt(strFmt("WeightyAim.Player%d.LastCustom", i), &g_WeightyAimLastCustom[j], 0, WEIGHTYAIM_NUM_CUSTOM - 1);
		g_WeightyAimAssistStrength[j] = 1.f;
		configRegisterFloat(strFmt("WeightyAim.Player%d.AimAssistStrength", i), &g_WeightyAimAssistStrength[j], 0.f, 1.f);

		g_WeightyAimGyroCfg[j] = g_WeightyAimGyroDefaults;
		configRegisterInt(strFmt("WeightyAim.Player%d.GyroMode", i), &g_WeightyAimGyroCfg[j].mode, 0, WEIGHTYAIM_NUM_GYROMODES - 1);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GyroSensitivity", i), &g_WeightyAimGyroCfg[j].sensitivity, 0.1f, 20.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GyroVertical", i), &g_WeightyAimGyroCfg[j].vertical, 0.f, 2.f);
		configRegisterInt(strFmt("WeightyAim.Player%d.GyroSpace", i), &g_WeightyAimGyroCfg[j].space, 0, WEIGHTYAIM_NUM_GYROSPACES - 1);
		configRegisterInt(strFmt("WeightyAim.Player%d.GyroInvertY", i), &g_WeightyAimGyroCfg[j].inverty, 0, 1);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GyroAcceleration", i), &g_WeightyAimGyroCfg[j].acceleration, 1.f, 4.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GyroAccelThreshold", i), &g_WeightyAimGyroCfg[j].accelthreshold, 5.f, 500.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GyroTightening", i), &g_WeightyAimGyroCfg[j].tightening, 0.f, 30.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.GyroSmoothing", i), &g_WeightyAimGyroCfg[j].smoothing, 0.f, 30.f);
		configRegisterInt(strFmt("WeightyAim.Player%d.GyroAutoCalibrate", i), &g_WeightyAimGyroCfg[j].autocalibrate, 0, 1);
		configRegisterInt(strFmt("WeightyAim.Player%d.GyroPauseWithStick", i), &g_WeightyAimGyroCfg[j].pausewithstick, 0, 1);

		for (s32 c = 0; c < WEIGHTYAIM_NUM_CUSTOM; c++) {
			snprintf(prefix, sizeof(prefix), "WeightyAim.Player%d.Custom%d", i, c + 1);
			weightyAimRegisterCfg(prefix, &g_WeightyAimCustomCfg[j][c]);
		}

		configRegisterInt(strFmt("WeightyAim.Player%d.StickCurve", i), &g_WeightyAimStickCfg[j].curve, 0, WEIGHTYAIM_NUM_CURVES - 1);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickInnerDeadzone", i), &g_WeightyAimStickCfg[j].innerdeadzone, 0.f, 0.9f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickOuterDeadzone", i), &g_WeightyAimStickCfg[j].outerdeadzone, 0.1f, 1.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickTurnSpeed", i), &g_WeightyAimStickCfg[j].turnspeed, 0.1f, 3.f);
		configRegisterFloat(strFmt("WeightyAim.Player%d.StickVerticalSensitivity", i), &g_WeightyAimStickCfg[j].verticalsens, 0.1f, 3.f);
		configRegisterInt(strFmt("WeightyAim.Player%d.StickLastCustomCurve", i), &g_WeightyAimStickCfg[j].lastcustomcurve, 0, 2);

		for (s32 k = 0; k < 4; k++) {
			configRegisterFloat(strFmt("WeightyAim.Player%d.StickCurve%s", i, pointnames[k]), &g_WeightyAimStickCfg[j].bezier[k], 0.f, 1.f);
		}

		for (s32 c = 0; c < 3; c++) {
			for (s32 k = 0; k < 4; k++) {
				configRegisterFloat(strFmt("WeightyAim.Player%d.StickCustom%d%s", i, c + 1, pointnames[k]),
						&g_WeightyAimStickCfg[j].bezierprofile[c][k], 0.f, 1.f);
			}
		}

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
