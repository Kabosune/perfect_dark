# Weighty Aim: Design & Developer Notes

How the Weighty Aim mod is built, and what to know before changing it.
Written for anyone (human or AI assistant) picking up work on this fork of the
Perfect Dark PC port (fgsfdsfgs/perfect_dark, MIT).

Players should read `WEIGHTY_AIM_BETA.md` instead. This file is for developers.

---

## 1. What the mod does

Weighty Aim makes aiming feel physical, in the spirit of Bodycam or Receiver-style shooters:

- **Free-aim zone.** Inside a deadzone box in the middle of the screen, the
  gun and crosshair move without turning the camera. Past the edge, the
  camera turns. Part of the gun's motion is always shared with the camera
  (Camera Share).
- **Spring-driven gun.** The gun chases the aim point with a damped spring,
  so it lags, overshoots slightly and settles. Turning adds drag.
- **Recentre.** When input stops, the aim point drifts back to the screen
  centre after a delay, with an ease-in (Catch-Up Smoothing).
- **Aim mode (hold aim).** Classic uses the game's own aiming. Modern adds
  optional aim-down-sights (gun raised, zoom, reduced sway and sensitivity),
  optional movement while aiming (d-pad, stick, both or none; keyboard
  separately), and per-mode crosshair and laser toggles.
- **Laser sight.** An optional laser (beam and dot) on any firearm. It shows
  a slight shimmer and follows recoil. The dot draws over smoke.
- **Stick response.** Look curves (a bezier, drawn as a graph in the menu),
  inner and outer deadzones, sensitivity, vertical sensitivity, and an option
  to turn off the game's built-in deadzone.
- **Look Acceleration.** Extra turn speed after holding the stick at full
  deflection (amount, delay and ramp time).
- **Gyro aim**, with calibration, gyro space options and auto-calibration.
- **Aim assist strength** slider.
- **Presets:** Weighty (default), Immersive, Boring, Classic (mod off) and
  Custom 1–3.
- **Split-screen:** settings are stored per player (Player 1–4).

There is no online play in this fork. It has no netplay code.

---

## 2. File map

| File | Role |
|---|---|
| `port/include/weightyaim.h` | Config structs (`weightyaimcfg`, `weightyaimstickcfg`, gyro cfg), constants, every hook declaration |
| `port/src/weightyaim.c` | All the logic: presets, look filter, spring, ADS, laser, gyro, `pd.ini` registration |
| `port/src/weightyaimmenu.c` | All Weighty Aim menus, including the custom-rendered curve graph |
| `src/game/bondmove.c` | Main input hook site: look filter, crosshair, aim assist, zoom FOV, movement while aiming, deadzone bypass |
| `src/game/bondwalk.c` | `weightyAimApplyMoveSpeed()`, called after the crouch speed is applied |
| `src/game/bondgun.c` | Gun position offset; laser update (checked *before* the Falcon 2's built-in laser) |
| `src/game/gunfx.c` | Laser beam and dot rendering: separate show/hide, shimmer, dot drawn without the Z-buffer |
| `src/game/sight.c` | Hide or force the crosshair |
| `src/game/prop.c` | Aim-assist scale |
| `src/game/menu.c`, `src/game/menuitem.c`, `src/include/constants.h` | Added `MENUITEMTYPE_CUSTOMRENDER` (lets a menu item draw with its own function) |
| `src/game/mainmenu.c` | "Weighty Aim Settings..." entry in Control Options |
| `port/src/optionsmenu.c` | `optionsOpenWeightyAimMenu()` plus shortcuts on the Mouse, Stick and Controller pages |
| `port/src/input.c` / `.h` | `inputKeyboardButtons()`, used by keyboard movement while aiming |
| `port/src/main.c` | Calls `weightyAimInit()` after `configInit()` |
| `src/game/gamefile.c` | Unlock Everything now sets `MODFILE_GAME` so the unlock actually saves |
| `.github/workflows/c-cpp.yml` | CI builds on pushes to `port` and `weighty-aim`; artifacts kept 30 days |

Hook functions in the game code are one-line calls into `weightyaim.c`. Keep
it that way: logic lives in `port/src/`, game files only get small calls,
which makes merges from the upstream port easy.

---

## 3. How the look pipeline works

The entry point is `weightyAimFilterLook()`, called from `bondmove.c` every
frame with the stick and mouse look input. In order:

1. **Stick rates.** `weightyAimStickRates()` applies deadzones, the curve
   (`weightyAimCurveOutput()`, a bezier via `weightyAimBezier()`),
   sensitivity, vertical sensitivity and Look Acceleration
   (`weightyAimApplyBoost()`).
2. **Degrees requested.** The stick, mouse and gyro are converted to degrees
   this frame, matching exactly how PD would have turned the camera. Gyro
   input is treated like the mouse.
3. **ADS blend.** `adsblend` eases between 0 and 1 over `adstime`. Aim
   sensitivity (`adssens`) is scaled in by that blend (Modern mode only).
4. **Active check.** The mod only runs while walking, alive, able to look,
   and in normal tick mode. When inactive, state resets. A custom stick
   response still applies even with the mod off.
5. **Free-aim zone.** Requested rotation first moves the aim point within
   the deadzone box. Camera Share passes part of it to the camera always;
   any overflow past the box edge goes to the camera. `adszone` ("Aim Feel
   While Aiming") blends toward camera-only aiming while ADS.
6. **Recentre.** After `recenterdelay` of no input, the aim point returns to
   centre at `recenterspeed`, eased in over `recentersmooth`. Before that,
   while moving, Camera Lead (`cameralead`) pulls the camera toward the aim
   point, stronger near the zone edge and scaled by how hard you're aiming
   (`leadinput`: full at half-stick speed, smoothed over 0.1s), so small
   corrections barely pull the camera. Lead is off while turning from the
   edge and eases back in over 0.3s (`leadramp`), so it doesn't snap back.
   Lead and Catch-Up both move aim from the crosshair into the camera, so
   the total aim never changes.
7. **Spring.** `weightyAimStepSpring()` moves the displayed gun and crosshair
   toward the aim point (`gunresponse`, `gundamping`, `turndrag`). The lag is
   capped after each spring step so fast turns can't leave the gun far
   behind.

The crosshair position comes from `weightyAimGetCrosshair()`, and the gun's
offset from `weightyAimAdjustGunPos()`.

Time uses `g_Vars.lvupdate60freal`, so everything is frame-rate independent.
`dt60 <= 0` means paused: return early and keep state.

---

## 4. Settings, presets and `pd.ini`

- **Force Original Aim & Settings** (bottom of the main page, `WeightyAim.ForceOriginalAiming`,
  global for all players, saved) is for tournaments: 1:1 with the original
  game whatever anyone's settings are. `weightyAimCfgEnabled()` returns false
  (every Weighty Aim feature off, like Classic), and it also blocks what
  Classic lets through: custom stick response and Look Acceleration, gyro,
  Aim Assist strength (forced to the game's own) and the deadzone tick
  (the game's deadzone is forced on). Nothing is overwritten, so unticking
  it brings everyone's setup back. The mouse stays as the port has it.
- **Force Mouse & Gyro as Stick** (`WeightyAim.ForceMouseGyroAsStick`, shown
  only with Force Original Aim & Settings): mouse and gyro are added to the stick's
  look rate and turned back into a stick value (`weightyAimVirtualStick()`),
  capped at full tilt and rounded to whole stick steps, so they can't turn
  faster than a stick. Mouse is still more precise for tiny moves.
- **Stick range:** the port maps a modern stick at full tilt to 127, but a
  real N64 stick reaches about 80, and the game's curve hits full speed at
  70. So the unmodded port maxes out at about half tilt and feels twitchy.
  `weightyAimStickRange()` (hooked in `bondmove.c` where the sticks are
  read) scales sticks to the N64 range (x80/127, like emulators) for the
  **Original** curve and under Force Original Aim & Settings. This covers
  turning, aim-mode edge turning and walking. **Source Port** is the same
  curve without the scaling, i.e. the unmodded port. Saved curve numbers are
  unchanged (Source Port is 6); the menu lists it second via
  `g_WeightyAimCurveOrder`.
- **Reticle...** page: Hip-Fire Reticle, Reticle While Aiming, and the
  port's own reticle settings (Size, Colour & Opacity, Colour by Health),
  shared from `optionsmenu.c`. The UI says "reticle"; code and `pd.ini`
  keys still say crosshair.
- **Edge Auto-Turn** (`edgeturnspeed`, 0 = off; on only in Arcade): while
  the reticle rests in the outer band of the zone (`edgeband`), the view keeps
  turning on its own, eased by smoothstep across the band, after a 0.15s
  delay and a 0.2s ease-in. Vertical is scaled by `edgevertical`. It applies
  to the input that moved the reticle last (`lastinput`; per-preset ticks
  `edgemouse`/`edgegyro` on, `edgestick` off), and not while aiming down
  sights. While it applies, pushing past the edge only turns by
  `edgeinfluence` (0.25), and Camera Lead and Catch-Up pause in the band.
  This deliberately breaks "aim always adds up to your input", like Wii
  shooters.
- **Reticle profiles:** reticle size, opacity and Smooth Reticle are
  per-player port settings, not preset fields. `reticleprofile` (1 in
  Arcade, copied into customs made from it) picks one of two saved sets;
  `weightyAimSyncReticle()` swaps them when the preset's profile changes
  (from `weightyAimApplyPreset()` and each frame). Arcade's defaults: size 3,
  opacity 0x50, smooth on. Classic uses the normal profile.
- **Reticle Opacity** edits the alpha byte of the port's reticle colour.
  **Smooth Reticle** (per player, off by default, forced off under Force Original):
  the reticle normally snaps to whole N64 pixels (about 4.5 screen pixels at
  1080p). `sight.c` now draws the hip-fire target and the aimer box at the
  whole pixel plus a quarter-pixel `gDPSetSubpixelOffsetEXT` offset, the
  finest step the renderer takes (U10.2). Finer than that would need a
  renderer change.
- Aim & Camera Feel shows 7 main sliders (zone width/height, Camera Share,
  Camera Lead, Catch-Up, Catch-Up Delay, Gun Response). The rest are behind
  the "Show Advanced Feel" tick (at the bottom of the page) (`WeightyAim.ShowAdvancedFeel`, saved;
  `WEIGHTYAIM_FEEL_NUM_MAIN` in `weightyaimmenu.c`).
- The aim-feel settings live in `struct weightyaimcfg`, one per preset. The
  field table `g_WeightyAimCfgFields` (the `WA_FLOAT` and `WA_INT` macros)
  drives both `pd.ini` registration and the sliders. To add a setting, add
  the field to the struct, add a `WA_*` line, set a value in each preset, and
  add a menu item.
- Keys are registered under `WeightyAim.*` (per-player keys look like
  `WeightyAim.Player%d.Preset`) through `configRegisterInt` and
  `configRegisterFloat` inside a `PD_CONSTRUCTOR`.
- **Gotcha:** `weightyAimInit()` runs after `pd.ini` loads and re-applies the
  built-in presets. Built-in preset values edited in `pd.ini` are
  overwritten; only the Custom presets keep ini edits. This is on purpose, so
  tuning changes in code reach existing players.
- Per-player stick settings (`weightyaimstickcfg`) are separate from presets.
  Current defaults: Balanced curve, 1% inner deadzone, 1.0 sensitivity,
  0.8 vertical, the game's deadzone on, Look Acceleration 2.0× after 0.1s
  over 0.25s.
- `weightyAimDefaultAimMovement()` and `weightyAimDefaultAimKeyboardMove()`
  give each aim mode's movement defaults.

Main preset traits:

- **Weighty:** Modern, d-pad movement while aiming, deadzone 7.5/4.5, camera
  share 0.5, gun response 8.5, damping 0.7, turn drag 0.35, catch-up 0.4.
- **Immersive:** Modern, camera share 0.5, stick movement, no crosshair while aiming, lasers
  on, slower gun (4.6), more drag (0.6), less sway.
- **Arcade:** Wii / rail-shooter style, "point at the screen". Big zone
  (28/18 deg), camera share 0, no lead or catch-up, light fast gun (14 Hz,
  damping 0.85, drag 0.1), little sway, Edge Auto-Turn 90 deg/s. Has its own
  reticle profile. Listed third in the menu (saved number 7, after the
  customs; `g_WeightyAimPresetOrder`).
- **Boring:** Modern, full movement, camera share 1 (no free-aim feel).
- **Classic:** mod off, the game's original aiming.

---

## 5. Menus (including custom drawing)

All Weighty Aim menus are in `port/src/weightyaimmenu.c`. They're opened
from Control Options (`mainmenu.c`) and from shortcuts in `optionsmenu.c`.

- Items use `MENUITEMFLAG_LITERAL_TEXT` so labels are plain C strings instead
  of language-file IDs.
- **Gotcha (caused a crash):** with `MENUITEMFLAG_LITERAL_TEXT`, a nonzero
  `param3` is read as a pointer to extra right-hand text. Keep `param3 = 0`
  unless you really mean that.
- Sliders are generated from `struct weightyaimslider` tables that point at
  config fields. The menus track slider positions by index (e.g. the first
  slider on the Aim Mode page is item 8, and the assist slider is item 12
  on the main page). Adding an item above a slider shifts those indices.
- **Custom drawing:** `MENUITEMTYPE_CUSTOMRENDER` (0x1c) is a menu item type
  **the mod added** to the game's menu code (look for `// [weightyaim]` in
  `src/game/menu.c`, `src/game/menuitem.c` and `src/include/constants.h`;
  PC builds only). The stick curve graph uses it. The item's fields are
  type, param, flags, **param2 = render function**, **param3 = height in
  pixels**, handler:
  ```c
  { MENUITEMTYPE_CUSTOMRENDER, 0, 0, (intptr_t)weightyAimRenderCurveGraph, 52, NULL },
  ```
  The width is fixed at 200. The item **can't be focused or selected**, so it
  receives no menu input. Anything interactive drawn this way has to read
  input itself or take it from a neighbouring item or the dialog handler.
  The function receives `(Gfx *gdl, struct menurendercontext *context)`,
  draws inside `context->x/y/width/height`, and returns the advanced `gdl`.
  Filled rectangles use `textSetPrimColour(gdl, 0xRRGGBBAA)` then
  `gDPFillRectangleScaled(gdl++, x1, y1, x2, y2)`; see
  `weightyAimGraphRect()`. This is the pattern to copy for any other custom
  widget or minigame drawn inside a menu.

---

## 6. Input details worth knowing

- PD builds stick input from `c1stickxsafe` and `c1stickysafe`, which include
  a 5-unit deadzone. With "Game's Built-In Deadzone" off,
  `weightyAimGameDeadzoneWanted()` makes `bondmove.c` use the raw value.
- In PC control mode, WASD are mapped to the C-buttons, which share bits with
  the d-pad. Keyboard movement while aiming therefore reads the real keyboard
  state (`inputKeyboardButtons()`) to tell keys apart from a controller
  d-pad.
- Movement while aiming is gated in `bondmove.c` via the `AIMSTEP(mask)`
  macro, using the `adsdpad`, `adsstick`, `adskb` and `kbbuttons` values.
  That covers d-pad steps, leaning and crouching while aiming.

---

## 7. The laser

- `weightyAimLaserWanted()` decides per hand whether a laser shows (firearms
  only). In `bondgun.c` it's checked before the Falcon 2's own laser.
- `weightyAimUpdateLaser()` positions the beam. While firing, it biases the
  beam toward the barrel so it follows recoil, and it refreshes
  `propFindAimingAt` so the dot lands on what you're actually hitting.
- `gunfx.c` draws the beam and dot through the game's existing laser-sight
  renderer and textures. No new assets were added. Beam and dot can be hidden
  separately (`weightyAimLaserBeamShown()` and `weightyAimLaserDotShown()`,
  with separate hip and aiming settings).
- **Gotcha:** impact smoke used to hide the dot while firing because of the
  depth buffer. The dot is now drawn with `G_ZBUFFER` cleared, then restored,
  and lifted slightly toward the camera.

---

## 8. Bugs already fixed (don't reintroduce)

| Symptom | Cause | Fix |
|---|---|---|
| Unlock Everything didn't save | Save file not marked dirty | Set `g_Vars.modifiedfiles \|= MODFILE_GAME` |
| Crosshair vanished with "Always" | Game skipped drawing when not aiming | `weightyAimForceCrosshair()` forces it |
| Crash opening Control Options (0xc0000005) | Literal-text item with `param3 = 4` | `param3 = 0` |
| Laser dot missing while firing | Smoke drawn over it (depth) | Draw the dot without the Z-buffer |

---

## 9. Open ideas / to-do

- Check the laser beam's recoil-following on the revolver. It follows recoil
  in general, but the revolver hasn't been tested.
- Improve or replace Catch-Up Smoothing with a proper ease-in for the
  camera's re-centre jerk.

---

## 10. Building and project rules

- Windows build: MSYS2 MinGW64, `cmake --build build -j4 -- -O` from the repo
  root.
- Branches: `weighty-aim` is the mod. `port` tracks upstream. Experiments go
  on their own branches (for example `tetris-toy`) and never merge into
  `weighty-aim`.
- Never commit or upload the ROM, `eeprom.bin` (save file) or `pd.ini`.
- Keep game-file changes as small hook calls. Put logic in `port/src/`.
- Releases: zip the build as `WeightyAim-vX.Y-windows.zip` and attach it to a
  GitHub Release with a `vX.Y` tag.

### Notes for AI assistants

- Commit messages must not include a session or conversation link, a
  `Co-Authored-By` line, or any other Claude/Anthropic credit. AI help is
  credited once in the README as "AI assistance (Claude Opus 5.5)".
- Don't rewrite history, amend or re-author the owner's commits.
- Prefer small, targeted edits. When the owner says "no testing" or "just
  compile", do exactly that.
