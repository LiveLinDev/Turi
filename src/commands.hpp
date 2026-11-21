#pragma once
#include "veloz.hpp"
#include <filesystem>

namespace veloz {
namespace fs = std::filesystem;
std::string readFile(const fs::path& file);
void replaceFile(const fs::path& file, std::string_view content);
struct BuildOptions { bool release = false; std::string cpu; };
void buildBinary(const Program& program, const fs::path& destination, const BuildOptions& options);
fs::path pathFrom(std::string_view utf8);
std::string pathText(const fs::path& path);
std::string jsonString(std::string_view input);
std::string formatSource(std::string_view source);
std::string agentCommand(const std::vector<std::string>& args, const fs::path& file,
                         const std::string& source, Program& program);
std::string explain(std::string_view topic);
}
