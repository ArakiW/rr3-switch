"""Rewrite every guest-visible `errno = EXXX;` in the shim layer to the bionic
numbering defined in bionic_errno.hpp.

Why a script and not 50 hand edits: the whole point of the fix is that the
NEWNLIB MACRO is the wrong number, so a reviewer has to be able to check the
result mechanically -- "no assignment still uses a bare newlib errno macro" is
a grep, whereas "all 50 were changed correctly" is not. The script also prints
a before/after count so a silent no-op is impossible to miss.

Skips: the frozen binding table (symbol_resolver.cpp is byte-locked by guard),
and any file that does not already include <errno.h> style shims.
"""
import re
from pathlib import Path

SRC = Path(r"E:\AI\RealRacing3-switch\native-switch\source")
# symbol_resolver.cpp is guarded byte-for-byte; never touch it.
FROZEN = {"symbol_resolver.cpp"}

NAMES = {
    "EACCES": "kGuestErrnoEACCES", "EAGAIN": "kGuestErrnoEAGAIN",
    "EBUSY": "kGuestErrnoEBUSY", "ECONNREFUSED": "kGuestErrnoECONNREFUSED",
    "EINVAL": "kGuestErrnoEINVAL", "EIO": "kGuestErrnoEIO",
    "ENAMETOOLONG": "kGuestErrnoENAMETOOLONG", "ENODEV": "kGuestErrnoENODEV",
    "ENOENT": "kGuestErrnoENOENT", "ENOMEM": "kGuestErrnoENOMEM",
    "ENOSPC": "kGuestErrnoENOSPC", "ENOSYS": "kGuestErrnoENOSYS",
    "ENOTSUP": "kGuestErrnoENOTSUP", "EROFS": "kGuestErrnoEROFS",
    "ETIMEDOUT": "kGuestErrnoETIMEDOUT", "EDOM": "kGuestErrnoEDOM",
    "ERANGE": "kGuestErrnoERANGE", "ELOOP": "kGuestErrnoELOOP",
    "ENOTEMPTY": "kGuestErrnoENOTEMPTY", "EPIPE": "kGuestErrnoEPIPE",
    "ECONNRESET": "kGuestErrnoECONNRESET", "EHOSTUNREACH": "kGuestErrnoEHOSTUNREACH",
    "ENETUNREACH": "kGuestErrnoENETUNREACH", "EADDRINUSE": "kGuestErrnoEADDRINUSE",
    "EBADF": "kGuestErrnoEBADF", "EINTR": "kGuestErrnoEINTR", "EFAULT": "kGuestErrnoEFAULT",
    "EEXIST": "kGuestErrnoEEXIST", "EXDEV": "kGuestErrnoEXDEV",
    "ENOTDIR": "kGuestErrnoENOTDIR", "EISDIR": "kGuestErrnoEISDIR",
    "EPERM": "kGuestErrnoEPERM", "ESRCH": "kGuestErrnoESRCH",
    "ENXIO": "kGuestErrnoENXIO", "E2BIG": "kGuestErrnoE2BIG",
    "ENOEXEC": "kGuestErrnoENOEXEC", "ECHILD": "kGuestErrnoECHILD",
    "ENOTBLK": "kGuestErrnoENOTBLK", "ENFILE": "kGuestErrnoENFILE",
    "EMFILE": "kGuestErrnoEMFILE", "ENOTTY": "kGuestErrnoENOTTY",
    "ETXTBSY": "kGuestErrnoETXTBSY", "EFBIG": "kGuestErrnoEFBIG",
    "ESPIPE": "kGuestErrnoESPIPE", "EMLINK": "kGuestErrnoEMLINK",
}

ASSIGN = re.compile(r"(?<![\w.>])errno\s*=\s*(E[A-Z0-9_]+)\s*;")
INCLUDE = re.compile(r'#include\s+"([^"]+)"')


def rel(h):
    p = Path(h)
    try:
        return p.relative_to(SRC).as_posix()
    except ValueError:
        return None


total = 0
for f in sorted(SRC.rglob("*.cpp")):
    if f.name in FROZEN:
        continue
    text = f.read_text(encoding="utf-8")
    if not ASSIGN.search(text):
        continue
    hits = ASSIGN.findall(text)

    def sub(m):
        name = m.group(1)
        if name not in NAMES:
            print("  !! unmapped errno macro %s in %s -- left alone" % (name, f.name))
            return m.group(0)
        return "errno = %s;" % NAMES[name]

    new = ASSIGN.sub(sub, text)
    left = ASSIGN.findall(new)
    if left:
        print("  !! %s still has %s" % (f.name, left))

    # add the include right after the first project include, else at the top
    if "bionic_errno.hpp" not in new:
        m = INCLUDE.search(new)
        if m and rel(m.group(1)) is not None:
            ins = m.start()
            new = new[:ins] + '#include "%s"\n' % rel(m.group(1)) + new[ins:]
        else:
            # no project include to anchor to: put it after the last #include
            incs = list(INCLUDE.finditer(new))
            if incs:
                at = incs[-1].end()
                new = new[:at] + '\n#include "bionic_errno.hpp"' + new[at:]
            else:
                new = '#include "bionic_errno.hpp"\n' + new
    f.write_text(new, encoding="utf-8")
    print("  %-28s %d assignments" % (f.relative_to(SRC).as_posix(), len(hits)))
    total += len(hits)

print()
print("rewrote %d errno assignments across the shim layer" % total)
