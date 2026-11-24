#pragma once
#include "commands.hpp"
#include <map>

namespace veloz {
using SourceOverrides = std::map<std::string, std::string>;
Program loadProject(const fs::path& entry, const SourceOverrides& overrides = {});
}
