# Formal UI 0.20.0 result

## Outcome

The default NRO entry now presents a 1280x720 native framebuffer UI rather than
the libnx software console. The player-facing surfaces are Simplified Chinese;
the four-page engineering dashboard remains available only through
`--rr3-console` or the hidden L+R+X chord.

## Original-package fidelity

- original Conduit ITC Pro and Eurostile faces are embedded and used;
- Switch shared Simplified Chinese font renders bootstrap Chinese text;
- original `text_cn-Hans.txt` is mounted before `text_en.txt`;
- the localization catalog header and `langs.utf8` language declaration are
  validated before the UI reports the package localization as available;
- original `MenuTransitions.xml` is validated for the authored 0-40 menu and
  0-90 car-selection windows;
- nine original `.banim` tracks are embedded unchanged.

The bootstrap/status UI intentionally does not invent substitute animations.
The original motion tracks are reserved for the game renderer, where their
authored keyframe ranges must be consumed directly.

## Validation

- clean devkitA64 build: passed;
- NRO/NACP 0.20.0 metadata: passed;
- native framebuffer and original-asset contract: passed;
- guest TLS, cleanup, memory model, and startup plan contracts: passed;
- output NRO size: 49,754,217 bytes.

Device validation is still required for the new framebuffer presentation and
for guarded dependency constructors. RR3 main constructors remain gated.
