// Adapter: maps EapRules trace.h/platform.h to our recovery_guard logging
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Map trace() to our appendGuestBootLine
void eap_trace(const char *fmt, ...);
void eap_warning(const char *fmt, ...);
void eap_fatal_error(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

// EapRules uses these macros from platform.h
#define trace(...) eap_trace(__VA_ARGS__)
#define warning(msg, ...) eap_warning(msg, ##__VA_ARGS__)
#define fatal_error(...) eap_fatal_error(__VA_ARGS__)

// Stub warning macro from platform.h
#define WARN_STUB eap_warning("Warning, stubbed function \"%s\".\n", __FUNCTION__)
