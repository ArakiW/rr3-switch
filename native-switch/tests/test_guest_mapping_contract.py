#!/usr/bin/env python3
"""The guest load bias must not be published before the guest image is mapped.

Real-hardware crash, 2026-11-10, attributed against the exact build the
console ran (commit a5ba9487):

    pc  static 0x836f4 -> rr3::reapDetachedThreadWorker(void*)
                        native-switch/source/pthread_bridge.cpp:5881
    lr  static 0x836f0 -> native-switch/source/pthread_bridge.cpp:5872
    esr=0x92000005  far=0x59a492e5d0  x25=0x59a29c1000

far - x25 == 0x1F6D5D0, the port's own kAssetSingletonElfVa, so the fault was
a read of a guest address.  It faulted because the loader had published the
load bias (the image was LOADED) while the image was still only RESERVED and
not MAPPED, and the guard in front of the read -- guestImageContains() --
asked the ELF's PT_LOAD table whether the address was in range and never
asked whether the image was mapped.

These checks keep that fix from being undone by a later edit.
"""

from pathlib import Path


def body_of(text: str, marker: str) -> str:
    """The text of the function whose DEFINITION starts at `marker`.

    A forward declaration is followed by ';' and must be skipped, so the
    occurrence whose parameter list is followed by '{' is the definition.
    """
    cursor = 0
    while True:
        found = text.find(marker, cursor)
        if found < 0:
            raise AssertionError("no definition of " + marker)
        open_paren = found + len(marker) - 1
        depth = 0
        close = -1
        for index in range(open_paren, len(text)):
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
                if depth == 0:
                    close = index
                    break
        if close >= 0 and text[close + 1:].lstrip().startswith("{"):
            brace = text.index("{", close)
            level = 0
            for index in range(brace, len(text)):
                if text[index] == "{":
                    level += 1
                elif text[index] == "}":
                    level -= 1
                    if level == 0:
                        return text[found:index + 1]
            raise AssertionError("unbalanced braces after " + marker)
        cursor = found + len(marker)


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    linker = (root / "source" / "multi_dso_linker.cpp").read_text(encoding="utf-8")
    header = (root / "include" / "rr3" / "elf_preflight.hpp").read_text(encoding="utf-8")
    pthread_bridge = (root / "source" / "pthread_bridge.cpp").read_text(encoding="utf-8")
    guard = (root / "source" / "recovery_guard.cpp").read_text(encoding="utf-8")

    # 1. The predicate that every guest read goes through must ask whether
    #    the image is MAPPED, not only whether the vaddr is in the ELF.
    contains = body_of(linker, "bool guestImageContains(")
    assert "loaderImageIsMapped" in contains, "guestImageContains ignores the mapping"
    assert "return false;" in contains
    assert "image.containsMemory" in contains, "the PT_LOAD range test was dropped"

    # 2. The bias is a promise that the bytes are readable, so it must be
    #    withheld until the mapping exists.
    bias = body_of(linker, "uintptr_t resolveMainImageLoadBias(")
    assert "loaderImageIsMapped" in bias, "resolveMainImageLoadBias ignores the mapping"
    assert "return 0;" in bias

    # 3. The helper itself must actually consult the loader's mapping state,
    #    and it must not be a constant-true expression.
    mapped = body_of(linker, "bool loaderImageIsMapped(")
    assert "processMapped" in mapped, "loaderImageIsMapped does not read the mapping state"
    assert "return true;" in mapped
    assert "return false;" in mapped
    assert "gSegmentedImages" in mapped

    # 4. The header must publish both, so the crash handler can ask.
    assert "bool loaderImageIsMapped(size_t index);" in header
    assert "bool loaderMainImageMapped();" in header
    assert "bool loaderMainImageMapped()" in linker

    # 5. The crash site keeps its guard: the raw read must still be behind
    #    guestImageContains(), within a few lines.
    read = "*reinterpret_cast<const volatile uintptr_t*>(slot)"
    read_at = pthread_bridge.index(read)
    window = pthread_bridge[max(0, read_at - 2000):read_at]
    assert "guestImageContains(" in window, "the crash site's guard was removed"
    assert "kAssetSingletonElfVa" in pthread_bridge
    guard_at = pthread_bridge.rindex("guestImageContains(", 0, read_at)
    assert read_at - guard_at < 2000, "the guard is too far from the read"
    guard_statement = "if (biasNow == 0 || !guestImageContains(slot, sizeof(uintptr_t))) {"
    assert pthread_bridge.count(guard_statement) == 1, \
        "the crash site's guard must be the only one of its kind"

    # 6. The dump must decode its own ESR, and must not carry the wrong
    #    claim that EC 0x24 is a SPAL.
    assert "0x24 = SPAL" not in guard, "the wrong ESR claim is back"
    assert "fault_class" in guard, "the dump does not decode its exception class"
    assert "far_attribution" in guard, "the dump does not say whose memory faulted"
    assert "main_image_mapped" in guard, "the dump does not say whether the image was mapped"
    assert "data-abort-translation-fault-l1" in guard
    assert "spal" in guard

    # 7. The handler still runs on the kernel exception stack: nothing there
    #    may allocate or call into the C++ runtime.
    handler = body_of(guard, "void __libnx_exception_handler(")
    for forbidden in ("std::", "malloc(", "free(", "fopen(", "snprintf("):
        assert forbidden not in handler, "the crash handler may not use " + forbidden

    print("PASS: the guest load bias is withheld until the image is mapped; "
          "the dump decodes its own ESR")


if __name__ == "__main__":
    main()
