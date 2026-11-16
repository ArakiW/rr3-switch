# -*- coding: utf-8 -*-
"""A MISSING MEASUREMENT MUST NOT READ AS A ZERO MEASUREMENT.

MEASURED 2026-11-16 (round p1real): the guest hung before the frame loop,
so there was NO census line at all -- and the summary printed "PCM enqueued
0 enqueues / 0 bytes" and "FMOD in use 0", because those fields default to
zero.  **=> THE SUMMARY SAID "MEASURED ZERO" FOR A MEASUREMENT THAT NEVER
HAPPENED.**  That is the same shape as the "audible is always 0" problem:
a zero that is not a measurement.

So the census now reports whether it ran, and the P1 lines say so.
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


path = os.path.join(ROOT, 'tools', 'Test-OnEden.ps1')
text, crlf, bom = load(path)

old = """    msFmodReal          = 0
"""
new = """    msFmodReal          = 'none'
    # WHETHER THE CENSUS RAN AT ALL.  Without it there is no measurement,
    # and a measurement that did not happen must not print as a zero.
    msCensusSeen        = $false
    msCensusLines       = 0
"""
text = sub(text, old, new, 'ps1: the census-seen flag')

old = """    if ($pcmEnq.Count -gt 0) {
        $summary.msPcmEnqueues = [int]$pcmEnq[$pcmEnq.Count - 1].Groups[1].Value
        $summary.msPcmBytes = [int]$pcmBytes[$pcmBytes.Count - 1].Groups[1].Value
        $last = $pcmFormat[$pcmFormat.Count - 1]
        $summary.msPcmFormat = '{0} Hz / {1} ch / {2} bit' -f $last.Groups[1].Value, $last.Groups[2].Value, $last.Groups[3].Value
        $summary.msFmodReal = [int]$fmodReal[$fmodReal.Count - 1].Groups[1].Value
    }
"""
new = """    $summary.msCensusLines = $censusLines.Count
    if ($censusLines.Count -gt 0) {
        $summary.msCensusSeen = $true
    }
    if ($pcmEnq.Count -gt 0) {
        $summary.msPcmEnqueues = [int]$pcmEnq[$pcmEnq.Count - 1].Groups[1].Value
        $summary.msPcmBytes = [int]$pcmBytes[$pcmBytes.Count - 1].Groups[1].Value
        $last = $pcmFormat[$pcmFormat.Count - 1]
        $summary.msPcmFormat = '{0} Hz / {1} ch / {2} bit' -f $last.Groups[1].Value, $last.Groups[2].Value, $last.Groups[3].Value
        $summary.msFmodReal = [int]$fmodReal[$fmodReal.Count - 1].Groups[1].Value
    }
"""
text = sub(text, old, new, 'ps1: the census-seen flag is set')

old = """    ('  PCM enqueued            {0} enqueues / {1} bytes   (what the guest handed the audio device)' -f $summary.msPcmEnqueues, $summary.msPcmBytes)
    ('  PCM format asked         {0}   (the guest own SLDataFormat)' -f $summary.msPcmFormat)
    ('  FMOD in use              {0}   (0 = the port own shim, 1 = the guest own libfmodex.so)' -f $summary.msFmodReal)
"""
new = """    ('  census lines             {0}   (the port own census; 0 means the guest never reached the frame loop, so there is NO measurement)' -f $summary.msCensusLines)
    ('  PCM enqueued            {0}   (what the guest handed the audio device)' -f $(if ($summary.msCensusSeen) { '{0} enqueues / {1} bytes' -f $summary.msPcmEnqueues, $summary.msPcmBytes } else { 'NOT MEASURED (no census line)' }))
    ('  PCM format asked         {0}   (the guest own SLDataFormat)' -f $(if ($summary.msCensusSeen) { $summary.msPcmFormat } else { 'NOT MEASURED (no census line)' }))
    ('  FMOD in use              {0}   (0 = the port own shim, 1 = the guest own libfmodex.so)' -f $(if ($summary.msCensusSeen) { $summary.msFmodReal } else { 'NOT MEASURED (no census line)' }))
"""
text = sub(text, old, new, 'ps1: the P1 print labels')
save(path, text, crlf, bom)
print('OK tools/Test-OnEden.ps1')
