// Stub AudioTrack implementation for Switch - audio handled by FMOD pump
#include <stdio.h>
#include <string.h>

#include "../../eap_port/platform_adapter.h"
#include "../../eap_port/trace_adapter.h"
#include "../jni.h"
#include "../jni_internals.h"
#include "media_AudioTrack.h"

// Stub implementations - FMOD pump handles actual audio
static void audio_ctor(JNIEnv *, jobject self, jclass) {
    new (self) AudioTrack();
}

static void audio_constructor(JNIEnv *, jobject self, jint streamType, jint sampleRate,
                              jint channelConfig, jint audioFormat, jint bufferSize, jint mode) {
    AudioTrack *track = (AudioTrack *)self;
    track->sampleRate = sampleRate;
    track->channelConfig = channelConfig;
    track->audioFormat = audioFormat;
    track->bufferSize = bufferSize;
    track->mode = mode;
    trace("AudioTrack: stub created (FMOD handles audio)");
}

static void audio_play(JNIEnv *, jobject self) {
    AudioTrack *track = (AudioTrack *)self;
    track->playing = true;
}

static void audio_pause(JNIEnv *, jobject self) {
    AudioTrack *track = (AudioTrack *)self;
    track->playing = false;
}

static void audio_stop(JNIEnv *, jobject self) {
    AudioTrack *track = (AudioTrack *)self;
    track->playing = false;
}

static void audio_release(JNIEnv *, jobject self) {
    AudioTrack *track = (AudioTrack *)self;
    track->playing = false;
}

static jint audio_write(JNIEnv *, jobject, jshortArray, jint, jint, jint) {
    // Stub - return success
    return 0;
}

static jint audio_getMinBufferSize(JNIEnv *, jclass, jint, jint, jint) {
    return 4096;
}

static jint audio_getMaxVolume(JNIEnv *, jclass, jint) {
    return 100;
}

static jint audio_getStreamVolume(JNIEnv *, jclass, jint) {
    return 100;
}

static const ManagedMethod audio_methods[] = {
    ManagedMethod::RegisterNonVirtual<&audio_ctor>(AudioTrack::clazz, "<init>", "()V"),
    ManagedMethod::Register<&audio_constructor>(AudioTrack::clazz, "<init>", "(IIIIII)V"),
    ManagedMethod::Register<&audio_play>(AudioTrack::clazz, "play", "()V"),
    ManagedMethod::Register<&audio_pause>(AudioTrack::clazz, "pause", "()V"),
    ManagedMethod::Register<&audio_stop>(AudioTrack::clazz, "stop", "()V"),
    ManagedMethod::Register<&audio_release>(AudioTrack::clazz, "release", "()V"),
    ManagedMethod::Register<&audio_write>(AudioTrack::clazz, "write", "([SIII)I"),
    ManagedMethod::RegisterStatic<&audio_getMinBufferSize>(AudioTrack::clazz, "getMinBufferSize", "(III)I"),
    ManagedMethod::RegisterStatic<&audio_getMaxVolume>(AudioTrack::clazz, "getMaxVolume", "(I)I"),
    ManagedMethod::RegisterStatic<&audio_getStreamVolume>(AudioTrack::clazz, "getStreamVolume", "(I)I"),
    {NULL},
};

Class AudioTrack::clazz = {
    .classpath = "android/media/AudioTrack",
    .classname = "AudioTrack",
    .managed_methods = audio_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(AudioTrack),
};

static const int audio_registered = ClassRegistry::register_class(AudioTrack::clazz);
