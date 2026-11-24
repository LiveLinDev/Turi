#pragma once

#include "veloz.hpp"
#include <algorithm>
#include <charconv>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>

namespace veloz::native {

struct IntRange {
    std::int64_t lower, upper;
};

class ReturnRanges {
public:
    using ArrayRange = std::function<std::optional<IntRange>(const Expr&)>;
    using NameRange = std::function<std::optional<IntRange>(const Expr&)>;

    explicit ReturnRanges(const Program& program) {
        std::vector<const Function*> functions;
        for (const auto& function : program.functions)
            if (!function.isTest && function.result == Type::Int) functions.push_back(&function);
        bool changed;
        do {
            changed = false;
            for (const auto* function : functions) {
                auto next = functionRange(*function);
                if (!next) continue;
                auto found = ranges.find(function->name);
                if (found == ranges.end()) {
                    ranges.emplace(function->name, *next);
                    changed = true;
                    continue;
                }
                auto merged = envelope(found->second, *next);
                if (merged.lower != found->second.lower || merged.upper != found->second.upper) {
                    found->second = merged;
                    changed = true;
                }
            }
        } while (changed);
    }

    std::optional<IntRange> expression(const Expr& expression) const {
        return expressionWithArrays(expression, [](const Expr&) -> std::optional<IntRange> { return {}; });
    }

    std::optional<IntRange> expressionWithArrays(const Expr& expression,
                                                  const ArrayRange& arrayRange,
                                                  const NameRange& nameRange = {}) const {
        switch (expression.kind) {
        case ExprKind::Integer: {
            std::int64_t value{};
            const auto parsed = std::from_chars(expression.text.data(),
                expression.text.data() + expression.text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != expression.text.data() + expression.text.size()) return {};
            return IntRange{value, value};
        }
        case ExprKind::Name:
            if (nameRange) return nameRange(expression);
            return {};
        case ExprKind::Call:
            if (expression.text == "len" && expression.args.at(0).type == Type::IntArray)
                return IntRange{0, maxArrayLength};
            if (auto found = ranges.find(expression.text); found != ranges.end()) return found->second;
            return {};
        case ExprKind::Index: return arrayRange(expression);
        case ExprKind::Unary: {
            if (expression.text != "-") return {};
            auto child = expressionWithArrays(expression.args.at(0), arrayRange, nameRange);
            if (!child) return {};
            auto lower = negate(child->upper);
            auto upper = negate(child->lower);
            if (!lower || !upper) return {};
            return IntRange{*lower, *upper};
        }
        case ExprKind::Binary: {
            if (expression.text == "%") {
                auto left = expressionWithArrays(expression.args.at(0), arrayRange, nameRange);
                if (auto divisor = constant(expression.args.at(1))) {
                    if (*divisor == 0) return {};
                    constexpr auto min = std::numeric_limits<std::int64_t>::min();
                    constexpr auto max = std::numeric_limits<std::int64_t>::max();
                    if (*divisor == min) {
                        if (left && left->lower >= 0) return IntRange{0, max};
                        return IntRange{min + 1, max};
                    }
                    const auto magnitude = *divisor < 0 ? -*divisor : *divisor;
                    if (left && left->lower >= 0)
                        return IntRange{0, std::min(left->upper, magnitude - 1)};
                    return IntRange{-(magnitude - 1), magnitude - 1};
                }
                auto right = expressionWithArrays(expression.args.at(1), arrayRange, nameRange);
                if (right) return remainderRanges(left, *right);
                return {};
            }
            if (expression.text == "/") {
                auto left = expressionWithArrays(expression.args.at(0), arrayRange, nameRange);
                auto right = expressionWithArrays(expression.args.at(1), arrayRange, nameRange);
                if (left && right) return divideRanges(*left, *right);
                return {};
            }
            if (expression.text != "+" && expression.text != "-" && expression.text != "*") return {};
            auto left = expressionWithArrays(expression.args.at(0), arrayRange, nameRange);
            auto right = expressionWithArrays(expression.args.at(1), arrayRange, nameRange);
            if (!left || !right) return {};
            if (expression.text == "+") return add(*left, *right);
            if (expression.text == "-") return subtract(*left, *right);
            return multiply(*left, *right);
        }
        default: return {};
        }
    }

    static IntRange envelope(IntRange left, IntRange right) {
        return {std::min(left.lower, right.lower), std::max(left.upper, right.upper)};
    }
    static std::optional<IntRange> negateRange(IntRange value) {
        auto lo = negate(value.upper);
        auto hi = negate(value.lower);
        if (!lo || !hi) return {};
        return IntRange{*lo, *hi};
    }
    static std::optional<IntRange> addRanges(IntRange left, IntRange right) { return add(left, right); }
    static std::optional<IntRange> subtractRanges(IntRange left, IntRange right) {
        return subtract(left, right);
    }
    static std::optional<IntRange> multiplyRanges(IntRange left, IntRange right) {
        return multiply(left, right);
    }
    static std::optional<IntRange> divideRanges(IntRange left, IntRange right) {
        if (right.lower >= 1) {
            std::int64_t lo = std::numeric_limits<std::int64_t>::max();
            std::int64_t hi = std::numeric_limits<std::int64_t>::min();
            for (auto a : {left.lower, left.upper}) {
                for (auto b : {right.lower, right.upper}) {
                    auto q = a / b;
                    lo = std::min(lo, q);
                    hi = std::max(hi, q);
                }
            }
            return IntRange{lo, hi};
        }
        if (right.upper <= -1 && (left.lower > std::numeric_limits<std::int64_t>::min() || right.upper < -1)) {
            std::int64_t lo = std::numeric_limits<std::int64_t>::max();
            std::int64_t hi = std::numeric_limits<std::int64_t>::min();
            for (auto a : {left.lower, left.upper}) {
                for (auto b : {right.lower, right.upper}) {
                    auto q = a / b;
                    lo = std::min(lo, q);
                    hi = std::max(hi, q);
                }
            }
            return IntRange{lo, hi};
        }
        return {};
    }
    static std::optional<IntRange> remainderRanges(std::optional<IntRange> left, IntRange right) {
        if (right.lower >= 1) {
            const auto bound = right.upper - 1;
            if (left && left->lower >= 0) return IntRange{0, std::min(left->upper, bound)};
            return IntRange{-bound, bound};
        }
        if (right.upper <= -1 && right.lower > std::numeric_limits<std::int64_t>::min()) {
            const auto mag = std::max(-right.lower, -right.upper);
            if (left && left->lower >= 0) return IntRange{0, std::min(left->upper, mag - 1)};
            return IntRange{-(mag - 1), mag - 1};
        }
        return {};
    }

private:
    std::unordered_map<std::string, IntRange> ranges;

    static std::optional<std::int64_t> addInteger(std::int64_t left, std::int64_t right) {
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        if ((right > 0 && left > max - right) || (right < 0 && left < min - right)) return {};
        return left + right;
    }
    static std::optional<std::int64_t> subtractInteger(std::int64_t left, std::int64_t right) {
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        if ((right > 0 && left < min + right) || (right < 0 && left > max + right)) return {};
        return left - right;
    }
    static std::optional<std::int64_t> multiplyInteger(std::int64_t left, std::int64_t right) {
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        if (left > 0 && ((right > 0 && left > max / right) || (right < 0 && right < min / left))) return {};
        if (left < 0 && ((right > 0 && left < min / right) || (right < 0 && left < max / right))) return {};
        return left * right;
    }
    static std::optional<std::int64_t> negate(std::int64_t value) {
        if (value == std::numeric_limits<std::int64_t>::min()) return {};
        return -value;
    }
    static std::optional<IntRange> add(IntRange left, IntRange right) {
        auto lower = addInteger(left.lower, right.lower);
        auto upper = addInteger(left.upper, right.upper);
        if (!lower || !upper) return {};
        return IntRange{*lower, *upper};
    }
    static std::optional<IntRange> subtract(IntRange left, IntRange right) {
        auto lower = subtractInteger(left.lower, right.upper);
        auto upper = subtractInteger(left.upper, right.lower);
        if (!lower || !upper) return {};
        return IntRange{*lower, *upper};
    }
    static std::optional<IntRange> multiply(IntRange left, IntRange right) {
        std::optional<IntRange> result;
        for (const auto a : {left.lower, left.upper}) {
            for (const auto b : {right.lower, right.upper}) {
                auto value = multiplyInteger(a, b);
                if (!value) return {};
                result = result ? envelope(*result, IntRange{*value, *value}) : IntRange{*value, *value};
            }
        }
        return result;
    }
    static std::optional<std::int64_t> constant(const Expr& expression) {
        if (expression.kind == ExprKind::Integer) {
            std::int64_t value{};
            const auto parsed = std::from_chars(expression.text.data(),
                expression.text.data() + expression.text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != expression.text.data() + expression.text.size()) return {};
            return value;
        }
        if (expression.kind == ExprKind::Unary && expression.text == "-") {
            auto child = constant(expression.args.at(0));
            if (!child) return {};
            return negate(*child);
        }
        if (expression.kind != ExprKind::Binary) return {};
        auto left = constant(expression.args.at(0));
        auto right = constant(expression.args.at(1));
        if (!left || !right) return {};
        if (expression.text == "+") return addInteger(*left, *right);
        if (expression.text == "-") return subtractInteger(*left, *right);
        if (expression.text == "*") return multiplyInteger(*left, *right);
        if (expression.text != "/" && expression.text != "%") return {};
        if (*right == 0 || (*left == std::numeric_limits<std::int64_t>::min() && *right == -1)) return {};
        return expression.text == "/" ? *left / *right : *left % *right;
    }
    std::optional<IntRange> functionRange(const Function& function) const {
        std::optional<IntRange> result;
        bool any = false;
        auto visit = [&](const auto& self, const std::vector<Stmt>& statements) -> bool {
            for (const auto& statement : statements) {
                if (statement.kind == StmtKind::Return) {
                    any = true;
                    if (statement.exprs.size() != 1) return false;
                    auto current = expression(statement.exprs.at(0));
                    if (!current) return false;
                    result = result ? envelope(*result, *current) : *current;
                }
                if (!self(self, statement.body) || !self(self, statement.otherwise)) return false;
                for (const auto& arm : statement.arms)
                    if (!self(self, arm.body)) return false;
            }
            return true;
        };
        if (!visit(visit, function.body) || !any) return {};
        return result;
    }
};

} // namespace veloz::native
