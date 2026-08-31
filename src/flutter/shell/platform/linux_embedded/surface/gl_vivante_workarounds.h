// Copyright 2026 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_GL_VIVANTE_WORKAROUNDS_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_GL_VIVANTE_WORKAROUNDS_H_

namespace flutter {

// Works around Vivante (VeriSilicon) GLES driver bugs that break Impeller.
//
// Returns a replacement entry point for |name| when one is needed, or
// nullptr when the driver's own |address| should be used as-is:
//  - "glShaderSource" is wrapped so that shaders using loops whose bounds
//    come from uniforms (Impeller's texture_downsample.frag) are rewritten
//    with compile-time-bounded loops. The Vivante shader linker (libVSC)
//    segfaults on the original pattern.
//
// The wrappers only alter behavior when the current GL_RENDERER reports a
// Vivante GPU. Set FLUTTER_ELINUX_DISABLE_GL_WORKAROUNDS=1 to turn this off.
void* GlVivanteWorkaround(const char* name, void* address);

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_GL_VIVANTE_WORKAROUNDS_H_
