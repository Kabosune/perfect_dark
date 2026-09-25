#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "platform.h"
#include "data.h"
#include "bss.h"
#include "types.h"
#include "game/menu.h"
#include "game/game_1531a0.h"
#include "lib/joy.h"
#include "lib/vi.h"
#include "gbiex.h"
#include "config.h"

/*
 * Options -> Extended -> Tetris
 *
 * A Tetris game drawn inside a menu dialog. The playfield is a single
 * MENUITEMTYPE_CUSTOMRENDER item (the same hook the Weighty Aim stick curve
 * graph uses), and the game runs from the dialog's MENUOP_TICK handler,
 * which reads the pads directly and then clears the menu inputs so the menu
 * doesn't also try to navigate. B / Esc leaves; the game stays where it was
 * and is paused when you come back.
 *
 * Rules follow the modern guideline: 7-bag randomiser, SRS rotation with wall
 * kicks, hold, 3-piece preview, ghost piece, lock delay with 15 move resets,
 * 100/300/500/800 x level for 1-4 lines, 1 per soft-dropped row and 2 per
 * hard-dropped row, a level every 10 lines.
 */

#define TET_W      10
#define TET_H      22 // two hidden rows above the visible 20
#define TET_HIDDEN 2
#define TET_NEXT   3

#define TET_DAS        10.f // frames (60 Hz) before a held left/right repeats
#define TET_ARR        2.f  // frames between repeats
#define TET_LOCKDELAY  30.f
#define TET_MAXRESETS  15
#define TET_CLEARTIME  18.f
#define TET_MAXLEVEL   30

#define TET_ITEM_HEIGHT 132

enum {
	TETSTATE_TITLE,
	TETSTATE_PLAYING,
	TETSTATE_CLEARING,
	TETSTATE_PAUSED,
	TETSTATE_GAMEOVER,
};

// virtual buttons, built from whatever the pads and binds produce
#define TB_LEFT    0x0001
#define TB_RIGHT   0x0002
#define TB_DOWN    0x0004
#define TB_UP      0x0008
#define TB_CW      0x0010
#define TB_CCW     0x0020
#define TB_HOLD    0x0040
#define TB_CONFIRM 0x0080
#define TB_START   0x0100

struct tetris {
	u8 board[TET_H][TET_W]; // 0 = empty, else piece + 1
	s8 state;
	s8 piece;
	s8 rot;
	s8 px;
	s8 py;
	s8 hold;                 // -1 = empty
	bool canhold;
	u8 queue[TET_NEXT];
	u8 bag[7];
	s8 bagleft;
	f32 fall;                // progress toward the next gravity row
	f32 lock;                // frames spent resting on the stack
	s32 lockresets;
	s32 lowesty;
	s8 dasdir;
	f32 dastimer;
	f32 statetimer;          // line clear animation / game over input delay
	u32 clearmask;
	s32 score;
	s32 lines;
	s32 level;
	u32 prevbuttons;
	bool wascurrent;
	u32 rng;
	f32 anim;                // free running, for blinking text
};

static struct tetris g_Tetris[MAX_PLAYERS];
static s32 g_TetrisHighScore = 0;

PD_CONSTRUCTOR static void tetrisConfigInit(void)
{
	configRegisterInt("Game.TetrisHighScore", &g_TetrisHighScore, 0, 999999999);
}

/**
 * Pieces in SRS spawn orientation: I O T S Z J L.
 * Each rotates inside a box of g_TetBox[piece] cells (0 = doesn't rotate).
 */
static const s8 g_TetCells[7][4][2] = {
	{ { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } }, // I
	{ { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } }, // O
	{ { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }, // T
	{ { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } }, // S
	{ { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } }, // Z
	{ { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }, // J
	{ { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }, // L
};

static const s8 g_TetBox[7] = { 4, 0, 3, 3, 3, 3, 3 };

static const u32 g_TetColours[7] = {
	0x40e0ffff, // I cyan
	0xffe040ff, // O yellow
	0xc060ffff, // T purple
	0x50e050ff, // S green
	0xff5050ff, // Z red
	0x5080ffff, // J blue
	0xffa030ff, // L orange
};

/**
 * SRS wall kicks as (x, y) with y up, in the order
 * 0->R, R->0, R->2, 2->R, 2->L, L->2, L->0, 0->L.
 */
static const s8 g_TetKicks[8][5][2] = {
	{ { 0, 0 }, { -1, 0 }, { -1,  1 }, { 0, -2 }, { -1, -2 } },
	{ { 0, 0 }, {  1, 0 }, {  1, -1 }, { 0,  2 }, {  1,  2 } },
	{ { 0, 0 }, {  1, 0 }, {  1, -1 }, { 0,  2 }, {  1,  2 } },
	{ { 0, 0 }, { -1, 0 }, { -1,  1 }, { 0, -2 }, { -1, -2 } },
	{ { 0, 0 }, {  1, 0 }, {  1,  1 }, { 0, -2 }, {  1, -2 } },
	{ { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0,  2 }, { -1,  2 } },
	{ { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0,  2 }, { -1,  2 } },
	{ { 0, 0 }, {  1, 0 }, {  1,  1 }, { 0, -2 }, {  1, -2 } },
};

static const s8 g_TetKicksI[8][5][2] = {
	{ { 0, 0 }, { -2, 0 }, {  1, 0 }, { -2, -1 }, {  1,  2 } },
	{ { 0, 0 }, {  2, 0 }, { -1, 0 }, {  2,  1 }, { -1, -2 } },
	{ { 0, 0 }, { -1, 0 }, {  2, 0 }, { -1,  2 }, {  2, -1 } },
	{ { 0, 0 }, {  1, 0 }, { -2, 0 }, {  1, -2 }, { -2,  1 } },
	{ { 0, 0 }, {  2, 0 }, { -1, 0 }, {  2,  1 }, { -1, -2 } },
	{ { 0, 0 }, { -2, 0 }, {  1, 0 }, { -2, -1 }, {  1,  2 } },
	{ { 0, 0 }, {  1, 0 }, { -2, 0 }, {  1, -2 }, { -2,  1 } },
	{ { 0, 0 }, { -1, 0 }, {  2, 0 }, { -1,  2 }, {  2, -1 } },
};

// frames per row at 60 Hz, by level (1 is the first entry)
static const u8 g_TetGravity[] = {
	48, 43, 38, 33, 28, 23, 18, 13, 8, 6,
	5, 5, 5, 4, 4, 4, 3, 3, 3, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 1,
};

static struct tetris *tetrisGet(void)
{
	return &g_Tetris[g_MpPlayerNum & (MAX_PLAYERS - 1)];
}

static u32 tetrisRandom(struct tetris *t)
{
	u32 x = t->rng ? t->rng : 0x2545f491;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	t->rng = x;

	return x;
}

static void tetrisCell(s32 piece, s32 rot, s32 i, s32 *x, s32 *y)
{
	s32 cx = g_TetCells[piece][i][0];
	s32 cy = g_TetCells[piece][i][1];
	const s32 n = g_TetBox[piece];

	if (n) {
		for (s32 r = 0; r < (rot & 3); r++) {
			const s32 tmp = cx;
			cx = n - 1 - cy;
			cy = tmp;
		}
	}

	*x = cx;
	*y = cy;
}

static bool tetrisFits(struct tetris *t, s32 piece, s32 rot, s32 px, s32 py)
{
	for (s32 i = 0; i < 4; i++) {
		s32 x, y;
		tetrisCell(piece, rot, i, &x, &y);
		x += px;
		y += py;

		if (x < 0 || x >= TET_W || y >= TET_H) {
			return false;
		}

		if (y >= 0 && t->board[y][x]) {
			return false;
		}
	}

	return true;
}

static s32 tetrisDrawFromBag(struct tetris *t)
{
	if (t->bagleft <= 0) {
		for (s32 i = 0; i < 7; i++) {
			t->bag[i] = i;
		}

		for (s32 i = 6; i > 0; i--) {
			const s32 j = tetrisRandom(t) % (i + 1);
			const u8 tmp = t->bag[i];
			t->bag[i] = t->bag[j];
			t->bag[j] = tmp;
		}

		t->bagleft = 7;
	}

	return t->bag[--t->bagleft];
}

static s32 tetrisTakeNext(struct tetris *t)
{
	const s32 piece = t->queue[0];

	for (s32 i = 0; i < TET_NEXT - 1; i++) {
		t->queue[i] = t->queue[i + 1];
	}

	t->queue[TET_NEXT - 1] = tetrisDrawFromBag(t);

	return piece;
}

static s32 tetrisGhostY(struct tetris *t)
{
	s32 y = t->py;

	while (tetrisFits(t, t->piece, t->rot, t->px, y + 1)) {
		y++;
	}

	return y;
}

static void tetrisGameOver(struct tetris *t)
{
	t->state = TETSTATE_GAMEOVER;
	t->statetimer = 0.f;

	if (t->score > g_TetrisHighScore) {
		g_TetrisHighScore = t->score;
	}

	menuPlaySound(MENUSOUND_ERROR);
}

static void tetrisSpawn(struct tetris *t, s32 piece)
{
	t->piece = piece;
	t->rot = 0;
	t->px = 3;
	t->py = 0;
	t->fall = 0.f;
	t->lock = 0.f;
	t->lockresets = 0;

	if (!tetrisFits(t, t->piece, t->rot, t->px, t->py)) {
		tetrisGameOver(t);
		return;
	}

	// the guideline drops a fresh piece one row straight away if it can
	if (tetrisFits(t, t->piece, t->rot, t->px, t->py + 1)) {
		t->py++;
	}

	t->lowesty = t->py;
}

static void tetrisNewGame(struct tetris *t)
{
	const u32 seed = t->rng ^ (osGetCount() * 2654435761u);

	memset(t->board, 0, sizeof(t->board));
	t->rng = seed ? seed : 1;
	t->bagleft = 0;
	t->hold = -1;
	t->canhold = true;
	t->score = 0;
	t->lines = 0;
	t->level = 1;
	t->dasdir = 0;
	t->dastimer = 0.f;
	t->clearmask = 0;
	t->state = TETSTATE_PLAYING;

	for (s32 i = 0; i < TET_NEXT; i++) {
		t->queue[i] = tetrisDrawFromBag(t);
	}

	tetrisSpawn(t, tetrisTakeNext(t));
}

// called after any successful move or rotation
static void tetrisMoved(struct tetris *t)
{
	if (!tetrisFits(t, t->piece, t->rot, t->px, t->py + 1) && t->lockresets < TET_MAXRESETS) {
		t->lock = 0.f;
		t->lockresets++;
	}
}

static bool tetrisShift(struct tetris *t, s32 dir)
{
	if (tetrisFits(t, t->piece, t->rot, t->px + dir, t->py)) {
		t->px += dir;
		tetrisMoved(t);
		return true;
	}

	return false;
}

static void tetrisRotate(struct tetris *t, s32 dir)
{
	const s32 from = t->rot;
	const s32 to = (from + dir) & 3;
	const s32 idx = dir > 0 ? from * 2 : (from * 2 + 7) & 7;
	const s8 (*kicks)[2] = t->piece == 0 ? g_TetKicksI[idx] : g_TetKicks[idx];

	if (g_TetBox[t->piece] == 0) {
		return;
	}

	for (s32 k = 0; k < 5; k++) {
		const s32 nx = t->px + kicks[k][0];
		const s32 ny = t->py - kicks[k][1];

		if (tetrisFits(t, t->piece, to, nx, ny)) {
			t->px = nx;
			t->py = ny;
			t->rot = to;
			tetrisMoved(t);
			return;
		}
	}
}

static void tetrisLockPiece(struct tetris *t)
{
	bool visible = false;
	s32 numlines = 0;

	for (s32 i = 0; i < 4; i++) {
		s32 x, y;
		tetrisCell(t->piece, t->rot, i, &x, &y);
		x += t->px;
		y += t->py;

		if (y >= 0 && y < TET_H) {
			t->board[y][x] = t->piece + 1;
		}

		if (y >= TET_HIDDEN) {
			visible = true;
		}
	}

	t->canhold = true;

	// locked entirely above the visible field
	if (!visible) {
		tetrisGameOver(t);
		return;
	}

	t->clearmask = 0;

	for (s32 y = 0; y < TET_H; y++) {
		s32 x;

		for (x = 0; x < TET_W; x++) {
			if (!t->board[y][x]) {
				break;
			}
		}

		if (x == TET_W) {
			t->clearmask |= 1u << y;
			numlines++;
		}
	}

	if (numlines) {
		static const s32 points[5] = { 0, 100, 300, 500, 800 };

		t->score += points[numlines] * t->level;
		t->lines += numlines;
		t->level = t->lines / 10 + 1;

		if (t->level > TET_MAXLEVEL) {
			t->level = TET_MAXLEVEL;
		}

		if (t->score > g_TetrisHighScore) {
			g_TetrisHighScore = t->score;
		}

		t->state = TETSTATE_CLEARING;
		t->statetimer = 0.f;
		menuPlaySound(numlines == 4 ? MENUSOUND_EXPLOSION : MENUSOUND_SUCCESS);
		return;
	}

	tetrisSpawn(t, tetrisTakeNext(t));
}

static void tetrisCollapse(struct tetris *t)
{
	s32 dst = TET_H - 1;

	for (s32 src = TET_H - 1; src >= 0; src--) {
		if (t->clearmask & (1u << src)) {
			continue;
		}

		if (dst != src) {
			memcpy(t->board[dst], t->board[src], TET_W);
		}

		dst--;
	}

	while (dst >= 0) {
		memset(t->board[dst], 0, TET_W);
		dst--;
	}

	t->clearmask = 0;
}

static void tetrisHold(struct tetris *t)
{
	s32 next;

	if (!t->canhold) {
		return;
	}

	if (t->hold < 0) {
		t->hold = t->piece;
		next = tetrisTakeNext(t);
	} else {
		next = t->hold;
		t->hold = t->piece;
	}

	t->canhold = false;
	tetrisSpawn(t, next);
	menuPlaySound(MENUSOUND_SUBFOCUS);
}

static u32 tetrisReadButtons(void)
{
	s8 pads[2];
	s32 stickx = 0;
	s32 sticky = 0;
	u32 raw = 0;
	u32 out = 0;

	menuGetContPads(&pads[0], &pads[1]);

	for (s32 i = 0; i < 2; i++) {
		s32 sx, sy;

		if (pads[i] < 0) {
			continue;
		}

		raw |= joyGetButtons(pads[i], 0xffffffff);
		sx = joyGetStickX(pads[i]);
		sy = joyGetStickY(pads[i]);

		if ((sx < 0 ? -sx : sx) > (stickx < 0 ? -stickx : stickx)) {
			stickx = sx;
		}

		if ((sy < 0 ? -sy : sy) > (sticky < 0 ? -sticky : sticky)) {
			sticky = sy;
		}
	}

#ifndef PLATFORM_N64
	// with the default PC binds the pad's B button is also D-pad left
	if (raw & (B_BUTTON | BUTTON_UI_CANCEL)) {
		raw &= ~L_JPAD;
	}
#endif

	if (stickx < -40 || (raw & (L_CBUTTONS | L_JPAD))) out |= TB_LEFT;
	if (stickx > 40 || (raw & (R_CBUTTONS | R_JPAD)))  out |= TB_RIGHT;
	if (sticky < -40 || (raw & (D_CBUTTONS | D_JPAD))) out |= TB_DOWN;
	if (sticky > 50 || (raw & (U_CBUTTONS | U_JPAD)))  out |= TB_UP;
	if (raw & (A_BUTTON | Z_TRIG))                     out |= TB_CW;
	if (raw & R_TRIG)                                  out |= TB_CCW;
	if (raw & L_TRIG)                                  out |= TB_HOLD;
	if (raw & (A_BUTTON | Z_TRIG))                     out |= TB_CONFIRM;
	if (raw & START_BUTTON)                            out |= TB_START;

#ifndef PLATFORM_N64
	if (raw & X_BUTTON)         out |= TB_CCW;
	if (raw & Y_BUTTON)         out |= TB_HOLD;
	if (raw & BUTTON_UI_ACCEPT) out |= TB_CONFIRM;
#endif

	return out;
}

static void tetrisTick(struct tetris *t, u32 held, u32 pressed, f32 dt)
{
	switch (t->state) {
	case TETSTATE_TITLE:
	case TETSTATE_PAUSED:
		// Start on the pause screen throws the current game away
		if (t->state == TETSTATE_PAUSED && (pressed & TB_START)) {
			tetrisNewGame(t);
			menuPlaySound(MENUSOUND_SELECT);
			return;
		}

		if (pressed & (TB_CONFIRM | TB_START)) {
			if (t->state == TETSTATE_TITLE) {
				tetrisNewGame(t);
			} else {
				t->state = TETSTATE_PLAYING;
			}

			menuPlaySound(MENUSOUND_SELECT);
		}
		return;
	case TETSTATE_GAMEOVER:
		t->statetimer += dt;

		if (t->statetimer > 40.f && (pressed & (TB_CONFIRM | TB_START))) {
			tetrisNewGame(t);
			menuPlaySound(MENUSOUND_SELECT);
		}
		return;
	case TETSTATE_CLEARING:
		t->statetimer += dt;

		if (t->statetimer >= TET_CLEARTIME) {
			tetrisCollapse(t);
			t->state = TETSTATE_PLAYING;
			tetrisSpawn(t, tetrisTakeNext(t));
		}
		return;
	}

	// playing
	if (pressed & TB_START) {
		t->state = TETSTATE_PAUSED;
		t->dasdir = 0;
		menuPlaySound(MENUSOUND_SELECT);
		return;
	}

	if (pressed & TB_HOLD) {
		tetrisHold(t);

		if (t->state != TETSTATE_PLAYING) {
			return;
		}
	}

	if (pressed & TB_CW) {
		tetrisRotate(t, 1);
	}

	if (pressed & TB_CCW) {
		tetrisRotate(t, -1);
	}

	// left / right with auto-repeat
	{
		const s32 dir = (held & TB_LEFT) && !(held & TB_RIGHT) ? -1
			: (held & TB_RIGHT) && !(held & TB_LEFT) ? 1 : 0;

		if (dir != t->dasdir) {
			t->dasdir = dir;
			t->dastimer = 0.f;

			if (dir) {
				tetrisShift(t, dir);
			}
		} else if (dir) {
			s32 steps = 0;

			t->dastimer += dt;

			while (t->dastimer >= TET_DAS && steps < TET_W) {
				t->dastimer -= TET_ARR;
				tetrisShift(t, dir);
				steps++;
			}
		}
	}

	// hard drop
	if (pressed & TB_UP) {
		const s32 y = tetrisGhostY(t);

		t->score += (y - t->py) * 2;
		t->py = y;
		menuPlaySound(MENUSOUND_TOGGLEOFF);
		tetrisLockPiece(t);
		return;
	}

	// gravity and soft drop
	{
		const s32 lvl = t->level < 1 ? 1 : t->level;
		const f32 rate = 1.f / g_TetGravity[lvl - 1];
		const bool soft = (held & TB_DOWN) != 0;
		f32 speed = rate;

		if (soft) {
			speed = rate * 20.f;

			if (speed < 0.5f) {
				speed = 0.5f;
			}
		}

		t->fall += speed * dt;

		while (t->fall >= 0.999f) {
			if (!tetrisFits(t, t->piece, t->rot, t->px, t->py + 1)) {
				t->fall = 0.f;
				break;
			}

			t->py++;
			t->fall -= 1.f;

			if (t->fall < 0.f) {
				t->fall = 0.f;
			}

			if (soft) {
				t->score++;
			}

			if (t->py > t->lowesty) {
				t->lowesty = t->py;
				t->lockresets = 0;
				t->lock = 0.f;
			}
		}
	}

	// lock delay
	if (!tetrisFits(t, t->piece, t->rot, t->px, t->py + 1)) {
		t->lock += dt;

		if (t->lock >= TET_LOCKDELAY) {
			tetrisLockPiece(t);
		}
	} else {
		t->lock = 0.f;
	}
}

static MenuDialogHandlerResult menudialogTetris(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	struct tetris *t = tetrisGet();

	if (operation == MENUOP_OPEN) {
		t->wascurrent = false;

		if (t->state == TETSTATE_PLAYING || t->state == TETSTATE_CLEARING) {
			// finish a pending line clear so the board is tidy while paused
			if (t->state == TETSTATE_CLEARING) {
				tetrisCollapse(t);
				tetrisSpawn(t, tetrisTakeNext(t));
			}

			if (t->state != TETSTATE_GAMEOVER) {
				t->state = TETSTATE_PAUSED;
			}
		}
	}

	if (operation == MENUOP_TICK) {
		struct menuinputs *inputs = data->dialog2.inputs;
		const bool current = g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef;
		f32 dt = g_Vars.diffframe60freal;

		if (!current) {
			t->wascurrent = false;
			return 0;
		}

		if (dt < 0.f) {
			dt = 0.f;
		} else if (dt > 6.f) {
			dt = 6.f;
		}

		t->anim += dt;

		{
			const u32 held = tetrisReadButtons();
			const u32 pressed = t->wascurrent ? held & ~t->prevbuttons : 0;

			t->prevbuttons = held;
			t->wascurrent = true;

			// B / Esc closes the dialog this frame, so don't also play it
			if (!inputs->back) {
				tetrisTick(t, held, pressed, dt);
			}
		}

		// the game owns the controls: stop the menu navigating or selecting
		inputs->leftright = 0;
		inputs->updown = 0;
		inputs->leftrightheld = 0;
		inputs->updownheld = 0;
		inputs->xaxis = 0;
		inputs->yaxis = 0;
		inputs->select = 0;
		inputs->shoulder = 0;
		inputs->start = 0; // otherwise Start also closes the pause menu
#ifndef PLATFORM_N64
		inputs->mousescroll = 0;
#endif
	}

	return 0;
}

/*
 * Drawing
 */

static Gfx *tetrisRect(Gfx *gdl, s32 x1, s32 y1, s32 x2, s32 y2, u32 colour)
{
	if (x2 <= x1 || y2 <= y1) {
		return gdl;
	}

	gdl = textSetPrimColour(gdl, colour);
	gDPFillRectangleScaled(gdl++, x1, y1, x2, y2);
	return gdl;
}

static u32 tetrisShade(u32 colour, s32 amount)
{
	s32 r = (colour >> 24) & 0xff;
	s32 g = (colour >> 16) & 0xff;
	s32 b = (colour >> 8) & 0xff;

	r += amount; g += amount; b += amount;
	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;

	return (u32)r << 24 | (u32)g << 16 | (u32)b << 8 | (colour & 0xff);
}

static Gfx *tetrisBlock(Gfx *gdl, s32 x, s32 y, s32 size, u32 colour)
{
	const s32 x2 = x + size - 1;
	const s32 y2 = y + size - 1;

	gdl = tetrisRect(gdl, x, y, x2, y2, colour);
	gdl = tetrisRect(gdl, x, y, x2, y + 1, tetrisShade(colour, 70));
	gdl = tetrisRect(gdl, x, y2 - 1, x2, y2, tetrisShade(colour, -80));

	return gdl;
}

static Gfx *tetrisMiniPiece(Gfx *gdl, s32 piece, s32 x, s32 y, s32 w, s32 h, s32 size, u32 alpha)
{
	s32 minx = 4, maxx = 0, miny = 4, maxy = 0;
	s32 ox, oy;

	for (s32 i = 0; i < 4; i++) {
		const s32 cx = g_TetCells[piece][i][0];
		const s32 cy = g_TetCells[piece][i][1];

		if (cx < minx) minx = cx;
		if (cx > maxx) maxx = cx;
		if (cy < miny) miny = cy;
		if (cy > maxy) maxy = cy;
	}

	ox = x + (w - (maxx - minx + 1) * size) / 2;
	oy = y + (h - (maxy - miny + 1) * size) / 2;

	for (s32 i = 0; i < 4; i++) {
		const s32 cx = g_TetCells[piece][i][0] - minx;
		const s32 cy = g_TetCells[piece][i][1] - miny;
		const u32 colour = (g_TetColours[piece] & 0xffffff00) | alpha;

		gdl = tetrisBlock(gdl, ox + cx * size, oy + cy * size, size, colour);
	}

	return gdl;
}

static Gfx *tetrisText(Gfx *gdl, s32 x, s32 y, s32 centrewidth, char *text, u32 colour, bool small)
{
	struct fontchar *chars = small ? g_CharsHandelGothicXs : g_CharsHandelGothicSm;
	struct font *font = small ? g_FontHandelGothicXs : g_FontHandelGothicSm;

	if (centrewidth > 0) {
		s32 textheight, textwidth;
		textMeasure(&textheight, &textwidth, text, chars, font, 0);
		x += (centrewidth - textwidth) / 2;
	}

	gdl = text0f153628(gdl);
	textSetWaveColours(colour, colour);
	gdl = textRenderProjected(gdl, &x, &y, text, chars, font, colour, viGetWidth(), viGetHeight(), 0, 0);
	gdl = text0f153780(gdl);

	return gdl;
}

static Gfx *tetrisRender(Gfx *gdl, struct menurendercontext *context)
{
	struct tetris *t = tetrisGet();
	const s32 cell = 6;
	const s32 bw = TET_W * cell;
	const s32 bh = (TET_H - TET_HIDDEN) * cell;
	const s32 bx = context->x + (context->width - bw) / 2;
	const s32 by = context->y + 6;
	const s32 lx = context->x + 8;           // left panel
	const s32 lw = bx - 6 - lx;
	const s32 rx = bx + bw + 6;              // right panel
	const s32 rw = context->x + context->width - 8 - rx;
	const u32 labelcolour = 0x80c0ffff;
	const u32 valuecolour = 0xffffffff;
	const bool showpiece = t->state == TETSTATE_PLAYING;
	char buffer[32];

	// playfield background, column lines and frame
	gdl = tetrisRect(gdl, bx, by, bx + bw, by + bh, 0x0000207f);

	for (s32 x = 1; x < TET_W; x++) {
		gdl = tetrisRect(gdl, bx + x * cell, by, bx + x * cell + 1, by + bh, 0xffffff0f);
	}

	gdl = tetrisRect(gdl, bx - 1, by - 1, bx + bw + 1, by, 0x80c0ffaf);
	gdl = tetrisRect(gdl, bx - 1, by + bh, bx + bw + 1, by + bh + 1, 0x80c0ffaf);
	gdl = tetrisRect(gdl, bx - 1, by, bx, by + bh, 0x80c0ffaf);
	gdl = tetrisRect(gdl, bx + bw, by, bx + bw + 1, by + bh, 0x80c0ffaf);

	// the stack
	if (t->state != TETSTATE_TITLE) {
		for (s32 y = TET_HIDDEN; y < TET_H; y++) {
			const s32 sy = by + (y - TET_HIDDEN) * cell;

			if (t->state == TETSTATE_CLEARING && (t->clearmask & (1u << y))) {
				const s32 phase = (s32)(t->statetimer / 3.f) & 1;
				gdl = tetrisRect(gdl, bx, sy, bx + bw, sy + cell, phase ? 0xffffffff : 0xffffff7f);
				continue;
			}

			for (s32 x = 0; x < TET_W; x++) {
				if (t->board[y][x]) {
					u32 colour = g_TetColours[t->board[y][x] - 1];

					if (t->state == TETSTATE_GAMEOVER) {
						colour = (tetrisShade(colour, -90) & 0xffffff00) | 0xff;
					}

					gdl = tetrisBlock(gdl, bx + x * cell, sy, cell, colour);
				}
			}
		}
	}

	// ghost and falling piece
	if (showpiece) {
		const s32 ghosty = tetrisGhostY(t);

		for (s32 i = 0; i < 4; i++) {
			s32 x, y;
			tetrisCell(t->piece, t->rot, i, &x, &y);
			x += t->px;

			if (y + ghosty >= TET_HIDDEN) {
				const s32 gx = bx + x * cell;
				const s32 gy = by + (y + ghosty - TET_HIDDEN) * cell;
				gdl = tetrisRect(gdl, gx, gy, gx + cell - 1, gy + cell - 1, (g_TetColours[t->piece] & 0xffffff00) | 0x40);
			}
		}

		for (s32 i = 0; i < 4; i++) {
			s32 x, y;
			tetrisCell(t->piece, t->rot, i, &x, &y);
			x += t->px;
			y += t->py;

			if (y >= TET_HIDDEN) {
				gdl = tetrisBlock(gdl, bx + x * cell, by + (y - TET_HIDDEN) * cell, cell, g_TetColours[t->piece]);
			}
		}
	}

	// hold box
	gdl = tetrisRect(gdl, lx, by + 12, lx + lw, by + 36, 0x0000207f);

	if (t->state != TETSTATE_TITLE && t->hold >= 0) {
		gdl = tetrisMiniPiece(gdl, t->hold, lx, by + 12, lw, 24, 5, t->canhold ? 0xff : 0x60);
	}

	// next queue
	gdl = tetrisRect(gdl, rx, by + 12, rx + rw, by + 84, 0x0000207f);

	if (t->state != TETSTATE_TITLE) {
		for (s32 i = 0; i < TET_NEXT; i++) {
			gdl = tetrisMiniPiece(gdl, t->queue[i], rx, by + 12 + i * 24, rw, 24, i == 0 ? 5 : 4, i == 0 ? 0xff : 0xb0);
		}
	}

	gdl = text0f153838(gdl);

	// panel text
	gdl = tetrisText(gdl, lx, by, lw, "HOLD", labelcolour, true);
	gdl = tetrisText(gdl, rx, by, rw, "NEXT", labelcolour, true);

	gdl = tetrisText(gdl, lx, by + 42, lw, "SCORE", labelcolour, true);
	sprintf(buffer, "%d", t->score);
	gdl = tetrisText(gdl, lx, by + 51, lw, buffer, valuecolour, false);

	gdl = tetrisText(gdl, lx, by + 66, lw, "LINES", labelcolour, true);
	sprintf(buffer, "%d", t->lines);
	gdl = tetrisText(gdl, lx, by + 75, lw, buffer, valuecolour, false);

	gdl = tetrisText(gdl, lx, by + 90, lw, "LEVEL", labelcolour, true);
	sprintf(buffer, "%d", t->state == TETSTATE_TITLE ? 1 : t->level);
	gdl = tetrisText(gdl, lx, by + 99, lw, buffer, valuecolour, false);

	gdl = tetrisText(gdl, rx, by + 90, rw, "BEST", labelcolour, true);
	sprintf(buffer, "%d", g_TetrisHighScore);
	gdl = tetrisText(gdl, rx, by + 99, rw, buffer, valuecolour, false);

	// title / paused / game over card
	if (t->state == TETSTATE_TITLE || t->state == TETSTATE_PAUSED || t->state == TETSTATE_GAMEOVER) {
		const s32 cx = context->x + 4;
		const s32 cw = context->width - 8;
		const bool blink = ((s32)(t->anim / 20.f) & 1) == 0;
		s32 y = context->y + 14;

		gdl = tetrisRect(gdl, cx, context->y + 8, cx + cw, context->y + context->height - 8, 0x000010d8);
		gdl = tetrisRect(gdl, cx, context->y + 8, cx + cw, context->y + 9, 0x80c0ffaf);
		gdl = tetrisRect(gdl, cx, context->y + context->height - 9, cx + cw, context->y + context->height - 8, 0x80c0ffaf);
		gdl = text0f153838(gdl);

		if (t->state == TETSTATE_GAMEOVER) {
			gdl = tetrisText(gdl, cx, y, cw, "GAME OVER", 0xff6060ff, false);
			y += 16;
			sprintf(buffer, "Score  %d", t->score);
			gdl = tetrisText(gdl, cx, y, cw, buffer, valuecolour, false);
			y += 12;
			sprintf(buffer, "Lines  %d     Level  %d", t->lines, t->level);
			gdl = tetrisText(gdl, cx, y, cw, buffer, valuecolour, true);
			y += 10;

			if (t->score > 0 && t->score >= g_TetrisHighScore) {
				gdl = tetrisText(gdl, cx, y, cw, "New best score!", 0xffe040ff, true);
			}

			y += 16;
		} else {
			gdl = tetrisText(gdl, cx, y, cw, t->state == TETSTATE_PAUSED ? "PAUSED" : "TETRIS", 0x40e0ffff, false);
			y += 16;
		}

		gdl = tetrisText(gdl, cx, y, cw, "Stick / D-Pad: Move", labelcolour, true);
		y += 9;
		gdl = tetrisText(gdl, cx, y, cw, "Down: Soft Drop     Up: Hard Drop", labelcolour, true);
		y += 9;
		gdl = tetrisText(gdl, cx, y, cw, "A/Z: Rotate     R/X: Rotate Back", labelcolour, true);
		y += 9;
		gdl = tetrisText(gdl, cx, y, cw, "L/Y: Hold   Start: Pause   B: Leave", labelcolour, true);
		y += 14;

		if (blink && (t->state != TETSTATE_GAMEOVER || t->statetimer > 40.f)) {
			const char *prompt = t->state == TETSTATE_PAUSED ? "A: Resume     Start: Restart"
				: t->state == TETSTATE_GAMEOVER ? "Press A to Play Again"
				: "Press A to Start";
			gdl = tetrisText(gdl, cx, y, cw, (char *)prompt, valuecolour, false);
		}
	}

	return gdl;
}

struct menuitem g_TetrisMenuItems[] = {
	{ MENUITEMTYPE_CUSTOMRENDER, 0, 0, (intptr_t)tetrisRender, TET_ITEM_HEIGHT, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_TetrisMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Tetris",
	g_TetrisMenuItems,
	menudialogTetris,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
