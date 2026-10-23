#pragma once

// ---------------------------------------------------------------------------
// THE ONE CANDIDATE ORDER FOR THE PORT'S ASSETS.
//
// The port TERMINATES EA's Cloudcell asset-download protocol: it answers
// assetManDownloadPackHash.php with bytes taken off the card and off the
// volumes. So for every asset the guest downloads, the port decides WHICH
// bytes the guest gets, and the guest then MD5s what it received and compares
// it with the md5 the manifest published for that path.
//
// That makes the candidate order a SHARED CONTRACT and not a detail of one
// translation unit. The md5 a manifest publishes is the digest of the bytes
// the endpoint sends; two copies of the order in two files can only ever
// agree by accident, and when they disagree the guest md5s one revision and
// renders another.
//
// ONE HEADER, TWO USERS:
//   native-switch/source/jni_compat/classes/cc_http_request.cpp -- the endpoint
//   native-switch/source/guest_filesystem.cpp                  -- the manifests
//
// THE RULES, in the order they are tried:
//
//   1. the guest's OWN depot on the card, under the same relative name:
//      ".depot" + path
//   2. the volume tree with the same relative name: "files/.depot" + path
//   3. the gui_assets tree, for the server's "/assets_<res>/" spelling:
//      "files/.depot/gui_assets/<res>" + rest
//   4. the card's installed tree, for the guest's "/gui_assets/<res>/" spelling:
//      ".depot/assets_<res>" + rest
//   5. the guest's own download cache, "<path>.ast", for BOTH spellings --
//      DIGEST-GATED, so a leftover of an older revision is never the answer
//   6. the nearest LOWER resolution (2048x1536 -> 960x640 -> 480x320), which
//      is the direction the port's runtime path space already falls back in
//
// WHY RULE 6 IS SAFE. The alias answers the request from a DIFFERENT
// resolution of the same asset, so the manifest has to publish the digest of
// the ALIASED file's bytes under the REQUESTED path -- not the digest of a
// file at the requested resolution that does not exist. The manifest side
// takes the digest from the very candidate the endpoint would send, so the
// two agree by construction and not by two derivations that happen to agree
// today.
//
// AND ONE SELECTION RULE, shared as well: see selectCandidate() below.
// ---------------------------------------------------------------------------

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace rr3 {
namespace asset {

// ONE CANDIDATE: a local file that can back a requested path.
struct Candidate {
    // The local path, in the form the port's own resolver takes it: relative
    // to the card's app root (".depot/...") or to the volume tree
    // ("files/.depot/...").
    char path[384];
    // The resolution this candidate was taken from. Empty when the request has
    // no resolution segment. It differs from the requested one ONLY for the
    // nearest lower resolution, and that is what the instrument reports as
    // the alias.
    char resolution[32];
    // TRUE for the guest's own download cache ("<asset>.ast"). Such a file
    // counts only when its digest IS the digest the guest asked for.
    bool digestMustMatch;
};

// TRUE for a resolution segment: "<digits>x<digits>", optionally followed by
// a separator. "960x640" and "2048x1536" are resolutions; "assets_something"
// is not, and must not be rewritten.
inline bool isResolutionSegment(const char* text)
{
    if (text == nullptr) {
        return false;
    }
    const char* cursor = text;
    int digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
        ++digits;
    }
    if (digits == 0 || *cursor != 'x') {
        return false;
    }
    ++cursor;
    digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    return *cursor == '\0' || *cursor == '/';
}

// The resolution segment of a requested path, and the rest of the path.
//
//   "/assets_960x640/common/x"      -> "960x640", "/common/x"
//   "/gui_assets/960x640/common/x"  -> "960x640", "/common/x"
//
// Returns false when the path has no resolution segment.
inline bool splitResolution(const char* path, char resolution[32], const char** restOut)
{
    if (path == nullptr || resolution == nullptr || restOut == nullptr) {
        return false;
    }
    const char* after = nullptr;
    if (std::strncmp(path, "/assets_", 8) == 0) {
        after = path + 8;
    } else if (std::strncmp(path, "/gui_assets/", 12) == 0) {
        after = path + 12;
    } else {
        return false;
    }
    const char* slash = std::strchr(after, '/');
    if (slash == nullptr) {
        return false;
    }
    const size_t length = static_cast<size_t>(slash - after);
    if (length == 0u || length >= 32u) {
        return false;
    }
    std::memcpy(resolution, after, length);
    resolution[length] = '\0';
    if (!isResolutionSegment(resolution)) {
        resolution[0] = '\0';
        return false;
    }
    *restOut = slash;
    return true;
}

// Append one candidate.
//
// A TRUNCATED PATH IS A DIFFERENT PATH, so a candidate that does not fit is
// DROPPED and not truncated: a truncated path can name a file that exists and
// is not the asset.
inline bool addCandidate(std::vector<Candidate>& out, const char* resolution,
                         bool digestMustMatch, const char* format, ...)
{
    char path[sizeof(Candidate::path)];
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(path, sizeof(path), format, args);
    va_end(args);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(path)) {
        return false;
    }
    Candidate candidate;
    std::memset(&candidate, 0, sizeof(candidate));
    std::memcpy(candidate.path, path, static_cast<size_t>(written) + 1u);
    if (resolution != nullptr) {
        std::strncpy(candidate.resolution, resolution,
                     sizeof(candidate.resolution) - 1u);
    }
    candidate.digestMustMatch = digestMustMatch;
    out.push_back(candidate);
    return true;
}

// THE RESOLUTION CHAIN, HIGH TO LOW.
//
// The alias walks DOWN this chain and never up, because that is the direction
// the port's runtime path space already falls back in: a 2048x1536 request may
// be answered from the 960x640 tree, never the other way round. The asset
// path space has to agree with the runtime path space, or the guest md5s one
// file and renders another.
inline const char* const* resolutionChain(unsigned& countOut)
{
    static const char* const kChain[] = {
        "2048x1536", "960x640", "480x320",
    };
    countOut = static_cast<unsigned>(sizeof(kChain) / sizeof(kChain[0]));
    return kChain;
}

// THE CANDIDATE LIST: the local files that can back a requested path, in the
// order they are tried.
//
// The list lives on the HEAP on purpose: this runs on a guest thread, and the
// port has already measured a 64 KiB stack buffer as fatal there
// (native-switch/source/guest_filesystem.cpp:1103-1114).
inline void buildCandidates(const char* path, std::vector<Candidate>& out)
{
    out.clear();
    if (path == nullptr) {
        return;
    }

    // 1 and 2: the same relative name, on the card and in the volume.
    addCandidate(out, "", false, ".depot%s", path);
    addCandidate(out, "", false, "files/.depot%s", path);

    // The resolution of the request, when it has one.
    char resolution[32] = {0};
    const char* rest = nullptr;
    const bool hasResolution = splitResolution(path, resolution, &rest);
    const bool assetsSpelling = (std::strncmp(path, "/assets_", 8) == 0);
    if (!hasResolution) {
        resolution[0] = '\0';
    }

    if (hasResolution && assetsSpelling) {
        // 3. The gui_assets tree, for the server's "/assets_<res>/" spelling.
        addCandidate(out, resolution, false,
                     "files/.depot/gui_assets/%s%s", resolution, rest);
    }
    if (hasResolution && !assetsSpelling) {
        // 4. The card's installed tree, for the guest's own spelling.
        addCandidate(out, resolution, false,
                     ".depot/assets_%s%s", resolution, rest);
    }

    // 5. THE GUEST'S OWN DOWNLOAD CACHE.
    //
    // The guest writes the bytes it downloaded to "<asset path>.ast" and
    // reads them back from there, so the cache holds the asset under one of
    // the two spellings -- and the port has seen BOTH on the card:
    //
    //   "gui_assets/<res>/..." for an "/assets_<res>/..." request (measured:
    //   the guest wrote .depot/gui_assets/960x640/common/sprites.android.
    //   atlas.ast while it REQUESTED /assets_960x640/common/sprites.android.
    //   atlas -- native-switch/source/guest_filesystem.cpp:1700-1703);
    //
    //   "assets_<res>/..." for an "/assets_<res>/..." request (measured: the
    //   deployed card's ONE ".ast" is .depot/assets_480x320/events/map/
    //   featured/weekly_events/lexus_is_f350_catalunya/sprites_0.rgb.pvr.z.
    //   ast, and the guest asked for /assets_480x320/events/map/featured/
    //   weekly_events/lexus_is_f350_catalunya/sprites_0.rgb.pvr.z).
    //
    // So both are tried, and both are digest-gated: the cache is the answer
    // only when it holds the bytes the guest asked for.
    addCandidate(out, resolution, true, ".depot%s.ast", path);
    if (hasResolution) {
        if (assetsSpelling) {
            addCandidate(out, resolution, true,
                         ".depot/gui_assets/%s%s.ast", resolution, rest);
        } else {
            addCandidate(out, resolution, true,
                         ".depot/assets_%s%s.ast", resolution, rest);
        }
    }

    // 6. THE NEAREST LOWER RESOLUTION.
    //
    // The live weekly-event assets are the case this exists for: the
    // 2048x1536 tree has no copy of them in ANY of the seven volumes (the
    // "weekly_events" subtree is absent from core.romfs, gui-sd.romfs and
    // gui-hd.romfs alike -- measured with RomFS.contains() and by walking
    // every volume's file table), and the card's 2048x1536 tree holds only
    // "common/". The 960x640 and the 480x320 trees have the asset.
    //
    // The sources are tried in the order the port prefers them everywhere
    // else: the card's installed copy, the card's own download cache, the
    // card's gui_assets copy, then the volume's gui_assets copy.
    if (hasResolution) {
        unsigned chainCount = 0;
        const char* const* chain = resolutionChain(chainCount);
        for (unsigned i = 0; i < chainCount; ++i) {
            if (std::strcmp(chain[i], resolution) != 0) {
                continue;
            }
            for (unsigned j = i + 1u; j < chainCount; ++j) {
                const char* lower = chain[j];
                addCandidate(out, lower, false,
                             ".depot/assets_%s%s", lower, rest);
                addCandidate(out, lower, true,
                             ".depot/assets_%s%s.ast", lower, rest);
                addCandidate(out, lower, false,
                             ".depot/gui_assets/%s%s", lower, rest);
                addCandidate(out, lower, false,
                             "files/.depot/gui_assets/%s%s", lower, rest);
            }
            break;
        }
    }
}

// The digests are lowercase hex on both sides, but the wire field is compared
// case-insensitively by the endpoint, so the comparison is too.
inline bool digestEquals(const char* left, const char* right)
{
    if (left == nullptr || right == nullptr) {
        return false;
    }
    for (unsigned i = 0; i < 33u; ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
        if (a == '\0') {
            return true;
        }
    }
    return true;
}

// THE ANSWER: which candidate backs a requested path.
struct Selection {
    // The index into the candidate list, or -1 when nothing answered.
    int index{-1};
    // The digest of the bytes the chosen candidate holds.
    char digest[33]{};
    // Their size.
    unsigned long long size{0};
    // TRUE when the digest matched the one the caller expected.
    bool exact{false};
    // TRUE when the answer is the guest's own download cache.
    bool fromDownloadCache{false};
};

// THE ONE SELECTION RULE.
//
// expectedDigest == nullptr -- THE MANIFEST SIDE. The digest is what this
//   call is computing, so there is nothing to match against: the first
//   candidate that exists is the answer, and the guest's own download cache
//   ("<path>.ast") is the answer only when NOTHING ELSE backs the path -- a
//   leftover of an unknown revision is not the asset.
//
// expectedDigest != nullptr -- THE ENDPOINT SIDE. The digest is the one the
//   guest asked for on the wire. A candidate whose digest MATCHES it is the
//   answer (the download cache included, which is exactly what rule 5's gate
//   is for); otherwise the first candidate that exists is the fallback, and
//   the download cache is NOT an answer.
//
// THE INVARIANT. The manifest publishes the digest the manifest side picks,
// and the guest then asks for THAT digest, so the endpoint's answer is the
// candidate whose digest is the published one -- the same bytes, the same
// digest. Two copies of this rule could only agree by accident; there is one.
//
// THE READER. reader(candidate, index, digest, size, bytes) reads ONE
// candidate and reports its digest and its size; it returns FALSE when the
// candidate cannot be read at all. `bytes` receives the content. `index` is
// the candidate's position, so a caller can remember per-candidate state.
template <typename Reader>
inline Selection selectCandidate(const std::vector<Candidate>& candidates,
                                  const char* expectedDigest,
                                  Reader&& reader,
                                  std::vector<unsigned char>* chosenBytes = nullptr)
{
    Selection selection;
    int fallbackIndex = -1;
    int cacheIndex = -1;
    char fallbackDigest[33] = {0};
    char cacheDigest[33] = {0};
    unsigned long long fallbackSize = 0;
    unsigned long long cacheSize = 0;
    std::vector<unsigned char> fallbackBytes;
    std::vector<unsigned char> cacheBytes;
    const bool manifestMode = (expectedDigest == nullptr);

    for (unsigned index = 0; index < candidates.size(); ++index) {
        const Candidate& candidate = candidates[index];
        char digest[33] = {0};
        unsigned long long size = 0;
        std::vector<unsigned char> bytes;
        if (!reader(candidate, index, digest, size, bytes)) {
            continue;
        }
        // THE EXACT MATCH WINS, wherever it is. This is the endpoint's md5
        // tie-breaker: the candidate whose digest is the one the guest asked
        // for is the file the guest wants, whatever else is lying around under
        // a similar name.
        if (!manifestMode && expectedDigest[0] != '\0' &&
            digestEquals(digest, expectedDigest)) {
            selection.index = static_cast<int>(index);
            selection.exact = true;
            selection.fromDownloadCache = candidate.digestMustMatch;
            std::memcpy(selection.digest, digest, sizeof(selection.digest));
            selection.size = size;
            if (chosenBytes != nullptr) {
                *chosenBytes = std::move(bytes);
            }
            return selection;
        }
        if (candidate.digestMustMatch) {
            // THE GUEST'S OWN DOWNLOAD CACHE IS THE ASSET ONLY WHEN IT IS THE
            // ASSET. A "<path>.ast" left over from an older revision hashes to
            // something else, and answering with it would be a false success.
            //
            // The manifest side may still USE it, but only when nothing else
            // backs the path: the manifest publishes a digest, and the digest
            // it publishes has to be the digest of bytes the endpoint will
            // send, so the cache is the answer only when it is the only
            // answer there is.
            if (cacheIndex < 0) {
                cacheIndex = static_cast<int>(index);
                std::memcpy(cacheDigest, digest, sizeof(cacheDigest));
                cacheSize = size;
                cacheBytes = std::move(bytes);
            }
            continue;
        }
        if (fallbackIndex < 0) {
            fallbackIndex = static_cast<int>(index);
            std::memcpy(fallbackDigest, digest, sizeof(fallbackDigest));
            fallbackSize = size;
            fallbackBytes = std::move(bytes);
            if (manifestMode) {
                // THE MANIFEST SIDE: the first candidate that exists IS the
                // answer, so the scan can stop here. The endpoint side cannot
                // stop: it still has to look for the digest the guest asked
                // for.
                break;
            }
        }
    }

    if (fallbackIndex >= 0) {
        selection.index = fallbackIndex;
        std::memcpy(selection.digest, fallbackDigest, sizeof(selection.digest));
        selection.size = fallbackSize;
        if (chosenBytes != nullptr) {
            *chosenBytes = std::move(fallbackBytes);
        }
        return selection;
    }
    if (manifestMode && cacheIndex >= 0) {
        selection.index = cacheIndex;
        selection.fromDownloadCache = true;
        std::memcpy(selection.digest, cacheDigest, sizeof(selection.digest));
        selection.size = cacheSize;
        if (chosenBytes != nullptr) {
            *chosenBytes = std::move(cacheBytes);
        }
        return selection;
    }
    return selection;
}

// The resolution an answer came from, when it is NOT the resolution that was
// asked for -- that is, the nearest lower resolution the alias used. "-" when
// the answer is the requested resolution, or when the request has no
// resolution at all.
inline const char* aliasResolution(const char* requested, const Candidate& answered)
{
    static const char kNone[] = "-";
    if (requested == nullptr) {
        return kNone;
    }
    char requestedResolution[32] = {0};
    const char* rest = nullptr;
    if (!splitResolution(requested, requestedResolution, &rest)) {
        return kNone;
    }
    if (answered.resolution[0] == '\0') {
        return kNone;
    }
    if (std::strcmp(answered.resolution, requestedResolution) == 0) {
        return kNone;
    }
    return answered.resolution;
}

} // namespace asset
} // namespace rr3
