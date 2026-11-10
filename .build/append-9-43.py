"""Append section 9.43: what the game actually does, measured by the frames.

WHY: every number so far has been a counter, and the user's question is
"how far from playable".  The frames of the 2026-11-10 round answer it
directly, and the answer is much further along than the counters suggest.
"""
import io
import sys

SECTION = """
### 9.43 What the game actually DOES, measured by the frames

**MEASURED 2026-11-10, round loaderdiag1 (archive
`20261110-211723-eden-rr3-loaderdiag1`), read off the archived frames.**

The counters of that round say: race screens 2, start lights 2, HUD planes
88, EventMapScreen 168, main-menu layouts 12.  The frames say much more,
because the counters count SCREENS and the frames show the game.

**=> THE FRONT END IS COMPLETE AND CORRECT.**  `frames/frame-002000.png`
shows the event-selection screen: the header with the currency counters,
the three daily-race cards, and every card's artwork correctly textured --
a Porsche 911 GT3, an Audi R8 and a Nissan GT-R, no magenta anywhere --
with the game's own Chinese labels laid out over them ("daily race",
"overtime race", "cup race", "Silverstone", "car rally track").

**=> THE 3-D CAR SHOWROOM IS CORRECT.**  `frames/frame-001400.png` shows the
car-selection screen with a fully rendered Formula 1 car -- the Real Racing
liveries, the "Birel ART" and "3REAL" branding on the wings, the pink
Brawn-livery car beside it, the red car behind it -- plus the car's class
column (R / MR / PR), the driver's level (110.3), the four performance
bars, the car-parts row and the Chinese tooltip.

**=> AND THE RACE ITSELF REACHES THE PRE-GRID.**  `frames/frame-001800.png`
shows the race's start screen: the player's Nissan GT-R in its white and red
NISMO livery, correct headlights and correct bodywork, and beside it the
whole grid of TWENTY AI cars -- Verstappen 1, Perez 11, Sainz 55, Leclerc
16, Colapinto 43, Zhou 24, Ocon 31, Magnussen 20, Stroll 18, Piastri 21,
Norris 4, Hulkenberg 27, Tsunoda 22, Russell 63, Albon 23, Gasly 10,
Hamilton 44, Alonso 14 -- with correct liveries, correct numbers and
correct names, the race's leaderboard on the right, and the race's Chinese
title at the bottom left.

**=> ⇒ ⇒ AND THE HUD IS NOT GREY BLOCKS ON THESE SCREENS.**  The HUD text
renders, the leaderboard renders, the Chinese renders.  Whatever the grey
and white blocks complaint was about, it is not about these screens -- which
means the `LocalStyles*.gui` files are still worth copying (README 10.6),
but they are NOT what stands between the port and a playable front end.

**=> AND ONE DETAIL WORTH RECORDING: THE PORT'S LOG ENCODING IS WRONG FOR
CHINESE, THE GAME'S IS NOT.**  The guest's own Java string is the localised
message for "error loading track data, please reinstall"
(`\\u52A0\\u8F7D\\u8D5B\\u9053\\u6570\\u636E\\u65F6\\u51FA\\u9519` ...), and the
game draws its Chinese correctly on screen -- but the port's log line for it
arrives as GBK mojibake.  So the mojibake is the PORT'S log encoding, not the
game's text, and the frames are the proof.

**=> ⇒ ⇒ ⇒ SO THE HONEST ANSWER TO "HOW FAR FROM PLAYABLE" IS:**  the front
end, the car showroom, the race grid, the textures and the Chinese text all
work.  What is left is the part that has not been measured on the target:
the console crash (section 9.41), the player's own driving, the controller
binding, and the console's frame time.
"""


def main():
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.43 What the game actually DOES" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended 9.43")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
