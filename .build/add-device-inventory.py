import pathlib

root = pathlib.Path(r"E:\AI\RealRacing3-switch\native-switch")

# ---------------------------------------------------------------- the header
h = root / "include" / "rr3" / "raw_hid.hpp"
t = h.read_text(encoding="utf-8")
decl = """// Low-rate observability line for guest-view log (called every 300 frames)
void logRawHidState(uint64_t frame, const RawHidDevice* activeDevice,
                    const rr3::gamepad::NpadState* state);
"""
new_decl = """// Low-rate observability line for guest-view log (called every 300 frames)
void logRawHidState(uint64_t frame, const RawHidDevice* activeDevice,
                    const rr3::gamepad::NpadState* state);

// THE DEVICE INVENTORY: every device the port can see, named.
//
// WHY IT IS SEPARATE FROM THE STATE LINE.  The state line only says
// something when a button is actually pressed, so "my controller does
// nothing" and "the port never saw my controller" are the SAME observation
// in the log -- which is exactly the question the user asked and could not
// be answered.
//
// ⇒ The inventory names every device, whether the HID service reports it as
// an Npad or the port found it as a raw one, with its VID/PID, its name and
// the layout the port recognised.  A device the port cannot identify is
// reported as unknown rather than dropped, because an unrecognised device
// that is REPORTED is a finding and a silently ignored one is the bug.
void logRawHidInventory(uint64_t frame);
"""
if "logRawHidInventory" not in t:
    t = t.replace(decl, new_decl)
    h.write_text(t, encoding="utf-8", newline="")
    print("header: declared")
else:
    print("header: already declared")

# ---------------------------------------------------------------- the source
c = root / "source" / "raw_hid.cpp"
s = c.read_text(encoding="utf-8")
impl = """
// THE DEVICE INVENTORY.
//
// Every device the port can see, named, with the layout the port recognised
// for it.  See the header for why this is separate from the state line.
void logRawHidInventory(uint64_t frame) {
    if ((frame % 300) != 0) {
        return;
    }
    RawHidDevice devices[kMaxTrackedDevices];
    const size_t count = enumerateRawHidDevices(devices, kMaxTrackedDevices);

    unsigned raw = 0;
    unsigned npad = 0;
    for (size_t i = 0; i < count; ++i) {
        if (devices[i].isNpadReported) {
            ++npad;
        } else {
            ++raw;
        }
        char details[320];
        std::snprintf(details, sizeof(details),
                      "dev=\"%s\" vid=0x%04x pid=0x%04x layout=%s report_len=%llu "
                      "npad_reported=%d connected=%d",
                      devices[i].name,
                      static_cast<unsigned>(devices[i].vid),
                      static_cast<unsigned>(devices[i].pid),
                      layoutName(devices[i].layout),
                      static_cast<unsigned long long>(devices[i].reportLength),
                      devices[i].isNpadReported ? 1 : 0,
                      devices[i].connected ? 1 : 0);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "hid-device", static_cast<unsigned>(i),
                            devices[i].deviceId, details);
    }

    char summary[192];
    std::snprintf(summary, sizeof(summary),
                   "devices=%llu raw=%u npad=%u "
                   "note=an_Xbox_or_DS_pad_that_is_not_listed_here_is_not_reachable",
                   static_cast<unsigned long long>(count), raw, npad);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "hid-inventory", 0, 0, summary);
}
"""
if "logRawHidInventory" not in s:
    # Put it right before logRawHidState, which is the last function in the file.
    anchor = "void logRawHidState(uint64_t frame, const RawHidDevice* activeDevice,"
    i = s.index(anchor)
    s = s[:i] + impl.lstrip("\n") + "\n" + s[i:]
    c.write_text(s, encoding="utf-8", newline="")
    print("source: implemented")
else:
    print("source: already implemented")

# ---------------------------------------------------------------- the caller
g = root / "source" / "guest_input.cpp"
u = g.read_text(encoding="utf-8")
call_anchor = "    rr3::raw_hid::logRawHidState(g_frameCounter, rawActive ? &rawDev : nullptr, &physical);"
new_call = call_anchor + """
    // THE DEVICE INVENTORY, EVERY 300 FRAMES, UNCONDITIONALLY.
    //
    // It is deliberately NOT next to the other loggers: the question it
    // answers -- "does the port see my controller at all?" -- has to be
    // answerable on a round where nobody touched anything, and that is
    // exactly the round where the state line says nothing.
    rr3::raw_hid::logRawHidInventory(g_frameCounter);"""
if "logRawHidInventory" not in u:
    if call_anchor not in u:
        raise SystemExit("the logRawHidState call site was not found")
    u = u.replace(call_anchor, new_call)
    g.write_text(u, encoding="utf-8", newline="")
    print("caller: wired")
else:
    print("caller: already wired")
