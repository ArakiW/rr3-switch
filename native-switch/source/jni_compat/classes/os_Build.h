#pragma once
#include "../jni.h"
#include "../jni_internals.h"

// android.os.Build was carrying one field, MANUFACTURER, holding the string
// "JohnnyonFlame" -- a placeholder, not a device. Every other field the engine
// can ask this class for was absent, so any read of Build.MODEL / DEVICE /
// PRODUCT / HARDWARE / FINGERPRINT came back null.
//
// That matters because this is where the engine builds its idea of what it is
// running on. FUN_012afaa0 -- the function whose return value is stored as
// [rend+0x4fc], the "GPU id" the render loop waits to equal 4 -- reads device
// state rather than the GL model: its string constants are 'device', 'id',
// 'onlyWIFI', "Carrier: '%s'", 'true' and 'false', it calls strcasecmp, and it
// goes near getSupportedAbis and getScreenDPI. The model string is provably
// NOT the input (530 -> id 5 and 420 -> id 5, with p500 moving each time to
// prove the edit reached the engine), so the Android side is the remaining
// candidate.
//
// The values below are one real, self-consistent Qualcomm handset: a Galaxy
// S5 (SM-G900F, "klte"), whose GPU is an Adreno 420. That is the same part
// gfx_gles.cpp advertises, so the GL identity and the Build identity now agree
// instead of contradicting each other. Nothing here is a fingerprint of a real
// person and no serial number is exposed.
//
// ACCEPTANCE, stated before the run so a null result is readable: this is a
// hypothesis, and it is only confirmed if [rend+0x4fc] MOVES. If the next Eden
// archive still reports t4fc=5, "the Build fields decide the id" is REFUTED
// the same way the model hypothesis was, and the search moves to whatever else
// 0x12afaa0 reads. Do not keep editing these strings one at a time hoping for
// a change; change them as a set, once, and let one run answer it.
class osBuild : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    // A/B switch, added 2026-10-04, DEFAULT IS THE SINGLE PLACEHOLDER.
    // Filled-in identity is a correctness fix (the class previously answered
    // "JohnnyonFlame" and null for everything else), but like the errno change
    // it was verified only against runs that were already in the broken state.
    // The errno A/B came out identical, so this is now the live suspect for
    // the regression that appeared after 2026-02-02. Set to 1 to restore the
    // full 22-field identity.
#ifndef RR3_BUILD_IDENTITY
#define RR3_BUILD_IDENTITY 1
#endif

#if RR3_BUILD_IDENTITY
    // Identity -- one real, self-consistent Qualcomm handset whose GPU matches
    // what gfx_gles.cpp advertises (Adreno 420): Galaxy S5, SM-G900F, klte.
    static String MANUFACTURER;
    static String MODEL;
    static String BRAND;
    static String DEVICE;
    static String PRODUCT;
    static String HARDWARE;
    static String BOARD;
    static String BOOTLOADER;
    static String RADIO;
    static String TAGS;
    static String TYPE;
    static String USER;
    static String HOST;
    static String ID;
    static String DISPLAY;
    static String FINGERPRINT;
    static String RELEASE;
    static int32_t SDK_INT;
    static String CPU_ABI;
    static String CPU_ABI2;
    static String CPU_ABL;
#else
    static String MANUFACTURER;
#endif
};
