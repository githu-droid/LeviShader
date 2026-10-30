// LeviShaders — Java-shader-pack style post-processing for Minecraft Bedrock on LeviLauncher.
#include <android/log.h>
#include <dlfcn.h>
#include <EGL/egl.h>

#include <cstdlib>
#include <string>
#include <string_view>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include "PostFx.hpp"

namespace {

constexpr const char* kTag = "LeviShaders";
constexpr const char* kModule = "levishaders.postfx";

using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
SwapFn g_origSwap = nullptr;
pl::memory::HookHandle g_swapHook;

EGLBoolean swapDetour(EGLDisplay dpy, EGLSurface surf) {
  lsx::onSwap(dpy, surf);  // draw post-process into the back buffer, then let the game present
  return g_origSwap ? g_origSwap(dpy, surf) : EGL_FALSE;
}

void onToggle(std::string_view id, bool enabled) {
  if (id == kModule) lsx::settings().enabled.store(enabled);
}

void onConfig(std::string_view id, std::string_view key, std::string_view value) {
  if (id != kModule) return;
  const std::string v(value);
  if (key == "pack") lsx::settings().packIndex.store(std::atoi(v.c_str()));
  else if (key == "scale") lsx::settings().renderScale.store(std::strtof(v.c_str(), nullptr));
}

class LeviShadersMod {
 public:
  bool load(pl::mod::ModContext& ctx) {
    lsx::setPackRoot(ctx.resourceDir() / "shaderpacks");
    lsx::refreshPackList();

    std::string options = "Off";
    for (const auto& n : lsx::packNames()) options += "," + n;

    const bool ok =
        pl::modmenu::ModuleBuilder(kModule, "Shader Packs")
            .description("Applies Java shader pack composite/final passes as a screen post-process. "
                         "Put unzipped packs in the mod's resources/shaderpacks folder.")
            .defaultEnabled(false)
            .onToggle(onToggle)
            .config("pack", "Pack", pl::modmenu::ConfigType::Radio, "0", options)
            .config("scale", "Render scale", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.5", "1.0")
            .onConfigChanged(onConfig)
            .registerModule();
    __android_log_print(ANDROID_LOG_INFO, kTag, "load: %zu packs, menu=%d",
                        lsx::packNames().size(), ok);
    return ok;
  }

  bool enable(pl::mod::ModContext&) {
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    void* sym = egl ? dlsym(egl, "eglSwapBuffers") : nullptr;
    if (!sym) {
      __android_log_print(ANDROID_LOG_ERROR, kTag, "eglSwapBuffers not found");
      return false;
    }
    g_swapHook = pl::memory::HookHandle(sym, reinterpret_cast<void*>(&swapDetour),
                                        reinterpret_cast<void**>(&g_origSwap),
                                        pl::memory::HookPriority::Normal);
    __android_log_print(ANDROID_LOG_INFO, kTag, "swap hook installed=%d", g_swapHook.installed());
    return g_swapHook.installed();
  }

  bool disable(pl::mod::ModContext&) {
    g_swapHook.reset();
    lsx::shutdown();
    pl::modmenu::unregisterModule(kModule);
    return true;
  }

  bool unload(pl::mod::ModContext&) { return true; }
};

LeviShadersMod g_mod;

}  // namespace

PL_REGISTER_MOD(LeviShadersMod, g_mod)
