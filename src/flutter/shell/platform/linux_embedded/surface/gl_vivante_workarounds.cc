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

bool WorkaroundsDisabled() {
  static const bool disabled = []() {
    auto* value = std::getenv("FLUTTER_ELINUX_DISABLE_GL_WORKAROUNDS");
    return value && std::strcmp(value, "1") == 0;
  }();
  return disabled;
}

}  // namespace

void* GlVivanteWorkaround(const char* name, void* address) {
  if (WorkaroundsDisabled() || !name) {
    return nullptr;
  }
  if (address && std::strcmp(name, "glShaderSource") == 0) {
    g_real_shader_source = reinterpret_cast<GlShaderSourceProc>(address);
    return reinterpret_cast<void*>(GlShaderSourceWithWorkaround);
  }
  return nullptr;
}

}  // namespace flutter
