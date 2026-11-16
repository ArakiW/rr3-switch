#!/usr/bin/env python3
"""The real-FMOD path: the port watches FMOD_System_Create, and the guest's
pthread attribute registry accepts a re-initialised attribute.

WHY THIS IS A CONTRACT AND NOT A COMMENT:

On 2026-11-16 the "fmod=real" Eden round ended in an endless walk over a
NULL object.  The first fault was a read at 0x50 -- the real library's
"ldr x0, [pool+0x50]" with pool == 0 -- then the whole FMOD System was
CONSTRUCTED AT ADDRESS 0, then null+0x8 and null+0x1C0 alternated 516,305
times each.  Read backwards from that log, the guest's FMOD_System_Create
returned a NULL handle, because FMOD's own memory source could not be
created.

TWO THINGS HAVE TO HOLD FOR THAT TO BE FIXABLE AND VISIBLE:

  1. THE PORT MUST SEE THE CALL.  The observer is a PASS-THROUGH to the
     guest's own libfmodex.so, not a second implementation, and it returns
     the real library's result untouched.  If it ever called the port's own
     shim, the guest would receive the port's FMOD object and the
     mixed-layout failure would come back through the back door.

  2. THE GUEST'S ATTRIBUTE REGISTRY MUST ACCEPT A RE-INITIALISED
     ATTRIBUTE.  The guest's own libfmodex.so keeps its pthread_mutexattr_t
     ON THE STACK, never destroys it, and initialises a new one from the
     same stack slot for its next memory source.  bionic and glibc both
     allow that; returning EBUSY made FMOD's memory source creation fail.

THE ASSERTIONS READ THE CODE, NOT THE PROSE: comments and string literals
are masked out before anything is inspected, so a comment that says "return
EBUSY" can neither satisfy nor break an assertion.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PTHREAD = ROOT / "source" / "pthread_bridge.cpp"
RESOLVER = ROOT / "source" / "symbol_resolver.cpp"


def mask_code(text):
    """Blank out comments and string literals, keeping the code."""
    out = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end < 0 else end
            out.append(" " * (end - index))
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[index:end]))
            index = end
        elif char in "\"'":
            quote = char
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == quote:
                    index += 1
                    break
                index += 1
            out.append(" ")
        else:
            out.append(char)
            index += 1
    return "".join(out)


def strip_comments(text):
    """Remove comments but KEEP the string literals.

    Used where the contract is about a literal (the symbol name that selects
    the observed entry point): a literal that only exists inside a comment
    must not count.
    """
    out = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end < 0 else end
            out.append(" " * (end - index))
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[index:end]))
            index = end
        elif char in "\"'":
            quote = char
            start = index
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == quote:
                    index += 1
                    break
                index += 1
            out.append(text[start:index])
        else:
            out.append(char)
            index += 1
    return "".join(out)


def balanced_body(text, open_index):
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[open_index + 1:index]
    raise AssertionError("unbalanced braces")


def definition_body(text, signature):
    """The body of a function DEFINITION whose name starts with signature."""
    pattern = re.compile(re.escape(signature) + r"[^;{]*\{")
    match = pattern.search(text)
    assert match, "no definition found for " + signature
    return balanced_body(text, match.end() - 1)


def main():
    pthread_text = PTHREAD.read_text(encoding="utf-8")
    resolver_text = RESOLVER.read_text(encoding="utf-8")
    masked_pthread = mask_code(pthread_text)
    masked_resolver = mask_code(resolver_text)

    # ---- 1. a re-initialised attribute is accepted, not refused ----------
    attr = definition_body(masked_pthread, "int guestPthreadMutexAttrInit")
    assert "EBUSY" not in attr, (
        "guestPthreadMutexAttrInit refuses a re-initialised attribute with "
        "EBUSY; the guest's own libfmodex.so re-uses its stack slot and "
        "never destroys the attribute, so this is the failure that stops "
        "FMOD from creating its memory source")
    slot = attr.find("findSlot(gMutexAttributes")
    assert slot >= 0, "the attribute registry lookup is gone"
    first_return = re.search(r"return\s+([^;]+);", attr[slot:])
    assert first_return, "the attribute registry lookup leads nowhere"
    assert first_return.group(1).strip() == "0", (
        "the re-initialised attribute must return 0, not %r"
        % first_return.group(1).strip())

    # ---- 2. the refusal that IS kept is named, not just counted ---------
    mutex = definition_body(masked_pthread, "int guestPthreadMutexInit")
    assert "EBUSY" in mutex, (
        "guestPthreadMutexInit no longer refuses a second initialisation of "
        "the same mutex; that refusal is a deliberate deviation and it has "
        "to stay")
    busy = mutex.find("EBUSY")
    region = mutex[max(0, busy - 800):busy + 200]
    assert "pthreadBridgeDiag" in region, (
        "the EBUSY path of guestPthreadMutexInit is not named in the log, so "
        "a round cannot tell it apart from any other failure")

    # ---- 3. the observer is a pass-through to the guest's own library ---
    observer = definition_body(masked_resolver, "int fmodRealSystemCreateObserved")
    assert "findLoaderExportBySoname" in observer, (
        "the observer must reach the guest's own libfmodex.so through the "
        "loader, not through a hard-coded address")
    assert "FMOD_System_Create" not in observer, (
        "the observer calls the port's own FMOD_System_Create shim; the "
        "guest would then get the port's FMOD object and the mixed-layout "
        "failure would come back through the back door")
    # THE RETURNED VALUE MUST BE THE ONE THE REAL LIBRARY PRODUCED.
    #
    # "the observer returns something called result" is NOT that property: a
    # version that calls the real library and throws the answer away still
    # returns it.  So the assignment has to be there, and the returned name
    # has to be the name that was assigned.
    calls = re.findall(r"(\w+)\s*=\s*(\w+)\s*\(\s*system\s*\)", observer)
    assert calls, (
        "the observer never stores the real library's return value, so it "
        "cannot pass it on")
    returned = re.search(r"return\s+(\w+)\s*;", observer)
    assert returned, "the observer returns nothing"
    assert returned.group(1) in [entry[0] for entry in calls], (
        "the observer returns %r instead of what the real library returned"
        % returned.group(1))
    assert "appendGuestBootLine" in observer, (
        "the observer must write its finding to the round's log")

    # ---- 4. the wiring selects exactly the one observed entry point -----
    resolver = definition_body(masked_resolver, "uintptr_t resolveHostSymbol")
    assert "fmodRealSystemCreateObserved" in resolver, (
        "resolveHostSymbol does not route the observed entry point to the "
        "observer")
    assert "return 0;" in resolver, (
        "resolveHostSymbol must still withhold every OTHER FMOD symbol, so "
        "the guest's own library provides them")
    literal_resolver = definition_body(strip_comments(resolver_text),
                                       "uintptr_t resolveHostSymbol")
    assert '"FMOD_System_Create"' in literal_resolver, (
        "the observed entry point is not selected by its name; a literal "
        "that only exists in a comment does not count")
    assert "fmod-real-create" in definition_body(strip_comments(resolver_text),
                                                  "int fmodRealSystemCreateObserved"), (
        "the observer does not write the finding the round looks for")

    print("PASS: the real-FMOD observer is a pass-through to the guest's own "
          "library, and a re-initialised pthread attribute is accepted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
