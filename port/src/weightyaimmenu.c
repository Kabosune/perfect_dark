#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/menu.h"
#include "game/game_1531a0.h"
#include "game/options.h"
#include "lib/joy.h"
#include "gbiex.h"
#include "weightyaim.h"

/*
 * Options -> Extended -> Weighty Aim
 *
 *   Preset
 *   Aim & Camera Feel...   free-aim zone, gun weight, camera lead, sway
 *   Stick Response...      look curve, deadzones, max turn speed
 *   Turn Boost...          extra turn speed at full stick
 *   Aim Mode...            what holding aim does: aim mode, aim down sights, reticle/laser
 *   Gyro Aim...            motion controls (Advanced... inside)
 *   Reticle...             on/off (hip-fire, aiming), size, opacity, smooth, colour
 *   Laser Sight / Laser Dot / Aim Assist / Debug Log / Reset
 *   Force Original Aim & Settings / Force Mouse & Gyro as Stick (tournaments)
 *
 * Sliders are table driven: each page has a table of weightyaimslider rows
 * that must be in the same order as its slider items. Adding a tunable is:
 * add the field, register it in weightyaim.c, add a row and a menu item.
 */

extern s32 optionsGetExtMenuPlayer(void);

struct weightyaimslider {
	size_t offset;         // field in the page's settings struct
	f32 step;              // value of one slider notch
	f32 min;               // lowest allowed value
	const char *fmt;       // printf format for the value shown next to the slider
	const char *zerolabel; // shown instead of the number when the value is 0 (optional)
	s32 percent;           // show value * 100
	f32 base;              // value at the leftmost notch (for ranges that go negative)
};

struct weightyaimsliderpage {
	struct menuitem *items;                 // the page's menu items
	s32 firstslider;                        // index of the first slider item
	const struct weightyaimslider *sliders; // one row per slider, in order
	s32 numsliders;
	void *(*getcfg)(void);                  // settings struct being edited
	void (*onchange)(s32 sliderindex);      // optional: called after a slider moves
};

#define AIMFIELD(f)   offsetof(struct weightyaimcfg, f)
#define STICKFIELD(f) offsetof(struct weightyaimstickcfg, f)

static inline struct weightyaimcfg *weightyAimMenuCfg(void)
{
	return &g_WeightyAimCfg[optionsGetExtMenuPlayer() & 3];
}

static inline struct weightyaimstickcfg *weightyAimMenuStickCfg(void)
{
	return &g_WeightyAimStickCfg[optionsGetExtMenuPlayer() & 3];
}

static void *weightyAimMenuCfgVoid(void)      { return weightyAimMenuCfg(); }
static void *weightyAimMenuStickCfgVoid(void) { return weightyAimMenuStickCfg(); }

static MenuItemHandlerResult menuhandlerWeightyAimSlider(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerWeightyAimPreset(s32 operation, struct menuitem *item, union handlerdata *data);

// the preset picker, shown at the top of each aim page so the sliders below
// update live as you flip through presets
#define WEIGHTYAIM_PRESET_ITEM \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Preset", 0, menuhandlerWeightyAimPreset }

#define WEIGHTYAIM_SLIDER(label, notches) \
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)(label), (notches), menuhandlerWeightyAimSlider }

#define WEIGHTYAIM_BACK \
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL }, \
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_OPTIONS_213, 0, NULL }, \
	{ MENUITEMTYPE_END }

/* ------------------------------------------------------------------------
 * Aim & Camera Feel
 */

// The first WEIGHTYAIM_FEEL_NUM_MAIN sliders are always shown; the rest are
// fine-tuning, shown with the "Show Advanced Feel" tick.
#define WEIGHTYAIM_FEEL_NUM_MAIN 8

static const struct weightyaimslider g_WeightyAimFeelSliders[] = {
	{ AIMFIELD(deadzonex),     0.5f,  0.f,  "%.1f deg", NULL,  0 },
	{ AIMFIELD(deadzoney),     0.5f,  0.f,  "%.1f deg", NULL,  0 },
	{ AIMFIELD(camerashare),   0.05f, 0.f,  "%.0f%%",   NULL,  1 },
	{ AIMFIELD(cameralead),    0.1f,  0.f,  "%.1f",     "Off", 0 },
	{ AIMFIELD(recenterspeed), 0.1f,  0.f,  "%.1f",     "Off", 0 },
	{ AIMFIELD(recenterdelay), 0.05f, 0.f,  "%.2fs",    NULL,  0 },
	{ AIMFIELD(gunresponse),   0.5f,  1.f,  "%.1f Hz",  NULL,  0 },
	{ AIMFIELD(edgeturnspeed), 10.f,  0.f,  "%.0f deg/s", "Off", 0 },
	// advanced
	{ AIMFIELD(reticlespeed),  0.05f, 0.25f, "%.0f%%",  NULL,  1, 0.25f },
	{ AIMFIELD(edgesmoothing), 0.02f, 0.f,  "%.2fs",    "Off", 0 },
	{ AIMFIELD(recentersmooth), 0.05f, 0.f, "%.2fs",    "Off", 0 },
	{ AIMFIELD(gundamping),    0.05f, 0.1f, "%.2f",     NULL,  0 },
	{ AIMFIELD(turndrag),      0.05f, 0.f,  "%.2f",     NULL,  0 },
	{ AIMFIELD(camerasway),    0.05f, 0.f,  "%.2f deg", "Off", 0 },
	{ AIMFIELD(walksway),      0.1f,  0.f,  "%.1f deg", "Off", 0 },
	{ AIMFIELD(edgeband),      0.05f, 0.05f, "%.0f%%",  NULL,  1 },
	{ AIMFIELD(edgeinfluence), 0.05f, 0.f,  "%.0f%%",   "Off", 1 },
	{ AIMFIELD(edgevertical),  0.05f, 0.f,  "%.0f%%",   "Off", 1 },
};

// Edge Auto-Turn inputs (advanced; hidden unless "Show Advanced Feel" is ticked)
#define WEIGHTYAIM_FEEL_ADV_CHECKBOX_HANDLER(fn, field) \
	static MenuItemHandlerResult fn(s32 operation, struct menuitem *item, union handlerdata *data) \
	{ \
		switch (operation) { \
		case MENUOP_GET: return weightyAimMenuCfg()->field; \
		case MENUOP_SET: \
			weightyAimMenuCfg()->field = data->checkbox.value ? 1 : 0; \
			weightyAimAimSettingsChanged(optionsGetExtMenuPlayer()); \
			break; \
		case MENUOP_CHECKHIDDEN: \
		case MENUOP_CHECKDISABLED: \
			return !g_WeightyAimShowAdvancedFeel; \
		} \
		return 0; \
	}

WEIGHTYAIM_FEEL_ADV_CHECKBOX_HANDLER(menuhandlerWeightyAimEdgeMouse, edgemouse)
WEIGHTYAIM_FEEL_ADV_CHECKBOX_HANDLER(menuhandlerWeightyAimEdgeGyro, edgegyro)
WEIGHTYAIM_FEEL_ADV_CHECKBOX_HANDLER(menuhandlerWeightyAimEdgeStick, edgestick)

static MenuItemHandlerResult menuhandlerWeightyAimShowAdvancedFeel(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_WeightyAimShowAdvancedFeel;
	case MENUOP_SET:
		g_WeightyAimShowAdvancedFeel = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

struct menuitem g_WeightyAimFeelMenuItems[] = {
	WEIGHTYAIM_PRESET_ITEM,
	// order must match g_WeightyAimFeelSliders
	WEIGHTYAIM_SLIDER("Free-Aim Zone Width", 80),   // 0 - 40 deg
	WEIGHTYAIM_SLIDER("Free-Aim Zone Height", 50),  // 0 - 25 deg
	WEIGHTYAIM_SLIDER("Camera Share", 20),          // 0 - 100 %
	WEIGHTYAIM_SLIDER("Camera Lead", 30),           // 0 - 3
	WEIGHTYAIM_SLIDER("Camera Catch-Up", 50),       // 0 - 5
	WEIGHTYAIM_SLIDER("Catch-Up Delay", 40),        // 0 - 2 s
	WEIGHTYAIM_SLIDER("Gun Response", 40),          // 1 - 20 Hz
	WEIGHTYAIM_SLIDER("Edge Auto-Turn", 18),        // 0 - 180 deg/s at the edge (Arcade)
	// advanced (hidden unless "Show Advanced Feel" is ticked)
	WEIGHTYAIM_SLIDER("Free-Aim Reticle Speed", 55), // 25 - 300 %: 100 % keeps aim 1:1 with the camera
	WEIGHTYAIM_SLIDER("Edge Smoothing", 25),        // 0 - 0.5 s
	WEIGHTYAIM_SLIDER("Catch-Up Smoothing", 20),    // 0 - 1 s: eases the camera into re-centring
	WEIGHTYAIM_SLIDER("Gun Damping", 30),           // 0.1 - 1.5
	WEIGHTYAIM_SLIDER("Turn Drag", 20),             // 0 - 1
	WEIGHTYAIM_SLIDER("Camera Sway", 30),           // 0 - 1.5 deg
	WEIGHTYAIM_SLIDER("Walk Sway", 30),             // 0 - 3 deg
	WEIGHTYAIM_SLIDER("Edge Auto-Turn Band", 12),   // 5 - 60 % of the zone
	WEIGHTYAIM_SLIDER("Edge Input Influence", 20),  // 0 - 100 %: your own push while auto-turning
	WEIGHTYAIM_SLIDER("Edge Vertical Turn", 20),    // 0 - 100 % of the horizontal speed
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Edge Auto-Turn: Mouse", 0, menuhandlerWeightyAimEdgeMouse },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Edge Auto-Turn: Gyro", 0, menuhandlerWeightyAimEdgeGyro },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Edge Auto-Turn: Stick", 0, menuhandlerWeightyAimEdgeStick },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Show Advanced Feel", 0, menuhandlerWeightyAimShowAdvancedFeel },
	WEIGHTYAIM_BACK,
};

struct menudialogdef g_WeightyAimFeelMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Aim & Camera Feel",
	g_WeightyAimFeelMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static void weightyAimFeelChanged(s32 sliderindex)
{
	// saved into the custom profile (switching to one if a built-in preset was on)
	weightyAimAimSettingsChanged(optionsGetExtMenuPlayer());
}

/* ------------------------------------------------------------------------
 * Stick Response
 */

static const struct weightyaimslider g_WeightyAimStickSliders[] = {
	{ STICKFIELD(turnspeed),     0.05f, 0.25f, "%.2fx",  NULL, 0 },
	{ STICKFIELD(verticalsens),  0.05f, 0.25f, "%.0f%%", NULL, 1 },
	{ STICKFIELD(innerdeadzone), 0.01f, 0.f,   "%.0f%%", NULL, 1 },
	{ STICKFIELD(outerdeadzone), 0.01f, 0.1f,  "%.0f%%", NULL, 1 },
	{ STICKFIELD(bezier[0]),     0.05f, 0.f,   "%.2f",   NULL, 0 },
	{ STICKFIELD(bezier[1]),     0.05f, 0.f,   "%.2f",   NULL, 0 },
	{ STICKFIELD(bezier[2]),     0.05f, 0.f,   "%.2f",   NULL, 0 },
	{ STICKFIELD(bezier[3]),     0.05f, 0.f,   "%.2f",   NULL, 0 },
};

#define STICK_FIRST_BEZIER_SLIDER 4
#define STICK_LAST_BEZIER_SLIDER  7

static void weightyAimStickChanged(s32 sliderindex)
{
	// moving a curve point switches to a custom curve and saves the points there
	if (sliderindex >= STICK_FIRST_BEZIER_SLIDER && sliderindex <= STICK_LAST_BEZIER_SLIDER) {
		weightyAimCurvePointsChanged(optionsGetExtMenuPlayer());
	}
}

// menu order (the saved numbers stay as they were, so Source Port is listed
// second although it was added last)
static const s32 g_WeightyAimCurveOrder[WEIGHTYAIM_NUM_CURVES] = {
	WEIGHTYAIM_CURVE_ORIGINAL,
	WEIGHTYAIM_CURVE_SOURCEPORT,
	WEIGHTYAIM_CURVE_LINEAR,
	WEIGHTYAIM_CURVE_BALANCED,
	WEIGHTYAIM_CURVE_CUSTOM1,
	WEIGHTYAIM_CURVE_CUSTOM2,
	WEIGHTYAIM_CURVE_CUSTOM3,
};

static MenuItemHandlerResult menuhandlerWeightyAimCurve(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = WEIGHTYAIM_NUM_CURVES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)g_WeightyAimCurveNames[g_WeightyAimCurveOrder[data->dropdown.value % WEIGHTYAIM_NUM_CURVES]];
	case MENUOP_SET:
		weightyAimSelectCurve(optionsGetExtMenuPlayer(), g_WeightyAimCurveOrder[data->dropdown.value % WEIGHTYAIM_NUM_CURVES]);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;
		for (s32 i = 0; i < WEIGHTYAIM_NUM_CURVES; i++) {
			if (g_WeightyAimCurveOrder[i] == weightyAimMenuStickCfg()->curve) {
				data->dropdown.value = i;
				break;
			}
		}
		break;
	}

	return 0;
}

/* ------------------------------------------------------------------------
 * Look curve graph (drawn live on the Stick Response page)
 *
 *   x: how far the stick is pushed, 0 - 100%
 *   y: look speed (up to Max Turn Speed)
 *   red band: inner deadzone, green band: past the outer deadzone
 *   faint diagonal: linear, for comparison
 *   orange squares: custom curve points
 *   white line and dot: where your look stick is right now
 */

static Gfx *weightyAimGraphRect(Gfx *gdl, s32 x1, s32 y1, s32 x2, s32 y2, u32 colour)
{
	if (x2 <= x1 || y2 <= y1) {
		return gdl;
	}

	gdl = textSetPrimColour(gdl, colour);
	gDPFillRectangleScaled(gdl++, x1, y1, x2, y2);
	return gdl;
}

static Gfx *weightyAimRenderCurveGraph(Gfx *gdl, struct menurendercontext *context)
{
	const struct weightyaimstickcfg *sc = weightyAimMenuStickCfg();
	const s32 gx1 = context->x + 10;
	const s32 gx2 = context->x + context->width - 10;
	const s32 gy1 = context->y + 3;
	const s32 gy2 = context->y + context->height - 3;
	const s32 gw = gx2 - gx1;
	const s32 gh = gy2 - gy1;
	const f32 ymax = sc->turnspeed > 1.f ? sc->turnspeed : 1.f;
	const bool original = WEIGHTYAIM_IS_NATIVE_CURVE(sc->curve);
	const f32 lo = original ? 0.f : sc->innerdeadzone;
	const f32 hi = original ? 1.f : (sc->outerdeadzone > lo + 0.05f ? sc->outerdeadzone : lo + 0.05f);
	s32 prevy = gy2;

	if (gw < 20 || gh < 20) {
		return gdl;
	}

#define GX(f) (gx1 + (s32)((f) * gw + 0.5f))
#define GY(v) (gy2 - (s32)((v) / ymax * gh + 0.5f))

	// background and deadzone bands
	gdl = weightyAimGraphRect(gdl, gx1, gy1, gx2, gy2, 0x0000207f);

	if (!original) {
		gdl = weightyAimGraphRect(gdl, gx1, gy1, GX(lo), gy2, 0xff20204f);
		gdl = weightyAimGraphRect(gdl, GX(hi), gy1, gx2, gy2, 0x20ff203f);
	}

	// grid at 25 / 50 / 75 %
	for (s32 g = 1; g < 4; g++) {
		gdl = weightyAimGraphRect(gdl, GX(g * 0.25f), gy1, GX(g * 0.25f) + 1, gy2, 0xffffff1f);
		gdl = weightyAimGraphRect(gdl, gx1, GY(g * 0.25f * ymax), gx2, GY(g * 0.25f * ymax) + 1, 0xffffff1f);
	}

	// frame
	gdl = weightyAimGraphRect(gdl, gx1, gy1, gx2, gy1 + 1, 0x80c0ffaf);
	gdl = weightyAimGraphRect(gdl, gx1, gy2 - 1, gx2, gy2, 0x80c0ffaf);
	gdl = weightyAimGraphRect(gdl, gx1, gy1, gx1 + 1, gy2, 0x80c0ffaf);
	gdl = weightyAimGraphRect(gdl, gx2 - 1, gy1, gx2, gy2, 0x80c0ffaf);

	// linear reference (dotted)
	for (s32 i = 0; i <= gw; i += 4) {
		const f32 f = (f32)i / gw;
		const s32 y = GY(f);
		gdl = weightyAimGraphRect(gdl, gx1 + i, y - 1, gx1 + i + 1, y, 0xffffff5f);
	}

	// custom curve points and their handles
	if (WEIGHTYAIM_IS_CUSTOM_CURVE(sc->curve)) {
		const f32 ts = sc->turnspeed;
		const s32 p1x = GX(lo + sc->bezier[0] * (hi - lo)), p1y = GY(sc->bezier[1] * ts);
		const s32 p2x = GX(lo + sc->bezier[2] * (hi - lo)), p2y = GY(sc->bezier[3] * ts);
		const s32 a0x = GX(lo), a0y = gy2;
		const s32 a1x = GX(hi), a1y = GY(ts);

		for (s32 k = 0; k <= 16; k++) {
			const f32 t = k / 16.f;
			const s32 hx1 = a0x + (s32)((p1x - a0x) * t), hy1 = a0y + (s32)((p1y - a0y) * t);
			const s32 hx2 = a1x + (s32)((p2x - a1x) * t), hy2 = a1y + (s32)((p2y - a1y) * t);
			gdl = weightyAimGraphRect(gdl, hx1, hy1, hx1 + 1, hy1 + 1, 0xffc0407f);
			gdl = weightyAimGraphRect(gdl, hx2, hy2, hx2 + 1, hy2 + 1, 0xffc0407f);
		}

		gdl = weightyAimGraphRect(gdl, p1x - 2, p1y - 2, p1x + 2, p1y + 2, 0xffc040ff);
		gdl = weightyAimGraphRect(gdl, p2x - 2, p2y - 2, p2x + 2, p2y + 2, 0xffc040ff);
	}

	// the curve: one column per pixel, joined so steep parts stay solid
	for (s32 i = 0; i <= gw; i++) {
		const f32 f = (f32)i / gw;
		const s32 y = GY(weightyAimCurveOutput(sc, f));
		const s32 top = y < prevy ? y : prevy;
		const s32 bottom = y > prevy ? y : prevy;

		gdl = weightyAimGraphRect(gdl, gx1 + i, top - 1, gx1 + i + 1, bottom + 1, 0x40e0ffff);
		prevy = y;
	}

	// live marker: where the look stick is right now
	{
		const s32 pad = optionsGetContpadNum1(optionsGetExtMenuPlayer());
		const f32 sx = joyGetStickX(pad) / 127.f;
		const f32 sy = joyGetStickY(pad) / 127.f;
		f32 mag = __builtin_sqrtf(sx * sx + sy * sy);

		if (mag > 1.f) {
			mag = 1.f;
		}

		if (mag > 0.02f) {
			const s32 mx = GX(mag);
			const s32 my = GY(weightyAimCurveOutput(sc, mag));

			gdl = weightyAimGraphRect(gdl, mx, gy1, mx + 1, gy2, 0xffffff8f);
			gdl = weightyAimGraphRect(gdl, mx - 2, my - 2, mx + 3, my + 3, 0xffffffff);
		}
	}

#undef GX
#undef GY

	gdl = text0f153838(gdl);
	return gdl;
}

static MenuItemHandlerResult menuhandlerWeightyAimGameDeadzone(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return weightyAimMenuStickCfg()->gamedeadzone;
	case MENUOP_SET:
		weightyAimMenuStickCfg()->gamedeadzone = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimStickReset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		weightyAimResetStickDefaults(optionsGetExtMenuPlayer());
	}

	return 0;
}

struct menuitem g_WeightyAimStickMenuItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Look Curve", 0, menuhandlerWeightyAimCurve },
	{ MENUITEMTYPE_CUSTOMRENDER, 0, 0, (intptr_t)weightyAimRenderCurveGraph, 52, NULL },
	// order must match g_WeightyAimStickSliders
	WEIGHTYAIM_SLIDER("Sensitivity", 50),        // 0.25 - 2.5x: look speed at full stick
	WEIGHTYAIM_SLIDER("Vertical Sensitivity", 40), // 25 - 200 % of the left/right speed
	WEIGHTYAIM_SLIDER("Inner Deadzone", 40),     // 0 - 40 %
	WEIGHTYAIM_SLIDER("Outer Deadzone", 100),    // 10 - 100 %
	WEIGHTYAIM_SLIDER("Custom Curve X1", 20),    // 0 - 1
	WEIGHTYAIM_SLIDER("Custom Curve Y1", 20),
	WEIGHTYAIM_SLIDER("Custom Curve X2", 20),
	WEIGHTYAIM_SLIDER("Custom Curve Y2", 20),
	// the game's own small deadzone (about 4%), on top of the one above
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Game's Built-In Deadzone", 0, menuhandlerWeightyAimGameDeadzone },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reset Stick Response\n", 0, menuhandlerWeightyAimStickReset },
	WEIGHTYAIM_BACK,
};

struct menudialogdef g_WeightyAimStickMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Stick Response",
	g_WeightyAimStickMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* ------------------------------------------------------------------------
 * Turn Boost
 */

static const struct weightyaimslider g_WeightyAimBoostSliders[] = {
	{ STICKFIELD(boostamount),    0.05f, 1.f,  "%.2fx",  NULL,  0 },
	{ STICKFIELD(boostthreshold), 0.01f, 0.5f, "%.0f%%", NULL,  1 },
	{ STICKFIELD(boostdelay),     0.02f, 0.f,  "%.2fs",  "None", 0 },
	{ STICKFIELD(boosttime),      0.05f, 0.f,  "%.2fs",  "None", 0 },
	{ STICKFIELD(boostvertical),  0.05f, 0.f,  "%.0f%%", "Off",  1 },
};

static MenuItemHandlerResult menuhandlerWeightyAimBoostMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = WEIGHTYAIM_NUM_BOOSTS;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)g_WeightyAimBoostNames[data->dropdown.value];
	case MENUOP_SET:
		weightyAimMenuStickCfg()->boostmode = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = weightyAimMenuStickCfg()->boostmode;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimBoostReset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		weightyAimResetBoostDefaults(optionsGetExtMenuPlayer());
	}

	return 0;
}

struct menuitem g_WeightyAimBoostMenuItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Look Acceleration", 0, menuhandlerWeightyAimBoostMode },
	// order must match g_WeightyAimBoostSliders
	WEIGHTYAIM_SLIDER("Acceleration Speed", 60),        // 1 - 4x
	WEIGHTYAIM_SLIDER("Stick Threshold", 100),   // 50 - 100 %
	WEIGHTYAIM_SLIDER("Acceleration Delay", 25),        // 0 - 0.5 s (Ramped)
	WEIGHTYAIM_SLIDER("Ramp-Up Time", 20),       // 0 - 1 s (Ramped)
	WEIGHTYAIM_SLIDER("Vertical Acceleration", 20),     // 0 - 100 %
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reset Look Acceleration\n", 0, menuhandlerWeightyAimBoostReset },
	WEIGHTYAIM_BACK,
};

struct menudialogdef g_WeightyAimBoostMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Look Acceleration",
	g_WeightyAimBoostMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* ------------------------------------------------------------------------
 * Aim Mode (holding the aim button)
 */

static const struct weightyaimslider g_WeightyAimAdsSliders[] = {
	{ AIMFIELD(adssens),      0.05f, 0.2f,  "%.0f%%", NULL,      1, 0.2f },
	{ AIMFIELD(adszone),      0.05f, 0.f,   "%.0f%%", "None",    1, 0.f },
	{ AIMFIELD(adssway),      0.05f, 0.f,   "%.0f%%", "None",    1, 0.f },
	{ AIMFIELD(adsmovespeed), 0.05f, 0.2f,  "%.0f%%", NULL,      1, 0.2f },
	{ AIMFIELD(adszoom),      0.05f, 1.f,   "%.2fx",  NULL,      0, 1.f },
	{ AIMFIELD(adstime),      0.02f, 0.f,   "%.2fs",  "Instant", 0, 0.f },
	{ AIMFIELD(adsheight),    0.25f, -10.f, "%+.2f",  NULL,      0, -10.f },
};

static MenuItemHandlerResult menuhandlerWeightyAimMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[WEIGHTYAIM_NUM_AIMMODES] = {
		"Classic",
		"Modern",
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = WEIGHTYAIM_NUM_AIMMODES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		weightyAimMenuCfg()->aimmode = data->dropdown.value;
		// each aim mode comes with its own Move While Aiming default
		weightyAimMenuCfg()->aimmovement = weightyAimDefaultAimMovement(data->dropdown.value);
		weightyAimMenuCfg()->aimkbmove = weightyAimDefaultAimKeyboardMove(data->dropdown.value);
		weightyAimAimSettingsChanged(optionsGetExtMenuPlayer());
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = weightyAimMenuCfg()->aimmode;
		break;
	}

	return 0;
}

#define WEIGHTYAIM_AIM_CHECKBOX_HANDLER(fn, field) \
	static MenuItemHandlerResult fn(s32 operation, struct menuitem *item, union handlerdata *data) \
	{ \
		switch (operation) { \
		case MENUOP_GET: return weightyAimMenuCfg()->field; \
		case MENUOP_SET: \
			weightyAimMenuCfg()->field = data->checkbox.value; \
			weightyAimAimSettingsChanged(optionsGetExtMenuPlayer()); \
			break; \
		} \
		return 0; \
	}

WEIGHTYAIM_AIM_CHECKBOX_HANDLER(menuhandlerWeightyAimAds, ads)
WEIGHTYAIM_AIM_CHECKBOX_HANDLER(menuhandlerWeightyAimKeyboardMove, aimkbmove)
WEIGHTYAIM_AIM_CHECKBOX_HANDLER(menuhandlerWeightyAimAimCrosshair, aimcrosshair)
WEIGHTYAIM_AIM_CHECKBOX_HANDLER(menuhandlerWeightyAimAimLaserDot, aimlaserdot)
WEIGHTYAIM_AIM_CHECKBOX_HANDLER(menuhandlerWeightyAimAimLaserBeam, aimlaserbeam)

static MenuItemHandlerResult menuhandlerWeightyAimMovement(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[WEIGHTYAIM_NUM_AIMMOVES] = {
		"Off",
		"D-Pad",
		"Stick",
		"D-Pad + Stick",
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = WEIGHTYAIM_NUM_AIMMOVES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		weightyAimMenuCfg()->aimmovement = data->dropdown.value;
		weightyAimAimSettingsChanged(optionsGetExtMenuPlayer());
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = weightyAimMenuCfg()->aimmovement;
		break;
	}

	return 0;
}

/*
 * Aim Mode:
 *   Classic         the game's own aiming: the camera stops, the stick moves the crosshair
 *   Modern          keep looking around with Weighty Aim while aiming, standing still
 *   (Move While Aiming decides what can walk while aiming, in either mode)
 * Aim Down Sights brings the gun in and zooms, in any of them.
 */
struct menuitem g_WeightyAimAdsMenuItems[] = {
	WEIGHTYAIM_PRESET_ITEM,
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Aim Mode", 0, menuhandlerWeightyAimMode },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Controller: Move While Aiming", 0, menuhandlerWeightyAimMovement },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Keyboard: Move While Aiming", 0, menuhandlerWeightyAimKeyboardMove },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Aim Down Sights", 0, menuhandlerWeightyAimAds },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reticle While Aiming", 0, menuhandlerWeightyAimAimCrosshair }, // also on the Reticle page
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Laser Dot While Aiming", 0, menuhandlerWeightyAimAimLaserDot },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Laser Beam While Aiming", 0, menuhandlerWeightyAimAimLaserBeam },
	// order must match g_WeightyAimAdsSliders
	WEIGHTYAIM_SLIDER("Aim Sensitivity", 16),           // 20 - 100 %
	WEIGHTYAIM_SLIDER("Aim Feel While Aiming", 40),     // 0 - 200 %: how much the reticle keeps moving freely (above 100 % exaggerates it)
	WEIGHTYAIM_SLIDER("Sway While Aiming", 20),         // 0 - 100 %
	WEIGHTYAIM_SLIDER("Move Speed While Aiming", 16),   // 20 - 100 %
	WEIGHTYAIM_SLIDER("Sights Zoom", 40),               // 1 - 3x, any aiming
	WEIGHTYAIM_SLIDER("Sights Raise Time", 25),         // 0 - 0.5 s (Aim Down Sights)
	WEIGHTYAIM_SLIDER("Sights Gun Height", 60),         // -10 - +5 (Aim Down Sights)
	WEIGHTYAIM_BACK,
};

struct menudialogdef g_WeightyAimAdsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Aim Mode",
	g_WeightyAimAdsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* ------------------------------------------------------------------------
 * Gyro Aim
 */

static inline struct weightyaimgyrocfg *weightyAimMenuGyroCfg(void)
{
	return &g_WeightyAimGyroCfg[optionsGetExtMenuPlayer() & 3];
}

static void *weightyAimMenuGyroCfgVoid(void) { return weightyAimMenuGyroCfg(); }

static char *weightyAimGyroStatusLabel(void *item)
{
	return (char *)weightyAimGyroStatusText(optionsGetExtMenuPlayer());
}

// keeps calibration and the status line live while the game is paused
static s32 menudialogWeightyAimGyro(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_TICK) {
		weightyAimGyroMenuTick(optionsGetExtMenuPlayer());
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimGyroMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = WEIGHTYAIM_NUM_GYROMODES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)g_WeightyAimGyroModeNames[data->dropdown.value];
	case MENUOP_SET:
		weightyAimMenuGyroCfg()->mode = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = weightyAimMenuGyroCfg()->mode;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimGyroSpace(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = WEIGHTYAIM_NUM_GYROSPACES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)g_WeightyAimGyroSpaceNames[data->dropdown.value];
	case MENUOP_SET:
		weightyAimMenuGyroCfg()->space = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = weightyAimMenuGyroCfg()->space;
		break;
	}

	return 0;
}

#define WEIGHTYAIM_GYRO_CHECKBOX_HANDLER(fn, field) \
	static MenuItemHandlerResult fn(s32 operation, struct menuitem *item, union handlerdata *data) \
	{ \
		switch (operation) { \
		case MENUOP_GET: return weightyAimMenuGyroCfg()->field; \
		case MENUOP_SET: weightyAimMenuGyroCfg()->field = data->checkbox.value; break; \
		} \
		return 0; \
	}

WEIGHTYAIM_GYRO_CHECKBOX_HANDLER(menuhandlerWeightyAimGyroInvertY, inverty)
WEIGHTYAIM_GYRO_CHECKBOX_HANDLER(menuhandlerWeightyAimGyroAutoCal, autocalibrate)
WEIGHTYAIM_GYRO_CHECKBOX_HANDLER(menuhandlerWeightyAimGyroPauseStick, pausewithstick)

static MenuItemHandlerResult menuhandlerWeightyAimGyroCalibrate(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		weightyAimGyroStartCalibration(optionsGetExtMenuPlayer());
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimGyroReset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		weightyAimResetGyroDefaults(optionsGetExtMenuPlayer());
	}

	return 0;
}

#define GYROFIELD(f) offsetof(struct weightyaimgyrocfg, f)

static const struct weightyaimslider g_WeightyAimGyroSliders[] = {
	{ GYROFIELD(sensitivity), 0.1f,  0.1f, "%.1f",   NULL, 0, 0.f },
	{ GYROFIELD(vertical),    0.05f, 0.f,  "%.0f%%", NULL, 1, 0.f },
};

static const struct weightyaimslider g_WeightyAimGyroAdvSliders[] = {
	{ GYROFIELD(acceleration),   0.1f, 1.f, "%.1fx",     NULL,  0, 1.f },
	{ GYROFIELD(accelthreshold), 5.f,  5.f, "%.0f deg/s", NULL, 0, 0.f },
	{ GYROFIELD(tightening),     0.5f, 0.f, "%.1f deg/s", "Off", 0, 0.f },
	{ GYROFIELD(smoothing),      0.5f, 0.f, "%.1f deg/s", "Off", 0, 0.f },
};

struct menuitem g_WeightyAimGyroAdvMenuItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Gyro Space", 0, menuhandlerWeightyAimGyroSpace },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Invert Vertical", 0, menuhandlerWeightyAimGyroInvertY },
	// order must match g_WeightyAimGyroAdvSliders
	WEIGHTYAIM_SLIDER("Acceleration", 30),        // 1 - 4x
	WEIGHTYAIM_SLIDER("Accel. Full At", 60),      // 5 - 300 deg/s
	WEIGHTYAIM_SLIDER("Tightening", 40),          // 0 - 20 deg/s
	WEIGHTYAIM_SLIDER("Smoothing", 40),           // 0 - 20 deg/s
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Auto-Calibrate When Still", 0, menuhandlerWeightyAimGyroAutoCal },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Pause While Using Look Stick", 0, menuhandlerWeightyAimGyroPauseStick },
	WEIGHTYAIM_BACK,
};

struct menudialogdef g_WeightyAimGyroAdvMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Gyro Advanced",
	g_WeightyAimGyroAdvMenuItems,
	menudialogWeightyAimGyro,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_WeightyAimGyroMenuItems[] = {
	{ MENUITEMTYPE_LABEL, 0, 0, (uintptr_t)weightyAimGyroStatusLabel, 0, NULL },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Gyro Aim", 0, menuhandlerWeightyAimGyroMode },
	// order must match g_WeightyAimGyroSliders
	WEIGHTYAIM_SLIDER("Sensitivity", 100),        // 0.1 - 10
	WEIGHTYAIM_SLIDER("Vertical Sensitivity", 40), // 0 - 200 %
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Calibrate (hold still)\n", 0, menuhandlerWeightyAimGyroCalibrate },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_OPENSDIALOG, (uintptr_t)"Advanced...\n", 0, (void *)&g_WeightyAimGyroAdvMenuDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reset Gyro Settings\n", 0, menuhandlerWeightyAimGyroReset },
	WEIGHTYAIM_BACK,
};

struct menudialogdef g_WeightyAimGyroMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Gyro Aim",
	g_WeightyAimGyroMenuItems,
	menudialogWeightyAimGyro,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* ------------------------------------------------------------------------
 * Aim assist strength (main page)
 */

static void *weightyAimMenuAssistVoid(void) { return &g_WeightyAimAssistStrength[optionsGetExtMenuPlayer() & 3]; }

static const struct weightyaimslider g_WeightyAimAssistSliders[] = {
	{ 0, 0.05f, 0.f, "%.0f%%", "Off", 1, 0.f },
};

extern struct menuitem g_WeightyAimMenuItems[];

/* ------------------------------------------------------------------------
 * Shared slider handler
 */

static const struct weightyaimsliderpage g_WeightyAimSliderPages[] = {
	{ g_WeightyAimFeelMenuItems,  1, g_WeightyAimFeelSliders,  ARRAYCOUNT(g_WeightyAimFeelSliders),  weightyAimMenuCfgVoid,      weightyAimFeelChanged },
	{ g_WeightyAimStickMenuItems, 2, g_WeightyAimStickSliders, ARRAYCOUNT(g_WeightyAimStickSliders), weightyAimMenuStickCfgVoid, weightyAimStickChanged },
	{ g_WeightyAimBoostMenuItems, 1, g_WeightyAimBoostSliders, ARRAYCOUNT(g_WeightyAimBoostSliders), weightyAimMenuStickCfgVoid, NULL },
	{ g_WeightyAimGyroMenuItems,  2, g_WeightyAimGyroSliders,    ARRAYCOUNT(g_WeightyAimGyroSliders),    weightyAimMenuGyroCfgVoid,  NULL },
	{ g_WeightyAimGyroAdvMenuItems, 2, g_WeightyAimGyroAdvSliders, ARRAYCOUNT(g_WeightyAimGyroAdvSliders), weightyAimMenuGyroCfgVoid, NULL },
	{ g_WeightyAimMenuItems,     12, g_WeightyAimAssistSliders,  ARRAYCOUNT(g_WeightyAimAssistSliders),  weightyAimMenuAssistVoid,   NULL },
	{ g_WeightyAimAdsMenuItems,   8, g_WeightyAimAdsSliders,   ARRAYCOUNT(g_WeightyAimAdsSliders),   weightyAimMenuCfgVoid,      weightyAimFeelChanged },
};

static MenuItemHandlerResult menuhandlerWeightyAimSlider(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const struct weightyaimsliderpage *page = NULL;
	const struct weightyaimslider *sl;
	s32 index = -1;
	f32 *field;
	f32 value;

	for (s32 p = 0; p < (s32)ARRAYCOUNT(g_WeightyAimSliderPages); p++) {
		const struct weightyaimsliderpage *pg = &g_WeightyAimSliderPages[p];
		const s32 i = (s32)(item - pg->items) - pg->firstslider;

		if (item >= pg->items && i >= 0 && i < pg->numsliders) {
			page = pg;
			index = i;
			break;
		}
	}

	if (!page) {
		return 0;
	}

	sl = &page->sliders[index];
	field = (f32 *)((u8 *)page->getcfg() + sl->offset);

	switch (operation) {
	case MENUOP_CHECKHIDDEN:
	case MENUOP_CHECKDISABLED:
		// Aim & Camera Feel: fine-tuning sliders only show with "Show Advanced Feel"
		if (page->items == g_WeightyAimFeelMenuItems && index >= WEIGHTYAIM_FEEL_NUM_MAIN
				&& !g_WeightyAimShowAdvancedFeel) {
			return true;
		}
		break;
	case MENUOP_GETSLIDER:
		value = (*field - sl->base) / sl->step + 0.5f;
		data->slider.value = value > 0.f ? (u32)value : 0;
		break;
	case MENUOP_SET:
		value = sl->base + data->slider.value * sl->step;
		*field = value < sl->min ? sl->min : value;
		if (page->onchange) {
			page->onchange(index);
		}
		break;
	case MENUOP_GETSLIDERLABEL:
		value = sl->base + data->slider.value * sl->step;
		if (value < sl->min) {
			value = sl->min;
		}
		if (sl->zerolabel && value <= 0.f && sl->base >= 0.f) {
			strcpy(data->slider.label, sl->zerolabel);
		} else {
			sprintf(data->slider.label, sl->fmt, sl->percent ? value * 100.f : value);
		}
		break;
	}

	return 0;
}

/* ------------------------------------------------------------------------
 * Main Weighty Aim page
 */

// menu order (saved numbers stay as they were, so Arcade is listed third
// although it was added last)
static const s32 g_WeightyAimPresetOrder[WEIGHTYAIM_NUM_PRESETS] = {
	WEIGHTYAIM_PRESET_WEIGHTY,
	WEIGHTYAIM_PRESET_IMMERSIVE,
	WEIGHTYAIM_PRESET_ARCADE,
	WEIGHTYAIM_PRESET_BORING,
	WEIGHTYAIM_PRESET_CLASSIC,
	WEIGHTYAIM_PRESET_CUSTOM1,
	WEIGHTYAIM_PRESET_CUSTOM2,
	WEIGHTYAIM_PRESET_CUSTOM3,
};

static MenuItemHandlerResult menuhandlerWeightyAimPreset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = WEIGHTYAIM_NUM_PRESETS;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)g_WeightyAimPresetNames[g_WeightyAimPresetOrder[data->dropdown.value % WEIGHTYAIM_NUM_PRESETS]];
	case MENUOP_SET:
		weightyAimApplyPreset(optionsGetExtMenuPlayer(), g_WeightyAimPresetOrder[data->dropdown.value % WEIGHTYAIM_NUM_PRESETS]);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;
		for (s32 i = 0; i < WEIGHTYAIM_NUM_PRESETS; i++) {
			if (g_WeightyAimPresetOrder[i] == weightyAimMenuCfg()->preset) {
				data->dropdown.value = i;
				break;
			}
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimCrosshair(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"On",
		"Off",
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		weightyAimMenuCfg()->crosshair = data->dropdown.value;
		weightyAimAimSettingsChanged(optionsGetExtMenuPlayer());
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = weightyAimMenuCfg()->crosshair;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimLaser(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return weightyAimMenuCfg()->laser;
	case MENUOP_SET:
		weightyAimMenuCfg()->laser = data->checkbox.value;
		weightyAimAimSettingsChanged(optionsGetExtMenuPlayer());
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimLaserDot(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return weightyAimMenuCfg()->laserdot;
	case MENUOP_SET:
		weightyAimMenuCfg()->laserdot = data->checkbox.value;
		weightyAimAimSettingsChanged(optionsGetExtMenuPlayer());
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimLaserPersist(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return weightyAimMenuCfg()->laserpersist;
	case MENUOP_SET:
		weightyAimMenuCfg()->laserpersist = data->checkbox.value;
		weightyAimAimSettingsChanged(optionsGetExtMenuPlayer());
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimDebugLog(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_WeightyAimDebugLog;
	case MENUOP_SET:
		g_WeightyAimDebugLog = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimReset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		weightyAimResetDefaults(optionsGetExtMenuPlayer());
	}

	return 0;
}

#define WEIGHTYAIM_SUBPAGE(label, dialog) \
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_OPENSDIALOG, (uintptr_t)(label), 0, (void *)&(dialog) }

/* ------------------------------------------------------------------------
 * Reticle
 */

// the port's own reticle settings (Extended -> Game), shared here
extern MenuItemHandlerResult menuhandlerCrosshairSize(s32 operation, struct menuitem *item, union handlerdata *data);
extern MenuItemHandlerResult menuhandlerCrosshairHealth(s32 operation, struct menuitem *item, union handlerdata *data);
extern struct menudialogdef g_ExtendedGameCrosshairColourMenuDialog;

// Reticle opacity: the alpha byte of the port's reticle colour, 0 - 100 %
static MenuItemHandlerResult menuhandlerWeightyAimReticleOpacity(s32 operation, struct menuitem *item, union handlerdata *data)
{
	u32 *colour = &g_PlayerExtCfg[optionsGetExtMenuPlayer() & 3].crosshaircolour;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = ((*colour & 0xff) * 20 + 127) / 255;
		break;
	case MENUOP_SET:
		*colour = (*colour & 0xffffff00) | (u32)((data->slider.value * 255 + 10) / 20);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d%%", data->slider.value * 5);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimSmoothReticle(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_WeightyAimSmoothReticle[optionsGetExtMenuPlayer() & 3];
	case MENUOP_SET:
		g_WeightyAimSmoothReticle[optionsGetExtMenuPlayer() & 3] = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

struct menuitem g_WeightyAimReticleMenuItems[] = {
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Hip-Fire Reticle", 0, menuhandlerWeightyAimCrosshair },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reticle While Aiming", 0, menuhandlerWeightyAimAimCrosshair },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Reticle Size", 4, menuhandlerCrosshairSize },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)"Reticle Opacity", 20, menuhandlerWeightyAimReticleOpacity },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Smooth Reticle", 0, menuhandlerWeightyAimSmoothReticle },
	WEIGHTYAIM_SUBPAGE("Reticle Colour...\n", g_ExtendedGameCrosshairColourMenuDialog),
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reticle Colour by Health", 0, menuhandlerCrosshairHealth },
	WEIGHTYAIM_BACK,
};

struct menudialogdef g_WeightyAimReticleMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Reticle",
	g_WeightyAimReticleMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerWeightyAimForceOriginal(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_WeightyAimForceOriginal;
	case MENUOP_SET:
		g_WeightyAimForceOriginal = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimForceMouseGyroStick(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_WeightyAimForceMouseGyroStick;
	case MENUOP_SET:
		g_WeightyAimForceMouseGyroStick = data->checkbox.value ? 1 : 0;
		break;
	case MENUOP_CHECKHIDDEN:
	case MENUOP_CHECKDISABLED:
		// only means something with Force Original Aim & Settings on
		return !g_WeightyAimForceOriginal;
	}

	return 0;
}

struct menuitem g_WeightyAimMenuItems[] = {
	WEIGHTYAIM_PRESET_ITEM,
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	WEIGHTYAIM_SUBPAGE("Aim & Camera Feel...\n", g_WeightyAimFeelMenuDialog),
	WEIGHTYAIM_SUBPAGE("Stick Response...\n", g_WeightyAimStickMenuDialog),
	WEIGHTYAIM_SUBPAGE("Look Acceleration...\n", g_WeightyAimBoostMenuDialog),
	WEIGHTYAIM_SUBPAGE("Aim Mode...\n", g_WeightyAimAdsMenuDialog),
	WEIGHTYAIM_SUBPAGE("Gyro Aim...\n", g_WeightyAimGyroMenuDialog),
	WEIGHTYAIM_SUBPAGE("Reticle...\n", g_WeightyAimReticleMenuDialog),
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Hip-Fire Laser Beam", 0, menuhandlerWeightyAimLaser },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Hip-Fire Laser Dot", 0, menuhandlerWeightyAimLaserDot },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Laser Dot Stays While Firing", 0, menuhandlerWeightyAimLaserPersist },
	WEIGHTYAIM_SLIDER("Aim Assist", 20),     // 0 - 100 % of the game's own (index must match g_WeightyAimSliderPages)

	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Debug Log", 0, menuhandlerWeightyAimDebugLog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reset to Weighty Preset\n", 0, menuhandlerWeightyAimReset },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	// for tournaments: 1:1 with the original game for every player, whatever their settings
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Force Original Aim & Settings", 0, menuhandlerWeightyAimForceOriginal },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Force Mouse & Gyro as Stick", 0, menuhandlerWeightyAimForceMouseGyroStick },
	WEIGHTYAIM_BACK,
};

// "Player 1" at the start: the player select dialog overwrites character 7
static char g_WeightyAimMenuTitle[] = "Player 1 Weighty Aim";

struct menudialogdef g_WeightyAimMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)g_WeightyAimMenuTitle,
	g_WeightyAimMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
