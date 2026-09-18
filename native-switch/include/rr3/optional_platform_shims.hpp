#pragma once

#include <cstdint>

namespace rr3 {

void arCoreApkCheckAvailability(void* env, void* context, int32_t* availability);
int32_t arCoreApkRequestInstall(void* env, void* activity, bool userRequested,
                               int32_t* installStatus);
int32_t arSessionCreate(void* env, void* context, void** session);
void arSessionDestroy(void* session);
int32_t arSessionConfigure(void* session, void* config);
int32_t arSessionPause(void* session);
int32_t arSessionResume(void* session);
void arSessionSetCameraTextureName(void* session, uint32_t textureName);
int32_t arSessionUpdate(void* session, void* frame);
void arConfigCreate(void* session, void** config);
void arConfigDestroy(void* config);
void arFrameCreate(void* session, void** frame);
void arFrameDestroy(void* frame);
void arFrameAcquireCamera(void* session, void* frame, void** camera);
void arCameraGetTrackingState(void* session, void* camera, int32_t* trackingState);
void arCameraGetDisplayOrientedPose(void* session, void* camera, void* pose);
void arPoseCreate(void* session, const float* rawPose, void** pose);
void arPoseDestroy(void* pose);
void arPoseGetMatrix(void* session, void* pose, float* matrix);

void* echoCreate(...);
void echoDestroy(...);
int echoLogin(...);
int echoResume(...);
int echoSet(...);
int echoSuspend(...);

uint64_t optionalPlatformSelfTest();

} // namespace rr3
