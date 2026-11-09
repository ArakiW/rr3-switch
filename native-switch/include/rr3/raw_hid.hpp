#pragma once
//
// RAW-HID CONTROLLER PATH
//
// WHY THIS EXISTS
// ---------------
// The Switch console's input API is Npad (libnx padUpdate / padGetState).
// On a hacked console, sys-con / dinput_redux intercept USB controllers
// (Xbox, DualShock 4, DualSense) and translate them into virtual Npads,
// so the port reads them seamlessly through the Npad API.
//
// On a PC running under an emulator (or native rehost harness), no such
// translation service exists: an Xbox Elite controller, DualShock 4, or
// generic USB gamepad arrives as a raw HID device. Because the port
// previously read ONLY through Npad, non-Npad controllers were completely
// invisible -- the port saw no buttons, no sticks, and no triggers.
//
// THE CONTRACT
// ------------
// 1. Enumerate HID devices, skipping those already claimed/reported by Npad.
// 2. Read input reports using hidRead.
// 3. Translate recognised layouts into rr3::gamepad::NpadState.
// 4. Feed NpadState into canonicalFromNpad() and applyMapping().
//    The rest of the port (the mapping file rr3-gamepad.txt, the guest's
//    SetButtonValueJNI / SetAxisValueJNI, the autopilot) never knows or
//    cares whether the input originated from an Npad or raw HID.
// 5. If a device cannot be recognised, LOG IT BY NAME AND REPORT LENGTH
//    and do not guess.
// ---------------------------------------------------------------------------

#include "rr3/gamepad.hpp"
#include <cstddef>
#include <cstdint>

namespace rr3 {
namespace raw_hid {

enum Layout : int {
    kLayoutUnknown = 0,
    kLayoutXbox = 1,
    kLayoutDualShock4 = 2,
    kLayoutDualSense = 3,
    kLayoutSwitchPro = 4,
    kLayoutGeneric = 5,
};

inline const char* layoutName(Layout layout) {
    switch (layout) {
        case kLayoutXbox: return "Xbox";
        case kLayoutDualShock4: return "DualShock4";
        case kLayoutDualSense: return "DualSense";
        case kLayoutSwitchPro: return "SwitchPro";
        case kLayoutGeneric: return "Generic";
        default: return "Unknown";
    }
}

struct RawHidDevice {
    uint32_t deviceId{0};
    uint16_t vid{0};
    uint16_t pid{0};
    char name[64]{0};
    size_t reportLength{0};
    Layout layout{kLayoutUnknown};
    bool isNpadReported{false};
    bool connected{false};
    void* sessionHandle{nullptr};
};

// Pure layout decoders: decode raw bytes into rr3::gamepad::NpadState.
// Everything here is pure and can be unit-tested on the host without Switch hardware.
bool decodeXboxToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out);
bool decodeDualShock4ToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out);
bool decodeDualSenseToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out);
bool decodeSwitchProToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out);
bool decodeGenericToNpad(const uint8_t* report, size_t length, rr3::gamepad::NpadState* out);

// Layout classifier: inspects VID/PID, device name, report length, and report header.
Layout identifyLayout(uint16_t vid, uint16_t pid, const char* name,
                      size_t reportLength, const uint8_t* initialReport);

// Translation dispatcher: routes a report from dev to the correct layout decoder.
bool translateRawHidToNpad(const RawHidDevice& dev, const uint8_t* report,
                           size_t length, rr3::gamepad::NpadState* out);

// System interface
void initRawHid();
void exitRawHid();
size_t enumerateRawHidDevices(RawHidDevice* outDevices, size_t maxDevices);
bool hidRead(RawHidDevice* dev, void* buffer, size_t length, size_t* bytesRead);

// Polling entry point for the frame loop:
// Reads the primary non-Npad raw HID device, if one is connected and active.
// Returns true if a raw HID device provided input.
bool pollRawHidInput(rr3::gamepad::NpadState* outState, RawHidDevice* outActiveDevice);

// Low-rate observability line for guest-view log (called every 300 frames)
void logRawHidState(uint64_t frame, const RawHidDevice* activeDevice,
                    const rr3::gamepad::NpadState* state);

// Host test helper: injects a mock raw HID report
void injectMockRawHidReport(const RawHidDevice& dev, const uint8_t* report, size_t length);
void clearMockRawHidReports();

} // namespace raw_hid
} // namespace rr3
