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
#define WEIGHTYAIM_PRESET_BORING  2 // plain modern FPS: crosshair locked to the centre, no sway
#define WEIGHTYAIM_PRESET_CLASSIC 3 // the game's original crosshair sway (mod off)
#define WEIGHTYAIM_PRESET_CUSTOM1 4 // your own profiles; each one remembers its settings
#define WEIGHTYAIM_PRESET_CUSTOM2 5
#define WEIGHTYAIM_PRESET_CUSTOM3 6
#define WEIGHTYAIM_NUM_PRESETS    7
#define WEIGHTYAIM_NUM_CUSTOM     3
#define WEIGHTYAIM_IS_CUSTOM(p)   ((p) >= WEIGHTYAIM_PRESET_CUSTOM1)

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
	s32 laser;           // RE4-style laser sight on every gun: glowing beam to a bright dot

	// aim down sights (holding the aim button)
	s32 ads;             // 1 = raise the gun to your eye, 0 = the game's own aim mode
	f32 adszoom;         // zoom while aiming (1 = none)
	f32 adstime;         // seconds to raise or lower the gun
	f32 adssway;         // share of the sway kept while aiming (0..1)
	f32 adszone;         // share of the free-aim zone kept while aiming (0..1)
	f32 adsheight;       // fine-tune how high the gun sits when aimed (screen units, + = higher)
};

#define WEIGHTYAIM_CURVE_ORIGINAL 0 // the game's response: squared, maxes out early (ignores the deadzones below)
#define WEIGHTYAIM_CURVE_LINEAR   1
#define WEIGHTYAIM_CURVE_BALANCED 2 // between linear and the original
#define WEIGHTYAIM_CURVE_CUSTOM1  3 // cubic bezier from (0,0) to (1,1), like DS4Windows;
#define WEIGHTYAIM_CURVE_CUSTOM2  4 // three custom curves, each remembers its own points
#define WEIGHTYAIM_CURVE_CUSTOM3  5
#define WEIGHTYAIM_NUM_CURVES     6
#define WEIGHTYAIM_IS_CUSTOM_CURVE(c) ((c) >= WEIGHTYAIM_CURVE_CUSTOM1)

#define WEIGHTYAIM_BOOST_OFF     0
#define WEIGHTYAIM_BOOST_INSTANT 1 // full boost the moment the stick hits the threshold
#define WEIGHTYAIM_BOOST_RAMPED  2 // short delay, then ramps up (like Apex Legends / CoD)
#define WEIGHTYAIM_NUM_BOOSTS    3

/*
 * Look-stick response. Kept separate from the aim presets so switching
 * presets never resets your stick tuning.
 */
struct weightyaimstickcfg {
	s32 curve;           // WEIGHTYAIM_CURVE_*
	f32 innerdeadzone;   // 0..1 of full deflection; below this the stick reads zero
	f32 outerdeadzone;   // 0..1; at or past this the stick reads full
	f32 bezier[4];       // the custom curve being used/edited: x1, y1, x2, y2 (0..1)
	f32 bezierprofile[3][4]; // saved points for Custom 1-3
	s32 lastcustomcurve; // which custom curve the curve sliders edit when a built-in curve is selected
	f32 turnspeed;       // look speed at full deflection (1 = the game's max turn rate)

	// turn boost: extra turn speed when the stick is held near full deflection
	s32 boostmode;       // WEIGHTYAIM_BOOST_*
	f32 boostamount;     // turn speed multiplier at full boost (1 = no boost)
	f32 boostthreshold;  // how far the stick must be pushed to start boosting (0..1)
	f32 boostdelay;      // ramped: seconds at the threshold before the boost starts
	f32 boosttime;       // ramped: seconds to go from no boost to full boost
	f32 boostvertical;   // share of the boost applied to looking up/down (0..1)
};

extern f32 g_WeightyAimAssistStrength[4]; // 0..1 of what the game and difficulty allow (1 = game default, 0 = off)

#define WEIGHTYAIM_GYRO_OFF     0
#define WEIGHTYAIM_GYRO_ALWAYS  1
#define WEIGHTYAIM_GYRO_AIMING  2 // only while holding aim
#define WEIGHTYAIM_NUM_GYROMODES 3

#define WEIGHTYAIM_GYROSPACE_PLAYER 0 // turning or leaning the controller both turn you (JibbSmart's "player space")
#define WEIGHTYAIM_GYROSPACE_LOCAL  1 // only turning the controller flat turns you
#define WEIGHTYAIM_NUM_GYROSPACES   2

/*
 * Gyro aiming, following JibbSmart's GyroWiki: sensitivity 1 means turning
 * the controller 1 degree turns you 1 degree. Gyro input goes through the
 * same free-aim zone as the stick and mouse.
 */
struct weightyaimgyrocfg {
	s32 mode;            // WEIGHTYAIM_GYRO_*
	f32 sensitivity;     // in-game degrees per real degree
	f32 vertical;        // vertical sensitivity as a share of horizontal
	s32 space;           // WEIGHTYAIM_GYROSPACE_* (advanced)
	s32 inverty;         // advanced
	f32 acceleration;    // advanced: sensitivity multiplier for fast motion (1 = off)
	f32 accelthreshold;  // advanced: speed (deg/s) where acceleration is at full
	f32 tightening;      // advanced: below this speed (deg/s) input is scaled down to hide jitter (0 = off)
	f32 smoothing;       // advanced: below this speed (deg/s) input is smoothed (0 = off)
	s32 autocalibrate;   // advanced: recalibrate whenever the controller is held still
	s32 pausewithstick;  // advanced: ignore gyro while the look stick is moving
};

extern struct weightyaimgyrocfg g_WeightyAimGyroCfg[4];
extern const char *g_WeightyAimGyroModeNames[WEIGHTYAIM_NUM_GYROMODES];
extern const char *g_WeightyAimGyroSpaceNames[WEIGHTYAIM_NUM_GYROSPACES];

void weightyAimResetGyroDefaults(s32 cfgindex);
void weightyAimGyroStartCalibration(s32 cfgindex);
void weightyAimGyroMenuTick(s32 cfgindex);
const char *weightyAimGyroStatusText(s32 cfgindex);

extern struct weightyaimcfg g_WeightyAimCfg[4];            // live settings
extern struct weightyaimcfg g_WeightyAimCustomCfg[4][3];     // saved Custom 1-3 profiles
extern s32 g_WeightyAimLastCustom[4];                        // custom profile edited from a built-in preset
extern struct weightyaimstickcfg g_WeightyAimStickCfg[4];
extern const char *g_WeightyAimCurveNames[WEIGHTYAIM_NUM_CURVES];
extern const char *g_WeightyAimBoostNames[WEIGHTYAIM_NUM_BOOSTS];
extern s32 g_WeightyAimDebugLog;       // write per-frame telemetry to weightyaim_log.csv
extern s32 g_WeightyAimDebugPattern;   // replace look input with a scripted test pattern

extern const char *g_WeightyAimPresetNames[WEIGHTYAIM_NUM_PRESETS];

void weightyAimResetDefaults(s32 cfgindex);
void weightyAimApplyPreset(s32 cfgindex, s32 preset);
void weightyAimResetStickDefaults(s32 cfgindex);
void weightyAimAimSettingsChanged(s32 cfgindex);
void weightyAimSelectCurve(s32 cfgindex, s32 curve);
void weightyAimCurvePointsChanged(s32 cfgindex);

/*
 * Look speed (0..max turn speed, before turn boost) for a stick pushed
 * 'deflection' (0..1) of the way, with this player's curve and deadzones.
 * Used to draw the curve graph.
 */
f32 weightyAimCurveOutput(const struct weightyaimstickcfg *sc, f32 deflection);
void weightyAimResetBoostDefaults(s32 cfgindex);

/*
 * Hook 1 (bondmove.c, before the look code runs): takes this frame's look
 * input, moves the gun inside the deadzone, and rewrites the input so that
 * only the leftover turning reaches the camera.
 */
void weightyAimFilterLook(s32 *analogturn, s32 *analogpitch, f32 *freelookdx, f32 *freelookdy,
		bool canlook, f32 mlookscale);

struct hand;

/*
 * Hook 1a (bondmove.c, just before hook 1): when aiming down sights, keep the
 * normal look controls instead of the game's aim mode (which freezes the
 * camera and moves the crosshair). Returns true while aiming down sights.
 */
struct movedata;
bool weightyAimPrepareMove(struct movedata *movedata);

/*
 * Hook 1b (bondmove.c, zoom): the field of view to zoom to this frame.
 */
f32 weightyAimAdjustZoomFov(f32 zoomfov);

/*
 * Hook 1c (bondgun.c, gun placement): moves the gun model toward the centre
 * of the screen, lined up with its barrel, while aiming down sights.
 */
void weightyAimAdjustGunPos(struct hand *hand, s32 handnum, struct coord *pos);

/*
 * Hook 6 (bondmove.c auto-aim checks and prop.c auto-aim window): aim assist
 * can only be reduced or turned off, never made stronger than the game and
 * difficulty allow. weightyAimAssistAllowed() is false when turned off;
 * weightyAimAssistScale() (0..1) multiplies the game's own aim assist window.
 */
bool weightyAimAssistAllowed(void);
f32 weightyAimAssistScale(void);

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
