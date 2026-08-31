// Copyright 2026 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/surface/gl_vivante_workarounds.h"

#include <EGL/egl.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "flutter/shell/platform/linux_embedded/logger.h"

namespace flutter {

namespace {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef char GLchar;

constexpr GLenum kGlRenderer = 0x1F01;

using GlShaderSourceProc = void (*)(GLuint shader,
                                    GLsizei count,
                                    const GLchar* const* string,
                                    const GLint* length);
using GlGetStringProc = const GLubyte* (*)(GLenum name);

GlShaderSourceProc g_real_shader_source = nullptr;

// Impeller's texture_downsample.frag iterates with bounds taken from a
// uniform ("edge"). The Vivante GLSL linker crashes (SIGSEGV in libVSC's
// vscLinkProgram) when linking that pattern, so the same math is expressed
// with constant loop bounds and early exits. The interface (uniform block,
// sampler, varying and output names) must match what SPIRV-Cross emits for
// the original shader.
constexpr char kDownsampleFragmentShader[] = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(std140) uniform FragInfo
{
    highp float edge;
    highp float ratio;
    highp vec2 pixel_size;
} frag_info;

uniform mediump sampler2D texture_sampler;

in highp vec2 v_texture_coords;
layout(location = 0) out highp vec4 frag_color;

void main()
{
    highp vec4 total = vec4(0.0);
    highp float x = -frag_info.edge;
    for (int i = 0; i < 8; i++)
    {
        if (x > frag_info.edge) { break; }
        highp float y = -frag_info.edge;
        for (int j = 0; j < 8; j++)
        {
            if (y > frag_info.edge) { break; }
            total += texture(texture_sampler, v_texture_coords + (frag_info.pixel_size * vec2(x, y)), -0.4749999940395355224609375) * frag_info.ratio;
            y += 2.0;
        }
        x += 2.0;
    }
    frag_color = total;
}
)GLSL";

bool IsVivanteRenderer() {
  static int cached = -1;
  if (cached < 0) {
    cached = 0;
    auto get_string =
        reinterpret_cast<GlGetStringProc>(eglGetProcAddress("glGetString"));
    if (get_string) {
      auto* renderer = reinterpret_cast<const char*>(get_string(kGlRenderer));
      if (renderer && (std::strstr(renderer, "Vivante") ||
                       std::strstr(renderer, "VeriSilicon"))) {
        cached = 1;
      }
    }
  }
  return cached == 1;
}

// Impeller's clip pipelines use a fragment shader with an empty main() and
// the color mask off; only their depth/stencil side effects matter. The
// Vivante compiler treats such draws as having no side effects and culls
// them, so clips never write the stencil mask or depth wall. Give the shader
// an output write (masked off by glColorMask) so the draw survives.
constexpr char kClipFragmentShader[] = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(location = 0) out highp vec4 frag_color;

void main()
{
    frag_color = vec4(0.0);
}
)GLSL";

bool IsEmptyFragmentShader(const std::string& source) {
  if (source.size() > 160) {
    return false;
  }
  if (source.find("void main()") == std::string::npos) {
    return false;
  }
  // No outputs, no builtins written: nothing between the braces of main.
  const auto open = source.find("void main()");
  const auto lbrace = source.find('{', open);
  const auto rbrace = source.find('}', lbrace);
  if (lbrace == std::string::npos || rbrace == std::string::npos) {
    return false;
  }
  for (auto i = lbrace + 1; i < rbrace; i++) {
    if (!std::isspace(static_cast<unsigned char>(source[i]))) {
      return false;
    }
  }
  return true;
}

bool IsDownsampleShader(const std::string& source) {
  return source.find("uniform FragInfo") != std::string::npos &&
         source.find("float edge;") != std::string::npos &&
         source.find("float ratio;") != std::string::npos &&
         source.find("vec2 pixel_size;") != std::string::npos;
}

void GlShaderSourceWithWorkaround(GLuint shader,
                                  GLsizei count,
                                  const GLchar* const* string,
                                  const GLint* length) {
  if (IsVivanteRenderer() && string) {
    std::string source;
    for (GLsizei i = 0; i < count; i++) {
      if (!string[i]) {
        continue;
      }
      if (length && length[i] >= 0) {
        source.append(string[i], static_cast<size_t>(length[i]));
      } else {
        source.append(string[i]);
      }
    }
    if (IsDownsampleShader(source)) {
      ELINUX_LOG(INFO) << "Applying Vivante loop-bound workaround to shader "
                       << shader;
      const GLchar* replacement = kDownsampleFragmentShader;
      g_real_shader_source(shader, 1, &replacement, nullptr);
      return;
    }
    if (IsEmptyFragmentShader(source)) {
      ELINUX_LOG(INFO) << "Applying Vivante empty-shader workaround to shader "
                       << shader;
      const GLchar* replacement = kClipFragmentShader;
      g_real_shader_source(shader, 1, &replacement, nullptr);
      return;
    }
  }
  g_real_shader_source(shader, count, string, length);
}

// Impeller uses GL_EXT_shader_framebuffer_fetch for advanced blends when the
// driver advertises it, but on Vivante framebuffer fetch returns garbage when
// rendering into renderbuffer-backed framebuffers (which the embedder uses to
// get a working depth/stencil attachment). Hide the framebuffer-fetch
// extensions so the engine falls back to classic blending.
bool IsMaskedExtension(const char* name) {
  return std::strstr(name, "shader_framebuffer_fetch") != nullptr;
}

using GlGetStringiProc = const GLubyte* (*)(GLenum, GLuint);
using GlGetIntegervProc = void (*)(GLenum, GLint*);

GlGetStringProc g_real_get_string = nullptr;
GlGetStringiProc g_real_get_stringi = nullptr;
GlGetIntegervProc g_real_get_integerv = nullptr;

constexpr GLenum kGlExtensions = 0x1F03;
constexpr GLenum kGlNumExtensions = 0x821D;

std::vector<std::string>& FilteredExtensionList() {
  static std::vector<std::string> list = []() {
    std::vector<std::string> extensions;
    if (!g_real_get_integerv) {
      g_real_get_integerv = reinterpret_cast<GlGetIntegervProc>(
          eglGetProcAddress("glGetIntegerv"));
    }
    if (!g_real_get_stringi) {
      g_real_get_stringi =
          reinterpret_cast<GlGetStringiProc>(eglGetProcAddress("glGetStringi"));
    }
    if (g_real_get_integerv && g_real_get_stringi) {
      GLint count = 0;
      g_real_get_integerv(kGlNumExtensions, &count);
      for (GLint i = 0; i < count; i++) {
        auto* name = reinterpret_cast<const char*>(
            g_real_get_stringi(kGlExtensions, static_cast<GLuint>(i)));
        if (name && !IsMaskedExtension(name)) {
          extensions.emplace_back(name);
        }
      }
    }
    return extensions;
  }();
  return list;
}

const GLubyte* GlGetStringWithWorkaround(GLenum name) {
  const GLubyte* result = g_real_get_string(name);
  if (name == kGlExtensions && result && IsVivanteRenderer()) {
    static const std::string filtered = []() {
      std::string joined;
      auto* all =
          reinterpret_cast<const char*>(g_real_get_string(kGlExtensions));
      std::string ext;
      for (const char* p = all;; p++) {
        if (*p == ' ' || *p == '\0') {
          if (!ext.empty() && !IsMaskedExtension(ext.c_str())) {
            joined += ext;
            joined += ' ';
          }
          ext.clear();
          if (*p == '\0') {
            break;
          }
        } else {
          ext += *p;
        }
      }
      return joined;
    }();
    return reinterpret_cast<const GLubyte*>(filtered.c_str());
  }
  return result;
}

const GLubyte* GlGetStringiWithWorkaround(GLenum name, GLuint index) {
  if (name == kGlExtensions && IsVivanteRenderer()) {
    auto& list = FilteredExtensionList();
    if (index < list.size()) {
      return reinterpret_cast<const GLubyte*>(list[index].c_str());
    }
    return nullptr;
  }
  return g_real_get_stringi(name, index);
}

void GlGetIntegervWithWorkaround(GLenum pname, GLint* data) {
  if (pname == kGlNumExtensions && IsVivanteRenderer()) {
    *data = static_cast<GLint>(FilteredExtensionList().size());
    return;
  }
  g_real_get_integerv(pname, data);
}

// The Vivante driver lets a glBufferData/glBufferSubData re-specification
// race the GPU's reads of the previous contents, which corrupts Impeller's
// per-frame vertex data (the engine orphans one big vertex buffer each
// frame). Serialize the pipeline before the buffer is re-specified.
using GlBufferDataProc = void (*)(GLenum, long, const void*, GLenum);
using GlFinishProc = void (*)(void);

GlBufferDataProc g_real_buffer_data = nullptr;
GlFinishProc g_real_finish = nullptr;

using GlBufferSubDataProc = void (*)(GLenum, long, long, const void*);

GlBufferSubDataProc g_real_buffer_sub_data = nullptr;

void GlSerializeGpu() {
  if (!g_real_finish) {
    g_real_finish =
        reinterpret_cast<GlFinishProc>(eglGetProcAddress("glFinish"));
  }
  if (g_real_finish) {
    g_real_finish();
  }
}

void GlBufferDataWithWorkaround(GLenum target,
                                long size,
                                const void* data,
                                GLenum usage) {
  if (IsVivanteRenderer()) {
    GlSerializeGpu();
  }
  g_real_buffer_data(target, size, data, usage);
}

// The driver also appears to consume the user pointer passed to
// glBufferSubData lazily instead of copying it during the call. Impeller
// recycles its staging memory right after the call, so freshly built screens
// come up half-rendered from clobbered vertex data until they are drawn
// again. A barrier after the call forces the copy; writes are a few per
// frame, so this is cheap.
void GlBufferSubDataWithWorkaround(GLenum target,
                                   long offset,
                                   long size,
                                   const void* data) {
  g_real_buffer_sub_data(target, offset, size, data);
  if (IsVivanteRenderer()) {
    GlSerializeGpu();
  }
}

using GlTexSubImage2DProc = void (*)(GLenum,
                                     GLint,
                                     GLint,
                                     GLint,
                                     GLsizei,
                                     GLsizei,
                                     GLenum,
                                     GLenum,
                                     const void*);

GlTexSubImage2DProc g_real_tex_sub_image_2d = nullptr;

// Texture uploads take the same lazily-consumed user pointer path.
void GlTexSubImage2DWithWorkaround(GLenum target,
                                   GLint level,
                                   GLint xoffset,
                                   GLint yoffset,
                                   GLsizei width,
                                   GLsizei height,
                                   GLenum format,
                                   GLenum type,
                                   const void* data) {
  g_real_tex_sub_image_2d(target, level, xoffset, yoffset, width, height,
                          format, type, data);
  if (IsVivanteRenderer()) {
    GlSerializeGpu();
  }
}

// Impeller attaches its combined depth+stencil renderbuffer to the separate
// GL_DEPTH_ATTACHMENT and GL_STENCIL_ATTACHMENT points. The Vivante driver
// leaves such framebuffers incomplete (and Impeller does not check), so every
// offscreen pass - route transitions, save layers - renders nothing. Attach
// to the combined GL_DEPTH_STENCIL_ATTACHMENT instead, which the driver
// handles correctly; the second (duplicate) attach call is idempotent.
constexpr GLenum kGlDepthAttachment = 0x8D00;
constexpr GLenum kGlStencilAttachment = 0x8D20;
constexpr GLenum kGlDepthStencilAttachment = 0x821A;

using GlFramebufferRenderbufferProc = void (*)(GLenum, GLenum, GLenum, GLuint);

GlFramebufferRenderbufferProc g_real_framebuffer_renderbuffer = nullptr;

void GlFramebufferRenderbufferWithWorkaround(GLenum target,
                                             GLenum attachment,
                                             GLenum renderbuffertarget,
                                             GLuint renderbuffer) {
  if (IsVivanteRenderer() && (attachment == kGlDepthAttachment ||
                              attachment == kGlStencilAttachment)) {
    attachment = kGlDepthStencilAttachment;
  }
  g_real_framebuffer_renderbuffer(target, attachment, renderbuffertarget,
                                  renderbuffer);
}

// Vivante only exports the core ES 3.0 instancing entry points, but the
// engine's GLES proc table asks for the EXT-suffixed names.
const char* CoreNameForExtProc(const char* name) {
  if (std::strcmp(name, "glDrawArraysInstancedEXT") == 0) {
    return "glDrawArraysInstanced";
  }
  if (std::strcmp(name, "glDrawElementsInstancedEXT") == 0) {
    return "glDrawElementsInstanced";
  }
  if (std::strcmp(name, "glVertexAttribDivisorEXT") == 0) {
    return "glVertexAttribDivisor";
  }
  return nullptr;
}

bool WorkaroundsDisabled() {
  static const bool disabled = []() {
    auto* value = std::getenv("FLUTTER_ELINUX_DISABLE_GL_WORKAROUNDS");
    return value && std::strcmp(value, "1") == 0;
  }();
  return disabled;
}

}  // namespace

bool IsVivanteDisplay(EGLDisplay display) {
  const char* vendor = eglQueryString(display, EGL_VENDOR);
  return vendor &&
         (std::strstr(vendor, "Vivante") || std::strstr(vendor, "VeriSilicon"));
}

bool VivanteWorkaroundsEnabled(EGLDisplay display) {
  return !WorkaroundsDisabled() && IsVivanteDisplay(display);
}

void WarnImpellerOnVivante() {
  static bool warned = false;
  if (warned) {
    return;
  }
  warned = true;
  ELINUX_LOG(WARNING)
      << "Impeller is running on a Vivante (VeriSilicon) GPU. This driver "
         "has known defects (shader linker crashes, broken window-surface "
         "depth/stencil, lazily consumed upload pointers, non-functional "
         "MSAA) and driver workarounds are engaged: rendering goes through "
         "an offscreen framebuffer, GPU/CPU synchronization is stricter, "
         "and framebuffer-fetch is hidden. Visual glitches may still occur. "
         "Set FLUTTER_ELINUX_DISABLE_GL_WORKAROUNDS=1 to disable the "
         "workarounds (expect severe rendering corruption).";
}

void* GlVivanteWorkaround(const char* name, void* address) {
  if (WorkaroundsDisabled() || !name) {
    return nullptr;
  }
  if (address && std::strcmp(name, "glShaderSource") == 0) {
    g_real_shader_source = reinterpret_cast<GlShaderSourceProc>(address);
    return reinterpret_cast<void*>(GlShaderSourceWithWorkaround);
  }
  if (address && std::strcmp(name, "glGetString") == 0) {
    g_real_get_string = reinterpret_cast<GlGetStringProc>(address);
    return reinterpret_cast<void*>(GlGetStringWithWorkaround);
  }
  if (address && std::strcmp(name, "glGetStringi") == 0) {
    g_real_get_stringi = reinterpret_cast<GlGetStringiProc>(address);
    return reinterpret_cast<void*>(GlGetStringiWithWorkaround);
  }
  if (address && std::strcmp(name, "glBufferSubData") == 0) {
    g_real_buffer_sub_data = reinterpret_cast<GlBufferSubDataProc>(address);
    return reinterpret_cast<void*>(GlBufferSubDataWithWorkaround);
  }
  if (address && std::strcmp(name, "glTexSubImage2D") == 0) {
    g_real_tex_sub_image_2d = reinterpret_cast<GlTexSubImage2DProc>(address);
    return reinterpret_cast<void*>(GlTexSubImage2DWithWorkaround);
  }
  if (address && std::strcmp(name, "glFramebufferRenderbuffer") == 0) {
    g_real_framebuffer_renderbuffer =
        reinterpret_cast<GlFramebufferRenderbufferProc>(address);
    return reinterpret_cast<void*>(GlFramebufferRenderbufferWithWorkaround);
  }
  if (address && std::strcmp(name, "glBufferData") == 0) {
    g_real_buffer_data = reinterpret_cast<GlBufferDataProc>(address);
    return reinterpret_cast<void*>(GlBufferDataWithWorkaround);
  }
  if (address && std::strcmp(name, "glGetIntegerv") == 0) {
    g_real_get_integerv = reinterpret_cast<GlGetIntegervProc>(address);
    return reinterpret_cast<void*>(GlGetIntegervWithWorkaround);
  }
  if (!address) {
    if (auto* core_name = CoreNameForExtProc(name)) {
      auto core = eglGetProcAddress(core_name);
      if (core) {
        ELINUX_LOG(INFO) << "Forwarding " << name << " to " << core_name;
        return reinterpret_cast<void*>(core);
      }
    }
  }
  return nullptr;
}

}  // namespace flutter
