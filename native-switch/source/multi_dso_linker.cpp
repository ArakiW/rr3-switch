#include "rr3/elf_preflight.hpp"
#include "rr3/fmod_audio_stub.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/symbol_resolver.hpp"
#include "rr3/tls_probe.hpp"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <utility>
#include <vector>

namespace rr3 {
namespace {

constexpr unsigned char kElfMagic[4] = {0x7f, 'E', 'L', 'F'};
constexpr uint16_t kEtDyn = 3;
constexpr uint16_t kEmAarch64 = 183;
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtDynamic = 2;
constexpr uint32_t kPtTls = 7;
constexpr uint32_t kPtGnuRelro = 0x6474e552;
constexpr uint32_t kPfExecute = 1;
constexpr uint32_t kPfWrite = 2;
constexpr uint32_t kPfRead = 4;
constexpr int64_t kDtNull = 0;
constexpr int64_t kDtNeeded = 1;
constexpr int64_t kDtHash = 4;
constexpr int64_t kDtStrTab = 5;
constexpr int64_t kDtSymTab = 6;
constexpr int64_t kDtRela = 7;
constexpr int64_t kDtRelaSize = 8;
constexpr int64_t kDtRelaEnt = 9;
constexpr int64_t kDtStrSize = 10;
constexpr int64_t kDtSymEnt = 11;
constexpr int64_t kDtPltRel = 20;
constexpr int64_t kDtJmpRel = 23;
constexpr int64_t kDtPltRelSize = 2;
constexpr int64_t kDtInitArray = 25;
constexpr int64_t kDtFiniArray = 26;
constexpr int64_t kDtInitArraySize = 27;
constexpr int64_t kDtFiniArraySize = 28;
constexpr int64_t kDtSoname = 14;
constexpr int64_t kDtGnuHash = 0x6ffffef5;
constexpr uint32_t kRAarch64Abs64 = 257;
constexpr uint32_t kRAarch64GlobDat = 1025;
constexpr uint32_t kRAarch64JumpSlot = 1026;
constexpr uint32_t kRAarch64Relative = 1027;
constexpr uint16_t kShnUndefined = 0;
constexpr uint16_t kShnAbsolute = 0xfff1;
constexpr uint8_t kBindGlobal = 1;
constexpr uint8_t kBindWeak = 2;
constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kMaximumImageSize = 128ULL * 1024ULL * 1024ULL;

#pragma pack(push, 1)
struct Elf64Header {
    unsigned char ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t programHeaderOffset;
    uint64_t sectionHeaderOffset;
    uint32_t flags;
    uint16_t headerSize;
    uint16_t programHeaderEntrySize;
    uint16_t programHeaderCount;
    uint16_t sectionHeaderEntrySize;
    uint16_t sectionHeaderCount;
    uint16_t sectionNameIndex;
};

struct Elf64Dynamic {
    int64_t tag;
    uint64_t value;
};

struct Elf64Rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};

struct Elf64Symbol {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t sectionIndex;
    uint64_t value;
    uint64_t size;
};
#pragma pack(pop)

static_assert(sizeof(GuestElf64ProgramHeader) == 56);

struct CrossImage {
    bool opened{};
    bool loaded{};
    std::string name;
    std::vector<GuestElf64ProgramHeader> programHeaders;
    std::vector<unsigned char> image;
    uint64_t minimumAddress{};
    uint64_t maximumAddress{};
    uint64_t relaAddress{};
    uint64_t relaSize{};
    uint64_t jumpRelAddress{};
    uint64_t jumpRelSize{};
    uint64_t symbolTableAddress{};
    uint64_t stringTableAddress{};
    uint64_t stringTableSize{};
    uint64_t symbolCount{};
    uint64_t initArrayAddress{};
    uint64_t initArraySize{};
    uint64_t finiArrayAddress{};
    uint64_t finiArraySize{};
    std::string soname;
    std::vector<std::string> neededNames;
    uint64_t loadSegments{};
    uint64_t executableSegments{};
    uint64_t writableSegments{};
    uint64_t readOnlySegments{};
    uint64_t writableExecutableSegments{};
    uint64_t relroAddress{};
    uint64_t relroSize{};
    uint64_t tlsSegments{};
    uint64_t tlsBytes{};
    unsigned char* writableLinkBase{};
    unsigned char* executableLinkBase{};

    bool containsMemory(uint64_t address, uint64_t size) const {
        for (const GuestElf64ProgramHeader& ph : programHeaders) {
            if (ph.type != kPtLoad || address < ph.virtualAddress) {
                continue;
            }
            const uint64_t relative = address - ph.virtualAddress;
            if (relative <= ph.memorySize && size <= ph.memorySize - relative) {
                return true;
            }
        }
        return false;
    }

    bool containsFileData(uint64_t address, uint64_t size) const {
        for (const GuestElf64ProgramHeader& ph : programHeaders) {
            if (ph.type != kPtLoad || address < ph.virtualAddress) {
                continue;
            }
            const uint64_t relative = address - ph.virtualAddress;
            if (relative <= ph.fileSize && size <= ph.fileSize - relative) {
                return true;
            }
        }
        return false;
    }

    unsigned char* pointer(uint64_t address) {
        unsigned char* base = writableLinkBase != nullptr ? writableLinkBase : image.data();
        return base + address - minimumAddress;
    }

    const unsigned char* pointer(uint64_t address) const {
        const unsigned char* base =
            writableLinkBase != nullptr ? writableLinkBase : image.data();
        return base + address - minimumAddress;
    }

    uintptr_t loadBias() const {
        const unsigned char* base =
            executableLinkBase != nullptr ? executableLinkBase : image.data();
        return reinterpret_cast<uintptr_t>(base) - minimumAddress;
    }

    uintptr_t executableStart() const {
        return loadBias() + minimumAddress;
    }
};

CrossImage gPrimaryImage;
CrossImage gDependencyImage;
constexpr size_t kMaximumLoaderImages = 8;
CrossImage gLoaderImages[kMaximumLoaderImages];
Jit gLoaderJits[kMaximumLoaderImages]{};
bool gLoaderJitCreated[kMaximumLoaderImages]{};
size_t gLoaderImageCount = 0;
size_t gLifecycleOrder[kMaximumLoaderImages]{};
size_t gLifecycleOrderCount = 0;
bool gGuestImageInitialized[kMaximumLoaderImages]{};
uint64_t gLoaderResolutionPlanHash = 0;

struct SegmentCodeMapping {
    Handle handle{INVALID_HANDLE};
    void* backing{};
    void* ownerAddress{};
    void* targetAddress{};
    size_t size{};
    u32 permission{};
    bool ownerMapped{};
    bool slaveMapped{};
    bool targetIsOwner{};
    bool normalMapped{};
};

struct SegmentedImageMapping {
    void* targetBase{};
    VirtmemReservation* targetReservation{};
    void* processBacking{};
    bool processMapped{};
    std::vector<SegmentCodeMapping> ranges;
};

SegmentedImageMapping gSegmentedImages[kMaximumLoaderImages];
bool gSegmentedSessionActive = false;

void closeSegmentedMappings();
ElfLoaderUnload closeSegmentedMappingsDetailed();
u32 permissionForFlags(uint32_t flags);

uint64_t alignDown(uint64_t value) {
    return value & ~(kPageSize - 1);
}

bool alignUp(uint64_t value, uint64_t* output) {
    if (value > UINT64_MAX - (kPageSize - 1)) {
        return false;
    }
    *output = (value + kPageSize - 1) & ~(kPageSize - 1);
    return true;
}

void setError(ElfCrossLink* result, const char* message) {
    if (result->error[0] == '\0') {
        std::snprintf(result->error, sizeof(result->error), "%s", message);
    }
}

void setError(ElfLoaderSession* result, const char* message) {
    if (result->error[0] == '\0') {
        std::snprintf(result->error, sizeof(result->error), "%s", message);
    }
}

uint64_t appendBytes(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t appendText(uint64_t hash, const char* text) {
    hash = appendBytes(hash, text, std::strlen(text));
    const unsigned char terminator = 0;
    return appendBytes(hash, &terminator, 1);
}

bool readFile(std::FILE* file, uint64_t offset, void* output, size_t size) {
    return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
        std::fread(output, 1, size, file) == size;
}

bool readU32(const CrossImage& image, uint64_t address, uint32_t* value) {
    if (!image.containsFileData(address, sizeof(*value))) {
        return false;
    }
    std::memcpy(value, image.pointer(address), sizeof(*value));
    return true;
}

bool calculateGnuSymbolCount(const CrossImage& image, uint64_t hashAddress,
                             uint64_t* count) {
    uint32_t bucketCount = 0;
    uint32_t symbolOffset = 0;
    uint32_t bloomSize = 0;
    if (!readU32(image, hashAddress, &bucketCount) ||
        !readU32(image, hashAddress + 4, &symbolOffset) ||
        !readU32(image, hashAddress + 8, &bloomSize) ||
        bucketCount == 0 || bloomSize == 0) {
        return false;
    }
    const uint64_t bucketAddress = hashAddress + 16 + static_cast<uint64_t>(bloomSize) * 8;
    const uint64_t chainAddress = bucketAddress + static_cast<uint64_t>(bucketCount) * 4;
    uint64_t maximum = symbolOffset;
    bool sawSymbol = false;
    for (uint32_t bucketIndex = 0; bucketIndex < bucketCount; ++bucketIndex) {
        uint32_t symbolIndex = 0;
        if (!readU32(image, bucketAddress + static_cast<uint64_t>(bucketIndex) * 4,
                     &symbolIndex)) {
            return false;
        }
        if (symbolIndex < symbolOffset) {
            continue;
        }
        sawSymbol = true;
        for (uint64_t guard = 0; guard < 1000000; ++guard, ++symbolIndex) {
            uint32_t chain = 0;
            const uint64_t entryAddress = chainAddress +
                static_cast<uint64_t>(symbolIndex - symbolOffset) * 4;
            if (!readU32(image, entryAddress, &chain)) {
                return false;
            }
            maximum = std::max<uint64_t>(maximum, symbolIndex);
            if ((chain & 1U) != 0) {
                break;
            }
            if (guard == 999999) {
                return false;
            }
        }
    }
    *count = sawSymbol ? maximum + 1 : symbolOffset;
    return *count != 0;
}

bool loadImage(const char* path, CrossImage* output, ElfCrossLink* result) {
    *output = {};
    output->name = path != nullptr ? path : "";
    std::FILE* file = path != nullptr ? std::fopen(path, "rb") : nullptr;
    if (file == nullptr) {
        setError(result, "cross-link ELF file not found");
        return false;
    }
    output->opened = true;

    Elf64Header header{};
    if (!readFile(file, 0, &header, sizeof(header)) ||
        std::memcmp(header.ident, kElfMagic, sizeof(kElfMagic)) != 0 ||
        header.ident[4] != 2 || header.ident[5] != 1 || header.type != kEtDyn ||
        header.machine != kEmAarch64 ||
        header.programHeaderEntrySize != sizeof(GuestElf64ProgramHeader)) {
        setError(result, "unsupported cross-link ELF header");
        std::fclose(file);
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        setError(result, "could not size cross-link ELF");
        std::fclose(file);
        return false;
    }
    const long fileLengthSigned = std::ftell(file);
    const uint64_t phBytes =
        static_cast<uint64_t>(header.programHeaderCount) * sizeof(GuestElf64ProgramHeader);
    if (fileLengthSigned < 0 || header.programHeaderOffset >
            static_cast<uint64_t>(fileLengthSigned) ||
        phBytes > static_cast<uint64_t>(fileLengthSigned) - header.programHeaderOffset) {
        setError(result, "cross-link program headers outside file");
        std::fclose(file);
        return false;
    }
    const uint64_t fileLength = static_cast<uint64_t>(fileLengthSigned);
    output->programHeaders.resize(header.programHeaderCount);
    if (!readFile(file, header.programHeaderOffset, output->programHeaders.data(),
                  static_cast<size_t>(phBytes))) {
        setError(result, "could not read cross-link program headers");
        std::fclose(file);
        return false;
    }

    output->minimumAddress = UINT64_MAX;
    output->maximumAddress = 0;
    const GuestElf64ProgramHeader* dynamicHeader = nullptr;
    for (const GuestElf64ProgramHeader& ph : output->programHeaders) {
        if (ph.type == kPtLoad) {
            if (ph.fileSize > ph.memorySize || ph.virtualAddress > UINT64_MAX - ph.memorySize ||
                ph.offset > fileLength || ph.fileSize > fileLength - ph.offset) {
                setError(result, "invalid cross-link PT_LOAD");
                std::fclose(file);
                return false;
            }
            output->minimumAddress = std::min(output->minimumAddress,
                                              alignDown(ph.virtualAddress));
            output->maximumAddress = std::max(output->maximumAddress,
                                              ph.virtualAddress + ph.memorySize);
            ++output->loadSegments;
            if ((ph.flags & kPfExecute) != 0) {
                ++output->executableSegments;
            }
            if ((ph.flags & kPfWrite) != 0) {
                ++output->writableSegments;
            }
            if ((ph.flags & (kPfWrite | kPfExecute)) == 0 &&
                (ph.flags & kPfRead) != 0) {
                ++output->readOnlySegments;
            }
            if ((ph.flags & (kPfWrite | kPfExecute)) ==
                (kPfWrite | kPfExecute)) {
                ++output->writableExecutableSegments;
            }
        }
        else if (ph.type == kPtDynamic) {
            if (dynamicHeader != nullptr) {
                setError(result, "multiple cross-link PT_DYNAMIC segments");
                std::fclose(file);
                return false;
            }
            dynamicHeader = &ph;
        }
        else if (ph.type == kPtGnuRelro) {
            if (output->relroSize != 0) {
                setError(result, "multiple GNU_RELRO segments");
                std::fclose(file);
                return false;
            }
            output->relroAddress = ph.virtualAddress;
            output->relroSize = ph.memorySize;
        }
        else if (ph.type == kPtTls) {
            ++output->tlsSegments;
            output->tlsBytes += ph.memorySize;
        }
    }
    if (output->minimumAddress == UINT64_MAX ||
        !alignUp(output->maximumAddress, &output->maximumAddress) ||
        output->maximumAddress <= output->minimumAddress ||
        output->maximumAddress - output->minimumAddress > kMaximumImageSize ||
        dynamicHeader == nullptr) {
        setError(result, "invalid cross-link image range");
        std::fclose(file);
        return false;
    }

    output->image.assign(static_cast<size_t>(output->maximumAddress - output->minimumAddress), 0);
    for (const GuestElf64ProgramHeader& ph : output->programHeaders) {
        if (ph.type != kPtLoad) {
            continue;
        }
        if (!readFile(file, ph.offset, output->pointer(ph.virtualAddress),
                      static_cast<size_t>(ph.fileSize))) {
            setError(result, "cross-link PT_LOAD copy failed");
            std::fclose(file);
            return false;
        }
    }
    std::fclose(file);

    if (dynamicHeader->fileSize == 0 ||
        dynamicHeader->fileSize % sizeof(Elf64Dynamic) != 0 ||
        !output->containsFileData(dynamicHeader->virtualAddress, dynamicHeader->fileSize)) {
        setError(result, "invalid cross-link dynamic table");
        return false;
    }

    uint64_t hashAddress = 0;
    uint64_t gnuHashAddress = 0;
    uint64_t relaEntrySize = 0;
    uint64_t symbolEntrySize = 0;
    uint64_t pltRelType = 0;
    uint64_t sonameOffset = UINT64_MAX;
    std::vector<uint64_t> neededOffsets;
    bool sawNull = false;
    const auto* dynamic = reinterpret_cast<const Elf64Dynamic*>(
        output->pointer(dynamicHeader->virtualAddress));
    const uint64_t dynamicCount = dynamicHeader->fileSize / sizeof(Elf64Dynamic);
    for (uint64_t index = 0; index < dynamicCount; ++index) {
        const Elf64Dynamic entry = dynamic[index];
        if (entry.tag == kDtNull) {
            sawNull = true;
            break;
        }
        switch (entry.tag) {
            case kDtNeeded: neededOffsets.push_back(entry.value); break;
            case kDtHash: hashAddress = entry.value; break;
            case kDtGnuHash: gnuHashAddress = entry.value; break;
            case kDtStrTab: output->stringTableAddress = entry.value; break;
            case kDtSymTab: output->symbolTableAddress = entry.value; break;
            case kDtRela: output->relaAddress = entry.value; break;
            case kDtRelaSize: output->relaSize = entry.value; break;
            case kDtRelaEnt: relaEntrySize = entry.value; break;
            case kDtStrSize: output->stringTableSize = entry.value; break;
            case kDtSymEnt: symbolEntrySize = entry.value; break;
            case kDtJmpRel: output->jumpRelAddress = entry.value; break;
            case kDtPltRelSize: output->jumpRelSize = entry.value; break;
            case kDtPltRel: pltRelType = entry.value; break;
            case kDtInitArray: output->initArrayAddress = entry.value; break;
            case kDtFiniArray: output->finiArrayAddress = entry.value; break;
            case kDtInitArraySize: output->initArraySize = entry.value; break;
            case kDtFiniArraySize: output->finiArraySize = entry.value; break;
            case kDtSoname: sonameOffset = entry.value; break;
            default: break;
        }
    }
    if (!sawNull || output->symbolTableAddress == 0 || output->stringTableAddress == 0 ||
        output->stringTableSize == 0 || symbolEntrySize != sizeof(Elf64Symbol) ||
        (output->relaSize != 0 && relaEntrySize != sizeof(Elf64Rela)) ||
        (output->jumpRelSize != 0 && pltRelType != static_cast<uint64_t>(kDtRela)) ||
        !output->containsFileData(output->stringTableAddress, output->stringTableSize) ||
        (output->relaSize != 0 && !output->containsFileData(output->relaAddress,
                                                            output->relaSize)) ||
        (output->jumpRelSize != 0 && !output->containsFileData(output->jumpRelAddress,
                                                               output->jumpRelSize)) ||
        (output->initArraySize != 0 &&
         (output->initArraySize % sizeof(uint64_t) != 0 ||
          !output->containsMemory(output->initArrayAddress, output->initArraySize))) ||
        (output->finiArraySize != 0 &&
         (output->finiArraySize % sizeof(uint64_t) != 0 ||
          !output->containsMemory(output->finiArrayAddress, output->finiArraySize)))) {
        setError(result, "incomplete cross-link dynamic metadata");
        return false;
    }

    if (hashAddress != 0) {
        uint32_t chainCount = 0;
        if (!readU32(*output, hashAddress + 4, &chainCount)) {
            setError(result, "invalid SYSV hash table");
            return false;
        }
        output->symbolCount = chainCount;
    }
    else if (gnuHashAddress == 0 ||
             !calculateGnuSymbolCount(*output, gnuHashAddress, &output->symbolCount)) {
        setError(result, "could not determine dynamic symbol count");
        return false;
    }
    if (output->symbolCount == 0 ||
        output->symbolCount > UINT64_MAX / sizeof(Elf64Symbol) ||
        !output->containsFileData(output->symbolTableAddress,
                                  output->symbolCount * sizeof(Elf64Symbol))) {
        setError(result, "invalid cross-link dynamic symbol table");
        return false;
    }

    const auto readDynamicString = [&](uint64_t stringOffset, std::string* value) {
        if (stringOffset >= output->stringTableSize) {
            return false;
        }
        const char* candidate = reinterpret_cast<const char*>(
            output->pointer(output->stringTableAddress + stringOffset));
        const size_t remaining = static_cast<size_t>(output->stringTableSize - stringOffset);
        const void* terminator = std::memchr(candidate, '\0', remaining);
        if (terminator == nullptr) {
            return false;
        }
        value->assign(candidate, static_cast<const char*>(terminator) - candidate);
        return true;
    };
    if (sonameOffset != UINT64_MAX && !readDynamicString(sonameOffset, &output->soname)) {
        setError(result, "invalid DT_SONAME string");
        return false;
    }
    if (output->soname.empty()) {
        const size_t slash = output->name.find_last_of("/\\");
        output->soname = slash == std::string::npos
            ? output->name
            : output->name.substr(slash + 1);
    }
    for (uint64_t neededOffset : neededOffsets) {
        std::string needed;
        if (!readDynamicString(neededOffset, &needed)) {
            setError(result, "invalid DT_NEEDED string");
            return false;
        }
        output->neededNames.push_back(needed);
    }
    output->loaded = true;
    return true;
}

bool readSymbol(const CrossImage& image, uint64_t index, Elf64Symbol* symbol,
                const char** name) {
    if (index >= image.symbolCount) {
        return false;
    }
    std::memcpy(symbol,
                image.pointer(image.symbolTableAddress + index * sizeof(Elf64Symbol)),
                sizeof(*symbol));
    if (symbol->name >= image.stringTableSize) {
        return false;
    }
    const char* candidate = reinterpret_cast<const char*>(
        image.pointer(image.stringTableAddress + symbol->name));
    const size_t remaining = static_cast<size_t>(image.stringTableSize - symbol->name);
    if (std::memchr(candidate, '\0', remaining) == nullptr) {
        return false;
    }
    *name = candidate;
    return true;
}

bool findExport(const CrossImage& image, const char* name, uintptr_t* address,
                uint64_t* symbolValue) {
    for (uint64_t index = 1; index < image.symbolCount; ++index) {
        Elf64Symbol symbol{};
        const char* candidate = nullptr;
        if (!readSymbol(image, index, &symbol, &candidate) ||
            symbol.sectionIndex == kShnUndefined || std::strcmp(candidate, name) != 0) {
            continue;
        }
        const uint8_t binding = symbol.info >> 4;
        const uint8_t visibility = symbol.other & 3;
        if ((binding != kBindGlobal && binding != kBindWeak) ||
            (visibility != 0 && visibility != 3)) {
            continue;
        }
        *symbolValue = symbol.value;
        *address = symbol.sectionIndex == kShnAbsolute
            ? static_cast<uintptr_t>(symbol.value)
            : image.loadBias() + symbol.value;
        return true;
    }
    return false;
}

void addUnique(std::vector<std::string>* names, const char* name) {
    if (std::find(names->begin(), names->end(), name) == names->end()) {
        names->emplace_back(name);
    }
}

enum class Provider : unsigned char {
    Relative = 'R',
    Internal = 'I',
    Cross = 'X',
    Host = 'H',
    WeakZero = 'W',
    Missing = 'M',
};

struct ApplyContext {
    ElfCrossLink* result;
    std::vector<std::string>* hostNames;
    std::vector<std::string>* crossNames;
    std::vector<std::string>* missingNames;
    bool primary;
};

void applyTable(CrossImage* image, const CrossImage& other, uint64_t address,
                uint64_t bytes, ApplyContext context) {
    if (bytes == 0) {
        return;
    }
    if (!image->containsFileData(address, bytes) || bytes % sizeof(Elf64Rela) != 0) {
        ++context.result->invalidRelocations;
        return;
    }
    const uint64_t count = bytes / sizeof(Elf64Rela);
    for (uint64_t index = 0; index < count; ++index) {
        Elf64Rela relocation{};
        std::memcpy(&relocation, image->pointer(address + index * sizeof(relocation)),
                    sizeof(relocation));
        const uint32_t type = static_cast<uint32_t>(relocation.info);
        const uint64_t symbolIndex = relocation.info >> 32;
        if (!image->containsMemory(relocation.offset, sizeof(uint64_t)) ||
            (relocation.offset & 7U) != 0) {
            ++context.result->invalidRelocations;
            continue;
        }

        Provider provider = Provider::Missing;
        const char* name = "";
        uint64_t symbolValue = 0;
        uintptr_t resolvedAddress = 0;
        bool patch = false;

        if (type == kRAarch64Relative) {
            if (symbolIndex != 0) {
                ++context.result->invalidRelocations;
                continue;
            }
            provider = Provider::Relative;
            resolvedAddress = image->loadBias();
            patch = true;
            ++context.result->relativeApplied;
        }
        else if (type == kRAarch64Abs64 || type == kRAarch64GlobDat ||
                 type == kRAarch64JumpSlot) {
            Elf64Symbol symbol{};
            if (symbolIndex == 0 || !readSymbol(*image, symbolIndex, &symbol, &name) ||
                name[0] == '\0') {
                ++context.result->invalidRelocations;
                continue;
            }
            symbolValue = symbol.value;
            if (symbol.sectionIndex != kShnUndefined) {
                provider = Provider::Internal;
                resolvedAddress = symbol.sectionIndex == kShnAbsolute
                    ? static_cast<uintptr_t>(symbol.value)
                    : image->loadBias() + symbol.value;
                patch = true;
                ++context.result->internalApplied;
            }
            else if (findExport(other, name, &resolvedAddress, &symbolValue)) {
                provider = Provider::Cross;
                patch = true;
                addUnique(context.crossNames, name);
                if (context.primary) {
                    ++context.result->primaryCrossRelocations;
                }
                else {
                    ++context.result->dependencyCrossRelocations;
                }
            }
            else if ((resolvedAddress = resolveHostSymbol(name)) != 0) {
                provider = Provider::Host;
                patch = true;
                addUnique(context.hostNames, name);
                if (context.primary) {
                    ++context.result->primaryHostRelocations;
                }
                else {
                    ++context.result->dependencyHostRelocations;
                }
            }
            else if ((symbol.info >> 4) == kBindWeak) {
                provider = Provider::WeakZero;
                patch = true;
                if (context.primary) {
                    ++context.result->primaryWeakZeroRelocations;
                }
                else {
                    ++context.result->dependencyWeakZeroRelocations;
                }
            }
            else {
                addUnique(context.missingNames, name);
                if (context.primary) {
                    ++context.result->primaryMissingRelocations;
                }
                else {
                    ++context.result->dependencyMissingRelocations;
                }
            }
        }
        else {
            ++context.result->invalidRelocations;
            continue;
        }

        const unsigned char imageTag = context.primary ? 'P' : 'D';
        const unsigned char providerTag = static_cast<unsigned char>(provider);
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &imageTag, sizeof(imageTag));
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &providerTag, sizeof(providerTag));
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &type, sizeof(type));
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &relocation.addend, sizeof(relocation.addend));
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &symbolValue, sizeof(symbolValue));
        context.result->resolutionPlanHash = appendText(context.result->resolutionPlanHash, name);

        if (!patch) {
            continue;
        }
        uint64_t value = static_cast<uint64_t>(resolvedAddress);
        if (type == kRAarch64Relative || type == kRAarch64Abs64) {
            value += static_cast<uint64_t>(relocation.addend);
        }
        std::memcpy(image->pointer(relocation.offset), &value, sizeof(value));
        ++context.result->patchedRelocations;
        uint64_t readBack = 0;
        std::memcpy(&readBack, image->pointer(relocation.offset), sizeof(readBack));
        if (readBack == value) {
            ++context.result->verifiedWrites;
        }
        else {
            ++context.result->invalidRelocations;
        }
    }
}

void verifyLifecycleArray(const CrossImage& image, uint64_t address, uint64_t bytes,
                          uint64_t* entries, ElfCrossLink* result) {
    if (bytes == 0) {
        return;
    }
    *entries += bytes / sizeof(uint64_t);
    const uintptr_t imageStart = reinterpret_cast<uintptr_t>(image.image.data());
    const uintptr_t imageEnd = imageStart + image.image.size();
    for (uint64_t offset = 0; offset < bytes; offset += sizeof(uint64_t)) {
        uint64_t target = 0;
        std::memcpy(&target, image.pointer(address + offset), sizeof(target));
        if (target >= imageStart && target < imageEnd && (target & 3U) == 0) {
            ++result->lifecycleTargetsVerified;
        }
        else {
            ++result->invalidRelocations;
        }
    }
}

void closeLoaderJits() {
    closeSegmentedMappings();
    for (size_t index = 0; index < kMaximumLoaderImages; ++index) {
        if (gLoaderJitCreated[index]) {
            jitClose(&gLoaderJits[index]);
            gLoaderJitCreated[index] = false;
            gLoaderJits[index] = {};
        }
        gLoaderImages[index].writableLinkBase = nullptr;
        gLoaderImages[index].executableLinkBase = nullptr;
    }
    gLoaderImageCount = 0;
}

bool closeWholeImageJitsPreserveImages() {
    bool success = true;
    for (size_t index = 0; index < kMaximumLoaderImages; ++index) {
        if (gLoaderJitCreated[index]) {
            if (R_FAILED(jitClose(&gLoaderJits[index]))) {
                success = false;
            }
            gLoaderJitCreated[index] = false;
            gLoaderJits[index] = {};
        }
        gLoaderImages[index].writableLinkBase = nullptr;
        gLoaderImages[index].executableLinkBase = nullptr;
    }
    return success;
}

ElfLoaderUnload closeSegmentedMappingsDetailed() {
    ElfLoaderUnload result{};
    const Handle process = envGetOwnProcessHandle();
    for (size_t reverseIndex = kMaximumLoaderImages; reverseIndex-- > 0;) {
        const size_t imageIndex = reverseIndex;
        SegmentedImageMapping& image = gSegmentedImages[imageIndex];
        bool imageClean = true;
        if (image.processMapped) {
            result.attempted = true;
            ++result.activeMappings;
            if (process != INVALID_HANDLE) {
                const Result operation = svcUnmapProcessCodeMemory(
                    process,
                    reinterpret_cast<uint64_t>(image.targetBase),
                    reinterpret_cast<uint64_t>(image.processBacking),
                    gLoaderImages[imageIndex].image.size());
                if (R_SUCCEEDED(operation)) {
                    image.processMapped = false;
                    ++result.unmappedMappings;
                }
                else {
                    imageClean = false;
                    ++result.failures;
                    if (result.firstFailureResult == 0) {
                        result.firstFailureResult = operation;
                    }
                }
            }
            else {
                imageClean = false;
                ++result.failures;
                if (result.firstFailureResult == 0) {
                    result.firstFailureResult =
                        MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
                }
            }
        }
        if (!image.processMapped) {
            std::free(image.processBacking);
            image.processBacking = nullptr;
        }
        for (auto cursor = image.ranges.rbegin(); cursor != image.ranges.rend(); ++cursor) {
            SegmentCodeMapping& range = *cursor;
            if (range.normalMapped) {
                svcUnmapMemory(range.targetAddress, range.backing, range.size);
                std::free(range.backing);
                continue;
            }
            if (range.slaveMapped) {
                svcControlCodeMemory(range.handle, CodeMapOperation_UnmapSlave,
                                     range.targetAddress, range.size, 0);
            }
            if (range.ownerMapped) {
                svcControlCodeMemory(range.handle, CodeMapOperation_UnmapOwner,
                                     range.ownerAddress, range.size, 0);
            }
            if (range.handle != INVALID_HANDLE) {
                svcCloseHandle(range.handle);
            }
            std::free(range.backing);
        }
        image.ranges.clear();
        if (imageClean && image.targetReservation != nullptr) {
            virtmemLock();
            virtmemRemoveReservation(image.targetReservation);
            virtmemUnlock();
            ++result.releasedReservations;
            image.targetReservation = nullptr;
        }
        if (imageClean) {
            image = {};
            gLoaderImages[imageIndex].writableLinkBase = nullptr;
            gLoaderImages[imageIndex].executableLinkBase = nullptr;
        }
    }
    result.completed = result.failures == 0;
    if (result.completed) {
        gSegmentedSessionActive = false;
    }
    return result;
}

void closeSegmentedMappings() {
    (void)closeSegmentedMappingsDetailed();
}

bool mapNormalSegmentedRange(size_t imageIndex, uint64_t pageOffset,
                             uint64_t bytes, u32 permission,
                             ElfLoaderSession* result) {
    CrossImage& image = gLoaderImages[imageIndex];
    SegmentedImageMapping& mappedImage = gSegmentedImages[imageIndex];
    SegmentCodeMapping range{};
    range.size = static_cast<size_t>(bytes);
    range.permission = permission;
    range.targetAddress = static_cast<unsigned char*>(mappedImage.targetBase) + pageOffset;
    range.backing = memalign(kPageSize, range.size);
    result->segmentedFailureImage = imageIndex;
    result->segmentedFailureRegion = result->segmentedRegions;
    result->segmentedExpectedPermission = permission;
    if (range.backing == nullptr) {
        result->segmentedFailureStage = 2;
        ++result->segmentedFailures;
        return false;
    }
    std::memcpy(range.backing, image.image.data() + pageOffset, range.size);
    Result operation = svcMapMemory(range.targetAddress, range.backing, range.size);
    if (R_FAILED(operation)) {
        result->segmentedFailureStage = 14;
        result->segmentedFailureResult = operation;
        std::free(range.backing);
        ++result->segmentedFailures;
        return false;
    }
    range.normalMapped = true;
    if (permission == Perm_R) {
        operation = svcSetMemoryPermission(range.targetAddress, range.size, Perm_R);
        if (R_FAILED(operation)) {
            result->segmentedFailureStage = 15;
            result->segmentedFailureResult = operation;
            svcUnmapMemory(range.targetAddress, range.backing, range.size);
            std::free(range.backing);
            ++result->segmentedFailures;
            return false;
        }
    }

    MemoryInfo memoryInfo{};
    u32 pageInfo = 0;
    operation = svcQueryMemory(&memoryInfo, &pageInfo,
                               reinterpret_cast<uint64_t>(range.targetAddress));
    result->segmentedActualPermission = memoryInfo.perm;
    const bool queryFailed = R_FAILED(operation);
    const bool permissionMismatch = !queryFailed && memoryInfo.perm != permission;
    const bool contentMismatch = !queryFailed && !permissionMismatch &&
        std::memcmp(range.targetAddress, image.image.data() + pageOffset, range.size) != 0;
    if (queryFailed || permissionMismatch || contentMismatch) {
        result->segmentedFailureStage = queryFailed ? 16 : (permissionMismatch ? 17 : 18);
        result->segmentedFailureResult = operation;
        svcUnmapMemory(range.targetAddress, range.backing, range.size);
        std::free(range.backing);
        ++result->segmentedFailures;
        return false;
    }

    mappedImage.ranges.push_back(range);
    ++result->segmentedRegions;
    ++result->segmentedRegionsVerified;
    result->segmentedFailureStage = 0;
    result->segmentedFailureResult = 0;
    return true;
}

bool reserveSegmentedTargets(ElfLoaderSession* result) {
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        CrossImage& image = gLoaderImages[index];
        virtmemLock();
        void* target = virtmemFindCodeMemory(image.image.size(), kPageSize);
        VirtmemReservation* reservation = target != nullptr
            ? virtmemAddReservation(target, image.image.size())
            : nullptr;
        virtmemUnlock();
        if (reservation == nullptr) {
            result->segmentedFailureImage = index;
            result->segmentedFailureStage = 1;
            ++result->segmentedFailures;
            return false;
        }
        gSegmentedImages[index].targetBase = target;
        gSegmentedImages[index].targetReservation = reservation;
        image.executableLinkBase = static_cast<unsigned char*>(target);
        image.writableLinkBase = nullptr;
    }
    return true;
}

bool mapSegmentedRange(size_t imageIndex, uint64_t pageOffset, uint64_t bytes,
                       u32 permission, ElfLoaderSession* result) {
    CrossImage& image = gLoaderImages[imageIndex];
    SegmentedImageMapping& mappedImage = gSegmentedImages[imageIndex];
    SegmentCodeMapping range{};
    range.size = static_cast<size_t>(bytes);
    range.permission = permission;
    range.targetAddress = static_cast<unsigned char*>(mappedImage.targetBase) + pageOffset;
    range.backing = memalign(kPageSize, range.size);
    result->segmentedFailureImage = imageIndex;
    result->segmentedFailureRegion = result->segmentedRegions;
    result->segmentedExpectedPermission = permission;
    if (range.backing == nullptr) {
        result->segmentedFailureStage = 2;
        ++result->segmentedFailures;
        return false;
    }
    Result operation = svcCreateCodeMemory(&range.handle, range.backing, range.size);
    if (R_FAILED(operation)) {
        result->segmentedFailureStage = 3;
        result->segmentedFailureResult = operation;
        std::free(range.backing);
        ++result->segmentedFailures;
        return false;
    }

    if (permission == Perm_Rw) {
        range.ownerAddress = range.targetAddress;
        range.targetIsOwner = true;
        operation = svcControlCodeMemory(range.handle, CodeMapOperation_MapOwner,
                                         range.ownerAddress, range.size, Perm_Rw);
        range.ownerMapped = R_SUCCEEDED(operation);
        if (range.ownerMapped) {
            std::memcpy(range.ownerAddress, image.image.data() + pageOffset, range.size);
        }
    }
    else {
        virtmemLock();
        range.ownerAddress = virtmemFindCodeMemory(range.size, kPageSize);
        virtmemUnlock();
        if (range.ownerAddress == nullptr) {
            result->segmentedFailureStage = 4;
            operation = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        else {
            operation = svcControlCodeMemory(range.handle, CodeMapOperation_MapOwner,
                                             range.ownerAddress, range.size, Perm_Rw);
            range.ownerMapped = R_SUCCEEDED(operation);
            if (R_SUCCEEDED(operation)) {
                std::memcpy(range.ownerAddress, image.image.data() + pageOffset, range.size);
                armDCacheFlush(range.ownerAddress, range.size);
                operation = svcControlCodeMemory(range.handle, CodeMapOperation_MapSlave,
                                                 range.targetAddress, range.size, permission);
                range.slaveMapped = R_SUCCEEDED(operation);
            }
        }
    }

    if (R_FAILED(operation)) {
        if (result->segmentedFailureStage == 0) {
            result->segmentedFailureStage = range.ownerMapped ? 6 : 5;
        }
        result->segmentedFailureResult = operation;
        if (range.slaveMapped) {
            svcControlCodeMemory(range.handle, CodeMapOperation_UnmapSlave,
                                 range.targetAddress, range.size, 0);
        }
        if (range.ownerMapped) {
            svcControlCodeMemory(range.handle, CodeMapOperation_UnmapOwner,
                                 range.ownerAddress, range.size, 0);
        }
        svcCloseHandle(range.handle);
        std::free(range.backing);
        ++result->segmentedFailures;
        return false;
    }

    if (range.slaveMapped) {
        armDCacheFlush(range.ownerAddress, range.size);
    }
    if (permission == Perm_Rx) {
        armICacheInvalidate(range.targetAddress, range.size);
    }
    MemoryInfo memoryInfo{};
    u32 pageInfo = 0;
    operation = svcQueryMemory(&memoryInfo, &pageInfo,
                               reinterpret_cast<uint64_t>(range.targetAddress));
    result->segmentedActualPermission = memoryInfo.perm;
    const bool queryFailed = R_FAILED(operation);
    const bool permissionMismatch = !queryFailed && memoryInfo.perm != permission;
    const bool contentMismatch = !queryFailed && !permissionMismatch &&
        std::memcmp(range.targetAddress, image.image.data() + pageOffset, range.size) != 0;
    if (queryFailed || permissionMismatch || contentMismatch) {
        result->segmentedFailureStage = queryFailed ? 7 : (permissionMismatch ? 8 : 9);
        result->segmentedFailureResult = operation;
        if (range.slaveMapped) {
            svcControlCodeMemory(range.handle, CodeMapOperation_UnmapSlave,
                                 range.targetAddress, range.size, 0);
        }
        if (range.ownerMapped) {
            svcControlCodeMemory(range.handle, CodeMapOperation_UnmapOwner,
                                 range.ownerAddress, range.size, 0);
        }
        svcCloseHandle(range.handle);
        std::free(range.backing);
        ++result->segmentedFailures;
        return false;
    }

    mappedImage.ranges.push_back(range);
    ++result->segmentedRegions;
    ++result->segmentedRegionsVerified;
    result->segmentedFailureStage = 0;
    result->segmentedFailureResult = 0;
    return true;
}

[[maybe_unused]] bool mapSegmentedImages(ElfLoaderSession* result) {
    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        const CrossImage& image = gLoaderImages[imageIndex];
        const size_t pageCount = image.image.size() / kPageSize;
        std::vector<u32> pagePermissions(pageCount, Perm_None);
        for (const GuestElf64ProgramHeader& ph : image.programHeaders) {
            if (ph.type != kPtLoad) {
                continue;
            }
            const uint64_t start = alignDown(ph.virtualAddress) - image.minimumAddress;
            uint64_t endAddress = 0;
            if (!alignUp(ph.virtualAddress + ph.memorySize, &endAddress)) {
                ++result->segmentedFailures;
                return false;
            }
            const uint64_t end = endAddress - image.minimumAddress;
            const u32 permission = permissionForFlags(ph.flags);
            for (uint64_t offset = start; offset < end; offset += kPageSize) {
                const size_t page = static_cast<size_t>(offset / kPageSize);
                if (page >= pagePermissions.size() ||
                    (pagePermissions[page] != Perm_None &&
                     pagePermissions[page] != permission)) {
                    result->segmentedFailureImage = imageIndex;
                    result->segmentedFailureStage = 12;
                    ++result->segmentedFailures;
                    return false;
                }
                pagePermissions[page] = permission;
            }
        }
        if (image.relroSize != 0) {
            const uint64_t start = alignDown(image.relroAddress) - image.minimumAddress;
            uint64_t endAddress = 0;
            if (!alignUp(image.relroAddress + image.relroSize, &endAddress)) {
                ++result->segmentedFailures;
                return false;
            }
            const uint64_t end = endAddress - image.minimumAddress;
            for (uint64_t offset = start; offset < end; offset += kPageSize) {
                const size_t page = static_cast<size_t>(offset / kPageSize);
                if (page >= pagePermissions.size()) {
                    result->segmentedFailureImage = imageIndex;
                    result->segmentedFailureStage = 13;
                    ++result->segmentedFailures;
                    return false;
                }
                pagePermissions[page] = Perm_R;
            }
        }

        size_t page = 0;
        while (page < pagePermissions.size()) {
            const u32 permission = pagePermissions[page];
            size_t endPage = page + 1;
            while (endPage < pagePermissions.size() &&
                   pagePermissions[endPage] == permission) {
                ++endPage;
            }
            if (permission != Perm_None) {
                const uint64_t pageOffset = static_cast<uint64_t>(page) * kPageSize;
                const uint64_t rangeBytes =
                    static_cast<uint64_t>(endPage - page) * kPageSize;
                const bool mapped = permission == Perm_Rx
                    ? mapSegmentedRange(imageIndex, pageOffset, rangeBytes,
                                        permission, result)
                    : mapNormalSegmentedRange(imageIndex, pageOffset, rangeBytes,
                                              permission, result);
                if (!mapped) {
                    return false;
                }
            }
            page = endPage;
        }
    }
    gSegmentedSessionActive = true;
    return true;
}

void unmapProcessCodeImages() {
    const Handle process = envGetOwnProcessHandle();
    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        SegmentedImageMapping& mapped = gSegmentedImages[imageIndex];
        if (mapped.processMapped && process != INVALID_HANDLE) {
            svcUnmapProcessCodeMemory(
                process,
                reinterpret_cast<uint64_t>(mapped.targetBase),
                reinterpret_cast<uint64_t>(mapped.processBacking),
                gLoaderImages[imageIndex].image.size());
        }
        mapped.processMapped = false;
        std::free(mapped.processBacking);
        mapped.processBacking = nullptr;
    }
}

bool mapProcessCodeImages(ElfLoaderSession* result) {
    const Handle process = envGetOwnProcessHandle();
    if (!envIsSyscallHinted(0x73) || !envIsSyscallHinted(0x77) ||
        !envIsSyscallHinted(0x78) || process == INVALID_HANDLE) {
        return false;
    }
    result->segmentedMapperMode = 1;
    result->permissionRanges = 0;
    result->permissionRangesVerified = 0;
    result->permissionFailures = 0;

    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        CrossImage& image = gLoaderImages[imageIndex];
        SegmentedImageMapping& mapped = gSegmentedImages[imageIndex];
        mapped.processBacking = memalign(kPageSize, image.image.size());
        result->segmentedFailureImage = imageIndex;
        result->segmentedFailureRegion = imageIndex;
        if (mapped.processBacking == nullptr) {
            result->segmentedFailureStage = 2;
            ++result->segmentedFailures;
            unmapProcessCodeImages();
            return false;
        }
        std::memcpy(mapped.processBacking, image.image.data(), image.image.size());
        armDCacheFlush(mapped.processBacking, image.image.size());
        MemoryInfo backingInfo{};
        MemoryInfo targetInfo{};
        u32 pageInfo = 0;
        svcQueryMemory(&backingInfo, &pageInfo,
                       reinterpret_cast<uint64_t>(mapped.processBacking));
        svcQueryMemory(&targetInfo, &pageInfo,
                       reinterpret_cast<uint64_t>(mapped.targetBase));
        result->processBackingType = backingInfo.type;
        result->processBackingPermission = backingInfo.perm;
        result->processBackingAttribute = backingInfo.attr;
        result->processTargetType = targetInfo.type;
        result->processTargetPermission = targetInfo.perm;
        const Result mapResult = svcMapProcessCodeMemory(
            process,
            reinterpret_cast<uint64_t>(mapped.targetBase),
            reinterpret_cast<uint64_t>(mapped.processBacking),
            image.image.size());
        if (R_FAILED(mapResult)) {
            result->processMapFailureImage = imageIndex;
            result->processMapFailureStage = 20;
            result->processMapFailureResult = mapResult;
            result->segmentedFailureStage = 20;
            result->segmentedFailureResult = mapResult;
            ++result->segmentedFailures;
            unmapProcessCodeImages();
            return false;
        }
        mapped.processMapped = true;
    }

    const auto setRange = [&](size_t imageIndex, uint64_t virtualAddress,
                              uint64_t bytes, u32 permission) {
        const CrossImage& image = gLoaderImages[imageIndex];
        const uint64_t start = alignDown(virtualAddress);
        uint64_t end = 0;
        if (!alignUp(virtualAddress + bytes, &end) || end <= start) {
            result->segmentedFailureStage = 21;
            ++result->permissionFailures;
            return false;
        }
        void* address = reinterpret_cast<void*>(image.loadBias() + start);
        ++result->permissionRanges;
        Result operation = svcSetProcessMemoryPermission(
            process, reinterpret_cast<uint64_t>(address), end - start, permission);
        if (R_FAILED(operation)) {
            result->processMapFailureImage = imageIndex;
            result->processMapFailureStage = 21;
            result->processMapFailureResult = operation;
            result->segmentedFailureImage = imageIndex;
            result->segmentedFailureStage = 21;
            result->segmentedFailureResult = operation;
            result->segmentedExpectedPermission = permission;
            ++result->permissionFailures;
            return false;
        }
        MemoryInfo memoryInfo{};
        u32 pageInfo = 0;
        operation = svcQueryMemory(
            &memoryInfo, &pageInfo, reinterpret_cast<uint64_t>(address));
        result->segmentedActualPermission = memoryInfo.perm;
        if (R_FAILED(operation) || memoryInfo.perm != permission) {
            result->processMapFailureImage = imageIndex;
            result->processMapFailureStage = 22;
            result->processMapFailureResult = operation;
            result->segmentedFailureImage = imageIndex;
            result->segmentedFailureStage = 22;
            result->segmentedFailureResult = operation;
            result->segmentedExpectedPermission = permission;
            ++result->permissionFailures;
            return false;
        }
        ++result->permissionRangesVerified;
        return true;
    };

    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        const CrossImage& image = gLoaderImages[imageIndex];
        const size_t pageCount = image.image.size() / kPageSize;
        std::vector<u32> pagePermissions(pageCount, Perm_None);
        for (const GuestElf64ProgramHeader& ph : image.programHeaders) {
            if (ph.type != kPtLoad) {
                continue;
            }
            const uint64_t start = alignDown(ph.virtualAddress) - image.minimumAddress;
            uint64_t endAddress = 0;
            if (!alignUp(ph.virtualAddress + ph.memorySize, &endAddress)) {
                result->processMapFailureImage = imageIndex;
                result->processMapFailureStage = 23;
                unmapProcessCodeImages();
                return false;
            }
            const uint64_t end = endAddress - image.minimumAddress;
            const u32 finalPermission = permissionForFlags(ph.flags);
            for (uint64_t offset = start; offset < end; offset += kPageSize) {
                const size_t page = static_cast<size_t>(offset / kPageSize);
                if (page >= pagePermissions.size() ||
                    (pagePermissions[page] != Perm_None &&
                     pagePermissions[page] != finalPermission)) {
                    result->processMapFailureImage = imageIndex;
                    result->processMapFailureStage = 23;
                    unmapProcessCodeImages();
                    return false;
                }
                pagePermissions[page] = finalPermission;
            }
        }
        if (image.relroSize != 0) {
            const uint64_t start = alignDown(image.relroAddress) - image.minimumAddress;
            uint64_t endAddress = 0;
            if (!alignUp(image.relroAddress + image.relroSize, &endAddress)) {
                result->processMapFailureImage = imageIndex;
                result->processMapFailureStage = 23;
                unmapProcessCodeImages();
                return false;
            }
            const uint64_t end = endAddress - image.minimumAddress;
            for (uint64_t offset = start; offset < end; offset += kPageSize) {
                const size_t page = static_cast<size_t>(offset / kPageSize);
                if (page >= pagePermissions.size() || pagePermissions[page] == Perm_None) {
                    result->processMapFailureImage = imageIndex;
                    result->processMapFailureStage = 23;
                    unmapProcessCodeImages();
                    return false;
                }
                pagePermissions[page] = Perm_R;
            }
        }
        size_t page = 0;
        while (page < pagePermissions.size()) {
            const u32 finalPermission = pagePermissions[page];
            size_t endPage = page + 1;
            while (endPage < pagePermissions.size() &&
                   pagePermissions[endPage] == finalPermission) {
                ++endPage;
            }
            if (finalPermission != Perm_None &&
                !setRange(imageIndex,
                          image.minimumAddress + static_cast<uint64_t>(page) * kPageSize,
                          static_cast<uint64_t>(endPage - page) * kPageSize,
                          finalPermission)) {
                unmapProcessCodeImages();
                return false;
            }
            page = endPage;
        }
        for (const GuestElf64ProgramHeader& ph : image.programHeaders) {
            if (ph.type == kPtLoad && (ph.flags & kPfExecute) != 0) {
                const uint64_t start = alignDown(ph.virtualAddress);
                uint64_t end = 0;
                if (alignUp(ph.virtualAddress + ph.memorySize, &end)) {
                    armICacheInvalidate(
                        reinterpret_cast<void*>(image.loadBias() + start), end - start);
                }
            }
        }
    }

    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        const CrossImage& image = gLoaderImages[imageIndex];
        for (const GuestElf64ProgramHeader& ph : image.programHeaders) {
            if (ph.type != kPtLoad) {
                continue;
            }
            const uint64_t start = alignDown(ph.virtualAddress) - image.minimumAddress;
            uint64_t endAddress = 0;
            if (!alignUp(ph.virtualAddress + ph.memorySize, &endAddress)) {
                result->segmentedFailureStage = 23;
                ++result->segmentedFailures;
                unmapProcessCodeImages();
                return false;
            }
            const uint64_t bytes = endAddress - image.minimumAddress - start;
            const void* target = reinterpret_cast<const void*>(
                image.executableStart() + start);
            if (std::memcmp(target, image.image.data() + start, bytes) != 0) {
                result->segmentedFailureImage = imageIndex;
                result->segmentedFailureStage = 24;
                ++result->segmentedFailures;
                unmapProcessCodeImages();
                return false;
            }
        }
    }

    result->segmentedRegions = gLoaderImageCount;
    result->segmentedRegionsVerified = gLoaderImageCount;
    result->segmentedMapperSucceeded = true;
    result->segmentPermissionsEnforced = true;
    result->permissionFailures = 0;
    gSegmentedSessionActive = true;
    return true;
}

[[maybe_unused]] bool prepareLoaderJits(ElfLoaderSession* result) {
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        CrossImage& image = gLoaderImages[index];
        const Result createResult = jitCreate(&gLoaderJits[index], image.image.size());
        if (R_FAILED(createResult)) {
            ++result->jitCreateFailures;
            setError(result, "loader JIT image allocation failed");
            return false;
        }
        gLoaderJitCreated[index] = true;
        image.writableLinkBase = static_cast<unsigned char*>(
            jitGetRwAddr(&gLoaderJits[index]));
        image.executableLinkBase = static_cast<unsigned char*>(
            jitGetRxAddr(&gLoaderJits[index]));
        std::memcpy(image.writableLinkBase, image.image.data(), image.image.size());
    }
    return true;
}

bool findExportInLoaderScope(size_t currentImage, const char* name,
                             uintptr_t* address, uint64_t* symbolValue) {
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        if (index == currentImage) {
            continue;
        }
        if (findExport(gLoaderImages[index], name, address, symbolValue)) {
            return true;
        }
    }
    return false;
}

struct LoaderApplyContext {
    ElfLoaderSession* result;
    std::vector<std::string>* hostNames;
    std::vector<std::string>* crossNames;
    std::vector<std::string>* missingNames;
    size_t imageIndex;
};

void applyLoaderTable(CrossImage* image, uint64_t address, uint64_t bytes,
                      LoaderApplyContext context) {
    if (bytes == 0) {
        return;
    }
    if (!image->containsFileData(address, bytes) || bytes % sizeof(Elf64Rela) != 0) {
        ++context.result->invalidRelocations;
        return;
    }
    const uint64_t count = bytes / sizeof(Elf64Rela);
    for (uint64_t index = 0; index < count; ++index) {
        Elf64Rela relocation{};
        std::memcpy(&relocation, image->pointer(address + index * sizeof(relocation)),
                    sizeof(relocation));
        const uint32_t type = static_cast<uint32_t>(relocation.info);
        const uint64_t symbolIndex = relocation.info >> 32;
        if (!image->containsMemory(relocation.offset, sizeof(uint64_t)) ||
            (relocation.offset & 7U) != 0) {
            ++context.result->invalidRelocations;
            continue;
        }

        Provider provider = Provider::Missing;
        const char* name = "";
        uint64_t symbolValue = 0;
        uintptr_t resolvedAddress = 0;
        bool patch = false;
        if (type == kRAarch64Relative) {
            if (symbolIndex != 0) {
                ++context.result->invalidRelocations;
                continue;
            }
            provider = Provider::Relative;
            resolvedAddress = image->loadBias();
            patch = true;
            ++context.result->relativeApplied;
        }
        else if (type == kRAarch64Abs64 || type == kRAarch64GlobDat ||
                 type == kRAarch64JumpSlot) {
            Elf64Symbol symbol{};
            if (symbolIndex == 0 || !readSymbol(*image, symbolIndex, &symbol, &name) ||
                name[0] == '\0') {
                ++context.result->invalidRelocations;
                continue;
            }
            symbolValue = symbol.value;
            if (symbol.sectionIndex != kShnUndefined) {
                provider = Provider::Internal;
                resolvedAddress = symbol.sectionIndex == kShnAbsolute
                    ? static_cast<uintptr_t>(symbol.value)
                    : image->loadBias() + symbol.value;
                patch = true;
                ++context.result->internalApplied;
            }
            else if (findExportInLoaderScope(context.imageIndex, name,
                                             &resolvedAddress, &symbolValue)) {
                provider = Provider::Cross;
                patch = true;
                ++context.result->crossRelocations;
                addUnique(context.crossNames, name);
            }
            else if ((resolvedAddress = resolveHostSymbol(name)) != 0) {
                provider = Provider::Host;
                patch = true;
                ++context.result->hostRelocations;
                addUnique(context.hostNames, name);
            }
            else if ((symbol.info >> 4) == kBindWeak) {
                provider = Provider::WeakZero;
                patch = true;
                ++context.result->weakZeroRelocations;
            }
            else {
                ++context.result->missingRelocations;
                addUnique(context.missingNames, name);
            }
        }
        else {
            ++context.result->invalidRelocations;
            continue;
        }

        const uint8_t imageTag = static_cast<uint8_t>(context.imageIndex);
        const unsigned char providerTag = static_cast<unsigned char>(provider);
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &imageTag, sizeof(imageTag));
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &providerTag, sizeof(providerTag));
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &type, sizeof(type));
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &relocation.addend, sizeof(relocation.addend));
        context.result->resolutionPlanHash = appendBytes(
            context.result->resolutionPlanHash, &symbolValue, sizeof(symbolValue));
        context.result->resolutionPlanHash = appendText(
            context.result->resolutionPlanHash, name);

        if (!patch) {
            continue;
        }
        uint64_t value = static_cast<uint64_t>(resolvedAddress);
        if (type == kRAarch64Relative || type == kRAarch64Abs64) {
            value += static_cast<uint64_t>(relocation.addend);
        }
        std::memcpy(image->pointer(relocation.offset), &value, sizeof(value));
        ++context.result->patchedRelocations;
        uint64_t readBack = 0;
        std::memcpy(&readBack, image->pointer(relocation.offset), sizeof(readBack));
        if (readBack == value) {
            ++context.result->verifiedWrites;
        }
        else {
            ++context.result->invalidRelocations;
        }
    }
}

void verifyLoaderLifecycleArray(const CrossImage& image, uint64_t address,
                                uint64_t bytes, uint64_t* entries,
                                ElfLoaderSession* result) {
    if (bytes == 0) {
        return;
    }
    *entries += bytes / sizeof(uint64_t);
    const uintptr_t imageStart = image.executableStart();
    const uintptr_t imageEnd = imageStart + image.image.size();
    for (uint64_t offset = 0; offset < bytes; offset += sizeof(uint64_t)) {
        uint64_t target = 0;
        std::memcpy(&target, image.pointer(address + offset), sizeof(target));
        if (target >= imageStart && target < imageEnd && (target & 3U) == 0) {
            ++result->lifecycleTargetsVerified;
        }
        else {
            ++result->invalidRelocations;
        }
    }
}

bool isVirtualizedNeeded(const std::string& name) {
    constexpr const char* kVirtualized[] = {
        "libandroid.so", "libc.so", "libdl.so", "libEGL.so", "libGLESv2.so",
        "liblog.so", "libm.so", "libz.so", "libarcore_sdk_c.so",
        "libarcore_sdk_jni.so", "libfuelmetrics.so",
        // FMOD Android backends. Horizon has no OpenSL/AAudio device; the
        // fmod_audio_stub provides silent/error entry points via guestDlopen.
        "libOpenSLES.so", "libaaudio.so",
    };
    for (const char* candidate : kVirtualized) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

bool hasEmbeddedSoname(const std::string& name) {
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        if (gLoaderImages[index].soname == name) {
            return true;
        }
    }
    return false;
}

size_t embeddedSonameIndex(const std::string& name) {
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        if (gLoaderImages[index].soname == name) {
            return index;
        }
    }
    return gLoaderImageCount;
}

bool visitLifecycleOrder(size_t imageIndex, std::vector<uint8_t>* states,
                         std::vector<size_t>* order, ElfLoaderSession* result) {
    if ((*states)[imageIndex] == 2) {
        return true;
    }
    if ((*states)[imageIndex] == 1) {
        ++result->dependencyCycles;
        return false;
    }
    (*states)[imageIndex] = 1;
    for (const std::string& needed : gLoaderImages[imageIndex].neededNames) {
        const size_t dependency = embeddedSonameIndex(needed);
        if (dependency != gLoaderImageCount &&
            !visitLifecycleOrder(dependency, states, order, result)) {
            return false;
        }
    }
    (*states)[imageIndex] = 2;
    order->push_back(imageIndex);
    return true;
}

bool buildLifecycleOrder(ElfLoaderSession* result) {
    std::vector<uint8_t> states(gLoaderImageCount, 0);
    std::vector<size_t> order;
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        if (!visitLifecycleOrder(index, &states, &order, result)) {
            setError(result, "embedded dependency cycle");
            return false;
        }
    }

    gLifecycleOrderCount = order.size();
    for (size_t index = 0; index < kMaximumLoaderImages; ++index) {
        gLifecycleOrder[index] = index < order.size() ? order[index] : 0;
        gGuestImageInitialized[index] = false;
    }

    result->constructorOrderHash = 1469598103934665603ULL;
    for (size_t index : order) {
        const CrossImage& image = gLoaderImages[index];
        const uint64_t entries = image.initArraySize / sizeof(uint64_t);
        result->constructorOrderEntries += entries;
        result->constructorOrderHash = appendText(
            result->constructorOrderHash, image.soname.c_str());
        result->constructorOrderHash = appendBytes(
            result->constructorOrderHash, &entries, sizeof(entries));
    }

    result->finalizerOrderHash = 1469598103934665603ULL;
    for (auto cursor = order.rbegin(); cursor != order.rend(); ++cursor) {
        const CrossImage& image = gLoaderImages[*cursor];
        const uint64_t entries = image.finiArraySize / sizeof(uint64_t);
        result->finalizerOrderEntries += entries;
        result->finalizerOrderHash = appendText(
            result->finalizerOrderHash, image.soname.c_str());
        result->finalizerOrderHash = appendBytes(
            result->finalizerOrderHash, &entries, sizeof(entries));
    }
    return true;
}

void auditRelro(const CrossImage& image, ElfLoaderSession* result) {
    if (image.relroSize == 0) {
        return;
    }
    ++result->relroRanges;
    result->relroBytes += image.relroSize;
    if (image.relroAddress < image.minimumAddress ||
        image.relroAddress > image.maximumAddress ||
        image.relroSize > image.maximumAddress - image.relroAddress) {
        ++result->invalidRelocations;
        return;
    }
    const uint64_t end = image.relroAddress + image.relroSize;
    const auto countTable = [&](uint64_t address, uint64_t bytes) {
        for (uint64_t offset = 0; offset < bytes; offset += sizeof(Elf64Rela)) {
            Elf64Rela relocation{};
            std::memcpy(&relocation, image.pointer(address + offset), sizeof(relocation));
            if (relocation.offset >= image.relroAddress &&
                relocation.offset <= end - sizeof(uint64_t)) {
                ++result->relroTargetsVerified;
            }
        }
    };
    countTable(image.relaAddress, image.relaSize);
    countTable(image.jumpRelAddress, image.jumpRelSize);
}

u32 permissionForFlags(uint32_t flags) {
    if ((flags & kPfExecute) != 0) {
        return Perm_Rx;
    }
    if ((flags & kPfWrite) != 0) {
        return Perm_Rw;
    }
    return Perm_R;
}

void applyPermissionRange(const CrossImage& image, uint64_t virtualAddress,
                          uint64_t bytes, u32 permission, Handle process,
                          ElfLoaderSession* result) {
    if (bytes == 0) {
        return;
    }
    const uint64_t start = alignDown(virtualAddress);
    uint64_t end = 0;
    if (!alignUp(virtualAddress + bytes, &end) || end <= start) {
        ++result->permissionFailures;
        return;
    }
    void* address = reinterpret_cast<void*>(image.loadBias() + start);
    ++result->permissionRanges;
    const Result setResult = svcSetProcessMemoryPermission(
        process, reinterpret_cast<uint64_t>(address), end - start, permission);
    if (R_FAILED(setResult)) {
        ++result->permissionFailures;
        return;
    }
    MemoryInfo memoryInfo{};
    u32 pageInfo = 0;
    const Result queryResult = svcQueryMemory(
        &memoryInfo, &pageInfo, reinterpret_cast<uint64_t>(address));
    if (R_FAILED(queryResult) || memoryInfo.perm != permission) {
        ++result->permissionFailures;
        return;
    }
    ++result->permissionRangesVerified;
}

[[maybe_unused]] void enforceSegmentPermissions(ElfLoaderSession* result) {
    const Handle process = envGetOwnProcessHandle();
    if (envIsSyscallHinted(0x73) && process != INVALID_HANDLE) {
        result->permissionApiAvailable = true;
        for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
            const CrossImage& image = gLoaderImages[imageIndex];
            applyPermissionRange(image, image.minimumAddress, image.image.size(),
                                 Perm_None, process, result);
            for (const GuestElf64ProgramHeader& ph : image.programHeaders) {
                if (ph.type == kPtLoad) {
                    applyPermissionRange(image, ph.virtualAddress, ph.memorySize,
                                         permissionForFlags(ph.flags), process, result);
                }
            }
            if (image.relroSize != 0) {
                applyPermissionRange(image, image.relroAddress, image.relroSize,
                                     Perm_R, process, result);
            }
        }
        result->segmentPermissionsEnforced = result->permissionRanges != 0 &&
            result->permissionFailures == 0 &&
            result->permissionRangesVerified == result->permissionRanges;
        if (result->segmentPermissionsEnforced) {
            return;
        }
        result->processPermissionFailures = result->permissionFailures;
        if (result->permissionRangesVerified != 0) {
            return;
        }
    }

    result->permissionFallbackUsed = true;
    result->permissionRanges = 0;
    result->permissionRangesVerified = 0;
    result->permissionFailures = 0;

    const auto applyCurrentRange = [&](const CrossImage& image, uint64_t virtualAddress,
                                       uint64_t bytes, u32 permission, bool keepExistingRx) {
        if (bytes == 0) {
            return;
        }
        const uint64_t start = alignDown(virtualAddress);
        uint64_t end = 0;
        if (!alignUp(virtualAddress + bytes, &end) || end <= start) {
            ++result->permissionFailures;
            return;
        }
        void* address = reinterpret_cast<void*>(image.loadBias() + start);
        ++result->permissionRanges;
        if (!keepExistingRx) {
            const Result setResult = svcSetMemoryPermission(address, end - start, permission);
            if (R_FAILED(setResult)) {
                ++result->permissionFailures;
                return;
            }
        }
        MemoryInfo memoryInfo{};
        u32 pageInfo = 0;
        const Result queryResult = svcQueryMemory(
            &memoryInfo, &pageInfo, reinterpret_cast<uint64_t>(address));
        if (R_FAILED(queryResult) || memoryInfo.perm != permission) {
            ++result->permissionFailures;
            return;
        }
        ++result->permissionRangesVerified;
    };

    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        const CrossImage& image = gLoaderImages[imageIndex];
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        for (const GuestElf64ProgramHeader& ph : image.programHeaders) {
            if (ph.type != kPtLoad) {
                continue;
            }
            const uint64_t start = alignDown(ph.virtualAddress);
            uint64_t end = 0;
            if (!alignUp(ph.virtualAddress + ph.memorySize, &end)) {
                ++result->permissionFailures;
                continue;
            }
            ranges.emplace_back(start, end);
            const u32 permission = permissionForFlags(ph.flags);
            applyCurrentRange(image, ph.virtualAddress, ph.memorySize, permission,
                              permission == Perm_Rx);
        }
        std::sort(ranges.begin(), ranges.end());
        uint64_t cursor = image.minimumAddress;
        for (const auto& range : ranges) {
            if (cursor < range.first) {
                applyCurrentRange(image, cursor, range.first - cursor, Perm_None, false);
            }
            cursor = std::max(cursor, range.second);
        }
        if (cursor < image.maximumAddress) {
            applyCurrentRange(image, cursor, image.maximumAddress - cursor,
                              Perm_None, false);
        }
        if (image.relroSize != 0) {
            applyCurrentRange(image, image.relroAddress, image.relroSize, Perm_R, false);
        }
    }
    result->segmentPermissionsEnforced = result->permissionRanges != 0 &&
        result->permissionFailures == 0 &&
        result->permissionRangesVerified == result->permissionRanges;
}

[[maybe_unused]] bool trySegmentedRemap(ElfLoaderSession* result) {
    result->segmentedMapperAttempted = true;
    if (!closeWholeImageJitsPreserveImages()) {
        result->segmentedFailureStage = 10;
        ++result->segmentedFailures;
        return false;
    }
    closeSegmentedMappings();
    if (!reserveSegmentedTargets(result)) {
        closeSegmentedMappings();
        return false;
    }

    ElfLoaderSession verification{};
    verification.resolutionPlanHash = 1469598103934665603ULL;
    std::vector<std::string> hostNames;
    std::vector<std::string> crossNames;
    std::vector<std::string> missingNames;
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        CrossImage& image = gLoaderImages[index];
        LoaderApplyContext context{
            &verification, &hostNames, &crossNames, &missingNames, index};
        applyLoaderTable(&image, image.relaAddress, image.relaSize, context);
        applyLoaderTable(&image, image.jumpRelAddress, image.jumpRelSize, context);
        verifyLoaderLifecycleArray(image, image.initArrayAddress, image.initArraySize,
                                   &verification.constructors, &verification);
        verifyLoaderLifecycleArray(image, image.finiArrayAddress, image.finiArraySize,
                                   &verification.finalizers, &verification);
        auditRelro(image, &verification);
    }
    if (verification.invalidRelocations != 0 ||
        verification.patchedRelocations != result->patchedRelocations ||
        verification.verifiedWrites != result->verifiedWrites ||
        verification.resolutionPlanHash != result->resolutionPlanHash ||
        verification.lifecycleTargetsVerified != result->lifecycleTargetsVerified ||
        verification.relroTargetsVerified != result->relroTargetsVerified) {
        result->segmentedFailureStage = 11;
        ++result->segmentedFailures;
        closeSegmentedMappings();
        return false;
    }
    if (mapProcessCodeImages(result)) {
        result->permissionFailures = 0;
        result->executableAliases = true;
        result->retained = true;
        return true;
    }
    closeSegmentedMappings();
    return false;
}

bool isMainGameImage(const CrossImage& image) {
    return image.soname == "libRealRacing3.so";
}

bool lifecycleTarget(const CrossImage& image, uint64_t arrayAddress,
                     uint64_t ordinal, uintptr_t* target) {
    if (target == nullptr || arrayAddress == 0 ||
        ordinal > (UINT64_MAX - arrayAddress) / sizeof(uint64_t)) {
        return false;
    }
    uint64_t rawTarget = 0;
    std::memcpy(&rawTarget,
                image.pointer(arrayAddress + ordinal * sizeof(uint64_t)),
                sizeof(rawTarget));
    if ((rawTarget & 3U) != 0) {
        return false;
    }
    for (const GuestElf64ProgramHeader& ph : image.programHeaders) {
        if (ph.type != kPtLoad || (ph.flags & kPfExecute) == 0) {
            continue;
        }
        const uintptr_t start = image.loadBias() + ph.virtualAddress;
        if (rawTarget >= start && rawTarget < start + ph.memorySize) {
            *target = static_cast<uintptr_t>(rawTarget);
            return true;
        }
    }
    return false;
}

bool finalizeInitializedGuestImages(ElfLoaderUnload* result) {
    for (size_t reverseOrder = gLifecycleOrderCount; reverseOrder-- > 0;) {
        const size_t imageIndex = gLifecycleOrder[reverseOrder];
        if (imageIndex >= gLoaderImageCount || !gGuestImageInitialized[imageIndex]) {
            continue;
        }
        const CrossImage& image = gLoaderImages[imageIndex];
        const uint64_t count = image.finiArraySize / sizeof(uint64_t);
        result->finalizersPlanned += count;
        for (uint64_t reverseOrdinal = count; reverseOrdinal-- > 0;) {
            uintptr_t target = 0;
            if (!lifecycleTarget(image, image.finiArrayAddress,
                                 reverseOrdinal, &target)) {
                ++result->failures;
                if (result->firstFailureResult == 0) {
                    result->firstFailureResult =
                        MAKERESULT(Module_Libnx, LibnxError_BadReloc);
                }
                return false;
            }
        }
    }

    GuestTlsScope tlsScope;
    if (!tlsScope.active()) {
        ++result->failures;
        if (result->firstFailureResult == 0) {
            result->firstFailureResult =
                MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
        }
        return false;
    }
    for (size_t reverseOrder = gLifecycleOrderCount; reverseOrder-- > 0;) {
        const size_t imageIndex = gLifecycleOrder[reverseOrder];
        if (imageIndex >= gLoaderImageCount || !gGuestImageInitialized[imageIndex]) {
            continue;
        }
        const CrossImage& image = gLoaderImages[imageIndex];
        const uint64_t count = image.finiArraySize / sizeof(uint64_t);
        for (uint64_t reverseOrdinal = count; reverseOrdinal-- > 0;) {
            uintptr_t target = 0;
            if (!lifecycleTarget(image, image.finiArrayAddress,
                                 reverseOrdinal, &target)) {
                return false;
            }
            reinterpret_cast<void (*)()>(target)();
            ++result->finalizersCalled;
        }
        gGuestImageInitialized[imageIndex] = false;
    }
    return true;
}

int countPhdrCallback(GuestDlPhdrInfo* info, size_t size, void* data) {
    auto* state = static_cast<uint64_t*>(data);
    if (size == sizeof(GuestDlPhdrInfo) && info != nullptr && info->name != nullptr &&
        info->programHeaders != nullptr && info->programHeaderCount != 0) {
        ++state[0];
        if (std::strstr(info->name, "libRealRacing3.so") != nullptr ||
            std::strstr(info->name, "libc++_shared.so") != nullptr) {
            ++state[1];
        }
    }
    return 0;
}

int stopPhdrCallback(GuestDlPhdrInfo*, size_t, void*) {
    return 77;
}

} // namespace

ElfCrossLink crossLinkElfPair(const char* primaryPath, const char* dependencyPath) {
    ElfCrossLink result{};
    result.resolutionPlanHash = 1469598103934665603ULL;
    const bool primaryLoaded = loadImage(primaryPath, &gPrimaryImage, &result);
    result.primaryOpened = gPrimaryImage.opened;
    result.primaryLoaded = primaryLoaded;
    if (!primaryLoaded) {
        return result;
    }
    const bool dependencyLoaded = loadImage(dependencyPath, &gDependencyImage, &result);
    result.dependencyOpened = gDependencyImage.opened;
    result.dependencyLoaded = dependencyLoaded;
    if (!dependencyLoaded) {
        return result;
    }
    result.primaryImageBytes = gPrimaryImage.image.size();
    result.dependencyImageBytes = gDependencyImage.image.size();

    std::vector<std::string> hostNames;
    std::vector<std::string> crossNames;
    std::vector<std::string> missingNames;
    ApplyContext primaryContext{&result, &hostNames, &crossNames, &missingNames, true};
    applyTable(&gPrimaryImage, gDependencyImage, gPrimaryImage.relaAddress,
               gPrimaryImage.relaSize, primaryContext);
    applyTable(&gPrimaryImage, gDependencyImage, gPrimaryImage.jumpRelAddress,
               gPrimaryImage.jumpRelSize, primaryContext);
    ApplyContext dependencyContext{&result, &hostNames, &crossNames, &missingNames, false};
    applyTable(&gDependencyImage, gPrimaryImage, gDependencyImage.relaAddress,
               gDependencyImage.relaSize, dependencyContext);
    applyTable(&gDependencyImage, gPrimaryImage, gDependencyImage.jumpRelAddress,
               gDependencyImage.jumpRelSize, dependencyContext);

    verifyLifecycleArray(gPrimaryImage, gPrimaryImage.initArrayAddress,
                         gPrimaryImage.initArraySize, &result.primaryConstructors, &result);
    verifyLifecycleArray(gDependencyImage, gDependencyImage.initArrayAddress,
                         gDependencyImage.initArraySize, &result.dependencyConstructors, &result);
    verifyLifecycleArray(gPrimaryImage, gPrimaryImage.finiArrayAddress,
                         gPrimaryImage.finiArraySize, &result.finalizers, &result);
    verifyLifecycleArray(gDependencyImage, gDependencyImage.finiArrayAddress,
                         gDependencyImage.finiArraySize, &result.finalizers, &result);

    result.uniqueHostSymbols = hostNames.size();
    result.uniqueCrossSymbols = crossNames.size();
    result.uniqueMissingSymbols = missingNames.size();
    result.retained = true;
    result.completed = result.invalidRelocations == 0 &&
        result.verifiedWrites == result.patchedRelocations;
    if (!result.completed) {
        setError(&result, "cross-link relocation validation failed");
    }
    return result;
}

ElfLoaderSession loadLinkElfSet(const char* const* paths, size_t pathCount) {
    ElfLoaderSession result{};
    result.resolutionPlanHash = 1469598103934665603ULL;
    closeLoaderJits();
    gLifecycleOrderCount = 0;
    gLoaderResolutionPlanHash = 0;
    for (bool& initialized : gGuestImageInitialized) {
        initialized = false;
    }
    if (paths == nullptr || pathCount == 0 || pathCount > kMaximumLoaderImages) {
        setError(&result, "invalid loader image set");
        return result;
    }

    gLoaderImageCount = pathCount;
    for (size_t index = 0; index < pathCount; ++index) {
        ElfCrossLink loadResult{};
        if (!loadImage(paths[index], &gLoaderImages[index], &loadResult)) {
            setError(&result, loadResult.error[0] != '\0'
                ? loadResult.error
                : "loader image parse failed");
            closeLoaderJits();
            return result;
        }
        const CrossImage& image = gLoaderImages[index];
        result.totalImageBytes += image.image.size();
        result.loadSegments += image.loadSegments;
        result.executableSegments += image.executableSegments;
        result.writableSegments += image.writableSegments;
        result.readOnlySegments += image.readOnlySegments;
        result.writableExecutableSegments += image.writableExecutableSegments;
        result.tlsSegments += image.tlsSegments;
        result.tlsBytes += image.tlsBytes;
    }
    result.imageCount = pathCount;

    for (size_t index = 0; index < pathCount; ++index) {
        for (const std::string& needed : gLoaderImages[index].neededNames) {
            ++result.neededEntries;
            if (hasEmbeddedSoname(needed)) {
                ++result.neededEmbedded;
            }
            else if (isVirtualizedNeeded(needed)) {
                ++result.neededVirtualized;
            }
            else {
                ++result.neededMissing;
            }
        }
    }
    if (result.neededMissing != 0) {
        setError(&result, "unavailable DT_NEEDED dependency");
        closeLoaderJits();
        return result;
    }
    if (!buildLifecycleOrder(&result)) {
        closeLoaderJits();
        return result;
    }
    result.segmentedMapperAttempted = true;
    result.segmentedMapperMode = 1;
    result.permissionApiAvailable = envIsSyscallHinted(0x73) &&
        envGetOwnProcessHandle() != INVALID_HANDLE;
    closeSegmentedMappings();
    if (!reserveSegmentedTargets(&result)) {
        setError(&result, "could not reserve loader guest ranges");
        closeSegmentedMappings();
        gLoaderImageCount = 0;
        return result;
    }

    std::vector<std::string> hostNames;
    std::vector<std::string> crossNames;
    std::vector<std::string> missingNames;
    for (size_t index = 0; index < pathCount; ++index) {
        CrossImage& image = gLoaderImages[index];
        LoaderApplyContext context{
            &result, &hostNames, &crossNames, &missingNames, index};
        applyLoaderTable(&image, image.relaAddress, image.relaSize, context);
        applyLoaderTable(&image, image.jumpRelAddress, image.jumpRelSize, context);
        verifyLoaderLifecycleArray(image, image.initArrayAddress, image.initArraySize,
                                   &result.constructors, &result);
        verifyLoaderLifecycleArray(image, image.finiArrayAddress, image.finiArraySize,
                                   &result.finalizers, &result);
        auditRelro(image, &result);
    }
    result.uniqueHostSymbols = hostNames.size();
    result.uniqueCrossSymbols = crossNames.size();
    result.uniqueMissingSymbols = missingNames.size();

    if (result.invalidRelocations != 0 ||
        result.verifiedWrites != result.patchedRelocations) {
        setError(&result, "loader relocation or lifecycle validation failed");
        closeSegmentedMappings();
        gLoaderImageCount = 0;
        return result;
    }

    if (!mapProcessCodeImages(&result)) {
        setError(&result, "process-code loader mapping failed");
        result.executableAliases = false;
        result.retained = false;
        result.completed = false;
        closeSegmentedMappings();
        gLoaderImageCount = 0;
        return result;
    }

    result.executableAliases = true;
    result.retained = true;
    result.completed = true;
    result.activationReady = result.missingRelocations == 0 &&
        result.neededMissing == 0 && result.writableExecutableSegments == 0 &&
        result.tlsSegments == 0 && result.segmentPermissionsEnforced;
    gLoaderResolutionPlanHash = result.resolutionPlanHash;
    return result;
}

GuestStartupPlan captureGuestStartupPlan() {
    GuestStartupPlan plan{};
    if (!gSegmentedSessionActive || gLoaderImageCount == 0 ||
        gLifecycleOrderCount != gLoaderImageCount) {
        return plan;
    }
    plan.available = true;
    plan.resolutionPlanHash = gLoaderResolutionPlanHash;
    plan.imageCount = gLifecycleOrderCount;
    for (size_t orderIndex = 0; orderIndex < gLifecycleOrderCount; ++orderIndex) {
        const size_t imageIndex = gLifecycleOrder[orderIndex];
        const CrossImage& image = gLoaderImages[imageIndex];
        GuestStartupImage& output = plan.images[orderIndex];
        output.base = image.executableStart();
        output.imageBytes = image.image.size();
        output.constructors = image.initArraySize / sizeof(uint64_t);
        output.finalizers = image.finiArraySize / sizeof(uint64_t);
        std::snprintf(output.soname, sizeof(output.soname), "%s",
                      image.soname.c_str());
        if (isMainGameImage(image)) {
            plan.mainConstructors += output.constructors;
        }
        else {
            plan.dependencyConstructors += output.constructors;
        }
    }
    return plan;
}

GuestInitializationResult initializeGuestDependencies() {
    GuestInitializationResult result{};
    result.attempted = true;
    result.stage = 1;
    if (!gSegmentedSessionActive || gLifecycleOrderCount != gLoaderImageCount ||
        gLoaderResolutionPlanHash == 0) {
        result.failureCode = 1;
        return result;
    }
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        if (gGuestImageInitialized[index]) {
            result.failureCode = 2;
            result.active = true;
            return result;
        }
    }

    result.stage = 2;
    for (size_t orderIndex = 0; orderIndex < gLifecycleOrderCount; ++orderIndex) {
        const size_t imageIndex = gLifecycleOrder[orderIndex];
        const CrossImage& image = gLoaderImages[imageIndex];
        if (isMainGameImage(image)) {
            continue;
        }
        const uint64_t count = image.initArraySize / sizeof(uint64_t);
        result.constructorsPlanned += count;
        for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
            uintptr_t target = 0;
            if (!lifecycleTarget(image, image.initArrayAddress, ordinal, &target)) {
                result.failureCode = 3;
                result.currentImage = imageIndex;
                result.currentOrdinal = ordinal;
                return result;
            }
        }
    }
    result.preflightPassed = true;

    result.stage = 3;
    GuestTlsScope tlsScope;
    if (!tlsScope.active()) {
        result.failureCode = 4;
        return result;
    }
    for (size_t orderIndex = 0; orderIndex < gLifecycleOrderCount; ++orderIndex) {
        const size_t imageIndex = gLifecycleOrder[orderIndex];
        const CrossImage& image = gLoaderImages[imageIndex];
        if (isMainGameImage(image)) {
            continue;
        }
        const uint64_t count = image.initArraySize / sizeof(uint64_t);
        for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
            uintptr_t target = 0;
            if (!lifecycleTarget(image, image.initArrayAddress, ordinal, &target)) {
                result.failureCode = 5;
                result.currentImage = imageIndex;
                result.currentOrdinal = ordinal;
                return result;
            }
            result.currentImage = imageIndex;
            result.currentOrdinal = ordinal;
            result.currentTarget = target;
            reinterpret_cast<void (*)()>(target)();
            ++result.constructorsCalled;
        }
        gGuestImageInitialized[imageIndex] = true;
    }
    result.stage = 4;
    result.completed = true;
    result.active = true;
    return result;
}

// Runs the RR3 main image's init_array (the 181 "main constructors") that
// initializeGuestDependencies deliberately skips. Same guardrails: every
// target is preflighted into an RX PT_LOAD, then called under GuestTlsScope
// with per-ordinal progress recorded so a crash report can be attributed.
GuestInitializationResult initializeGuestMainConstructors(
    const char* progressPath) {
    GuestInitializationResult result{};
    result.attempted = true;
    result.stage = 1;
    if (!gSegmentedSessionActive || gLifecycleOrderCount != gLoaderImageCount ||
        gLoaderResolutionPlanHash == 0) {
        result.failureCode = 1;
        return result;
    }
    const size_t mainIndex = embeddedSonameIndex("libRealRacing3.so");
    if (mainIndex >= gLoaderImageCount) {
        result.failureCode = 6;
        return result;
    }
    if (gGuestImageInitialized[mainIndex]) {
        result.failureCode = 2;
        result.active = true;
        return result;
    }
    const CrossImage& image = gLoaderImages[mainIndex];
    const uint64_t count = image.initArraySize / sizeof(uint64_t);

    result.stage = 2;
    result.constructorsPlanned = count;
    for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
        uintptr_t target = 0;
        if (!lifecycleTarget(image, image.initArrayAddress, ordinal, &target)) {
            result.failureCode = 3;
            result.currentImage = mainIndex;
            result.currentOrdinal = ordinal;
            return result;
        }
    }
    result.preflightPassed = true;

    result.stage = 3;
    // One persistent guest TLS scope across the whole loop (matching the proven
    // dependency-constructor path), so any thread-local an early ctor sets stays
    // valid for later ctors. The breadcrumb is fsync'd from inside the scope;
    // that is safe because the guest override only touches TPIDR_EL0 while
    // libnx/newlib reach their thread vars through TPIDRRO_EL0.
    GuestTlsScope tlsScope;
    if (!tlsScope.active()) {
        result.failureCode = 4;
        return result;
    }
    for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
        uintptr_t target = 0;
        if (!lifecycleTarget(image, image.initArrayAddress, ordinal, &target)) {
            result.failureCode = 5;
            result.currentImage = mainIndex;
            result.currentOrdinal = ordinal;
            return result;
        }
        result.currentImage = mainIndex;
        result.currentOrdinal = ordinal;
        result.currentTarget = target;
        // Durable breadcrumb before the risky call; if this ctor faults with no
        // saved crash report, the next launch reads back exactly which
        // ordinal/target died.
        if (progressPath != nullptr) {
            writeGuestBootProgress(progressPath, "main-ctor", ordinal, target);
        }
        reinterpret_cast<void (*)()>(target)();
        ++result.constructorsCalled;
    }
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "main-ctors-complete", count, 0);
    }
    gGuestImageInitialized[mainIndex] = true;
    result.stage = 4;
    result.completed = true;
    result.active = true;
    return result;
}

// Resolves an exported dynamic symbol from the main game image by name, so
// the host can call JNI lifecycle entry points (onCreateJNI etc.) directly.
uintptr_t resolveMainImageExport(const char* name) {
    if (name == nullptr) {
        return 0;
    }
    const size_t mainIndex = embeddedSonameIndex("libRealRacing3.so");
    if (mainIndex >= gLoaderImageCount) {
        return 0;
    }
    uintptr_t address = 0;
    uint64_t symbolValue = 0;
    if (!findExport(gLoaderImages[mainIndex], name, &address, &symbolValue)) {
        return 0;
    }
    return address;
}

uintptr_t resolveMainImageLoadBias() {
    const size_t mainIndex = embeddedSonameIndex("libRealRacing3.so");
    if (mainIndex >= gLoaderImageCount || !gLoaderImages[mainIndex].loaded) {
        return 0;
    }
    return gLoaderImages[mainIndex].loadBias();
}

// Collects the runtime address of `name` from every loaded guest image that
// defines it. Used for JNI_OnLoad, which Android's loader would normally call
// on each library and which caches the JavaVM pointer that later constructors
// dereference; here only libNimble exports it, but scanning all images keeps
// the host from assuming which one.
size_t collectGuestExports(const char* name, uintptr_t* out, size_t max) {
    if (name == nullptr || out == nullptr || max == 0) {
        return 0;
    }
    size_t found = 0;
    for (size_t index = 0; index < gLoaderImageCount && found < max; ++index) {
        uintptr_t address = 0;
        uint64_t symbolValue = 0;
        if (findExport(gLoaderImages[index], name, &address, &symbolValue)) {
            out[found++] = address;
        }
    }
    return found;
}

ElfLoaderUnload unloadLinkElfSet() {
    ElfLoaderUnload lifecycle{};
    bool anyInitialized = false;
    for (bool initialized : gGuestImageInitialized) {
        anyInitialized = anyInitialized || initialized;
    }
    if (anyInitialized) {
        lifecycle.attempted = true;
        if (!finalizeInitializedGuestImages(&lifecycle)) {
            lifecycle.completed = false;
            return lifecycle;
        }
    }

    ElfLoaderUnload result = closeSegmentedMappingsDetailed();
    result.attempted = result.attempted || lifecycle.attempted;
    result.finalizersPlanned = lifecycle.finalizersPlanned;
    result.finalizersCalled = lifecycle.finalizersCalled;
    if (!result.completed) {
        return result;
    }
    for (size_t index = 0; index < kMaximumLoaderImages; ++index) {
        if (gLoaderJitCreated[index]) {
            const Result operation = jitClose(&gLoaderJits[index]);
            if (R_FAILED(operation)) {
                ++result.failures;
                if (result.firstFailureResult == 0) {
                    result.firstFailureResult = operation;
                }
            }
            else {
                gLoaderJitCreated[index] = false;
                gLoaderJits[index] = {};
            }
        }
        if (!gLoaderJitCreated[index]) {
            gLoaderImages[index] = {};
        }
    }
    result.completed = result.failures == 0;
    if (result.completed) {
        gLoaderImageCount = 0;
        gLifecycleOrderCount = 0;
        gLoaderResolutionPlanHash = 0;
        for (bool& initialized : gGuestImageInitialized) {
            initialized = false;
        }
    }
    return result;
}

uint64_t loaderRollbackSelfTest() {
    uint64_t passed = 0;
    const char* paths[] = {
        "romfs:/libNimble.so",
        "romfs:/rr3-loader-intentionally-missing.so",
    };
    const ElfLoaderSession failed = loadLinkElfSet(paths, 2);
    if (!failed.completed && failed.error[0] != '\0') {
        passed |= 1ULL << 0;
    }
    if (!failed.retained && !failed.executableAliases) {
        passed |= 1ULL << 1;
    }
    if (gLoaderImageCount == 0) {
        passed |= 1ULL << 2;
    }
    bool anyJit = false;
    for (bool created : gLoaderJitCreated) {
        anyJit = anyJit || created;
    }
    if (!anyJit) {
        passed |= 1ULL << 3;
    }
    return passed;
}

namespace {

thread_local char gGuestDlError[160]{};
thread_local bool gGuestDlErrorPending = false;

void setGuestDlError(const char* message) {
    std::snprintf(gGuestDlError, sizeof(gGuestDlError), "%s", message);
    gGuestDlErrorPending = true;
}

const char* fileNamePart(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* separator = slash;
    if (backslash != nullptr && (separator == nullptr || backslash > separator)) {
        separator = backslash;
    }
    return separator != nullptr ? separator + 1 : path;
}

size_t loaderHandleIndex(void* handle) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    if (value < 2 || value - 2 >= gLoaderImageCount) {
        return gLoaderImageCount;
    }
    return static_cast<size_t>(value - 2);
}

} // namespace

void* guestDlopen(const char* fileName, int flags) {
    (void)flags;
    gGuestDlErrorPending = false;
    if (fileName == nullptr) {
        return reinterpret_cast<void*>(1);
    }
    // Silent/error audio stubs are available even before an image session so
    // FMOD never blocks on a missing Android audio DSO.
    if (isVirtualAudioLibrary(fileName)) {
        return virtualAudioLibraryHandle(fileName);
    }
    if (gLoaderImageCount == 0) {
        setGuestDlError("loader session is not active");
        return nullptr;
    }
    const char* requested = fileNamePart(fileName);
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        if (gLoaderImages[index].soname == requested ||
            fileNamePart(gLoaderImages[index].name.c_str()) == std::string(requested)) {
            return reinterpret_cast<void*>(index + 2);
        }
    }
    setGuestDlError("requested DSO is not loaded");
    return nullptr;
}

void* guestDlsym(void* handle, const char* symbolName) {
    gGuestDlErrorPending = false;
    if (symbolName == nullptr || symbolName[0] == '\0') {
        setGuestDlError("invalid symbol name");
        return nullptr;
    }
    if (handle != nullptr) {
        void* audioSymbol = resolveVirtualAudioSymbol(handle, symbolName);
        if (audioSymbol != nullptr) {
            return audioSymbol;
        }
        // A known virtual audio handle that does not export this name is a
        // clear miss — do not fall through into the image table.
        if (reinterpret_cast<uintptr_t>(handle) == 0x051E5 ||
            reinterpret_cast<uintptr_t>(handle) == 0xAA1D10) {
            setGuestDlError("symbol is not available in audio stub scope");
            return nullptr;
        }
    }
    uintptr_t address = 0;
    uint64_t symbolValue = 0;
    if (handle == nullptr || handle == reinterpret_cast<void*>(1) ||
        handle == reinterpret_cast<void*>(UINTPTR_MAX)) {
        for (size_t index = 0; index < gLoaderImageCount; ++index) {
            if (findExport(gLoaderImages[index], symbolName, &address, &symbolValue)) {
                return reinterpret_cast<void*>(address);
            }
        }
    }
    else {
        const size_t index = loaderHandleIndex(handle);
        if (index == gLoaderImageCount) {
            setGuestDlError("invalid DSO handle");
            return nullptr;
        }
        if (findExport(gLoaderImages[index], symbolName, &address, &symbolValue)) {
            return reinterpret_cast<void*>(address);
        }
    }
    address = resolveHostSymbol(symbolName);
    if (address != 0) {
        return reinterpret_cast<void*>(address);
    }
    setGuestDlError("symbol is not available in loader scope");
    return nullptr;
}

int guestDlclose(void* handle) {
    gGuestDlErrorPending = false;
    if (handle == reinterpret_cast<void*>(1) || loaderHandleIndex(handle) != gLoaderImageCount) {
        return 0;
    }
    setGuestDlError("invalid DSO handle");
    return -1;
}

const char* guestDlerror() {
    if (!gGuestDlErrorPending) {
        return nullptr;
    }
    gGuestDlErrorPending = false;
    return gGuestDlError;
}

int guestDladdr(const void* address, GuestDlInfo* info) {
    if (address == nullptr || info == nullptr) {
        return 0;
    }
    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        const CrossImage& image = gLoaderImages[imageIndex];
        const uintptr_t start = image.executableStart();
        const uintptr_t end = start + image.image.size();
        if (target < start || target >= end) {
            continue;
        }
        *info = {};
        info->fileName = image.name.c_str();
        info->fileBase = reinterpret_cast<void*>(start);
        uint64_t bestValue = 0;
        const char* bestName = nullptr;
        for (uint64_t symbolIndex = 1; symbolIndex < image.symbolCount; ++symbolIndex) {
            Elf64Symbol symbol{};
            const char* name = nullptr;
            if (!readSymbol(image, symbolIndex, &symbol, &name) ||
                symbol.sectionIndex == kShnUndefined || name[0] == '\0') {
                continue;
            }
            const uintptr_t symbolAddress = image.loadBias() + symbol.value;
            if (symbolAddress <= target && symbol.value >= bestValue) {
                bestValue = symbol.value;
                bestName = name;
            }
        }
        if (bestName != nullptr) {
            info->symbolName = bestName;
            info->symbolAddress = reinterpret_cast<void*>(image.loadBias() + bestValue);
        }
        return 1;
    }
    return 0;
}

uint64_t guestDynamicLinkSelfTest() {
    uint64_t passed = 0;
    void* handle = guestDlopen("libc++_shared.so", 0);
    if (handle != nullptr) {
        passed |= 1ULL << 0;
    }
    void* cxaThrow = guestDlsym(handle, "__cxa_throw");
    if (cxaThrow != nullptr) {
        passed |= 1ULL << 1;
    }
    if (guestDlsym(nullptr, "malloc") != nullptr) {
        passed |= 1ULL << 2;
    }
    GuestDlInfo info{};
    if (guestDladdr(cxaThrow, &info) == 1 && info.fileName != nullptr &&
        std::strstr(info.fileName, "libc++_shared.so") != nullptr) {
        passed |= 1ULL << 3;
    }
    if (guestDlopen("rr3-intentionally-missing.so", 0) == nullptr &&
        guestDlerror() != nullptr && guestDlerror() == nullptr) {
        passed |= 1ULL << 4;
    }
    if (guestDlclose(handle) == 0) {
        passed |= 1ULL << 5;
    }
    return passed;
}

int guestDlIteratePhdr(GuestDlPhdrCallback callback, void* data) {
    if (callback == nullptr) {
        return 0;
    }
    CrossImage* pairImages[] = {&gPrimaryImage, &gDependencyImage};
    const size_t imageCount = gLoaderImageCount != 0 ? gLoaderImageCount : 2;
    for (size_t index = 0; index < imageCount; ++index) {
        CrossImage* image = gLoaderImageCount != 0
            ? &gLoaderImages[index]
            : pairImages[index];
        if (!image->loaded) {
            continue;
        }
        GuestDlPhdrInfo info{};
        info.address = image->loadBias();
        info.name = image->name.c_str();
        info.programHeaders = image->programHeaders.data();
        info.programHeaderCount = static_cast<uint16_t>(image->programHeaders.size());
        info.loadEvents = imageCount;
        const int result = callback(&info, sizeof(info), data);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

uint64_t guestPhdrRegistrySelfTest() {
    uint64_t passed = 0;
    const size_t expectedImages = gLoaderImageCount != 0 ? gLoaderImageCount : 2;
    if ((gLoaderImageCount != 0 && gLoaderImages[0].loaded) ||
        (gLoaderImageCount == 0 && gPrimaryImage.loaded && gDependencyImage.loaded)) {
        passed |= 1ULL << 0;
    }
    uint64_t state[2]{};
    if (guestDlIteratePhdr(countPhdrCallback, state) == 0 &&
        state[0] == expectedImages && state[1] == 2) {
        passed |= 1ULL << 1;
    }
    if (guestDlIteratePhdr(stopPhdrCallback, nullptr) == 77) {
        passed |= 1ULL << 2;
    }
    return passed;
}

} // namespace rr3
