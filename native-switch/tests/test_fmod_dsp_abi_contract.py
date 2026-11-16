#!/usr/bin/env python3
"""The FMOD_DSP_DESCRIPTION ABI, checked by OFFSET *AND* BY TYPE.

THE GUEST FILLS THIS STRUCT ITSELF, so the guest's binary is the authority
for its layout, and the port's header has to match it byte for byte.  This
test parses the header's struct, computes the C offsets, and compares them
with the offsets MEASURED IN THE GUEST (libRealRacing3.so, the only
createDSP call site is 0xc05cd4):

    0x00 name[32]          strncpy(desc, "decklink", 32)   @0xc05c6c
    0x20 version           8-byte store                      @0xc05ccc
    0x24 channels          8-byte store                      @0xc05ccc
    0x28 create            stp x9, x10                       @0xc05c8c
    0x30 release           stp x9, x10                       @0xc05c8c
    0x38 reset             str x8                            @0xc05cb0
    0x40 read              stp x9, xzr                       @0xc05ca8
    0x48 setposition       stp x9, xzr                       @0xc05ca8
    0x50 numparameters     str wzr                           @0xc05cac
    0x58 paramdesc         str q0                            @0xc05c84
    0x60 setparameter      str q0                            @0xc05c84
    0x68 getparameter      str q0                            @0xc05c88
    0x70 config            str q0                            @0xc05c88
    0x78 configwidth       stp xzr, x8                       @0xc05cd0
    0x7C configheight      stp xzr, x8                       @0xc05cd0
    0x80 userdata          stp xzr, x8                       @0xc05cd0

AND THE REAL libfmodex.so AGREES: it copies the description out of exactly
these offsets into a 136-byte internal struct (0x4f3e0-0x4f44c).

A WRONG ABI IS NOT A COSMETIC DEFECT: with the old definition (a
pluginsdk_version and a const char* name) the port read the guest's
description at the wrong offsets entirely, so the guest's read callback
could never be found, let alone called.  That is why the port is silent.

OFFSETS ALONE ARE NOT ENOUGH.  Every callback field is 8 bytes wide, so
turning "FMOD_DSP_READCALLBACK read;" into "void* read;" keeps every offset
and every size identical -- the layout still matches while the port can no
longer call the guest's callback at all.  So this test also pins:

  * the DECLARED TYPE of every callback field (the typedef, not a void*);
  * the FULL PARAMETER LIST of every callback typedef, in order -- the
    guest's read takes SIX arguments, which is the only thing that fixes
    its shape;
  * the shim's BEHAVIOUR (the description is kept; getUserData answers
    with the description's userdata), checked inside the function bodies
    rather than by a global text search.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "rr3" / "fmod_shim.hpp"
SHIM = ROOT / "source" / "fmod_shim.cpp"

# The offsets the GUEST was measured to use.  This is the oracle.
GUEST_OFFSETS = {
    "name": 0x00,
    "version": 0x20,
    "channels": 0x24,
    "create": 0x28,
    "release": 0x30,
    "reset": 0x38,
    "read": 0x40,
    "setposition": 0x48,
    "numparameters": 0x50,
    "paramdesc": 0x58,
    "setparameter": 0x60,
    "getparameter": 0x68,
    "config": 0x70,
    "configwidth": 0x78,
    "configheight": 0x7C,
    "userdata": 0x80,
}
GUEST_SIZE = 136

# The DECLARED TYPE of every callback field.  A void* here is the exact
# defect this test exists to catch.
GUEST_CALLBACK_TYPES = {
    "create": "FMOD_DSP_CREATECALLBACK",
    "release": "FMOD_DSP_RELEASECALLBACK",
    "reset": "FMOD_DSP_RESETCALLBACK",
    "read": "FMOD_DSP_READCALLBACK",
    "setposition": "FMOD_DSP_SETPOSITIONCALLBACK",
    "setparameter": "FMOD_DSP_SETPARAMCALLBACK",
    "getparameter": "FMOD_DSP_GETPARAMCALLBACK",
}

# The FULL PARAMETER LIST of every callback typedef, in order.  The guest's
# read callback takes six arguments; that is what fixes its shape.
GUEST_CALLBACK_PARAMS = {
    "FMOD_DSP_CREATECALLBACK": ["void* state"],
    "FMOD_DSP_RELEASECALLBACK": ["void* state"],
    "FMOD_DSP_RESETCALLBACK": ["void* state"],
    "FMOD_DSP_READCALLBACK": [
        "void* state", "float* inbuffer", "float* outbuffer",
        "unsigned int length", "int inchannels", "int outchannels",
    ],
    "FMOD_DSP_SETPOSITIONCALLBACK": ["void* state", "unsigned int position"],
    "FMOD_DSP_SETPARAMCALLBACK": ["void* state", "int param", "float value"],
    "FMOD_DSP_GETPARAMCALLBACK": [
        "void* state", "int param", "float* value", "char* valuestr",
    ],
}

FIELD = re.compile(
    r"^\s*(?P<type>[A-Za-z_][A-Za-z0-9_:<>*]*(?:\s+[A-Za-z_][A-Za-z0-9_:<>*]*)*)"
    r"\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"(?P<array>\[\s*(\d+)\s*\])?\s*;\s*$")

TYPEDEF = re.compile(
    r"typedef\s+int\s*\(\s*\*\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)\s*"
    r"\((?P<params>[^;]*?)\)\s*;", re.S)


def mask_code(text):
    """Blank out comments and string literals, keeping the code.

    Brace matching over raw text swallows a brace inside a comment and runs
    away for hundreds of thousands of characters, which is how an assertion
    ends up satisfied by the wrong thing.  Masking first keeps the code and
    throws away the noise.
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


def struct_body(text, name):
    start = text.index("struct " + name)
    start = text.index("{", start)
    return balanced_body(text, start)


def definition_body(text, signature):
    """The body of a function DEFINITION whose name starts with signature.

    A forward declaration would send a plain index() hunt into the next
    function's braces, so the definition is matched by its opening brace
    instead.
    """
    pattern = re.compile(re.escape(signature) + r"[^;{]*\{")
    match = pattern.search(text)
    assert match, "no definition found for " + signature
    return balanced_body(text, match.end() - 1)


def field_layout(body):
    """The C layout of the fields: alignment, size and offset."""
    layout = []
    offset = 0
    for raw in body.splitlines():
        line = raw.split("//")[0].strip()
        if not line:
            continue
        match = FIELD.match(line)
        assert match, "unparsed field line: " + repr(raw)
        name = match.group("name")
        field_type = match.group("type")
        array = match.group("array")
        if array is not None:
            size = int(match.group(4))
            align = 1
        elif field_type in ("unsigned int", "int"):
            size = 4
            align = 4
        else:
            # a pointer or a function pointer: 8 bytes, 8-byte aligned
            size = 8
            align = 8
        if offset % align:
            offset += align - (offset % align)
        layout.append((name, field_type, offset, size))
        offset += size
    return layout, offset


def typedef_params(header):
    params = {}
    for match in TYPEDEF.finditer(header):
        items = []
        for item in match.group("params").split(","):
            item = " ".join(item.split())
            if item:
                items.append(item)
        params[match.group("name")] = items
    return params


def main():
    header = HEADER.read_text(encoding="utf-8")
    shim = SHIM.read_text(encoding="utf-8")
    masked_header = mask_code(header)
    masked_shim = mask_code(shim)

    body = struct_body(masked_header, "FMOD_DSP_DESCRIPTION")
    layout, size = field_layout(body)
    offsets = {name: off for name, _, off, _ in layout}
    types = {name: field_type for name, field_type, _, _ in layout}
    arrays = {}
    for raw in body.splitlines():
        line = raw.split("//")[0].strip()
        match = FIELD.match(line)
        if match and match.group("array"):
            arrays[match.group("name")] = int(match.group(4))

    assert offsets == GUEST_OFFSETS, (
        "the DSP description's offsets differ from the guest's:\n"
        "  header: %r\n  guest : %r" % (offsets, GUEST_OFFSETS))
    assert size == GUEST_SIZE, (
        "the DSP description is %d bytes, the guest's is %d" % (size, GUEST_SIZE))

    # THE NAME IS AN INLINE ARRAY, NOT A POINTER.  The old definition had a
    # const char* and read the guest's struct at the wrong offsets entirely.
    assert types.get("name") == "char" and arrays.get("name") == 32, (
        "the name is not an inline char[32]: type=%r array=%r"
        % (types.get("name"), arrays.get("name")))
    assert "pluginsdk_version" not in offsets, \
        "pluginsdk_version belongs to another FMOD generation"

    # THE CALLBACK FIELDS MUST CARRY THEIR TYPEDEF.  A void* keeps every
    # offset and every size -- and loses the ability to call the guest.
    for field, expected in GUEST_CALLBACK_TYPES.items():
        actual = types.get(field)
        assert actual == expected, (
            "the %r field is declared %r, the guest's callback needs %r"
            % (field, actual, expected))

    # THE CALLBACK SIGNATURES ARE THE GUEST'S, IN ORDER.  The read callback
    # takes six arguments; that is the only thing that fixes its shape.
    params = typedef_params(masked_header)
    for typedef, expected in GUEST_CALLBACK_PARAMS.items():
        actual = params.get(typedef)
        assert actual is not None, "the %s typedef is missing" % typedef
        assert actual == expected, (
            "the %s typedef takes %r, the guest's callback takes %r"
            % (typedef, actual, expected))

    # THE DESCRIPTION IS KEPT, OR THE GUEST'S AUDIO CALLBACK IS LOST.
    create_body = definition_body(
        masked_shim, "FMOD_RESULT fmodSystemCreateDSP(")
    assert re.search(r"created->description\s*=\s*description\s*;", create_body), \
        "createDSP throws the guest's description away"

    getdata_body = definition_body(
        masked_shim, "FMOD_RESULT fmodDSPGetUserData(")
    assert re.search(r"->description->userdata", getdata_body), \
        "getUserData does not answer with the description's userdata"

    print("PASS: the DSP description matches the guest's layout byte for byte "
          "(%d bytes), every callback field carries its typedef, the read "
          "callback has the guest's six arguments, and the description is "
          "kept" % size)


if __name__ == "__main__":
    main()
