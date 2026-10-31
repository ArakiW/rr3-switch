# THE GUEST'S SCREEN REGISTRY, AND HOW TO OPEN ONE BY NAME

**WHAT THIS IS.**  The guest's front end keeps a registry of the screens it
knows, and it has its own function for opening one.  This is the address
book for that registry and the recipe for the call.

**WHY IT MATTERS.**  The feature matrix (docs/FEATURE-MATRIX.md) counts the
guest's UI feature FILES, and a run opens 59 of 967 -- 6%.  The racing half
works; the meta half -- the car shop, the garage, the upgrades, the
community, the profile -- has never been opened, because the port's UI
driver finds buttons by their COLOURS and can therefore only press what
happens to be accented.

Opening a screen BY NAME is the way out of that: the guest knows the name
of every screen it has, and the port can ask for one.

## WHERE IT LIVES IN THE GUEST

    G + 0x17f8          the FrontEnd2::MainMenuManager
    mgr + 0xa8          the registry's root (an intrusive tree)
    node + 0x20         the screen's NAME (a 24-byte libc++ string)
    node + 0x38         the screen's INSTANCE

    0x14bfb0c           MainMenuManager::RegisterAllScreens -- 65 Register calls
    0x14a6158           Register(name, instance)
    0x14ab198           the tree's insert
    0x14a7d5c           ScreenManager::Find(name)

    0x14a4820           FrontEnd2::Manager::OpenScreen(screen, flags)
    0x14a48ec           the switch itself (takes a 24-byte descriptor)
    0x14a62e4           the screen-advance routine

    mgr + 0x3b8         the screen stack's depth
    mgr + 0xb8 + i*0x30 the screen stack's entries

## THE RECIPE

    OpenScreen(mgr, screen, flags):            ; ELF 0x14a4820
        if (top_of_stack == screen) return    ; mgr+0x3b8, mgr+0xb8+(depth-1)*0x30
        bookkeeping(screen, mgr)               ; 0x10bb9c0
        bookkeeping(mgr, screen)               ; 0x14a44b8
        descriptor = { u32 kind = 0; u32 pad = 0;
                       u64 screen = screen; u8 flag = flags & 1; u32 one = 1; }
        switch(mgr, &descriptor)               ; 0x14a48ec

The port already knows each screen's INSTANCE by name -- the semantic UI
probe walks the same registry -- so the only new thing is the call.

## THE SCREENS

`mgr+` is where the instance lives inside the manager.  `computed` means the
Register call site took it from a register rather than a fixed offset; the
registry's own node+0x38 always holds the pointer, so the port does not
need the offset -- it looks the name up.

    AchievementScreen                            mgr+0x88 (computed)  Register@0x14bfc38
    AppleTVBluetoothControllerScreen             mgr+0x88 (computed)  Register@0x14bfec0
    AssetDownloadScreen                          mgr+0x870            Register@0x14bfcfc
    CHAMPIONSHIP_ROUND_HUB_SCREEN                mgr+0x74a8           Register@0x14c007c
    CarCustomisationScreen                       mgr+0x88 (computed)  Register@0x14bfd3c
    CarFilterScreen                              mgr+0x88 (computed)  Register@0x14bfd54
    CarPurchaseScreen                            mgr+0x88 (computed)  Register@0x14bfd6c
    CarSelectScreen                              mgr+0xe78            Register@0x14bfbc8
    CareerGroupScreen                            mgr+0x88 (computed)  Register@0x14bfed8
    CommunityScreen                              mgr+0x88 (computed)  Register@0x14bfc50
    DebugRaceSelectScreen                        mgr+0xb20            Register@0x14bfd10
    DriverSelectScreen                           mgr+0x6e10           Register@0x14bff90
    Early Access Hub                             mgr+0x7fc8           Register@0x14c0164
    EventMapScreen                               mgr+0x88 (computed)  Register@0x14bfce8
    EventsScreen                                 mgr+0x24f0           Register@0x14bfcb8
    ExclusiveSeriesScreen                        mgr+0x88 (computed)  Register@0x14bfef0
    F12019GPMultiCarSelectScreen                 mgr+0x59c8           Register@0x14bfea8
    FrontEndAwardScreen                          mgr+0x1240           Register@0x14bfca4
    GARAGE_OVERVIEW_SCREEN                       mgr+0x7480           Register@0x14c0018
    GhostChallengeMenu                           mgr+0x49e0           Register@0x14bfdf4
    HelpMenu                                     mgr+0x88 (computed)  Register@0x14bff38
    IntroScreen                                  mgr+0x88 (computed)  Register@0x14bfdb0
    LegalAndPrivacySettingsScreen                mgr+0x7470           Register@0x14bfff0
    LtsSyncScreen                                mgr+0x88 (computed)  Register@0x14bff20
    MOTORSPORT_CREW_DRIVER_SELECT_SCREEN         mgr+0x7490           Register@0x14c0040
    MOTORSPORT_CREW_PRINCIPAL_SELECT_SCREEN      mgr+0x7498           Register@0x14c0054
    MOTORSPORT_QUEST_DRIVER_SELECT_SCREEN        mgr+0x74a0           Register@0x14c0068
    MOTORSPORT_TEAM_SELECT                       mgr+0x7488           Register@0x14c002c
    MailScreen                                   x1, x19, #4, lsl #12 Register@0x14bfd98
    MainMenuPromotional                          mgr+0x5c0            Register@0x14bfe08
    ManufacturerDemoEventSelectScreen            mgr+0x88 (computed)  Register@0x14c00d4
    ManufacturerDemoGameModeScreen               mgr+0x88 (computed)  Register@0x14c00bc
    ManufacturerDemoMultiplayerTrackScreen       mgr+0x88 (computed)  Register@0x14c00ec
    ManufacturerDemoTrackEventScreen             mgr+0x88 (computed)  Register@0x14c0104
    MotorsportJourneyHubScreen                   mgr+0x6e00           Register@0x14bff68
    MotorsportJourneyHubScreen2019               mgr+0x6e08           Register@0x14bff7c
    MotorsportsExclusiveSeriesScreen             mgr+0x88 (computed)  Register@0x14bffdc
    MotorsportsSeriesScreen                      mgr+0x88 (computed)  Register@0x14bff08
    MyGarageScreen                               mgr+0x88 (computed)  Register@0x14bfbe0
    NewCarPurchasedScreen                        mgr+0x3388           Register@0x14bfc7c
    OrbitModeScreen                              mgr+0x88 (computed)  Register@0x14bfe20
    PartyPlayLocalScreen                         mgr+0x88 (computed)  Register@0x14bfe38
    PowerLegacyMultiCarSelectScreen              mgr+0x74b8           Register@0x14c00a4
    ProfileLoadSaveScreen                        mgr+0xe70            Register@0x14bfd24
    ProfileMenu                                  mgr+0x88 (computed)  Register@0x14bfc68
    ProfileMenuFriendCompare                     mgr+0x88 (computed)  Register@0x14bfde0
    QuestEventScreen                             mgr+0x88 (computed)  Register@0x14bfe80
    QuestIntroScreen                             mgr+0x88 (computed)  Register@0x14bfe50
    QuestMultiCarSelectScreen                    mgr+0x59c0           Register@0x14bfe94
    QuestOutroScreen                             mgr+0x88 (computed)  Register@0x14bfe68
    ROUND_REWARDS_SCREEN                         mgr+0x74b0           Register@0x14c0090
    RepairsScreen                                mgr+0x24e8           Register@0x14bfc20
    SeriesScreen                                 mgr+0x88 (computed)  Register@0x14bfcd0
    StreamIntroScreen                            mgr+0x88 (computed)  Register@0x14bfdc8
    TSMInfoScreen                                mgr+0x88 (computed)  Register@0x14bfd84
    TUNING_SCREEN                                mgr+0x7478           Register@0x14c0004
    TrophyUnlockScreen                           mgr+0xe80            Register@0x14bfc90
    UpgradesScreen                               mgr+0x24e0           Register@0x14bfc0c
    YourGarageScreen                             mgr+0x88 (computed)  Register@0x14bfbf8

## WHERE THIS STANDS

**MEASURED 2026-10-31 (q48).**  The call REACHES the guest -- the port logs
`phase=ui-open ... calling=1` -- and the guest then DIES: the log stops on
that line, and Eden reports writes to 0x30/0x34/0x48 of a null pointer.

The cause is visible in the recipe's own terms: **a screen that has never
been built has no components.**  The guest was building the screen's
component tree and walking a null root.

So there is one step missing, and it is the guest's own:

    vtable slot 69       the screen's XML LOADER

    0xdb8c90  EventMapScreen's slot 69:
        0xdb8cb4  adrp x8, #0x1f6d000
        0xdb8cb8  ldr  x8, [x8, #0x840]      ; the manager
        0xdb8cbc  ldr  x8, [x8]
        0xdb8cc0  mov  x0, x8
        0xdb8cc4  bl  0x13833cc              ; the front-end gate
        0xdb8cc8  tbz w0, #0, ...
        0xdb8cd4  adrp x1, #0x449000          ; "EventMapScreen.xml"
        0xdb8cd8  add  x1, x1, #0x765

    vtable slot 73       the screen's NAME getter
                        (0x9faa1c in the base, 0xdb97dc in EventMapScreen)

**SO THE NEXT STEP IS: build the screen first (slot 69), then open it.**

Until that is done the port REFUSES rather than crashes: the instance's
first word must point into the guest's vtable section (0x1e1aae0 ..
0x1f6cd80, which is .data.rel.ro -- ABOVE the executable region, so the
text's end is NOT the vtables' end), or the request is reported and skipped.

**NOTHING IS ACTIVE BY DEFAULT.**  The card's rr3-open.txt ships with every
line commented out.

---

## AFTER THE BUILD: MEASURED (q51)

    phase=ui-open screen="CarPurchaseScreen" refused=1
              reason=the-screen-did-not-build
    phase=ui-open screen="UpgradesScreen" built=1 children=1
    phase=ui-open screen="UpgradesScreen" calling=1          <- and then nothing

**=>=> THE BUILD STEP WORKS.**  UpgradesScreen's own XML loader (slot 69)
DID produce a component tree, and the port went on to OpenScreen -- and the
guest died with writes to 0x30/0x34/0x48 of a null pointer.

**=>=> SO THE MISSING BUILD WAS NECESSARY BUT NOT SUFFICIENT.**  A screen
with ONE child is built but not laid out.

**AND THE TWO CANDIDATES FOR WHAT IS STILL MISSING.**  OpenScreen runs two
bookkeeping calls before the switch, and the second one is where the answer
is:

    0x14a44b8  mgr-bookkeeping(mgr, screen)
        0x14a44e4  ldr  x8, [x1]            ; the screen's vtable
        0x14a44f0  ldr  x8, [x8, #0x248]    ; slot 73 -- the NAME getter
        0x14a44f4  blr  x8
        0x14a44fc  bl   0x1e17bf0            ; an EXTERNAL function (a PLT
                                           ; thunk into the GOT at
                                           ; 0x1f7e890 -- so this is a
                                           ; C++ library call, not a
                                           ; guest-internal lookup)
        0x14a450c  cmp  x0, #0x17            ; and its result is used as an
                                           ; index, and the index is 0..23

**=>=> AND THE NAMES ARE NOT THE SAME STRING.**  Slot 73 returns a plain
`const char*`, and for EventMapScreen it returns

    0xdb97dc  adrp x0, #0x39f000        ; "Stream Selection"
    0xdb97e0  add  x0, x0, #0xb1b
    0xdb97e4  ret

-- "Stream Selection", NOT "EventMapScreen".  The base class's slot 73
(0x9faa1c) returns another string.  **=>=> SO A SCREEN HAS TWO NAMES: the
name it is REGISTERED under (the registry, which the port has) and the name
it CALLS ITSELF (slot 73).  They are not the same string.**

**=>=> AND WHAT THE GUEST DOES WITH THE SECOND NAME.**  The external call
returns an INDEX, and the code checks it:

    0x14a44fc  bl   0x1e17bf0        ; the screen's own name -> an index
    0x14a450c  cmp  x0, #0x17        ; and the index must be < 23
    0x14a4510  b.hs 0x14a452c        ; otherwise: the error path

So the front end has a table of at most 24 FRONT-END screens, and a screen
that is not one of them cannot be opened this way.  **There are 59
registered screens and only 24 front-end slots, so most of the meta screens
are NOT openable this way -- and the open is done by the screen's DISPLAY
name, not by its registry name.**

**=>=> THE NEXT STEP IS THEREFORE A RUNTIME READ, NOT MORE GUESSING.**
The port can call slot 73 (a plain getter, no side effects) to learn each
screen's display name, and it can read the front end's table to see which
of the 59 screens have a slot at all.  Both are reads.  Until that is done
the open stays off.

The other candidate is the flags word: the descriptor's byte at +0x10 is
`flags & 1`, and the port passes 0.  It is a one-bit experiment.

**AND UNTIL THE OPEN IS PROVEN, IT IS TWO LEVELS.**  A screen name on a
line of its own makes the port BUILD the screen and report whether the
build worked -- which IS proven.  The card's "open=1" line is what turns
the open on.

---

## THE DISPLAY NAMES, MEASURED (q52)

The port now calls each registered screen's vtable slot 73 -- a plain
`const char*` getter with no side effects -- and logs every name and its
length, once per run.  This is the front end's own vocabulary, and it is
NOT the registry's:

    MyGarageScreen=Garage Screen(13)            EventsScreen=Event Select Screen(19)
    CarSelectScreen=Car Select Screen(17)       AchievementScreen=Achievement Screen(18)
    CarPurchaseScreen=Car Purchase Screen(19)    CommunityScreen=Community Screen(16)
    EventMapScreen=Stream Selection(16)          ProfileMenu=Profile Screen(14)
    UpgradesScreen=Upgrades Screen(15)           RepairsScreen=Repairs Screen(14)
    HelpMenu=Help Screen(11)                     MailScreen=Mail Screen(11)
    CarCustomisationScreen=Customisation Screen(20)  TUNING_SCREEN=TUNING_SCREEN(13)
    GARAGE_OVERVIEW_SCREEN=GARAGE_OVERVIEW_SCREEN(22)  TrophyUnlockScreen=Trophy Unlock Screen(20)
    GhostChallengeMenu=GhostChallengeMenu(18)    PartyPlayLocalScreen=Party Play(10)
    PhotoModeScreen=PhotoModeScreen(15)          NewCarPurchasedScreen=New Car Purchased Screen(24)
    ProfileLoadSaveScreen=Profile Load/Save Screen(24)
    FrontEndAwardScreen=Front End Award Screen(22)  ManufacturerDemoGameModeScreen=ManufacturerDemoPromo(21)
    MOTORSPORT_TEAM_SELECT=MOTORSPORT_TEAM_SELECT(22)
    CHAMPIONSHIP_ROUND_HUB_SCREEN=CHAMPIONSHIP_ROUND_HUB_SCREEN(29)
    CarFilterScreen=Car Manufacturer Filter Screen(30)
    AppleTVBluetoothControllerScreen=Apple TV Bluetooth Controls Screen(34)
    MOTORSPORT_QUEST_DRIVER_SELECT_SCREEN=MOTORSPORT_QUEST_DRIVER_SELECT_SCREEN(37)

**=>=> THE GUEST'S OWN BUFFER IS 24 BYTES, SO A NAME OF 23 CHARACTERS OR MORE
TAKES ITS ERROR PATH.**  Six of the screens are too long to be opened this
way: AppleTVBluetoothControllerScreen (34), MOTORSPORT_QUEST_DRIVER_SELECT_SCREEN
(37), CarFilterScreen (30), CHAMPIONSHIP_ROUND_HUB_SCREEN (29),
NewCarPurchasedScreen (24) and ProfileLoadSaveScreen (24).

**=>=> BUT THE LENGTH IS NOT WHAT STOPPED US.**  UpgradesScreen's display name
is "Upgrades Screen" -- 15 characters, comfortably inside the guest's buffer
-- and it still took the guest down (q51).  So the check that kills the
guest is further along: the bookkeeping compares the display name against
the front end's own table of names, and THAT comparison is where it dies.

**=>=> AND THE BUILD ITSELF WORKS FOR THE SCREENS THAT MATTER.**  q52, with
the card asking for three screens and WITHOUT the open turned on:

    CarPurchaseScreen  refused=1  reason=the-screen-did-not-build
    UpgradesScreen     built=1 children=1  display="Upgrades Screen" (15)
    CommunityScreen    built=1 children=3  display="Community Screen" (16)

So the guest's own XML loader builds CommunityScreen's tree with three
children, and UpgradesScreen's with one.  Only CarPurchaseScreen's build
failed -- and that one is refused and reported, not crashed.

## THE GUEST HAS ANOTHER WAY TO OPEN A SCREEN, AND IT IS THE ONE TO USE

The front end's own entry (ELF 0xa679a0) opens a screen WITHOUT any of the
descriptor machinery:

    0xa679fc  ldr  x8, [x8, #0x5d8]   ; G
    0xa67a08  ldr  x8, [x8]           ; G
    0xa67a0c  ldr  x0, [x8, #0x17f8]  ; the screen manager
    0xa67a10  bl   0x14a7d5c          ; ScreenManager::Find("EventMapScreen")
    0xa67a14  cbz  x0, ...            ; if null, bail
    0xa67a2c  bl   0x1e17c80          ; __dynamic_cast<EventMapScreen*>
    0xa67a3c  bl   0xdac364           ; EventMapScreen::Enter(1)

**=>=> FIND BY NAME, THEN ENTER.  No descriptor, no bookkeeping, no 24-slot
table, no length check.**  And the port can supply the name argument without
allocating anything in guest memory: a registry name is a SHORT libc++
string, so its characters are INLINE in the registry node, and the address of
those characters IS the `const char*` that Find wants.

**=>=> SO THE NEXT STEP IS: call the guest's own Find (0x14a7d5c) with the
registry node's own name address, and then the screen's Enter.**

---

## THE NAME LITERALS: THE PORT DOES NOT HAVE TO BUILD A STRING (q53)

Every registered screen's name is a plain literal in the guest's .rodata:

    EventMapScreen           0x3fcc66
    CarPurchaseScreen        0x3a87fe
    CommunityScreen          0x4175e4
    UpgradesScreen           0x3fec1e
    CarCustomisationScreen   0x47a4ff
    MyGarageScreen           0x4503be

**=>=> SO THE PORT CAN CALL THE GUEST'S OWN ScreenManager::Find
(0x14a7d5c) WITH THE ADDRESS OF THE NAME LITERAL.**  No string is built, no
guest memory is allocated, and the lookup is the guest's own.

**AND THE DEEP LINKS ARE NOT THE GENERAL MECHANISM.**  The guest's action
#145 is "Main Menu|Enter Deep Link" (its function is at 0xa75734, and it
prints "Enter Deep Link (rr3:// will be added):"), and the image contains
only seven deep-link literals:

    rr3://RaceTeamsAdmin            0x39e0a8
    rr3://MULTIPLAYERINVITE/        0x3b1d7c
    rr3://RaceTeams                 0x3eee4f
    rr3://STREAM/STREAM_ID          0x46e02e
    rr3://MULTIPLAYERINVITE/server:port:room:key:etc   0x47c657

so a deep link is not how a screen is opened in general.

**=>=> WHICH LEAVES ONE THING, AND IT IS SMALL.**  `Enter` is not virtual
(0xdac364 is not a vtable entry), so it cannot be called through the
vtable.  BUT THE GUEST HAS A PER-SCREEN ENTRY FOR EVERY SCREEN IT CAN
OPEN, and each one is exactly the guest's own path:

    Find(name literal) -> __dynamic_cast -> Enter(1)

The guest's front-end entry for the event map is at 0xa679a0 and reads:

    0xa67a0c  ldr  x0, [x8, #0x17f8]   ; the screen manager
    0xa67a10  bl   0x14a7d5c            ; Find("EventMapScreen")
    0xa67a2c  bl   0x1e17c80            ; __dynamic_cast
    0xa67a3c  bl   0xdac364             ; EventMapScreen::Enter(1)

**=>=> SO THE NEXT STEP IS A SCAN, NOT A GUESS: find the function that
references a wanted screen's name literal, and call THAT.**  The guest then
does its own Find, its own cast and its own Enter, with all of its own
preconditions satisfied -- which is exactly what the port cannot reproduce
by hand.

**AND THE ENTRY'S OWN SCREEN NAME IS THE ANCHOR, NOT ITS ADDRESS.**  The
xref machinery is the same one that proved the vtable slots: scan .text for
the adrp/add pair that forms the literal's address.
