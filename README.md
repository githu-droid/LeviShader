# LeviShaders

LeviLauncher native mod (`.so` / `.levipack`) that runs Java-shader-pack **post-process stages**
(`composite*.fsh`, `final.fsh`) on the official Minecraft Bedrock Android client.

## How it works
1. `PL_REGISTER_MOD` lifecycle + `pl::modmenu` module (toggle, pack radio, render-scale slider).
2. `pl::memory::hook` on `eglSwapBuffers` (resolved from `libEGL.so`; **no game offsets, so it is
   independent of the Minecraft version**. Verified against 1.26.52.03: `libminecraftpe.so` imports
   `eglSwapBuffers` + GLES2-style calls and renders through RenderDragon/bgfx on GLES).
3. Before each swap: blit the finished back buffer into `colortex0`, run each pass (ping-pong
   `colortexN`), draw the last pass straight to the screen. All GL state is saved and restored.
4. `ShaderPack.cpp` inlines `#include`, strips `#version`/`#extension`, and rewrites GLSL 1.20/3.30
   compat constructs to GLSL ES 3.00 (`varying`, `texture2D`, `gl_FragData`, `gl_TexCoord[0]`,
   `gl_Vertex`, `ftransform()` ...).

## Build
Push to GitHub: the workflow in `.github/workflows/build.yml` builds with NDK r27c and uploads
`libLeviShaders.so` and `LeviShaders.levipack`. Locally:
```
git clone https://github.com/LiteLDev/preloader-android
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared
cmake --build build
```
Install the `.levipack` through LeviLauncher's mod manager. Unzipped packs go in
`<mod folder>/resources/shaderpacks/<PackName>/shaders/...` (see `examples/Demo`).

## Known limits
- Post-process only: no terrain/entity/shadow geometry passes (`gbuffers_*`, `shadow`).
- No depth/normal G-buffer: `depthtex*` and `shadowtex*` are a 1x1 white texture, matrices are identity.
  Packs that need depth (SSAO, fog, DoF, volumetrics) will look wrong.
- Applies to the HUD/UI too, since it runs on the final frame.
- GLSL ES is stricter than GLSL 1.20 (no implicit int->float, e.g. `float x = 1;`). Packs with such
  code fail to compile; the error is logged (`adb logcat -s LeviShaders`) and the pack is skipped.
- `shaders.properties` and pack options screens are ignored. Folders only, no `.zip`.
- Not tested on a device (no NDK/phone in the build sandbox).
