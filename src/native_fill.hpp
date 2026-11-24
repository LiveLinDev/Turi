#pragma once
#include "veloz.hpp"
#include <unordered_set>

namespace veloz::native {

inline bool isName(const Expr& e, const std::string& name) {
    return e.kind == ExprKind::Name && e.text == name;
}

inline bool sameExpression(const Expr& left, const Expr& right) {
    if (left.kind != right.kind || left.text != right.text || left.args.size() != right.args.size())
        return false;
    for (std::size_t i = 0; i < left.args.size(); ++i)
        if (!sameExpression(left.args[i], right.args[i])) return false;
    return true;
}

inline bool mentions(const Expr& expression, const std::string& name) {
    if (expression.kind == ExprKind::Name && expression.text == name) return true;
    for (const auto& arg : expression.args)
        if (mentions(arg, name)) return true;
    return false;
}

inline bool stableSize(const Expr& expression, const std::unordered_set<std::string>& reassigned) {
    if (expression.kind == ExprKind::Integer) return true;
    if (expression.kind == ExprKind::Name) return !reassigned.contains(expression.text);
    if (expression.kind == ExprKind::Unary)
        return expression.text == "-" && stableSize(expression.args.at(0), reassigned);
    if (expression.kind == ExprKind::Binary) {
        if (expression.text != "+" && expression.text != "-" && expression.text != "*" &&
            expression.text != "/" && expression.text != "%") return false;
        return stableSize(expression.args.at(0), reassigned) &&
               stableSize(expression.args.at(1), reassigned);
    }
    return expression.kind == ExprKind::Call && expression.text == "len" &&
           expression.args.at(0).kind == ExprKind::Name &&
           !reassigned.contains(expression.args.at(0).text);
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

inline std::optional<std::vector<const Expr*>> firstFullWrite(
    const Stmt& decl, const Stmt& loop, const std::unordered_set<std::string>& reassigned) {
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
    const bool sameSize = sameExpression(size, end) && stableSize(size, reassigned) &&
                          !mentions(size, decl.name);
    const bool measuredSize = end.kind == ExprKind::Call && end.text == "len" &&
                              isName(end.args.at(0), decl.name);
    if (!sameSize && !measuredSize) return {};
    std::vector<const Expr*> reads;
    if (!initialReads(write.exprs.at(1), decl.name, loop.name, reads)) return {};
    return reads;
}

} // namespace veloz::native
