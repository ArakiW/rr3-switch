# -*- coding: utf-8 -*-
"""The fixes the adversarial review found (F1-F7).

F1  the census counters were counted in logResolvedPathOnce(), which ONE
    resolver branch calls; six other branches resolved the same files
    without counting.  They are counted in resolvePath() now, the one place
    every path takes.
F2  the contract test is rewritten: it now fails on every mutant the review
    built, not on one of five.
F3  the doc's "lower bound" claim is corrected (that is a doc fix, done
    after the new measurement).
F4  the two race numbers moved to the HEAD of the census line, and the
    round census reads only the COMPLETE census lines, because the log is
    written by two threads and a line can be split.
F5  the census says where its numbers came from (port or text fallback).
F6  the census prints the frame the sample was taken at.
F7  the test's body extractor no longer counts braces inside comments and
    strings.
"""
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path):
    with io.open(path, 'rb') as handle:
        raw = handle.read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    if bom:
        raw = raw[3:]
    crlf = b'\r\n' in raw
    return raw.decode('utf-8').replace('\r\n', '\n'), crlf, bom


def save(path, text, crlf, bom):
    if crlf:
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    with io.open(path, 'wb') as handle:
        handle.write(data)


def sub(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit('FAIL %s: expected 1 occurrence, found %d' % (label, count))
    return text.replace(old, new)


# --------------------------------------------------- 1. guest_filesystem.cpp
path = os.path.join(ROOT, 'native-switch', 'source', 'guest_filesystem.cpp')
text, crlf, bom = load(path)

old = """    // THE CENSUS'S COUNTERS: how many times the guest asked for the main
    // menu's layout, and for the two-square panel inside it.  Counted BEFORE
    // the dedup below, because the dedup applies to the LOG and these count
    // EVENTS -- which is what the census meant to measure.
    if (std::strstr(path, "gui/processed/main_menu/") != nullptr) {
        __atomic_add_fetch(&g_guiLayoutOpens, 1ULL, __ATOMIC_RELAXED);
        if (std::strstr(path, "main_menu_twosquare.gui") != nullptr) {
            __atomic_add_fetch(&g_guiTwosquareOpens, 1ULL, __ATOMIC_RELAXED);
        }
    }
    // THE RACE MARKERS, COUNTED HERE -- before the thinning below and before
    // the dedup -- so the number cannot depend on what the port writes to the
    // log.  The census used to count the STRINGS in the log, and the only
    // occurrences left in it are the port's own path-trace lines: the race
    // screens are requested LATE, so after the dedup table filled up their
    // paths were never written and the count collapsed.  That is the whole
    // 27-to-2 story.  See docs/OPEN-ISSUES.md section 9.47.
    if (std::strstr(path, "gui/processed/StartLightsScreen.gui") != nullptr) {
        __atomic_add_fetch(&g_startLightsOpens, 1ULL, __ATOMIC_RELAXED);
    }
    if (std::strstr(path, "gui/processed/InGameScreen.gui") != nullptr) {
        __atomic_add_fetch(&g_raceScreenOpens, 1ULL, __ATOMIC_RELAXED);
    }
"""
new = """    // THE CENSUS'S COUNTERS ARE NOT COUNTED HERE, AND THAT IS A FIX.
    //
    // They used to be counted here, and this function is called from ONE
    // resolver branch (mapAppRootToContent()).  The guest reaches the same
    // files through six other branches -- afterPackageFiles,
    // afterPackageRootDepot, the ".depot/" prefix rule, the asset-relative
    // rule and the device-path fall-through -- and those resolved the files
    // WITHOUT counting them.  So the counter measured "requests that
    // arrived in the app-root spelling", not "requests", while the census
    // and the docs both said the second thing.  The difference is invisible
    // in the log: every branch produces the same rr3core:/files/.depot/...
    // spelling, so the trace cannot say which branch ran.
    //
    // The counters live in resolvePath() now, the ONE place every path
    // takes.  See docs/OPEN-ISSUES.md section 9.47.
"""
text = sub(text, old, new, 'guest_filesystem.cpp: counters out of the trace')

old = """    ScopedFsResolveTimer resolveTimer;
    const bool resolved = resolvePathImpl(path, writing, resolution);
    if (resolved && !writing) {
"""
new = """    ScopedFsResolveTimer resolveTimer;
    const bool resolved = resolvePathImpl(path, writing, resolution);
    // THE CENSUS'S COUNTERS, COUNTED HERE, IN THE ONE PLACE EVERY PATH TAKES.
    //
    // WHY HERE.  resolvePath() is the wrapper every caller uses --
    // resolvePathImpl() is called from nowhere else -- so this is the ONE
    // place every path takes.  The trace function is NOT: it is called from
    // one resolver branch only, and the guest reaches the same files
    // through six more that resolved them without counting.  A counter in
    // the trace therefore measured "requests in one spelling".
    //
    // WHAT IT COUNTS.  RESOLUTION EVENTS -- the guest asked for that path.
    // The counters are read by the round census instead of a regex over the
    // log's text, because the log's text is governed by the port's own
    // policy: the gui/processed/ thinning, the 2048-slot dedup, and the
    // guest-log quota of 4000 lines.  That is what made "27 start lights"
    // become "2" without the game changing.  A measurement that depends on
    // what the port writes is not a measurement.
    //
    // WHY THE RESOLVED PATH.  Every branch of the resolver ends at the same
    // device spelling (rr3core:/files/.depot/gui/processed/...), so the
    // marker is found the same way whichever branch ran.  When the
    // resolution FAILED the requested path is the only one there is.
    {
        const char* counted = resolved ? resolution.primary : path;
        if (counted != nullptr && counted[0] != '\0') {
            if (std::strstr(counted, "gui/processed/main_menu/") != nullptr) {
                __atomic_add_fetch(&g_guiLayoutOpens, 1ULL, __ATOMIC_RELAXED);
                if (std::strstr(counted, "main_menu_twosquare.gui") != nullptr) {
                    __atomic_add_fetch(&g_guiTwosquareOpens, 1ULL, __ATOMIC_RELAXED);
                }
            }
            if (std::strstr(counted, "gui/processed/StartLightsScreen.gui") != nullptr) {
                __atomic_add_fetch(&g_startLightsOpens, 1ULL, __ATOMIC_RELAXED);
            }
            if (std::strstr(counted, "gui/processed/InGameScreen.gui") != nullptr) {
                __atomic_add_fetch(&g_raceScreenOpens, 1ULL, __ATOMIC_RELAXED);
            }
        }
    }
    if (resolved && !writing) {
"""
text = sub(text, old, new, 'guest_filesystem.cpp: counters into resolvePath')
save(path, text, crlf, bom)
print('OK guest_filesystem.cpp')

# ---------------------------------------------------- 2. guest_view_probe.cpp
path = os.path.join(ROOT, 'native-switch', 'source', 'guest_view_probe.cpp')
text, crlf, bom = load(path)

old = """                    "frame=%llu census bound=%d viewport=%d,%d,%d,%d"
"""
new = """                    "frame=%llu lights=%llu race=%llu census bound=%d viewport=%d,%d,%d,%d"
"""
text = sub(text, old, new, 'guest_view_probe.cpp: census format head')

old = """                    " gui=%llu twosq=%llu lights=%llu race=%llu",
                    static_cast<unsigned long long>(frames),
                    static_cast<int>(g_hdBoundAfterRender),
"""
new = """                    " gui=%llu twosq=%llu",
                    static_cast<unsigned long long>(frames),
                    // THE TWO RACE NUMBERS COME FIRST, ON PURPOSE.  The log
                    // file is written by two threads into one file, so a
                    // line can be SPLIT: the head keeps its "phase=..."
                    // prefix and the tail loses it.  MEASURED: exactly one
                    // spliced census line per round, in every archive.  The
                    // fields at the END of a line are the ones a split can
                    // separate from their line, so the two numbers the
                    // census reads are at the HEAD, where a split cannot
                    // take them away.
                    static_cast<unsigned long long>(rr3::guestStartLightsOpens()),
                    static_cast<unsigned long long>(rr3::guestRaceScreenOpens()),
                    static_cast<int>(g_hdBoundAfterRender),
"""
text = sub(text, old, new, 'guest_view_probe.cpp: census argument order')

old = """                    static_cast<unsigned long long>(rr3::guestGuiLayoutOpens()),
                    static_cast<unsigned long long>(rr3::guestGuiTwosquareOpens()),
                    static_cast<unsigned long long>(rr3::guestStartLightsOpens()),
                    static_cast<unsigned long long>(rr3::guestRaceScreenOpens()));
"""
new = """                    static_cast<unsigned long long>(rr3::guestGuiLayoutOpens()),
                    static_cast<unsigned long long>(rr3::guestGuiTwosquareOpens()));
"""
text = sub(text, old, new, 'guest_view_probe.cpp: census tail arguments')
save(path, text, crlf, bom)
print('OK guest_view_probe.cpp')

# ------------------------------------------------------ 3. tools/Test-OnEden.ps1
path = os.path.join(ROOT, 'tools', 'Test-OnEden.ps1')
text, crlf, bom = load(path)

old = """    $portGuiOpens = [regex]::Matches($guestText, ' gui=(\d+) ')
    $portTwosquare = [regex]::Matches($guestText, ' twosq=(\d+)(?:\s|$)')
    if ($portGuiOpens.Count -gt 0 -and $portTwosquare.Count -gt 0) {
        # The LAST value is the final count: the counter grows.
        $summary.msMainMenuLayouts = [int]$portGuiOpens[$portGuiOpens.Count - 1].Groups[1].Value
        $summary.msTwosquare = [int]$portTwosquare[$portTwosquare.Count - 1].Groups[1].Value
    }
    else {
        # An older log, without the counters: fall back to the text.
        $summary.msTwosquare = [regex]::Matches(
            $guestText, 'gui/processed/main_menu/main_menu_twosquare\.gui').Count
        $summary.msMainMenuLayouts = [regex]::Matches(
            $guestText, 'gui/processed/main_menu/').Count
    }
"""
new = """    # ONLY THE CENSUS'S OWN COMPLETE LINES.  The log file is written by two
    # threads into one file, so a line can be SPLIT: the head keeps its
    # "phase=..." prefix and the tail loses it.  MEASURED: exactly one
    # spliced census line per round, in every archive.  A regex over the
    # whole text would happily read the fields of a spliced TAIL -- which
    # belongs to no line at all -- so the census takes the COMPLETE lines
    # only: the ones that begin with its own phase.
    $censusLines = [regex]::Matches($guestText, '(?m)^phase=guest-frame-census[^\r\n]*')
    $censusText = ''
    if ($censusLines.Count -gt 0) {
        $censusText = [string]::Join("\n", ($censusLines | ForEach-Object { $_.Value }))
    }
    $portGuiOpens = [regex]::Matches($censusText, ' gui=(\d+)(?: |\r?$)')
    $portTwosquare = [regex]::Matches($censusText, ' twosq=(\d+)(?: |\r?$)')
    if ($portGuiOpens.Count -gt 0 -and $portTwosquare.Count -gt 0) {
        # The LAST value is the final count: the counter grows.
        $summary.msMainMenuLayouts = [int]$portGuiOpens[$portGuiOpens.Count - 1].Groups[1].Value
        $summary.msTwosquare = [int]$portTwosquare[$portTwosquare.Count - 1].Groups[1].Value
        $summary.msFrontEndSource = 'port'
    }
    else {
        # An older log, without the counters: fall back to the text.  The
        # summary says WHICH it used: the text count is the log-policy
        # number that section 9.47 exists to retire, and it must never be
        # printed under the port's label.
        $summary.msTwosquare = [regex]::Matches(
            $guestText, 'gui/processed/main_menu/main_menu_twosquare\.gui').Count
        $summary.msMainMenuLayouts = [regex]::Matches(
            $guestText, 'gui/processed/main_menu/').Count
        $summary.msFrontEndSource = 'text'
    }
"""
text = sub(text, old, new, 'Test-OnEden.ps1: census lines only')

old = """    $portStartLights = [regex]::Matches($guestText, ' lights=(\d+)(?:\s|$)')
    $portRaceScreen = [regex]::Matches($guestText, ' race=(\d+)(?:\s|$)')
    $summary.msStartLightsText = [regex]::Matches($guestText, 'StartLightsScreen').Count
    $summary.msRaceScreenText = [regex]::Matches($guestText, 'InGameScreen').Count
    if ($portStartLights.Count -gt 0 -and $portRaceScreen.Count -gt 0) {
        # The LAST value is the final count: the counter grows.
        $summary.msStartLights = [int]$portStartLights[$portStartLights.Count - 1].Groups[1].Value
        $summary.msRaceScreen = [int]$portRaceScreen[$portRaceScreen.Count - 1].Groups[1].Value
    }
    else {
        # An older log, without the counters: fall back to the text.
        $summary.msStartLights = $summary.msStartLightsText
        $summary.msRaceScreen = $summary.msRaceScreenText
    }
"""
new = """    $portStartLights = [regex]::Matches($censusText, ' lights=(\d+)(?: |\r?$)')
    $portRaceScreen = [regex]::Matches($censusText, ' race=(\d+)(?: |\r?$)')
    $summary.msStartLightsText = [regex]::Matches($guestText, 'StartLightsScreen').Count
    $summary.msRaceScreenText = [regex]::Matches($guestText, 'InGameScreen').Count
    if ($portStartLights.Count -gt 0 -and $portRaceScreen.Count -gt 0) {
        # The LAST value is the final count: the counter grows.
        $summary.msStartLights = [int]$portStartLights[$portStartLights.Count - 1].Groups[1].Value
        $summary.msRaceScreen = [int]$portRaceScreen[$portRaceScreen.Count - 1].Groups[1].Value
        $summary.msRaceMarkerSource = 'port'
        # WHEN the sample was taken.  The census line is written every 60
        # frames, so the counters are a SAMPLE: the guest's threads keep
        # resolving after the last frame, and the log can also lose its last
        # lines when the run is cut short.  Printing the frame the sample
        # was taken at makes the age of the number visible instead of
        # implied.
        $lastCensus = $censusLines[$censusLines.Count - 1].Value
        $censusFrame = [regex]::Match($lastCensus, ' frame=(\d+)')
        if ($censusFrame.Success) {
            $summary.msCensusFrame = [int]$censusFrame.Groups[1].Value
        }
    }
    else {
        # An older log, without the counters: fall back to the text, and
        # LABEL it as the text count.  The text count is the log-policy
        # number that section 9.47 exists to retire; printing it under the
        # port's label is how it would have gone on looking healthy.
        $summary.msStartLights = $summary.msStartLightsText
        $summary.msRaceScreen = $summary.msRaceScreenText
        $summary.msRaceMarkerSource = 'text'
    }
"""
text = sub(text, old, new, 'Test-OnEden.ps1: race markers from census lines')

old = """    msStartLights       = 0   # the race start sequence, counted by the PORT
    msRaceScreen        = 0   # the race itself, counted by the PORT
    msStartLightsText   = 0   # the same, counted in the log's text (cross-check)
    msRaceScreenText    = 0   # the same, counted in the log's text (cross-check)
"""
new = """    msStartLights       = 0   # the race start sequence
    msRaceScreen        = 0   # the race itself
    msStartLightsText   = 0   # the same, counted in the log's text (cross-check)
    msRaceScreenText    = 0   # the same, counted in the log's text (cross-check)
    # WHERE EACH NUMBER CAME FROM.  "port" is the port's own counter; "text"
    # is the old regex over the log, which measures the LOG'S POLICY and is
    # only used for an archive written by an older binary.  The summary
    # prints it, so a number can never wear the wrong label.
    msRaceMarkerSource  = 'none'
    msFrontEndSource    = 'none'
    # The frame the census sampled the counters at: the counters grow, and
    # the last line in the log is a SAMPLE of them, not the end of the run.
    msCensusFrame       = 0
"""
text = sub(text, old, new, 'Test-OnEden.ps1: summary fields')

old = """    ('  front-end layout opened  {0}   ("gui/processed/main_menu/main_menu_twosquare.gui" in the PORT trace -- want > 0 for the main menu panel)' -f $summary.msTwosquare)
    ('  main menu layouts open   {0}   (gui/processed/main_menu/ -- want > 0 once the front end is up)' -f $summary.msMainMenuLayouts)
"""
new = """    ('  front-end layout opened  {0}   (the guest asked for the two-square panel; counted by the {1} -- want > 0 for the main menu panel)' -f $summary.msTwosquare, $summary.msFrontEndSource)
    ('  main menu layouts open   {0}   (the guest asked for a main-menu layout; counted by the {1} -- want > 0 once the front end is up)' -f $summary.msMainMenuLayouts, $summary.msFrontEndSource)
"""
text = sub(text, old, new, 'Test-OnEden.ps1: front-end print labels')

old = """    ('  start lights             {0}   (the race start sequence, counted by the PORT)' -f $summary.msStartLights)
    ('  race screen              {0}   (the race itself, counted by the PORT; want > 0 to be IN a race)' -f $summary.msRaceScreen)
"""
new = """    ('  start lights             {0}   (the race start sequence, counted by the {1})' -f $summary.msStartLights, $summary.msRaceMarkerSource)
    ('  race screen              {0}   (the race itself, counted by the {1}; want > 0 to be IN a race)' -f $summary.msRaceScreen, $summary.msRaceMarkerSource)
    ('  census sample frame      {0}   (the frame the counters were sampled at; the guest keeps working after the last frame)' -f $summary.msCensusFrame)
"""
text = sub(text, old, new, 'Test-OnEden.ps1: race marker print labels')
save(path, text, crlf, bom)
print('OK tools/Test-OnEden.ps1')
