#pragma once

/*
 * rr3/glyph_wrap.hpp -- the line breaker the port uses for
 * android.text.StaticLayout.
 *
 * WHY THIS IS A HEADER AND NOT A PIECE OF glyph_vector.cpp
 * ---------------------------------------------------------
 * The breaker is the part of the paragraph layout that can be WRONG in a way
 * a screenshot cannot show: a line that starts with a closing bracket, a
 * Latin word cut in half, a CJK sentence that never breaks. It is also the
 * part that can be checked on the HOST, against the game's own font, with
 * no guest and no Switch. So it lives here, free of FreeType, of libnx and
 * of the port's own headers, and both the port and
 * native-switch/tests/host/glyph_wrap_selftest.cpp compile the same code.
 *
 * WHAT ANDROID ACTUALLY DOES
 * --------------------------
 * StaticLayout asks android.text.Layout for its line breaks, and on API 29
 * (the guest's @TargetApi(29) on GlyphVector.initWithParagraph) that is
 * Minikin's LineBreaker, which asks ICU's line BreakIterator. ICU
 * implements the Unicode text segmentation algorithm, UAX #14, so:
 *
 *   * a run of Han, kana or Hangul breaks BETWEEN CHARACTERS, because those
 *     scripts do not use spaces and their characters carry the ideographic
 *     line-break class;
 *   * a Latin word does NOT break inside (LB28);
 *   * a line may not START with closing punctuation (LB13: the "x CL" rule)
 *     and may not END with opening punctuation (LB14: the "OP x" rule);
 *   * a letter and an ideograph are not separated (LB30a/LB30b).
 *
 * The port's old breaker knew only two kinds of break: at a space, and
 * between the characters of a word that is wider than the whole paragraph.
 * For a Chinese sentence -- one run, no spaces -- that is a single "word",
 * and it was broken between characters only when the WHOLE sentence was
 * wider than the wrap width. The result was a layout that no StaticLayout
 * would produce: a line that starts with a full stop, and a Latin word cut
 * in half in the middle of a Chinese sentence.
 *
 * THE RULES THIS HEADER IMPLEMENTS
 * ---------------------------------
 * The four UAX #14 line-break classes the breaker needs, reduced from the
 * full table:
 *
 *   Space        SP  -- the breaker never breaks BEFORE a space and always
 *                       breaks AFTER one.
 *   Ideographic  ID  -- Han, kana, Hangul and the CJK compatibility blocks.
 *   Open         OP  -- an opening bracket or quote. A line may not end
 *                       with one.
 *   Close    CL / CP  -- a closing bracket, a full stop, a comma. A line
 *                       may not start with one.
 *   Latin         AL  -- letters, digits, and everything else.
 *
 * The ranges below are the Unicode blocks those classes cover. They are
 * deliberately generous: a codepoint that is misfiled as Latin only costs a
 * missing break opportunity, while a codepoint that is misfiled as
 * ideographic would break a word that must stay whole.
 */

#include <stddef.h>
#include <stdint.h>

namespace rr3 {
namespace glyphwrap {

// ---------------------------------------------------------------------------
// The character classes
// ---------------------------------------------------------------------------

enum class CharClass : uint8_t {
    Space = 0,
    Ideographic,
    Open,
    Close,
    Latin,
};

// THE HAN, KANA AND HANGUL BLOCKS.
//
// UAX #14 calls these "ideographic" for line-breaking purposes (classes ID,
// CJ, H2, H3, JL, JV and JT), and they are exactly the scripts that have
// no spaces and therefore need to break between characters.
inline bool isIdeographic(uint32_t cp) {
    return (cp >= 0x1100u && cp <= 0x11FFu) ||   // Hangul Jamo
           (cp >= 0x2E80u && cp <= 0x2EFFu) ||   // CJK radicals supplement
           (cp >= 0x3005u && cp <= 0x3007u) ||   // ideographic iteration marks
           (cp >= 0x3021u && cp <= 0x3029u) ||   // Hangul tone marks (CJ)
           (cp >= 0x3031u && cp <= 0x3035u) ||
           (cp >= 0x303Bu && cp <= 0x303Cu) ||
           (cp >= 0x3041u && cp <= 0x309Fu) ||   // Hiragana
           (cp >= 0x30A0u && cp <= 0x30FFu) ||   // Katakana
           (cp >= 0x3105u && cp <= 0x312Fu) ||   // Bopomofo
           (cp >= 0x3131u && cp <= 0x318Eu) ||   // Hangul compatibility Jamo
           (cp >= 0x3190u && cp <= 0x319Fu) ||
           (cp >= 0x31A0u && cp <= 0x31BFu) ||   // Bopomofo extended
           (cp >= 0x31C0u && cp <= 0x31EFu) ||   // CJK strokes
           (cp >= 0x31F0u && cp <= 0x31FFu) ||   // Katakana phonetic extensions
           (cp >= 0x3200u && cp <= 0x4DBFu) ||   // enclosed CJK .. CJK ext. A
           (cp >= 0x4E00u && cp <= 0x9FFFu) ||   // CJK unified ideographs
           (cp >= 0xA000u && cp <= 0xA4CFu) ||   // Yi
           (cp >= 0xA960u && cp <= 0xA97Fu) ||   // Hangul Jamo extended-A
           (cp >= 0xAC00u && cp <= 0xD7A3u) ||   // Hangul syllables
           (cp >= 0xF900u && cp <= 0xFAFFu) ||   // CJK compatibility ideographs
           (cp >= 0xFE10u && cp <= 0xFE19u) ||   // vertical forms
           (cp >= 0xFE30u && cp <= 0xFE6Fu) ||   // CJK compatibility forms
           (cp >= 0x20000u && cp <= 0x2FA1Fu);   // CJK ext. B .. F
}

// THE CLOSING PUNCTUATION.
//
// A line may not START with one of these (UAX #14 LB13: never break before
// CL or CP). The set is the ASCII brackets and sentence punctuation plus
// the full-width and CJK forms the game's Chinese text uses.
//
// The full stop and the comma of a Chinese sentence are here, which is the
// whole point: the old breaker put one of them alone on a line.
inline bool isClosingPunctuation(uint32_t cp) {
    switch (cp) {
        case 0x0021:  // !
        case 0x0029:  // )
        case 0x002C:  // ,
        case 0x002E:  // .
        case 0x003A:  // :
        case 0x003B:  // ;
        case 0x003F:  // ?
        case 0x005D:  // ]
        case 0x007D:  // }
        case 0x2026:  // ... (horizontal ellipsis)
        case 0x3001:  // ideographic comma
        case 0x3002:  // ideographic full stop
        case 0x3009:  // right angle bracket
        case 0x300B:  // right double angle bracket
        case 0x300D:  // right corner bracket
        case 0x300F:  // right white corner bracket
        case 0x3011:  // right black lenticular bracket
        case 0x3015:  // right tortoise shell bracket
        case 0x3017:  // right white lenticular bracket
        case 0x3019:  // right tortoise shell bracket
        case 0x301B:  // right white lenticular bracket
        case 0xFF01:  // fullwidth !
        case 0xFF09:  // fullwidth )
        case 0xFF0C:  // fullwidth ,
        case 0xFF0E:  // fullwidth .
        case 0xFF1A:  // fullwidth :
        case 0xFF1B:  // fullwidth ;
        case 0xFF1F:  // fullwidth ?
        case 0xFF3D:  // fullwidth }
        case 0xFF5D:  // fullwidth }
        case 0xFF60:  // fullwidth ideographic full stop
            return true;
        default:
            return false;
    }
}

// THE OPENING PUNCTUATION.
//
// A line may not END with one of these (UAX #14 LB14: never break after an
// opening bracket or quote).
inline bool isOpeningPunctuation(uint32_t cp) {
    switch (cp) {
        case 0x0028:  // (
        case 0x005B:  // [
        case 0x007B:  // {
        case 0x00AB:  // left-pointing double angle quotation mark
        case 0x2018:  // left single quotation mark
        case 0x201C:  // left double quotation mark
        case 0x201E:  // double low-9 quotation mark
        case 0x3008:  // left angle bracket
        case 0x300A:  // left double angle bracket
        case 0x300E:  // left corner bracket
        case 0x3010:  // left black lenticular bracket
        case 0x3014:  // left tortoise shell bracket
        case 0x3016:  // left white lenticular bracket
        case 0x3018:  // left tortoise shell bracket
        case 0x301A:  // left white lenticular bracket
        case 0xFF08:  // fullwidth (
        case 0xFF3B:  // fullwidth {
        case 0xFF5B:  // fullwidth [
            return true;
        default:
            return false;
    }
}

inline CharClass classify(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
        return CharClass::Space;
    }
    if (isOpeningPunctuation(cp)) {
        return CharClass::Open;
    }
    if (isClosingPunctuation(cp)) {
        return CharClass::Close;
    }
    if (isIdeographic(cp)) {
        return CharClass::Ideographic;
    }
    return CharClass::Latin;
}

// ---------------------------------------------------------------------------
// The break opportunities
// ---------------------------------------------------------------------------

// MAY A LINE START AT `next`, GIVEN THAT `prev` ENDS THE PREVIOUS ONE?
//
// The rules, in the order UAX #14 applies them:
//
//   LB13   never break BEFORE a closing punctuation  -> no line starts with
//          a full stop, a comma or a closing bracket;
//   LB14   never break AFTER an opening punctuation  -> no line ends with an
//          opening bracket;
//   LB18   break after a space;
//   LB7    never break before a space;
//   LB31   break between two ideographs (this is the CJK case);
//   LB30a  no break between a letter and an ideograph;
//   LB30b  no break between an ideograph and a letter;
//   LB28   no break inside a run of letters.
inline bool canBreakBetween(uint32_t prev, uint32_t next) {
    const CharClass p = classify(prev);
    const CharClass n = classify(next);
    if (n == CharClass::Close) {
        return false;  // LB13
    }
    if (p == CharClass::Open) {
        return false;  // LB14
    }
    if (p == CharClass::Space) {
        return true;  // LB18
    }
    if (n == CharClass::Space) {
        return false;  // LB7
    }
    if (p == CharClass::Ideographic && n == CharClass::Ideographic) {
        return true;  // LB31: the CJK run breaks between its characters
    }
    if (p == CharClass::Ideographic || n == CharClass::Ideographic) {
        return false;  // LB30a / LB30b
    }
    if (p == CharClass::Latin && n == CharClass::Latin) {
        return false;  // LB28
    }
    return true;  // LB31
}

// ---------------------------------------------------------------------------
// The greedy fill
// ---------------------------------------------------------------------------

// One item of the text: a codepoint and the width it advances the pen by.
struct Item {
    uint32_t codepoint;
    float advance;
    bool isSpace;
};

// One line of the result.
struct Line {
    size_t first;   // index of the first item on the line
    size_t end;     // index just past the last item on the line. The spaces
                    // that trail the line are NOT part of it: Android does
                    // not draw the space that ends a line either, and the
                    // space belongs to no line rather than to the next one.
    float width;    // the advance width of the line, trailing spaces excluded
};

// THE BREAKER.
//
// `items` is the measured text: one item per codepoint, in order. `wrap` is
// false when the paragraph has no width limit, and then the whole text is
// one line.
//
// The fill is greedy, the way StaticLayout's is: items are added to the
// line until the next one does not fit, and the line is broken there. A
// trailing space does not count towards the line's width, the way Android's
// does not.
//
// A unit -- the run of items a line may not be broken inside -- is a Latin
// word, or ONE ideographic character, with an opening punctuation mark
// glued to the unit after it and a closing punctuation mark glued to the
// unit before it. A unit that is still wider than the wrap width is broken
// between its characters, because a line has to hold something.
//
// Returns how many lines were produced.
inline size_t breakLines(const Item* items, size_t itemCount, float wrapWidth,
                          bool wrap, Line* out, size_t maxLines) {
    if (items == nullptr || out == nullptr || maxLines == 0) {
        return 0;
    }
    if (itemCount == 0) {
        // An empty paragraph is still ONE line, the way StaticLayout counts
        // it: the guest reads the line count back and draws a line box.
        out[0].first = 0;
        out[0].end = 0;
        out[0].width = 0.f;
        return 1;
    }

    // The whole text on one line: no width limit, or nothing to break.
    if (!wrap || wrapWidth <= 0.f) {
        size_t last = itemCount;
        while (last > 0 && items[last - 1].isSpace) {
            --last;
        }
        float width = 0.f;
        for (size_t i = 0; i < last; ++i) {
            width += items[i].advance;
        }
        out[0].first = 0;
        out[0].end = last;
        out[0].width = width;
        return 1;
    }

    size_t lineCount = 0;
    size_t lineFirst = 0;       // index of the first item on the line
    size_t contentEnd = 0;      // index just past the last item of the line
    float lineWidth = 0.f;      // the line's width, trailing spaces excluded
    float pendingSpace = 0.f;   // the spaces at the end of the line
    size_t i = 0;

    auto emit = [&](size_t end, float width) {
        if (lineCount >= maxLines) {
            return;
        }
        out[lineCount].first = lineFirst;
        out[lineCount].end = end;
        out[lineCount].width = width;
        ++lineCount;
    };

    while (i < itemCount) {
        if (items[i].isSpace) {
            // A hard break ends the line where it is.
            if (items[i].codepoint == '\n' || items[i].codepoint == '\r') {
                emit(contentEnd, lineWidth);
                lineFirst = i + 1;
                contentEnd = i + 1;
                lineWidth = 0.f;
                pendingSpace = 0.f;
                ++i;
                continue;
            }
            pendingSpace += items[i].advance;
            ++i;
            continue;
        }

        // THE UNIT: the run of items a line may not be broken inside.
        const size_t unitStart = i;
        float unitWidth = 0.f;
        size_t unitEnd = i;
        while (unitEnd < itemCount && !items[unitEnd].isSpace) {
            unitWidth += items[unitEnd].advance;
            ++unitEnd;
            if (unitEnd < itemCount && !items[unitEnd].isSpace &&
                canBreakBetween(items[unitEnd - 1].codepoint,
                                items[unitEnd].codepoint)) {
                break;
            }
        }

        if (lineWidth > 0.f && lineWidth + pendingSpace + unitWidth > wrapWidth) {
            // The unit does not fit on this line: break before it.
            emit(contentEnd, lineWidth);
            lineFirst = unitStart;
            contentEnd = unitStart;
            lineWidth = 0.f;
            pendingSpace = 0.f;
        }

        if (lineWidth == 0.f && unitWidth > wrapWidth) {
            // The unit does not fit on a line of its own either. StaticLayout
            // breaks it between its characters rather than let it run off
            // the paragraph. The break never lands in front of a closing
            // punctuation mark: that mark belongs to the character before it.
            float chunk = 0.f;
            size_t chunkStart = unitStart;
            for (size_t k = unitStart; k < unitEnd; ++k) {
                if (chunk > 0.f && chunk + items[k].advance > wrapWidth &&
                    !isClosingPunctuation(items[k].codepoint)) {
                    emit(k, chunk);
                    lineFirst = chunkStart;
                    chunkStart = k;
                    chunk = 0.f;
                }
                chunk += items[k].advance;
            }
            lineFirst = chunkStart;
            contentEnd = unitEnd;
            lineWidth = chunk;
            pendingSpace = 0.f;
            i = unitEnd;
            continue;
        }

        lineWidth += pendingSpace + unitWidth;
        contentEnd = unitEnd;
        pendingSpace = 0.f;
        i = unitEnd;
    }

    if (contentEnd > lineFirst || lineCount == 0) {
        // The last line.
        emit(contentEnd, lineWidth);
    }
    return lineCount;
}

}  // namespace glyphwrap
}  // namespace rr3
