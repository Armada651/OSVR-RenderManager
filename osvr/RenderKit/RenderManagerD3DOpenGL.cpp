
/** @file
@brief Source file implementing OpenGL-over-D3D-based OSVR direct-to-device
rendering interface

@date 2015

@author
Russ Taylor working through ReliaSolve.com for Sensics, Inc.
<http://sensics.com/osvr>
*/

// Copyright 2015 Sensics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <GL/glew.h>
#include <GL/wglew.h>
#include "RenderManagerD3DOpenGL.h"
#include "GraphicsLibraryD3D11.h"
#include "GraphicsLibraryOpenGL.h"
#include <iostream>

namespace osvr {
namespace renderkit {

    RenderManagerD3D11OpenGL::RenderManagerD3D11OpenGL(
        std::shared_ptr<osvr::clientkit::ClientContext> context,
        ConstructorParameters p,
        std::unique_ptr<RenderManagerD3D11Base>&& D3DToHarness)
        : RenderManagerOpenGL(context, p),
          m_D3D11Renderer(std::move(D3DToHarness)) {
        // Initialize all of the variables that don't have to be done in the
        // list above, so we don't get warnings about out-of-order
        // initialization if they are re-ordered in the header file.
        m_doingOkay = true;
        m_displayOpen = false;
        m_library.OpenGL = nullptr;

        if (!m_D3D11Renderer) {
            std::cerr << "RenderManagerD3D11OpenGL::RenderManagerD3D11OpenGL: "
                      << "NULL pointer to D3D Renderer to harness."
                      << std::endl;
            return;
        }

        // Construct the appropriate GraphicsLibrary pointer.
        m_library.OpenGL = new GraphicsLibraryOpenGL;
        m_buffers.OpenGL = new RenderBufferOpenGL;
    }

    RenderManagerD3D11OpenGL::~RenderManagerD3D11OpenGL() {
        cleanupGL();

        if (m_displayOpen) {
            /// @todo Add anything else we need to clean up
            m_displayOpen = false;
        }

        // @todo Clean up all of the mm_oglToD3D entries we have made.
    }

    void RenderManagerD3D11OpenGL::cleanupGL() {
        if (m_glD3DHandle) {
            for (size_t i = 0; i < m_oglToD3D.size(); i++) {
                wglDXUnregisterObjectNV(m_glD3DHandle,
                                        m_oglToD3D[i].glColorHandle);
            }
            wglDXCloseDeviceNV(m_glD3DHandle);
            m_glD3DHandle = nullptr;
        }
        delete m_buffers.OpenGL;
        m_buffers.OpenGL = nullptr;
        delete m_library.OpenGL;
        m_library.OpenGL = nullptr;
        removeOpenGLContexts();
    }

    RenderManager::OpenResults RenderManagerD3D11OpenGL::OpenDisplay(void) {
        // All public methods that use internal state should be guarded
        // by a mutex.
        std::lock_guard<std::mutex> lock(m_mutex);

        OpenResults ret;
        ret.status = FAILURE;
        if (!doingOkay()) {
            return ret;
        }

        //======================================================
        // We need to call glewInit() so that we have access to
        // the wgl extensions needed below.  This means that we
        // need to get an OpenGL context open.  We use methods
        // in our OpenGL parent class to open the desired context.
        // We leave the context open so that we can get our
        // RenderBuffer names.
        GLContextParams p;
        p.visible = false; // Make the context invisible, to not distract
        if (!addOpenGLContext(p)) {
            std::cerr << "RenderManagerD3D11OpenGL::OpenDisplay: Can't open GL "
                         "context"
                      << std::endl;
            return ret;
        }
        glewExperimental = true; // Needed for core profile
        if (glewInit() != GLEW_OK) {
            removeOpenGLContexts();
            std::cerr << "RenderManagerD3D11OpenGL::OpenDisplay: Can't "
                         "initialize GLEW"
                      << std::endl;
            return ret;
        }

        //======================================================
        // Open the D3D display we're going to use and get a handle
        // on it to use to map our OpenGL objects.
        ret = m_D3D11Renderer->OpenDisplay();
        if (ret.status == FAILURE) {
            std::cerr << "RenderManagerD3D11OpenGL::OpenDisplay: Can't open "
                         "D3D11 display"
                      << std::endl;
            m_doingOkay = false;
            return ret;
        }
        m_glD3DHandle = wglDXOpenDeviceNV(ret.library.D3D11->device);
        if (m_glD3DHandle == nullptr) {
            std::cerr << "RenderManagerD3D11OpenGL::OpenDisplay: Can't get GL "
                         "device handle"
                      << std::endl;
            ret.status = FAILURE;
            return ret;
        }

        //======================================================
        // Construct the present buffers we're going to use when in Render()
        // mode, to
        // wrap the PresenMode interface.
        if (!constructRenderBuffers()) {
            removeOpenGLContexts();
            std::cerr << "RenderManagerOpenGL::OpenDisplay: Could not "
                         "construct present buffers to wrap Render() path"
                      << std::endl;
            ret.status = FAILURE;
            return ret;
        }

        //======================================================
        // Register the presentation buffers, which will cause them to
        // be associated with newly-constructed D3D buffers.
        if (!RegisterRenderBuffersInternal(m_colorBuffers)) {
            removeOpenGLContexts();
            std::cerr << "RenderManagerOpenGL::OpenDisplay: Could not regsiter "
                         "present buffers to wrap Render() path"
                      << std::endl;
            ret.status = FAILURE;
            return ret;
        }

        // Fill in our library with the things the application may need to
        // use to do its graphics state set-up.
        ret.library = m_library;
        ret.buffers = m_buffers;
        m_displayOpen = true;
        return ret;
    }

    bool RenderManagerD3D11OpenGL::RenderFrameInitialize() {
        RenderManagerOpenGL::RenderFrameInitialize();
        if (m_D3D11Renderer == nullptr) {
            return false;
        }
        return m_D3D11Renderer->RenderFrameInitialize();
    }

    bool RenderManagerD3D11OpenGL::RenderDisplayInitialize(size_t display) {
        if (m_D3D11Renderer == nullptr) {
            return false;
        }
        // @todo Do we need this?
        return m_D3D11Renderer->RenderDisplayInitialize(display);
    }

    bool RenderManagerD3D11OpenGL::RenderEyeInitialize(size_t eye) {
        checkForGLError(
            "RenderManagerD3D11OpenGL::RenderEyeInitialize beginning");

        // Attach the Direct3D buffers to our framebuffer object
        glBindFramebuffer(GL_FRAMEBUFFER, m_frameBuffer);
        checkForGLError(
            "RenderManagerD3D11OpenGL::RenderEyeInitialize BindFrameBuffer");
        glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             m_colorBuffers[eye].OpenGL->colorBufferName, 0);
        checkForGLError(
            "RenderManagerD3D11OpenGL::RenderEyeInitialize Bind color buffer");
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, m_depthBuffers[eye]);
        checkForGLError(
            "RenderManagerD3D11OpenGL::RenderEyeInitialize Bind depth buffer");

        // Set the list of draw buffers.
        GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, DrawBuffers); // "1" is the size of DrawBuffers
        checkForGLError(
            "RenderManagerD3D11OpenGL::RenderEyeInitialize DrawBuffers");

        // Always check that our framebuffer is ok
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "RenderManagerD3D11OpenGL::RenderEyeInitialize: "
                         "Incomplete Framebuffer"
                      << std::endl;
            return false;
        }

        // Set the viewport for rendering this eye
        OSVR_ViewportDescription v;
        ConstructViewportForRender(eye, v);
        glViewport(static_cast<GLint>(v.left), static_cast<GLint>(v.lower),
                   static_cast<GLint>(v.width), static_cast<GLint>(v.height));

        // Set the OpenGL projection matrix based on the one we
        // computed.
        GLdouble projection[16];
        OSVR_Projection_to_OpenGL(projection,
                                  m_renderInfoForRender[eye].projection);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMultMatrixd(projection);

        // Set the matrix mode to ModelView, so render code doesn't mess with
        // the projection matrix on accident.
        glMatrixMode(GL_MODELVIEW);

        // Call the display set-up callback, which needs to be done now for each
        // eye
        if (m_displayCallback.m_callback != nullptr) {
            m_displayCallback.m_callback(m_displayCallback.m_userData,
                                         m_library, m_colorBuffers[eye]);
        }
        checkForGLError("RenderManagerD3D11OpenGL::RenderEyeInitialize after "
                        "display user callback");

        return true;
    }

    bool RenderManagerD3D11OpenGL::PresentDisplayInitialize(size_t display) {
        if (m_D3D11Renderer == nullptr) {
            return false;
        }
        return m_D3D11Renderer->PresentDisplayInitialize(display);
    }

    bool RenderManagerD3D11OpenGL::PresentDisplayFinalize(size_t display) {
        if (m_D3D11Renderer == nullptr) {
            return false;
        }
        return m_D3D11Renderer->PresentDisplayFinalize(display);
    }

    bool RenderManagerD3D11OpenGL::PresentFrameInitialize() {
        if (m_D3D11Renderer == nullptr) {
            return false;
        }
        return m_D3D11Renderer->PresentFrameInitialize();
    }

    bool RenderManagerD3D11OpenGL::PresentFrameFinalize() {
        if (m_D3D11Renderer == nullptr) {
            return false;
        }
        return m_D3D11Renderer->PresentFrameFinalize();
    }

    bool RenderManagerD3D11OpenGL::RegisterRenderBuffersInternal(
        const std::vector<RenderBuffer>& buffers,
        bool appWillNotOverwriteBeforeNewPresent) {
        // Make sure we're doing okay.
        if (!doingOkay()) {
            std::cerr << "RenderManagerD3D11OpenGL::RegisterRenderBuffers(): "
                         "Display not opened."
                      << std::endl;
            return false;
        }

        // Make sure we don't have more eyes than buffers.  We can have fewer
        // because the client may have consolidated a number of eyes onto
        // one buffer.
        size_t numEyes = GetNumEyes();
        if (buffers.size() > numEyes) {
            std::cerr << "RenderManagerD3D11OpenGL::RegisterRenderBuffers: "
                         "Wrong number of buffers: "
                      << buffers.size() << ", need " << numEyes << std::endl;
            return false;
        }

        // Delete any previously-registered buffers.
        for (size_t i = 0; i < m_oglToD3D.size(); i++) {
            wglDXUnregisterObjectNV(m_glD3DHandle, m_oglToD3D[i].glColorHandle);
            m_oglToD3D[i].D3DrenderTargetView->Release();
            m_oglToD3D[i].D3DTexture->Release();
        }
        m_oglToD3D.clear();

        // Allocate D3D buffers to be used and tie them to the OpenGL buffers.
        for (size_t i = 0; i < buffers.size(); i++) {
            // If we have already mapped this buffer, we go ahead and skip it,
            // so that we don't tie the same OpenGL buffer to multiple D3D
            // buffers.  It is not an error to map the same buffer twice.
            bool found = false;
            for (size_t j = 0; j < i; j++) {
                if (buffers[i].OpenGL->colorBufferName ==
                    buffers[j].OpenGL->colorBufferName) {
                    found = true;
                }
            }
            if (found) {
                continue;
            }

            // Figure out how large the buffer should be by binding this texture
            // and querying the size of the 0th mipmap level.
            GLint width = 0, height = 0;
            glBindTexture(GL_TEXTURE_2D, buffers[i].OpenGL->colorBufferName);
            const GLint mipLevel = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, mipLevel, GL_TEXTURE_WIDTH,
                                     &width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, mipLevel, GL_TEXTURE_HEIGHT,
                                     &height);
            if ((width == 0) || (height == 0)) {
                std::cerr << "RenderManagerD3D11OpenGL::RegisterRenderBuffers: "
                             "Zero-sized buffer for buffer: "
                          << i << std::endl;
                return false;
            }

            // Initialize a new render target texture description.
            // Make it a shared texture so that we can use it with OpenGL
            // Interop.
            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = width;
            textureDesc.Height = height;
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            // textureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.SampleDesc.Quality = 0;
            textureDesc.Usage = D3D11_USAGE_DEFAULT;
            // We need it to be both a render target and a shader resource
            textureDesc.BindFlags =
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            textureDesc.CPUAccessFlags = 0;
            textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

            // Create a new render target texture to use.
            ID3D11Texture2D* D3DTexture = nullptr;
            HRESULT hr = m_D3D11Renderer->m_D3D11device->CreateTexture2D(
                &textureDesc, NULL, &D3DTexture);
            if (FAILED(hr)) {
                std::cerr << "RenderManagerD3D11OpenGL::RegisterRenderBuffers: "
                             "Can't create texture"
                          << std::endl;
                return false;
            }

            // Fill in the resource view for your render texture buffer here
            D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc = {};
            // This must match what was created in the texture to be rendered
            // @todo Figure this out by introspection on the texture?
            // renderTargetViewDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            renderTargetViewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            renderTargetViewDesc.Texture2D.MipSlice = 0;

            // Create the render target view.
            ID3D11RenderTargetView*
                renderTargetView; //< Pointer to our render target view
            hr = m_D3D11Renderer->m_D3D11device->CreateRenderTargetView(
                D3DTexture, &renderTargetViewDesc, &renderTargetView);
            if (FAILED(hr)) {
                std::cerr << "RenderManagerD3D11OpenGL::RegisterRenderBuffers: "
                             "Could not create render target"
                          << std::endl;
                return false;
            }

            // Create a share handle for the texture, to enable it to be shared
            // between multiple devices.  Then register the share handle with
            // interop.
            // https://msdn.microsoft.com/en-us/library/windows/desktop/ff476531(v=vs.85).aspx
            // https://www.opengl.org/registry/specs/NV/DX_interop.txt
            IDXGIResource* pOtherResource = nullptr;
            hr = D3DTexture->QueryInterface(__uuidof(IDXGIResource),
                                            (void**)&pOtherResource);
            HANDLE sharedHandle;
            hr = pOtherResource->GetSharedHandle(&sharedHandle);
            if (FAILED(hr)) {
                std::cerr << "RenderManagerD3D11OpenGL::RegisterRenderBuffers: "
                             "Could not get shared handle"
                          << std::endl;
                return false;
            }
            if (wglDXSetResourceShareHandleNV(D3DTexture, sharedHandle) !=
                TRUE) {
                std::cerr << "RenderManagerD3D11OpenGL::RegisterRenderBuffers()"
                             ": Could not share resource"
                          << std::endl;
                return false;
            }

            // Prepare the things we need for wrapping Direct3D
            // objects. Information on how to do this comes from
            // the DX_interop2.txt file from opengl.org and from the
            // secondstory/ofDxSharedTextureExample project on Github.
            // Bind the OpenGL texture to the D3D texture for rendering
            HANDLE glColorHandle = wglDXRegisterObjectNV(
                m_glD3DHandle, D3DTexture, buffers[i].OpenGL->colorBufferName,
                GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
            if (glColorHandle == nullptr) {
                std::cerr << "RenderManagerD3D11OpenGL::RegisterRenderBuffers: "
                             "Can't get Color buffer handle"
                          << " (error " << GetLastError() << ")";
                switch (GetLastError()) {
                case ERROR_INVALID_HANDLE:
                    std::cerr << "  (Invalid handle)" << std::endl;
                    break;
                case ERROR_INVALID_DATA:
                    std::cerr << "  (Invalid data)" << std::endl;
                    break;
                case ERROR_OPEN_FAILED:
                    std::cerr << "  (Could not open Direct3D resource)"
                              << std::endl;
                    break;
                default:
                    std::cerr << "  (Unexpected error code)" << std::endl;
                }
                return false;
            }

            // New object to fill in
            OglToD3DTexture map;
            map.OpenGLTexture = buffers[i].OpenGL->colorBufferName;
            map.glColorHandle = glColorHandle;
            map.D3DTexture = D3DTexture;
            map.D3DrenderTargetView = renderTargetView;
            m_oglToD3D.push_back(map);

            // Lock the render target for OpenGL access
            if (!wglDXLockObjectsNV(m_glD3DHandle, 1, &map.glColorHandle)) {
                std::cerr << "RenderManagerD3D11OpenGL::RegisterRenderBuffers: "
                             "Can't lock Color buffer"
                          << std::endl;
                return false;
            }
        }

        // We're done -- call the base-class function to notify that we've
        // registered our buffers
        return RenderManager::RegisterRenderBuffersInternal(buffers,
          appWillNotOverwriteBeforeNewPresent);
    }

    bool RenderManagerD3D11OpenGL::PresentEye(PresentEyeParameters params) {
        if (params.m_buffer.OpenGL == nullptr) {
            std::cerr
                << "RenderManagerD3D11OpenGL::PresentEye(): NULL buffer pointer"
                << std::endl;
            return false;
        }

        // Verify that we have registered this buffer.
        OglToD3DTexture* oglMap = nullptr;
        for (size_t i = 0; i < m_oglToD3D.size(); i++) {
            if (m_oglToD3D[i].OpenGLTexture ==
                params.m_buffer.OpenGL->colorBufferName) {
                oglMap = &m_oglToD3D[i];
            }
        }
        if (oglMap == nullptr) {
            std::cerr
                << "RenderManagerD3D11OpenGL::PresentEye(): Unregistered buffer"
                << " (call RegisterRenderBuffers before presenting)"
                << std::endl;
            return false;
        }
        if (params.m_buffer.OpenGL->colorBufferName != oglMap->OpenGLTexture) {
            std::cerr
                << "RenderManagerD3D11OpenGL::PresentEye(): Mis-matched buffer"
                << " (call RegisterRenderBuffers whenever a new render-texture "
                   "is created)"
                << std::endl;
            return false;
        }

        // Create a new param buffer and fill in the new D3D buffer pointer
        // along with our current info.  Invert the flip-buffer flag, because
        // we need to render upside-down.  Rotate around the negative Z axis
        // rather than the positive.  Then call the D3D PresentEye method
        // and clean up after ourselves.
        PresentEyeParameters sendParams = params;
        sendParams.m_flipInY = !sendParams.m_flipInY;
        sendParams.m_buffer.OpenGL = nullptr;
        sendParams.m_buffer.D3D11 = new RenderBufferD3D11;
        sendParams.m_buffer.D3D11->colorBuffer = oglMap->D3DTexture;
        sendParams.m_buffer.D3D11->colorBufferView =
            oglMap->D3DrenderTargetView;

        // Transpose the texture matrix because we're feeding it to Direct3D,
        // which stores matrix elements in a different order.  We're sending
        // it our OpenGL transforms because we used OpenGL to render the two
        // images, so that's what the texture matrix needs to assume.
        matrix16 textureMat;
        if (params.m_ATW != nullptr) {
            // Transpose the matrix, because it is in OpenGL format.
            for (size_t r = 0; r < 4; r++) {
                for (size_t c = 0; c < 4; c++) {
                    textureMat.data[r * 4 + c] = params.m_ATW->data[c * 4 + r];
                }
            }
            sendParams.m_ATW = &textureMat;
        }

        // Unlock the render target to enable Direct3D access
        if (!wglDXUnlockObjectsNV(m_glD3DHandle, 1, &oglMap->glColorHandle)) {
            std::cerr << "RenderManagerD3D11OpenGL::PresentEye: Can't unlock "
                         "Color buffer"
                      << std::endl;
            return false;
        }

        // Render the the eye with the relevant texture.
        bool ret = m_D3D11Renderer->PresentEye(sendParams);

        // Lock the render target for OpenGL access
        if (!wglDXLockObjectsNV(m_glD3DHandle, 1, &oglMap->glColorHandle)) {
            std::cerr << "RenderManagerD3D11OpenGL::PresentEye: Can't lock "
                         "Color buffer"
                      << std::endl;
            return false;
        }

        delete sendParams.m_buffer.D3D11;
        return ret;
    }

} // namespace renderkit
} // namespace osvr
