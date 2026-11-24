#pragma once
#include <string>
#include <vector>

namespace veloz {
int runMcpServer();
int runLspServer();
int runNewCommand(const std::vector<std::string>& args);
int runBenchCommand(const std::vector<std::string>& args);
}
