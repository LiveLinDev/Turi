#pragma once
#include "veloz.hpp"
#include <filesystem>

namespace veloz {
namespace fs = std::filesystem;
std::string readFile(const fs::path& file);
void replaceFile(const fs::path& file, std::string_view content);
struct BuildOptions {
    bool release = false;
    bool web = false;
    std::string cpu;
    bool hasPgoGen = false;
    std::string pgoGen;
    std::string pgoUse;
    bool extreme = false;
    int extremeBudget = 12;
    std::string extremeCache;
    std::string extremeTrain;
    std::string extremeProposer;
    bool gpu = false;
    std::string sourcePath;
};
void buildBinary(const Program& program, const fs::path& destination, const BuildOptions& options);
fs::path pathFrom(std::string_view utf8);
std::string pathText(const fs::path& path);
std::string jsonString(std::string_view input);
std::string formatSource(std::string_view source);
std::string agentCommand(const std::vector<std::string>& args, const fs::path& file,
                         const std::string& source, Program& program);
void checkMemoryLevels(const Program& checked, int maxLevel, std::string_view filter = {});
void checkEffects(const Program& checked);
std::string explain(std::string_view topic);
}
