"""The guest's string-to-number imports must NOT reach the host's parser.

The host's strtoul scans until it meets a non-digit, and on the guest's
thread that scan is unbounded: a guest pointer to an unterminated string
walks the guest's address space, the port burns a CPU, and nothing is
logged.  Measured 2026-11-01 (round q89): the guest's root thread was
sampled inside the host image at pc_off=0x5c6154, which is the link-time
vaddr of `_strtoul_l.isra.0`, and the run hung for 120 s with the log
frozen.

So the contract is:
  * every strto*/ato* import is a SHIM, not a BIND;
  * the shim parses a COPY, never the guest's own pointer;
  * the copy is bounded, and the bound is a constant in the source;
  * the copy is read through a readability check, not by blind dereference;
  * a refusal is logged, so the failure is a named event and not a hang.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    resolver = (ROOT / "source" / "symbol_resolver.cpp").read_text(encoding="utf-8")
    shim = (ROOT / "source" / "guest_strto.cpp").read_text(encoding="utf-8")
    header = (ROOT / "include" / "rr3" / "guest_strto.hpp").read_text(encoding="utf-8")

    # 1. The imports are shims.  A BIND here is the bug this file exists for.
    for name, fn in (
        ("strtol", "guestStrtol"),
        ("strtoll", "guestStrtoll"),
        ("strtoul", "guestStrtoul"),
        ("strtoull", "guestStrtoull"),
        ("strtod", "guestStrtod"),
        ("strtof", "guestStrtof"),
        ("atoi", "guestAtoi"),
        ("atol", "guestAtol"),
        ("atof", "guestAtof"),
    ):
        assert 'RR3_SHIM("%s", %s)' % (name, fn) in resolver, name
        assert "RR3_BIND(%s)" % name not in resolver, name

    # 2. The parser never sees the guest's pointer.
    for parser in ("strtol", "strtoll", "strtoul", "strtoull", "strtod", "strtof"):
        assert "std::%s(copy.buffer" % parser in shim, parser
        assert "std::%s(text" % parser not in shim, parser

    # 3. The bound is a constant, and it is small.
    assert "constexpr size_t kMaxScan = 512;" in shim
    assert "for (size_t i = 0; i < kMaxScan; ++i)" in shim

    # 3b. THE REGRESSION GUARD: A BOUND ON THE SCAN MUST NOT CHANGE THE
    #     ANSWER.  The first version refused every string that was longer
    #     than the window and answered 0; the guest parses long dotted
    #     lists, so the guest got 0 where it expected a number and the
    #     race's metrics came back empty (the car's pointer 0, the lap
    #     distance -1).  A truncated string is LEGAL: the copy parses to
    #     the same value the host's parser would have returned, and THAT
    #     value is returned.
    assert "TextState::Truncated" in shim
    assert "TextState::Unreadable" in shim
    for name in ("strtol", "strtoll", "strtoul", "strtoull", "strtod", "strtof"):
        block = shim[shim.index("guest%s(" % name.capitalize()):]
        block = block[:block.index("\n}\n")]
        # the truncation branch must NOT return: it falls through to the parse
        truncated = block[block.index("TextState::Truncated"):]
        truncated = truncated.split("char* stop")[0]
        # the comments may talk about "returned"; the CODE may not return.
        code = "\n".join(line.split("//")[0] for line in truncated.split("\n"))
        assert "return" not in code, name
        # and the parse must run on the COPY
        assert ".buffer" in block, name

    # 4. The guest's memory is read through a readability check.
    assert "svcQueryMemory" in shim
    assert "Perm_R" in shim
    assert "pointerPlausible" in shim

    # 5. A refusal is a NAMED EVENT, not a silent zero.
    assert "guest-strto-refused" in shim
    assert "rr3::appendGuestBootLine" in shim

    # 6. The header declares what the resolver binds to.
    for fn in ("guestStrtol", "guestStrtoul", "guestStrtod", "guestAtoi",
               "guestAtof"):
        assert fn in header, fn

    print("guest_strto contract: 9 imports shimmed, copy bounded to 512 bytes, "
          "a truncated string still answers with the copy's value")


if __name__ == "__main__":
    main()
