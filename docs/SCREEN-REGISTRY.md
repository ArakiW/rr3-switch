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
first word must point into the guest's vtable section, or the request is
reported and skipped.

**NOTHING IS ACTIVE BY DEFAULT.**  The card's rr3-open.txt ships with every
line commented out.
