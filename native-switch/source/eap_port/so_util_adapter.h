// Adapter: maps EapRules so_util.h interface to our symbol_resolver
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal so_module structure matching EapRules interface
typedef struct so_module {
    uintptr_t base;
    const char *soname;
    // We don't need most fields - just enough for the ported code to compile
} so_module;

// Symbol resolution - maps to our symbol_resolver
uintptr_t so_symbol(so_module *mod, const char *name);

// Hook support - we'll implement this with our ARM64 trampoline
void hook_address(so_module *mod, uintptr_t addr, uintptr_t dst);

// Get the main game module
so_module *realracing3_module(void);

#ifdef __cplusplus
}
#endif

// Convenience macro from EapRules
#define ABI_ATTR
