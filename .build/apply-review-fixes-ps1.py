# -*- coding: utf-8 -*-
"""The PowerShell half of the review fixes (F4, F5, F6)."""
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


path = os.path.join(ROOT, 'tools', 'Test-OnEden.ps1')
text, crlf, bom = load(path)

old = r"""    $portGuiOpens = [regex]::Matches($guestText, ' gui=(\d+) ')
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
new = r"""    # ONLY THE CENSUS'S OWN COMPLETE LINES.  The log file is written by two
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

old = r"""    $portStartLights = [regex]::Matches($guestText, ' lights=(\d+)(?:\s|$)')
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
new = r"""    $portStartLights = [regex]::Matches($censusText, ' lights=(\d+)(?: |\r?$)')
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

old = """    msStartLights       = 0   # the race's start sequence, counted by the PORT
    msRaceScreen        = 0   # the race itself, counted by the PORT
    msStartLightsText   = 0   # the same, counted in the log's text (cross-check)
    msRaceScreenText    = 0   # the same, counted in the log's text (cross-check)
"""
new = """    msStartLights       = 0   # the race's start sequence
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
new = """    ('  start lights             {0}   (the race's start sequence, counted by the {1})' -f $summary.msStartLights, $summary.msRaceMarkerSource)
    ('  race screen              {0}   (the race itself, counted by the {1}; want > 0 to be IN a race)' -f $summary.msRaceScreen, $summary.msRaceMarkerSource)
    ('  census sample frame      {0}   (the frame the counters were sampled at; the guest keeps working after the last frame)' -f $summary.msCensusFrame)
"""
text = sub(text, old, new, 'Test-OnEden.ps1: race marker print labels')
save(path, text, crlf, bom)
print('OK tools/Test-OnEden.ps1')
