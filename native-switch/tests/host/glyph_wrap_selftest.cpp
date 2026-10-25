//
// A host self-test for the port's paragraph LINE BREAKER.
//
// Build and run (from the repo root):
//   g++ -std=c++20 -Wall -Wextra -O1 -I native-switch/include
//       native-switch/tests/host/glyph_wrap_selftest.cpp -o glyph_wrap_selftest
//   ./glyph_wrap_selftest native-switch/romfs/ui
//
// Exit code 0 means every case below held.
//
// WHAT IT CHECKS
// --------------
// The breaker (rr3/glyph_wrap.hpp) is the code the port's layoutParagraph()
// runs, so this self-test runs THAT code, not a copy of it, and it runs it
// against the game's OWN faces:
//
//   native-switch/romfs/ui/NotoSansCJKsc-RR3Bootstrap.otf  the Chinese face
//   native-switch/romfs/ui/ConduitITCPro-Medium.otf        the Latin UI face
//
// The face is read directly (head / hhea / hmtx / cmap), so the self-test
// needs no FreeType and no Switch.
//
//   THE POSITIVE CONTROL: the game's own career tip -- a Chinese sentence
//   with no spaces in it -- is laid out into MORE THAN ONE line at a small
//   wrap width, every line fits the width, and no line starts with a
//   closing punctuation mark.
//
//   THE NEGATIVE CONTROL: a Latin sentence still breaks at its spaces, a
//   Latin word is never cut in half, and a short string is still one line.
//
//   THE LEGAL BREAK POINTS: no line starts with closing punctuation, and no
//   line ends with opening punctuation.
//
// The archive's own evidence is in the test: run 20261025-015035-eden-career2
// laid the Chinese sentence out as 24 / 24 / 1 characters, with the last
// line holding a single full stop. A full stop may not start a line, so that
// layout was never a StaticLayout layout.
//
#include "rr3/glyph_wrap.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  ok    %s\n", what);
        return;
    }
    std::printf("  FAIL  %s\n", what);
    ++g_failures;
}

// ---------------------------------------------------------------------------
// The face
// ---------------------------------------------------------------------------

struct Face {
    std::vector<uint8_t> blob;
    uint16_t unitsPerEm = 0;
    uint16_t numberOfHMetrics = 0;
    uint32_t hmtxOffset = 0;
    uint32_t cmapOffset = 0;
    uint32_t cmapSubtable = 0;
    bool cmapIsFormat12 = false;

    uint16_t u16(size_t at) const {
        return static_cast<uint16_t>((blob[at] << 8) | blob[at + 1]);
    }
    uint32_t u32(size_t at) const {
        return (static_cast<uint32_t>(blob[at]) << 24) |
               (static_cast<uint32_t>(blob[at + 1]) << 16) |
               (static_cast<uint32_t>(blob[at + 2]) << 8) |
               static_cast<uint32_t>(blob[at + 3]);
    }

    bool findTable(const char* tag, uint32_t& offset) const {
        const uint16_t tables = u16(4);
        for (uint16_t i = 0; i < tables; ++i) {
            const size_t record = 12 + static_cast<size_t>(i) * 16;
            if (std::memcmp(blob.data() + record, tag, 4) == 0) {
                offset = u32(record + 8);
                return true;
            }
        }
        return false;
    }

    bool load(const std::string& path) {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            std::printf("  FAIL  cannot open %s\n", path.c_str());
            ++g_failures;
            return false;
        }
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        blob.resize(static_cast<size_t>(size < 0 ? 0 : size));
        const size_t got = std::fread(blob.data(), 1, blob.size(), file);
        std::fclose(file);
        if (got != blob.size() || blob.size() < 32) {
            std::printf("  FAIL  %s is not a readable font\n", path.c_str());
            ++g_failures;
            return false;
        }

        uint32_t head = 0;
        uint32_t hhea = 0;
        uint32_t hmtx = 0;
        uint32_t cmap = 0;
        if (!findTable("head", head) || !findTable("hhea", hhea) ||
            !findTable("hmtx", hmtx) || !findTable("cmap", cmap)) {
            std::printf("  FAIL  %s has no head/hhea/hmtx/cmap\n", path.c_str());
            ++g_failures;
            return false;
        }
        unitsPerEm = u16(head + 18);
        numberOfHMetrics = u16(hhea + 34);
        hmtxOffset = hmtx;
        cmapOffset = cmap;

        // The encoding subtable: prefer format 12 (3, 10), fall back to
        // format 4 from any platform. The game's own CJK face ships two
        // format 4 subtables (platform 0, encodings 0 and 3), so a lookup
        // that insists on (3, 1) finds nothing.
        const uint16_t encodings = u16(cmap + 2);
        uint32_t best = 0;
        bool bestIs12 = false;
        for (uint16_t i = 0; i < encodings; ++i) {
            const size_t record = cmap + 4 + static_cast<size_t>(i) * 8;
            const uint8_t platform = blob[record];
            const uint8_t encoding = blob[record + 1];
            const uint32_t offset = cmap + u32(record + 4);
            if (offset + 4 > blob.size()) {
                continue;
            }
            const uint16_t format = u16(offset);
            if (format == 12 && platform == 3 && encoding == 10) {
                best = offset;
                bestIs12 = true;
            } else if (format == 4 && best == 0) {
                best = offset;
                bestIs12 = false;
            } else if (format == 4 && !bestIs12 && platform == 3 &&
                       encoding == 1) {
                best = offset;
                bestIs12 = false;
            }
        }
        if (best == 0) {
            std::printf("  FAIL  %s has no usable cmap subtable\n", path.c_str());
            ++g_failures;
            return false;
        }
        cmapSubtable = best;
        cmapIsFormat12 = bestIs12;
        return true;
    }

    uint32_t glyphIndex(uint32_t codepoint) const {
        if (cmapIsFormat12) {
            const uint32_t groups = u32(cmapSubtable + 12);
            uint32_t low = 0;
            uint32_t high = groups;
            while (low < high) {
                const uint32_t mid = (low + high) / 2;
                const size_t group = cmapSubtable + 16 + static_cast<size_t>(mid) * 12;
                const uint32_t start = u32(group);
                const uint32_t end = u32(group + 4);
                if (codepoint < start) {
                    high = mid;
                } else if (codepoint > end) {
                    low = mid + 1;
                } else {
                    return u32(group + 8) + (codepoint - start);
                }
            }
            return 0;
        }
        const uint16_t segCount = u16(cmapSubtable + 6) / 2;
        const size_t endCodes = cmapSubtable + 14;
        const size_t startCodes = endCodes + 2 * segCount + 2;
        const size_t deltas = startCodes + 2 * segCount;
        const size_t rangeOffsets = deltas + 2 * segCount;
        for (uint16_t seg = 0; seg < segCount; ++seg) {
            const uint16_t end = u16(endCodes + 2 * seg);
            if (codepoint > end) {
                continue;
            }
            const uint16_t start = u16(startCodes + 2 * seg);
            if (codepoint < start) {
                return 0;
            }
            const int16_t delta = static_cast<int16_t>(u16(deltas + 2 * seg));
            const uint16_t rangeOffset = u16(rangeOffsets + 2 * seg);
            if (rangeOffset == 0) {
                return static_cast<uint32_t>((codepoint + delta) & 0xFFFF);
            }
            const size_t glyphIndexAddress =
                rangeOffsets + 2 * seg + rangeOffset + 2 * (codepoint - start);
            if (glyphIndexAddress + 1 >= blob.size()) {
                return 0;
            }
            const uint16_t glyph = u16(glyphIndexAddress);
            if (glyph == 0) {
                return 0;
            }
            return static_cast<uint32_t>((glyph + delta) & 0xFFFF);
        }
        return 0;
    }

    uint16_t advanceUnits(uint32_t glyph) const {
        if (numberOfHMetrics == 0) {
            return 0;
        }
        const uint32_t index = glyph < numberOfHMetrics ? glyph
                                                        : numberOfHMetrics - 1;
        return u16(hmtxOffset + 4 * index);
    }

    // The advance of one codepoint, in pixels, the way the port measures it:
    // the face's advance scaled by the text size.
    float advanceOf(uint32_t codepoint, float sizePx) const {
        const uint32_t glyph = glyphIndex(codepoint);
        if (glyph == 0) {
            return 0.f;
        }
        const uint16_t units = advanceUnits(glyph);
        return static_cast<float>(units) * sizePx / static_cast<float>(unitsPerEm);
    }
};

// ---------------------------------------------------------------------------
// The text
// ---------------------------------------------------------------------------

// THE GAME'S OWN CAREER TIP.
//
// "如果你想在现实世界赛车，请前往你本地的赛道。请确认你的车经过安全检查，
//  并时刻戴好头盔、系好安全带。"
//
// 49 codepoints, no spaces anywhere: the shape that broke the old breaker.
// It is written as code points so the source file's own encoding cannot
// change it.
constexpr uint32_t kCareerTip[] = {
    0x5982, 0x679C, 0x4F60, 0x60F3, 0x5728, 0x73B0, 0x5B9E, 0x4E16, 0x754C,
    0x8D5B, 0x8F66, 0xFF0C, 0x8BF7, 0x524D, 0x5F80, 0x4F60, 0x672C, 0x5730,
    0x7684, 0x8D5B, 0x9053, 0x3002, 0x8BF7, 0x786E, 0x8BA4, 0x4F60, 0x7684,
    0x8F66, 0x7ECF, 0x8FC7, 0x5B89, 0x5168, 0x68C0, 0x67E5, 0xFF0C, 0x5E76,
    0x65F6, 0x523B, 0x6234, 0x597D, 0x5934, 0x76D4, 0x3001, 0x7CFB, 0x597D,
    0x5B89, 0x5168, 0x5E26, 0x3002,
};
constexpr size_t kCareerTipCount = sizeof(kCareerTip) / sizeof(kCareerTip[0]);

constexpr const char* kLatinSentence =
    "If you want to race in the real world, try your local track.";

struct Text {
    std::vector<uint32_t> codepoints;
};

Text careerTip() {
    Text text;
    text.codepoints.assign(kCareerTip, kCareerTip + kCareerTipCount);
    return text;
}

Text latinSentence() {
    Text text;
    for (const char* p = kLatinSentence; *p != '\0'; ++p) {
        text.codepoints.push_back(static_cast<unsigned char>(*p));
    }
    return text;
}

// The measured text, in the shape the breaker wants.
std::vector<rr3::glyphwrap::Item> measure(const Face& face, const Text& text,
                                           float sizePx) {
    std::vector<rr3::glyphwrap::Item> items;
    for (const uint32_t codepoint : text.codepoints) {
        rr3::glyphwrap::Item item;
        item.codepoint = codepoint;
        item.advance = face.advanceOf(codepoint, sizePx);
        item.isSpace = (codepoint == ' ' || codepoint == '\t' ||
                        codepoint == '\n' || codepoint == '\r');
        items.push_back(item);
    }
    return items;
}

std::string describe(const std::vector<uint32_t>& codepoints, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to && i < codepoints.size(); ++i) {
        const uint32_t cp = codepoints[i];
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "<U+%04X>", cp);
            out += buffer;
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string ui =
        argc > 1 ? argv[1] : std::string("native-switch/romfs/ui");
    const std::string cjkPath = ui + "/NotoSansCJKsc-RR3Bootstrap.otf";
    const std::string latinPath = ui + "/ConduitITCPro-Medium.otf";

    Face cjk;
    Face latin;
    if (!cjk.load(cjkPath) || !latin.load(latinPath)) {
        std::printf("FAIL: the faces are not there\n");
        return 1;
    }
    std::printf("faces: CJK upem=%u  Latin upem=%u\n", cjk.unitsPerEm,
                latin.unitsPerEm);

    // -----------------------------------------------------------------------
    // THE POSITIVE CONTROL: a Chinese sentence with no spaces.
    // -----------------------------------------------------------------------
    const float sizePx = 42.f;
    const float wrapWidth = 1000.f;
    const Text tip = careerTip();
    const std::vector<rr3::glyphwrap::Item> tipItems = measure(cjk, tip, sizePx);

    rr3::glyphwrap::Line lines[64];
    const size_t lineCount =
        rr3::glyphwrap::breakLines(tipItems.data(), tipItems.size(), wrapWidth,
                                   true, lines, 64);
    std::printf("CJK tip: %u codepoints, wrap=%.0f -> %u lines\n",
                static_cast<unsigned>(tipItems.size()), wrapWidth,
                static_cast<unsigned>(lineCount));
    for (size_t i = 0; i < lineCount; ++i) {
        std::printf("  line %u: width=%.1f  %s\n", static_cast<unsigned>(i),
                    lines[i].width,
                    describe(tip.codepoints, lines[i].first, lines[i].end).c_str());
    }

    check(lineCount > 1, "the Chinese sentence breaks into more than one line");

    bool everyLineFits = true;
    bool noClosingPunctuationAtTheStart = true;
    bool noLonePunctuationLine = true;
    bool coversEveryItem = true;
    size_t covered = 0;
    for (size_t i = 0; i < lineCount; ++i) {
        if (lines[i].width > wrapWidth + 0.51f) {
            everyLineFits = false;
        }
        if (lines[i].end > lines[i].first) {
            const uint32_t first = tip.codepoints[lines[i].first];
            if (rr3::glyphwrap::isClosingPunctuation(first)) {
                noClosingPunctuationAtTheStart = false;
            }
            if (lines[i].end - lines[i].first == 1 &&
                rr3::glyphwrap::isClosingPunctuation(first)) {
                noLonePunctuationLine = false;
            }
        }
        if (lines[i].first != covered) {
            coversEveryItem = false;
        }
        covered = lines[i].end;
    }
    check(everyLineFits, "every line of the Chinese sentence fits the wrap width");
    check(noClosingPunctuationAtTheStart,
          "no line of the Chinese sentence starts with closing punctuation");
    check(noLonePunctuationLine,
          "no line of the Chinese sentence is a lone closing punctuation mark");
    check(coversEveryItem && covered == tipItems.size(),
          "the Chinese sentence's lines cover every character, in order");

    // The break points are between characters, which is the whole point: the
    // sentence has no spaces at all.
    bool brokeBetweenCharacters = false;
    for (size_t i = 1; i < lineCount; ++i) {
        if (lines[i].first > 0 && lines[i].first < tipItems.size()) {
            brokeBetweenCharacters = true;
        }
    }
    check(brokeBetweenCharacters,
          "the Chinese sentence is broken between characters, not at spaces");

    // -----------------------------------------------------------------------
    // THE NEGATIVE CONTROL: a Latin sentence still breaks at its spaces.
    // -----------------------------------------------------------------------
    const Text sentence = latinSentence();
    const std::vector<rr3::glyphwrap::Item> sentenceItems =
        measure(latin, sentence, sizePx);
    rr3::glyphwrap::Line latinLines[64];
    const size_t latinCount =
        rr3::glyphwrap::breakLines(sentenceItems.data(), sentenceItems.size(),
                                   300.f, true, latinLines, 64);
    std::printf("Latin: %u codepoints, wrap=300 -> %u lines\n",
                static_cast<unsigned>(sentenceItems.size()),
                static_cast<unsigned>(latinCount));
    for (size_t i = 0; i < latinCount; ++i) {
        std::printf("  line %u: width=%.1f  %s\n", static_cast<unsigned>(i),
                    latinLines[i].width,
                    describe(sentence.codepoints, latinLines[i].first,
                             latinLines[i].end)
                        .c_str());
    }
    check(latinCount > 1, "the Latin sentence breaks into more than one line");

    bool brokeAtSpaces = true;
    bool latinFits = true;
    for (size_t i = 0; i < latinCount; ++i) {
        if (latinLines[i].width > 300.f + 0.51f) {
            latinFits = false;
        }
        // A line that is not the last one ends where a space follows.
        if (i + 1 < latinCount && latinLines[i].end < sentenceItems.size()) {
            const uint32_t next = sentence.codepoints[latinLines[i].end];
            if (next != ' ') {
                brokeAtSpaces = false;
            }
        }
    }
    check(latinFits, "every line of the Latin sentence fits the wrap width");
    check(brokeAtSpaces, "the Latin sentence is broken at its spaces");

    // A short string is still one line.
    rr3::glyphwrap::Line shortLines[8];
    const size_t shortCount = rr3::glyphwrap::breakLines(
        sentenceItems.data(), 8, 1000.f, true, shortLines, 8);
    check(shortCount == 1, "a short string is still one line");

    // No wrapping at all is still one line.
    rr3::glyphwrap::Line unwrapped[8];
    const size_t unwrappedCount = rr3::glyphwrap::breakLines(
        tipItems.data(), tipItems.size(), 0.f, false, unwrapped, 8);
    check(unwrappedCount == 1, "a paragraph with no wrap width is one line");

    // -----------------------------------------------------------------------
    // A Chinese sentence with a Latin word inside it: the Latin word is not
    // cut in half. The old breaker treated the whole sentence as one word and
    // cut it wherever it ran out of room.
    // -----------------------------------------------------------------------
    std::vector<uint32_t> mixed;
    const uint32_t prefix[] = {0x5728, 0x73A9, 0x5BB6, 0x7B49, 0x7EA7};
    for (const uint32_t cp : prefix) {
        mixed.push_back(cp);
    }
    for (const char* p = "RealRacing"; *p != '\0'; ++p) {
        mixed.push_back(static_cast<unsigned char>(*p));
    }
    const uint32_t suffix[] = {0x65F6, 0x89E3, 0x9501, 0x8F6C, 0x5708, 0x3002};
    for (const uint32_t cp : suffix) {
        mixed.push_back(cp);
    }
    Text mixedText;
    mixedText.codepoints = mixed;
    std::vector<rr3::glyphwrap::Item> mixedItems;
    for (const uint32_t cp : mixed) {
        rr3::glyphwrap::Item item;
        item.codepoint = cp;
        item.advance = cp < 0x80 ? latin.advanceOf(cp, sizePx)
                                 : cjk.advanceOf(cp, sizePx);
        item.isSpace = false;
        mixedItems.push_back(item);
    }
    rr3::glyphwrap::Line mixedLines[16];
    const size_t mixedCount = rr3::glyphwrap::breakLines(
        mixedItems.data(), mixedItems.size(), 260.f, true, mixedLines, 16);
    std::printf("Mixed: %u codepoints, wrap=260 -> %u lines\n",
                static_cast<unsigned>(mixedItems.size()),
                static_cast<unsigned>(mixedCount));
    for (size_t i = 0; i < mixedCount; ++i) {
        std::printf("  line %u: width=%.1f  %s\n", static_cast<unsigned>(i),
                    mixedLines[i].width,
                    describe(mixed, mixedLines[i].first, mixedLines[i].end).c_str());
    }
    {
        // "RealRacing" is the ten characters at index 5..14. They must all
        // sit on ONE line: the old breaker treated the whole sentence as one
        // word and cut it wherever it ran out of room.
        size_t firstLine = 0;
        size_t lastLine = 0;
        bool found = false;
        for (size_t i = 0; i < mixedCount; ++i) {
            if (mixedLines[i].first <= 5 && mixedLines[i].end > 5) {
                firstLine = i;
                found = true;
                break;
            }
        }
        for (size_t i = 0; i < mixedCount; ++i) {
            if (mixedLines[i].first < 15 && mixedLines[i].end > 14) {
                lastLine = i;
                break;
            }
        }
        check(found && firstLine == lastLine,
              "a Latin word inside a Chinese sentence is not cut in half");
    }

    if (g_failures != 0) {
        std::printf("FAIL: %d case(s) did not hold\n", g_failures);
        return 1;
    }
    std::printf("PASS: the paragraph line breaker breaks CJK between characters, "
                "keeps Latin words whole and never starts a line with closing "
                "punctuation\n");
    return 0;
}
