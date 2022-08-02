#include <napi/napi.h>
#include "NativeCamera.h"
#include "NativeCameraImpl.h"
#include <string>
#include <android/native_window_jni.h>
#include <AndroidExtensions/Globals.h>
#include <AndroidExtensions/Permissions.h>
#include <android/log.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <arcana/threading/dispatcher.h>
#include <Babylon/JsRuntimeScheduler.h>
#include <Babylon/Graphics/DeviceContext.h>
#include <arcana/threading/task_schedulers.h>
#include <arcana/macros.h>
#include <memory>

using namespace android;
using namespace android::global;

namespace Babylon::Plugins
{
    static constexpr char CAMERA_VERT_SHADER[]{R"(#version 300 es
        precision highp float;
        out vec2 cameraFrameUV;
        void main() {
            cameraFrameUV = vec2(gl_VertexID&1, (gl_VertexID &2)>>1) * 2.f;
            gl_Position = vec4(cameraFrameUV * 2.f - 1.f, 0.0, 1.0);
        }
    )"};

    static constexpr char CAMERA_FRAG_SHADER[]{R"(#version 300 es
        #extension GL_OES_EGL_image_external_essl3 : require
        precision mediump float;
        in vec2 cameraFrameUV;
        uniform samplerExternalOES cameraTexture;
        layout(location = 0) out vec4 oFragColor;
        void main() {
            oFragColor = texture(cameraTexture, cameraFrameUV);
        }
    )"};

    GLuint Camera::Impl::GenerateOESTexture()
    {
        GLuint oesTexture;
        glGenTextures(1, &oesTexture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTexture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        return oesTexture;
    }

    std::string Camera::Impl::GetCameraId(bool frontCamera)
    {
        API24::ACameraIdList *cameraIds = nullptr;
        GET_CAMERA_FUNCTION(ACameraManager_getCameraIdList)(m_cameraManager, &cameraIds);

        std::string cameraId{};

        for (int i = 0; i < cameraIds->numCameras; ++i)
        {
            const char *id = cameraIds->cameraIds[i];

            API24::ACameraMetadata *metadataObj;
            GET_CAMERA_FUNCTION(ACameraManager_getCameraCharacteristics)(m_cameraManager, id, &metadataObj);

            API24::ACameraMetadata_const_entry lensInfo = {};
            GET_CAMERA_FUNCTION(ACameraMetadata_getConstEntry)(metadataObj, API24::ACAMERA_LENS_FACING, &lensInfo);

            auto facing = static_cast<API24::acamera_metadata_enum_android_lens_facing_t>(lensInfo.data.u8[0]);

            // Found a corresponding facing camera?
            if (facing == (frontCamera ? API24::ACAMERA_LENS_FACING_FRONT : API24::ACAMERA_LENS_FACING_BACK))
            {
                cameraId = id;
                break;
            }
        }

        GET_CAMERA_FUNCTION(ACameraManager_deleteCameraIdList)(cameraIds);
        return cameraId;
    }

    // device callbacks
    static void onDisconnected(void* /*context*/, API24::ACameraDevice* /*device*/)
    {
    }

    static void onError(void* /*context*/, API24::ACameraDevice* /*device*/, int /*error*/)
    {
    }

    static API24::ACameraDevice_stateCallbacks cameraDeviceCallbacks = {
            .context = nullptr,
            .onDisconnected = onDisconnected,
            .onError = onError
    };

    // session callbacks
    static void onSessionActive(void* /*context*/, API24::ACameraCaptureSession* /*session*/)
    {
    }

    static void onSessionReady(void* /*context*/, API24::ACameraCaptureSession* /*session*/)
    {
    }

    static void onSessionClosed(void* /*context*/, API24::ACameraCaptureSession* /*session*/)
    {
    }

    static API24::ACameraCaptureSession_stateCallbacks sessionStateCallbacks {
        .context = nullptr,
        .onClosed = onSessionClosed,
        .onReady = onSessionReady,
        .onActive = onSessionActive
    };

    // capture callbacks
    static void onCaptureFailed(void* /*context*/, API24::ACameraCaptureSession* /*session*/, API24::ACaptureRequest* /*request*/, API24::ACameraCaptureFailure* /*failure*/)
    {
    }

    static void onCaptureSequenceCompleted(void* /*context*/, API24::ACameraCaptureSession* /*session*/, int /*sequenceId*/, int64_t /*frameNumber*/)
    {
    }

    static void onCaptureSequenceAborted(void* /*context*/, API24::ACameraCaptureSession* /*session*/, int /*sequenceId*/)
    {
    }

    static void onCaptureCompleted (void* /*context*/, API24::ACameraCaptureSession* /*session*/, API24::ACaptureRequest* /*request*/, const API24::ACameraMetadata* /*result*/)
    {
    }

    static API24::ACameraCaptureSession_captureCallbacks captureCallbacks {
        .context = nullptr,
        .onCaptureStarted = nullptr,
        .onCaptureProgressed = nullptr,
        .onCaptureCompleted = onCaptureCompleted,
        .onCaptureFailed = onCaptureFailed,
        .onCaptureSequenceCompleted = onCaptureSequenceCompleted,
        .onCaptureSequenceAborted = onCaptureSequenceAborted,
        .onCaptureBufferLost = nullptr,
    };

    Camera::Impl::Impl(Napi::Env env, bool overrideCameraTexture)
        : m_deviceContext{nullptr}
        , m_env{env}
        , m_overrideCameraTexture{overrideCameraTexture}
    {
        if (API_LEVEL < 24 && !overrideCameraTexture)
        {
            throw std::runtime_error{"Android Platform level < 24. Only camera texture override is available."};
        }
    }

    Camera::Impl::~Impl()
    {
    }

    arcana::task<void, std::exception_ptr> Camera::Impl::Open(uint32_t width, uint32_t height, bool frontCamera)
    {
        if (!m_deviceContext){
            m_deviceContext = &Graphics::DeviceContext::GetFromJavaScript(m_env);
        }

        return android::Permissions::CheckCameraPermissionAsync().then(arcana::inline_scheduler, arcana::cancellation::none(), [this, width, height, frontCamera]()
        {
            m_width = width;
            m_height = height;
        
            // Check if there is an already available context for this thread
            EGLContext currentContext = eglGetCurrentContext();
            if (currentContext == EGL_NO_CONTEXT)
            {
                // create a shared context with bgfx so JNI thread (by surfaceTexture) can update the texture
                m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
                eglInitialize(m_display, nullptr, nullptr);

                static const EGLint attrs[] ={
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                    EGL_BLUE_SIZE, 8,
                    EGL_GREEN_SIZE, 8,
                    EGL_RED_SIZE, 8,
                    EGL_ALPHA_SIZE, 8,
                    EGL_DEPTH_SIZE, 16,
                    EGL_STENCIL_SIZE, 8,
                    EGL_NONE
                };

                EGLConfig  config;
                EGLint numConfig = 0;
                eglChooseConfig(m_display, attrs, &config, 1, &numConfig);

                static const EGLint contextAttribs[] = {
                    EGL_CONTEXT_MAJOR_VERSION_KHR,
                    3,
                    EGL_CONTEXT_MINOR_VERSION_KHR,
                    0,
                    EGL_NONE};

                m_context = eglCreateContext(m_display, config, bgfx::getInternalData()->context, contextAttribs);
                if (eglMakeCurrent(m_display, 0/*surface*/, 0/*surface*/, m_context) == EGL_FALSE)
                {
                    throw std::runtime_error{"Unable to create a shared GL context for camera texture."};
                }
            }

            glGenTextures(1, &m_cameraRGBATextureId);
            glBindTexture(GL_TEXTURE_2D, m_cameraRGBATextureId);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glGenerateMipmap(GL_TEXTURE_2D);

            glBindTexture(GL_TEXTURE_2D, 0);

            glGenFramebuffers(1, &m_frameBufferId);
            glBindFramebuffer(GL_FRAMEBUFFER, m_frameBufferId);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_cameraRGBATextureId, 0);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            m_cameraShaderProgramId = android::OpenGLHelpers::CreateShaderProgram(CAMERA_VERT_SHADER, CAMERA_FRAG_SHADER);

            if (API_LEVEL >= 24 && libCamera2NDK && !m_overrideCameraTexture) {
                m_cameraOESTextureId = GenerateOESTexture();

                // Create the surface and surface texture that will receive the camera preview
                m_surfaceTexture.InitWithTexture(m_cameraOESTextureId);
                android::view::Surface surface(m_surfaceTexture);

                // open the front or back camera
                m_cameraManager = GET_CAMERA_FUNCTION(ACameraManager_create)();
                auto id = GetCameraId(frontCamera);
                GET_CAMERA_FUNCTION(ACameraManager_openCamera)(m_cameraManager, id.c_str(),
                                                               &cameraDeviceCallbacks,
                                                               &m_cameraDevice);

                m_textureWindow = reinterpret_cast<API24::ANativeWindow *>(ANativeWindow_fromSurface(
                        GetEnvForCurrentThread(), surface));

                // Prepare request for texture target
                GET_CAMERA_FUNCTION(ACameraDevice_createCaptureRequest)(m_cameraDevice,
                                                                        API24::TEMPLATE_PREVIEW,
                                                                        &m_request);

                // Prepare outputs for session
                GET_CAMERA_FUNCTION(ACaptureSessionOutput_create)(m_textureWindow,
                                                                  &m_textureOutput);
                GET_CAMERA_FUNCTION(ACaptureSessionOutputContainer_create)(&m_outputs);
                GET_CAMERA_FUNCTION(ACaptureSessionOutputContainer_add)(m_outputs, m_textureOutput);

                // Prepare target surface
                GET_CAMERA_FUNCTION(ANativeWindow_acquire)(m_textureWindow);
                GET_CAMERA_FUNCTION(ACameraOutputTarget_create)(m_textureWindow, &m_textureTarget);
                GET_CAMERA_FUNCTION(ACaptureRequest_addTarget)(m_request, m_textureTarget);

                // Create the session
                GET_CAMERA_FUNCTION(ACameraDevice_createCaptureSession)(m_cameraDevice, m_outputs,
                                                                        &sessionStateCallbacks,
                                                                        &m_textureSession);

                // Start capturing continuously
                GET_CAMERA_FUNCTION(ACameraCaptureSession_setRepeatingRequest)(m_textureSession,
                                                                               &captureCallbacks,
                                                                               1, &m_request,
                                                                               nullptr);
            }

            if (eglMakeCurrent(m_display, 0/*surface*/, 0/*surface*/, currentContext) == EGL_FALSE)
            {
                throw std::runtime_error{"Unable to restore GL context for camera texture init."};
            }
        });
    }

    void Camera::Impl::SetTextureOverride(void* texturePtr)
    {
        if (!m_overrideCameraTexture)
        {
            throw std::runtime_error{"Trying to override NativeCamera Texture."};
        }
        m_cameraOESTextureId = reinterpret_cast<uintptr_t>(texturePtr);
    }

    void Camera::Impl::UpdateCameraTexture(bgfx::TextureHandle textureHandle)
    {
        EGLContext currentContext = eglGetCurrentContext();
        if (m_context != EGL_NO_CONTEXT)
        {
            // use the newly created shared context
            if (eglMakeCurrent(m_display, 0/*surface*/, 0/*surface*/, m_context) == EGL_FALSE)
            {
                throw std::runtime_error{"Unable to make current shared GL context for camera texture."};
            }
        }

        if (API_LEVEL >= 24 && !m_overrideCameraTexture)
        {
            m_surfaceTexture.updateTexImage();
        }

        glBindFramebuffer(GL_FRAMEBUFFER, m_frameBufferId);
        glViewport(0, 0, m_width, m_height);
        glUseProgram(m_cameraShaderProgramId);

        // Configure the camera texture
        auto cameraTextureUniformLocation{glGetUniformLocation(m_cameraShaderProgramId, "cameraTexture")};
        glUniform1i(cameraTextureUniformLocation, android::OpenGLHelpers::GetTextureUnit(GL_TEXTURE0));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_cameraOESTextureId);
        glBindSampler(android::OpenGLHelpers::GetTextureUnit(GL_TEXTURE0), 0);

        // Draw the quad
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 3);

        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // bind previously bound context
        if (eglMakeCurrent(m_display, 0/*surface*/, 0/*surface*/, currentContext) == EGL_FALSE)
        {
            throw std::runtime_error{"Unable to make current shared GL context for camera texture."};
        }

        arcana::make_task(m_deviceContext->BeforeRenderScheduler(), arcana::cancellation::none(), [this, textureHandle] {
            bgfx::overrideInternal(textureHandle, m_cameraRGBATextureId);
        });
    }

    void Camera::Impl::Close()
    {
        if (API_LEVEL >= 24 && !m_overrideCameraTexture)
        {
            // Stop recording to SurfaceTexture and do some cleanup
            GET_CAMERA_FUNCTION(ACameraCaptureSession_stopRepeating)(m_textureSession);
            GET_CAMERA_FUNCTION(ACameraCaptureSession_close)(m_textureSession);
            GET_CAMERA_FUNCTION(ACaptureSessionOutputContainer_free)(m_outputs);
            GET_CAMERA_FUNCTION(ACaptureSessionOutput_free)(m_output);

            GET_CAMERA_FUNCTION(ACameraDevice_close)(m_cameraDevice);
            GET_CAMERA_FUNCTION(ACameraManager_delete)(m_cameraManager);

            // Capture request for SurfaceTexture
            ANativeWindow_release(m_textureWindow);
            GET_CAMERA_FUNCTION(ACaptureRequest_free)(m_request);
        }

        if (m_context != EGL_NO_CONTEXT)
        {
            eglDestroyContext(m_display, m_context);
        }
    }
}