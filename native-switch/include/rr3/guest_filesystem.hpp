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
