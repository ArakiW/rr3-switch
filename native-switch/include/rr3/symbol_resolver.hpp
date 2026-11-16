#pragma once

#include <cstddef>
#include <cstdint>

namespace rr3 {

struct HostSymbol {
    const char* name;
    uintptr_t address;
};

uintptr_t resolveHostSymbol(const char* name);
// TRUE when the guest's FMOD symbols are NOT answered by the port, so the
// guest's own libfmodex.so provides them.  "fmod=real" in
// sdmc:/switch/rr3-census.conf turns it on; the default is the port's
// shim, which is what every released build does.
bool guestFmodUsesRealLibrary();
size_t hostSymbolCount();
uint64_t hostSymbolNameHash();

} // namespace rr3

