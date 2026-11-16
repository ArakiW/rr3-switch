# -*- coding: utf-8 -*-
"""Fix the apostrophe that broke the PowerShell parser."""
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
path = os.path.join(ROOT, 'tools', 'Test-OnEden.ps1')

with io.open(path, 'rb') as handle:
    raw = handle.read()
bom = raw.startswith(b'\xef\xbb\xbf')
if bom:
    raw = raw[3:]
crlf = b'\r\n' in raw
text = raw.decode('utf-8').replace('\r\n', '\n')

old = """    ('  start lights             {0}   (the race's start sequence, counted by the {1})' -f $summary.msStartLights, $summary.msRaceMarkerSource)"""
new = """    ('  start lights             {0}   (the race start sequence, counted by the {1})' -f $summary.msStartLights, $summary.msRaceMarkerSource)"""
if text.count(old) != 1:
    raise SystemExit('FAIL: anchor not found exactly once')
text = text.replace(old, new)

if crlf:
    text = text.replace('\n', '\r\n')
data = text.encode('utf-8')
if bom:
    data = b'\xef\xbb\xbf' + data
with io.open(path, 'wb') as handle:
    handle.write(data)
print('OK Test-OnEden.ps1')
