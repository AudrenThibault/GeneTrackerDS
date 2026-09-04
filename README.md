# GeneTrackerDS

A music tracker for the **Nintendo DSi**, playing the Sega Mega Drive's sound
chips: the YM2612 for FM and the SN76489 for PSG, both emulated on the machine
itself.

It is the sibling of [GeneTrackerMD](https://github.com/AudrenThibault/NativeMegadriveTracker),
which runs on the actual console. Songs travel between the two, samples
included.

## What it is, and what it is not

- The **top screen** carries the tracker, full frame — nothing around it, no
  panels, no instrument list, the way LSDJ does it.
- Everything is driven with the D-pad and the buttons. **The touch screen is
  not used.**
- Target: **DSi and DSi XL**. Their ARM9 runs at 133 MHz, which leaves room to
  emulate the YM2612 with ymfm — so you get the real chip's voice, not an
  approximation.

## What it does

- **Sequences** ten voices, through the song → chain → phrase chain, with
  tables.
- **Edits FM instruments**: four operators, eleven parameters each, algorithm,
  feedback, LFO.
- **PSG macros** — volume, arpeggio, noise mode — and a three-point envelope.
- **Imports DefleMask modules** (`.dmf`) and reads and writes its own `.mdm`
  format.
- **Loads WAV samples** from the SD card.
- **Exports a player ROM** — a Mega Drive cartridge that plays the song — and
  **a GeneTrackerMD ROM**, where the song stays editable on the console.
- **Imports a project back** from a GeneTrackerMD ROM.

## Building

You need [devkitPro](https://devkitpro.org/) with `devkitARM` and `libnds`.

```sh
./build.sh
```

The result is `GeneTrackerDS.nds`.

## Third-party code

This program includes work by others, under their own licences — see
[TIERS.md](TIERS.md):

- **ymfm** by Aaron Giles — YM2612 emulation, BSD 3-Clause.
- **emu76489** by Mitsutaka Okazaki — SN76489 emulation, MIT.

Both are compatible with the GPL, and their notices are kept in the sources.

## Licence

GNU General Public License version 3 — see [LICENSE](LICENSE).

Copyright (C) 2026 Audren Thibault

This program comes with **absolutely no warranty**. You are free to
redistribute and modify it under the terms of the GPL v3.

GeneTrackerDS belongs to a family: **GeneTracker** on iPad, **GeneTrackerMD**
on the Mega Drive, **GeneTrackerDS** here. The three are independent projects
— code is copied between them, never referenced — and **the licence of this
repository covers the DS version only**.

### Additional term (GPL v3, section 7)

Section 7(b) of the licence allows an author to require that attribution be
preserved. This project uses it, and it is the only condition added:

> **You must keep, in the source code and in the legal notices the program
> displays, the author credit "Audren Thibault" and the address of the
> original repository
> `https://github.com/AudrenThibault/MDTrackerDS`.**

In other words: do what you like with it, modify it, redistribute it, even
sell it — but **the name and the link stay**, in the files as on screen.
