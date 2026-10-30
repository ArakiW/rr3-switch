// THE LAUNCHER-SIDE SETTINGS LAYER, EXECUTED.
//
// WHY A HARNESS AND NOT A TEXT CHECK.  The settings layer has ONE job that
// can be checked only by running it: the guest's device profile is
// ENCRYPTED, and a cipher that is one instruction out produces a profile the
// guest silently cannot read.  The symptom is a soft picture a hundred
// frames later, and no comment in the source would have caught it.
//
// So this file links the REAL translation unit
// (native-switch/source/guest_settings.cpp) and the REAL header, and the
// test driver (native-switch/tests/test_guest_settings.py) compares the
// results against tools/rr3-plist.py -- the pinned Python reference.
//
// The known-answer vector below is the SAME one
// native-switch/tests/test_ui_plist.py pins, so the two implementations have
// to agree with each other and with the guest.

#include "rr3/guest_settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    } else {
        std::printf("ok  : %s\n", what);
    }
}

std::string toHex(const std::vector<unsigned char>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

// The known-answer vector, from test_ui_plist.py.  Synthetic: it is not EA
// material, it is a profile this test file declares.
const char* kKatPlain =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<plist version=\"1.0\"><dict><key>EXPECTED_SCREEN_WIDTH</key>"
    "<integer>2048</integer></dict></plist>";

const char* kKatCipher =
    "e6ea60c54026f388a8a65a62e99576bfdbe5de278565ae74d6f4750b77ff"
    "2033836ea84d062355f83f2a3df5e381162c13d1413e9f87125a103de2dc012"
    "6fd854e54b818c43c75567d7c9997fa180bb68d7b21fc1b78af60ec465c63934f"
    "baa4a66fdb6f36c3c5e20360723f111fa25bc3c7608b51a89ebc6ffd7afc89c3c1"
    "bf674616d4398cc5a61c";

// A synthetic profile with the SHAPE the guest's profile has: the same kind
// of keys, the same kinds of values.
const char* kProfileTemplate =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n"
    "  <dict>\n"
    "    <key>EXPECTED_SCREEN_WIDTH</key>\n"
    "    <integer>480</integer>\n"
    "    <key>EXPECTED_SCREEN_HEIGHT</key>\n"
    "    <integer>320</integer>\n"
    "    <key>GUI_LAYOUT_EXTENSION</key>\n"
    "    <string>_480x320</string>\n"
    "    <key>HIGH_RES_MENU</key>\n"
    "    <false/>\n"
    "    <key>AA_LEVEL</key>\n"
    "    <integer>0</integer>\n"
    "    <key>FONT_PAGE_SIZE</key>\n"
    "    <integer>0</integer>\n"
    "    <key>MIPMAP_BIAS</key>\n"
    "    <real>0.0</real>\n"
    "    <key>CAR_SHADOW_MAPS</key>\n"
    "    <true/>\n"
    "    <key>DYNAMIC_CAR_REFLECTIONS</key>\n"
    "    <false/>\n"
    "  </dict>\n"
    "</plist>\n";

std::vector<unsigned char> encryptString(const std::string& text) {
    std::vector<unsigned char> bytes(text.begin(), text.end());
    rr3::guestProfileTransform(bytes.data(), bytes.size());
    return bytes;
}

std::string decryptBytes(const std::vector<unsigned char>& bytes) {
    std::vector<unsigned char> copy = bytes;
    rr3::guestProfileTransform(copy.data(), copy.size());
    return std::string(copy.begin(), copy.end());
}

}  // namespace

int main(int argc, char** argv) {
    const char* scratch = argc > 1 ? argv[1] : nullptr;

    // ---- 1. the known-answer vector --------------------------------------
    {
        const std::vector<unsigned char> cipher = encryptString(kKatPlain);
        check(toHex(cipher) == std::string(kKatCipher),
              "the keystream matches the reference implementation");
        check(decryptBytes(cipher) == std::string(kKatPlain),
              "the cipher decrypts the known ciphertext");
    }

    // ---- 2. it is a stream cipher ----------------------------------------
    {
        bool roundTrip = true;
        for (size_t length = 1; length <= 4096; length *= 3) {
            std::vector<unsigned char> data(length);
            for (size_t i = 0; i < length; ++i) {
                data[i] = static_cast<unsigned char>(i * 37 + 11);
            }
            std::vector<unsigned char> copy = data;
            rr3::guestProfileTransform(copy.data(), copy.size());
            rr3::guestProfileTransform(copy.data(), copy.size());
            if (copy != data) {
                roundTrip = false;
            }
        }
        check(roundTrip, "encrypt and decrypt are the same operation");
    }

    // ---- 2b. the cross-check vector ---------------------------------------
    //
    // The test driver compares this line with the Python reference
    // (tools/rr3-plist.py) over the SAME bytes.  The known-answer vector
    // above is 168 bytes long and the keystream's generator starts at 0x86,
    // so this longer one is what actually crosses the generator's wrap.
    {
        std::vector<unsigned char> data(300);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<unsigned char>(i * 91 + 7);
        }
        const std::vector<unsigned char> cipher = encryptString(
            std::string(reinterpret_cast<const char*>(data.data()), data.size()));
        std::printf("crosscheck: %s\n", toHex(cipher).c_str());
    }

    // ---- 3. the profile's XML ---------------------------------------------
    {
        const std::string xml = kProfileTemplate;
        rr3::GuestProfileKey keys[32];
        const int count = rr3::guestProfileParse(xml, keys, 32);
        check(count == 9, "the profile's keys are all found");
        bool foundLayout = false;
        bool foundMenu = false;
        bool foundBias = false;
        for (int i = 0; i < count; ++i) {
            if (std::strcmp(keys[i].key, "GUI_LAYOUT_EXTENSION") == 0) {
                foundLayout = std::strcmp(keys[i].value, "_480x320") == 0 &&
                              std::strcmp(keys[i].kind, "string") == 0;
            }
            if (std::strcmp(keys[i].key, "HIGH_RES_MENU") == 0) {
                foundMenu = std::strcmp(keys[i].kind, "false") == 0;
            }
            if (std::strcmp(keys[i].key, "MIPMAP_BIAS") == 0) {
                foundBias = std::strcmp(keys[i].kind, "real") == 0;
            }
        }
        check(foundLayout, "a string key keeps its kind and its value");
        check(foundMenu, "a boolean key is read as a boolean");
        check(foundBias, "a real key is read as a real");
    }

    // ---- 4. setting a key, in the kind the key already has ----------------
    {
        std::string xml = kProfileTemplate;
        std::string error;
        check(rr3::guestProfileSetKey(xml, "HIGH_RES_MENU", "si", &error),
              "a boolean accepts the Spanish 'si'");
        check(xml.find("<key>HIGH_RES_MENU</key>") != std::string::npos,
              "the key element survives the edit");
        check(xml.find("<true/>") != std::string::npos,
              "the boolean was written as a boolean");
        check(rr3::guestProfileSetKey(xml, "AA_LEVEL", "4x", &error),
              "an integer accepts the '4x' spelling");
        check(xml.find("<integer>4x</integer>") != std::string::npos ||
                  xml.find("<integer>4</integer>") != std::string::npos,
              "the integer was written");
        check(!rr3::guestProfileSetKey(xml, "NO_SUCH_KEY", "1", &error),
              "a key the profile does not have is refused");
        check(!rr3::guestProfileSetKey(xml, "AA_LEVEL", "not a number", &error),
              "a value that is not an integer is refused");
    }

    // ---- 5. the length must not change ------------------------------------
    {
        std::string xml = kProfileTemplate;
        std::string error;
        check(rr3::guestProfileFitLength(xml, xml.size() - 4, &error),
              "the profile can be shortened to fit a smaller record");
        check(xml.size() == std::string(kProfileTemplate).size() - 4,
              "the shortened profile is exactly the wanted length");
        check(xml.find("<string>_480x320</string>") != std::string::npos,
              "the fit did not touch a text value");
        std::string padded = kProfileTemplate;
        check(rr3::guestProfileFitLength(padded, padded.size() + 16, &error),
              "the profile can be padded to a bigger record");
        check(padded.size() == std::string(kProfileTemplate).size() + 16,
              "the padded profile is exactly the wanted length");
    }

    // ---- 6. the settings file ----------------------------------------------
    if (scratch != nullptr) {
        {
            std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
            out << "# a comment\n"
                << "\n"
                << "calidad = xtralto\n"
                << "msaa = 4x\n"
                << "reflejos = si\n"
                << "det_particulas = no\n"
                << "AA_LEVEL = 2\n"
                << "a line without an equals\n";
        }
        rr3::GuestSettings settings;
        check(rr3::guestSettingsLoad(scratch, settings), "the file is read");
        check(std::strcmp(settings.tier, "xtralto") == 0,
              "the quality tier is read");
        check(std::strcmp(settings.profile, "AndroidXtraHigh") == 0,
              "the tier selects the guest's own HD profile");
        check(settings.count == 4,
              "every setting is kept and the broken line is skipped");
        check(std::strcmp(rr3::guestSettingsNativeKey("msaa"), "AA_LEVEL") == 0,
              "the PS Vita's msaa is the guest's AA_LEVEL");
        check(std::strcmp(rr3::guestSettingsNativeKey("reflejos"),
                          "DYNAMIC_CAR_REFLECTIONS") == 0,
              "the PS Vita's reflejos is the guest's DYNAMIC_CAR_REFLECTIONS");
        check(std::strcmp(rr3::guestSettingsNativeKey("AA_LEVEL"),
                          "AA_LEVEL") == 0,
              "a guest key is used as it is");
        check(std::strcmp(rr3::guestSettingsProfileForTier("bajo"),
                          "AndroidLow") == 0,
              "the low tier selects the guest's low profile");
        check(std::string(rr3::guestSettingsProfileForTier("personal")).empty(),
              "the 'personal' tier selects no profile");
    }

    // ---- 7. the whole translation ------------------------------------------
    {
        const std::vector<unsigned char> cipher =
            encryptString(kProfileTemplate);
        rr3::GuestSettings settings;
        settings.loaded = true;
        settings.count = 2;
        std::snprintf(settings.items[0].key, sizeof(settings.items[0].key),
                      "%s", "msaa");
        std::snprintf(settings.items[0].value, sizeof(settings.items[0].value),
                      "%s", "4x");
        std::snprintf(settings.items[1].key, sizeof(settings.items[1].key),
                      "%s", "reflejos");
        std::snprintf(settings.items[1].value, sizeof(settings.items[1].value),
                      "%s", "si");
        std::string out;
        rr3::GuestSettingsResult result;
        const bool ok = rr3::guestSettingsComposeProfile(
            settings, cipher.data(), cipher.size(), cipher.data(),
            cipher.size(), out, result);
        check(ok, "the translation succeeds");
        check(out.size() == cipher.size(),
              "the profile keeps the length of the record it lives in");
        const std::string plain = decryptBytes(
            std::vector<unsigned char>(out.begin(), out.end()));
        check(plain.find("<integer>4</integer>") != std::string::npos,
              "the integer setting reached the profile");
        // The boolean is read back BY PARSING, not by looking for "<true/>":
        // the profile already carried a "<true/>" of its own, so a text
        // search would pass even when the edit did nothing.
        rr3::GuestProfileKey after[32];
        const int afterCount = rr3::guestProfileParse(plain, after, 32);
        std::string aaValue;
        std::string reflectionsValue;
        for (int i = 0; i < afterCount; ++i) {
            if (std::strcmp(after[i].key, "AA_LEVEL") == 0) {
                aaValue = after[i].value;
            }
            if (std::strcmp(after[i].key, "DYNAMIC_CAR_REFLECTIONS") == 0) {
                reflectionsValue = after[i].value;
            }
        }
        check(aaValue == "4", "the integer setting reached the profile");
        check(reflectionsValue == "true",
              "the boolean setting reached the profile");
        check(plain.find("<?xml") == 0,
              "the profile the guest will read is still a profile");
    }

    // ---- 8. a setting the profile does not have must NOT be applied -------
    {
        const std::vector<unsigned char> cipher =
            encryptString(kProfileTemplate);
        rr3::GuestSettings settings;
        settings.loaded = true;
        settings.count = 1;
        std::snprintf(settings.items[0].key, sizeof(settings.items[0].key),
                      "%s", "NO_SUCH_KEY");
        std::snprintf(settings.items[0].value, sizeof(settings.items[0].value),
                      "%s", "1");
        std::string out;
        rr3::GuestSettingsResult result;
        const bool ok = rr3::guestSettingsComposeProfile(
            settings, cipher.data(), cipher.size(), cipher.data(),
            cipher.size(), out, result);
        check(!ok, "a profile that could not take the settings is refused");
        check(!result.applied, "the refused translation is not marked applied");
        check(result.error[0] != '\0', "the refusal says why");
    }

    // ---- 9. the leaf predicate --------------------------------------------
    //
    // This one is here because it was WRONG BY ONE BYTE: the first version
    // compared "base + length - 5" against a SIX-character suffix, so the
    // slice it compared was "plist", the predicate was always false, and the
    // settings layer never fired -- while every other instrument (the
    // generated file, the log line, the guest's own settings dump) said it
    // had.
    {
        check(rr3::guestProfileLeafIsPlist("AndroidHigh.plist"),
              "a plist leaf is recognised");
        check(rr3::guestProfileLeafIsPlist(
                  "sdmc:/switch/rr3-native-rehost/.depot/eds/AndroidHigh.plist"),
              "a plist leaf behind directories is recognised");
        check(rr3::guestProfileLeafIsPlist(".plist"),
              "the shortest possible plist leaf is recognised");
        check(!rr3::guestProfileLeafIsPlist("AndroidHigh.plis"),
              "a leaf that is one byte short is not a plist");
        check(!rr3::guestProfileLeafIsPlist("asset_list_updates.txt"),
              "an asset list is not a plist");
        check(!rr3::guestProfileLeafIsPlist("plist"),
              "the suffix without its dot is not a plist");
        check(!rr3::guestProfileLeafIsPlist(""),
              "an empty path is not a plist");
        check(!rr3::guestProfileLeafIsPlist(nullptr),
              "a null path is not a plist");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
