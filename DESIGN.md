---
name: RR3 Native Rehost
description: Faithful Nintendo Switch presentation of the original Real Racing 3 interface.
colors:
  asphalt: "#111318"
  carbon: "#252A31"
  porcelain: "#F4F4F2"
  silver: "#C9CDD0"
  steel: "#8796AE"
  rr3-blue: "#1F9FE5"
  action-orange: "#FFA313"
  confirm-green: "#6BCF48"
  danger-red: "#ED3D3D"
  reward-gold: "#E3B33B"
typography:
  headline:
    fontFamily: "Conduit ITC Pro, sans-serif"
    fontSize: "42px"
    fontWeight: 800
    lineHeight: 1
    letterSpacing: "-0.01em"
  title:
    fontFamily: "Conduit ITC Pro, sans-serif"
    fontSize: "30px"
    fontWeight: 700
    lineHeight: 1.1
  body:
    fontFamily: "Conduit ITC Pro, sans-serif"
    fontSize: "24px"
    fontWeight: 500
    lineHeight: 1.2
  telemetry:
    fontFamily: "Eurostile LT Std, sans-serif"
    fontSize: "22px"
    fontWeight: 700
    lineHeight: 1
rounded:
  none: "0px"
  status: "50%"
spacing:
  xs: "8px"
  sm: "16px"
  md: "24px"
  lg: "40px"
  xl: "64px"
components:
  action-primary:
    backgroundColor: "{colors.action-orange}"
    textColor: "{colors.asphalt}"
    typography: "{typography.title}"
    rounded: "{rounded.none}"
    padding: "18px 34px"
  action-selected:
    backgroundColor: "{colors.rr3-blue}"
    textColor: "{colors.porcelain}"
    typography: "{typography.title}"
    rounded: "{rounded.none}"
    padding: "18px 34px"
  panel:
    backgroundColor: "{colors.porcelain}"
    textColor: "{colors.asphalt}"
    rounded: "{rounded.none}"
    padding: "24px"
---

# Design System: RR3 Native Rehost

## Overview

**Creative North Star: "The Original RR3 Garage"**

This surface is a continuation of the shipped Real Racing 3 interface, not a
Switch-themed launcher and not a fan remaster. Large white and silver planes,
hard diagonal cuts, black mechanical structure, condensed motorsport type and a
small set of saturated status colors carry the original identity.

Motion follows the source `MenuTransitions.xml`: transitions are camera or
panel movement tied to known frame ranges, never decorative UI choreography.
The player-facing shell shows only honest game or compatibility states;
engineering diagnostics live behind a deliberate developer entry.

**Key Characteristics:**

- Original atlas geometry and font families.
- Controller-first focus with high-contrast selected states.
- Flat, layered planes with diagonal edges instead of rounded cards.
- Chinese package localization first; English fallback.
- Original 40/90-frame transition timing.

## Colors

The palette is the decoded 2048×1536 common atlas vocabulary: neutral bodywork
with blue selection, orange action, green confirmation and red failure.

### Primary

- **RR3 Selection Blue**: active navigation, focused tiles and progress.
- **Action Orange**: start/continue and primary confirmations only.

### Secondary

- **Confirm Green**: successful readiness and safe completion.
- **Danger Red**: failures, destructive cancellation and recovery interruption.
- **Reward Gold**: rewards and premium game economy only, never port progress.

### Neutral

- **Asphalt**: full-screen foundation and high-contrast text on light planes.
- **Carbon**: secondary dark structure.
- **Porcelain/Silver**: the dominant original menu planes.
- **Steel**: inactive information and disabled regions.

**The Original Roles Rule.** A semantic color keeps the role it has in the
decoded atlas. Do not recolor the interface to create a new port identity.

## Typography

**Display Font:** Conduit ITC Pro ExtraBold
**Body Font:** Conduit ITC Pro Regular/Medium
**Label/Telemetry Font:** Eurostile LT Std Extended/Bold

**Character:** Condensed, direct and engineered. Chinese text uses the Switch
Simplified Chinese shared font because the Android package likewise relies on a
platform CJK face; Latin labels retain the original game fonts.

### Hierarchy

- **Headline** (800, 42px, 1.0): screen and mode names.
- **Title** (700, 30px, 1.1): selected actions and major state.
- **Body** (500, 24px, 1.2): localized explanations and recovery guidance.
- **Telemetry** (700, 22px, 1.0): versions, counts, hashes and developer data.

**The Condensed Interface Rule.** Use Conduit for the player flow and Eurostile
for numbers; never use a generic console monospace font in the main UI.

## Elevation

The original system is flat and structurally layered. Depth comes from
overlapping white/silver planes, black outlines, metallic ramps and directional
lighting inside source textures. Generic soft card shadows are forbidden.

**The Bodywork Rule.** Panels behave like fitted bodywork: hard edges, precise
overlap and limited metallic shading. They do not float as rounded cards.

## Components

### Buttons

- **Shape:** rectangular body with original diagonal end cut; no corner radius.
- **Primary:** action orange with asphalt text.
- **Selected:** RR3 blue with porcelain text and source highlight frame.
- **Disabled:** steel/silver with reduced contrast but readable labels.
- **Focus:** visible fill/state change plus controller prompt; never color alone.

### Cards / Containers

- **Corner Style:** hard square/angled corners.
- **Background:** porcelain, silver or asphalt planes.
- **Shadow Strategy:** tonal/texture layering only.
- **Border:** one-pixel black or metallic separator when present in the atlas.
- **Internal Padding:** 24–40px at 1280×720.

### Navigation

Horizontal RR3 navigation bars and large menu tiles use blue selected fills,
orange forward actions and original swipe/slide audio. L/R changes sections;
A confirms; B returns. Developer diagnostics require a non-player chord and are
never a visible main tab.

### Recovery State

Recovery is an honest compatibility surface in the same RR3 visual vocabulary:
red status icon, plain Chinese explanation, one clear safe action, and a hidden
technical detail affordance. It must never resemble an operating-system crash.

## Do's and Don'ts

### Do:

- **Do** use original `.gui`, atlas, font, music and transition resources as the
  source of truth.
- **Do** preserve 40-frame menu/event and 90-frame car-selection transitions.
- **Do** prioritize `text_cn-Hans.txt` and fall back to `text_en.txt`.
- **Do** render at 1280×720 with controller focus visible in handheld use.
- **Do** keep unavailable guest features explicit and recoverable.

### Don't:

- **Don't** present a terminal or diagnostics dashboard as the finished game UI.
- **Don't** apply generic Nintendo Switch system styling over RR3.
- **Don't** use WebView, Material controls, rounded cards or glassmorphism.
- **Don't** add fan-remaster neon, excessive glow or gradients not present in
  the source game.
- **Don't** show fake menu states that imply the guest runtime is playable.
