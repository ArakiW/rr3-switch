#pragma once

#include <switch.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace rr3 {

constexpr size_t kExternalDataVolumeCount = 7;

struct ExternalDataStatus {
    bool attempted{};
    bool ready{};
    bool saveReady{};
    bool cacheReady{};
    bool looseFallback{};
    uint32_t mountedVolumes{};
    Result volumeResults[kExternalDataVolumeCount]{};
    char error[160]{};
};

ExternalDataStatus mountExternalData();
void unmountExternalData();
const ExternalDataStatus& externalDataStatus();
bool externalDataReady();

// Pure path-contract probe. Expected mask is 0x1fff (bits 512/1024 added 2026-10-04:`r`n// for the leaf-scoped save exclusion -- see mapAppRootToContent).
// 2026-10-22: bit 4 now asserts that the TWO SPELLINGS of one logical
// asset ("gui_assets/<res>/X" and "assets_<res>/X") meet at ONE file, and
// bit 1024 additionally asserts that a DOUBLED path (a device named twice)
// is refused with ENOENT. The mask is unchanged: main.cpp gates the guest
// boot on it.
uint64_t guestFilesystemSelfTest();

std::FILE* guestFopen(const char* path, const char* mode);
int guestOpen(const char* path, int flags, ...);
int guestOpen2(const char* path, int flags);
int guestAccess(const char* path, int mode);
int guestStat(const char* path, struct stat* output);
int guestLstat(const char* path, struct stat* output);
// Verifies that the bionic stat layout writes st_size at +0x30, the offset the
// guest loads its mmap length from (ELF 0x12a4a24). Depends on nothing but the
// converter, so it needs no volume and no guest run.
bool bionicStatLayoutSelfTest();
// The active stat-layout A/B mode, so the boot line reports the mask that is
// actually true for the build instead of a constant that can go stale.
int statBionicModeForLog();
int guestMkdir(const char* path, mode_t mode);
DIR* guestOpendir(const char* path);
// The guest's readdir returns a POINTER to a dirent in BIONIC's layout --
// d_type at 0x12, d_name at 0x13 -- because the guest reads those offsets
// (measured at 0x12a5c60, 0x12a5c84, 0x12b553c, 0x1892b48, 0x1b5e12c) and the
// host's devkitPro newlib layout is different: MEASURED with this repo's
// toolchain, newlib's `struct dirent` is
//   { ino_t d_ino; unsigned char d_type; char d_name[NAME_MAX+1]; }
// with a TWO-BYTE ino_t, so d_type is at 0x02 and d_name at 0x03 (not
// 0x08/0x09: that was the old comment's claim and it was wrong).
// Returning the host's entry unchanged made the guest read d_type out of
// the middle of the name and d_name 16 bytes (0x13 - 3) into it -- so every
// directory entry was invisible to the guest and every name it did show was
// chopped by exactly 16 bytes. The return type is void* because the value
// is a bionic-layout record, not a host `struct dirent`.
void* guestReaddir(DIR* directory);
// Verifies the dirent layout on BOTH sides: that the host's entry really
// carries d_type at 2 and d_name at 3 (measured, and asserted at compile
// time next to the BionicDirent type), and that the converted record
// carries d_type at 0x12 and d_name at 0x13, the offsets the guest reads.
// Depends on nothing but the converter and the header, so it needs no
// volume and no guest run.
bool bionicDirentLayoutSelfTest();
// ---------------------------------------------------------------------------
// THE SHARED ASSET RESOLVER.
//
// ONE mapping from a Cloudcell asset path to the local file that backs it.
//
// WHY IT IS PUBLIC. The port TERMINATES the Cloudcell asset protocol: it
// answers assetManDownloadPackHash.php with bytes taken off the card and the
// volumes, so for every asset the guest downloads the port decides WHICH
// bytes the guest gets. The guest then MD5s what it received and compares it
// with the md5 in files/.depot/asset_list_updates.txt; if the two disagree it
// answers "didn't match its MD5, corrupt sync?" and DELETES the .ast it just
// wrote.
//
// So the list the port hands the guest has to describe the bytes the port
// will send, and the two sides have to use the SAME resolver -- otherwise a
// list entry can name one file while the endpoint serves another.
//
// The candidate order is the pack endpoint's own order, character for
// character (cc_http_request.cpp assetCandidates): ".depot"+path,
// "files/.depot"+path, and for the server's "/assets_<res>/" spelling
// "files/.depot/gui_assets/<res>"+rest. The read chain inside one candidate
// is the port's own (the volume primary, its alias, the app-local card copy),
// so the file this resolves is the file the endpoint serves -- by
// construction, not by two derivations that happen to agree today.
//
// Returns true and fills outPath (the device path of the file that answered),
// digest (32 lowercase hex characters plus NUL) and sizeOut when the path
// resolves to a local file. Returns false when it resolves to nothing.
// ---------------------------------------------------------------------------
bool resolveAssetLocalFile(const char* assetPath, char* outPath, size_t outPathSize,
                           char digest[33], unsigned long long* sizeOut);

// Rewrites an asset list so that every entry describes the bytes the port
// will actually serve for that path.
//
// THE GUEST-VISIBLE SEMANTICS DO NOT CHANGE: the path text, the entry order,
// the entry count and the field layout are preserved exactly, and only the
// md5 (and the size fields, when the layout has them) is replaced -- and only
// for an entry whose path resolves to a local file. An entry that resolves to
// nothing keeps the server's md5, because there is no real digest to put
// there.
//
// `listPath` is a GUEST path the port resolves (the list is read through the
// port's own read chain, so the bytes are the bytes the guest would have
// read). The rewritten list is written to `outputPath`. Returns true when the
// rewritten list was written.
bool buildConsistentAssetList(const char* listPath, const char* outputPath,
                               unsigned* rewrittenOut, unsigned* unresolvedOut);

long guestPathconf(const char* path, int name);
ssize_t guestReadlink(const char* path, char* output, size_t size);
char* guestRealpath(const char* path, char* output);
int guestStatvfs(const char* path, struct statvfs* output);
int guestRemove(const char* path);
int guestRename(const char* oldPath, const char* newPath);
int guestUnlink(const char* path);
int guestLink(const char* oldPath, const char* newPath);
int guestSymlink(const char* target, const char* linkPath);
int guestBionicStatfs(const char* path, void* output);

} // namespace rr3
