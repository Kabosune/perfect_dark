#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/menu.h"
#include "weightyaim.h"

/*
 * Options -> Extended -> Weighty Aim
 *
 *   Preset
 *   Aim & Camera Feel...   free-aim zone, gun weight, camera lead, sway
 *   Stick Response...      look curve, deadzones, max turn speed
 *   Turn Boost...          extra turn speed at full stick
 *   Aim Down Sights...     raising the gun to your eye when holding aim
 *   Crosshair / Laser Sight / Debug Log / Reset
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

static const struct weightyaimslider g_WeightyAimFeelSliders[] = {
	{ AIMFIELD(deadzonex),     0.5f,  0.f,  "%.1f deg", NULL,  0 },
	{ AIMFIELD(deadzoney),     0.5f,  0.f,  "%.1f deg", NULL,  0 },
	{ AIMFIELD(camerashare),   0.05f, 0.f,  "%.0f%%",   NULL,  1 },
	{ AIMFIELD(cameralead),    0.1f,  0.f,  "%.1f",     "Off", 0 },
	{ AIMFIELD(edgesmoothing), 0.02f, 0.f,  "%.2fs",    "Off", 0 },
	{ AIMFIELD(recenterspeed), 0.1f,  0.f,  "%.1f",     "Off", 0 },
	{ AIMFIELD(recenterdelay), 0.05f, 0.f,  "%.2fs",    NULL,  0 },
	{ AIMFIELD(gunresponse),   0.5f,  1.f,  "%.1f Hz",  NULL,  0 },
	{ AIMFIELD(gundamping),    0.05f, 0.1f, "%.2f",     NULL,  0 },
	{ AIMFIELD(turndrag),      0.05f, 0.f,  "%.2f",     NULL,  0 },
	{ AIMFIELD(camerasway),    0.05f, 0.f,  "%.2f deg", "Off", 0 },
	{ AIMFIELD(walksway),      0.1f,  0.f,  "%.1f deg", "Off", 0 },
};

struct menuitem g_WeightyAimFeelMenuItems[] = {
	WEIGHTYAIM_PRESET_ITEM,
	// order must match g_WeightyAimFeelSliders
	WEIGHTYAIM_SLIDER("Free-Aim Zone Width", 40),   // 0 - 20 deg
	WEIGHTYAIM_SLIDER("Free-Aim Zone Height", 30),  // 0 - 15 deg
	WEIGHTYAIM_SLIDER("Camera Share", 20),          // 0 - 100 %
	WEIGHTYAIM_SLIDER("Camera Lead", 30),           // 0 - 3
	WEIGHTYAIM_SLIDER("Edge Smoothing", 25),        // 0 - 0.5 s
	WEIGHTYAIM_SLIDER("Camera Catch-Up", 50),       // 0 - 5
	WEIGHTYAIM_SLIDER("Catch-Up Delay", 40),        // 0 - 2 s
	WEIGHTYAIM_SLIDER("Gun Response", 40),          // 1 - 20 Hz
	WEIGHTYAIM_SLIDER("Gun Damping", 30),           // 0.1 - 1.5
	WEIGHTYAIM_SLIDER("Turn Drag", 20),             // 0 - 1
	WEIGHTYAIM_SLIDER("Camera Sway", 30),           // 0 - 1.5 deg
	WEIGHTYAIM_SLIDER("Walk Sway", 30),             // 0 - 3 deg
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
	{ STICKFIELD(innerdeadzone), 0.01f, 0.f,   "%.0f%%", NULL, 1 },
	{ STICKFIELD(outerdeadzone), 0.01f, 0.1f,  "%.0f%%", NULL, 1 },
	{ STICKFIELD(turnspeed),     0.05f, 0.25f, "%.2fx",  NULL, 0 },
	{ STICKFIELD(bezier[0]),     0.05f, 0.f,   "%.2f",   NULL, 0 },
	{ STICKFIELD(bezier[1]),     0.05f, 0.f,   "%.2f",   NULL, 0 },
	{ STICKFIELD(bezier[2]),     0.05f, 0.f,   "%.2f",   NULL, 0 },
	{ STICKFIELD(bezier[3]),     0.05f, 0.f,   "%.2f",   NULL, 0 },
};

#define STICK_FIRST_BEZIER_SLIDER 3

static void weightyAimStickChanged(s32 sliderindex)
{
	// moving a curve point switches to a custom curve and saves the points there
	if (sliderindex >= STICK_FIRST_BEZIER_SLIDER) {
		weightyAimCurvePointsChanged(optionsGetExtMenuPlayer());
	}
}

static MenuItemHandlerResult menuhandlerWeightyAimCurve(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = WEIGHTYAIM_NUM_CURVES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)g_WeightyAimCurveNames[data->dropdown.value];
	case MENUOP_SET:
		weightyAimSelectCurve(optionsGetExtMenuPlayer(), data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = weightyAimMenuStickCfg()->curve;
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
	// order must match g_WeightyAimStickSliders
	WEIGHTYAIM_SLIDER("Inner Deadzone", 40),     // 0 - 40 %
	WEIGHTYAIM_SLIDER("Outer Deadzone", 100),    // 10 - 100 %
	WEIGHTYAIM_SLIDER("Max Turn Speed", 50),     // 0.25 - 2.5x
	WEIGHTYAIM_SLIDER("Custom Curve X1", 20),    // 0 - 1
	WEIGHTYAIM_SLIDER("Custom Curve Y1", 20),
	WEIGHTYAIM_SLIDER("Custom Curve X2", 20),
	WEIGHTYAIM_SLIDER("Custom Curve Y2", 20),
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
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Turn Boost", 0, menuhandlerWeightyAimBoostMode },
	// order must match g_WeightyAimBoostSliders
	WEIGHTYAIM_SLIDER("Boost Speed", 60),        // 1 - 3x
	WEIGHTYAIM_SLIDER("Stick Threshold", 100),   // 50 - 100 %
	WEIGHTYAIM_SLIDER("Boost Delay", 25),        // 0 - 0.5 s (Ramped)
	WEIGHTYAIM_SLIDER("Ramp-Up Time", 20),       // 0 - 1 s (Ramped)
	WEIGHTYAIM_SLIDER("Vertical Boost", 20),     // 0 - 100 %
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reset Turn Boost\n", 0, menuhandlerWeightyAimBoostReset },
	WEIGHTYAIM_BACK,
};

struct menudialogdef g_WeightyAimBoostMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Turn Boost",
	g_WeightyAimBoostMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* ------------------------------------------------------------------------
 * Aim Down Sights
 */

static const struct weightyaimslider g_WeightyAimAdsSliders[] = {
	{ AIMFIELD(adszoom),   0.05f, 1.f,   "%.2fx",  NULL,   0, 1.f },
	{ AIMFIELD(adstime),   0.02f, 0.f,   "%.2fs",  "Instant", 0, 0.f },
	{ AIMFIELD(adssway),   0.05f, 0.f,   "%.0f%%", "None", 1, 0.f },
	{ AIMFIELD(adszone),   0.05f, 0.f,   "%.0f%%", "None", 1, 0.f },
	{ AIMFIELD(adsheight), 0.25f, -5.f,  "%+.2f",  NULL,   0, -5.f },
};

static MenuItemHandlerResult menuhandlerWeightyAimAds(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return weightyAimMenuCfg()->ads;
	case MENUOP_SET:
		weightyAimMenuCfg()->ads = data->checkbox.value;
		weightyAimAimSettingsChanged(optionsGetExtMenuPlayer());
		break;
	}

	return 0;
}

struct menuitem g_WeightyAimAdsMenuItems[] = {
	WEIGHTYAIM_PRESET_ITEM,
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Aim Down Sights", 0, menuhandlerWeightyAimAds },
	// order must match g_WeightyAimAdsSliders
	WEIGHTYAIM_SLIDER("Zoom", 40),                  // 1 - 3x
	WEIGHTYAIM_SLIDER("Raise Time", 25),            // 0 - 0.5 s
	WEIGHTYAIM_SLIDER("Sway While Aiming", 20),     // 0 - 100 %
	WEIGHTYAIM_SLIDER("Free-Aim While Aiming", 20), // 0 - 100 %
	WEIGHTYAIM_SLIDER("Sight Height", 40),          // -5 - +5
	WEIGHTYAIM_BACK,
};

struct menudialogdef g_WeightyAimAdsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Aim Down Sights",
	g_WeightyAimAdsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* ------------------------------------------------------------------------
 * Shared slider handler
 */

static const struct weightyaimsliderpage g_WeightyAimSliderPages[] = {
	{ g_WeightyAimFeelMenuItems,  1, g_WeightyAimFeelSliders,  ARRAYCOUNT(g_WeightyAimFeelSliders),  weightyAimMenuCfgVoid,      weightyAimFeelChanged },
	{ g_WeightyAimStickMenuItems, 1, g_WeightyAimStickSliders, ARRAYCOUNT(g_WeightyAimStickSliders), weightyAimMenuStickCfgVoid, weightyAimStickChanged },
	{ g_WeightyAimBoostMenuItems, 1, g_WeightyAimBoostSliders, ARRAYCOUNT(g_WeightyAimBoostSliders), weightyAimMenuStickCfgVoid, NULL },
	{ g_WeightyAimAdsMenuItems,   2, g_WeightyAimAdsSliders,   ARRAYCOUNT(g_WeightyAimAdsSliders),   weightyAimMenuCfgVoid,      weightyAimFeelChanged },
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

static MenuItemHandlerResult menuhandlerWeightyAimPreset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = WEIGHTYAIM_NUM_PRESETS;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)g_WeightyAimPresetNames[data->dropdown.value];
	case MENUOP_SET:
		weightyAimApplyPreset(optionsGetExtMenuPlayer(), data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = weightyAimMenuCfg()->preset;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimCrosshair(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Always",
		"Only When Aiming",
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

struct menuitem g_WeightyAimMenuItems[] = {
	WEIGHTYAIM_PRESET_ITEM,
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	WEIGHTYAIM_SUBPAGE("Aim & Camera Feel...\n", g_WeightyAimFeelMenuDialog),
	WEIGHTYAIM_SUBPAGE("Stick Response...\n", g_WeightyAimStickMenuDialog),
	WEIGHTYAIM_SUBPAGE("Turn Boost...\n", g_WeightyAimBoostMenuDialog),
	WEIGHTYAIM_SUBPAGE("Aim Down Sights...\n", g_WeightyAimAdsMenuDialog),
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Crosshair", 0, menuhandlerWeightyAimCrosshair },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Laser Sight", 0, menuhandlerWeightyAimLaser },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Debug Log", 0, menuhandlerWeightyAimDebugLog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Reset to Weighty Preset\n", 0, menuhandlerWeightyAimReset },
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
