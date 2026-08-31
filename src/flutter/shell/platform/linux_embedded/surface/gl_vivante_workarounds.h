// Copyright 2026 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_GL_VIVANTE_WORKAROUNDS_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_GL_VIVANTE_WORKAROUNDS_H_

#include <EGL/egl.h>

namespace flutter {

// True when |display| is driven by a Vivante (VeriSilicon) GPU. Uses the EGL
// vendor string, so it works before any GL context exists.
bool IsVivanteDisplay(EGLDisplay display);

// True when the Vivante workarounds should be applied for |display|:
// a Vivante GPU and FLUTTER_ELINUX_DISABLE_GL_WORKAROUNDS is not set.
bool VivanteWorkaroundsEnabled(EGLDisplay display);

// Emits a one-time, prominent warning that Impeller is running on a Vivante
// GPU with driver workarounds engaged.
void WarnImpellerOnVivante();

// Works around Vivante (VeriSilicon) GLES driver bugs that break Impeller.
//
// Returns a replacement entry point for |name| when one is needed, or
// nullptr when the driver's own |address| should be used as-is:
//  - "glShaderSource" is wrapped so that shaders using loops whose bounds
//    come from uniforms (Impeller's texture_downsample.frag) are rewritten
//    with compile-time-bounded loops. The Vivante shader linker (libVSC)
//    segfaults on the original pattern.
//  - The glDrawArraysInstancedEXT / glDrawElementsInstancedEXT /
//    glVertexAttribDivisorEXT names, which Vivante does not export, are
//    forwarded to their core OpenGL ES 3.0 equivalents.
//
// The wrappers only alter behavior when the current GL_RENDERER reports a
// Vivante GPU. Set FLUTTER_ELINUX_DISABLE_GL_WORKAROUNDS=1 to turn this off.
void* GlVivanteWorkaround(const char* name, void* address);

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_GL_VIVANTE_WORKAROUNDS_H_
