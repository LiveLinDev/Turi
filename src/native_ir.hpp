#pragma once

#include "veloz.hpp"
#include "native_fill.hpp"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace veloz::native {

struct Value {
    Type type; unsigned id;
    std::optional<unsigned> owner{}; // slot that can transfer its heap allocation
    bool freshRegion = false;
};
enum class Op {
    Int, Bool, Text, Load, Store, Call, Length, Negate, Not, Binary, Print, Assert,
    Construct, Field, Tag, RepeatHeap, RepeatRegion, Clone, Drop, ClearOwner,
    RegionMark, RegionRewind, Index, IndexSet, ReadByte
};
struct Operation {
    Op op;
    Value result;
    std::vector<Value> inputs;
    std::string data;
    unsigned slot;
    Span span;
    std::vector<unsigned> fields;
    bool initialize = true;
};
enum class Exit { Open, Jump, Branch, Switch, Return, Unreachable };
struct Block {
    std::vector<Operation> ops;
    Exit exit = Exit::Open;
    std::optional<Value> value;
    unsigned yes = 0, no = 0;
    std::vector<unsigned> targets; // switch tag -> block, yes is the fallback
};
struct FunctionIR {
    const Function& source;
    std::vector<Type> slots;
    std::vector<Block> blocks{1};
    bool usesRegion = false;
};

// Registers are immutable. Locals use typed frame slots allocated once per call.
// Branch targets, including short-circuit edges, are explicit before LLVM lowering.
class Lowerer {
    const Program& program;
    FunctionIR fn;
    struct Scope {
        std::unordered_map<std::string, unsigned> names;
        std::vector<unsigned> arrays;
        std::optional<Value> mark;
    };
    std::vector<Scope> scopes{1};
    std::vector<unsigned> temporaries;
    std::unordered_map<const Expr*, Value> forwardedReads;
    unsigned here = 0;
    unsigned nextValue = 0;

    unsigned block() {
        fn.blocks.emplace_back();
        return static_cast<unsigned>(fn.blocks.size() - 1);
    }
    unsigned allocate(Type type) {
        fn.slots.push_back(type);
        return static_cast<unsigned>(fn.slots.size() - 1);
    }
    unsigned declare(const std::string& name, Type type) {
        const auto slot = allocate(type);
        scopes.back().names.emplace(name, slot);
        return slot;
    }
    unsigned lookup(const std::string& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (auto found = it->names.find(name); found != it->names.end()) return found->second;
        }
        throw std::logic_error("unresolved local in checked program");
    }
    Value emit(Op op, Type type, std::vector<Value> inputs = {}, std::string data = {},
               unsigned slot = 0, Span span = {}) {
        const Value result{type, nextValue++};
        fn.blocks[here].ops.push_back({op, result, std::move(inputs), std::move(data), slot, span, {}});
        return result;
    }
    static unsigned memberIndex(const auto& members, const std::string& name) {
        const auto it = std::find_if(members.begin(), members.end(), [&](const auto& m) { return m.name == name; });
        if (it == members.end()) throw std::logic_error("unresolved member in checked program");
        return static_cast<unsigned>(it - members.begin());
    }
    static unsigned variantOf(const DataDecl& decl, const std::string& qualified) {
        return memberIndex(decl.variants, qualified.substr(qualified.find('.') + 1));
    }
    Value field(Value base, Type type, std::vector<unsigned> path) {
        auto result = emit(Op::Field, type, {base});
        fn.blocks[here].ops.back().fields = std::move(path);
        return result;
    }
    Value construct(const Expr& e) {
        const auto& decl = *program.declaration(e.type);
        const auto tag = decl.isEnum ? variantOf(decl, e.text) : 0;
        std::vector<Value> args;
        for (const auto& arg : e.args) args.push_back(expression(arg));
        auto result = emit(Op::Construct, e.type, std::move(args), {}, tag);
        fn.blocks[here].ops.back().fields.assign(e.fieldIndices.begin(), e.fieldIndices.end());
        return result;
    }
    Value load(unsigned slot) { return emit(Op::Load, fn.slots.at(slot), {}, {}, slot); }
    void store(unsigned slot, Value value) { emit(Op::Store, Type::Void, {value}, {}, slot); }
    void drop(unsigned slot) { emit(Op::Drop, Type::Void, {}, {}, slot); }
    Value track(Value value) {
        if (value.type == Type::IntArray && !value.freshRegion) {
            const auto slot = allocate(Type::IntArray);
            store(slot, value);
            temporaries.push_back(slot);
            value.owner = slot;
        }
        return value;
    }
    Value own(Value value, Span span) {
        if (value.type != Type::IntArray || value.freshRegion) return value;
        if (!value.owner) return emit(Op::Clone, Type::IntArray, {value}, {}, 0, span);
        emit(Op::ClearOwner, Type::Void, {}, {}, *value.owner);
        value.owner.reset();
        return value;
    }
    Value returned(const Expr& e) {
        if (e.kind != ExprKind::Name || e.type != Type::IntArray) return own(expression(e), e.span);
        const auto slot = lookup(e.text);
        auto value = load(slot);
        // Only heap-owned locals are on the drop list. A return is their last
        // use on this path. Parameters are borrowed and region storage expires.
        for (const auto& scope : scopes) {
            if (std::ranges::find(scope.arrays, slot) != scope.arrays.end()) {
                value.owner = slot;
                break;
            }
        }
        return own(value, e.span);
    }
    void dropTemporaries() {
        for (auto it = temporaries.rbegin(); it != temporaries.rend(); ++it) drop(*it);
        temporaries.clear();
    }
    void markRegion(const std::vector<Stmt>& statements) {
        for (const auto& s : statements) {
            if ((s.kind == StmtKind::Let || s.kind == StmtKind::Var) &&
                s.exprs.at(0).allocation == ArrayAllocation::Region) {
                fn.usesRegion = true;
                scopes.back().mark = emit(Op::RegionMark, Type::Void);
                return;
            }
        }
    }
    void release(const Scope& scope) {
        for (auto it = scope.arrays.rbegin(); it != scope.arrays.rend(); ++it) drop(*it);
        if (scope.mark) emit(Op::RegionRewind, Type::Void, {*scope.mark});
    }
    void cleanupReturn() {
        dropTemporaries();
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) release(*it);
    }
    void jump(unsigned target) {
        Block& current = fn.blocks[here];
        if (current.exit == Exit::Open) {
            current.exit = Exit::Jump;
            current.yes = target;
        }
    }
    void branch(Value test, unsigned yes, unsigned no) {
        auto& current = fn.blocks[here];
        current.exit = Exit::Branch;
        current.value = test;
        current.yes = yes;
        current.no = no;
    }
    Value lazy(const Expr& e) {
        const auto slot = allocate(Type::Bool);
        const auto first = expression(e.args[0]);
        store(slot, first);
        const auto rhs = block();
        const auto end = block();
        if (e.text == "&&") branch(first, rhs, end);
        else branch(first, end, rhs);
        here = rhs;
        store(slot, expression(e.args[1]));
        jump(end);
        here = end;
        return load(slot);
    }
    struct Repetition { Value array, element; };
    Repetition repeat(const Expr& e, bool initialize) {
        const auto element = expression(e.args.at(0));
        const auto size = expression(e.args.at(1));
        const bool region = e.allocation == ArrayAllocation::Region;
        auto array = emit(region ? Op::RepeatRegion : Op::RepeatHeap, e.type,
                          {element, size}, {}, 0, e.span);
        fn.blocks[here].ops.back().initialize = initialize;
        array.freshRegion = region;
        return {track(array), element};
    }
    Value expression(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Integer: return emit(Op::Int, e.type, {}, e.text);
        case ExprKind::Boolean: return emit(Op::Bool, e.type, {}, e.text);
        case ExprKind::String: return emit(Op::Text, e.type, {}, e.text);
        case ExprKind::Name: return load(lookup(e.text));
        case ExprKind::Index: {
            if (auto it = forwardedReads.find(&e); it != forwardedReads.end()) return it->second;
            const auto base = expression(e.args.at(0));
            const auto index = expression(e.args.at(1));
            return emit(Op::Index, Type::Int, {base, index}, {}, 0, e.span);
        }
        case ExprKind::Construct: return construct(e);
        case ExprKind::Field: {
            const auto base = expression(e.args.at(0));
            return field(base, e.type, {static_cast<unsigned>(e.fieldIndices.at(0))});
        }
        case ExprKind::Unary:
            return emit(e.text == "!" ? Op::Not : Op::Negate, e.type, {expression(e.args[0])}, {}, 0, e.span);
        case ExprKind::Call: {
            if (e.text == "repeat") return repeat(e, true).array;
            std::vector<Value> arguments;
            for (const auto& arg : e.args) arguments.push_back(expression(arg));
            Op op = e.text == "len" ? Op::Length : Op::Call;
            if (e.text == "readByte") op = Op::ReadByte;
            auto value = emit(op, e.type, std::move(arguments), e.text, 0, e.span);
            return track(value);
        }
        case ExprKind::Binary: {
            if (e.text == "&&" || e.text == "||") return lazy(e);
            const auto left = expression(e.args[0]);
            const auto right = expression(e.args[1]);
            return emit(Op::Binary, e.type, {left, right}, e.text, 0, e.span);
        }
        }
        throw std::logic_error("unknown expression in lowering");
    }
    void bind(const Stmt& decl, Value init) {
        const auto value = own(init, decl.exprs.at(0).span);
        const auto slot = declare(decl.name, value.type);
        store(slot, value);
        if (value.type == Type::IntArray && !value.freshRegion) scopes.back().arrays.push_back(slot);
    }
    void sequence(const std::vector<Stmt>& statements) {
        for (std::size_t i = 0; i < statements.size(); ++i) {
            if (fn.blocks[here].exit != Exit::Open) break;
            const Stmt& stmt = statements[i];
            if (i + 1 < statements.size()) {
                if (const auto reads = firstFullWrite(stmt, statements[i + 1])) {
                    const auto fill = repeat(stmt.exprs.at(0), false);
                    bind(stmt, fill.array);
                    dropTemporaries();
                    for (const auto* read : *reads) forwardedReads.emplace(read, fill.element);
                    statement(statements[++i]);
                    for (const auto* read : *reads) forwardedReads.erase(read);
                    dropTemporaries();
                    continue;
                }
            }
            statement(stmt);
            dropTemporaries();
        }
    }
    void nested(const std::vector<Stmt>& statements) {
        scopes.emplace_back();
        markRegion(statements);
        sequence(statements);
        if (fn.blocks[here].exit == Exit::Open) release(scopes.back());
        scopes.pop_back();
    }
    void ifStatement(const Stmt& s) {
        const auto cond = expression(s.exprs[0]);
        dropTemporaries();
        const auto yes = block();
        const auto no = block();
        const auto end = block();
        branch(cond, yes, no);
        here = yes;
        nested(s.body);
        const bool yesReturns = fn.blocks[here].exit != Exit::Open;
        jump(end);
        here = no;
        nested(s.otherwise);
        const bool noReturns = fn.blocks[here].exit != Exit::Open;
        jump(end);
        here = end;
        if (yesReturns && noReturns) fn.blocks[here].exit = Exit::Unreachable;
    }
    void loop(const Stmt& s) {
        const bool counting = s.kind == StmtKind::For;
        std::optional<unsigned> iterator;
        Value upper{};
        if (counting) {
            const auto lower = expression(s.exprs[0]);
            upper = expression(s.exprs[1]);
            dropTemporaries();
            scopes.emplace_back();
            iterator = declare(s.name, Type::Int);
            store(*iterator, lower);
        }
        const auto header = block();
        const auto body = block();
        const auto end = block();
        jump(header);
        here = header;
        const auto condition = counting
            ? emit(Op::Binary, Type::Bool, {load(*iterator), upper}, "<")
            : expression(s.exprs[0]);
        dropTemporaries();
        branch(condition, body, end);
        here = body;
        nested(s.body);
        if (counting && fn.blocks[here].exit == Exit::Open) {
            const auto n = load(*iterator);
            const auto one = emit(Op::Int, Type::Int, {}, "1");
            // The exclusive upper bound proves that incrementing the index is representable.
            store(*iterator, emit(Op::Binary, Type::Int, {n, one}, "rangeIncrement"));
        }
        jump(header);
        if (counting) scopes.pop_back();
        here = end;
    }
    void match(const Stmt& s) {
        auto candidate = expression(s.exprs.at(0));
        dropTemporaries();
        const auto& decl = *program.declaration(candidate.type);
        const auto tag = emit(Op::Tag, Type::Int, {candidate});
        const auto dispatch = here;
        const auto invalid = block();
        const auto end = block();
        fn.blocks[invalid].exit = Exit::Unreachable; // only declared constructors produce tags
        fn.blocks[dispatch].exit = Exit::Switch;
        fn.blocks[dispatch].value = tag;
        fn.blocks[dispatch].yes = invalid;
        fn.blocks[dispatch].targets.resize(decl.variants.size(), invalid);

        bool fallsThrough = false;
        for (const auto& arm : s.arms) {
            const auto variant = variantOf(decl, arm.variant);
            here = block();
            fn.blocks[dispatch].targets[variant] = here;
            scopes.emplace_back();
            markRegion(arm.body);
            const auto& fields = decl.variants[variant].fields;
            for (unsigned i = 0; i < arm.names.size(); ++i) {
                auto v = field(candidate, fields.at(i).type, {variant + 1, i});
                store(declare(arm.names[i], v.type), v);
            }
            sequence(arm.body);
            if (fn.blocks[here].exit == Exit::Open) release(scopes.back());
            scopes.pop_back();
            fallsThrough = fallsThrough || fn.blocks[here].exit == Exit::Open;
            jump(end);
        }
        here = end;
        if (!fallsThrough) fn.blocks[end].exit = Exit::Unreachable;
    }
    void statement(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Let:
        case StmtKind::Var: {
            bind(s, expression(s.exprs[0]));
            break;
        }
        case StmtKind::Assign: {
            const auto v = own(expression(s.exprs[0]), s.exprs[0].span);
            const auto slot = lookup(s.name);
            if (v.type == Type::IntArray) drop(slot);
            store(slot, v);
            break;
        }
        case StmtKind::IndexSet: {
            const auto base = load(lookup(s.name));
            const auto index = expression(s.exprs.at(0));
            const auto value = expression(s.exprs.at(1));
            emit(Op::IndexSet, Type::Void, {base, index, value}, {}, 0, s.span);
            break;
        }
        case StmtKind::Call: expression(s.exprs[0]); break;
        case StmtKind::If: ifStatement(s); break;
        case StmtKind::While:
        case StmtKind::For: loop(s); break;
        case StmtKind::Match: match(s); break;
        case StmtKind::Return: {
            std::optional<Value> v;
            if (!s.exprs.empty()) v = returned(s.exprs[0]);
            cleanupReturn();
            fn.blocks[here].exit = Exit::Return;
            fn.blocks[here].value = v;
            break;
        }
        case StmtKind::Assert: emit(Op::Assert, Type::Void, {expression(s.exprs[0])}, {}, 0, s.span); break;
        case StmtKind::Print: emit(Op::Print, Type::Void, {expression(s.exprs[0])}, {}, 0, s.span); break;
        }
    }
public:
    Lowerer(const Program& program, const Function& f) : program(program), fn{f, {}, {Block{}}} {}
    FunctionIR lower() {
        for (const auto& p : fn.source.params) declare(p.name, p.type);
        markRegion(fn.source.body);
        sequence(fn.source.body);
        if (fn.blocks[here].exit == Exit::Open) {
            cleanupReturn();
            fn.blocks[here].exit = fn.source.result == Type::Void ? Exit::Return : Exit::Unreachable;
        }
        return std::move(fn);
    }
};

} // namespace veloz::native
