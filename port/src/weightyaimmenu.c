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
 * "Weighty Aim" page under Options -> Extended.
 *
 * Every slider is described by one row in g_WeightyAimSliders, so adding a new
 * tunable is: add a field to struct weightyaimcfg, register it in weightyaim.c,
 * add a row here and a menu item below.
 */

extern s32 optionsGetExtMenuPlayer(void);

struct weightyaimslider {
	size_t offset;    // field in struct weightyaimcfg
	f32 step;         // value of one slider notch
	f32 min;          // lowest allowed value
	const char *fmt;  // printf format for the value shown next to the slider
	const char *zerolabel; // shown instead of the number when the value is 0 (optional)
};

#define CFGFIELD(f) offsetof(struct weightyaimcfg, f)

// order must match the slider items in g_WeightyAimMenuItems
static const struct weightyaimslider g_WeightyAimSliders[] = {
	{ CFGFIELD(deadzonex),     0.5f,  0.f,  "%.1f deg", NULL },
	{ CFGFIELD(deadzoney),     0.5f,  0.f,  "%.1f deg", NULL },
	{ CFGFIELD(stickaimspeed), 0.05f, 0.f,  "%.2fx",    NULL },
	{ CFGFIELD(mouseaimspeed), 0.05f, 0.f,  "%.2fx",    NULL },
	{ CFGFIELD(recenterspeed), 0.1f,  0.f,  "%.1f",     "Off" },
	{ CFGFIELD(recenterdelay), 0.05f, 0.f,  "%.2fs",    NULL },
	{ CFGFIELD(gunresponse),   0.5f,  1.f,  "%.1f Hz",  NULL },
	{ CFGFIELD(gundamping),    0.05f, 0.1f, "%.2f",     NULL },
	{ CFGFIELD(turndrag),      0.05f, 0.f,  "%.2f",     NULL },
};

#define FIRST_SLIDER_ITEM 1

extern struct menuitem g_WeightyAimMenuItems[];

static inline struct weightyaimcfg *weightyAimMenuCfg(void)
{
	return &g_WeightyAimCfg[optionsGetExtMenuPlayer() & 3];
}

static MenuItemHandlerResult menuhandlerWeightyAimSlider(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 index = (s32)(item - g_WeightyAimMenuItems) - FIRST_SLIDER_ITEM;
	const struct weightyaimslider *sl;
	f32 *field;
	f32 value;

	if (index < 0 || index >= (s32)ARRAYCOUNT(g_WeightyAimSliders)) {
		return 0;
	}

	sl = &g_WeightyAimSliders[index];
	field = (f32 *)((u8 *)weightyAimMenuCfg() + sl->offset);

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (u32)(*field / sl->step + 0.5f);
		break;
	case MENUOP_SET:
		value = data->slider.value * sl->step;
		*field = value < sl->min ? sl->min : value;
		break;
	case MENUOP_GETSLIDERLABEL:
		value = data->slider.value * sl->step;
		if (value < sl->min) {
			value = sl->min;
		}
		if (sl->zerolabel && value <= 0.f) {
			strcpy(data->slider.label, sl->zerolabel);
		} else {
			sprintf(data->slider.label, sl->fmt, value);
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerWeightyAimEnabled(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return weightyAimMenuCfg()->enabled;
	case MENUOP_SET:
		weightyAimMenuCfg()->enabled = data->checkbox.value;
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

#define WEIGHTYAIM_SLIDER(label, notches) \
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE, (uintptr_t)(label), (notches), menuhandlerWeightyAimSlider }

struct menuitem g_WeightyAimMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Weighty Aim Enabled",
		0,
		menuhandlerWeightyAimEnabled,
	},
	// sliders: order must match g_WeightyAimSliders
	WEIGHTYAIM_SLIDER("Free-Aim Zone Width", 40),     // 0 - 20 deg
	WEIGHTYAIM_SLIDER("Free-Aim Zone Height", 30),    // 0 - 15 deg
	WEIGHTYAIM_SLIDER("Gun Speed (Stick)", 40),       // 0 - 2x
	WEIGHTYAIM_SLIDER("Gun Speed (Mouse)", 40),       // 0 - 2x
	WEIGHTYAIM_SLIDER("Camera Catch-Up", 50),         // 0 - 5
	WEIGHTYAIM_SLIDER("Catch-Up Delay", 40),          // 0 - 2 s
	WEIGHTYAIM_SLIDER("Gun Response", 40),            // 1 - 20 Hz
	WEIGHTYAIM_SLIDER("Gun Damping", 30),             // 0.1 - 1.5
	WEIGHTYAIM_SLIDER("Turn Drag", 20),               // 0 - 1
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Debug Log",
		0,
		menuhandlerWeightyAimDebugLog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Reset to Defaults\n",
		0,
		menuhandlerWeightyAimReset,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
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
