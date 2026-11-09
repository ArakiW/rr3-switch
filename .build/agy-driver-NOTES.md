# The Antigravity CLI's semantic front-end driver, and the ONE correction to it.
#
# WHY THIS FILE IS HERE.  The driver was produced by the Antigravity CLI
# (gemini-3.8-flash-high, effort high) on 2026-11-09 from the brief in
# .build/agy-front-end-driver.txt.  Its full output is kept next to this file
# as agy-front-end-driver.md.
#
# WHAT IT GETS RIGHT
#
#   * It is TABLE-DRIVEN BY SCREEN NAME, which is the whole point: a
#     frame-based script cannot express "press the button that means go", and
#     it cannot express "do not press anything on this screen".
#   * IT ACTS ONCE PER SCREEN VISIT.  That is the v4 defect (a pulse WALKED
#     the guest into the NASCAR submenu) and the driver is explicitly built
#     not to repeat it.
#   * It navigates back to a raceable event with uiScreenOpen("EventMapScreen")
#     when the current screen has nothing to press.
#   * It reads ONLY the cached UiControlSample and the cached control
#     rectangles, and it is pumped from the INPUT PUMP, so the frame loop
#     never touches guest memory.
#   * It logs every decision under phase=fe-driver.
#
# THE ONE CORRECTION: THE TABLE'S CONTROL IDS.
#
# The driver's table presses BTN_CLOSE, ACTION_BTN, CARD_BUTTON and BTN_RACE.
# The guest's own .gui files say otherwise, and the archives agree:
#
#   ACTION_BTN        is on CareerBanner_foursquare, GarageBanner_foursquare,
#                      MTX_foursquare, MotorsportsBanner_foursquare,
#                      NativeSponsorshipAd_foursquare and
#                      OnlineMultiplayerBanner_foursquare -- BANNER screens.
#                      It is NOT on the event map.
#   CARD_BUTTON       does not exist in the index at all.
#   BTN_RACE          is on MainMenu and RaceStartScreen.  Correct.
#   MAP_EVENT_CARD_BTN is on EventMapScreen_STREAM_GROUP_*_card -- EVERY
#                      group -- and it is the PRESSABLE event card.  THAT is
#                      the control the event map needs, and it is the one the
#                      table is missing.
#
# The ids come from tools/gui_index.py, which decrypts all 939 of the
# guest's .gui files and indexes their 4,729 id_strings.  .build/gui-ids.py
# queries that index.
#
# AND THE TABLE NEEDS THE STREAM CHECK AS WELL, because the ids alone are not
# enough:
#
#   MAP_EVENT_CARD_STREAM is present only on the STREAMABLE groups (AMATEUR,
#   ELITE, EXPERT, LEGEND, MASTER, PRO, PRO_AM, career_group_card) and
#   absent on the MANUFACTURER cups (NASCAR, ENDURANCE, FORMULA_E,
#   SUPERCARS, EURO_TRACK_MASTERS, ...).  Those are the groups whose assets
#   are not on the card, so pressing their card wedges the guest on a 0 %
#   progress bar (round v4).
#
# ⇒ SO THE TABLE'S ENTRY FOR THE EVENT MAP IS:
#
#     on EventMapScreen:
#         if MAP_EVENT_CARD_STREAM is present  -> press MAP_EVENT_CARD_BTN
#         if it is NOT present                 -> uiScreenOpen("EventMapScreen")
#                                                 and pick another group
#
# ⇒ AND THE PROOF THAT THE IDS ARE RIGHT IS ROUND v5 (2026-11-09), which
# needed NO new code at all: rr3-ui.txt asked for MAP_EVENT_CARD_BTN and
# BTN_RACE, the pad script held the accelerator, and the guest reached the
# race with the normal 7 start lights and 10 race screens, with flips=1 and
# stalls=0.  That is the behaviour the driver's table has to reproduce, and
# it is the acceptance test for the driver.
