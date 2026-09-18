#include "rr3/elf_preflight.hpp"
#include "rr3/symbol_resolver.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <vector>

namespace rr3 {
namespace {

constexpr unsigned char kElfMagic[4] = {0x7f, 'E', 'L', 'F'};
constexpr uint8_t kElfClass64 = 2;
constexpr uint8_t kElfDataLittleEndian = 1;
constexpr uint16_t kEtDyn = 3;
constexpr uint16_t kEmAarch64 = 183;

constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtDynamic = 2;
constexpr uint32_t kPtTls = 7;
constexpr uint32_t kPfExecute = 1;

constexpr int64_t kDtNull = 0;
constexpr int64_t kDtNeeded = 1;
constexpr int64_t kDtPltRelSize = 2;
constexpr int64_t kDtStrTab = 5;
constexpr int64_t kDtSymTab = 6;
constexpr int64_t kDtRelaSize = 8;
constexpr int64_t kDtRelaEnt = 9;
constexpr int64_t kDtStrSize = 10;
constexpr int64_t kDtSymEnt = 11;
constexpr int64_t kDtJmpRel = 23;
constexpr int64_t kDtRela = 7;
constexpr int64_t kDtPltRel = 20;
constexpr int64_t kDtVerSym = 0x6ffffff0;

constexpr uint32_t kRAarch64Abs64 = 257;
constexpr uint32_t kRAarch64GlobDat = 1025;
constexpr uint32_t kRAarch64JumpSlot = 1026;
constexpr uint32_t kRAarch64Relative = 1027;

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

struct Elf64ProgramHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtualAddress;
    uint64_t physicalAddress;
    uint64_t fileSize;
    uint64_t memorySize;
    uint64_t alignment;
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

template <typename T>
bool readObject(std::FILE* file, uint64_t offset, T* value) {
    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(value, sizeof(T), 1, file) == 1;
}

void setError(ElfPreflight* result, const char* message) {
    std::snprintf(result->error, sizeof(result->error), "%s", message);
}

void setError(ElfDryLoad* result, const char* message) {
    std::snprintf(result->error, sizeof(result->error), "%s", message);
}

uint64_t alignDown(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

bool alignUp(uint64_t value, uint64_t alignment, uint64_t* output) {
    if (value > UINT64_MAX - (alignment - 1)) {
        return false;
    }
    *output = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

uint64_t fnv1a64(const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    uint64_t hash = 1469598103934665603ULL;
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t fnv1a64Append(uint64_t hash, const char* text) {
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
         *cursor != 0;
         ++cursor) {
        hash ^= *cursor;
        hash *= 1099511628211ULL;
    }
    hash ^= 0;
    hash *= 1099511628211ULL;
    return hash;
}

bool startsWith(const char* value, const char* prefix) {
    return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

} // namespace

ElfPreflight inspectElfFile(const char* path) {
    ElfPreflight result{};
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        setError(&result, "file not found");
        return result;
    }
    result.opened = true;

    Elf64Header header{};
    if (!readObject(file, 0, &header)) {
        setError(&result, "could not read ELF header");
        std::fclose(file);
        return result;
    }

    result.validElf = std::memcmp(header.ident, kElfMagic, sizeof(kElfMagic)) == 0 &&
        header.ident[4] == kElfClass64 && header.ident[5] == kElfDataLittleEndian;
    result.machine = header.machine;
    result.type = header.type;
    result.isAarch64 = header.machine == kEmAarch64;
    result.isDynamicObject = header.type == kEtDyn;
    result.programHeaderCount = header.programHeaderCount;
    if (!result.validElf) {
        setError(&result, "not a 64-bit little-endian ELF");
        std::fclose(file);
        return result;
    }
    if (header.programHeaderEntrySize != sizeof(Elf64ProgramHeader)) {
        setError(&result, "unexpected program-header size");
        std::fclose(file);
        return result;
    }

    if (std::fseek(file, 0, SEEK_END) != 0) {
        setError(&result, "could not size ELF file");
        std::fclose(file);
        return result;
    }
    const long fileLength = std::ftell(file);
    const uint64_t programHeaderBytes =
        static_cast<uint64_t>(header.programHeaderCount) * sizeof(Elf64ProgramHeader);
    if (fileLength < 0 || header.programHeaderOffset > static_cast<uint64_t>(fileLength) ||
        programHeaderBytes > static_cast<uint64_t>(fileLength) - header.programHeaderOffset) {
        setError(&result, "program-header table is outside the file");
        std::fclose(file);
        return result;
    }

    std::vector<Elf64ProgramHeader> programHeaders(header.programHeaderCount);
    if (std::fseek(file, static_cast<long>(header.programHeaderOffset), SEEK_SET) != 0 ||
        std::fread(programHeaders.data(), sizeof(Elf64ProgramHeader), programHeaders.size(), file) !=
            programHeaders.size()) {
        setError(&result, "could not read program headers");
        std::fclose(file);
        return result;
    }

    const Elf64ProgramHeader* dynamicHeader = nullptr;
    uint64_t minimumAddress = UINT64_MAX;
    uint64_t maximumAddress = 0;
    for (const Elf64ProgramHeader& ph : programHeaders) {
        if (ph.type == kPtLoad) {
            ++result.loadSegments;
            if ((ph.flags & kPfExecute) != 0) {
                ++result.executableSegments;
            }
            minimumAddress = std::min(minimumAddress, ph.virtualAddress);
            maximumAddress = std::max(maximumAddress, ph.virtualAddress + ph.memorySize);
        }
        else if (ph.type == kPtDynamic) {
            ++result.dynamicSegments;
            dynamicHeader = &ph;
        }
        else if (ph.type == kPtTls) {
            ++result.tlsSegments;
        }
    }
    if (minimumAddress != UINT64_MAX && maximumAddress >= minimumAddress) {
        result.mappedBytes = maximumAddress - minimumAddress;
    }

    if (dynamicHeader != nullptr) {
        const uint64_t count = dynamicHeader->fileSize / sizeof(Elf64Dynamic);
        uint64_t relaAddress = 0;
        uint64_t relaSize = 0;
        uint64_t relaEntrySize = 0;
        uint64_t jumpRelAddress = 0;
        for (uint64_t index = 0; index < count; ++index) {
            Elf64Dynamic entry{};
            if (!readObject(file, dynamicHeader->offset + index * sizeof(entry), &entry)) {
                setError(&result, "could not read dynamic table");
                break;
            }
            if (entry.tag == kDtNull) {
                break;
            }
            if (entry.tag == kDtNeeded) {
                ++result.neededCount;
            }
            else if (entry.tag == kDtRela) {
                relaAddress = entry.value;
            }
            else if (entry.tag == kDtPltRelSize) {
                result.pltRelocationBytes = entry.value;
            }
            else if (entry.tag == kDtRelaSize) {
                relaSize = entry.value;
            }
            else if (entry.tag == kDtRelaEnt) {
                relaEntrySize = entry.value;
            }
            else if (entry.tag == kDtJmpRel) {
                jumpRelAddress = entry.value;
            }
        }
        if (relaEntrySize != 0) {
            result.relaCount = relaSize / relaEntrySize;
        }

        const auto virtualAddressToFileOffset = [&programHeaders](uint64_t address, uint64_t size,
                                                                  uint64_t* fileOffset) {
            for (const Elf64ProgramHeader& ph : programHeaders) {
                if (ph.type != kPtLoad || address < ph.virtualAddress) {
                    continue;
                }
                const uint64_t relative = address - ph.virtualAddress;
                if (relative <= ph.fileSize && size <= ph.fileSize - relative) {
                    *fileOffset = ph.offset + relative;
                    return true;
                }
            }
            return false;
        };

        const auto countRelocations = [&result, file](uint64_t fileOffset, uint64_t bytes) {
            const uint64_t count = bytes / sizeof(Elf64Rela);
            for (uint64_t index = 0; index < count; ++index) {
                Elf64Rela relocation{};
                if (!readObject(file, fileOffset + index * sizeof(relocation), &relocation)) {
                    ++result.unsupportedRelocations;
                    break;
                }
                const uint32_t type = static_cast<uint32_t>(relocation.info);
                switch (type) {
                    case kRAarch64Relative:
                        ++result.relativeRelocations;
                        break;
                    case kRAarch64Abs64:
                        ++result.absolute64Relocations;
                        break;
                    case kRAarch64GlobDat:
                        ++result.globalDataRelocations;
                        break;
                    case kRAarch64JumpSlot:
                        ++result.jumpSlotRelocations;
                        break;
                    default:
                        ++result.unsupportedRelocations;
                        break;
                }
            }
        };

        uint64_t relocationFileOffset = 0;
        if (relaAddress != 0 && relaSize != 0 &&
            virtualAddressToFileOffset(relaAddress, relaSize, &relocationFileOffset)) {
            countRelocations(relocationFileOffset, relaSize);
        }
        else if (relaSize != 0) {
            result.unsupportedRelocations += result.relaCount;
        }

        uint64_t jumpRelFileOffset = 0;
        if (jumpRelAddress != 0 && result.pltRelocationBytes != 0 &&
            virtualAddressToFileOffset(jumpRelAddress, result.pltRelocationBytes,
                                       &jumpRelFileOffset)) {
            countRelocations(jumpRelFileOffset, result.pltRelocationBytes);
        }
        else if (result.pltRelocationBytes != 0) {
            result.unsupportedRelocations +=
                result.pltRelocationBytes / sizeof(Elf64Rela);
        }
    }

    std::fclose(file);
    return result;
}

ElfDryLoad dryLoadElfFile(const char* path) {
    ElfDryLoad result{};
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        setError(&result, "file not found");
        return result;
    }
    result.opened = true;

    Elf64Header header{};
    if (!readObject(file, 0, &header) ||
        std::memcmp(header.ident, kElfMagic, sizeof(kElfMagic)) != 0 ||
        header.ident[4] != kElfClass64 || header.ident[5] != kElfDataLittleEndian ||
        header.machine != kEmAarch64 || header.type != kEtDyn ||
        header.programHeaderEntrySize != sizeof(Elf64ProgramHeader)) {
        setError(&result, "unsupported ELF header");
        std::fclose(file);
        return result;
    }
    result.validElf = true;

    if (std::fseek(file, 0, SEEK_END) != 0) {
        setError(&result, "could not size ELF file");
        std::fclose(file);
        return result;
    }
    const long fileLength = std::ftell(file);
    const uint64_t programHeaderBytes =
        static_cast<uint64_t>(header.programHeaderCount) * sizeof(Elf64ProgramHeader);
    if (fileLength < 0 || header.programHeaderOffset > static_cast<uint64_t>(fileLength) ||
        programHeaderBytes > static_cast<uint64_t>(fileLength) - header.programHeaderOffset) {
        setError(&result, "program-header table is outside the file");
        std::fclose(file);
        return result;
    }

    std::vector<Elf64ProgramHeader> programHeaders(header.programHeaderCount);
    if (std::fseek(file, static_cast<long>(header.programHeaderOffset), SEEK_SET) != 0 ||
        std::fread(programHeaders.data(), sizeof(Elf64ProgramHeader), programHeaders.size(), file) !=
            programHeaders.size()) {
        setError(&result, "could not read program headers");
        std::fclose(file);
        return result;
    }

    constexpr uint64_t kPageSize = 0x1000;
    constexpr uint64_t kMaximumImageSize = 128ULL * 1024ULL * 1024ULL;
    uint64_t minimumAddress = UINT64_MAX;
    uint64_t maximumAddress = 0;
    const Elf64ProgramHeader* dynamicHeader = nullptr;
    for (const Elf64ProgramHeader& ph : programHeaders) {
        if (ph.type == kPtLoad) {
            const bool validAlignment = ph.alignment == 0 || ph.alignment == 1 ||
                ((ph.alignment & (ph.alignment - 1)) == 0 &&
                 (ph.virtualAddress % ph.alignment) == (ph.offset % ph.alignment));
            if (!validAlignment || ph.fileSize > ph.memorySize ||
                ph.virtualAddress > UINT64_MAX - ph.memorySize ||
                ph.offset > static_cast<uint64_t>(fileLength) ||
                ph.fileSize > static_cast<uint64_t>(fileLength) - ph.offset) {
                setError(&result, "invalid PT_LOAD segment");
                std::fclose(file);
                return result;
            }
            minimumAddress = std::min(minimumAddress, alignDown(ph.virtualAddress, kPageSize));
            maximumAddress = std::max(maximumAddress, ph.virtualAddress + ph.memorySize);
        }
        else if (ph.type == kPtDynamic) {
            if (dynamicHeader != nullptr) {
                setError(&result, "multiple PT_DYNAMIC segments");
                std::fclose(file);
                return result;
            }
            dynamicHeader = &ph;
        }
    }
    if (minimumAddress == UINT64_MAX || !alignUp(maximumAddress, kPageSize, &maximumAddress) ||
        maximumAddress <= minimumAddress || maximumAddress - minimumAddress > kMaximumImageSize) {
        setError(&result, "invalid mapped image range");
        std::fclose(file);
        return result;
    }

    const auto containsLoadMemoryRange = [&programHeaders](uint64_t address, uint64_t size) {
        for (const Elf64ProgramHeader& ph : programHeaders) {
            if (ph.type != kPtLoad || address < ph.virtualAddress) {
                continue;
            }
            const uint64_t relative = address - ph.virtualAddress;
            if (relative <= ph.memorySize && size <= ph.memorySize - relative) {
                return true;
            }
        }
        return false;
    };
    const auto containsLoadFileRange = [&programHeaders](uint64_t address, uint64_t size) {
        for (const Elf64ProgramHeader& ph : programHeaders) {
            if (ph.type != kPtLoad || address < ph.virtualAddress) {
                continue;
            }
            const uint64_t relative = address - ph.virtualAddress;
            if (relative <= ph.fileSize && size <= ph.fileSize - relative) {
                return true;
            }
        }
        return false;
    };

    result.imageBytes = maximumAddress - minimumAddress;
    void* allocation = memalign(kPageSize, static_cast<size_t>(result.imageBytes));
    if (allocation == nullptr) {
        setError(&result, "image allocation failed");
        std::fclose(file);
        return result;
    }
    result.allocated = true;
    std::memset(allocation, 0, static_cast<size_t>(result.imageBytes));
    auto* image = static_cast<unsigned char*>(allocation);

    for (const Elf64ProgramHeader& ph : programHeaders) {
        if (ph.type != kPtLoad) {
            continue;
        }
        const uint64_t relative = ph.virtualAddress - minimumAddress;
        if (relative > result.imageBytes || ph.memorySize > result.imageBytes - relative ||
            std::fseek(file, static_cast<long>(ph.offset), SEEK_SET) != 0 ||
            std::fread(image + relative, 1, static_cast<size_t>(ph.fileSize), file) != ph.fileSize) {
            setError(&result, "PT_LOAD copy failed");
            std::free(allocation);
            std::fclose(file);
            return result;
        }
        result.copiedBytes += ph.fileSize;
        result.zeroFilledBytes += ph.memorySize - ph.fileSize;
    }
    result.imageHash = fnv1a64(image, static_cast<size_t>(result.imageBytes));

    if (dynamicHeader == nullptr || dynamicHeader->fileSize == 0 ||
        dynamicHeader->fileSize > dynamicHeader->memorySize ||
        dynamicHeader->fileSize % sizeof(Elf64Dynamic) != 0 ||
        !containsLoadFileRange(dynamicHeader->virtualAddress, dynamicHeader->fileSize)) {
        setError(&result, "dynamic table is not file-backed PT_LOAD data");
        std::free(allocation);
        std::fclose(file);
        return result;
    }

    uint64_t relaAddress = 0;
    uint64_t relaSize = 0;
    uint64_t relaEntrySize = 0;
    uint64_t jumpRelAddress = 0;
    uint64_t jumpRelSize = 0;
    uint64_t pltRelType = 0;
    uint64_t symbolTableAddress = 0;
    uint64_t symbolEntrySize = 0;
    uint64_t stringTableAddress = 0;
    uint64_t stringTableSize = 0;
    uint64_t versionTableAddress = 0;
    bool sawRelaAddress = false;
    bool sawRelaSize = false;
    bool sawRelaEntrySize = false;
    bool sawJumpRelAddress = false;
    bool sawJumpRelSize = false;
    bool sawPltRelType = false;
    bool sawSymbolTable = false;
    bool sawSymbolEntrySize = false;
    bool sawStringTable = false;
    bool sawStringTableSize = false;
    bool sawVersionTable = false;
    bool sawNull = false;
    const auto* dynamic = reinterpret_cast<const Elf64Dynamic*>(
        image + dynamicHeader->virtualAddress - minimumAddress);
    const uint64_t dynamicCount = dynamicHeader->fileSize / sizeof(Elf64Dynamic);
    const auto assignUnique = [&](bool* seen, uint64_t* target, uint64_t value) {
        if (*seen && *target != value) {
            setError(&result, "conflicting dynamic tags");
            return false;
        }
        *seen = true;
        *target = value;
        return true;
    };
    for (uint64_t index = 0; index < dynamicCount; ++index) {
        const Elf64Dynamic& entry = dynamic[index];
        if (entry.tag == kDtNull) {
            sawNull = true;
            break;
        }
        if (entry.tag == kDtRela) {
            if (!assignUnique(&sawRelaAddress, &relaAddress, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtRelaSize) {
            if (!assignUnique(&sawRelaSize, &relaSize, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtRelaEnt) {
            if (!assignUnique(&sawRelaEntrySize, &relaEntrySize, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtJmpRel) {
            if (!assignUnique(&sawJumpRelAddress, &jumpRelAddress, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtPltRelSize) {
            if (!assignUnique(&sawJumpRelSize, &jumpRelSize, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtPltRel) {
            if (!assignUnique(&sawPltRelType, &pltRelType, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtSymTab) {
            if (!assignUnique(&sawSymbolTable, &symbolTableAddress, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtSymEnt) {
            if (!assignUnique(&sawSymbolEntrySize, &symbolEntrySize, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtStrTab) {
            if (!assignUnique(&sawStringTable, &stringTableAddress, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtStrSize) {
            if (!assignUnique(&sawStringTableSize, &stringTableSize, entry.value)) {
                break;
            }
        }
        else if (entry.tag == kDtVerSym) {
            if (!assignUnique(&sawVersionTable, &versionTableAddress, entry.value)) {
                break;
            }
        }
    }

    if (result.error[0] != '\0' || !sawNull) {
        if (result.error[0] == '\0') {
            setError(&result, "unterminated dynamic table");
        }
        std::free(allocation);
        std::fclose(file);
        return result;
    }

    const bool hasAnyRelaTag = sawRelaAddress || sawRelaSize || sawRelaEntrySize;
    if (hasAnyRelaTag &&
        (!sawRelaAddress || !sawRelaSize || !sawRelaEntrySize ||
         relaEntrySize != sizeof(Elf64Rela))) {
        setError(&result, "incomplete or unsupported RELA table");
    }
    const bool hasAnyPltTag = sawJumpRelAddress || sawJumpRelSize || sawPltRelType;
    if (result.error[0] == '\0' && hasAnyPltTag &&
        (!sawJumpRelAddress || !sawJumpRelSize || !sawPltRelType ||
         pltRelType != static_cast<uint64_t>(kDtRela))) {
        setError(&result, "incomplete or unsupported PLT relocation table");
    }
    if (result.error[0] != '\0') {
        std::free(allocation);
        std::fclose(file);
        return result;
    }

    const auto scanRelocationSymbols = [&](uint64_t address, uint64_t bytes) {
        if (!containsLoadFileRange(address, bytes) || bytes % sizeof(Elf64Rela) != 0) {
            ++result.invalidRelocations;
            return;
        }
        const auto* relocations = reinterpret_cast<const Elf64Rela*>(
            image + address - minimumAddress);
        const uint64_t count = bytes / sizeof(Elf64Rela);
        for (uint64_t index = 0; index < count; ++index) {
            Elf64Rela relocation{};
            std::memcpy(&relocation, relocations + index, sizeof(relocation));
            const uint32_t type = static_cast<uint32_t>(relocation.info);
            const uint64_t symbolIndex = relocation.info >> 32;
            if ((relocation.offset & (alignof(uint64_t) - 1)) != 0 ||
                !containsLoadMemoryRange(relocation.offset, sizeof(uint64_t))) {
                ++result.invalidRelocations;
                continue;
            }
            if (type == kRAarch64Relative) {
                if (symbolIndex != 0) {
                    ++result.invalidRelocations;
                }
                continue;
            }
            if (type != kRAarch64Abs64 && type != kRAarch64GlobDat &&
                type != kRAarch64JumpSlot) {
                ++result.invalidRelocations;
                continue;
            }
            if (symbolIndex == 0) {
                ++result.invalidRelocations;
                continue;
            }
            result.maximumSymbolIndex = std::max(result.maximumSymbolIndex, symbolIndex);
        }
    };

    if (sawRelaAddress && relaSize != 0) {
        scanRelocationSymbols(relaAddress, relaSize);
    }
    if (sawJumpRelAddress && jumpRelSize != 0) {
        scanRelocationSymbols(jumpRelAddress, jumpRelSize);
    }
    if (result.invalidRelocations != 0) {
        setError(&result, "invalid relocation table, type, or target");
        std::free(allocation);
        std::fclose(file);
        return result;
    }

    if (!sawSymbolTable || !sawSymbolEntrySize || !sawStringTable ||
        !sawStringTableSize || symbolEntrySize != sizeof(Elf64Symbol) ||
        stringTableSize == 0 || result.maximumSymbolIndex >
            (UINT64_MAX / sizeof(Elf64Symbol)) - 1) {
        setError(&result, "incomplete or unsupported dynamic symbol table");
        std::free(allocation);
        std::fclose(file);
        return result;
    }
    const uint64_t requiredSymbolBytes =
        (result.maximumSymbolIndex + 1) * sizeof(Elf64Symbol);
    if (!containsLoadFileRange(symbolTableAddress, requiredSymbolBytes) ||
        !containsLoadFileRange(stringTableAddress, stringTableSize) ||
        (sawVersionTable && !containsLoadFileRange(
            versionTableAddress, (result.maximumSymbolIndex + 1) * sizeof(uint16_t)))) {
        setError(&result, "symbol, string, or version table is outside file-backed LOAD data");
        std::free(allocation);
        std::fclose(file);
        return result;
    }

    std::vector<uint8_t> seenExternalSymbols(result.maximumSymbolIndex + 1, 0);
    result.symbolNameHash = 1469598103934665603ULL;
    result.resolverTableEntries = hostSymbolCount();
    result.resolverTableNameHash = hostSymbolNameHash();
    result.lookupMatchedNameHash = 1469598103934665603ULL;

    const auto readSymbol = [&](uint64_t symbolIndex, Elf64Symbol* symbol,
                                const char** name) {
        if (symbolIndex > result.maximumSymbolIndex) {
            return false;
        }
        const uint64_t symbolOffset = symbolTableAddress - minimumAddress +
            symbolIndex * sizeof(Elf64Symbol);
        std::memcpy(symbol, image + symbolOffset, sizeof(*symbol));
        if (symbol->name >= stringTableSize) {
            return false;
        }
        const char* candidate = reinterpret_cast<const char*>(
            image + stringTableAddress - minimumAddress + symbol->name);
        const size_t remaining = static_cast<size_t>(stringTableSize - symbol->name);
        if (std::memchr(candidate, '\0', remaining) == nullptr) {
            return false;
        }
        *name = candidate;
        return true;
    };
    const auto classifyExternalSymbol = [&](const char* name) {
        if (startsWith(name, "FMOD_") || startsWith(name, "_ZN4FMOD")) {
            ++result.fmodSymbols;
        }
        else if (startsWith(name, "egl")) {
            ++result.eglSymbols;
        }
        else if (startsWith(name, "gl") && name[2] >= 'A' && name[2] <= 'Z') {
            ++result.glesSymbols;
        }
        else if (startsWith(name, "Ar") && name[2] >= 'A' && name[2] <= 'Z') {
            ++result.arcoreSymbols;
        }
        else if (startsWith(name, "pthread")) {
            ++result.pthreadSymbols;
        }
        else if (startsWith(name, "__android_")) {
            ++result.androidSymbols;
        }
        else if (startsWith(name, "_Z") || startsWith(name, "__cxa") ||
                 startsWith(name, "__gxx")) {
            ++result.cxxSymbols;
        }
        else {
            ++result.otherSymbols;
        }
    };

    const uintptr_t loadBias = reinterpret_cast<uintptr_t>(image) - minimumAddress;
    const auto processRelocations = [&](uint64_t address, uint64_t bytes) {
        if (!containsLoadFileRange(address, bytes) || bytes % sizeof(Elf64Rela) != 0) {
            ++result.invalidRelocations;
            return;
        }
        const auto* relocations = reinterpret_cast<const Elf64Rela*>(
            image + address - minimumAddress);
        const uint64_t count = bytes / sizeof(Elf64Rela);
        for (uint64_t index = 0; index < count; ++index) {
            const Elf64Rela relocation = relocations[index];
            const uint32_t type = static_cast<uint32_t>(relocation.info);
            const uint64_t symbolIndex = relocation.info >> 32;
            if ((relocation.offset & (alignof(uint64_t) - 1)) != 0 ||
                !containsLoadMemoryRange(relocation.offset, sizeof(uint64_t))) {
                ++result.invalidRelocations;
                continue;
            }
            if (type != kRAarch64Relative) {
                Elf64Symbol symbol{};
                const char* name = nullptr;
                if (!readSymbol(symbolIndex, &symbol, &name) || name[0] == '\0') {
                    ++result.invalidRelocations;
                    continue;
                }
                if (symbol.sectionIndex != 0) {
                    ++result.internalSymbolRelocations;
                    continue;
                }
                ++result.externalPending;
                const uintptr_t hostAddress = resolveHostSymbol(name);
                if (hostAddress != 0) {
                    ++result.lookupMatchedRelocations;
                }
                else {
                    ++result.lookupMissingRelocations;
                }
                if (seenExternalSymbols[symbolIndex] == 0) {
                    seenExternalSymbols[symbolIndex] = 1;
                    ++result.uniqueExternalSymbols;
                    result.symbolNameHash = fnv1a64Append(result.symbolNameHash, name);
                    if (hostAddress != 0) {
                        ++result.lookupMatchedSymbols;
                        result.lookupMatchedNameHash =
                            fnv1a64Append(result.lookupMatchedNameHash, name);
                    }
                    else {
                        ++result.lookupMissingSymbols;
                    }
                    if ((symbol.info >> 4) == 2) {
                        ++result.weakExternalSymbols;
                    }
                    if (sawVersionTable) {
                        uint16_t version = 0;
                        const uint64_t versionOffset = versionTableAddress - minimumAddress +
                            symbolIndex * sizeof(uint16_t);
                        std::memcpy(&version, image + versionOffset, sizeof(version));
                        if ((version & 0x7fff) > 1) {
                            ++result.versionedExternalSymbols;
                        }
                    }
                    classifyExternalSymbol(name);
                }
                continue;
            }
            if (symbolIndex != 0) {
                ++result.invalidRelocations;
                continue;
            }
            const uint64_t relocated =
                static_cast<uint64_t>(loadBias) + static_cast<uint64_t>(relocation.addend);
            std::memcpy(image + relocation.offset - minimumAddress,
                        &relocated,
                        sizeof(relocated));
            ++result.relativeApplied;
        }
    };

    if (sawRelaAddress && relaSize != 0) {
        processRelocations(relaAddress, relaSize);
    }
    if (sawJumpRelAddress && jumpRelSize != 0) {
        processRelocations(jumpRelAddress, jumpRelSize);
    }
    if (result.invalidRelocations != 0) {
        setError(&result, "invalid relocation range or target");
    }
    else {
        result.completed = true;
    }

    std::free(allocation);
    std::fclose(file);
    return result;
}

} // namespace rr3
