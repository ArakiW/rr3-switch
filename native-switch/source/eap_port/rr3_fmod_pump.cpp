// FMOD pump stub for Switch - audio handled by Switch audio API
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

#include "platform_adapter.h"
#include "trace_adapter.h"
#include "../jni_compat/jni.h"
#include "so_util_adapter.h"
#include "rr3_fmod_pump.h"

// FMOD info codes
enum {
    FMOD_INFO_SAMPLE_RATE = 0,
    FMOD_INFO_BUFFER_LENGTH = 1,
    FMOD_INFO_NUM_BUFFERS = 2,
    FMOD_INFO_CONSTANT = 3,
    FMOD_INFO_CHANNELS = 4,
};

static std::atomic<bool> g_fmod_running{false};

extern "C" jint fmodGetInfo(JNIEnv *, jobject, jint what) {
    switch (what) {
        case FMOD_INFO_SAMPLE_RATE: return 48000;
        case FMOD_INFO_BUFFER_LENGTH: return 1024;
        case FMOD_INFO_NUM_BUFFERS: return 4;
        case FMOD_INFO_CONSTANT: return 1;
        case FMOD_INFO_CHANNELS: return 2;
        default: return -1;
    }
}

extern "C" jint fmodProcess(JNIEnv *env, jobject, jobject buffer) {
    // Stub - return silence
    return 0;
}

void rr3_fmod_pump_init(void) {
    g_fmod_running = true;
    trace("FMOD pump: stub initialized (Switch audio)");
}

void rr3_fmod_pump_shutdown(void) {
    g_fmod_running = false;
}

void rr3_fmod_pump_frame(void) {
    // Stub - no-op
}