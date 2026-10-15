// Adapter: maps EapRules crash.h to our environment
#pragma once

#include "so_util_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Installs the handler and remembers where the module was mapped.
void crash_report_init(so_module *mod, const char *soname);

// Prints the module return addresses on the current thread's stack.
void crash_report_backtrace(const char *tag);

#ifdef __cplusplus
}
#endif
