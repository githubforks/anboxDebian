/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "RenderControl.h"

#include "ChecksumCalculatorThreadInfo.h"
#include "DispatchTables.h"
#include "DisplayManager.h"
#include "RenderThreadInfo.h"
#include "Renderer.h"
#include "RendererConfig.h"

#include "OpenGLESDispatch/EGLDispatch.h"

#include "anbox/graphics/layer_composer.h"
#include "anbox/logger.h"

#include <map>
#include <string>

static const GLint rendererVersion = 1;
static std::shared_ptr<anbox::graphics::LayerComposer> composer;
static std::shared_ptr<Renderer> renderer;

void registerLayerComposer(
    const std::shared_ptr<anbox::graphics::LayerComposer> &c) {
  composer = c;
}

void registerRenderer(const std::shared_ptr<Renderer> &r) {
  renderer = r;
}

static GLint rcGetRendererVersion() { return rendererVersion; }

static EGLint rcGetEGLVersion(EGLint *major, EGLint *minor) {
  if (!renderer)
    return EGL_FALSE;

  *major = (EGLint)renderer->getCaps().eglMajor;
  *minor = (EGLint)renderer->getCaps().eglMinor;

  return EGL_TRUE;
}

static EGLint rcQueryEGLString(EGLenum name, void *buffer, EGLint bufferSize) {
  if (!renderer)
    return 0;

  const char *str = s_egl.eglQueryString(renderer->getDisplay(), name);
  if (!str) {
    return 0;
  }

  int len = strlen(str) + 1;
  if (!buffer || len > bufferSize) {
    return -len;
  }

  strcpy((char *)buffer, str);
  return len;
}

static EGLint rcGetGLString(EGLenum name, void *buffer, EGLint bufferSize) {
  RenderThreadInfo *tInfo = RenderThreadInfo::get();
  std::string result;

  if (tInfo && tInfo->currContext) {
    const char *str = nullptr;
    if (tInfo->currContext->isGL2())
      str = reinterpret_cast<const char *>(s_gles2.glGetString(name));
    else
      str = reinterpret_cast<const char *>(s_gles1.glGetString(name));

    if (str) result += str;
  }

  // We're forcing version 2.0 no matter what the host provides as
  // our emulation layer isn't prepared for anything newer (yet).
  // This goes in parallel with filtering the extension set for
  // any unwanted extensions. If we don't force the right version
  // here certain parts of the system will assume API conditions
  // which aren't met.
  if (name == GL_VERSION)
    result = "OpenGL ES 2.0";
  else if (name == GL_EXTENSIONS) {
    std::string approved_extensions = result;
    std::vector<std::string> unsupported_extensions = {
        // Leaving this enabled gives crippeled text rendering when
        // using the host mesa GLES drivers.
        "GL_EXT_unpack_subimage",
    };

    for (const auto &extension : unsupported_extensions) {
      size_t start_pos = approved_extensions.find(extension);
      if (start_pos == std::string::npos) continue;
      approved_extensions.replace(start_pos, extension.length(), "");
    }

    result = approved_extensions;
  }

  int nextBufferSize = result.size() + 1;

  if (!buffer || nextBufferSize > bufferSize) return -nextBufferSize;

  snprintf(static_cast<char *>(buffer), nextBufferSize, "%s", result.c_str());
  return nextBufferSize;
}

static EGLint rcGetNumConfigs(uint32_t *p_numAttribs) {
  int numConfigs = 0, numAttribs = 0;

  renderer->getConfigs()->getPackInfo(&numConfigs, &numAttribs);
  if (p_numAttribs) {
    *p_numAttribs = static_cast<uint32_t>(numAttribs);
  }
  return numConfigs;
}

static EGLint rcGetConfigs(uint32_t bufSize, GLuint *buffer) {
  GLuint bufferSize = (GLuint)bufSize;
  return renderer->getConfigs()->packConfigs(bufferSize, buffer);
}

static EGLint rcChooseConfig(EGLint *attribs, uint32_t attribs_size,
                             uint32_t *configs, uint32_t configs_size) {
  if (!renderer || attribs_size == 0)
    return 0;

  return renderer->getConfigs()->chooseConfig(attribs, (EGLint *)configs,
                                        (EGLint)configs_size);
}

static EGLint rcGetFBParam(EGLint param) {
  if (!renderer)
    return 0;

  EGLint ret = 0;

  switch (param) {
    case FB_WIDTH:
      ret = DisplayManager::get()->display_info().horizontal_resolution;
      break;
    case FB_HEIGHT:
      ret = DisplayManager::get()->display_info().vertical_resolution;
      break;
    case FB_XDPI:
      ret = 72;  // XXX: should be implemented
      break;
    case FB_YDPI:
      ret = 72;  // XXX: should be implemented
      break;
    case FB_FPS:
      ret = 60;
      break;
    case FB_MIN_SWAP_INTERVAL:
      ret = 1;  // XXX: should be implemented
      break;
    case FB_MAX_SWAP_INTERVAL:
      ret = 1;  // XXX: should be implemented
      break;
    default:
      break;
  }

  return ret;
}

static uint32_t rcCreateContext(uint32_t config, uint32_t share,
                                uint32_t glVersion) {
  if (!renderer)
    return 0;

  // To make it consistent with the guest, create GLES2 context when GL
  // version==2 or 3
  HandleType ret =
      renderer->createRenderContext(config, share, glVersion == 2 || glVersion == 3);
  return ret;
}

static void rcDestroyContext(uint32_t context) {
  if (!renderer)
    return;

  renderer->DestroyRenderContext(context);
}

static uint32_t rcCreateWindowSurface(uint32_t config, uint32_t width,
                                      uint32_t height) {
  if (!renderer)
    return 0;

  return renderer->createWindowSurface(config, width, height);
}

static void rcDestroyWindowSurface(uint32_t windowSurface) {
  if (!renderer)
    return;

  renderer->DestroyWindowSurface(windowSurface);
}

static uint32_t rcCreateColorBuffer(uint32_t width, uint32_t height,
                                    GLenum internalFormat) {
  if (!renderer)
    return 0;

  return renderer->createColorBuffer(width, height, internalFormat);
}

static int rcOpenColorBuffer2(uint32_t colorbuffer) {
  if (!renderer)
    return -1;

  return renderer->openColorBuffer(colorbuffer);
}

// Deprecated, kept for compatibility with old system images only.
// Use rcOpenColorBuffer2 instead.
static void rcOpenColorBuffer(uint32_t colorbuffer) {
  (void)rcOpenColorBuffer2(colorbuffer);
}

static void rcCloseColorBuffer(uint32_t colorbuffer) {
  if (!renderer)
    return;

  renderer->closeColorBuffer(colorbuffer);
}

static int rcFlushWindowColorBuffer(uint32_t windowSurface) {
  if (!renderer)
    return -1;

  if (!renderer->flushWindowSurfaceColorBuffer(windowSurface))
    return -1;

  return 0;
}

static void rcSetWindowColorBuffer(uint32_t windowSurface,
                                   uint32_t colorBuffer) {
  if (!renderer)
    return;

  renderer->setWindowSurfaceColorBuffer(windowSurface, colorBuffer);
}

static EGLint rcMakeCurrent(uint32_t context, uint32_t drawSurf,
                            uint32_t readSurf) {
  if (!renderer)
    return EGL_FALSE;

  bool ret = renderer->bindContext(context, drawSurf, readSurf);

  return (ret ? EGL_TRUE : EGL_FALSE);
}

static void rcFBPost(uint32_t colorBuffer) { WARNING("Not implemented"); }

static void rcFBSetSwapInterval(EGLint interval) {
  // XXX: TBD - should be implemented
}

static void rcBindTexture(uint32_t colorBuffer) {
  if (!renderer)
    return;

  renderer->bindColorBufferToTexture(colorBuffer);
}

static void rcBindRenderbuffer(uint32_t colorBuffer) {
  if (!renderer)
    return;

  renderer->bindColorBufferToRenderbuffer(colorBuffer);
}

static EGLint rcColorBufferCacheFlush(uint32_t colorBuffer, EGLint postCount,
                                      int forRead) {
  // XXX: TBD - should be implemented
  return 0;
}

static void rcReadColorBuffer(uint32_t colorBuffer, GLint x, GLint y,
                              GLint width, GLint height, GLenum format,
                              GLenum type, void *pixels) {
  if (!renderer)
    return;

  renderer->readColorBuffer(colorBuffer, x, y, width, height, format, type, pixels);
}

static int rcUpdateColorBuffer(uint32_t colorBuffer, GLint x, GLint y,
                               GLint width, GLint height, GLenum format,
                               GLenum type, void *pixels) {
  if (!renderer)
    return -1;

  renderer->updateColorBuffer(colorBuffer, x, y, width, height, format, type, pixels);
  return 0;
}

static uint32_t rcCreateClientImage(uint32_t context, EGLenum target,
                                    GLuint buffer) {
  if (!renderer)
    return 0;

  return renderer->createClientImage(context, target, buffer);
}

static int rcDestroyClientImage(uint32_t image) {
  if (!renderer)
    return 0;

  return renderer->destroyClientImage(image);
}

static void rcSelectChecksumCalculator(uint32_t protocol, uint32_t reserved) {
  ChecksumCalculatorThreadInfo::setVersion(protocol);
}

int rcGetNumDisplays() {
  // For now we only support a single display but that single display
  // will contain more than one display so that we simply spawn up a big
  // virtual display which should match the real display arrangement
  // in most cases.
  return 1;
}

int rcGetDisplayWidth(uint32_t display_id) {
  (void)display_id;
  return DisplayManager::get()->display_info().horizontal_resolution;
}

int rcGetDisplayHeight(uint32_t display_id) {
  (void)display_id;
  return DisplayManager::get()->display_info().vertical_resolution;
}

int rcGetDisplayDpiX(uint32_t display_id) {
  (void)display_id;
  return 120;
}

int rcGetDisplayDpiY(uint32_t display_id) {
  (void)display_id;
  return 120;
}

int rcGetDisplayVsyncPeriod(uint32_t display_id) {
  (void)display_id;
  return 1;
}

static std::vector<Renderable> frame_layers;

bool is_layer_blacklisted(const std::string &name) {
  static std::vector<std::string> blacklist = {
      // The 'Sprite' layer is the mouse cursor Android uses as soon
      // as it has a pointer input device available. We don't want to
      // display this layer at all but don't have a good way of disabling
      // the cursor on the Android side yet.
      "Sprite",
  };
  return std::find(blacklist.begin(), blacklist.end(), name) != blacklist.end();
}

void rcPostLayer(const char *name, uint32_t color_buffer,
                 int32_t sourceCropLeft, int32_t sourceCropTop,
                 int32_t sourceCropRight, int32_t sourceCropBottom,
                 int32_t displayFrameLeft, int32_t displayFrameTop,
                 int32_t displayFrameRight, int32_t displayFrameBottom) {
  Renderable r{
      name,
      color_buffer,
      {displayFrameLeft, displayFrameTop, displayFrameRight,
       displayFrameBottom},
      {sourceCropLeft, sourceCropTop, sourceCropRight, sourceCropBottom}};
  frame_layers.push_back(r);
}

void rcPostAllLayersDone() {
  if (composer) composer->submit_layers(frame_layers);

  frame_layers.clear();
}

void initRenderControlContext(renderControl_decoder_context_t *dec) {
  dec->rcGetRendererVersion = rcGetRendererVersion;
  dec->rcGetEGLVersion = rcGetEGLVersion;
  dec->rcQueryEGLString = rcQueryEGLString;
  dec->rcGetGLString = rcGetGLString;
  dec->rcGetNumConfigs = rcGetNumConfigs;
  dec->rcGetConfigs = rcGetConfigs;
  dec->rcChooseConfig = rcChooseConfig;
  dec->rcGetFBParam = rcGetFBParam;
  dec->rcCreateContext = rcCreateContext;
  dec->rcDestroyContext = rcDestroyContext;
  dec->rcCreateWindowSurface = rcCreateWindowSurface;
  dec->rcDestroyWindowSurface = rcDestroyWindowSurface;
  dec->rcCreateColorBuffer = rcCreateColorBuffer;
  dec->rcOpenColorBuffer = rcOpenColorBuffer;
  dec->rcCloseColorBuffer = rcCloseColorBuffer;
  dec->rcSetWindowColorBuffer = rcSetWindowColorBuffer;
  dec->rcFlushWindowColorBuffer = rcFlushWindowColorBuffer;
  dec->rcMakeCurrent = rcMakeCurrent;
  dec->rcFBPost = rcFBPost;
  dec->rcFBSetSwapInterval = rcFBSetSwapInterval;
  dec->rcBindTexture = rcBindTexture;
  dec->rcBindRenderbuffer = rcBindRenderbuffer;
  dec->rcColorBufferCacheFlush = rcColorBufferCacheFlush;
  dec->rcReadColorBuffer = rcReadColorBuffer;
  dec->rcUpdateColorBuffer = rcUpdateColorBuffer;
  dec->rcOpenColorBuffer2 = rcOpenColorBuffer2;
  dec->rcCreateClientImage = rcCreateClientImage;
  dec->rcDestroyClientImage = rcDestroyClientImage;
  dec->rcSelectChecksumCalculator = rcSelectChecksumCalculator;
  dec->rcGetNumDisplays = rcGetNumDisplays;
  dec->rcGetDisplayWidth = rcGetDisplayWidth;
  dec->rcGetDisplayHeight = rcGetDisplayHeight;
  dec->rcGetDisplayDpiX = rcGetDisplayDpiX;
  dec->rcGetDisplayDpiY = rcGetDisplayDpiY;
  dec->rcGetDisplayVsyncPeriod = rcGetDisplayVsyncPeriod;
  dec->rcPostLayer = rcPostLayer;
  dec->rcPostAllLayersDone = rcPostAllLayersDone;
}
