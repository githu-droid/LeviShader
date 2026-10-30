#include "PostFx.hpp"

#include <GLES3/gl3.h>
#include <android/log.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

#include "ShaderPack.hpp"

#define TAG "LeviShaders"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace fs = std::filesystem;

namespace lsx {
namespace {

Settings g_settings;
fs::path g_root;
std::vector<std::string> g_names;
std::mutex g_namesMutex;

// ---------------------------------------------------------------------------------------------
// GL state save/restore. bgfx caches GL state and assumes nobody else touches it, so we must put
// back everything we change.
// ---------------------------------------------------------------------------------------------
struct GlStateGuard {
  GLint program, vao, arrayBuf, drawFbo, readFbo, activeTex;
  GLint viewport[4], scissor[4];
  GLboolean colorMask[4], depthMask;
  GLboolean blend, depthTest, cull, scissorTest, stencilTest, polyOffset, dither, a2c, rasterDiscard;
  GLint tex[16], sampler[16];

  GlStateGuard() {
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    blend = glIsEnabled(GL_BLEND);
    depthTest = glIsEnabled(GL_DEPTH_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    stencilTest = glIsEnabled(GL_STENCIL_TEST);
    polyOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    dither = glIsEnabled(GL_DITHER);
    a2c = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
    rasterDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
    for (int u = 0; u < 16; ++u) {
      glActiveTexture(GL_TEXTURE0 + u);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex[u]);
      glGetIntegerv(GL_SAMPLER_BINDING, &sampler[u]);
    }
  }

  ~GlStateGuard() {
    for (int u = 0; u < 16; ++u) {
      glActiveTexture(GL_TEXTURE0 + u);
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex[u]));
      glBindSampler(u, static_cast<GLuint>(sampler[u]));
    }
    glActiveTexture(static_cast<GLenum>(activeTex));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
    glBindVertexArray(static_cast<GLuint>(vao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuf));
    glUseProgram(static_cast<GLuint>(program));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glDepthMask(depthMask);
    auto set = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
    set(GL_BLEND, blend);
    set(GL_DEPTH_TEST, depthTest);
    set(GL_CULL_FACE, cull);
    set(GL_SCISSOR_TEST, scissorTest);
    set(GL_STENCIL_TEST, stencilTest);
    set(GL_POLYGON_OFFSET_FILL, polyOffset);
    set(GL_DITHER, dither);
    set(GL_SAMPLE_ALPHA_TO_COVERAGE, a2c);
    set(GL_RASTERIZER_DISCARD, rasterDiscard);
    while (glGetError() != GL_NO_ERROR) {}  // don't leak our errors into the game's checks
  }
};

// ---------------------------------------------------------------------------------------------
enum class TexKind { ColorTex, White, Noise, Black };

struct UniformInfo {
  GLint loc = -1;
  GLenum type = 0;
  std::string name;
  GLint unit = -1;          // samplers only
  TexKind kind = TexKind::Black;
  int index = 0;            // colortex index
};

struct Program {
  GLuint id = 0;
  bool isFinal = false;
  std::vector<int> targets;
  std::vector<UniformInfo> uniforms;
};

bool mapSampler(const std::string& n, TexKind& kind, int& idx) {
  auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
  if (starts("colortex") && n.size() > 8 && std::isdigit(static_cast<unsigned char>(n[8]))) {
    kind = TexKind::ColorTex; idx = n[8] - '0'; return true;
  }
  static const struct { const char* name; int idx; } legacy[] = {
      {"gcolor", 0}, {"gdepth", 1}, {"gnormal", 2}, {"composite", 3},
      {"gaux1", 4}, {"gaux2", 5}, {"gaux3", 6}, {"gaux4", 7}};
  for (auto& l : legacy) if (n == l.name) { kind = TexKind::ColorTex; idx = l.idx; return true; }
  if (starts("depthtex") || n == "gdepthtex" || starts("shadowtex") || n == "shadow" ||
      n == "watershadow") { kind = TexKind::White; return true; }
  if (n == "noisetex") { kind = TexKind::Noise; return true; }
  kind = TexKind::Black;
  return false;
}

class Renderer {
 public:
  void frame(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;
    if (ctx != ctx_) { dropGlHandles(); ctx_ = ctx; }  // context lost/recreated (app resume)

    const int want = g_settings.packIndex.load();
    if (!g_settings.enabled.load() || want <= 0) return;

    EGLint W = 0, H = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &W);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &H);
    if (W <= 0 || H <= 0) return;

    if (want != loadedIndex_) {
      dropPrograms();
      loadedIndex_ = want;
      broken_ = false;
    }
    if (broken_) return;

    GlStateGuard guard;
    if (!ready_ && !initGl()) { broken_ = true; return; }
    if (programs_.empty() && !buildPack(want)) { broken_ = true; return; }

    const float scale = std::max(0.25f, std::min(1.0f, g_settings.renderScale.load()));
    const int w = std::max(1, static_cast<int>(W * scale));
    const int h = std::max(1, static_cast<int>(H * scale));
    if (!ensureTargets(w, h)) { broken_ = true; return; }

    execute(W, H, w, h);
  }

  void reset() { dropPrograms(); loadedIndex_ = 0; }

 private:
  // --- one-time GL setup ---------------------------------------------------------------------
  bool initGl() {
    GLint samples = 0;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glGetIntegerv(GL_SAMPLES, &samples);
    if (samples > 0) {
      LOGE("default framebuffer is multisampled (%d); blit-based capture unsupported", samples);
      return false;
    }
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    floatTargets_ = ext && (std::strstr(ext, "GL_EXT_color_buffer_float") ||
                            std::strstr(ext, "GL_EXT_color_buffer_half_float"));
    GLint maxAtt = 4;
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxAtt);
    maxTargets_ = std::max(1, std::min(8, maxAtt));

    // fullscreen triangle: x y u v
    static const float verts[] = {-1.f, -1.f, 0.f, 0.f, 3.f, -1.f, 2.f, 0.f, -1.f, 3.f, 0.f, 2.f};
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(8));
    glGenFramebuffers(1, &fbo_);

    // 1x1 white (depth/shadow stand-in), 1x1 black, 256x256 noise
    const unsigned char white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255};
    white_ = make1x1(white);
    black_ = make1x1(black);
    std::vector<unsigned char> noise(256 * 256 * 4);
    uint32_t s = 0x9E3779B9u;
    for (auto& b : noise) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b = static_cast<unsigned char>(s); }
    glGenTextures(1, &noise_);
    glBindTexture(GL_TEXTURE_2D, noise_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, noise.data());
    setParams(GL_NEAREST, GL_REPEAT);

    start_ = std::chrono::steady_clock::now();
    ready_ = true;
    LOGI("init ok: floatTargets=%d maxTargets=%d", floatTargets_, maxTargets_);
    return true;
  }

  GLuint make1x1(const unsigned char* px) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    setParams(GL_NEAREST, GL_CLAMP_TO_EDGE);
    return t;
  }

  static void setParams(GLint filter, GLint wrap) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
  }

  // --- pack -> GL programs -------------------------------------------------------------------
  static GLuint compile(GLenum type, const std::string& src, const std::string& label) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[2048] = {};
      glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
      LOGE("%s compile failed: %s", label.c_str(), log);
      glDeleteShader(s);
      return 0;
    }
    return s;
  }

  bool buildPack(int index) {
    std::string name;
    {
      std::lock_guard<std::mutex> lk(g_namesMutex);
      if (index < 1 || index > static_cast<int>(g_names.size())) return false;
      name = g_names[index - 1];
    }
    Pack pack;
    std::string err;
    // names had ',' replaced by ' '; try the literal folder first, then a comma variant
    fs::path dir = g_root / name;
    if (!fs::exists(dir)) { std::string alt = name; for (auto& c : alt) if (c == ' ') c = ','; dir = g_root / alt; }
    if (!loadPack(dir, pack, err)) { LOGE("pack '%s': %s", name.c_str(), err.c_str()); return false; }

    int maxTex = 0;
    for (const auto& pass : pack.passes) {
      Program prog;
      prog.isFinal = pass.isFinal;
      prog.targets = pass.fsh.targets;
      GLuint vs = compile(GL_VERTEX_SHADER, pass.vsh.text, pass.name + ".vsh");
      GLuint fs_ = compile(GL_FRAGMENT_SHADER, pass.fsh.text, pass.name + ".fsh");
      if (!vs || !fs_) { if (vs) glDeleteShader(vs); if (fs_) glDeleteShader(fs_); dropPrograms(); return false; }
      prog.id = glCreateProgram();
      glAttachShader(prog.id, vs);
      glAttachShader(prog.id, fs_);
      glBindAttribLocation(prog.id, 0, "_glVertex");
      glBindAttribLocation(prog.id, 1, "_glUV");
      glLinkProgram(prog.id);
      glDeleteShader(vs);
      glDeleteShader(fs_);
      GLint ok = 0;
      glGetProgramiv(prog.id, GL_LINK_STATUS, &ok);
      if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(prog.id, sizeof(log) - 1, nullptr, log);
        LOGE("%s link failed: %s", pass.name.c_str(), log);
        glDeleteProgram(prog.id);
        dropPrograms();
        return false;
      }
      GLint n = 0, unit = 0;
      glGetProgramiv(prog.id, GL_ACTIVE_UNIFORMS, &n);
      for (GLint i = 0; i < n; ++i) {
        char buf[128] = {};
        GLsizei len = 0; GLint size = 0; GLenum type = 0;
        glGetActiveUniform(prog.id, i, sizeof(buf) - 1, &len, &size, &type, buf);
        UniformInfo u;
        u.name = buf;
        if (auto b = u.name.find('['); b != std::string::npos) u.name.resize(b);
        u.loc = glGetUniformLocation(prog.id, buf);
        u.type = type;
        if (u.loc < 0) continue;
        if (type == GL_SAMPLER_2D) {
          mapSampler(u.name, u.kind, u.index);
          u.unit = unit++;
          if (u.kind == TexKind::ColorTex) maxTex = std::max(maxTex, u.index);
        }
        prog.uniforms.push_back(std::move(u));
      }
      for (int t : prog.targets) maxTex = std::max(maxTex, t);
      programs_.push_back(std::move(prog));
    }
    numTex_ = std::min(maxTargets_, maxTex + 1);
    for (auto& p : programs_)
      for (auto& t : p.targets) if (t >= numTex_) t = 0;
    packName_ = name;
    frameCounter_ = 0;
    LOGI("pack '%s' ready: %zu passes, %d colortex", name.c_str(), programs_.size(), numTex_);
    return true;
  }

  // --- render targets ------------------------------------------------------------------------
  bool ensureTargets(int w, int h) {
    if (w == texW_ && h == texH_ && tex_[0][0]) return true;
    freeTargets();
    texW_ = w; texH_ = h;
    const GLenum internal = floatTargets_ ? GL_RGBA16F : GL_RGBA8;
    const GLenum type = floatTargets_ ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
    for (int i = 0; i < numTex_; ++i)
      for (int j = 0; j < 2; ++j) {
        glGenTextures(1, &tex_[i][j]);
        glBindTexture(GL_TEXTURE_2D, tex_[i][j]);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, type, nullptr);
        setParams(GL_LINEAR, GL_CLAMP_TO_EDGE);
        main_[i] = 0;
      }
    // verify renderable
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_[0][0], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      LOGE("colortex framebuffer incomplete (float=%d)", floatTargets_);
      if (floatTargets_) { floatTargets_ = false; freeTargets(); return ensureTargets(w, h); }
      return false;
    }
    return true;
  }

  void freeTargets() {
    for (auto& row : tex_) for (auto& t : row) { if (t) glDeleteTextures(1, &t); t = 0; }
    texW_ = texH_ = 0;
  }

  // --- the frame -----------------------------------------------------------------------------
  void execute(int W, int H, int w, int h) {
    for (int i = 0; i < numTex_; ++i) main_[i] = 0;

    // 1) capture the finished frame into colortex0
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_[0][0], 0);
    GLenum a0 = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &a0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, W, H, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // 2) common state for fullscreen passes
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST); glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); glDisable(GL_RASTERIZER_DISCARD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(vao_);

    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count();
    bool wroteFinal = false;

    for (auto& prog : programs_) {
      GLenum att[8];
      if (prog.isFinal) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glViewport(0, 0, W, H);
        wroteFinal = true;
      } else {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        int n = 0;
        for (int tgt : prog.targets) {
          if (n >= 8) break;
          glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + n, GL_TEXTURE_2D,
                                 tex_[tgt][main_[tgt] ^ 1], 0);
          att[n] = GL_COLOR_ATTACHMENT0 + n;
          ++n;
        }
        glDrawBuffers(n, att);
        glViewport(0, 0, w, h);
      }

      glUseProgram(prog.id);
      for (const auto& u : prog.uniforms) bindUniform(u, t, W, H, w, h);
      glDrawArrays(GL_TRIANGLES, 0, 3);

      if (!prog.isFinal)
        for (int tgt : prog.targets) main_[tgt] ^= 1;  // flip written targets
    }

    // pack without final.fsh: blit colortex0 to the screen
    if (!wroteFinal) {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             tex_[0][main_[0]], 0);
      glReadBuffer(GL_COLOR_ATTACHMENT0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      glBlitFramebuffer(0, 0, w, h, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    ++frameCounter_;
  }

  void bindUniform(const UniformInfo& u, float t, int W, int H, int w, int h) {
    switch (u.type) {
      case GL_SAMPLER_2D: {
        GLuint tex = black_;
        switch (u.kind) {
          case TexKind::ColorTex:
            tex = (u.index < numTex_) ? tex_[u.index][main_[u.index]] : black_;
            break;
          case TexKind::White: tex = white_; break;
          case TexKind::Noise: tex = noise_; break;
          case TexKind::Black: break;
        }
        glActiveTexture(GL_TEXTURE0 + u.unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(u.unit, 0);
        glUniform1i(u.loc, u.unit);
        break;
      }
      case GL_FLOAT: {
        const std::string& n = u.name;
        float v = 0.f;
        if (n == "viewWidth") v = static_cast<float>(w);
        else if (n == "viewHeight") v = static_cast<float>(h);
        else if (n == "aspectRatio") v = static_cast<float>(W) / static_cast<float>(H);
        else if (n == "frameTimeCounter") v = t;
        else if (n == "frameTime") v = 1.f / 60.f;
        else if (n == "near") v = 0.05f;
        else if (n == "far") v = 256.f;
        else if (n == "sunAngle") v = 0.1f;
        else if (n == "screenBrightness") v = 0.5f;
        else if (n == "centerDepthSmooth") v = 1.f;
        glUniform1f(u.loc, v);
        break;
      }
      case GL_INT: {
        if (u.name == "frameCounter") glUniform1i(u.loc, frameCounter_);
        else glUniform1i(u.loc, 0);
        break;
      }
      case GL_FLOAT_VEC2:
        if (u.name == "viewSize" || u.name == "resolution") glUniform2f(u.loc, float(w), float(h));
        break;
      case GL_FLOAT_VEC3:
        if (u.name == "upPosition" || u.name == "sunPosition" || u.name == "moonPosition" ||
            u.name == "shadowLightPosition")
          glUniform3f(u.loc, 0.f, 100.f, 0.f);
        break;
      case GL_FLOAT_MAT4: {
        static const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        glUniformMatrix4fv(u.loc, 1, GL_FALSE, I);
        break;
      }
      case GL_FLOAT_MAT3: {
        static const float I[9] = {1,0,0, 0,1,0, 0,0,1};
        glUniformMatrix3fv(u.loc, 1, GL_FALSE, I);
        break;
      }
      default: break;
    }
  }

  // --- teardown ------------------------------------------------------------------------------
  void dropPrograms() {
    if (ready_ && eglGetCurrentContext() == ctx_) {
      for (auto& p : programs_) if (p.id) glDeleteProgram(p.id);
    }
    programs_.clear();
    packName_.clear();
  }

  void dropGlHandles() {  // the context changed; old handles are invalid
    programs_.clear();
    for (auto& row : tex_) for (auto& t : row) t = 0;
    vao_ = vbo_ = fbo_ = white_ = black_ = noise_ = 0;
    texW_ = texH_ = 0;
    ready_ = false;
    broken_ = false;
    loadedIndex_ = 0;
  }

  EGLContext ctx_ = EGL_NO_CONTEXT;
  bool ready_ = false, broken_ = false, floatTargets_ = false;
  int maxTargets_ = 4, numTex_ = 1, loadedIndex_ = 0, frameCounter_ = 0;
  int texW_ = 0, texH_ = 0;
  GLuint vao_ = 0, vbo_ = 0, fbo_ = 0, white_ = 0, black_ = 0, noise_ = 0;
  GLuint tex_[8][2] = {};
  int main_[8] = {};
  std::vector<Program> programs_;
  std::string packName_;
  std::chrono::steady_clock::time_point start_;
};

Renderer& renderer() {
  static Renderer r;
  return r;
}

}  // namespace

Settings& settings() { return g_settings; }

void setPackRoot(const fs::path& root) {
  g_root = root;
  std::error_code ec;
  fs::create_directories(root, ec);
}

void refreshPackList() {
  auto names = listPacks(g_root);
  std::lock_guard<std::mutex> lk(g_namesMutex);
  g_names = std::move(names);
}

const std::vector<std::string>& packNames() { return g_names; }

void onSwap(EGLDisplay dpy, EGLSurface surf) { renderer().frame(dpy, surf); }

void shutdown() { renderer().reset(); }

}  // namespace lsx
