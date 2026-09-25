# Weighty Aim — Beta

Weighty Aim is a mod for the Perfect Dark PC port that reworks aiming: a free-aim
zone where the gun moves before the camera turns, weapon weight and sway, aim
down sights, stick response curves, look acceleration, gyro aiming and an optional
laser sight.

**This is a beta.** Expect rough edges, and please report anything odd.

## Install (Windows)

1. Download the zip from the latest pre-release on the
   [Releases](../../releases) page, or the `pd-x86_64-windows` build from the
   latest run on the [Actions](../../actions) tab.
2. Unzip it anywhere you can write to (not `Program Files`).
3. Put **your own** Perfect Dark ROM in the `data` folder, named
   `pd.ntsc-final.z64` (NTSC 1.1). The ROM is not included and can't be shared.
4. Run `pd.x86_64.exe`.

Your settings (`pd.ini`) and save (`eeprom.bin`) are created next to the exe.

## Where the settings are

Options → Extended → Weighty Aim (there are also shortcuts on the Mouse,
Controller, Stick and in-game Control Options pages).

- **Preset**: Weighty (default), Immersive, Arcade (new: Wii / rail-shooter
  style, point at the screen and the view turns at the edges), Boring (plain
  modern FPS), Classic (mod off), or three custom profiles. Changing any
  setting on a built-in preset saves it to a custom profile.
- **Aim Mode**: what holding aim does.
  - Classic: the original game's aiming.
  - Modern (default): look around normally while aiming.
  - Move While Aiming (controller and keyboard settings): whether the d-pad,
    stick or WASD can walk (slower) while aiming.
  - Aim Down Sights, zoom, aim sensitivity and crosshair/laser while aiming
    are on the same page. Aim Feel While Aiming now goes up to 200%.
- **Stick Response**: sensitivity, vertical sensitivity, deadzones, look curve.
  The **Original** curve now feels like the real N64 stick (full speed near
  full tilt, like an emulator). **Source Port** keeps the unmodded port's
  feel.
- **Aim & Camera Feel**: the settings that matter most are always shown (zone
  size, Camera Share, Camera Lead, Catch-Up, Gun Response, Edge Auto-Turn).
  Tick **Show Advanced Feel** at the bottom for the rest, including Free-Aim
  Reticle Speed (breaks the 1:1 aim on purpose, for an older feel).
- **Edge Auto-Turn** (on in Arcade): while the reticle rests at the edge of
  the zone, the view keeps turning on its own, so mouse and gyro don't have
  to keep moving. Works for mouse and gyro by default, not the stick.
- **Reticle**: on/off for hip-fire and aiming, size, opacity, colour, and
  **Smooth Reticle** (moves in finer steps instead of snapping to the N64's
  pixel grid). Arcade keeps its own reticle settings.
- **Force Original Aim & Settings** (bottom of the Weighty Aim page): for
  tournaments. Aiming and every Weighty Aim setting become 1:1 with the
  original game for all players. **Force Mouse & Gyro as Stick** under it
  caps mouse and gyro at stick speed.

## What to test

New in v0.2:

- **Arcade**: does pointing and edge-turning feel like a Wii shooter? Is
  the auto-turn too fast or slow? Do mouse flicks at the edge feel OK?
- **Camera Lead** at high values: no more snapping back at the edge?
- **Original vs Source Port curves**: does Original feel like an emulator?
- **Force Original Aim & Settings**: does it match the original game?
- **Smooth Reticle**, and switching to and from Arcade keeping your reticle.

Also:

- Does aiming feel good with a controller? With a mouse? With gyro?
- Each Aim Mode, with Aim Down Sights on and off.
- Fast turns: does the reticle keep up?
- The laser sight with different guns (the revolver's recoil is a known issue).
- Anything that crashes: note what you were doing and which menu was open.

## Reporting bugs

Open an issue on the [Issues](../../issues) tab with:

- what happened and what you expected,
- steps to make it happen again,
- your preset, Aim Mode and look curve, and whether you use a controller,
  mouse or gyro,
- if it crashed, the crash text the game shows (or a screenshot of it).

Weighty Aim is built on the
[Perfect Dark PC port](https://github.com/fgsfdsfgs/perfect_dark); see
[README.md](README.md) for the port itself.
