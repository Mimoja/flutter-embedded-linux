// Copyright 2023 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/surface/surface_gl.h"

#include <EGL/egl.h>

#include "flutter/shell/platform/linux_embedded/logger.h"
#include "flutter/shell/platform/linux_embedded/surface/gl_vivante_workarounds.h"

namespace flutter {

namespace {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;

constexpr GLenum kGlFramebuffer = 0x8D40;
constexpr GLenum kGlReadFramebuffer = 0x8CA8;
constexpr GLenum kGlDrawFramebuffer = 0x8CA9;
constexpr GLenum kGlRenderbuffer = 0x8D41;
constexpr GLenum kGlColorAttachment0 = 0x8CE0;
constexpr GLenum kGlDepthStencilAttachment = 0x821A;
constexpr GLenum kGlRgb8 = 0x8051;
constexpr GLenum kGlDepth24Stencil8 = 0x88F0;
constexpr GLenum kGlFramebufferComplete = 0x8CD5;
constexpr GLbitfield kGlColorBufferBit = 0x4000;
constexpr GLenum kGlNearest = 0x2600;
constexpr GLenum kGlScissorTest = 0x0C11;

struct GlFboProcs {
  void (*disable)(GLenum);
  void (*renderbufferStorageMultisample)(GLenum,
                                         GLsizei,
                                         GLenum,
                                         GLsizei,
                                         GLsizei);
  void (*genFramebuffers)(GLsizei, GLuint*);
  void (*deleteFramebuffers)(GLsizei, const GLuint*);
  void (*bindFramebuffer)(GLenum, GLuint);
  void (*genRenderbuffers)(GLsizei, GLuint*);
  void (*deleteRenderbuffers)(GLsizei, const GLuint*);
  void (*bindRenderbuffer)(GLenum, GLuint);
  void (*renderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
  void (*framebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
  GLenum (*checkFramebufferStatus)(GLenum);
  void (*blitFramebuffer)(GLint,
                          GLint,
                          GLint,
                          GLint,
                          GLint,
                          GLint,
                          GLint,
                          GLint,
                          GLbitfield,
                          GLenum);
  void (*finish)(void);
  bool valid;
};

const GlFboProcs& FboProcs(const ContextEgl* context) {
  static GlFboProcs procs = [context]() {
    GlFboProcs p{};
#define RESOLVE(member, name)                            \
  p.member = reinterpret_cast<decltype(p.member)>(       \
      const_cast<void*>(context->GlProcResolver(name))); \
  if (!p.member) {                                       \
    ELINUX_LOG(ERROR) << "Failed to resolve " << name;   \
    p.valid = false;                                     \
    return p;                                            \
  }
    p.valid = true;
    RESOLVE(disable, "glDisable")
    RESOLVE(renderbufferStorageMultisample, "glRenderbufferStorageMultisample")
    RESOLVE(genFramebuffers, "glGenFramebuffers")
    RESOLVE(deleteFramebuffers, "glDeleteFramebuffers")
    RESOLVE(bindFramebuffer, "glBindFramebuffer")
    RESOLVE(genRenderbuffers, "glGenRenderbuffers")
    RESOLVE(deleteRenderbuffers, "glDeleteRenderbuffers")
    RESOLVE(bindRenderbuffer, "glBindRenderbuffer")
    RESOLVE(renderbufferStorage, "glRenderbufferStorage")
    RESOLVE(framebufferRenderbuffer, "glFramebufferRenderbuffer")
    RESOLVE(checkFramebufferStatus, "glCheckFramebufferStatus")
    RESOLVE(blitFramebuffer, "glBlitFramebuffer")
    RESOLVE(finish, "glFinish")
#undef RESOLVE
    return p;
  }();
  return procs;
}

}  // namespace

SurfaceGl::SurfaceGl(std::unique_ptr<ContextEgl> context) {
  context_ = std::move(context);
}

bool SurfaceGl::GLContextMakeCurrent() const {
  return onscreen_surface_->MakeCurrent();
}

bool SurfaceGl::GLContextClearCurrent() const {
  return context_->ClearCurrent();
}

uint32_t SurfaceGl::EnsureOffscreenFramebuffer() const {
  if (!context_->EnableImpeller() || fbo_failed_) {
    return 0;
  }
  EGLDisplay display = eglGetCurrentDisplay();
  EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
  if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) {
    return fbo_;
  }
  // The offscreen path exists to work around broken window-surface
  // depth/stencil attachments on Vivante; healthy drivers render directly
  // into the (multisampled) window surface as before.
  if (!VivanteWorkaroundsEnabled(display)) {
    return 0;
  }
  EGLint width = 0;
  EGLint height = 0;
  eglQuerySurface(display, surface, EGL_WIDTH, &width);
  eglQuerySurface(display, surface, EGL_HEIGHT, &height);
  if (width <= 0 || height <= 0) {
    return fbo_;
  }
  if (fbo_ != 0 && width == fbo_width_ && height == fbo_height_) {
    return fbo_;
  }
  const GlFboProcs& gl = FboProcs(context_.get());
  if (!gl.valid) {
    fbo_failed_ = true;
    return 0;
  }
  DestroyOffscreenFramebuffer();
  // The window surface's own MSAA is a silent no-op on this class of driver,
  // so antialiasing comes from a multisampled offscreen framebuffer that the
  // present blit resolves into the window. Fall back to single-sampled
  // attachments when multisampled ones don't yield a complete framebuffer.
  for (int samples = 4; samples >= 0; samples -= 4) {
    gl.genFramebuffers(1, &fbo_);
    gl.bindFramebuffer(kGlFramebuffer, fbo_);
    gl.genRenderbuffers(1, &color_rb_);
    gl.bindRenderbuffer(kGlRenderbuffer, color_rb_);
    if (samples > 0) {
      gl.renderbufferStorageMultisample(kGlRenderbuffer, samples, kGlRgb8,
                                        width, height);
    } else {
      gl.renderbufferStorage(kGlRenderbuffer, kGlRgb8, width, height);
    }
    gl.framebufferRenderbuffer(kGlFramebuffer, kGlColorAttachment0,
                               kGlRenderbuffer, color_rb_);
    gl.genRenderbuffers(1, &depth_stencil_rb_);
    gl.bindRenderbuffer(kGlRenderbuffer, depth_stencil_rb_);
    if (samples > 0) {
      gl.renderbufferStorageMultisample(kGlRenderbuffer, samples,
                                        kGlDepth24Stencil8, width, height);
    } else {
      gl.renderbufferStorage(kGlRenderbuffer, kGlDepth24Stencil8, width,
                             height);
    }
    gl.framebufferRenderbuffer(kGlFramebuffer, kGlDepthStencilAttachment,
                               kGlRenderbuffer, depth_stencil_rb_);
    if (gl.checkFramebufferStatus(kGlFramebuffer) == kGlFramebufferComplete) {
      multisampled_ = samples > 0;
      break;
    }
    gl.bindFramebuffer(kGlFramebuffer, 0);
    DestroyOffscreenFramebuffer();
    if (samples == 0) {
      ELINUX_LOG(ERROR) << "Offscreen framebuffer incomplete; falling back "
                           "to the window surface";
      fbo_failed_ = true;
      return 0;
    }
  }
  if (multisampled_) {
    // A multisample resolve blit requires identical read/draw formats, which
    // the window's default framebuffer does not guarantee. Resolve into a
    // single-sampled renderbuffer of the same format first; the copy from
    // there to the window is an ordinary blit that may convert.
    gl.genFramebuffers(1, &resolve_fbo_);
    gl.bindFramebuffer(kGlFramebuffer, resolve_fbo_);
    gl.genRenderbuffers(1, &resolve_rb_);
    gl.bindRenderbuffer(kGlRenderbuffer, resolve_rb_);
    gl.renderbufferStorage(kGlRenderbuffer, kGlRgb8, width, height);
    gl.framebufferRenderbuffer(kGlFramebuffer, kGlColorAttachment0,
                               kGlRenderbuffer, resolve_rb_);
    if (gl.checkFramebufferStatus(kGlFramebuffer) != kGlFramebufferComplete) {
      gl.deleteFramebuffers(1, &resolve_fbo_);
      gl.deleteRenderbuffers(1, &resolve_rb_);
      resolve_fbo_ = 0;
      resolve_rb_ = 0;
      multisampled_ = false;
    }
    gl.bindFramebuffer(kGlFramebuffer, fbo_);
  }
  fbo_width_ = width;
  fbo_height_ = height;
  ELINUX_LOG(INFO) << "Rendering Impeller into a " << width << "x" << height
                   << " client framebuffer";
  return fbo_;
}

void SurfaceGl::DestroyOffscreenFramebuffer() const {
  const GlFboProcs& gl = FboProcs(context_.get());
  if (!gl.valid) {
    return;
  }
  if (resolve_fbo_ != 0) {
    gl.deleteFramebuffers(1, &resolve_fbo_);
    resolve_fbo_ = 0;
  }
  if (resolve_rb_ != 0) {
    gl.deleteRenderbuffers(1, &resolve_rb_);
    resolve_rb_ = 0;
  }
  if (fbo_ != 0) {
    gl.deleteFramebuffers(1, &fbo_);
    fbo_ = 0;
  }
  if (color_rb_ != 0) {
    gl.deleteRenderbuffers(1, &color_rb_);
    color_rb_ = 0;
  }
  if (depth_stencil_rb_ != 0) {
    gl.deleteRenderbuffers(1, &depth_stencil_rb_);
    depth_stencil_rb_ = 0;
  }
  fbo_width_ = 0;
  fbo_height_ = 0;
}

// Copies the offscreen framebuffer to the window surface. The default
// framebuffer must stay bound across eglSwapBuffers: the Vivante driver only
// reliably posts the window surface when FBO 0 is bound at swap time. The
// engine sets fbo_reset_after_present, so it re-queries the FBO afterwards.
void SurfaceGl::BlitOffscreenFramebuffer() const {
  const GlFboProcs& gl = FboProcs(context_.get());
  // glBlitFramebuffer honors the scissor test; a scissor left over from the
  // last draw would confine the copy (and show stale content outside it).
  gl.disable(kGlScissorTest);
  uint32_t source = fbo_;
  if (multisampled_ && resolve_fbo_ != 0) {
    gl.bindFramebuffer(kGlReadFramebuffer, fbo_);
    gl.bindFramebuffer(kGlDrawFramebuffer, resolve_fbo_);
    gl.blitFramebuffer(0, 0, fbo_width_, fbo_height_, 0, 0, fbo_width_,
                       fbo_height_, kGlColorBufferBit, kGlNearest);
    source = resolve_fbo_;
  }
  gl.bindFramebuffer(kGlReadFramebuffer, source);
  gl.bindFramebuffer(kGlDrawFramebuffer, 0);
  gl.blitFramebuffer(0, 0, fbo_width_, fbo_height_, 0, 0, fbo_width_,
                     fbo_height_, kGlColorBufferBit, kGlNearest);
  gl.bindFramebuffer(kGlFramebuffer, 0);
}

bool SurfaceGl::GLContextPresent(uint32_t fbo_id) const {
  if (fbo_ != 0) {
    BlitOffscreenFramebuffer();
  }
  if (!onscreen_surface_->SwapBuffers()) {
    return false;
  }
  native_window_->SwapBuffers();
  return true;
}

bool SurfaceGl::GLContextPresentWithInfo(const FlutterPresentInfo* info) const {
  if (fbo_ != 0) {
    BlitOffscreenFramebuffer();
  }
  if (!onscreen_surface_->SwapBuffers(info)) {
    return false;
  }
  native_window_->SwapBuffers();
  return true;
}

void SurfaceGl::PopulateExistingDamage(const intptr_t fbo_id,
                                       FlutterDamage* existing_damage) const {
  onscreen_surface_->PopulateExistingDamage(fbo_id, existing_damage);
}

uint32_t SurfaceGl::GLContextFBO() const {
  return EnsureOffscreenFramebuffer();
}

void* SurfaceGl::GlProcResolver(const char* name) const {
  return context_->GlProcResolver(name);
}

}  // namespace flutter
