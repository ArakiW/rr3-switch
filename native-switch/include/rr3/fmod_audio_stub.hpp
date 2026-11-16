#pragma once

#include <cstdint>

namespace rr3 {

// Honest telemetry for the FMOD/Android silent audio device stub.
// audible is ALWAYS zero: the stub never claims samples reached hardware.
struct FmodAudioStubStats {
    uint32_t engineCreates{};
    uint32_t engineRealizes{};
    uint32_t outputMixCreates{};
    uint32_t playerCreates{};
    uint32_t playStateTransitions{};
    uint32_t bufferEnqueues{};
    uint32_t bufferCallbacks{};
    uint32_t aaudioAttempts{};
    uint32_t aaudioRefusals{};
    uint32_t openFailures{};
    // Requested SL_PLAYSTATE_PLAYING / AAudio START; never means "heard sound".
    uint32_t playRequested{};
    // Always 0. There is no Horizon Android audio device behind this stub.
    uint32_t audible{};
    // The device's own format: what it would produce if it produced
    // anything.  It matches what the guest's FMOD asks the port for.
    uint32_t deviceSampleRate{};
    uint32_t deviceChannels{};
    // The device's self-test bits, kept so a reader can see them later.
    uint32_t selfTestBits{};
    // THE PCM THE GUEST ENQUEUED.  This is the P1 measurement: does the
    // guest's own FMOD produce sound for this device to carry?  The
    // device's self-test bytes are excluded.
    uint32_t pcmEnqueues{};
    uint32_t pcmBytes{};
    uint32_t pcmFrames{};
    // THE FORMAT THE GUEST ASKED FOR, read out of its SLDataFormat.
    uint32_t formatRate{};
    uint32_t formatChannels{};
    uint32_t formatBits{};
};

FmodAudioStubStats fmodAudioStubStats();

// Self-test bits:
//  bit0 slCreateEngine silent success
//  bit1 Realize + GetInterface engine
//  bit2 CreateOutputMix / CreateAudioPlayer silent success
//  bit3 Enqueue discards without blocking and without claiming audible output
//  bit4 AAudio stream create returns a clear unavailable error
//  bit5 Virtual OpenSL / AAudio dlopen handles resolve entry points
//  bit6 the volume interface answers, and the device reports a LIVE level
//  bit7 the Android configuration interface reports the device's format
//  bit8 the virtual libOpenSLES.so exports every symbol the guest's real
//       FMOD looks up (MEASURED from the strings inside libfmodex.so)
//  bit9 an IID the object does not have is an EXPLICIT failure
//
// THE EXPECTED MASK IS A FUNCTION, NOT A LITERAL: the boot log prints it
// next to the value, so a new check cannot leave the printed expectation
// behind.
uint64_t fmodAudioStubSelfTest();
uint64_t fmodAudioStubSelfTestExpected();

// OpenSL ES C entry (silent device). Returns SL_RESULT_SUCCESS with a
// discard-only engine. Buffers are consumed immediately so mixer threads
// never block waiting for a missing Android device.
uint32_t slCreateEngine(void** engine, uint32_t numOptions, const void* options,
                        uint32_t numInterfaces, const void* interfaceIds,
                        const void* interfaceRequired);

// AAudio C entries. Clear unavailable error — no silent "started" claim.
int32_t aaudioCreateStreamBuilder(void** builder);
int32_t aaudioStreamBuilderOpenStream(void* builder, void** stream);
void aaudioStreamBuilderDelete(void* builder);
int32_t aaudioStream_requestStart(void* stream);
int32_t aaudioStream_requestStop(void* stream);
int32_t aaudioStream_close(void* stream);
int32_t aaudioStream_write(void* stream, const void* buffer, int32_t numFrames,
                           int64_t timeoutNanoseconds);
int32_t aaudioStream_getState(void* stream);

// Virtual DSO handles for guestDlopen("libOpenSLES.so" / "libaaudio.so").
bool isVirtualAudioLibrary(const char* fileName);
void* virtualAudioLibraryHandle(const char* fileName);
void* resolveVirtualAudioSymbol(void* handle, const char* symbolName);

} // namespace rr3
