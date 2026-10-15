# Product

## Register

product

## Users

Real Racing 3 players running a legitimate local native rehost on Nintendo
Switch. They use the application in handheld and docked contexts with a
controller, expecting the familiar RR3 flow rather than an engineering tool.

## Product Purpose

Rehost the existing ARM64 RR3 game runtime on Horizon/libnx without an Android
runtime, streaming wrapper, or WebView. Success means the original localized
game interface, timing, assets, input flow, audio, and offline race experience
run natively. Engineering status must remain truthful until the actual guest
menu and gameplay paths execute.

## Brand Personality

Premium motorsport, performance-focused, cinematic. Preserve the personality
already encoded by the original RR3 assets and motion instead of inventing a
new port brand.

## Anti-references

- A terminal or diagnostics dashboard presented as the finished game UI.
- Generic Nintendo Switch system styling layered over RR3.
- Web, mobile-web, or material-design controls.
- Fan-remaster neon, excessive glow, glass panels, or decorative gradients not
  present in the source game.
- Fake menu states that imply the guest runtime is playable when it is not.

## Design Principles

1. Original assets and localization are the source of truth.
2. Preserve original hierarchy, transitions, timing, and interaction cadence.
3. Adapt input to Joy-Con/controller semantics without visually redesigning the
   game.
4. Separate player-facing surfaces from hidden engineering diagnostics.
5. Show honest recovery or compatibility states when original functionality is
   not yet available.

## Accessibility & Inclusion

Prefer the Chinese localization already present in the game package, with
English fallback when a localized entry is absent. Preserve original motion as
requested. Keep text legible at 1280×720 handheld resolution and never encode a
critical recovery state by color alone. Controller focus and button prompts must
remain visible without touch input.
