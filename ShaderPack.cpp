#include "ShaderPack.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lsx {
namespace {

constexpr size_t kMaxFile = 2u << 20;      // 2 MB per file
constexpr size_t kMaxTotal = 6u << 20;     // 6 MB per expanded stage
constexpr int kMaxIncludeDepth = 16;

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

void replaceWord(std::string& s, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    const bool l = pos == 0 || !isIdent(s[pos - 1]);
    const size_t e = pos + from.size();
    const bool r = e >= s.size() || !isIdent(s[e]);
    if (l && r) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    } else {
      pos = e;
    }
  }
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

bool readFile(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return out.size() <= kMaxFile;
}

bool insideRoot(const fs::path& root, const fs::path& p) {
  std::error_code ec;
  auto r = fs::weakly_canonical(root, ec);
  auto c = fs::weakly_canonical(p, ec);
  const std::string rs = r.generic_string();
  const std::string cs = c.generic_string();
  return cs.size() >= rs.size() && cs.compare(0, rs.size(), rs) == 0;
}

// Recursively inlines #include, drops #version/#extension.
bool expand(const fs::path& root, const fs::path& file, int depth, std::string& out,
            std::string& err) {
  if (depth > kMaxIncludeDepth) { err = "include depth exceeded"; return false; }
  if (!insideRoot(root, file)) { err = "include escapes shaders/: " + file.string(); return false; }
  std::string text;
  if (!readFile(file, text)) { err = "cannot read " + file.string(); return false; }

  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (line.compare(i, 8, "#include") == 0) {
      const size_t a = line.find('"', i);
      const size_t b = a == std::string::npos ? a : line.find('"', a + 1);
      if (b == std::string::npos) { err = "bad #include in " + file.string(); return false; }
      std::string rel = line.substr(a + 1, b - a - 1);
      fs::path target = (!rel.empty() && rel[0] == '/') ? root / rel.substr(1)
                                                       : file.parent_path() / rel;
      if (!expand(root, target.lexically_normal(), depth + 1, out, err)) return false;
    } else if (line.compare(i, 8, "#version") == 0 || line.compare(i, 10, "#extension") == 0) {
      out += '\n';
    } else {
      out += line;
      out += '\n';
    }
    if (out.size() > kMaxTotal) { err = "shader too large"; return false; }
  }
  return true;
}

// "/* RENDERTARGETS: 0,1 */" or "/* DRAWBUFFERS:01 */"
std::vector<int> parseTargets(const std::string& s) {
  std::vector<int> t;
  size_t p = s.find("RENDERTARGETS:");
  if (p != std::string::npos) {
    p += 14;
    while (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == ',' || s[p] == ' ')) {
      if (std::isdigit(static_cast<unsigned char>(s[p]))) {
        int v = 0;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) v = v * 10 + (s[p++] - '0');
        t.push_back(v);
      } else {
        ++p;
      }
    }
    return t;
  }
  p = s.find("DRAWBUFFERS:");
  if (p != std::string::npos) {
    p += 12;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) t.push_back(s[p++] - '0');
  }
  return t;
}

const char* kHeaderCommon =
    "#version 300 es\n"
    "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
    "#define MC_VERSION 12100\n#define MC_GL_VERSION 320\n#define MC_GLSL_VERSION 300\n"
    "#define LEVISHADERS 1\n";

StageSource translateFragment(std::string src) {
  StageSource r;
  r.targets = parseTargets(src);

  int maxFragData = -1;
  for (int i = 0; i < 8; ++i) {
    const std::string k = "gl_FragData[" + std::to_string(i) + "]";
    if (contains(src, k.c_str())) { maxFragData = i; replaceAll(src, k, "_out" + std::to_string(i)); }
  }
  replaceWord(src, "gl_FragColor", "_out0");

  const bool userOut = contains(src, "out vec4") || contains(src, "out  vec4");
  const int outCount = std::max<int>({1, maxFragData + 1, static_cast<int>(r.targets.size())});
  if (r.targets.empty()) for (int i = 0; i < outCount; ++i) r.targets.push_back(i);
  r.outputCount = outCount;

  const bool usesTc0 = contains(src, "gl_TexCoord[0]");
  replaceAll(src, "gl_TexCoord[0]", "_glTexCoord0");
  const bool usesColor = contains(src, "gl_Color");
  replaceWord(src, "gl_Color", "_glFrontColor");

  replaceWord(src, "varying", "in");
  replaceWord(src, "texture2D", "texture");
  replaceWord(src, "texture2DLod", "textureLod");
  replaceWord(src, "texture2DGrad", "textureGrad");
  replaceWord(src, "texture2DProj", "textureProj");
  replaceWord(src, "texture3D", "texture");
  replaceWord(src, "shadow2D", "texture");

  std::string head = kHeaderCommon;
  if (usesTc0) head += "in vec4 _glTexCoord0;\n";
  if (usesColor) head += "in vec4 _glFrontColor;\n";
  if (!userOut) {
    for (int i = 0; i < outCount; ++i)
      head += "layout(location=" + std::to_string(i) + ") out vec4 _out" + std::to_string(i) + ";\n";
  }
  r.text = head + src;
  return r;
}

StageSource translateVertex(std::string src) {
  StageSource r;
  const bool usesTc0 = contains(src, "gl_TexCoord[0]");
  replaceAll(src, "gl_TexCoord[0]", "_glTexCoord0");
  replaceAll(src, "gl_TextureMatrix[0]", "_I4");
  replaceAll(src, "gl_TextureMatrix[1]", "_I4");
  const bool usesFront = contains(src, "gl_FrontColor");
  replaceWord(src, "gl_FrontColor", "_glFrontColor");
  replaceWord(src, "gl_Color", "vec4(1.0)");
  replaceWord(src, "gl_Normal", "vec3(0.0,0.0,1.0)");
  replaceWord(src, "gl_ModelViewProjectionMatrix", "_I4");
  replaceWord(src, "gl_ModelViewMatrix", "_I4");
  replaceWord(src, "gl_ProjectionMatrix", "_I4");
  replaceWord(src, "gl_ModelViewMatrixInverse", "_I4");
  replaceWord(src, "gl_ProjectionMatrixInverse", "_I4");
  replaceWord(src, "gl_NormalMatrix", "_I3");
  replaceAll(src, "ftransform()", "(_I4 * _glVertex)");
  replaceWord(src, "gl_Vertex", "_glVertex");
  replaceWord(src, "gl_MultiTexCoord0", "_glUV");
  replaceWord(src, "gl_MultiTexCoord1", "vec4(0.0,0.0,0.0,1.0)");
  replaceWord(src, "gl_MultiTexCoord2", "vec4(0.0,0.0,0.0,1.0)");
  replaceWord(src, "attribute", "in");
  replaceWord(src, "varying", "out");
  replaceWord(src, "texture2D", "texture");
  replaceWord(src, "texture2DLod", "textureLod");

  std::string head = kHeaderCommon;
  head += "layout(location=0) in vec4 _glVertex;\nlayout(location=1) in vec4 _glUV;\n"
          "const mat4 _I4 = mat4(1.0);\nconst mat3 _I3 = mat3(1.0);\n";
  if (usesTc0) head += "out vec4 _glTexCoord0;\n";
  if (usesFront) head += "out vec4 _glFrontColor;\n";
  r.text = head + src;
  return r;
}

// For packs without a .vsh: emit one that feeds every float/vec `in` of the fsh with uv.
std::string makeDefaultVertex(const std::string& translatedFsh) {
  std::string body;
  std::string decls;
  std::istringstream in(translatedFsh);
  std::string line;
  while (std::getline(in, line)) {
    if (line.compare(0, 3, "in ") != 0) continue;
    std::istringstream ls(line);
    std::string in_kw, type, name;
    ls >> in_kw >> type >> name;
    if (!name.empty() && name.back() == ';') name.pop_back();
    if (name.empty()) continue;
    if (type == "vec2") body += "  " + name + " = _glUV.xy;\n";
    else if (type == "vec3") body += "  " + name + " = vec3(_glUV.xy, 0.0);\n";
    else if (type == "vec4") body += "  " + name + " = vec4(_glUV.xy, 0.0, 1.0);\n";
    else if (type == "float") body += "  " + name + " = 0.0;\n";
    else continue;
    decls += "out " + type + " " + name + ";\n";
  }
  std::string v = kHeaderCommon;
  v += "layout(location=0) in vec4 _glVertex;\nlayout(location=1) in vec4 _glUV;\n" + decls +
       "void main() {\n" + body + "  gl_Position = vec4(_glVertex.xy, 0.0, 1.0);\n}\n";
  return v;
}

// returns 1 = built, 0 = file absent, -1 = error
int buildPass(const fs::path& shadersDir, const std::string& name, bool isFinal, PassSource& out,
              std::string& err) {
  const fs::path fshPath = shadersDir / (name + ".fsh");
  if (!fs::exists(fshPath)) return 0;
  std::string fsrc;
  if (!expand(shadersDir, fshPath, 0, fsrc, err)) { err = name + ".fsh: " + err; return -1; }
  out.name = name;
  out.isFinal = isFinal;
  out.fsh = translateFragment(std::move(fsrc));

  const fs::path vshPath = shadersDir / (name + ".vsh");
  if (fs::exists(vshPath)) {
    std::string vsrc;
    if (!expand(shadersDir, vshPath, 0, vsrc, err)) { err = name + ".vsh: " + err; return -1; }
    out.vsh = translateVertex(std::move(vsrc));
  } else {
    out.vsh.text = makeDefaultVertex(out.fsh.text);
  }
  return 1;
}

}  // namespace

std::vector<std::string> listPacks(const fs::path& root) {
  std::vector<std::string> names;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return names;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    if (e.is_directory() && fs::is_directory(e.path() / "shaders", ec)) {
      std::string n = e.path().filename().string();
      std::replace(n.begin(), n.end(), ',', ' ');  // ',' separates mod-menu radio options
      names.push_back(std::move(n));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool loadPack(const fs::path& packDir, Pack& out, std::string& err) {
  out = Pack{};
  out.name = packDir.filename().string();
  const fs::path shaders = packDir / "shaders";
  if (!fs::is_directory(shaders)) { err = "no shaders/ folder"; return false; }

  // "composite", "composite1" .. "composite15" (there is no "composite0")
  for (int i = 0; i < 16; ++i) {
    const std::string name = i == 0 ? "composite" : "composite" + std::to_string(i);
    PassSource p;
    const int rc = buildPass(shaders, name, false, p, err);
    if (rc < 0) return false;
    if (rc > 0) out.passes.push_back(std::move(p));
  }
  PassSource fin;
  const int rc = buildPass(shaders, "final", true, fin, err);
  if (rc < 0) return false;
  if (rc > 0) out.passes.push_back(std::move(fin));

  if (out.passes.empty()) {
    err = "pack has no composite*.fsh or final.fsh (only post-process stages are supported)";
    return false;
  }
  return true;
}

}  // namespace lsx
