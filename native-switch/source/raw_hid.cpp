#include "rr3/raw_hid.hpp"
#include "rr3/recovery_guard.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace rr3 {
namespace raw_hid {

namespace {

// Helper: normalizes unsigned 8-bit to [-1.0f, +1.0f]
inline float byteToUnit(uint8_t value) {
    return (static_cast<float>(value) - 128.0f) / 127.0f;
}

// Helper: normalizes unsigned 8-bit to [0.0f, 1.0f]
inline float byteToPositive(uint8_t value) {
    return static_cast<float>(value) / 255.0f;
}

// Helper: clamp float to [-1.0f, +1.0f]
inline float clampUnit(float value) {
    if (value > 1.0f) return 1.0f;
    if (value < -1.0f) return -1.0f;
    return value;
}

// Helper: clamp float to [0.0f, 1.0f]
inline float clampPositive(float value) {
    if (value > 1.0f) return 1.0f;
    if (value < 0.0f) return 0.0f;
    return value;
}

// Mock state for host testing or emulator injection
constexpr size_t kMaxMockDevices = 4;
RawHidDevice g_mockDevices[kMaxMockDevices]{};
uint8_t g_mockReports[kMaxMockDevices][256]{};
size_t g_mockReportLengths[kMaxMockDevices]{};
bool g_mockReportAvailable[kMaxMockDevices]{};
size_t g_mockDeviceCount = 0;

// Device table for runtime tracking
constexpr size_t kMaxTrackedDevices = 8;
RawHidDevice g_trackedDevices[kMaxTrackedDevices]{};
size_t g_trackedCount = 0;
bool g_initialized = false;
uint64_t g_lastUnknownLoggedId = 0;

} // namespace

bool decodeXboxToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out) {
    // Standard XInput report: 20 bytes (or at least 8 bytes for inputs)
    // byte 0: D-pad & meta buttons
    // byte 1: Left trigger (0..255)
    // byte 2: Right trigger (0..255)
    // byte 3: Left stick X (0..255, 0=left, 255=right)
    // byte 4: Left stick Y (0..255, 0=up, 255=down)
    // byte 5: Right stick X (0..255)
    // byte 6: Right stick Y (0..255)
    // byte 7: Face buttons (ABXY, LB, RB, Guide, Menu)
    if (report == nullptr || out == nullptr || length < 8) {
        return false;
    }
    *out = rr3::gamepad::NpadState{};

    const uint8_t meta = report[0];
    const uint8_t face = report[7];

    out->up = (meta & 0x01u) != 0;
    out->down = (meta & 0x02u) != 0;
    out->left = (meta & 0x04u) != 0;
    out->right = (meta & 0x08u) != 0;
    out->plus = (meta & 0x10u) != 0;   // Start -> Plus
    out->minus = (meta & 0x20u) != 0;  // Back -> Minus
    out->stickL = (meta & 0x40u) != 0; // Left stick click
    out->stickR = (meta & 0x80u) != 0; // Right stick click

    out->a = (face & 0x01u) != 0;
    out->b = (face & 0x02u) != 0;
    out->x = (face & 0x04u) != 0;
    out->y = (face & 0x08u) != 0;
    out->l = (face & 0x10u) != 0;      // LB
    out->r = (face & 0x20u) != 0;      // RB
    out->home = (face & 0x40u) != 0;   // Guide

    // Analogue triggers: 0..1
    out->lt = byteToPositive(report[1]);
    out->rt = byteToPositive(report[2]);
    out->zl = out->lt > 0.5f;
    out->zr = out->rt > 0.5f;

    // Sticks: X is right-positive, Y is up-positive
    // byte 4 and 6 are down-positive (0=up), so we negate byteToUnit()
    out->lx = byteToUnit(report[3]);
    out->ly = -byteToUnit(report[4]);
    out->rx = byteToUnit(report[5]);
    out->ry = -byteToUnit(report[6]);

    return true;
}

bool decodeDualShock4ToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out) {
    if (report == nullptr || out == nullptr || length < 9) {
        return false;
    }
    // Determine common part offset
    size_t offset = 0;
    if (report[0] == 0x01u) {
        offset = 1; // USB or minimal BT
    } else if (report[0] == 0x11u) {
        offset = 3; // Extended BT
    } else {
        // Fallback: assume raw common part
        offset = 0;
    }
    if (length < offset + 9) {
        return false;
    }
    const uint8_t* common = report + offset;
    *out = rr3::gamepad::NpadState{};

    // Parse buttons: common[4..6]
    const uint8_t b0 = common[4];
    const uint8_t b1 = common[5];
    const uint8_t b2 = common[6];

    // Hat switch (b0 & 0x0F): 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=Center
    const uint8_t hat = b0 & 0x0Fu;
    static const int8_t kHatX[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};
    static const int8_t kHatY[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};
    if (hat < 9) {
        if (kHatX[hat] < 0) out->left = true;
        if (kHatX[hat] > 0) out->right = true;
        if (kHatY[hat] < 0) out->up = true;
        if (kHatY[hat] > 0) out->down = true;
    }

    out->x = (b0 & 0x10u) != 0; // Square -> X
    out->a = (b0 & 0x20u) != 0; // Cross -> A
    out->b = (b0 & 0x40u) != 0; // Circle -> B
    out->y = (b0 & 0x80u) != 0; // Triangle -> Y

    out->l = (b1 & 0x01u) != 0; // L1
    out->r = (b1 & 0x02u) != 0; // R1
    out->zl = (b1 & 0x04u) != 0; // L2 button
    out->zr = (b1 & 0x08u) != 0; // R2 button
    out->minus = (b1 & 0x10u) != 0; // Share -> Minus
    out->plus = (b1 & 0x20u) != 0;  // Options -> Plus
    out->stickL = (b1 & 0x40u) != 0; // L3
    out->stickR = (b1 & 0x80u) != 0; // R3

    out->home = (b2 & 0x01u) != 0; // PS button

    // Sticks
    out->lx = byteToUnit(common[0]);
    out->ly = -byteToUnit(common[1]); // Sony Y is down-positive -> negate for up-positive
    out->rx = byteToUnit(common[2]);
    out->ry = -byteToUnit(common[3]);

    // Triggers: common[7] (L2 pressure), common[8] (R2 pressure)
    out->lt = byteToPositive(common[7]);
    out->rt = byteToPositive(common[8]);
    if (out->lt > 0.5f) out->zl = true;
    if (out->rt > 0.5f) out->zr = true;

    return true;
}

bool decodeDualSenseToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out) {
    if (report == nullptr || out == nullptr || length < 10) {
        return false;
    }
    size_t offset = (report[0] == 0x01u) ? 1 : 0;
    if (length < offset + 10) {
        return false;
    }
    const uint8_t* common = report + offset;
    *out = rr3::gamepad::NpadState{};

    // Sticks at common[0..3]
    out->lx = byteToUnit(common[0]);
    out->ly = -byteToUnit(common[1]);
    out->rx = byteToUnit(common[2]);
    out->ry = -byteToUnit(common[3]);

    // Triggers at common[4..5]
    out->lt = byteToPositive(common[4]);
    out->rt = byteToPositive(common[5]);
    if (out->lt > 0.5f) out->zl = true;
    if (out->rt > 0.5f) out->zr = true;

    // Buttons at common[7..9] (common[6] is sequence counter)
    const uint8_t b0 = common[7];
    const uint8_t b1 = common[8];
    const uint8_t b2 = common[9];

    const uint8_t hat = b0 & 0x0Fu;
    static const int8_t kHatX[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};
    static const int8_t kHatY[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};
    if (hat < 9) {
        if (kHatX[hat] < 0) out->left = true;
        if (kHatX[hat] > 0) out->right = true;
        if (kHatY[hat] < 0) out->up = true;
        if (kHatY[hat] > 0) out->down = true;
    }

    out->x = (b0 & 0x10u) != 0;
    out->a = (b0 & 0x20u) != 0;
    out->b = (b0 & 0x40u) != 0;
    out->y = (b0 & 0x80u) != 0;

    out->l = (b1 & 0x01u) != 0;
    out->r = (b1 & 0x02u) != 0;
    if (b1 & 0x04u) out->zl = true;
    if (b1 & 0x08u) out->zr = true;
    out->minus = (b1 & 0x10u) != 0; // Create -> Minus
    out->plus = (b1 & 0x20u) != 0;  // Options -> Plus
    out->stickL = (b1 & 0x40u) != 0;
    out->stickR = (b1 & 0x80u) != 0;

    out->home = (b2 & 0x01u) != 0;

    return true;
}

bool decodeSwitchProToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out) {
    if (report == nullptr || out == nullptr || length < 8) {
        return false;
    }
    *out = rr3::gamepad::NpadState{};

    // Standard report (0x30 / 0x21): 64 or 49 bytes
    if ((report[0] == 0x30u || report[0] == 0x21u) && length >= 12) {
        // Byte 3: Right buttons
        const uint8_t rBtns = report[3];
        out->y = (rBtns & 0x01u) != 0;
        out->x = (rBtns & 0x02u) != 0;
        out->b = (rBtns & 0x04u) != 0;
        out->a = (rBtns & 0x08u) != 0;
        out->r = (rBtns & 0x40u) != 0;
        out->zr = (rBtns & 0x80u) != 0;
        if (out->zr) out->rt = 1.0f;

        // Byte 4: Shared buttons
        const uint8_t sBtns = report[4];
        out->minus = (sBtns & 0x01u) != 0;
        out->plus = (sBtns & 0x02u) != 0;
        out->stickR = (sBtns & 0x04u) != 0;
        out->stickL = (sBtns & 0x08u) != 0;
        out->home = (sBtns & 0x10u) != 0;

        // Byte 5: Left buttons
        const uint8_t lBtns = report[5];
        out->down = (lBtns & 0x01u) != 0;
        out->up = (lBtns & 0x02u) != 0;
        out->right = (lBtns & 0x04u) != 0;
        out->left = (lBtns & 0x08u) != 0;
        out->l = (lBtns & 0x40u) != 0;
        out->zl = (lBtns & 0x80u) != 0;
        if (out->zl) out->lt = 1.0f;

        // 12-bit stick values centered at 2048
        const uint16_t rawLx = static_cast<uint16_t>(report[6] | ((report[7] & 0x0Fu) << 8));
        const uint16_t rawLy = static_cast<uint16_t>((report[7] >> 4) | (report[8] << 4));
        const uint16_t rawRx = static_cast<uint16_t>(report[9] | ((report[10] & 0x0Fu) << 8));
        const uint16_t rawRy = static_cast<uint16_t>((report[10] >> 4) | (report[11] << 4));

        out->lx = clampUnit((static_cast<float>(rawLx) - 2048.0f) / 1500.0f);
        out->ly = clampUnit((static_cast<float>(rawLy) - 2048.0f) / 1500.0f);
        out->rx = clampUnit((static_cast<float>(rawRx) - 2048.0f) / 1500.0f);
        out->ry = clampUnit((static_cast<float>(rawRy) - 2048.0f) / 1500.0f);
        return true;
    }

    // Simple HID report (0x3F): 12 bytes
    if (report[0] == 0x3Fu && length >= 8) {
        const uint8_t b0 = report[1];
        out->a = (b0 & 0x01u) != 0;
        out->b = (b0 & 0x02u) != 0;
        out->x = (b0 & 0x04u) != 0;
        out->y = (b0 & 0x08u) != 0;
        out->l = (b0 & 0x10u) != 0;
        out->r = (b0 & 0x20u) != 0;
        out->zl = (b0 & 0x40u) != 0;
        out->zr = (b0 & 0x80u) != 0;
        if (out->zl) out->lt = 1.0f;
        if (out->zr) out->rt = 1.0f;

        const uint8_t b1 = report[2];
        out->minus = (b1 & 0x01u) != 0;
        out->plus = (b1 & 0x02u) != 0;
        out->stickL = (b1 & 0x04u) != 0;
        out->stickR = (b1 & 0x08u) != 0;
        out->home = (b1 & 0x10u) != 0;

        const uint8_t hat = report[3];
        static const int8_t kHatX[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};
        static const int8_t kHatY[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};
        if (hat < 9) {
            if (kHatX[hat] < 0) out->left = true;
            if (kHatX[hat] > 0) out->right = true;
            if (kHatY[hat] < 0) out->up = true;
            if (kHatY[hat] > 0) out->down = true;
        }

        out->lx = byteToUnit(report[4]);
        out->ly = -byteToUnit(report[5]);
        out->rx = byteToUnit(report[6]);
        out->ry = -byteToUnit(report[7]);
        return true;
    }

    return false;
}

bool decodeGenericToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out) {
    // Generic standard 8-button + 2-stick layout (6 to 14 bytes)
    // byte 0: Left stick X (0..255)
    // byte 1: Left stick Y (0..255, 0=up)
    // byte 2: Right stick X (0..255)
    // byte 3: Right stick Y (0..255, 0=up)
    // byte 4: Buttons 1..8
    // byte 5 (optional): Extended buttons / D-pad
    if (report == nullptr || out == nullptr || length < 5) {
        return false;
    }
    *out = rr3::gamepad::NpadState{};

    out->lx = byteToUnit(report[0]);
    out->ly = -byteToUnit(report[1]); // Up-positive
    out->rx = byteToUnit(report[2]);
    out->ry = -byteToUnit(report[3]);

    const uint8_t b0 = report[4];
    out->a = (b0 & 0x01u) != 0; // Button 1 -> A
    out->b = (b0 & 0x02u) != 0; // Button 2 -> B
    out->x = (b0 & 0x04u) != 0; // Button 3 -> X
    out->y = (b0 & 0x08u) != 0; // Button 4 -> Y
    out->l = (b0 & 0x10u) != 0; // Button 5 -> L
    out->r = (b0 & 0x20u) != 0; // Button 6 -> R
    out->zl = (b0 & 0x40u) != 0; // Button 7 -> ZL
    out->zr = (b0 & 0x80u) != 0; // Button 8 -> ZR
    if (out->zl) out->lt = 1.0f;
    if (out->zr) out->rt = 1.0f;

    if (length >= 6) {
        const uint8_t b1 = report[5];
        out->minus = (b1 & 0x01u) != 0;
        out->plus = (b1 & 0x02u) != 0;
        out->stickL = (b1 & 0x04u) != 0;
        out->stickR = (b1 & 0x08u) != 0;

        // High nibble or hat
        if ((b1 & 0xF0u) != 0) {
            const uint8_t hat = (b1 >> 4) & 0x0Fu;
            static const int8_t kHatX[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};
            static const int8_t kHatY[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};
            if (hat < 9) {
                if (kHatX[hat] < 0) out->left = true;
                if (kHatX[hat] > 0) out->right = true;
                if (kHatY[hat] < 0) out->up = true;
                if (kHatY[hat] > 0) out->down = true;
            }
        }
    }

    return true;
}

Layout identifyLayout(uint16_t vid, uint16_t pid, const char* name,
                      size_t reportLength, const uint8_t* initialReport) {
    // 1. Xbox 360 / One / Series / Elite (VID 0x045E or XInput name / 20-byte report)
    if (vid == 0x045Eu ||
        (name != nullptr && (std::strstr(name, "Xbox") != nullptr ||
                             std::strstr(name, "X-Box") != nullptr ||
                             std::strstr(name, "XInput") != nullptr)) ||
        (reportLength == 20 && initialReport != nullptr && (initialReport[0] & 0xF0u) <= 0x70u)) {
        return kLayoutXbox;
    }

    // 2. DualShock 4 / DualSense
    if (vid == 0x054Cu) {
        if (pid == 0x0CE6u || pid == 0x0DF2u ||
            (name != nullptr && std::strstr(name, "DualSense") != nullptr)) {
            return kLayoutDualSense;
        }
        return kLayoutDualShock4;
    }
    if (name != nullptr) {
        if (std::strstr(name, "DualSense") != nullptr) {
            return kLayoutDualSense;
        }
        if (std::strstr(name, "DualShock") != nullptr ||
            std::strstr(name, "Wireless Controller") != nullptr) {
            return kLayoutDualShock4;
        }
    }
    if (initialReport != nullptr) {
        if ((initialReport[0] == 0x01u && reportLength == 64) ||
            (initialReport[0] == 0x11u && reportLength == 78)) {
            return kLayoutDualShock4;
        }
    }

    // 3. Switch Pro Controller (raw HID)
    if (vid == 0x057Eu && (pid == 0x2009u || pid == 0x2006u || pid == 0x2007u)) {
        return kLayoutSwitchPro;
    }
    if (name != nullptr && std::strstr(name, "Pro Controller") != nullptr) {
        return kLayoutSwitchPro;
    }
    if (initialReport != nullptr) {
        if (initialReport[0] == 0x30u || initialReport[0] == 0x21u || initialReport[0] == 0x3Fu) {
            return kLayoutSwitchPro;
        }
    }

    // 4. Generic 8-button + 2-stick layout
    if (reportLength >= 5 && reportLength <= 14) {
        return kLayoutGeneric;
    }

    // Device layout is unrecognised -- do NOT guess!
    return kLayoutUnknown;
}

bool translateRawHidToNpad(const RawHidDevice& dev, const uint8_t* report,
                           size_t length, rr3::gamepad::NpadState* out) {
    if (report == nullptr || out == nullptr) {
        return false;
    }
    switch (dev.layout) {
        case kLayoutXbox:
            return decodeXboxToNpad(report, length, out);
        case kLayoutDualShock4:
            return decodeDualShock4ToNpad(report, length, out);
        case kLayoutDualSense:
            return decodeDualSenseToNpad(report, length, out);
        case kLayoutSwitchPro:
            return decodeSwitchProToNpad(report, length, out);
        case kLayoutGeneric:
            return decodeGenericToNpad(report, length, out);
        case kLayoutUnknown:
        default:
            // An unrecognised device must be logged by name and report length, and not guessed!
            if (dev.deviceId != g_lastUnknownLoggedId) {
                g_lastUnknownLoggedId = dev.deviceId;
                char details[192];
                std::snprintf(details, sizeof(details),
                              "name=\"%s\" vid=0x%04x pid=0x%04x rlen=%zu",
                              dev.name, dev.vid, dev.pid, length);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "raw-hid-unrecognised", dev.deviceId, 0, details);
            }
            return false;
    }
}

void initRawHid() {
    if (g_initialized) {
        return;
    }
#if defined(__SWITCH__)
    (void)usbHsInitialize();
#endif
    g_initialized = true;
}

void exitRawHid() {
    if (!g_initialized) {
        return;
    }
#if defined(__SWITCH__)
    usbHsExit();
#endif
    g_initialized = false;
    g_trackedCount = 0;
}

size_t enumerateRawHidDevices(RawHidDevice* outDevices, size_t maxDevices) {
    if (outDevices == nullptr || maxDevices == 0) {
        return 0;
    }

    size_t count = 0;

    // Check mock / injected devices first (host test & emulation injection)
    for (size_t i = 0; i < g_mockDeviceCount && count < maxDevices; ++i) {
        outDevices[count++] = g_mockDevices[i];
    }
    if (count > 0) {
        return count;
    }

#if defined(__SWITCH__)
    // Switch hardware / emulator enumeration
    UsbHsInterfaceFilter filter{};
    UsbHsInterface interfaces[8];
    s32 total = 0;
    Result res = usbHsQueryAllInterfaces(&filter, interfaces, sizeof(interfaces), &total);
    if (R_SUCCEEDED(res) && total > 0) {
        const size_t available = static_cast<size_t>(total);
        for (size_t i = 0; i < available && count < maxDevices; ++i) {
            const auto& inf = interfaces[i];
            const uint16_t vid = inf.device_desc.idVendor;
            const uint16_t pid = inf.device_desc.idProduct;

            // Check if Npad already manages this controller
            // Joy-Con or standard Pro controller managed by Npad API
            bool isNpad = false;
            if (vid == 0x057Eu && (pid == 0x2009u || pid == 0x2006u || pid == 0x2007u)) {
                // If Npad style set is active on console, Npad owns it
                isNpad = true;
            }

            RawHidDevice dev{};
            dev.deviceId = static_cast<uint32_t>(inf.deviceID);
            dev.vid = vid;
            dev.pid = pid;
            dev.isNpadReported = isNpad;
            dev.connected = true;
            dev.reportLength = 64; // default USB buffer size
            std::snprintf(dev.name, sizeof(dev.name), "USB_%04x:%04x", vid, pid);

            dev.layout = identifyLayout(vid, pid, dev.name, dev.reportLength, nullptr);
            outDevices[count++] = dev;
        }
    }
#endif

    return count;
}

bool hidRead(RawHidDevice* dev, void* buffer, size_t length, size_t* bytesRead) {
    if (dev == nullptr || buffer == nullptr || length == 0) {
        return false;
    }

    // 1. Mock / injected report read
    for (size_t i = 0; i < g_mockDeviceCount; ++i) {
        if (g_mockDevices[i].deviceId == dev->deviceId && g_mockReportAvailable[i]) {
            const size_t toCopy = std::min(length, g_mockReportLengths[i]);
            std::memcpy(buffer, g_mockReports[i], toCopy);
            if (bytesRead != nullptr) {
                *bytesRead = toCopy;
            }
            return true;
        }
    }

#if defined(__SWITCH__)
    // 2. Hardware USB read if session handle is established
    // (Omitted direct async URB submission here for safety unless device acquired)
#endif

    return false;
}

// THE DEVICE LIST IS CACHED, AND RESCANNED RARELY.
//
// WHY: pollRawHidInput() is called from the input pump EVERY FRAME while the
// Npad reports no activity -- and on a real console that is ALWAYS, because
// every console controller IS an Npad.  Enumerating the HID service's device
// list is an IPC round trip per device, and the port's frame budget is the
// thing this project has been fighting since the first day.
//
// ⇒ The list is refreshed every kRawHidRescanFrames frames and the reports
// are read from the CACHED list, so a pad that IS connected is still read
// every frame and stays responsive.  The refresh interval is short enough
// (a quarter of a second at 60 fps) that plugging a pad in is noticed
// immediately, and long enough that the console does not pay for the
// enumeration on every frame.
//
// The one thing this deliberately does NOT do is skip the report read: a
// controller that is being used must not feel throttled.
constexpr uint64_t kRawHidRescanFrames = 15;

bool pollRawHidInput(rr3::gamepad::NpadState* outState, RawHidDevice* outActiveDevice) {
    if (outState == nullptr) {
        return false;
    }

    static RawHidDevice s_devices[kMaxTrackedDevices];
    static size_t s_count = 0;
    static uint64_t s_polls = 0;
    const uint64_t poll = ++s_polls;
    if (poll == 1u || (poll % kRawHidRescanFrames) == 0u) {
        s_count = enumerateRawHidDevices(s_devices, kMaxTrackedDevices);
    }
    // NOT const: the layout of a device whose report has just been seen is
    // learned here and remembered, so the cache is written to.
    RawHidDevice* devices = s_devices;
    const size_t count = s_count;

    for (size_t i = 0; i < count; ++i) {
        // Skip devices claimed by Npad (console Joy-Cons / Npads)
        if (devices[i].isNpadReported) {
            continue;
        }

        uint8_t reportBuf[256];
        size_t bytes = 0;
        if (hidRead(&devices[i], reportBuf, sizeof(reportBuf), &bytes) && bytes > 0) {
            if (devices[i].layout == kLayoutUnknown) {
                devices[i].layout = identifyLayout(devices[i].vid, devices[i].pid,
                                                   devices[i].name, bytes, reportBuf);
            }
            if (translateRawHidToNpad(devices[i], reportBuf, bytes, outState)) {
                if (outActiveDevice != nullptr) {
                    *outActiveDevice = devices[i];
                }
                return true;
            }
        }
    }

    return false;
}

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

void logRawHidState(uint64_t frame, const RawHidDevice* activeDevice,
                    const rr3::gamepad::NpadState* state) {
    if ((frame % 300) != 0) {
        return;
    }
    char details[256];
    if (activeDevice != nullptr && activeDevice->connected) {
        std::snprintf(details, sizeof(details),
                      "frame=%llu dev=\"%s\" vid=0x%04x pid=0x%04x layout=%s "
                      "mapped=A:%d B:%d X:%d Y:%d LT:%.2f RT:%.2f LX:%.2f LY:%.2f",
                      static_cast<unsigned long long>(frame),
                      activeDevice->name, activeDevice->vid, activeDevice->pid,
                      layoutName(activeDevice->layout),
                      state ? state->a : 0, state ? state->b : 0,
                      state ? state->x : 0, state ? state->y : 0,
                      state ? state->lt : 0.0f, state ? state->rt : 0.0f,
                      state ? state->lx : 0.0f, state ? state->ly : 0.0f);
    } else {
        std::snprintf(details, sizeof(details),
                      "frame=%llu dev=none reading=npad",
                      static_cast<unsigned long long>(frame));
    }
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "raw-hid-device", 300, 0, details);
}

void injectMockRawHidReport(const RawHidDevice& dev, const uint8_t* report, size_t length) {
    size_t slot = g_mockDeviceCount;
    for (size_t i = 0; i < g_mockDeviceCount; ++i) {
        if (g_mockDevices[i].deviceId == dev.deviceId) {
            slot = i;
            break;
        }
    }
    if (slot >= kMaxMockDevices) {
        slot = 0; // overwrite first if full
    }
    g_mockDevices[slot] = dev;
    const size_t copyLen = std::min(length, sizeof(g_mockReports[slot]));
    if (report != nullptr && copyLen > 0) {
        std::memcpy(g_mockReports[slot], report, copyLen);
        g_mockReportLengths[slot] = copyLen;
        g_mockReportAvailable[slot] = true;
    } else {
        g_mockReportLengths[slot] = 0;
        g_mockReportAvailable[slot] = false;
    }
    if (slot == g_mockDeviceCount && g_mockDeviceCount < kMaxMockDevices) {
        ++g_mockDeviceCount;
    }
}

void clearMockRawHidReports() {
    g_mockDeviceCount = 0;
    std::memset(g_mockDevices, 0, sizeof(g_mockDevices));
    std::memset(g_mockReportLengths, 0, sizeof(g_mockReportLengths));
    std::memset(g_mockReportAvailable, 0, sizeof(g_mockReportAvailable));
}

} // namespace raw_hid
} // namespace rr3
