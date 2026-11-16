# THE GUEST'S FMOD ABI CONTRACT, PINNED AGAINST THE PORT.
#
# WHY THIS FILE EXISTS.  The port's FMOD shim used the MODERN FMOD 5
# constants while the guest was compiled against an OLDER generation
# (FMOD Ex 4.x).  The port's own self-test compared the shim against the
# PORT'S OWN constants, so it reported 0xfff -- every check passed -- while
# the guest heard nothing at all.  The instrument was answering the ledger,
# not the reality.
#
# THE ORACLE IS THE GUEST BINARY, NOT THIS PORT.  Every constant below
# carries the guest's own instruction that proves it:
#
#   openstate   libRealRacing3.so 0xc05684 is the guest's ONLY getOpenState
#               call; 0xc0568c "cbz w8, 0xc056d4" takes the branch that
#               calls playSound when the state is 0.  => READY == 0.
#   speakermode 0xc05d6c "mov w1, #2" with 2 output channels (0xc05e90
#               "mov w4, #2").  => STEREO == 2.
#   dsp type    0xc06a20/0xc06aa4/0xc06ae4/0xc06b2c/0xc06b70/0xc06b98/
#               0xc06bb8 pass 6, 11, 5, 10, 18, 8, 7.
#   exinfo      0xc075dc "add x3, sp, #0x68" is a 216-byte struct whose
#               fields are numchannels +0x0c, defaultfrequency +0x10,
#               format +0x14, decodebufferlength +0x18, callback +0x38.
#   length type 0xc07d48, 0xc08730, 0xc08aa0 pass 1 and 2 -- never 0.
#   position    0xc07d38, 0xc08ac8 pass 1 -- never 0.
#   format      0xc090f4 "mov w10, #0xbb80" (48000) and 0xc09100
#               "ccmp w8, #2" -- the guest REQUIRES rate 48000 and PCM16.
#
# THE TEST READS THE CODE, WITH COMMENTS AND STRINGS MASKED OUT.  A test
# that asserts "the text appears somewhere" is satisfied by a comment, and
# this repository has already been bitten by that.
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HPP = ROOT / "native-switch" / "include" / "rr3" / "fmod_shim.hpp"
CPP = ROOT / "native-switch" / "source" / "fmod_shim.cpp"
PROBE = ROOT / "native-switch" / "source" / "guest_view_probe.cpp"

failures = []


def fail(message):
    failures.append(message)


def mask(text):
    """Remove C++ comments and string/char literals.

    A comment must not be able to satisfy an assertion: the port's own
    self-test once printed "expected=0xfff" only inside a comment.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if ch in ("\"", "'"):
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            out.append(" ")
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def mask_comments(text):
    """Remove C++ comments but KEEP string literals.

    The census format IS a string literal, so masking strings away would
    make "the census carries the measurement" untestable.  Comments are
    masked so a comment cannot satisfy the assertion.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def body_of(source, marker):
    """Return the masked body of the first function whose text contains marker."""
    masked = mask(source)
    at = masked.find(marker)
    if at < 0:
        return None
    start = masked.find("{", at)
    if start < 0:
        return None
    depth = 0
    for i in range(start, len(masked)):
        if masked[i] == "{":
            depth += 1
        elif masked[i] == "}":
            depth -= 1
            if depth == 0:
                return masked[start:i + 1]
    return None


def enum_values(source, name):
    masked = mask(source)
    m = re.search(r"enum\s+" + re.escape(name) + r"\s*\{", masked)
    if not m:
        return None
    start = m.end() - 1
    depth = 0
    end = None
    for i in range(start, len(masked)):
        if masked[i] == "{":
            depth += 1
        elif masked[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is None:
        return None
    values = {}
    for entry in masked[start + 1:end].split(","):
        entry = entry.strip()
        if not entry:
            continue
        m2 = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*(0[xX][0-9a-fA-F]+|\d+))?$", entry)
        if not m2:
            continue
        raw = m2.group(2)
        values[m2.group(1)] = int(raw, 0) if raw is not None else len(values)
    return values


def struct_layout(source, name):
    """Compute the byte offsets of a struct's members, like the compiler does."""
    masked = mask(source)
    m = re.search(r"struct\s+" + re.escape(name) + r"\s*\{", masked)
    if not m:
        return None
    start = m.end() - 1
    depth = 0
    end = None
    for i in range(start, len(masked)):
        if masked[i] == "{":
            depth += 1
        elif masked[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is None:
        return None
    offsets = {}
    cursor = 0
    align = 1
    for entry in masked[start + 1:end].split(";"):
        entry = entry.strip()
        if not entry:
            continue
        m2 = re.match(
            r"(unsigned\s+int|int|unsigned\s+char|void\s*\*|char)\s*\**\s*"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\s*([^\]]*)\])?$",
            entry,
        )
        if not m2:
            continue
        kind = m2.group(1)
        member = m2.group(2)
        if kind == "unsigned int" or kind == "int":
            size, member_align = 4, 4
        elif kind == "unsigned char" or kind == "char":
            size, member_align = 1, 1
        else:
            size, member_align = 8, 8
        if m2.group(3):
            count_expr = m2.group(3).strip()
            if not re.match(r"^\d+$", count_expr):
                # An array whose length is an expression: evaluate the
                # constant forms this project uses.
                expr = count_expr.replace("0x", "")
                m3 = re.match(r"^([0-9a-fA-F]+)\s*-\s*0x([0-9a-fA-F]+)$", count_expr)
                if m3:
                    count = int(m3.group(1), 16) - int(m3.group(2), 16)
                else:
                    continue
            else:
                count = int(count_expr, 0)
            size = size * count
        align = max(align, member_align)
        if cursor % member_align:
            cursor += member_align - (cursor % member_align)
        offsets[member] = cursor
        cursor += size
    if cursor % align:
        cursor += align - (cursor % align)
    return offsets, cursor


hpp = HPP.read_text(encoding="utf-8")
cpp = CPP.read_text(encoding="utf-8")
probe = PROBE.read_text(encoding="utf-8")

# --- 1. THE OPENSTATE GATE -------------------------------------------------
openstate = enum_values(hpp, "FMOD_OPENSTATE")
if openstate is None:
    fail("FMOD_OPENSTATE enum not found")
else:
    if openstate.get("FMOD_OPENSTATE_READY") != 0:
        fail(
            "FMOD_OPENSTATE_READY must be 0 -- the guest's cbz w8 at "
            "libRealRacing3.so 0xc0568c takes the play branch only for 0; "
            f"the port has {openstate.get('FMOD_OPENSTATE_READY')}"
        )
    for name, value in openstate.items():
        if name != "FMOD_OPENSTATE_READY" and value == 0:
            fail(f"{name} must not be 0: 0 is the guest's READY value")

create_sound = body_of(cpp, "FMOD_RESULT fmodSystemCreateSound")
if create_sound is None:
    fail("fmodSystemCreateSound body not found")
elif "created->openState = FMOD_OPENSTATE_READY;" not in create_sound:
    fail("fmodSystemCreateSound must mark a new sound READY")

get_open_state = body_of(cpp, "FMOD_RESULT fmodSoundGetOpenState")
if get_open_state is None:
    fail("fmodSoundGetOpenState body not found")
else:
    if "*openstate = self->openState;" not in get_open_state:
        fail("fmodSoundGetOpenState must answer with the sound's own state")
    if "openStateQueries" not in get_open_state or "openStateReady" not in get_open_state:
        fail("fmodSoundGetOpenState must count the gate (openStateQueries/Ready)")

# --- 2. THE SPEAKER MODE ---------------------------------------------------
speaker = enum_values(hpp, "FMOD_SPEAKERMODE")
if speaker is None:
    fail("FMOD_SPEAKERMODE enum not found")
else:
    if speaker.get("FMOD_SPEAKERMODE_STEREO") != 2:
        fail(
            "FMOD_SPEAKERMODE_STEREO must be 2 -- the guest passes 2 for a "
            f"2-channel stream (0xc05d6c); the port has "
            f"{speaker.get('FMOD_SPEAKERMODE_STEREO')}"
        )
    if "FMOD_SPEAKERMODE_DEFAULT" in speaker:
        fail("FMOD_SPEAKERMODE must not have DEFAULT: the guest's generation has no member 0")

# --- 3. THE DSP TYPES ------------------------------------------------------
dsp = enum_values(hpp, "FMOD_DSP_TYPE")
if dsp is None:
    fail("FMOD_DSP_TYPE enum not found")
else:
    expected = {
        "FMOD_DSP_TYPE_HIGHPASS": 5,
        "FMOD_DSP_TYPE_ECHO": 6,
        "FMOD_DSP_TYPE_FLANGE": 7,
        "FMOD_DSP_TYPE_DISTORTION": 8,
        "FMOD_DSP_TYPE_LIMITER": 10,
        "FMOD_DSP_TYPE_PARAMEQ": 11,
        "FMOD_DSP_TYPE_SFXREVERB": 18,
    }
    for name, want in expected.items():
        got = dsp.get(name)
        if got != want:
            fail(
                f"{name} must be {want} (the guest passes that number at "
                f"0xc06a20/0xc06aa4/0xc06ae4/0xc06b2c/0xc06b70/0xc06b98/0xc06bb8); "
                f"the port has {got}"
            )

# --- 4. THE exinfo STRUCT --------------------------------------------------
layout = struct_layout(hpp, "FMOD_CREATESOUNDEXINFO")
if layout is None:
    fail("FMOD_CREATESOUNDEXINFO not found")
else:
    offsets, size = layout
    expected_offsets = {
        "cbsize": 0x00,
        "length": 0x04,
        "fileoffset": 0x08,
        "numchannels": 0x0C,
        "defaultfrequency": 0x10,
        "format": 0x14,
        "decodebufferlength": 0x18,
        "pcmreadcallback": 0x38,
    }
    for member, want in expected_offsets.items():
        got = offsets.get(member)
        if got != want:
            fail(
                f"FMOD_CREATESOUNDEXINFO::{member} must be at +0x{want:02x} "
                f"(the guest's own layout); the port has "
                + ("missing" if got is None else f"+0x{got:02x}")
            )
    if size != 216:
        fail(
            "FMOD_CREATESOUNDEXINFO must be 216 bytes -- the guest's cbsize is "
            f"216 (0xc075c0); the port's is {size}"
        )
if "static_assert(sizeof(FMOD_CREATESOUNDEXINFO) == 216" not in mask(hpp):
    fail(
        "the 216-byte size must be ENFORCED by a static_assert -- a size that "
        "is only a convention is a size that drifts"
    )

# --- 5. THE LENGTH AND POSITION TYPES -------------------------------------
get_length = body_of(cpp, "FMOD_RESULT fmodSoundGetLength")
if get_length is None:
    fail("fmodSoundGetLength body not found")
elif "lengthtype == 0U" in get_length:
    fail(
        "fmodSoundGetLength must not answer only lengthtype 0 -- the guest "
        "passes 1 and 2 (0xc07d48, 0xc08730, 0xc08aa0) and always got 0"
    )
elif "lengthtype <= 2U" not in get_length:
    fail("fmodSoundGetLength must answer the guest's length types 1 and 2")

get_position = body_of(cpp, "FMOD_RESULT fmodChannelGetPosition")
if get_position is None:
    fail("fmodChannelGetPosition body not found")
elif "positiontype == 0U" in get_position:
    fail(
        "fmodChannelGetPosition must not answer only positiontype 0 -- the "
        "guest passes 1 (0xc07d38, 0xc08ac8) and always got 0"
    )
elif "positiontype <= 1U" not in get_position:
    fail("fmodChannelGetPosition must answer the guest's position type 1")

# --- 6. THE SOFTWARE FORMAT -------------------------------------------------
set_format = body_of(cpp, "FMOD_RESULT fmodSystemSetSoftwareFormat")
if set_format is None:
    fail("fmodSystemSetSoftwareFormat body not found")
elif "self->softwareRate =" in set_format or "self->softwareFormat =" in set_format:
    fail(
        "fmodSystemSetSoftwareFormat must not overwrite the DEVICE's format: "
        "the guest reads it back and returns 25 unless the rate is 48000 and "
        "the format is PCM16 (0xc090f4 / 0xc09100)"
    )

get_format = body_of(cpp, "FMOD_RESULT fmodSystemGetSoftwareFormat")
if get_format is None:
    fail("fmodSystemGetSoftwareFormat body not found")
else:
    if "*samplerate = self->softwareRate;" not in get_format:
        fail("fmodSystemGetSoftwareFormat must answer the device's rate")
    if "*format = self->softwareFormat;" not in get_format:
        fail("fmodSystemGetSoftwareFormat must answer the device's format")

masked_hpp = mask(hpp)
if "softwareRate = 48000;" not in masked_hpp:
    fail("the device's rate must be 48000 (0xc090f4)")
if "softwareFormat = FMOD_SOUND_FORMAT_PCM16;" not in masked_hpp:
    fail("the device's format must be PCM16 (0xc09100)")

# --- 7. THE CENSUS MUST CARRY THE GATE ------------------------------------
masked_probe = mask_comments(probe)
if "osq=%llu" not in masked_probe or "osr=%llu" not in masked_probe:
    fail(
        "the census line must carry osq= and osr= -- the gate's measurement "
        "has to be in the number a round reads, not in a log nobody reads"
    )

if failures:
    print("FAIL: the port does not match the guest's FMOD ABI")
    for item in failures:
        print("  - " + item)
    sys.exit(1)

print(
    "PASS: the port's FMOD constants match the guest's binary "
    "(openstate READY=0, stereo=2, the seven DSP types, the 216-byte exinfo, "
    "the guest's length/position types, and the device's 48000/PCM16 format)"
)
