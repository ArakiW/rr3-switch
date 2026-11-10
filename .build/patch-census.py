import io
p = r"E:\AI\RealRacing3-switch\tools\Test-OnEden.ps1"
text = io.open(p, encoding="utf-8", errors="surrogateescape").read()
old = """    $summary.msTwosquare = [regex]::Matches(
        $guestText, 'gui/processed/main_menu/main_menu_twosquare\\.gui').Count
    $summary.msMainMenuLayouts = [regex]::Matches(
        $guestText, 'gui/processed/main_menu/').Count"""
new = """    # THE CENSUS READS THE PORT'S OWN COUNTERS NOW, NOT THE LOG'S TEXT.
    #
    # WHY: the counter used to be a REGEX over the guest-view log, which made
    # the measurement a function of what the port writes.  MEASURED 2026-11-10
    # (round slim1): thinning the path trace to the census's own subtree cut
    # the census with it -- 'front-end layout opened 1' and 'main menu layouts
    # open 5' against yield2's 15 and 75 -- while the game was visibly fine.
    # See docs/OPEN-ISSUES.md section 9.37.
    #
    # The counter is of RESOLUTION EVENTS (the guest asked for the layout),
    # which is what this census always meant to measure; the log's text counted
    # lines, which the dedup and the trace's thinness both change.
    $portGuiOpens = [regex]::Matches($guestText, ' gui=(\\d+) ')
    $portTwosquare = [regex]::Matches($guestText, ' twosq=(\\d+) ')
    if ($portGuiOpens.Count -gt 0 -and $portTwosquare.Count -gt 0) {
        # The LAST value is the final count: the counter grows.
        $summary.msMainMenuLayouts = [int]$portGuiOpens[$portGuiOpens.Count - 1].Groups[1].Value
        $summary.msTwosquare = [int]$portTwosquare[$portTwosquare.Count - 1].Groups[1].Value
    }
    else {
        # An older log, without the counters: fall back to the text.
        $summary.msTwosquare = [regex]::Matches(
            $guestText, 'gui/processed/main_menu/main_menu_twosquare\\.gui').Count
        $summary.msMainMenuLayouts = [regex]::Matches(
            $guestText, 'gui/processed/main_menu/').Count
    }"""
if old in text:
    text = text.replace(old, new)
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)
    print("census patched")
else:
    print("PATTERN NOT FOUND")