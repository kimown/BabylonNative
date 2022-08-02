#pragma once

#include <bgfx/bgfx.h>
#include <napi/napi.h>
#include <NativeCamera.h>
#include <arcana/threading/task.h>

namespace Babylon::Plugins
{
    class Camera::Impl final : public std::enable_shared_from_this<Camera::Impl>
    {
    public:
        Impl(Napi::Env env, bool overrideCameraTexture);
        ~Impl();
        arcana::task<void, std::exception_ptr> Open(uint32_t width, uint32_t height, bool frontCamera);
        void SetTextureOverride(void* texturePtr);
        void UpdateCameraTexture(bgfx::TextureHandle textureHandle);
        void Close();
    };
}