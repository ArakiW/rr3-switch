# THE FEATURE MATRIX

**WHAT THIS IS.**  The guest's own feature list, and what a real
run has actually opened.  It exists because "完整移植安卓端功能"
had no countable content: nobody could say what was done.

**THE GUEST NAMES ITS FEATURES ITSELF.**  Every screen and panel it
has is a GUI file under `gui/processed/`, named after the feature
(`BankScreen.gui`, `BuyCarScreen.gui`, `CrewScreen.gui`,
`CommunityScreen.gui`, `online_multiplayer/`, `store/`, `quests/`),
and every asset it expects is named in its asset lists.  So the
inventory is READABLE, and a run says which of it it touched.

## THE NUMBERS

| | |
|---|---|
| the guest's whole inventory | **40864 assets** in **1090 features** |
| the UI feature files (`gui/processed/*`) | **967** |
| what a full run (rr3-q47-open-new-screen) actually opened | **59 of 967** UI features (6%) |
| distinct asset paths the run touched | 9818 |

**=>=> SO THE HONEST ANSWER TO "how much is left" IS THIS: the
racing half of the game works and the meta half is untouched.**
The run reaches the event map, the car select, a race, the pause
screen and the store menu -- and it has never opened a single
bank, garage-customisation, crew, community, friends,
leaderboard, multiplayer or quest screen.

## WHAT THE RUN DID OPEN (59)

- `/gui/processed/AssistsPopupper`
- `/gui/processed/BackButtonLayout`
- `/gui/processed/BuyCarBar`
- `/gui/processed/CarFilterScreen`
- `/gui/processed/CarFilter_Page`
- `/gui/processed/CarSelect`
- `/gui/processed/CarSelect_DeliveryPanel`
- `/gui/processed/CarSelect_UpgradePanel`
- `/gui/processed/CarStatsBar`
- `/gui/processed/CurrencyLabel`
- `/gui/processed/Drive_bar_slot`
- `/gui/processed/EventMapScreen`
- `/gui/processed/EventMapScreen_STREAM_GROUP_CAREER_card`
- `/gui/processed/EventMapScreen_STREAM_GROUP_MOTORSPORTS_card`
- `/gui/processed/EventMapScreen_Scroller`
- `/gui/processed/EventMapScreen_TSM_item`
- `/gui/processed/EventMapScreen_WeeklyTimeTrialCompetition`
- `/gui/processed/EventMapScreen_event_page_TimeTrialTournament`
- `/gui/processed/Event_card`
- `/gui/processed/Event_item`
- `/gui/processed/EventsScreen`
- `/gui/processed/GlobalStyles`
- `/gui/processed/GuiOptionSlider`
- `/gui/processed/Hud_Objective`
- `/gui/processed/Hud_Opponent`
- `/gui/processed/InGameScreen`
- `/gui/processed/LimitedEditionCallout`
- `/gui/processed/LoadingScreen_Firemint`
- `/gui/processed/LoadingScreen_Init`
- `/gui/processed/LoadingScreen_R3`
- `/gui/processed/LoadingScreen_Track`
- `/gui/processed/Main_menu_foursquare_shadow`
- `/gui/processed/MotorsportsBanner_foursquare`
- `/gui/processed/PauseScreen`
- `/gui/processed/PreRaceScreen`
- `/gui/processed/PreRace_Intro_SoloItem`
- `/gui/processed/SettingsMenu`
- `/gui/processed/SettingsToolbar_ProfileSocial`
- `/gui/processed/SettingsToolbar_Settings`
- `/gui/processed/SpriteAnimation`
- `/gui/processed/StartLightsScreen`
- `/gui/processed/StatusIconBar`
- `/gui/processed/StoreMenu`
- `/gui/processed/TSM_WinLosses_item`
- `/gui/processed/Timeline`
- `/gui/processed/Timeline_Node`
- `/gui/processed/Timeline_Percent`
- `/gui/processed/ToasterPopupGamepad`
- `/gui/processed/carselect_expiry_component`
- `/gui/processed/championships`
- `/gui/processed/early_access_track`
- `/gui/processed/exclusive_series`
- `/gui/processed/ftue_rect_scene`
- `/gui/processed/full_upgrades_bonus`
- `/gui/processed/limited_time_series`
- `/gui/processed/main_menu`
- `/gui/processed/pitlanebar`
- `/gui/processed/rookie_championship`
- `/gui/processed/ultimate_driver`

## WHAT IS STILL UNTOUCHED (908), BY FAMILY

| family | untouched features |
|---|---|
| **EventMapScreen** | 39 |
| **LoadingScreen** | 36 |
| **BuyCarBar** | 24 |
| **F** | 24 |
| **lemans** | 22 |
| **Speedrush** | 16 |
| **Eagle** | 13 |
| **gte** | 13 |
| **n** | 13 |
| **nascar** | 13 |
| **renault** | 13 |
| **LeaderboardRow** | 12 |
| **Pack** | 12 |
| **PreRaceScreen** | 12 |
| **asterions** | 12 |
| **countach** | 12 |
| **lions** | 12 |
| **reign** | 12 |
| **stingray** | 12 |
| **ContextMenu** | 11 |
| **PackScreen** | 11 |
| **RaceTeamListRow** | 11 |
| **StoreItem** | 10 |
| **trackday** | 10 |
| **tts** | 10 |
| **Profile** | 9 |
| **LMScroller** | 8 |
| **ControlsMenu** | 6 |
| **MTXTileCardContent** | 6 |
| **Popup** | 6 |
| **PreRace** | 6 |
| **ConfirmCancelPopup** | 5 |
| **CrewScreen** | 5 |
| **Event** | 5 |
| **EventLeaderboardScreen** | 5 |
| **IntroScreen** | 5 |
| **RepairScreen** | 5 |
| **SettingsToolbar** | 5 |
| **Store** | 5 |
| **TimeTrialTournament** | 5 |
| **AwardsScreen** | 4 |
| **FirstRacePopup** | 4 |
| **Lemans** | 4 |
| **MDollarPopupBonus** | 4 |
| **SponsorCollectionPopup** | 4 |
| **TutorialScreen** | 4 |
| **Upgrade** | 4 |
| **BankScreen** | 3 |
| **Bootcamp** | 3 |
| **CarPurchase** | 3 |
| **CarUnlockRequirementRow** | 3 |
| **Maserati** | 3 |
| **Orientation** | 3 |
| **RaceTeamEventLabel** | 3 |
| **RaceTeamTab** | 3 |
| **StorePackCard** | 3 |
| **StorePackCardContent** | 3 |
| **TutorialPopup** | 3 |
| **academy** | 3 |
| **rrtv** | 3 |
| **speedrush** | 3 |
| **CRI** | 2 |
| **DownloadingPopup** | 2 |
| **DragRace** | 2 |
| **EventRequirement** | 2 |
| **FilterPopup** | 2 |
| **GoldCarUnlockPopup** | 2 |
| **HyundaiRewards** | 2 |
| **Instructions** | 2 |
| **Leaderboard** | 2 |
| **LeaderboardGroupSync** | 2 |
| **MailScreen** | 2 |
| **NascarRaceTicker** | 2 |
| **OnlineMultiplayerConnectPopup** | 2 |
| **PurchaseCarPopup** | 2 |
| **R** | 2 |
| **RaceTeamLeaderboardTab** | 2 |
| **RaceTeamPopup** | 2 |
| **RepairsScreen** | 2 |
| **ResultsContainer** | 2 |
| **SaveGame** | 2 |
| **SeriesScreen** | 2 |
| **SocialMediaInvitePopup** | 2 |
| **Summary** | 2 |
| **TTC** | 2 |
| **Timeline** | 2 |
| **Tournament** | 2 |
| **TrackSelectMenu** | 2 |
| **UpgradeTypeScreen** | 2 |
| **UpgradesScreen** | 2 |
| **car** | 2 |
| **power** | 2 |
| **r** | 2 |
| **store** | 2 |
| **Achievement** | 1 |
| **AchievementBar** | 1 |
| **AchievementPopup** | 1 |
| **AchievementScreen** | 1 |
| **AdLoadingPage** | 1 |
| **AllowMarketingInfoPopup** | 1 |
| **AppleTVBluetoothControllerScreen** | 1 |
| **AppleTVSupportPopup** | 1 |
| **AssetDownloadScreen** | 1 |
| **AutoPlayAd** | 1 |
| **AwardScreen** | 1 |
| **BankScrollerScreen** | 1 |
| **Banned** | 1 |
| **BonusSeriesUnlocked** | 1 |
| **BootcampQuestIntro** | 1 |
| **BootcampQuestLandingPage** | 1 |
| **BoughtNewCarScreen** | 1 |
| **BuyCarScreen** | 1 |
| **CarCustomisationScreen** | 1 |
| **CarDebugViewer** | 1 |
| **CarDebugViewerPaintSwatch** | 1 |
| **CarLabel** | 1 |
| **CarLockedPopup** | 1 |
| **CarSalesPopup** | 1 |
| **CarStatsBar** | 1 |
| **CarUnlockRequirementSpacer** | 1 |
| **CarUnlockedScreen** | 1 |
| **CareerBanner** | 1 |
| **Character** | 1 |
| **CinematicLogo** | 1 |
| **CloudSaveToaster** | 1 |
| **CloudSaveToasterFail** | 1 |
| **CommunityScreen** | 1 |
| **ConfirmCancelPopupScroll** | 1 |
| **ConfirmCancelWithCheckboxPopup** | 1 |
| **ConfirmPopupScroll** | 1 |
| **CrewAlertPopup** | 1 |
| **CrewPopup** | 1 |
| **CrewPopupCard** | 1 |
| **CurveEditor** | 1 |
| **CustomMenuTileTest** | 1 |
| **CustomisationBarPaintSwatch** | 1 |
| **CustomisationCategoryButton** | 1 |
| **CustomisationItem** | 1 |
| **CustomisationItemColour** | 1 |
| **CustomisationItemDecal** | 1 |
| **CustomisationItemGroup** | 1 |
| **CustomisationItemPaint** | 1 |
| **CustomisationItemSuspension** | 1 |
| **CustomisationItemWheel** | 1 |
| **CustomisationLandingScreen** | 1 |
| **CustomisationLoadoutItem** | 1 |
| **CustomisationLoadoutPopup** | 1 |
| **CustomisationSelectScreen** | 1 |
| **CustomisationUnlockedPopup** | 1 |
| **CustomiseDecalsScreen** | 1 |
| **CustomisePaintScreen** | 1 |
| **CustomiseRideHeightScreen** | 1 |
| **Data** | 1 |
| **DebugCreateSalePopup** | 1 |
| **DebugInfoScreen** | 1 |
| **DebugPendingSalesPopup** | 1 |
| **DebugRaceSelectScreen** | 1 |
| **DebugRecPackEntry** | 1 |
| **DebugRecPackPurchasePopup** | 1 |
| **DebugSaleEntry** | 1 |
| **DeferredStorePaymentPopup** | 1 |
| **DiskSpaceRequired** | 1 |
| **DisplayMenu** | 1 |
| **DownloadWaitScreen** | 1 |
| **DragRaceControls** | 1 |
| **DragRaceRoundEndButtons** | 1 |
| **DriveRefillPopup** | 1 |
| **DriverAssists** | 1 |
| **ElitePlayerBadge** | 1 |
| **ElitePlayerPopUp** | 1 |
| **FacebookLegalPopup** | 1 |
| **FailedVipClaimPopup** | 1 |
| **FalseStartScreen** | 1 |
| **Formula** | 1 |
| **FrontEndMessagePopup** | 1 |
| **FullScreenConfirmCancelPopup** | 1 |
| **GarageBanner** | 1 |
| **GarageScreen** | 1 |
| **GarageSearchEntry** | 1 |
| **GarageSearchPopup** | 1 |
| **GauntletToEventPopup** | 1 |
| **GhostChallenge** | 1 |
| **GhostChallengeAward** | 1 |
| **GhostChallengeLoading** | 1 |
| **GroupSelectScreen** | 1 |
| **GuiSwitch** | 1 |
| **GuiTripleSwitch** | 1 |
| **HSVColourPicker** | 1 |
| **HTMLViewerPopup** | 1 |
| **HUD** | 1 |
| **HelpScreen** | 1 |
| **HyundaiCompetitionPopup** | 1 |
| **InspectCar** | 1 |
| **InterstitialAdPopup** | 1 |
| **Job** | 1 |
| **JobDescriptionScreen** | 1 |
| **JobsBar** | 1 |
| **KnownIssuesPopup** | 1 |
| **LMQuestsScreen** | 1 |
| **LeMans** | 1 |
| **LeMansIntro** | 1 |
| **LeMansOfferPopup** | 1 |
| **LegalAndPrivacySettingsScreen** | 1 |
| **LemansQuestLandingPage** | 1 |
| **LimitedEditionLogo** | 1 |
| **LiveryCustomisationScreen** | 1 |
| **LockedSeriesPopup** | 1 |
| **LtsTermsPopup** | 1 |
| **MDollarPopup** | 1 |
| **MDollarPopupFrame** | 1 |
| **MDollarPopupRace** | 1 |
| **MDollarRewardScreen** | 1 |
| **MTX** | 1 |
| **MainMenu** | 1 |
| **MainMenuAndroidTvDemo** | 1 |
| **MainMenuPromotional** | 1 |
| **MaintenanceConfirmPopup** | 1 |
| **MaseratiConcludePopup** | 1 |
| **MaseratiOfferPopup** | 1 |
| **MaseratiQuestIntro** | 1 |
| **MaseratiQuestLandingPage** | 1 |
| **MaseratiQuestOutro** | 1 |
| **MessagePopup** | 1 |
| **MessageRepairPopup** | 1 |
| **MotorsportsSeasonQuestCard** | 1 |
| **MotorsportsSeasonQuestsScreen** | 1 |
| **MoviePlayerPopup** | 1 |
| **MultipleCurrencyConfirmPopup** | 1 |
| **MyCarsScreen** | 1 |
| **NativeSponsorshipAd** | 1 |
| **NativeSponsorshipAdPopup** | 1 |
| **NewCrewPopup** | 1 |
| **NewLabel** | 1 |
| **NewStoreItem** | 1 |
| **NewUpdatePopup** | 1 |
| **NewsPopup** | 1 |
| **NfcDebugScreen** | 1 |
| **OfferwallPopup** | 1 |
| **Online** | 1 |
| **OnlineMultiPlayerLandingPage** | 1 |
| **OnlineMultiplayer** | 1 |
| **OnlineMultiplayerBanner** | 1 |
| **OnlineMultiplayerInGameScreen** | 1 |
| **OnlineMultiplayerInfoCard** | 1 |
| **OnlineMultiplayerLandingCard** | 1 |
| **OnlineMultiplayerMainMenuCard** | 1 |
| **OnlineMultiplayerResultsScreen** | 1 |
| **OnlineMultiplayerRewardsCard** | 1 |
| **OnlineMultiplayerSyncingCard** | 1 |
| **OnlineScreen** | 1 |
| **OrbitModeScreen** | 1 |
| **PartyPlayLocal** | 1 |
| **PayCancelPopup** | 1 |
| **PerformanceIssuePopup** | 1 |
| **PhotoMode** | 1 |
| **PhotoModeFilterItem** | 1 |
| **PhotoModeFilterPack** | 1 |
| **PitLaneBar** | 1 |
| **PostRaceBackground** | 1 |
| **PreRepairScreen** | 1 |
| **ProfileFriendCompareScreen** | 1 |
| **ProfileLoadSaveScreen** | 1 |
| **ProfileScreen** | 1 |
| **PromoPopup** | 1 |
| **PromoRedeemedPopup** | 1 |
| **PullDown** | 1 |
| **PurchaseAwardedPopup** | 1 |
| **PurchaseItemsPopup** | 1 |
| **PurchaseProcessingPopup** | 1 |
| **PurchaseValidationPopup** | 1 |
| **QuestConcludedPopup** | 1 |
| **QuestMultiCarSelect** | 1 |
| **QuestMultiCarSelectScreen** | 1 |
| **QuestProgressLabel** | 1 |
| **Quests** | 1 |
| **QuestsLocked** | 1 |
| **QuestsPopup** | 1 |
| **RGBColourPicker** | 1 |
| **RRTVPostContentPopup** | 1 |
| **RaceCompleteScreen** | 1 |
| **RacePositionScreen** | 1 |
| **RaceSchoolBasicQuestIntro** | 1 |
| **RaceSchoolBasicQuestLandingPage** | 1 |
| **RaceStartScreen** | 1 |
| **RaceTeamCreatePage** | 1 |
| **RaceTeamGoalsTab** | 1 |
| **RaceTeamHub** | 1 |
| **RaceTeamInfoTab** | 1 |
| **RaceTeamInvite** | 1 |
| **RaceTeamJoinPage** | 1 |
| **RaceTeamLandingPage** | 1 |
| **RaceTeamLeaderboardTeam** | 1 |
| **RaceTeamLoadingPage** | 1 |
| **RaceTeamMainMenuBanner** | 1 |
| **RaceTeamManageTab** | 1 |
| **RaceTeamMembersTab** | 1 |
| **RaceTeamMembersWall** | 1 |
| **RaceTeamMembersWallEpoch** | 1 |
| **RaceTeamMembersWallPost** | 1 |
| **RaceTeamMembersWallSystemMessage** | 1 |
| **RaceTeamPage** | 1 |
| **RaceTeamRequest** | 1 |
| **RaceTeamRewards** | 1 |
| **RaceTeamTabNotification** | 1 |
| **RaceTeamTeamGoals** | 1 |
| **RaceTeamTeamGoalsRewards** | 1 |
| **RaceTeamTieredRewards** | 1 |
| **RaceTeamToasterNotitifcation** | 1 |
| **RaceTeamToasterPopup** | 1 |
| **RacingSchoolBasicsLotusQuestIntro** | 1 |
| **RealRacingTv** | 1 |
| **RealRacingTvItemHighlightPopup** | 1 |
| **RegeraOfferPopup** | 1 |
| **RentCarPopup** | 1 |
| **RewardPopup** | 1 |
| **ScreenOverscanPopup** | 1 |
| **SocialMediaImagePostPopup** | 1 |
| **SocialMediaLoginPopup** | 1 |
| **SocialMediaPostPopup** | 1 |
| **SoloResults** | 1 |
| **SpeedrushOfferPopup** | 1 |
| **SpeedrushQuestIntro** | 1 |
| **SpeedrushQuestLandingPage** | 1 |
| **StandardRaceResultRow** | 1 |
| **StorePackCardFrame** | 1 |
| **StorePurchaseItem** | 1 |
| **StorePurchaseSaleItem** | 1 |
| **StoryPopup** | 1 |
| **StreamIntro** | 1 |
| **SuggestiveSellPopup** | 1 |
| **SuspensionAdjustmentHandle** | 1 |
| **TSMInfoScreen** | 1 |
| **TermsAndConditionsPopup** | 1 |
| **TestDriveScreen** | 1 |
| **TextEntryPopup** | 1 |
| **TextEntryPopupLarge** | 1 |
| **TextRenderingTest** | 1 |
| **ThirdPartyAdsPopup** | 1 |
| **TimeTrial** | 1 |
| **TimeTrialBanner** | 1 |
| **TimeTrialMenu** | 1 |
| **TimeTrialTournamentAggregateScreen** | 1 |
| **TimeTrialTournamentAwardScreen** | 1 |
| **TimeTrialTournamentSyncScreen** | 1 |
| **ToasterPopupIconText** | 1 |
| **ToasterPopupMail** | 1 |
| **ToasterQuestStatus** | 1 |
| **TournamentProgress** | 1 |
| **TrophyUnlockScreen** | 1 |
| **TutorialPopupWithTwoOptions** | 1 |
| **UnderMaintenance** | 1 |
| **UnlockPopup** | 1 |
| **UnlockedSeriesPopup** | 1 |
| **VipBar** | 1 |
| **VipDeliveryPopup** | 1 |
| **WaitingPopup** | 1 |
| **WallOfTextPopup** | 1 |
| **WebImage** | 1 |
| **WelcomeBackPopup** | 1 |
| **YesNoCancelPopup** | 1 |
| **academyLandingPage** | 1 |
| **academyQuestIntro** | 1 |
| **appletv** | 1 |
| **auto** | 1 |
| **bg** | 1 |
| **boxter** | 1 |
| **broken** | 1 |
| **buycarbar** | 1 |
| **career** | 1 |
| **collections** | 1 |
| **content** | 1 |
| **customisation** | 1 |
| **daily** | 1 |
| **eagle** | 1 |
| **featured** | 1 |
| **gamemodes** | 1 |
| **garage** | 1 |
| **leaderboardRow** | 1 |
| **lts** | 1 |
| **motorsports** | 1 |
| **online** | 1 |
| **party** | 1 |
| **prize** | 1 |
| **quests** | 1 |
| **racers** | 1 |
| **rental** | 1 |
| **reward** | 1 |
| **select** | 1 |
| **sesto** | 1 |
| **skippable** | 1 |
| **special** | 1 |
| **svgTest** | 1 |
| **test** | 1 |
| **ultimate** | 1 |
| **unreliable** | 1 |

### THE BIG ONES

| feature group | assets |
|---|---|
| `/gui/processed/limited_time_series` | 863 |
| `/gui/processed/quests` | 244 |
| `/gui/processed/ultimate_driver` | 80 |
| `/gui/processed/championships` | 39 |
| `/gui/processed/power` | 37 |
| `/gui/processed/motorsports` | 35 |
| `/gui/processed/reward_cards` | 33 |

## HOW TO REGENERATE

```
python .build/feature_matrix.py      # the numbers
python tools/build_feature_matrix.py  # this file
```

The inventory comes from the card's
`.depot/asset_list_base.txtCache.txt` (the guest's own list of
every asset it expects); the coverage comes from the guest log of
a real run (`20261031-132546-eden-rr3-q47-open-new-screen`).

**A FEATURE IS "OPENED" WHEN THE GUEST OPENED ONE OF ITS FILES.**
That is deliberately conservative: it says the guest RENDERED it,
not that the feature works.  A feature that opens but then fails
needs a per-feature check, and that is the next layer of this
matrix.
