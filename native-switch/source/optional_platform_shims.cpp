#include "rr3/optional_platform_shims.hpp"

#include <cstring>

namespace rr3 {
namespace {

constexpr int32_t kArUnsupportedDevice = 100;
constexpr int32_t kArInstallDeclined = 1;
constexpr int32_t kArTrackingStopped = 2;
constexpr int32_t kArErrorFatal = -2;
alignas(16) unsigned char gArConfigToken[16]{};
alignas(16) unsigned char gArFrameToken[16]{};
alignas(16) unsigned char gArPoseToken[16]{};

} // namespace

void arCoreApkCheckAvailability(void*, void*, int32_t* availability) {
    if (availability != nullptr) {
        *availability = kArUnsupportedDevice;
    }
}

int32_t arCoreApkRequestInstall(void*, void*, bool, int32_t* installStatus) {
    if (installStatus != nullptr) {
        *installStatus = kArInstallDeclined;
    }
    return kArErrorFatal;
}

int32_t arSessionCreate(void*, void*, void** session) {
    if (session != nullptr) {
        *session = nullptr;
    }
    return kArErrorFatal;
}

void arSessionDestroy(void*) {}
int32_t arSessionConfigure(void*, void*) { return kArErrorFatal; }
int32_t arSessionPause(void*) { return 0; }
int32_t arSessionResume(void*) { return kArErrorFatal; }
void arSessionSetCameraTextureName(void*, uint32_t) {}
int32_t arSessionUpdate(void*, void*) { return kArErrorFatal; }

void arConfigCreate(void*, void** config) {
    if (config != nullptr) {
        *config = gArConfigToken;
    }
}
void arConfigDestroy(void*) {}

void arFrameCreate(void*, void** frame) {
    if (frame != nullptr) {
        *frame = gArFrameToken;
    }
}
void arFrameDestroy(void*) {}
void arFrameAcquireCamera(void*, void*, void** camera) {
    if (camera != nullptr) {
        *camera = nullptr;
    }
}

void arCameraGetTrackingState(void*, void*, int32_t* trackingState) {
    if (trackingState != nullptr) {
        *trackingState = kArTrackingStopped;
    }
}
void arCameraGetDisplayOrientedPose(void*, void*, void*) {}

void arPoseCreate(void*, const float*, void** pose) {
    if (pose != nullptr) {
        *pose = gArPoseToken;
    }
}
void arPoseDestroy(void*) {}
void arPoseGetMatrix(void*, void*, float* matrix) {
    if (matrix == nullptr) {
        return;
    }
    std::memset(matrix, 0, sizeof(float) * 16);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

void* echoCreate(...) { return nullptr; }
void echoDestroy(...) {}
int echoLogin(...) { return 0; }
int echoResume(...) { return 0; }
int echoSet(...) { return 0; }
int echoSuspend(...) { return 0; }

uint64_t optionalPlatformSelfTest() {
    uint64_t passed = 0;
    int32_t availability = 0;
    arCoreApkCheckAvailability(nullptr, nullptr, &availability);
    if (availability == kArUnsupportedDevice) {
        passed |= 1ULL << 0;
    }
    void* session = reinterpret_cast<void*>(1);
    if (arSessionCreate(nullptr, nullptr, &session) == kArErrorFatal && session == nullptr) {
        passed |= 1ULL << 1;
    }
    float matrix[16]{};
    arPoseGetMatrix(nullptr, nullptr, matrix);
    if (matrix[0] == 1.0f && matrix[5] == 1.0f &&
        matrix[10] == 1.0f && matrix[15] == 1.0f) {
        passed |= 1ULL << 2;
    }
    if (echoCreate() == nullptr) {
        passed |= 1ULL << 3;
    }
    return passed;
}

} // namespace rr3
