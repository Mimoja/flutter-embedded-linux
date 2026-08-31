// Copyright 2023 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_SURFACE_GL_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_SURFACE_GL_H_

#include <memory>

#include "flutter/shell/platform/linux_embedded/surface/context_egl.h"
#include "flutter/shell/platform/linux_embedded/surface/surface_base.h"
#include "flutter/shell/platform/linux_embedded/surface/surface_gl_delegate.h"

namespace flutter {

class SurfaceGl final : public SurfaceBase, public SurfaceGlDelegate {
 public:
  SurfaceGl(std::unique_ptr<ContextEgl> context);
  ~SurfaceGl() = default;

  // |SurfaceGlDelegate|
  bool GLContextMakeCurrent() const override;

  // |SurfaceGlDelegate|
  bool GLContextClearCurrent() const override;

  // |SurfaceGlDelegate|
  bool GLContextPresent(uint32_t fbo_id) const override;

  // |SurfaceGlDelegate|
  bool GLContextPresentWithInfo(const FlutterPresentInfo* info) const override;

  // |SurfaceGlDelegate|
  void PopulateExistingDamage(const intptr_t fbo_id,
                              FlutterDamage* existing_damage) const override;

  // |SurfaceGlDelegate|
  uint32_t GLContextFBO() const override;

  // |SurfaceGlDelegate|
  void* GlProcResolver(const char* name) const override;

 private:
  // Impeller needs a working depth/stencil attachment, but some GLES drivers
  // (e.g. Vivante on i.MX8) have broken depth buffers on window surfaces.
  // Render into a client framebuffer with real renderbuffers instead and
  // blit to the window surface on present. Returns 0 (the window surface)
  // when Impeller is off or the framebuffer could not be created.
  uint32_t EnsureOffscreenFramebuffer() const;
  void DestroyOffscreenFramebuffer() const;
  void BlitOffscreenFramebuffer() const;

  mutable uint32_t fbo_ = 0;
  mutable uint32_t color_rb_ = 0;
  mutable uint32_t depth_stencil_rb_ = 0;
  mutable uint32_t resolve_fbo_ = 0;
  mutable uint32_t resolve_rb_ = 0;
  mutable bool multisampled_ = false;
  mutable int fbo_width_ = 0;
  mutable int fbo_height_ = 0;
  mutable bool fbo_failed_ = false;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_SURFACE_GL_H_
