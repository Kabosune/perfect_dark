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

- **Preset**: Weighty (default), Immersive, Boring (plain modern FPS), Classic
  (mod off), or three custom profiles. Changing any setting on a built-in preset
  saves it to a custom profile.
- **Aim Mode**: what holding aim does.
  - Classic: the original game's aiming.
  - Modern Classic (default): look around normally while aiming, stand still;
    the d-pad / WASD can still step.
  - Mobile: look around normally and walk (slower) while aiming.
  - Aim Down Sights, zoom, aim sensitivity and crosshair/laser while aiming
    are on the same page.
- **Stick Response**: sensitivity, vertical sensitivity, deadzones, look curve.
- **Aim & Camera Feel**: free-aim zone size, gun weight, camera lead, sway.

## What to test

- Does aiming feel good with a controller? With a mouse? With gyro?
- Each Aim Mode, with Aim Down Sights on and off.
- Fast turns: does the crosshair keep up?
- The laser sight with different guns (the revolver's recoil is a known issue).
- Anything that crashes: note what you were doing and which menu was open.

## Reporting bugs

Open an issue on the [Issues](../../issues) tab with:

- what happened and what you expected,
- steps to make it happen again,
- your preset and Aim Mode, and whether you use a controller, mouse or gyro,
- if it crashed, the crash text the game shows (or a screenshot of it).

Weighty Aim is built on the
[Perfect Dark PC port](https://github.com/fgsfdsfgs/perfect_dark); see
[README.md](README.md) for the port itself.
