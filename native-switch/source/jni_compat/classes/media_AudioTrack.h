// Stub AudioTrack for Switch - audio handled by FMOD pump
#pragma once

#include "../jni.h"
#include "../jni_internals.h"

#define ENCODING_PCM_16BIT  2
#define ENCODING_PCM_8BIT   3
#define ENCODING_PCM_FLOAT  4
#define WRITE_BLOCKING      0
#define WRITE_NON_BLOCKING  1
#define MODE_STATIC 1
#define MODE_STREAM 0

class AudioTrack : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    int sampleRate;
    int channelConfig;
    int audioFormat;
    int bufferSize;
    int mode;
    bool playing;

    AudioTrack() : sampleRate(0), channelConfig(0), audioFormat(0),
                   bufferSize(0), mode(0), playing(false) {}
};
