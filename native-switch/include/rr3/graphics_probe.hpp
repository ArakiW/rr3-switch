#pragma once

#include <cstdint>

struct NWindow;

namespace rr3 {

struct GraphicsProbe {
    bool attempted{};
    bool displayCreated{};
    bool initialized{};
    bool configChosen{};
    bool contextCreated{};
    bool surfaceCreated{};
    bool madeCurrent{};
    bool drawCompleted{};
    bool vertexShaderCompiled{};
    bool fragmentShaderCompiled{};
    bool programLinked{};
    bool realisticShaderCompiled{};
    bool realisticProgramLinked{};
    bool textureUploaded{};
    bool texturedDrawCompleted{};
    bool assetOpened{};
    bool assetInflated{};
    bool assetDdsParsed{};
    bool s3tcSupported{};
    bool realAssetUploaded{};
    bool realAssetDrawCompleted{};
    uint32_t assetWidth{};
    uint32_t assetHeight{};
    char assetFourcc[8]{};
    int major{};
    int minor{};
    uint32_t glError{};
    uint32_t eglError{};
    uint32_t stage{};
    uint32_t surfaceMode{};
    char version[96]{};
    char shaderLog[192]{};
};

GraphicsProbe runWindowGraphicsProbe(NWindow* window);

} // namespace rr3
