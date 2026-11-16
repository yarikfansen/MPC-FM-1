# FM-1 design v12 (approved decisions, 29.10.2026)

Mockups: `docs/mockups/FM1-v12-screens.html` (all screens), `FM1-v12-seq.html`, `FM1-v12-microd-screen.html`. Only DX7 ALGORITHM is approved so far; the rest waits for the user's "yes".

## Grid (240x240)
- Top row y 0..26: BPM left, title centre (track/sound name 20 px), PLAY + battery right.
- Row 2 y 27..61: F A S L 2x2 at 16 px (as afa1cb4); up to five plates 35x34 at x 44+39n; last plate (x 200) = state: "16"+horizontal chain (all but GLOBAL = CPU, SAVE = song number, DX7 = ALGO number).
- Central field y 62..190, x 5..235. Window frame 6,66,228x124 only where it existed (MICRO D, TRACK FX, LFO, BINAURAL, MASTER FX); none on MIXER, ENGINES, SEQ, SONGS, GLOBAL.
- Window name (TRACK FX, DRIVE, ...) lcd30 at y 73.
- Knobs y 195..240: colour line, label 16 px, value 24 px, fixed sizes; long values shortened (MICRO D -> MD).

## Colours
- Engines: MICRO D amber, DX7 green, 303 lime, 808 orange, PSG pink, PLUCK blue, SAMPLER yellow, BINAURAL violet (icon + preset name + MIXER number box and volume stripe).
- MIXER: vertical activity bar before the number; state letter S yellow / M light red / L,R teal, no circle.

## Names
- SAVE hold = SONGS (slots hold songs, plate shows song number); SEQ hold (chain) = MIX; PATTERN CHAIN is gone.
- MASTER FX: 10 effects, 2 pages of 5 plates.
- SEQ: 64 steps + 16 chain slots corner to corner, cursor drawn inside the cell.
