// Adapter implementations for EapRules ported code
#include "so_util_adapter.h"
#include "trace_adapter.h"
#include "platform_adapter.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Include our symbol resolver
#include "../../include/rr3/symbol_resolver.hpp"
#include "../../include/rr3/recovery_guard.hpp"

// Global game module instance
static so_module g_realracing3_module = {};

extern "C" {

// HOST symbol table only. `mod` is accepted for the EapRules so_util interface
// and then ignored: the host table is the only thing this can search, and it
// never contains a guest image's symbols. Anything belonging to
// libRealRacing3.so must go through rr3::resolveMainImageExport() instead -
// see jni_compat/classes/cloudcell_natives.h for a lookup that used to come
// through here and therefore always returned 0.
uintptr_t so_symbol(so_module *mod, const char *name) {
    if (!mod || !name) return 0;
    // Use our symbol resolver
    return rr3::resolveHostSymbol(name);
}

void hook_address(so_module *mod, uintptr_t addr, uintptr_t dst) {
    // ARM64 hook implementation - we'll implement this with trampolines
    // For now, just log that we would hook
    eap_trace("hook_address: %p -> %p", (void*)addr, (void*)dst);
    // TODO: Implement ARM64 trampoline hooking
}

// Retained for compatibility with the ported EapRules code that still calls it
// (rr3_asset_patch.cpp, jni_compat/class_registry.cpp, jni_compat/classes/
// ea_EAIO.cpp). The returned handle is a fixed placeholder - it carries no
// guest load address - and so_symbol() on it resolves HOST symbols only. Do
// not reintroduce a guest lookup through this pair; use
// rr3::resolveMainImageExport() for the main image.
so_module *realracing3_module(void) {
    return &g_realracing3_module;
}

void eap_trace(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Write to our guest-view.log
    rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", buf, 0, 0);
}

void eap_warning(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Prefix with warning
    char warn_buf[560];
    snprintf(warn_buf, sizeof(warn_buf), "WARN: %s", buf);
    rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", warn_buf, 0, 0);
}

void eap_fatal_error(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Log and abort
    char fatal_buf[560];
    snprintf(fatal_buf, sizeof(fatal_buf), "FATAL: %s", buf);
    rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", fatal_buf, 0, 0);
    abort();
}

extern "C" const char *io_game_dir(void) {
    // Return our data directory
    return "sdmc:/switch/rr3-native-rehost";
}

extern "C" const char *io_writable_dir(void) {
    // Return a writable directory for save files
    return "sdmc:/switch/rr3-native-rehost/save";
}

extern "C" void io_set_game_dir(const char *dir) {
    // We use a fixed path, so this is a no-op
    (void)dir;
}

extern "C" const char *fix_path(const char *path, char *buf, size_t buf_len) {
    if (!path || !buf || buf_len == 0) return path;

    // Replace Android paths with our paths
    const char *android_prefix = "/storage/emulated/0/Android/data/com.ea.games.r3_row/files/";
    const char *our_prefix = "sdmc:/switch/rr3-native-rehost/";

    if (strncmp(path, android_prefix, strlen(android_prefix)) == 0) {
        snprintf(buf, buf_len, "%s%s", our_prefix, path + strlen(android_prefix));
        return buf;
    }

    // No change needed - copy to buf anyway for consistency
    strncpy(buf, path, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return buf;
}

} // extern "C"

// Stub crash reporting
extern "C" void crash_report_init(void *mod, const char *soname) {
    (void)mod; (void)soname;
}

extern "C" void crash_report_backtrace(const char *tag) {
    (void)tag;
}
