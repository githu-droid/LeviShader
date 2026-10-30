#pragma once
#include <EGL/egl.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace lsx {

struct Settings {
  std::atomic<bool> enabled{false};
  std::atomic<int> packIndex{0};      // 0 = off, N = packNames()[N-1]
  std::atomic<float> renderScale{1.0f};
};

Settings& settings();

void setPackRoot(const std::filesystem::path& root);
void refreshPackList();
const std::vector<std::string>& packNames();

// Called from the eglSwapBuffers detour, on the game's render thread, BEFORE the real swap.
void onSwap(EGLDisplay dpy, EGLSurface surf);

// Drops CPU-side state. (GL objects belong to the game's context; see PostFx.cpp.)
void shutdown();

}  // namespace lsx
