#pragma once
//
// THE LAUNCHER-SIDE SETTINGS LAYER.
//
// WHAT THIS IS
// ------------
// The PS Vita port of this game ships a launcher ("eboot.bin") AND a separate
// GRAPHICAL CONFIGURATOR ("configurador.bin"), and the configurator writes a
// PLAIN TEXT "config.txt" which the launcher TRANSLATES into the guest's own
// ENCRYPTED device profile ("eds/Vita*.plist").  The guest never sees the
// plain text: it reads its own profile, decrypts it, and reads the quality
// switches out of it.
//
// THAT IS THE LESSON AND IT IS THE RIGHT ONE, because it is the GUEST'S OWN
// mechanism: the settings live in the guest's device profile, and the launcher
// only decides WHAT GOES INTO IT.  Nothing in the host invents a parallel
// configuration system that the guest cannot see.
//
// SO THE PORT IS THE LAUNCHER.  This module is the translation step:
//
//     sdmc:/switch/rr3-native-rehost/rr3-settings.txt      (plain text)
//         -> the guest's own device profile, re-encrypted
//         -> the guest reads it exactly as it reads the original
//
// WHY THE SETTINGS FILE IS PLAIN TEXT AND THE PROFILE IS NOT
// ----------------------------------------------------------
// The guest's profile is encrypted with a key that is compiled into the guest
// binary.  A user who wants to change a setting must not have to run a
// hex editor, and the PS Vita port solved this the same way: a plain text
// file on the card, and a launcher that translates it.
//
// WHY THIS IS NOT "THE HOST INVENTING A QUALITY SETTING"
// ------------------------------------------------------
// The keys here are the GUEST'S OWN keys -- the ones the guest's native
// readers ask for (the bool getter at ELF 0x13c65a8, the int getter at
// 0x13c64d4) -- and the PS Vita spellings are accepted as ALIASES for them.
// So there is one truth (the guest's profile) and one writer (the port).
//
// THE THING THAT IS NOT FREE
// --------------------------
// Changing a quality switch changes the SET of shaders the guest compiles,
// and the guest recompiles ALL of them the first time it sees the new set.
// Measured 2026-10-31, archive 20261031-003750 (q27): the run died 42 s in
// while it was still compiling.  So a settings change is NOT free, and the
// file says so where the user will read it.
//
// THE CIPHER
// ----------
// The guest's RC4 variant, with the two instruction-level quirks: the S box
// lives at B[2..257] inside a 262-byte frame, and the 133-byte drop loop
// reads the UN-OFFSETED B[i] while writing the offsetted B[2+j].  The key is
// 8 bytes: a5 35 b3 b1 e8 43 e7 cf.
//
// tools/rr3-plist.py is the reference implementation and is pinned by
// native-switch/tests/test_ui_plist.py with a known-answer vector.  This
// header is the SAME algorithm in C++, and
// native-switch/tests/test_guest_settings.py compiles BOTH sides and compares
// them byte for byte -- a port that is one instruction out would ship a
// profile the guest silently cannot read, and the only symptom is a soft
// picture a hundred frames later.
//
// IT IS A STREAM CIPHER, so encrypt == decrypt, and the plaintext and the
// ciphertext have the SAME LENGTH.  That is why the profile can be edited
// without moving a single byte, and why the module keeps the length it was
// given.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <string>

namespace rr3 {

// The guest's profile cipher key: the guest's immediate
// 0xCFE743E8_B1B335A5, eight bytes.
constexpr unsigned char kGuestProfileKey[8] = {
    0xa5, 0x35, 0xb3, 0xb1, 0xe8, 0x43, 0xe7, 0xcf};

// The keystream of the guest's RC4 variant, `length` bytes.
void guestProfileKeystream(unsigned char* out, size_t length);

// XOR the keystream over `data` in place.  Encrypt and decrypt are the SAME
// operation, because it is a stream cipher.
void guestProfileTransform(unsigned char* data, size_t length);

// The plaintext of a profile.  Returns false when the bytes are not a
// profile (the guest's own XML header), and says why in `error`.
bool guestProfileDecrypt(const unsigned char* cipher, size_t length,
                          std::string& out, std::string* error);

// TRUE when the LEAF of a path ends in ".plist".
//
// THIS IS IN THE SETTINGS MODULE AND NOT IN THE FILE LAYER ON PURPOSE: the
// file layer cannot be built by a host compiler, so a predicate that lives
// there cannot be TESTED, and a predicate that cannot be tested is one
// offset away from being wrong.
//
// IT WAS WRONG BY ONE.  The first version compared "base + length - 5"
// against ".plist", which is SIX characters: the slice it compared was
// "plist", never ".plist", so the test was ALWAYS false and the settings
// layer never fired -- while every other instrument said it had.
//
// The rule the length arithmetic has to keep: the suffix is six bytes, so
// the offset is length - 6, and the guard is length >= 6.
bool guestProfileLeafIsPlist(const char* path);

// ---------------------------------------------------------------------------
// THE SETTINGS FILE
// ---------------------------------------------------------------------------

// How many settings one file may carry.
constexpr int kMaxGuestSettings = 64;

struct GuestSetting {
    char key[64];     // as written in the file, lower-cased
    char value[96];   // as written in the file
};

// One parsed settings file.
struct GuestSettings {
    int count{0};
    GuestSetting items[kMaxGuestSettings];
    // The quality tier, "calidad".  Empty when the file does not name one.
    char tier[32]{};
    // The guest profile the tier selects ("AndroidHigh", "AndroidXtraHigh",
    // ...).  Empty when the file does not select one, which means "keep the
    // profile the guest asked for".
    char profile[64]{};
    // The profile to PATCH, named by the "perfil" key.  Empty when the file
    // does not name one, which means "the profile the guest asks for", and
    // the port measures that to be AndroidHigh.plist.
    char target[64]{};
    // How many of the settings are explicit overrides of a guest key.
    int overrides{0};
    bool loaded{false};
    char path[512]{};
    char error[192]{};
};

// Reads a plain text settings file.
//
// The format is the PS Vita one, one setting per line:
//
//     # a comment
//     key = value
//
// Blank lines are ignored and a "#" starts a comment when it is the first
// non-blank character on the line.  Keys are matched case-insensitively.
// A file that is not there is NOT an error: it means "no settings", which is
// exactly what a card that has never been configured looks like.
bool guestSettingsLoad(const char* path, GuestSettings& out);

// The guest profile a quality tier selects.
//
// "bajo" / "medio" / "medioalto" / "alto" / "ultrawide" / "xtralto" select the
// guest's own capability tiers; "personal" and the empty string select
// nothing, which means "keep the profile the guest asked for and apply the
// explicit overrides only".
//
// Returns an empty string when the tier is not one of these.
const char* guestSettingsProfileForTier(const char* tier);

// The guest key a PS Vita spelling stands for, or null when the key is not an
// alias (in which case the key IS a guest key and is used as it is).
//
// The aliases are the PS Vita config.txt's Spanish switches.  The MAPPING is
// by MEANING and is not one-for-one measured; the AUTHORITATIVE spelling is
// always the guest's own key, and the settings file accepts it directly.
const char* guestSettingsAliasKey(const char* key);

// The guest key for a PS Vita alias, or the key itself when it is not an
// alias.  Never returns null.
const char* guestSettingsNativeKey(const char* key);

// ---------------------------------------------------------------------------
// THE PROFILE
// ---------------------------------------------------------------------------

// One <key>/<value> pair of the profile's XML.
struct GuestProfileKey {
    char key[64];
    char kind[16];    // "string", "integer", "real", "true" or "false"
    char value[96];
};

// Lists the keys a profile carries.  Returns how many were found.
int guestProfileParse(const std::string& xml, GuestProfileKey* out, int max);

// Sets one key of the profile's XML.
//
// The value is written in the KIND the profile already uses for that key, so
// a boolean key stays a boolean and an integer key stays an integer.  The
// Spanish "si"/"no" are accepted for booleans.
//
// Returns false and fills `error` when the profile has no such key, or when
// the value does not fit the kind.
bool guestProfileSetKey(std::string& xml, const char* key, const char* value,
                        std::string* error);

// Makes the plaintext exactly `size` bytes.
//
// The length must not change: the ciphertext is the plaintext XORed with a
// stream of the same length, and the profile sits in a record whose size is
// fixed.  Whitespace BETWEEN elements is what absorbs the difference -- XML
// does not care about it, and it is not inside a text value, so shortening it
// cannot change what the guest reads.
bool guestProfileFitLength(std::string& xml, size_t size, std::string* error);

// ---------------------------------------------------------------------------
// THE WHOLE TRANSLATION
// ---------------------------------------------------------------------------

// The result of the translation, so the caller can REPORT what happened
// instead of guessing.
struct GuestSettingsResult {
    bool applied{false};
    int changed{0};          // how many keys were really changed
    bool tierApplied{false}; // the tier swapped the profile
    char profile[64]{};      // the profile the guest will read
    char error[192]{};
};

// Translates the settings into the profile the guest must read.
//
// `requestedCipher` / `requestedLength` are the bytes the guest asked for and
// `baseCipher` / `baseLength` are the bytes of the profile the settings
// selected (the same bytes when the settings did not select one).  The
// RESULT has exactly the length of the requested profile, always: a profile
// that changed size would not fit the record it lives in.
//
// The bytes that come out are CIPHERTEXT.  The plaintext is what goes in.
bool guestSettingsComposeProfile(const GuestSettings& settings,
                                  const unsigned char* requestedCipher,
                                  size_t requestedLength,
                                  const unsigned char* baseCipher,
                                  size_t baseLength,
                                  std::string& out,
                                  GuestSettingsResult& result);

}  // namespace rr3
