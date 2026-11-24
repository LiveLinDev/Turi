#pragma once
#include "veloz.hpp"
#include <deque>
#include <functional>
#include <map>
#include <set>

namespace veloz {
class GenericInstances {
    Program& program;
    std::map<std::string, Function> templates;
    std::map<std::pair<std::string, std::vector<Type>>, std::size_t> cache;
    std::set<Type> opaque;
    std::set<std::size_t> abstractInstances;
    Function& instance(const Function& definition, const std::vector<Type>& concrete);
public:
    std::deque<Function> pending;
    // Separates templates, checks name collisions and queues an opaque validation copy of each.
    explicit GenericInstances(Program& program);
    bool contains(const std::string& name) const { return templates.contains(name); }
    const Function& templateOf(const std::string& name) const { return templates.at(name); }
    const Function& resolve(Expr& call, const std::vector<Type>& arguments,
                            const std::function<bool(Type, Type, Span)>& ensureImpl = {});
    void finish(); // append executable instances only after all pending bodies have been checked
};
}
