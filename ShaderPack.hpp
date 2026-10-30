#pragma once
// Loads a Java-Edition (OptiFine/Iris style) shader pack folder and translates
// its composite*/final GLSL (1.20 / 3.30 compat) into GLSL ES 3.00.
#include <filesystem>
#include <string>
#include <vector>

namespace lsx {

struct StageSource {
  std::string text;          // translated GLSL ES 3.00 (empty = missing)
  std::vector<int> targets;  // colortex indices written (fragment only)
  int outputCount = 1;       // number of fragment outputs declared
};

struct PassSource {
  std::string name;  // "composite", "composite1", "final"...
  StageSource vsh;
  StageSource fsh;
  bool isFinal = false;
};

struct Pack {
  std::string name;
  std::vector<PassSource> passes;  // composite..., then final (if present)
};

// Lists sub-folders of `root` that contain a `shaders/` directory.
std::vector<std::string> listPacks(const std::filesystem::path& root);

// Loads + translates composite.fsh ... composite15.fsh and final.fsh.
bool loadPack(const std::filesystem::path& packDir, Pack& out, std::string& err);

}  // namespace lsx
