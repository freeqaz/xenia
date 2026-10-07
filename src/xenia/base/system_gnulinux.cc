/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <alloca.h>
#include <dlfcn.h>
#include <stdlib.h>

#include <cstdio>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"

// Use headers in third party to not depend on system sdl headers for building
#include "third_party/SDL2/include/SDL.h"

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
  auto cmd = std::string("xdg-open ");
  cmd.append(url);
  system(cmd.c_str());
}

void LaunchFileExplorer(const std::filesystem::path& path) { assert_always(); }

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  // Check if a display server is available. Without X11 or Wayland, SDL2
  // message boxes will fail (e.g. headless mode, SSH, no GUI).
  const char* x11_display = std::getenv("DISPLAY");
  const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
  bool has_display = (x11_display && x11_display[0] != '\0') ||
                     (wayland_display && wayland_display[0] != '\0');

  if (!has_display) {
    // Fall back to stderr when no display is available.
    const char* prefix;
    switch (type) {
      default:
      case SimpleMessageBoxType::Help:
        prefix = "[Xenia Help] ";
        break;
      case SimpleMessageBoxType::Warning:
        prefix = "[Xenia Warning] ";
        break;
      case SimpleMessageBoxType::Error:
        prefix = "[Xenia Error] ";
        break;
    }
    std::fprintf(stderr, "%s%.*s\n", prefix, static_cast<int>(message.size()),
                 message.data());
    return;
  }

  void* libsdl2 = dlopen("libSDL2.so", RTLD_LAZY | RTLD_LOCAL);
  assert_not_null(libsdl2);
  if (libsdl2) {
    auto* pSDL_ShowSimpleMessageBox =
        reinterpret_cast<decltype(SDL_ShowSimpleMessageBox)*>(
            dlsym(libsdl2, "SDL_ShowSimpleMessageBox"));
    assert_not_null(pSDL_ShowSimpleMessageBox);
    if (pSDL_ShowSimpleMessageBox) {
      Uint32 flags;
      const char* title;
      char* message_copy = reinterpret_cast<char*>(alloca(message.size() + 1));
      std::memcpy(message_copy, message.data(), message.size());
      message_copy[message.size()] = '\0';

      switch (type) {
        default:
        case SimpleMessageBoxType::Help:
          title = "Xenia Help";
          flags = SDL_MESSAGEBOX_INFORMATION;
          break;
        case SimpleMessageBoxType::Warning:
          title = "Xenia Warning";
          flags = SDL_MESSAGEBOX_WARNING;
          break;
        case SimpleMessageBoxType::Error:
          title = "Xenia Error";
          flags = SDL_MESSAGEBOX_ERROR;
          break;
      }
      pSDL_ShowSimpleMessageBox(flags, title, message_copy, NULL);
    }
    dlclose(libsdl2);
  }
}

}  // namespace xe
