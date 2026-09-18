#pragma once

#include <cstddef>
#include <cstdint>

namespace rr3 {

struct ElfPreflight {
    bool opened{};
    bool validElf{};
    bool isAarch64{};
    bool isDynamicObject{};
    uint16_t machine{};
    uint16_t type{};
    uint16_t programHeaderCount{};
    uint32_t loadSegments{};
    uint32_t executableSegments{};
    uint32_t dynamicSegments{};
    uint32_t tlsSegments{};
    uint64_t mappedBytes{};
    uint64_t relaCount{};
    uint64_t pltRelocationBytes{};
    uint64_t relativeRelocations{};
    uint64_t absolute64Relocations{};
    uint64_t globalDataRelocations{};
    uint64_t jumpSlotRelocations{};
    uint64_t unsupportedRelocations{};
    uint32_t neededCount{};
    char error[160]{};
};

struct ElfDryLoad {
    bool opened{};
    bool validElf{};
    bool allocated{};
    bool completed{};
    uint64_t imageBytes{};
    uint64_t copiedBytes{};
    uint64_t zeroFilledBytes{};
    uint64_t relativeApplied{};
    uint64_t externalPending{};
    uint64_t internalSymbolRelocations{};
    uint64_t uniqueExternalSymbols{};
    uint64_t weakExternalSymbols{};
    uint64_t versionedExternalSymbols{};
    uint64_t maximumSymbolIndex{};
    uint64_t symbolNameHash{};
    uint64_t resolverTableEntries{};
    uint64_t resolverTableNameHash{};
    uint64_t lookupMatchedSymbols{};
    uint64_t lookupMatchedRelocations{};
    uint64_t lookupMissingSymbols{};
    uint64_t lookupMissingRelocations{};
    uint64_t lookupMatchedNameHash{};
    uint64_t glesSymbols{};
    uint64_t eglSymbols{};
    uint64_t fmodSymbols{};
    uint64_t arcoreSymbols{};
    uint64_t pthreadSymbols{};
    uint64_t androidSymbols{};
    uint64_t cxxSymbols{};
    uint64_t otherSymbols{};
    uint64_t invalidRelocations{};
    uint64_t imageHash{};
    char error[160]{};
};

struct ElfCrossLink {
    bool primaryOpened{};
    bool dependencyOpened{};
    bool primaryLoaded{};
    bool dependencyLoaded{};
    bool retained{};
    bool completed{};
    uint64_t primaryImageBytes{};
    uint64_t dependencyImageBytes{};
    uint64_t relativeApplied{};
    uint64_t internalApplied{};
    uint64_t primaryHostRelocations{};
    uint64_t primaryCrossRelocations{};
    uint64_t primaryWeakZeroRelocations{};
    uint64_t primaryMissingRelocations{};
    uint64_t dependencyHostRelocations{};
    uint64_t dependencyCrossRelocations{};
    uint64_t dependencyWeakZeroRelocations{};
    uint64_t dependencyMissingRelocations{};
    uint64_t patchedRelocations{};
    uint64_t verifiedWrites{};
    uint64_t uniqueHostSymbols{};
    uint64_t uniqueCrossSymbols{};
    uint64_t uniqueMissingSymbols{};
    uint64_t primaryConstructors{};
    uint64_t dependencyConstructors{};
    uint64_t finalizers{};
    uint64_t lifecycleTargetsVerified{};
    uint64_t resolutionPlanHash{};
    uint64_t invalidRelocations{};
    char error[160]{};
};

struct ElfLoaderSession {
    bool completed{};
    bool retained{};
    bool executableAliases{};
    bool permissionApiAvailable{};
    bool permissionFallbackUsed{};
    bool segmentPermissionsEnforced{};
    bool segmentedMapperAttempted{};
    bool segmentedMapperSucceeded{};
    uint32_t segmentedMapperMode{};
    bool activationReady{};
    uint64_t imageCount{};
    uint64_t totalImageBytes{};
    uint64_t loadSegments{};
    uint64_t executableSegments{};
    uint64_t writableSegments{};
    uint64_t readOnlySegments{};
    uint64_t writableExecutableSegments{};
    uint64_t relativeApplied{};
    uint64_t internalApplied{};
    uint64_t hostRelocations{};
    uint64_t crossRelocations{};
    uint64_t weakZeroRelocations{};
    uint64_t missingRelocations{};
    uint64_t patchedRelocations{};
    uint64_t verifiedWrites{};
    uint64_t uniqueHostSymbols{};
    uint64_t uniqueCrossSymbols{};
    uint64_t uniqueMissingSymbols{};
    uint64_t neededEntries{};
    uint64_t neededEmbedded{};
    uint64_t neededVirtualized{};
    uint64_t neededMissing{};
    uint64_t relroRanges{};
    uint64_t relroBytes{};
    uint64_t relroTargetsVerified{};
    uint64_t tlsSegments{};
    uint64_t tlsBytes{};
    uint64_t constructors{};
    uint64_t finalizers{};
    uint64_t lifecycleTargetsVerified{};
    uint64_t constructorOrderEntries{};
    uint64_t finalizerOrderEntries{};
    uint64_t constructorOrderHash{};
    uint64_t finalizerOrderHash{};
    uint64_t dependencyCycles{};
    uint64_t jitCreateFailures{};
    uint64_t jitExecutableFailures{};
    uint64_t permissionRanges{};
    uint64_t permissionRangesVerified{};
    uint64_t permissionFailures{};
    uint64_t processPermissionFailures{};
    uint64_t segmentedRegions{};
    uint64_t segmentedRegionsVerified{};
    uint64_t segmentedFailures{};
    uint64_t segmentedFailureImage{};
    uint64_t segmentedFailureRegion{};
    uint64_t segmentedFailureStage{};
    uint32_t segmentedFailureResult{};
    uint32_t segmentedExpectedPermission{};
    uint32_t segmentedActualPermission{};
    uint64_t processMapFailureImage{};
    uint64_t processMapFailureStage{};
    uint32_t processMapFailureResult{};
    uint32_t processBackingType{};
    uint32_t processBackingPermission{};
    uint32_t processBackingAttribute{};
    uint32_t processTargetType{};
    uint32_t processTargetPermission{};
    uint64_t resolutionPlanHash{};
    uint64_t invalidRelocations{};
    char error[192]{};
};

struct GuestElf64ProgramHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtualAddress;
    uint64_t physicalAddress;
    uint64_t fileSize;
    uint64_t memorySize;
    uint64_t alignment;
};

struct GuestDlPhdrInfo {
    uint64_t address;
    const char* name;
    const GuestElf64ProgramHeader* programHeaders;
    uint16_t programHeaderCount;
    uint64_t loadEvents;
    uint64_t unloadEvents;
    size_t tlsModuleId;
    void* tlsData;
};

using GuestDlPhdrCallback = int (*)(GuestDlPhdrInfo*, size_t, void*);

struct GuestDlInfo {
    const char* fileName;
    void* fileBase;
    const char* symbolName;
    void* symbolAddress;
};

struct ElfLoaderUnload {
    bool attempted{};
    bool completed{};
    uint64_t activeMappings{};
    uint64_t unmappedMappings{};
    uint64_t releasedReservations{};
    uint64_t failures{};
    uint32_t firstFailureResult{};
    uint64_t finalizersPlanned{};
    uint64_t finalizersCalled{};
};

constexpr size_t kGuestStartupMaximumImages = 8;

struct GuestStartupImage {
    uint64_t base{};
    uint64_t imageBytes{};
    uint64_t constructors{};
    uint64_t finalizers{};
    char soname[64]{};
};

struct GuestStartupPlan {
    bool available{};
    uint64_t resolutionPlanHash{};
    uint64_t imageCount{};
    uint64_t dependencyConstructors{};
    uint64_t mainConstructors{};
    GuestStartupImage images[kGuestStartupMaximumImages]{};
};

struct GuestInitializationResult {
    bool attempted{};
    bool preflightPassed{};
    bool completed{};
    bool active{};
    uint32_t stage{};
    uint32_t failureCode{};
    uint64_t constructorsPlanned{};
    uint64_t constructorsCalled{};
    uint64_t currentImage{};
    uint64_t currentOrdinal{};
    uint64_t currentTarget{};
};

ElfPreflight inspectElfFile(const char* path);
ElfDryLoad dryLoadElfFile(const char* path);
ElfCrossLink crossLinkElfPair(const char* primaryPath, const char* dependencyPath);
ElfLoaderSession loadLinkElfSet(const char* const* paths, size_t pathCount);
ElfLoaderUnload unloadLinkElfSet();
GuestStartupPlan captureGuestStartupPlan();
GuestInitializationResult initializeGuestDependencies();
// progressPath (optional): if non-null, a durable breadcrumb is fsync'd before
// each constructor call with host TLS restored, so a power-off crash leaves the
// last-reached ordinal on disk for the next launch to read.
GuestInitializationResult initializeGuestMainConstructors(
    const char* progressPath = nullptr);
uintptr_t resolveMainImageExport(const char* name);
// Runtime load bias of libRealRacing3.so (0 if the image is not loaded).
// Guest VA = loadBias + ELF virtual address.
uintptr_t resolveMainImageLoadBias();
// Collects the runtime address of the named export from every loaded guest
// image that defines it (e.g. JNI_OnLoad, which only libNimble exports today).
// Writes up to `max` addresses into `out` and returns the count found.
size_t collectGuestExports(const char* name, uintptr_t* out, size_t max);
uint64_t loaderRollbackSelfTest();
int guestDlIteratePhdr(GuestDlPhdrCallback callback, void* data);
void* guestDlopen(const char* fileName, int flags);
void* guestDlsym(void* handle, const char* symbolName);
int guestDlclose(void* handle);
const char* guestDlerror();
int guestDladdr(const void* address, GuestDlInfo* info);
uint64_t guestDynamicLinkSelfTest();
uint64_t guestPhdrRegistrySelfTest();

} // namespace rr3
