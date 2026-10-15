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
struct dirent* guestReaddir(DIR* directory);
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
