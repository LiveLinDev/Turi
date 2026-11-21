#pragma once
#include "veloz.hpp"

namespace veloz::native {

inline bool isName(const Expr& e, const std::string& name) {
    return e.kind == ExprKind::Name && e.text == name;
}

// The loop writes exactly once to every slot before exposing the array. A read
// of a[i] on that iteration still sees the value computed by repeat. A bare
// array use (including any argument to another function) invalidates the proof.
inline bool initialReads(const Expr& e, const std::string& array, const std::string& index,
                         std::vector<const Expr*>& reads) {
    if (isName(e, array)) return false;
    if (e.kind == ExprKind::Index && isName(e.args.at(0), array)) {
        if (!isName(e.args.at(1), index)) return false;
        reads.push_back(&e);
        return true;
    }
    for (const auto& child : e.args)
        if (!initialReads(child, array, index, reads)) return false;
    return true;
}

inline std::optional<std::vector<const Expr*>> firstFullWrite(const Stmt& decl, const Stmt& loop) {
    if (decl.kind != StmtKind::Var || loop.kind != StmtKind::For || loop.body.size() != 1) return {};
    const auto& init = decl.exprs.at(0);
    if (init.kind != ExprKind::Call || init.text != "repeat") return {};
    const auto& write = loop.body[0];
    if (write.kind != StmtKind::IndexSet || write.name != decl.name ||
        !isName(write.exprs.at(0), loop.name)) return {};
    const auto& start = loop.exprs.at(0);
    if (start.kind != ExprKind::Integer || start.text != "0") return {};
    const auto& size = init.args.at(1);
    const auto& end = loop.exprs.at(1);
    const bool sameSize = end.kind == size.kind && end.text == size.text &&
                          (size.kind == ExprKind::Name || size.kind == ExprKind::Integer);
    const bool measuredSize = end.kind == ExprKind::Call && end.text == "len" &&
                              isName(end.args.at(0), decl.name);
    if (!sameSize && !measuredSize) return {};
    std::vector<const Expr*> reads;
    if (!initialReads(write.exprs.at(1), decl.name, loop.name, reads)) return {};
    return reads;
}

} // namespace veloz::native
