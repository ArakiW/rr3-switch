#pragma once

#include <cstddef>
#include <cstdint>

namespace rr3 {

struct HostSymbol {
    const char* name;
    uintptr_t address;
};

uintptr_t resolveHostSymbol(const char* name);
size_t hostSymbolCount();
uint64_t hostSymbolNameHash();

} // namespace rr3

