// Copyright 2021 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_RENDERER_ELINUX_SHADER_CONTEXT_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_RENDERER_ELINUX_SHADER_CONTEXT_H_

#include <GLES3/gl32.h>

#include <string>

#include "flutter/shell/platform/linux_embedded/logger.h"

namespace flutter {

class ELinuxShaderContext {
 public:
  ELinuxShaderContext(std::string code, GLenum type);
  ~ELinuxShaderContext();

  GLuint Shader() const { return shader_; }

 private:
  GLuint shader_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_RENDERER_ELINUX_SHADER_CONTEXT_H_
