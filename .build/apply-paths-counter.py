# -*- coding: utf-8 -*-
"""Move the resolution counter to the resolver too (the same defect, F1)."""
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


path = os.path.join(ROOT, 'native-switch', 'source', 'guest_filesystem.cpp')
text, crlf, bom = load(path)

old = """    // THE NUMBER OF RESOLUTIONS, so the information survives even when the
    // trace is thinned one day.  It is on the race-state line as `paths=`.
    __atomic_add_fetch(&g_resolvedPathCount, 1ULL, __ATOMIC_RELAXED);
"""
new = """    // THE NUMBER OF RESOLUTIONS IS NOT COUNTED HERE EITHER, FOR THE SAME
    // REASON THE CENSUS'S COUNTERS ARE NOT: this function is called from ONE
    // resolver branch, so the number here was the number of resolutions
    // that arrived in the app-root spelling.  It is counted in
    // resolvePath() now, and it is still on the race-state line as
    // `paths=`.
"""
text = sub(text, old, new, 'guest_filesystem.cpp: paths counter out of the trace')

old = """    {
        const char* counted = resolved ? resolution.primary : path;
        if (counted != nullptr && counted[0] != '\0') {
"""
new = """    if (path != nullptr && path[0] != '\0') {
        // THE NUMBER OF RESOLUTIONS, so the information survives even when
        // the trace is thinned one day.  It is on the race-state line as
        // `paths=`.  Counted here for the same reason as the census's
        // counters: this is the one place every path takes.
        __atomic_add_fetch(&g_resolvedPathCount, 1ULL, __ATOMIC_RELAXED);
    }
    {
        const char* counted = resolved ? resolution.primary : path;
        if (counted != nullptr && counted[0] != '\0') {
"""
text = sub(text, old, new, 'guest_filesystem.cpp: paths counter into resolvePath')
save(path, text, crlf, bom)
print('OK guest_filesystem.cpp')
