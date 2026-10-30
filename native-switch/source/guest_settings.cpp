#include "rr3/guest_settings.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rr3 {
namespace {

// ---------------------------------------------------------------------------
// THE ALIAS TABLE
// ---------------------------------------------------------------------------
//
// The PS Vita port's "config.txt" names its switches in Spanish.  The guest
// names the same switches in the guest's own English.  This table is the
// translation.
//
// THE MAPPING IS BY MEANING, and the honest part of this table is which
// entries are CERTAIN (the two names are the same idea) and which are a
// GUESS (the Spanish word is broader than the English key).  The certainty
// is written next to each entry, because the settings file is the thing a
// USER edits, and a wrong guess there is a wrong picture.
//
// THE AUTHORITATIVE SPELLING IS ALWAYS THE GUEST'S OWN KEY.  Every key in
// this table can be written directly in the settings file, and when the two
// disagree the guest's own key wins.  That is the rule that makes a wrong
// guess here harmless: the user can always say the real thing.
// ---------------------------------------------------------------------------
struct AliasEntry {
    const char* psKey;      // the PS Vita spelling
    const char* nativeKey;  // the guest's own key
    const char* owner;      // "profile", "rr3-pad.txt", "rr3-drive.txt"
};

// The certainty column:
//   name    -- the two names are the same idea, so the mapping is certain.
//   meaning -- the Spanish word is BROADER than the guest's key, so the
//              mapping is a guess and the guest's own key should be used
//              when it matters.
constexpr AliasEntry kAliases[] = {
    // The PS Vita's own quality tier: not a guest key at all.  It selects
    // WHICH guest profile the guest reads, which is a different thing
    // entirely (see guestSettingsProfileForTier).
    {"calidad", "", "tier"},
    // "name"
    {"msaa", "AA_LEVEL", "profile"},
    {"reflejos", "DYNAMIC_CAR_REFLECTIONS", "profile"},
    {"det_particulas", "USE_PARTICLE_EFFECTS", "profile"},
    {"det_pantalla_completa", "USE_FULL_SCREEN_EFFECTS", "profile"},
    {"det_danios", "FULL_OPPONENT_DAMAGE", "profile"},
    {"det_sombras_cabina", "CAR_INTERIOR_SHADOW_MAPS", "profile"},
    {"det_sombras_coche", "CAR_SHADOW_MAPS", "profile"},
    {"det_interior", "CAR_INTERIOR_LOD", "profile"},
    {"det_rivales", "EXTERIOR_SHADOWS_OPPONENT", "profile"},
    {"det_sombras_rivales", "EXTERIOR_SHADOWS_OPPONENT", "profile"},
    {"det_texturas", "TEXTURE_MIPMAP_LEVELS_TO_SKIP", "profile"},
    {"det_pista", "LIGHTMAP_MIPMAP_LEVELS_TO_SKIP", "profile"},
    // "meaning" -- these two are guesses and are labelled as such.
    {"det_coche", "EXTERIOR_SHADOWS_PLAYER", "profile"},
    {"brillos", "USE_FULL_SCREEN_EFFECTS", "profile"},
    // The PS Vita's CONTROL and SOUND switches do not belong to the guest's
    // profile at all: they are the LAUNCHER's own.  They are listed here so
    // that a user who ports a Vita config.txt over gets told WHERE each key
    // belongs instead of a silent "unknown key".
    {"giro", "", "rr3-pad.txt"},
    {"zona_muerta", "", "rr3-pad.txt"},
    {"invertir_giro", "", "rr3-pad.txt"},
    {"inclinacion", "", "rr3-pad.txt"},
    {"botones", "", "rr3-pad.txt"},
    {"volumen_motor", "", "rr3-drive.txt"},
    {"actualizaciones", "", "rr3-updater"},
};

constexpr int kAliasCount = static_cast<int>(sizeof(kAliases) / sizeof(kAliases[0]));

const AliasEntry* findAlias(const char* key) {
    if (key == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < kAliasCount; ++i) {
        if (std::strcmp(kAliases[i].psKey, key) == 0) {
            return &kAliases[i];
        }
    }
    return nullptr;
}

void toLowerInPlace(char* text) {
    if (text == nullptr) {
        return;
    }
    for (char* p = text; *p != '\0'; ++p) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = static_cast<char>(*p - 'A' + 'a');
        }
    }
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void trim(const char* begin, const char* end, char* out, size_t outSize) {
    while (begin < end && isSpace(*begin)) {
        ++begin;
    }
    while (end > begin && isSpace(*(end - 1))) {
        --end;
    }
    size_t length = static_cast<size_t>(end - begin);
    if (length >= outSize) {
        length = outSize - 1;
    }
    std::memcpy(out, begin, length);
    out[length] = '\0';
}

void copyBounded(char* out, size_t outSize, const std::string& value) {
    const size_t length =
        value.size() < outSize - 1 ? value.size() : outSize - 1;
    std::memcpy(out, value.data(), length);
    out[length] = '\0';
}

// The boolean spellings the settings file accepts.  The PS Vita's "si"/"no"
// are here because that is what a user porting a Vita config.txt will type.
bool parseBoolean(const char* value, bool* out) {
    if (value == nullptr) {
        return false;
    }
    char lowered[32]{};
    copyBounded(lowered, sizeof(lowered), value);
    toLowerInPlace(lowered);
    if (std::strcmp(lowered, "true") == 0 || std::strcmp(lowered, "si") == 0 ||
        std::strcmp(lowered, "yes") == 0 || std::strcmp(lowered, "on") == 0 ||
        std::strcmp(lowered, "1") == 0) {
        *out = true;
        return true;
    }
    if (std::strcmp(lowered, "false") == 0 || std::strcmp(lowered, "no") == 0 ||
        std::strcmp(lowered, "off") == 0 || std::strcmp(lowered, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

// An integer, with the PS Vita's "4x" spelling accepted: the guest's AA_LEVEL
// is a plain integer and a user writing "4x" means four samples.
bool parseInteger(const char* value, long* out) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    char buffer[64]{};
    copyBounded(buffer, sizeof(buffer), value);
    char* end = nullptr;
    const long parsed = std::strtol(buffer, &end, 10);
    if (end == buffer) {
        return false;
    }
    while (end != nullptr && *end != '\0' && isSpace(*end)) {
        ++end;
    }
    if (end != nullptr && *end != '\0') {
        if (*end != 'x' && *end != 'X') {
            return false;
        }
        ++end;
        while (end != nullptr && *end != '\0' && isSpace(*end)) {
            ++end;
        }
        if (end != nullptr && *end != '\0') {
            return false;
        }
    }
    *out = parsed;
    return true;
}

// ---------------------------------------------------------------------------
// THE PROFILE'S XML
// ---------------------------------------------------------------------------
//
// The profile is an XML plist, and the pairs look like this:
//
//     <key>AA_LEVEL</key>
//     <integer>4</integer>
//
// so a key is found by its "<key>NAME</key>" and the VALUE ELEMENT is what
// follows it.  The value element is what gets replaced; the key element and
// the whitespace between them are left alone, which keeps the edit as small
// as the thing it changes.
// ---------------------------------------------------------------------------
struct KeyElement {
    size_t start{0};   // the first byte of the value element
    size_t end{0};     // one past its last byte
    std::string kind;  // "string", "integer", "real", "true" or "false"
    std::string value; // the text inside the value element
};

bool equalsIgnoreCase(const std::string& a, const char* b) {
    if (b == nullptr) {
        return false;
    }
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        char left = a[i];
        char right = b[i];
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<char>(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

// Parses the VALUE ELEMENT that follows a "</key>".
bool parseValueElement(const std::string& xml, size_t afterKey, KeyElement* out) {
    size_t p = afterKey;
    while (p < xml.size() && isSpace(xml[p])) {
        ++p;
    }
    if (p >= xml.size() || xml[p] != '<') {
        return false;
    }
    const size_t start = p;
    size_t q = p + 1;
    while (q < xml.size() && xml[q] != '>' && xml[q] != '/' &&
           xml[q] != ' ') {
        ++q;
    }
    if (q >= xml.size()) {
        return false;
    }
    const std::string tag = xml.substr(p + 1, q - (p + 1));
    if (tag == "true" || tag == "false") {
        const size_t close = xml.find('>', q);
        if (close == std::string::npos) {
            return false;
        }
        out->start = start;
        out->end = close + 1;
        out->kind = tag;
        out->value = tag;
        return true;
    }
    const size_t open = xml.find('>', q);
    if (open == std::string::npos) {
        return false;
    }
    const size_t close = xml.find("</", open);
    if (close == std::string::npos) {
        return false;
    }
    const size_t closeEnd = xml.find('>', close);
    if (closeEnd == std::string::npos) {
        return false;
    }
    out->start = start;
    out->end = closeEnd + 1;
    out->kind = tag;
    out->value = xml.substr(open + 1, close - (open + 1));
    return true;
}

bool findKeyElement(const std::string& xml, const char* key, KeyElement* out) {
    if (key == nullptr || out == nullptr) {
        return false;
    }
    const std::string needle = std::string("<key>") + key + "</key>";
    const size_t at = xml.find(needle);
    if (at != std::string::npos) {
        return parseValueElement(xml, at + needle.size(), out);
    }
    // THE SPELLING IN THE SETTINGS FILE IS NOT THE SPELLING IN THE PROFILE.
    //
    // The settings file is matched case-INSENSITIVELY (so "msaa" and "MSAA"
    // are the same key, and so that the PS Vita's lower-case names work), and
    // the profile's keys are UPPER CASE.  So a key that is not there in that
    // exact spelling is looked for again, ignoring the case, and the
    // PROFILE's own spelling is what gets written.
    size_t pos = 0;
    while (true) {
        const size_t keyStart = xml.find("<key>", pos);
        if (keyStart == std::string::npos) {
            return false;
        }
        const size_t keyEnd = xml.find("</key>", keyStart);
        if (keyEnd == std::string::npos) {
            return false;
        }
        const std::string name =
            xml.substr(keyStart + 5, keyEnd - (keyStart + 5));
        if (equalsIgnoreCase(name, key)) {
            return parseValueElement(xml, keyEnd + 6, out);
        }
        pos = keyEnd + 6;
    }
}

// Removes ONE byte of whitespace that sits BETWEEN two elements.
//
// The length of the plaintext has to stay the length of the record it lives
// in, and whitespace between elements is the only thing that can give bytes
// back: XML does not care about it, and it is not inside a text value, so
// removing it cannot change what the guest reads.
bool shrinkOneInterElementSpace(std::string& xml) {
    for (size_t i = 0; i + 1 < xml.size(); ++i) {
        if (xml[i] != '>') {
            continue;
        }
        size_t j = i + 1;
        while (j < xml.size() && isSpace(xml[j])) {
            ++j;
        }
        if (j == i + 1) {
            continue;  // no whitespace at all
        }
        if (j >= xml.size() || xml[j] != '<') {
            continue;  // not between two elements
        }
        if (j - (i + 1) < 2) {
            continue;  // keep at least one space, so the XML stays readable
        }
        xml.erase(j - 1, 1);
        return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// THE PATH PREDICATES
// ---------------------------------------------------------------------------

namespace {

// The leaf of a path: everything after the last separator.
const char* leafName(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    const char* base = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }
    return base;
}

}  // namespace

bool guestProfileLeafIsPlist(const char* path) {
    const char* base = leafName(path);
    if (base == nullptr || base[0] == '\0') {
        return false;
    }
    const size_t length = std::strlen(base);
    // ".plist" is SIX bytes, so the slice starts SIX bytes before the end.
    // The offset is length - 5 and the slice is "plist": the predicate is
    // then ALWAYS false, and the settings layer never fires.
    if (length < 6) {
        return false;
    }
    return std::strcmp(base + length - 6, ".plist") == 0;
}

// ---------------------------------------------------------------------------
// THE CIPHER
// ---------------------------------------------------------------------------

void guestProfileKeystream(unsigned char* out, size_t length) {
    if (out == nullptr || length == 0) {
        return;
    }
    // The S box lives at B[2..257] inside a 262-byte frame.
    unsigned char box[262];
    std::memset(box, 0, sizeof(box));
    for (int i = 0; i < 256; ++i) {
        box[2 + i] = static_cast<unsigned char>(i);
    }
    // The key schedule.
    int j = 0;
    for (int i = 0; i < 256; ++i) {
        j = (j + box[2 + i] + kGuestProfileKey[i & 7]) & 0xFF;
        const unsigned char swap = box[2 + i];
        box[2 + i] = box[2 + j];
        box[2 + j] = swap;
    }
    // THE DROP: 133 steps that read the UN-OFFSETED B[i] and write the
    // OFFSETTED B[2+j].  A textbook RC4 has no drop at all, and this is the
    // one instruction that a "close enough" implementation gets wrong.
    j = 0;
    for (int i = 3; i < 3 + 133; ++i) {
        const unsigned char value = box[i];
        j = (j + value) & 0xFF;
        box[i] = box[2 + j];
        box[2 + j] = value;
    }
    // The generator, starting at 0x86.
    int index = 0x86;
    for (size_t n = 0; n < length; ++n) {
        const unsigned char old = box[2 + index];
        j = (j + old) & 0xFF;
        const unsigned char a = box[2 + index];
        box[2 + index] = box[2 + j];
        box[2 + j] = a;
        const unsigned char k =
            static_cast<unsigned char>(box[2 + index] + old);
        out[n] = box[2 + k];
        index = (index + 1) & 0xFF;
    }
}

void guestProfileTransform(unsigned char* data, size_t length) {
    if (data == nullptr || length == 0) {
        return;
    }
    std::vector<unsigned char> stream(length);
    guestProfileKeystream(stream.data(), length);
    for (size_t i = 0; i < length; ++i) {
        data[i] = static_cast<unsigned char>(data[i] ^ stream[i]);
    }
}

bool guestProfileDecrypt(const unsigned char* cipher, size_t length,
                          std::string& out, std::string* error) {
    if (cipher == nullptr || length == 0) {
        if (error != nullptr) {
            *error = "the profile is empty";
        }
        return false;
    }
    std::vector<unsigned char> plain(cipher, cipher + length);
    guestProfileTransform(plain.data(), plain.size());
    out.assign(reinterpret_cast<const char*>(plain.data()), plain.size());
    if (out.compare(0, 5, "<?xml") != 0) {
        if (error != nullptr) {
            *error = "the bytes do not decrypt to a profile";
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// THE SETTINGS FILE
// ---------------------------------------------------------------------------

const char* guestSettingsProfileForTier(const char* tier) {
    if (tier == nullptr || tier[0] == '\0') {
        return "";
    }
    char lowered[32]{};
    copyBounded(lowered, sizeof(lowered), tier);
    toLowerInPlace(lowered);
    // The guest's own capability tiers, by name.  These are the profiles the
    // volume ships: AndroidLow / AndroidMid / AndroidMid2 / AndroidHigh /
    // AndroidXtraHigh / AndroidUltraWide.
    if (std::strcmp(lowered, "bajo") == 0) {
        return "AndroidLow";
    }
    if (std::strcmp(lowered, "medio") == 0) {
        return "AndroidMid";
    }
    if (std::strcmp(lowered, "medioalto") == 0) {
        return "AndroidMid2";
    }
    if (std::strcmp(lowered, "alto") == 0) {
        return "AndroidHigh";
    }
    if (std::strcmp(lowered, "ultrawide") == 0) {
        return "AndroidUltraWide";
    }
    if (std::strcmp(lowered, "xtralto") == 0) {
        return "AndroidXtraHigh";
    }
    // "personal" means "the profile the guest asked for, plus the explicit
    // overrides below", which is exactly what an absent tier means too.
    return "";
}

const char* guestSettingsAliasKey(const char* key) {
    if (key == nullptr) {
        return nullptr;
    }
    const AliasEntry* entry = findAlias(key);
    if (entry == nullptr) {
        return nullptr;
    }
    if (entry->nativeKey[0] == '\0') {
        return nullptr;
    }
    return entry->nativeKey;
}

const char* guestSettingsNativeKey(const char* key) {
    if (key == nullptr) {
        return "";
    }
    const char* alias = guestSettingsAliasKey(key);
    return alias != nullptr ? alias : key;
}

bool guestSettingsLoad(const char* path, GuestSettings& out) {
    out = GuestSettings{};
    if (path == nullptr) {
        return false;
    }
    copyBounded(out.path, sizeof(out.path), path);
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        // An absent file is not an error: it means "no settings".
        return false;
    }
    std::string text;
    char chunk[1024];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        text.append(chunk, got);
    }
    std::fclose(file);

    // A UTF-8 BOM IS NOT A COMMENT.
    //
    // A file written by a Windows editor (or by PowerShell's Out-File) starts
    // with the three bytes EF BB BF, and the first line's "#" then sits
    // behind them: the line looks like a setting, has no "=", and the whole
    // file is reported as broken on its FIRST line.  So the BOM is dropped
    // here, where the file is read, rather than asked of every user.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = text.size();
        }
        const char* begin = text.c_str() + lineStart;
        const char* end = text.c_str() + lineEnd;
        // A comment: the first non-blank character is '#'.
        while (begin < end && isSpace(*begin)) {
            ++begin;
        }
        if (begin < end && *begin != '#') {
            const char* equals = begin;
            while (equals < end && *equals != '=') {
                ++equals;
            }
            if (equals == end) {
                if (out.error[0] == '\0') {
                    copyBounded(out.error, sizeof(out.error),
                               "a line has no '='; it was skipped");
                }
            } else {
                char key[64]{};
                char value[96]{};
                trim(begin, equals, key, sizeof(key));
                trim(equals + 1, end, value, sizeof(value));
                toLowerInPlace(key);
                if (key[0] == '\0') {
                    // nothing to do
                } else if (std::strcmp(key, "calidad") == 0) {
                    copyBounded(out.tier, sizeof(out.tier), value);
                    const char* profile = guestSettingsProfileForTier(value);
                    copyBounded(out.profile, sizeof(out.profile), profile);
                } else if (std::strcmp(key, "perfil") == 0) {
                    // WHICH PROFILE TO PATCH.  The guest asks for one by name,
                    // and the port measures that name to be AndroidHigh.plist;
                    // a settings file may name another one.
                    copyBounded(out.target, sizeof(out.target), value);
                } else if (out.count < kMaxGuestSettings) {
                    copyBounded(out.items[out.count].key,
                                sizeof(out.items[out.count].key), key);
                    copyBounded(out.items[out.count].value,
                                sizeof(out.items[out.count].value), value);
                    ++out.count;
                } else if (out.error[0] == '\0') {
                    copyBounded(out.error, sizeof(out.error),
                               "too many settings; the rest were skipped");
                }
            }
        }
        if (lineEnd >= text.size()) {
            break;
        }
        lineStart = lineEnd + 1;
    }
    out.overrides = out.count;
    out.loaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// THE PROFILE
// ---------------------------------------------------------------------------

int guestProfileParse(const std::string& xml, GuestProfileKey* out, int max) {
    if (out == nullptr || max <= 0) {
        return 0;
    }
    int count = 0;
    size_t pos = 0;
    while (pos < xml.size() && count < max) {
        const size_t keyStart = xml.find("<key>", pos);
        if (keyStart == std::string::npos) {
            break;
        }
        const size_t keyEnd = xml.find("</key>", keyStart);
        if (keyEnd == std::string::npos) {
            break;
        }
        const std::string key =
            xml.substr(keyStart + 5, keyEnd - (keyStart + 5));
        KeyElement element{};
        if (!findKeyElement(xml, key.c_str(), &element)) {
            pos = keyEnd + 6;
            continue;
        }
        copyBounded(out[count].key, sizeof(out[count].key), key);
        copyBounded(out[count].kind, sizeof(out[count].kind), element.kind);
        copyBounded(out[count].value, sizeof(out[count].value), element.value);
        ++count;
        pos = element.end;
    }
    return count;
}

bool guestProfileSetKey(std::string& xml, const char* key, const char* value,
                        std::string* error) {
    KeyElement element{};
    if (!findKeyElement(xml, key, &element)) {
        if (error != nullptr) {
            *error = std::string("the profile has no key '") + key + "'";
        }
        return false;
    }
    std::string replacement;
    if (element.kind == "string") {
        replacement = "<string>" + std::string(value) + "</string>";
    } else if (element.kind == "integer") {
        long parsed = 0;
        if (!parseInteger(value, &parsed)) {
            if (error != nullptr) {
                *error = std::string("'") + value + "' is not an integer";
            }
            return false;
        }
        // The PARSED number, not the text that was typed: the PS Vita writes
        // "4x" for four samples, and "<integer>4x</integer>" is not an
        // integer to anything that reads a plist.
        char number[32]{};
        std::snprintf(number, sizeof(number), "%ld", parsed);
        replacement = std::string("<integer>") + number + "</integer>";
    } else if (element.kind == "real") {
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        (void)parsed;
        if (end == value || (end != nullptr && *end != '\0')) {
            if (error != nullptr) {
                *error = std::string("'") + value + "' is not a number";
            }
            return false;
        }
        replacement = "<real>" + std::string(value) + "</real>";
    } else if (element.kind == "true" || element.kind == "false") {
        bool flag = false;
        if (!parseBoolean(value, &flag)) {
            if (error != nullptr) {
                *error = std::string("'") + value +
                         "' is not a boolean (use si/no or true/false)";
            }
            return false;
        }
        replacement = flag ? "<true/>" : "<false/>";
    } else {
        if (error != nullptr) {
            *error = std::string("the key '") + key + "' has the unknown kind '" +
                     element.kind + "'";
        }
        return false;
    }
    xml.replace(element.start, element.end - element.start, replacement);
    return true;
}

bool guestProfileFitLength(std::string& xml, size_t size, std::string* error) {
    if (xml.size() == size) {
        return true;
    }
    if (xml.size() < size) {
        // A shorter plaintext simply grows trailing whitespace.
        xml.append(size - xml.size(), ' ');
        return true;
    }
    while (xml.size() > size) {
        if (!shrinkOneInterElementSpace(xml)) {
            if (error != nullptr) {
                *error =
                    "the edited profile is longer than the record it lives in, "
                    "and there is no whitespace left to shorten";
            }
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// THE WHOLE TRANSLATION
// ---------------------------------------------------------------------------

bool guestSettingsComposeProfile(const GuestSettings& settings,
                                  const unsigned char* requestedCipher,
                                  size_t requestedLength,
                                  const unsigned char* baseCipher,
                                  size_t baseLength,
                                  std::string& out,
                                  GuestSettingsResult& result) {
    result = GuestSettingsResult{};
    std::string error;
    std::string base;
    if (!guestProfileDecrypt(baseCipher, baseLength, base, &error)) {
        copyBounded(result.error, sizeof(result.error), error.c_str());
        return false;
    }
    std::string requested;
    if (!guestProfileDecrypt(requestedCipher, requestedLength, requested,
                              &error)) {
        copyBounded(result.error, sizeof(result.error), error.c_str());
        return false;
    }

    int changed = 0;
    bool first = true;
    for (int i = 0; i < settings.count; ++i) {
        const char* key = settings.items[i].key;
        const char* value = settings.items[i].value;
        const AliasEntry* alias = findAlias(key);
        if (alias != nullptr && alias->owner[0] != 'p') {
            // A key that belongs to another file (rr3-pad.txt, rr3-drive.txt).
            // It is NOT an error: the caller reports where it belongs.
            continue;
        }
        const char* native = guestSettingsNativeKey(key);
        std::string reason;
        if (!guestProfileSetKey(base, native, value, &reason)) {
            if (first) {
                copyBounded(result.error, sizeof(result.error), reason.c_str());
                first = false;
            }
            continue;
        }
        ++changed;
    }
    if (!first) {
        // At least one setting did not apply, so the profile is NOT what the
        // settings asked for.  The caller keeps the original.
        return false;
    }

    // The length must not change: the ciphertext is the plaintext XORed with
    // a stream of the same length, and the profile sits in a record whose
    // size is fixed.
    if (!guestProfileFitLength(base, requestedLength, &error)) {
        copyBounded(result.error, sizeof(result.error), error.c_str());
        return false;
    }
    out.assign(base.size(), '\0');
    std::memcpy(&out[0], base.data(), base.size());
    guestProfileTransform(reinterpret_cast<unsigned char*>(&out[0]), out.size());
    result.applied = true;
    result.changed = changed;
    // The tier is applied by the CALLER, which is what chooses which profile
    // the bytes come from; the result says whether that happened.
    result.tierApplied = settings.profile[0] != '\0';
    copyBounded(result.profile, sizeof(result.profile),
                settings.profile[0] != '\0' ? settings.profile : "requested");
    return true;
}

}  // namespace rr3
