#pragma once

#include "veloz.hpp"
#include "layout.hpp"
#include "native_fill.hpp"
#include "native_range.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace veloz::native {

inline bool needsDropType(const Program& program, Type t) {
    std::unordered_set<Type> seen;
    auto check = [&](auto& self, Type tp) -> bool {
        if (program.isAnyPtr(tp) || program.isHandle(tp) || program.isSimd(tp)) return false;
        if (tp == Type::Str || tp == Type::StringBuilder || Program::isPrimitiveResource(tp) ||
            program.isArray(tp) || program.isMap(tp) || program.isSet(tp) ||
            program.isPool(tp) || program.isShared(tp) || program.isTask(tp) || program.isFrozen(tp) ||
            program.isAtomic(tp) || program.isRingQueue(tp) ||
            program.isDyn(tp) || program.isFn(tp)) return true;
        if (tp < Type::NamedBase || !seen.insert(tp).second) return false;
        if (const auto* decl = program.declaration(tp)) {
            for (const auto& v : decl->variants) {
                for (const auto& f : v.fields) {
                    if (self(self, f.type)) return true;
                }
            }
        }
        return false;
    };
    return check(check, t);
}

// ---- Regiones de celdas ---------------------------------------------------------------------
// Una celda es la caja del montón de un campo recursivo (Shared<T>). Un árbol que se crea y se
// consume dentro de una vuelta de bucle no necesita liberarse nodo a nodo: sus celdas se reservan
// en una región y la región entera se suelta de golpe al acabar la vuelta. Para que eso sea
// seguro, nada creado dentro puede seguir vivo fuera y nada de fuera puede quedar retenido dentro.

// ¿Un valor de este tipo no puede contener ninguna celda? Ante la duda, false.
inline bool holdsNoCells(const Program& program, Type t) {
    std::unordered_set<Type> seen;
    auto check = [&](auto& self, Type tp) -> bool {
        if (tp == Type::Unknown) return false;
        if (tp == Type::Void || tp == Type::Bool || tp == Type::Str || tp == Type::StringBuilder ||
            tp == Type::IntArray || Program::isNumericType(tp)) return true;
        if (program.isShared(tp) || program.isDyn(tp) || program.isFn(tp) || program.isPool(tp) ||
            program.isTask(tp) || program.isFrozen(tp) || program.isAtomic(tp) || program.isRingQueue(tp) ||
            program.isAnyPtr(tp) || program.isHandle(tp) || Program::isPrimitiveResource(tp)) return false;
        if (program.isArray(tp)) return self(self, program.elemOf(tp));
        if (program.isMap(tp)) return self(self, program.mapKey(tp)) && self(self, program.mapVal(tp));
        if (program.isSet(tp)) return self(self, program.setElem(tp));
        if (program.isSimd(tp)) return true;
        if (tp < Type::NamedBase) return false;
        const auto* decl = program.declaration(tp);
        if (!decl) return false;
        if (!seen.insert(tp).second) return true;
        for (const auto& v : decl->variants)
            for (const auto& f : v.fields)
                if (!self(self, f.type)) return false;
        return true;
    };
    return check(check, t);
}

struct CellRegionInfo {
    std::unordered_set<std::string> safe;          // funciones que pueden ejecutarse dentro de una región
    std::unordered_set<std::string> allocs;        // funciones que crean celdas, ellas o lo que llaman
    std::unordered_map<std::string, bool> userFns; // función del usuario -> ¿es externa (C)?
};

template <class OnExpr, class OnStmt>
void walkAst(const Expr& e, OnExpr& onExpr, OnStmt& onStmt);
template <class OnExpr, class OnStmt>
void walkAst(const std::vector<Stmt>& body, OnExpr& onExpr, OnStmt& onStmt) {
    for (const auto& s : body) {
        onStmt(s);
        for (const auto& e : s.exprs) walkAst(e, onExpr, onStmt);
        walkAst(s.body, onExpr, onStmt);
        walkAst(s.otherwise, onExpr, onStmt);
        for (const auto& arm : s.arms) {
            walkAst(arm.body, onExpr, onStmt);
            if (arm.resultExpr) walkAst(*arm.resultExpr, onExpr, onStmt);
        }
    }
}
template <class OnExpr, class OnStmt>
void walkAst(const Expr& e, OnExpr& onExpr, OnStmt& onStmt) {
    onExpr(e);
    for (const auto& a : e.args) walkAst(a, onExpr, onStmt);
    if (e.matchExpr) {
        walkAst(e.matchExpr->scrutinee, onExpr, onStmt);
        for (const auto& arm : e.matchExpr->arms) {
            walkAst(arm.body, onExpr, onStmt);
            if (arm.resultExpr) walkAst(*arm.resultExpr, onExpr, onStmt);
        }
    }
    if (e.closure) walkAst(e.closure->body, onExpr, onStmt);
    if (e.ifExpr) {
        walkAst(e.ifExpr->condition, onExpr, onStmt);
        walkAst(e.ifExpr->thenBody, onExpr, onStmt);
        if (e.ifExpr->thenResult) walkAst(*e.ifExpr->thenResult, onExpr, onStmt);
        walkAst(e.ifExpr->elseBody, onExpr, onStmt);
        if (e.ifExpr->elseResult) walkAst(*e.ifExpr->elseResult, onExpr, onStmt);
    }
}

// ¿Puede este programa ejecutar código Turi en más de un hilo? Lo hace si lanza tareas, si entrega
// punteros a función a código C o si exporta funciones que C puede llamar desde donde quiera.
inline bool programMayUseThreads(const Program& program) {
    bool threads = false;
    auto onExpr = [&](const Expr& e) {
        if (e.kind == ExprKind::Call &&
            (e.text == "taskSpawn" || e.text == "taskSpawnMove" || e.text == "parallelMap" ||
             e.text == "taskScope" || e.text == "fnPtr"))
            threads = true;
    };
    auto onStmt = [](const Stmt&) {};
    for (const auto& f : program.functions) {
        if (f.isExternC && !f.isExternDecl) return true;
        walkAst(f.body, onExpr, onStmt);
    }
    return threads;
}

// Operaciones integradas que no sacan una celda de la región ni meten una de fuera: aritmética,
// texto, colecciones y celdas. Lo que no está aquí (hilos, canales, punteros, cierres, FFI,
// archivos) impide usar una región.
inline bool cellRegionBuiltin(const std::string& name) {
    static const std::unordered_set<std::string> ok{
        "Some", "None", "Ok", "Err", "abs", "argAt", "argsCount", "bswap", "cap", "ceil", "clear", "clockNs",
        "clz", "cos", "ctz", "float", "floor", "int", "isErr", "isInf", "isNaN", "isNone", "isOk", "isSome",
        "len", "mapDel", "mapGet", "mapHas", "mapKeys", "mapNew", "mapSet", "mapValues", "mapWithCap", "max",
        "min", "ord", "parseFloat", "parseInt", "pop", "popcnt", "pow", "push", "repeat", "reserve", "rotl",
        "rotr", "round", "saturatingAdd", "saturatingMul", "saturatingSub", "sbClear", "sbNew", "sbPush",
        "sbPushByte", "sbPushFloat", "sbPushInt", "sbStr", "sbTake", "sbWithCap", "setAdd", "setDel",
        "setHas", "setItems", "setNew", "setWithCap", "sharedGet", "sharedNew", "sin", "sort", "sortStable",
        "sqrt", "str", "strContains", "strEndsWith", "strFromByte", "strIndexOf", "strStartsWith",
        "tryParseFloat", "tryParseInt", "unwrapOr", "wrappingAdd", "wrappingMul", "wrappingSub",
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
        "eprint", "write", "ewrite", "flush", "writeByte"};
    return ok.contains(name);
}

// Lo que un trozo de código hace respecto a las celdas: a quién llama, si crea celdas y si
// contiene algo que impide una región.
struct CellRegionScan {
    std::vector<std::string> calls;
    bool allocs = false;
    bool blocked = false;
};
inline CellRegionScan scanCellRegion(const std::vector<Stmt>& body,
                                     const std::unordered_map<std::string, bool>& userFns) {
    CellRegionScan scan;
    auto onExpr = [&](const Expr& e) {
        if (e.isResourceMove) scan.blocked = true;
        switch (e.kind) {
        case ExprKind::Call: {
            if (auto it = userFns.find(e.text); it != userFns.end()) {
                if (it->second) scan.blocked = true;
                else scan.calls.push_back(e.text);
            } else if (!cellRegionBuiltin(e.text)) {
                scan.blocked = true;
            } else if (e.text == "sharedNew") {
                scan.allocs = true;
            }
            break;
        }
        case ExprKind::MethodCall: case ExprKind::AssocCall: case ExprKind::DynCall: case ExprKind::BoxDyn:
        case ExprKind::Closure: case ExprKind::ClosureCall:
            scan.blocked = true;
            break;
        default:
            break;
        }
    };
    auto onStmt = [&](const Stmt& s) {
        if (s.kind == StmtKind::SafetyBlock) scan.blocked = true;
    };
    walkAst(body, onExpr, onStmt);
    return scan;
}

inline std::shared_ptr<const CellRegionInfo> cellRegionInfo(const Program& program) {
    if (program.cellRegionCache && program.cellRegionCacheFns == program.functions.size())
        return std::static_pointer_cast<const CellRegionInfo>(program.cellRegionCache);
    auto info = std::make_shared<CellRegionInfo>();
    for (const auto& f : program.functions)
        if (!f.isTest) info->userFns.emplace(f.name, f.isExternC || f.isExternDecl);
    std::unordered_map<std::string, CellRegionScan> scans;
    for (const auto& f : program.functions) {
        if (f.isTest || f.isExternC || f.isExternDecl || scans.contains(f.name)) continue;
        auto scan = scanCellRegion(f.body, info->userFns);
        if (!scan.blocked) info->safe.insert(f.name);
        if (scan.allocs) info->allocs.insert(f.name);
        scans.emplace(f.name, std::move(scan));
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [name, scan] : scans) {
            if (info->safe.contains(name)) {
                for (const auto& callee : scan.calls) {
                    if (!info->safe.contains(callee)) { info->safe.erase(name); changed = true; break; }
                }
            }
            if (!info->allocs.contains(name)) {
                for (const auto& callee : scan.calls) {
                    if (info->allocs.contains(callee)) { info->allocs.insert(name); changed = true; break; }
                }
            }
        }
    }
    program.cellRegionCache = info;
    program.cellRegionCacheFns = program.functions.size();
    return info;
}

// ---- Resultado construido en su celda ---------------------------------------------------------
// `Node { left: Some(build(d - 1)), ... }` guarda en una celda lo que `build` devuelve. Devolver el
// valor (por memoria si no cabe en dos registros), releerlo y copiarlo a la celda son tres pasadas por
// los mismos bytes. Para cada función cuyo resultado alguien mete directamente en una celda se genera
// una variante `f.cell` que reserva la celda, construye el resultado dentro y devuelve el puntero.
// Las llamadas `sharedNew(f(...))` pasan a ser `f.cell(...)`. La función original no cambia.
inline void wrapReturnsInCell(Expr& e, Type cellT);
inline void wrapReturnsInCell(std::vector<Stmt>& body, Type cellT) {
    for (auto& s : body) {
        for (auto& e : s.exprs) wrapReturnsInCell(e, cellT);
        if (s.kind == StmtKind::Return && !s.exprs.empty()) {
            Expr inner = std::move(s.exprs[0]);
            Expr call{ExprKind::Call, inner.span, "sharedNew", {}};
            call.args.push_back(std::move(inner));
            call.type = cellT;
            s.exprs[0] = std::move(call);
        }
        wrapReturnsInCell(s.body, cellT);
        wrapReturnsInCell(s.otherwise, cellT);
        for (auto& arm : s.arms) {
            wrapReturnsInCell(arm.body, cellT);
            if (arm.resultExpr) wrapReturnsInCell(*arm.resultExpr, cellT);
        }
    }
}
inline void wrapReturnsInCell(Expr& e, Type cellT) {
    // Las closures tienen sus propios `return`: no se entra en ellas.
    for (auto& a : e.args) wrapReturnsInCell(a, cellT);
    if (e.matchExpr) {
        wrapReturnsInCell(e.matchExpr->scrutinee, cellT);
        for (auto& arm : e.matchExpr->arms) {
            wrapReturnsInCell(arm.body, cellT);
            if (arm.resultExpr) wrapReturnsInCell(*arm.resultExpr, cellT);
        }
    }
    if (e.ifExpr) {
        wrapReturnsInCell(e.ifExpr->condition, cellT);
        wrapReturnsInCell(e.ifExpr->thenBody, cellT);
        if (e.ifExpr->thenResult) wrapReturnsInCell(*e.ifExpr->thenResult, cellT);
        wrapReturnsInCell(e.ifExpr->elseBody, cellT);
        if (e.ifExpr->elseResult) wrapReturnsInCell(*e.ifExpr->elseResult, cellT);
    }
}

template <class OnExpr>
void rewriteExprs(Expr& e, OnExpr& onExpr);
template <class OnExpr>
void rewriteExprs(std::vector<Stmt>& body, OnExpr& onExpr) {
    for (auto& s : body) {
        for (auto& e : s.exprs) rewriteExprs(e, onExpr);
        rewriteExprs(s.body, onExpr);
        rewriteExprs(s.otherwise, onExpr);
        for (auto& arm : s.arms) {
            rewriteExprs(arm.body, onExpr);
            if (arm.resultExpr) rewriteExprs(*arm.resultExpr, onExpr);
        }
    }
}
template <class OnExpr>
void rewriteExprs(Expr& e, OnExpr& onExpr) {
    for (auto& a : e.args) rewriteExprs(a, onExpr);
    if (e.matchExpr) {
        rewriteExprs(e.matchExpr->scrutinee, onExpr);
        for (auto& arm : e.matchExpr->arms) {
            rewriteExprs(arm.body, onExpr);
            if (arm.resultExpr) rewriteExprs(*arm.resultExpr, onExpr);
        }
    }
    if (e.ifExpr) {
        rewriteExprs(e.ifExpr->condition, onExpr);
        rewriteExprs(e.ifExpr->thenBody, onExpr);
        if (e.ifExpr->thenResult) rewriteExprs(*e.ifExpr->thenResult, onExpr);
        rewriteExprs(e.ifExpr->elseBody, onExpr);
        if (e.ifExpr->elseResult) rewriteExprs(*e.ifExpr->elseResult, onExpr);
    }
    if (e.closure) rewriteExprs(e.closure->body, onExpr);
    onExpr(e);
}

inline void addCellVariants(Program& program) {
    if (envEnabled("VELOZ_NO_CELL_RETURN")) return;
    std::unordered_map<std::string, std::size_t> byName;
    for (std::size_t i = 0; i < program.functions.size(); ++i)
        if (!program.functions[i].isTest) byName.emplace(program.functions[i].name, i);
    const auto eligible = [&](const Expr& e) -> const Function* {
        if (e.kind != ExprKind::Call || e.text != "sharedNew" || e.args.size() != 1) return nullptr;
        if (byName.contains("sharedNew")) return nullptr;
        const Expr& call = e.args[0];
        if (call.kind != ExprKind::Call) return nullptr;
        const auto found = byName.find(call.text);
        if (found == byName.end()) return nullptr;
        const Function& f = program.functions[found->second];
        if (f.isExternC || f.isExternDecl || f.isImpl || !f.generics.empty() || !f.ensuresClauses.empty() ||
            f.name == "main" || f.result != call.type || !program.declaration(f.result) ||
            !program.isShared(e.type) || program.sharedElem(e.type) != f.result)
            return nullptr;
        if (call.args.size() != f.params.size()) return nullptr;
        for (const auto& a : call.args)
            if (a.isMutArg) return nullptr;
        return &f;
    };
    std::unordered_map<std::string, Type> wanted; // función -> tipo de su celda
    auto collect = [&](Expr& e) {
        if (const Function* f = eligible(e)) wanted.emplace(f->name, e.type);
    };
    for (auto& f : program.functions) rewriteExprs(f.body, collect);
    if (wanted.empty()) return;
    for (const auto& [name, cellT] : wanted) {
        if (byName.contains(name + ".cell")) continue;
        Function variant = program.functions[byName.at(name)];
        variant.name = name + ".cell";
        variant.sourceName = variant.sourceName + ".cell";
        variant.result = cellT;
        wrapReturnsInCell(variant.body, cellT);
        program.functions.push_back(std::move(variant));
    }
    auto redirect = [&](Expr& e) {
        const Function* f = eligible(e);
        if (!f || !wanted.contains(f->name)) return;
        Expr call = std::move(e.args[0]);
        call.text = f->name + ".cell";
        call.type = wanted.at(f->name);
        e = std::move(call);
    };
    for (auto& f : program.functions) rewriteExprs(f.body, redirect);
}

struct Value {
    Type type; unsigned id;
    std::optional<unsigned> owner{}; // slot that can transfer its heap allocation
    bool freshRegion = false;
    bool immortal = false; // celda constante compartida: se usa sin contar referencias
};
enum class Op {
    Int, Float, Bool, Text, ArrayLit, Interp, Slice, Load, Store, Call, ExternCall, FnPtr, Builtin, BoxDyn, DynCall, MakeClosure, ClosureCall, Length, Negate, Not, BitNot, Binary, Print, Assert,
    Construct, Field, FieldSet, PlaceSet, Tag, RepeatHeap, RepeatRegion, Clone, Drop, ClearOwner, ClearField, ClearIndex,
    RegionMark, RegionRewind, Index, IndexSet, ReadByte,
    Min, Max, AbsDiff, Select, TileRepeat, FindFirst,
    TruncI32, SextI64, GemmF32, MatMulMod, FloydStepU16, ArgMinU16
};
struct PlaceStepIR {
    bool isIndex = false;
    unsigned fieldIdx = 0;
    unsigned idxSlot = 0;
    bool boundsProven = false; // índice demostrado dentro de rango: sin guarda R_BOUNDS
};
enum class ProofKind : std::uint8_t {
    None,
    ConstRange,
    LoopBound,
    ContractRequires,
    DiffGraph,
    SentinelLoop,
    ModuloBound,
    RawUnchecked
};
struct Operation {
    Op op;
    Value result;
    std::vector<Value> inputs;
    std::string data;
    unsigned slot = 0;
    Span span{};
    std::vector<unsigned> fields{};
    std::vector<unsigned> mutArgSlots{};
    std::vector<std::vector<PlaceStepIR>> mutArgPaths{};
    std::vector<PlaceStepIR> placeSteps{};
    bool initialize = true;
    bool boundsProven = false;
    bool overflowProven = false;
    bool divSafe = false;
    bool unsignedDiv = false;
    std::uint8_t width = 64;
    std::uint8_t elemBits = 64;
    bool elemUnsigned = false;
    int aliasGroup = -1;
    int loopAccessGroup = -1;
    bool unchecked = false;
    bool reassoc = false; // acumulación flotante `x += e`: el orden de agrupación es libre
    bool regionEnd = false; // Drop de algo que muere con su región de celdas: se puede omitir
    bool elided = false;    // ClearOwner que solo marca la entrega de un temporal: no genera código
    bool modNoOp = false;
    std::uint8_t modSubSteps = 0; // 0 = full %, 1 = 0 <= x < 2*m, 2 = 0 <= x < 3*m
    std::int64_t lhsUpper = -1;
    ProofKind proofKind = ProofKind::None;
};
enum class LoopHint : std::uint8_t { None, Vectorize, VectorizeParallel, FullUnroll };
enum class Exit { Open, Jump, Branch, Switch, Return, Unreachable };
struct Block {
    std::vector<Operation> ops;
    Exit exit = Exit::Open;
    std::optional<Value> value;
    unsigned yes = 0, no = 0;
    std::vector<unsigned> targets; // switch tag -> block, yes is the fallback
    LoopHint loopHint = LoopHint::None;
    int loopAccessGroup = -1;
    bool coldBranchYes = false;
    std::uint8_t unrollCount = 0;
    bool noInterleave = false;
};
struct FunctionIR {
    const Function& source;
    std::vector<Type> slots;
    std::vector<Block> blocks{1};
    bool usesRegion = false;
    bool canStackArena = false;
    unsigned nextValue = 0;
    std::vector<std::uint8_t> slotElemBits{};
    std::vector<bool> slotElemUnsigned{};
    std::vector<int> slotAliasGroup{};
    std::vector<bool> slotByRef{};
    std::vector<bool> slotOwned{};
};

// Registers are immutable. Locals use typed frame slots allocated once per call.
// Branch targets, including short-circuit edges, are explicit before LLVM lowering.
class Lowerer {
    const Program& program;
    const ReturnRanges& returns;
    FunctionIR fn;
    struct Scope {
        std::unordered_map<std::string, unsigned> names;
        std::vector<unsigned> arrays;
        std::optional<Value> mark;
        std::optional<Value> cellMark; // región de celdas abierta por este ámbito
        std::size_t cellLoopDepth = 0; // bucles abiertos cuando se abrió la región
    };
    std::vector<Scope> scopes{1};
    std::vector<unsigned> temporaries;
    std::unordered_map<unsigned, unsigned> tempBlock; // temporal -> bloque donde se creó
    std::optional<unsigned> movedSlot;                // variable que un `return` acaba de entregar
    std::size_t tempFloor = 0;
    std::unordered_map<const Expr*, Value> forwardedReads;
    std::unordered_set<std::string> reassigned;
    std::unordered_map<unsigned, IntRange> arrayElementRanges;
    struct SizeProof {
        const Expr* expression;
        std::unordered_map<const Expr*, unsigned> bindings;
        std::optional<unsigned> squareSlot;
    };
    std::unordered_map<unsigned, SizeProof> repetitionSizes;
    std::unordered_map<unsigned, IntRange> iteratorRanges;
    std::unordered_set<const Expr*> provenAccumulations;
    struct PendingAccumulation {
        const Expr* update;
        IntRange initial, lower, upper;
    };
    struct PendingFullWrite {
        unsigned array;
        const Expr* value;
        std::optional<IntRange> elements;
    };
    std::unordered_map<const Stmt*, PendingAccumulation> pendingAccumulations;
    std::unordered_map<const Stmt*, PendingFullWrite> pendingFullWrites;
    std::unordered_multimap<unsigned, unsigned> arraysWithLength;
    struct Linear {
        std::int64_t constant = 0;
        std::map<unsigned, std::int64_t> terms;
    };
    struct RangeProof { unsigned iterator; Linear lower, upper; };
    std::unordered_map<unsigned, Linear> arrayLengths;
    std::vector<RangeProof> rangeProofs;
    std::vector<std::pair<unsigned, unsigned>> validIndices;
    struct LoopTarget {
        std::string label;
        unsigned continueBlk;
        unsigned breakBlk;
        std::size_t breakScopeCount;
        std::size_t continueScopeCount;
        std::size_t tempCount = 0;
    };
    std::vector<LoopTarget> loopTargets;
    std::unordered_map<std::string, unsigned> paramSlots;
    unsigned here = 0;
    unsigned nextValue = 0;
    SafetyTier curSafety = SafetyTier::Verified;

    static bool parseIntLiteral(std::string_view text, std::int64_t& value) {
        if (text.starts_with("0x") || text.starts_with("0X") || text.starts_with("0b") || text.starts_with("0B")) {
            const int base = (text[1] == 'x' || text[1] == 'X') ? 16 : 2;
            std::uint64_t uv{};
            const auto end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data() + 2, end, uv, base);
            if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
            value = static_cast<std::int64_t>(uv);
            return true;
        }
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, value);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    }

    static bool parseUIntLiteral(std::string_view text, std::uint64_t& value) {
        if (text.starts_with("0x") || text.starts_with("0X") || text.starts_with("0b") || text.starts_with("0B")) {
            const int base = (text[1] == 'x' || text[1] == 'X') ? 16 : 2;
            const auto end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data() + 2, end, value, base);
            return parsed.ec == std::errc{} && parsed.ptr == end;
        }
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, value);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    }

    static std::int64_t parseFloatBits(std::string_view text) {
        double value{};
        std::from_chars(text.data(), text.data() + text.size(), value);
        return std::bit_cast<std::int64_t>(value);
    }

    static std::uint32_t parseFloat32Bits(std::string_view text) {
        float value{};
        std::from_chars(text.data(), text.data() + text.size(), value);
        return std::bit_cast<std::uint32_t>(value);
    }

    const Function* findUserFunction(std::string_view name) const {
        for (const auto& f : program.functions) {
            if (!f.isTest && f.name == name) return &f;
        }
        return nullptr;
    }

    static void collectMutRootsInExpr(const Expr& e, std::unordered_set<std::string>& out) {
        if (e.kind == ExprKind::Call || e.kind == ExprKind::DynCall) {
            for (const auto& arg : e.args) {
                if (arg.isMutArg) {
                    const Expr* cur = &arg;
                    while (cur->kind == ExprKind::Field || cur->kind == ExprKind::Index)
                        cur = &cur->args.at(0);
                    if (cur->kind == ExprKind::Name) out.insert(cur->text);
                }
                collectMutRootsInExpr(arg, out);
            }
            return;
        }
        if (e.matchExpr) {
            collectMutRootsInExpr(e.matchExpr->scrutinee, out);
            for (const auto& arm : e.matchExpr->arms) {
                collectMutRootsInBlock(arm.body, out);
                if (arm.resultExpr) collectMutRootsInExpr(*arm.resultExpr, out);
            }
            return;
        }
        if (e.ifExpr) {
            collectMutRootsInExpr(e.ifExpr->condition, out);
            collectMutRootsInBlock(e.ifExpr->thenBody, out);
            if (e.ifExpr->thenResult) collectMutRootsInExpr(*e.ifExpr->thenResult, out);
            collectMutRootsInBlock(e.ifExpr->elseBody, out);
            if (e.ifExpr->elseResult) collectMutRootsInExpr(*e.ifExpr->elseResult, out);
            return;
        }
        for (const auto& arg : e.args) collectMutRootsInExpr(arg, out);
    }

    static void collectMutRootsInBlock(const std::vector<Stmt>& body, std::unordered_set<std::string>& out) {
        for (const auto& statement : body) {
            if (statement.kind == StmtKind::Assign || statement.kind == StmtKind::IndexSet ||
                statement.kind == StmtKind::FieldSet || statement.kind == StmtKind::PlaceSet)
                out.insert(statement.name);
            for (const auto& e : statement.exprs) collectMutRootsInExpr(e, out);
            collectMutRootsInBlock(statement.body, out);
            collectMutRootsInBlock(statement.otherwise, out);
            for (const auto& arm : statement.arms) collectMutRootsInBlock(arm.body, out);
        }
    }

    void findAssignments(const std::vector<Stmt>& body) {
        collectMutRootsInBlock(body, reassigned);
    }
    std::optional<IntRange> squareRange(const Expr& name) const {
        const auto slot = findSlot(name.text);
        if (!slot) return {};
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            for (const auto& entry : scope->names) {
                const auto proof = repetitionSizes.find(entry.second);
                if (proof != repetitionSizes.end() && proof->second.squareSlot &&
                    *proof->second.squareSlot == *slot) {
                    constexpr std::int64_t root = 4096;
                    static_assert(root * root == maxArrayLength);
                    return IntRange{-root, root};
                }
            }
        }
        return {};
    }
    std::optional<IntRange> range(const Expr& expression) const {
        return returns.expressionWithArrays(expression, [this](const Expr& index) -> std::optional<IntRange> {
            const auto& base = index.args.at(0);
            if (base.kind != ExprKind::Name) return {};
            const auto slot = findSlot(base.text);
            if (!slot) return {};
            auto found = arrayElementRanges.find(*slot);
            if (found == arrayElementRanges.end()) return {};
            return found->second;
        }, [this](const Expr& name) -> std::optional<IntRange> {
            const auto slot = findSlot(name.text);
            if (!slot) return {};
            if (const auto found = iteratorRanges.find(*slot); found != iteratorRanges.end()) return found->second;
            return squareRange(name);
        });
    }
    bool stableSize(const Expr& expression) const {
        return veloz::native::stableSize(expression, reassigned);
    }
    const Expr* fullWriteValue(const Stmt& decl, const Stmt& loop) const {
        if (decl.kind != StmtKind::Var || loop.kind != StmtKind::For || loop.body.size() != 1) return nullptr;
        const auto& init = decl.exprs.at(0);
        if (init.kind != ExprKind::Call || init.text != "repeat") return nullptr;
        const auto& write = loop.body.at(0);
        if (write.kind != StmtKind::IndexSet || write.name != decl.name ||
            !isName(write.exprs.at(0), loop.name)) return nullptr;
        if (loop.exprs.at(0).kind != ExprKind::Integer || loop.exprs.at(0).text != "0") return nullptr;
        const auto& size = init.args.at(1);
        const auto& end = loop.exprs.at(1);
        const bool measured = end.kind == ExprKind::Call && end.text == "len" && isName(end.args.at(0), decl.name);
        if (!measured && (mentions(size, decl.name) || !stableSize(size) || !sameExpression(size, end))) return nullptr;
        return &write.exprs.at(1);
    }
    void collectSizeBindings(const Expr& expression, SizeProof& proof) const {
        if (expression.kind == ExprKind::Name) proof.bindings.emplace(&expression, lookup(expression.text));
        for (const auto& arg : expression.args) collectSizeBindings(arg, proof);
    }
    bool matchesSize(const SizeProof& proof, const Expr& candidate) const {
        const Expr& original = *proof.expression;
        if (original.kind != candidate.kind || original.text != candidate.text ||
            original.args.size() != candidate.args.size()) return false;
        if (original.kind == ExprKind::Name) {
            const auto binding = proof.bindings.find(&original);
            const auto slot = findSlot(candidate.text);
            return binding != proof.bindings.end() && slot && *slot == binding->second;
        }
        for (std::size_t i = 0; i < original.args.size(); ++i) {
            SizeProof child{&original.args.at(i), proof.bindings, proof.squareSlot};
            if (!matchesSize(child, candidate.args.at(i))) return false;
        }
        return true;
    }
    std::optional<IntRange> iterationBound(const Expr& expression) const {
        if (const auto direct = range(expression)) return direct;
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            for (const auto& [name, slot] : scope->names) {
                const auto proof = repetitionSizes.find(slot);
                if (proof != repetitionSizes.end() && matchesSize(proof->second, expression))
                    return IntRange{0, maxArrayLength};
            }
        }
        return {};
    }
    std::optional<IntRange> iteratorRange(const Stmt& loop) const {
        const auto lower = iterationBound(loop.exprs.at(0));
        const auto upper = iterationBound(loop.exprs.at(1));
        if (!lower || !upper) return {};
        const auto last = ReturnRanges::subtractRanges(
            {upper->upper, upper->upper}, {1, 1});
        if (!last || lower->lower > last->lower) return {};
        return IntRange{lower->lower, last->lower};
    }
    const Expr* accumulatorUpdate(const Stmt& decl, const Stmt& loop) const {
        if (decl.kind != StmtKind::Var || loop.kind != StmtKind::For || loop.body.size() != 1) return nullptr;
        const auto& assignment = loop.body.at(0);
        if (assignment.kind != StmtKind::Assign || assignment.name != decl.name) return nullptr;
        const auto& update = assignment.exprs.at(0);
        if (update.kind != ExprKind::Binary || update.text != "+" ||
            !isName(update.args.at(0), decl.name)) return nullptr;
        return &update;
    }
    std::optional<PendingAccumulation> prepareAccumulation(const Stmt& decl, const Stmt& loop,
                                                            const Expr& update) const {
        const auto initial = range(decl.exprs.at(0));
        const auto lower = iterationBound(loop.exprs.at(0));
        const auto upper = iterationBound(loop.exprs.at(1));
        if (!initial || !lower || !upper) return {};
        return PendingAccumulation{&update, *initial, *lower, *upper};
    }
    bool accumulationProven(const PendingAccumulation& pending) const {
        const auto increment = range(pending.update->args.at(1));
        if (!increment) return false;
        const auto count = ReturnRanges::subtractRanges(
            {pending.upper.upper, pending.upper.upper}, {pending.lower.lower, pending.lower.lower});
        if (!count) return false;
        const auto prefixes = ReturnRanges::multiplyRanges(
            {0, std::max(std::int64_t{0}, count->lower)}, *increment);
        return prefixes && ReturnRanges::addRanges(pending.initial, *prefixes).has_value();
    }
    static bool addInteger(std::int64_t a, std::int64_t b, std::int64_t& result) {
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) return false;
        result = a + b;
        return true;
    }
    static bool multiplyInteger(std::int64_t a, std::int64_t b, std::int64_t& result) {
        constexpr auto min = std::numeric_limits<std::int64_t>::min();
        constexpr auto max = std::numeric_limits<std::int64_t>::max();
        if (a > 0 && ((b > 0 && a > max / b) || (b < 0 && b < min / a))) return false;
        if (a < 0 && ((b > 0 && a < min / b) || (b < 0 && a < max / b))) return false;
        if (a == 0 && b == min) return false;
        result = a * b;
        return true;
    }
    static std::optional<Linear> scale(Linear value, std::int64_t factor) {
        if (!multiplyInteger(value.constant, factor, value.constant)) return {};
        for (auto& [slot, coefficient] : value.terms)
            if (!multiplyInteger(coefficient, factor, coefficient)) return {};
        return value;
    }
    static std::optional<Linear> merge(Linear left, const Linear& right, std::int64_t factor = 1) {
        auto scaled = scale(right, factor);
        if (!scaled || !addInteger(left.constant, scaled->constant, left.constant)) return {};
        for (const auto& [slot, coefficient] : scaled->terms) {
            auto& target = left.terms[slot];
            if (!addInteger(target, coefficient, target)) return {};
            if (target == 0) left.terms.erase(slot);
        }
        return left;
    }
    std::optional<Linear> linear(const Expr& expression) const {
        if (expression.kind == ExprKind::Integer) {
            std::int64_t value{};
            if (!parseIntLiteral(expression.text, value)) return {};
            return Linear{value, {}};
        }
        if (expression.kind == ExprKind::Name && !reassigned.contains(expression.text))
            return Linear{0, {{lookup(expression.text), 1}}};
        if (expression.kind == ExprKind::Unary && expression.text == "-") {
            auto child = linear(expression.args.at(0));
            return child ? scale(*child, -1) : std::optional<Linear>{};
        }
        if (expression.kind != ExprKind::Binary) return {};
        auto left = linear(expression.args.at(0));
        auto right = linear(expression.args.at(1));
        if (!left || !right) return {};
        if (expression.text == "+") return merge(*left, *right);
        if (expression.text == "-") return merge(*left, *right, -1);
        if (expression.text == "*" && left->terms.empty()) return scale(*right, left->constant);
        if (expression.text == "*" && right->terms.empty()) return scale(*left, right->constant);
        return {};
    }
    bool inBounds(unsigned array, const Expr& index) const {
        if (index.kind == ExprKind::Name &&
            std::ranges::find(validIndices, std::pair{array, lookup(index.text)}) != validIndices.end()) return true;
        auto offset = linear(index);
        auto length = arrayLengths.find(array);
        if (!offset || length == arrayLengths.end()) return false;
        for (auto range = rangeProofs.rbegin(); range != rangeProofs.rend(); ++range) {
            if (offset->terms.size() != 1) continue;
            auto term = offset->terms.find(range->iterator);
            if (term == offset->terms.end() || term->second != 1) continue;
            auto lower = merge(range->lower, Linear{offset->constant, {}});
            auto upper = merge(range->upper, Linear{offset->constant, {}});
            if (!lower || !upper) continue;
            auto difference = merge(*upper, length->second, -1);
            if (lower->terms.empty() && lower->constant >= 0 && difference &&
                difference->terms.empty() && difference->constant <= 0) return true;
        }
        return false;
    }

    unsigned block() {
        fn.blocks.emplace_back();
        return static_cast<unsigned>(fn.blocks.size() - 1);
    }
    unsigned allocate(Type type) {
        fn.slots.push_back(type);
        fn.slotByRef.push_back(false);
        fn.slotOwned.push_back(false);
        return static_cast<unsigned>(fn.slots.size() - 1);
    }
    unsigned declare(const std::string& name, Type type) {
        const auto slot = allocate(type);
        scopes.back().names.emplace(name, slot);
        return slot;
    }
    std::optional<unsigned> findSlot(const std::string& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (auto found = it->names.find(name); found != it->names.end()) return found->second;
        }
        return {};
    }
    unsigned lookup(const std::string& name) const {
        if (const auto slot = findSlot(name)) return *slot;
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
        return memberIndex(decl.variants, qualified.substr(qualified.rfind('.') + 1));
    }
    bool needsDrop(Type t) const { return needsDropType(program, t); }
    std::pair<unsigned, std::vector<PlaceStepIR>> resolveMutArg(const Expr& e) {
        struct RevStep { bool isIndex; unsigned fieldIdx; const Expr* idxExpr; };
        std::vector<RevStep> rev;
        const Expr* cur = &e;
        while (cur->kind == ExprKind::Field || cur->kind == ExprKind::Index) {
            if (cur->kind == ExprKind::Index) {
                rev.push_back({true, 0u, &cur->args.at(1)});
            } else {
                rev.push_back({false, static_cast<unsigned>(cur->fieldIndices.at(0)), nullptr});
            }
            cur = &cur->args.at(0);
        }
        std::vector<PlaceStepIR> steps;
        steps.reserve(rev.size());
        for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
            if (it->isIndex) {
                const Value iv = expression(*it->idxExpr);
                const unsigned iSlot = allocate(Type::Int);
                store(iSlot, iv);
                steps.push_back({true, 0u, iSlot});
            } else {
                steps.push_back({false, it->fieldIdx, 0u});
            }
        }
        const unsigned rootSlot = lookup(cur->text);
        arrayElementRanges.erase(rootSlot);
        arrayLengths.erase(rootSlot);
        repetitionSizes.erase(rootSlot);
        return {rootSlot, std::move(steps)};
    }

    Value field(Value base, Type type, std::vector<unsigned> path) {
        auto result = emit(Op::Field, type, {base});
        fn.blocks[here].ops.back().fields = std::move(path);
        return result;
    }
    // Valor cuyos bits son todos cero: 0, false, la primera variante sin campos de un enum (None) y
    // los structs hechos solo de eso.
    bool zeroConst(const Expr& e) const {
        if (e.kind == ExprKind::Integer) return e.text == "0";
        if (e.kind == ExprKind::Boolean) return e.text == "false";
        if (e.kind != ExprKind::Construct) return false;
        const auto* decl = program.declaration(e.type);
        if (!decl) return false;
        if (decl->isEnum && (!e.args.empty() || variantOf(*decl, e.text) != 0)) return false;
        for (const auto& a : e.args)
            if (!zeroConst(a)) return false;
        return true;
    }
    // La celda compartida lleva un contador que no es atómico: solo en programas de un hilo.
    std::optional<bool> zeroCellsMemo;
    bool zeroCellsAllowed() {
        if (!zeroCellsMemo)
            zeroCellsMemo = !envEnabled("VELOZ_NO_ZERO_CELLS") && !programMayUseThreads(program);
        return *zeroCellsMemo;
    }
    Value construct(const Expr& e) {
        const auto& decl = *program.declaration(e.type);
        const auto tag = decl.isEnum ? variantOf(decl, e.text) : 0;
        std::vector<Value> args;
        for (const auto& arg : e.args) args.push_back(own(expression(arg), arg.span));
        auto result = emit(Op::Construct, e.type, std::move(args), {}, tag);
        fn.blocks[here].ops.back().fields.assign(e.fieldIndices.begin(), e.fieldIndices.end());
        return track(result);
    }
    Value load(unsigned slot) { return emit(Op::Load, fn.slots.at(slot), {}, {}, slot); }
    void store(unsigned slot, Value value) { emit(Op::Store, Type::Void, {value}, {}, slot); }
    void drop(unsigned slot) { emit(Op::Drop, Type::Void, {}, {}, slot); }
    // Dentro de una región de celdas, lo que muere fuera de un bucle interior no hace falta soltarlo:
    // la región entera se libera al salir. Dentro de un bucle interior sí se suelta, para que su
    // memoria se reutilice y no crezca con el número de vueltas. El generador omite el Drop solo si
    // el tipo no posee nada más que celdas de región.
    bool dropsDieWithRegion() const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (it->cellMark) return loopTargets.size() == it->cellLoopDepth;
        return false;
    }
    void scopeDrop(unsigned slot) {
        drop(slot);
        if (dropsDieWithRegion()) fn.blocks[here].ops.back().regionEnd = true;
    }
    Value track(Value value) {
        if (needsDrop(value.type) && !value.freshRegion && !value.immortal) {
            const auto slot = allocate(value.type);
            store(slot, value);
            temporaries.push_back(slot);
            tempBlock[slot] = here;
            value.owner = slot;
        }
        return value;
    }
    Value own(Value value, Span span) {
        if (!needsDrop(value.type) || value.freshRegion || value.immortal) return value;
        if (!value.owner) return emit(Op::Clone, value.type, {value}, {}, 0, span);
        // Un temporal que se entrega en el mismo bloque donde nació ya no hay que soltarlo en
        // ningún camino: se quita de la lista en vez de vaciarlo y soltarlo vacío después.
        std::size_t floor = tempFloor;
        for (const auto& lt : loopTargets) floor = std::max(floor, lt.tempCount);
        const auto born = tempBlock.find(*value.owner);
        const auto listed = floor <= temporaries.size()
            ? std::find(temporaries.begin() + static_cast<std::ptrdiff_t>(floor), temporaries.end(), *value.owner)
            : temporaries.end();
        // El ClearOwner se sigue emitiendo, marcado como `elided`: los pases del optimizador lo usan
        // como señal de que el bloque mueve un valor con dueño (sin él, el pase que convierte un `if`
        // en selección ejecutaba sin condición el Drop del valor anterior). No genera código.
        const bool elide = born != tempBlock.end() && born->second == here && listed != temporaries.end();
        if (elide) temporaries.erase(listed);
        emit(Op::ClearOwner, Type::Void, {}, {}, *value.owner);
        if (elide) fn.blocks[here].ops.back().elided = true;
        value.owner.reset();
        return value;
    }
    Value returned(const Expr& e) {
        if (e.kind != ExprKind::Name || !needsDrop(e.type)) return own(expression(e), e.span);
        const auto slot = lookup(e.text);
        auto value = load(slot);
        // Only heap-owned locals are on the drop list. A return is their last
        // use on this path. Parameters are borrowed and region storage expires.
        for (const auto& scope : scopes) {
            if (std::ranges::find(scope.arrays, slot) != scope.arrays.end()) {
                value.owner = slot;
                movedSlot = slot;
                break;
            }
        }
        return own(value, e.span);
    }
    void dropTemporaries() {
        while (temporaries.size() > tempFloor) {
            scopeDrop(temporaries.back());
            temporaries.pop_back();
        }
    }
    void emitTemporaryDropsDownTo(std::size_t floor) {
        for (std::size_t k = temporaries.size(); k > floor; --k) drop(temporaries[k - 1]);
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
        for (auto it = scope.arrays.rbegin(); it != scope.arrays.rend(); ++it) {
            if (movedSlot && *it == *movedSlot) continue; // ya entregada al que llama
            scopeDrop(*it);
        }
        if (scope.mark) emit(Op::RegionRewind, Type::Void, {*scope.mark});
        if (scope.cellMark) emit(Op::Builtin, Type::Void, {*scope.cellMark}, "cellLeave");
    }
    void cleanupReturn() {
        emitTemporaryDropsDownTo(0);
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
    // Lectura sin efectos: como `callFree`, pero puede atravesar celdas que nunca se modifican.
    bool pureRead(const Expr& e) const {
        if (callFree(e)) return true;
        if (e.kind == ExprKind::Field || e.kind == ExprKind::Index) {
            for (const auto& a : e.args)
                if (!pureRead(a)) return false;
            return true;
        }
        return borrowsCell(e);
    }
    // `sharedGet(x)` cuyo contenido se puede prestar en vez de copiar: `x` es una lectura sin
    // efectos y el programa no modifica en su sitio ninguna celda de ese tipo.
    bool borrowsCell(const Expr& e) const {
        return e.kind == ExprKind::Call && e.text == "sharedGet" && e.args.size() == 1 &&
               program.isShared(e.args[0].type) && !program.cellMutated(program.sharedElem(e.args[0].type)) &&
               pureRead(e.args[0]);
    }
    // Contenido de una celda sin copiarlo ni tomar posesión: quien necesite conservarlo lo clona.
    Value sharedPeek(const Expr& get) {
        const Value cell = expression(get.args[0]);
        return emit(Op::Builtin, get.type, {cell}, "sharedPeek", 0, get.span);
    }
    // Sitio que no cambia mientras viva su variable: una variable que la función nunca reasigna ni
    // presta con `mut`, sus campos, y las celdas inmutables que cuelgan de ella.
    bool stablePlace(const Expr& e) const {
        if (e.kind == ExprKind::Name) return !reassigned.contains(e.text) && findSlot(e.text).has_value();
        if (e.kind == ExprKind::Field) return e.args.size() == 1 && stablePlace(e.args[0]);
        return borrowsCell(e) && stablePlace(e.args[0]);
    }
    // Expresión sin llamadas ni efectos: literales, nombres y aritmética, campos, índices y cortes sobre ellos.
    static bool callFree(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Integer: case ExprKind::Float: case ExprKind::Boolean: case ExprKind::String:
        case ExprKind::Name:
            return true;
        case ExprKind::Unary: case ExprKind::Binary: case ExprKind::Field: case ExprKind::Index: case ExprKind::Slice:
            for (const auto& a : e.args)
                if (!callFree(a)) return false;
            return true;
        default:
            return false;
        }
    }
    static bool sameExpr(const Expr& a, const Expr& b) {
        if (a.kind != b.kind || a.text != b.text || a.args.size() != b.args.size()) return false;
        for (std::size_t i = 0; i < a.args.size(); ++i)
            if (!sameExpr(a.args[i], b.args[i])) return false;
        return true;
    }
    // Reconoce `mapSet(m, k, unwrapOr(mapGet(m, k), 0) op d)` con valor int o float de 64 bits.
    // Devuelve `d`, o nullptr si no es esa forma.
    const Expr* mapUpdateDelta(const Expr& e) const {
        if (e.text != "mapSet" || e.args.size() != 3 || e.args[0].kind != ExprKind::Name) return nullptr;
        const Type vt = program.mapVal(e.args[0].type);
        if (vt != Type::Int && vt != Type::Float) return nullptr;
        const Expr& value = e.args[2];
        if (value.kind != ExprKind::Binary || value.args.size() != 2 ||
            (value.text != "+" && value.text != "-" && value.text != "*")) return nullptr;
        const Expr& current = value.args[0];
        if (current.kind != ExprKind::Call || current.text != "unwrapOr" || current.args.size() != 2) return nullptr;
        const Expr& get = current.args[0];
        const Expr& zero = current.args[1];
        if (get.kind != ExprKind::Call || get.text != "mapGet" || get.args.size() != 2) return nullptr;
        if (get.args[0].kind != ExprKind::Name || get.args[0].text != e.args[0].text) return nullptr;
        const bool isZero = (zero.kind == ExprKind::Integer && zero.text == "0") ||
                            (zero.kind == ExprKind::Float && (zero.text == "0.0" || zero.text == "0"));
        if (!isZero || !sameExpr(get.args[1], e.args[1])) return nullptr;
        if (!callFree(e.args[1]) || !callFree(value.args[1])) return nullptr;
        return &value.args[1];
    }
    struct Repetition { Value array, element; };
    Repetition repeat(const Expr& e, bool initialize) {
        const auto element = expression(e.args.at(0));
        const auto size = expression(e.args.at(1));
        const bool region = e.allocation == ArrayAllocation::Region && !program.isResource(e.type);
        auto array = emit(region ? Op::RepeatRegion : Op::RepeatHeap, e.type,
                          {element, size}, {}, 0, e.span);
        fn.blocks[here].ops.back().initialize = initialize;
        array.freshRegion = region;
        return {track(array), element};
    }
    Value expression(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Integer: {
            if (e.type == Type::U64) {
                std::uint64_t parsed{};
                parseUIntLiteral(e.text, parsed);
                return emit(Op::Int, e.type, {}, std::to_string(parsed));
            }
            std::int64_t parsed{};
            parseIntLiteral(e.text, parsed);
            return emit(Op::Int, e.type, {}, std::to_string(parsed));
        }
        case ExprKind::Float:
            if (e.type == Type::F32)
                return emit(Op::Float, e.type, {}, std::to_string(parseFloat32Bits(e.text)));
            return emit(Op::Float, e.type, {}, std::to_string(parseFloatBits(e.text)));
        case ExprKind::Boolean: return emit(Op::Bool, e.type, {}, e.text);
        case ExprKind::String: return emit(Op::Text, e.type, {}, e.text);
        case ExprKind::Name: {
            const unsigned slot = lookup(e.text);
            Value v = load(slot);
            if (e.isResourceMove) v.owner = slot;
            return v;
        }
        case ExprKind::ArrayLit: {
            std::vector<Value> elems;
            elems.reserve(e.args.size());
            for (const auto& arg : e.args) elems.push_back(own(expression(arg), arg.span));
            return track(emit(Op::ArrayLit, e.type, std::move(elems), {}, 0, e.span));
        }
        case ExprKind::Interp: {
            std::vector<Value> parts;
            parts.reserve(e.args.size());
            for (const auto& arg : e.args) parts.push_back(expression(arg));
            return track(emit(Op::Interp, Type::Str, std::move(parts), {}, 0, e.span));
        }
        case ExprKind::Slice: {
            const auto base = expression(e.args.at(0));
            const auto lo = expression(e.args.at(1));
            const auto hi = expression(e.args.at(2));
            return track(emit(Op::Slice, e.type, {base, lo, hi}, {}, 0, e.span));
        }
        case ExprKind::Index: {
            if (auto it = forwardedReads.find(&e); it != forwardedReads.end()) return it->second;
            const auto base = expression(e.args.at(0));
            const auto index = expression(e.args.at(1));
            const auto value = emit(Op::Index, e.type, {base, index}, {}, 0, e.span);
            const auto& array = e.args.at(0);
            if (array.kind == ExprKind::Name && inBounds(lookup(array.text), e.args.at(1))) {
                fn.blocks[here].ops.back().boundsProven = true;
                fn.blocks[here].ops.back().proofKind = ProofKind::LoopBound;
            }
            return value;
        }
        case ExprKind::Construct: return construct(e);
        case ExprKind::Try: {
            const Type innerT = e.args[0].type;
            const bool isOpt = program.isOption(innerT);
            const Type okT = isOpt ? program.optionInner(innerT) : program.resultOk(innerT);
            const Type errT = isOpt ? Type::Unknown : program.resultErr(innerT);
            const Type retT = fn.source.result;

            const Value cand = own(expression(e.args[0]), e.args[0].span);
            const unsigned candSlot = allocate(innerT);
            store(candSlot, cand);

            const Value tagVal = emit(Op::Tag, Type::Int, {load(candSlot)});
            const Value okTag = emit(Op::Int, Type::Int, {}, isOpt ? "1" : "0");
            const Value isOk = emit(Op::Binary, Type::Bool, {tagVal, okTag}, "==", 0, e.span);

            const unsigned okBlk = block();
            const unsigned errBlk = block();
            const unsigned contBlk = block();
            branch(isOk, okBlk, errBlk);

            const unsigned okSlot = allocate(okT);

            here = errBlk;
            if (isOpt) {
                const Value retVal = emit(Op::Construct, retT, {}, {}, 0, e.span);
                if (needsDrop(innerT)) emit(Op::ClearOwner, Type::Void, {}, {}, candSlot);
                cleanupReturn();
                fn.blocks[here].exit = Exit::Return;
                fn.blocks[here].value = retVal;
            } else {
                const Value errPayload = field(load(candSlot), errT, {2u, 0u});
                const Value retVal = emit(Op::Construct, retT, {errPayload}, {}, 1, e.span);
                fn.blocks[here].ops.back().fields = {0u};
                if (needsDrop(innerT)) emit(Op::ClearOwner, Type::Void, {}, {}, candSlot);
                cleanupReturn();
                fn.blocks[here].exit = Exit::Return;
                fn.blocks[here].value = retVal;
            }

            here = okBlk;
            const Value okPayload = field(load(candSlot), okT, {isOpt ? 2u : 1u, 0u});
            store(okSlot, okPayload);
            if (needsDrop(innerT)) emit(Op::ClearOwner, Type::Void, {}, {}, candSlot);
            jump(contBlk);

            here = contBlk;
            Value res = load(okSlot);
            if (needsDrop(okT)) {
                temporaries.push_back(okSlot);
                res.owner = okSlot;
            }
            return res;
        }
        case ExprKind::Field: {
            // Leer un campo a través de una celda no necesita copiar el valor entero de la celda.
            const auto base = borrowsCell(e.args.at(0)) ? sharedPeek(e.args.at(0)) : expression(e.args.at(0));
            return field(base, e.type, {static_cast<unsigned>(e.fieldIndices.at(0))});
        }
        case ExprKind::Unary: {
            const Op uop = e.text == "!" ? Op::Not : (e.text == "~" ? Op::BitNot : Op::Negate);
            return emit(uop, e.type, {expression(e.args[0])}, {}, 0, e.span);
        }
        case ExprKind::Call: {
            if (e.text == "repeat") return repeat(e, true).array;
            if (e.text == "len") return emit(Op::Length, e.type, {expression(e.args[0])}, e.text, 0, e.span);
            if (e.text == "readByte") return emit(Op::ReadByte, e.type, {}, e.text, 0, e.span);
            if (e.text == "sizeOf") {
                LayoutCalculator lc(program);
                return emit(Op::Int, Type::Int, {}, std::to_string(lc.sizeAlignOf(e.typeArgs.at(0)).first));
            }
            if (e.text == "alignOf") {
                LayoutCalculator lc(program);
                return emit(Op::Int, Type::Int, {}, std::to_string(lc.sizeAlignOf(e.typeArgs.at(0)).second));
            }
            if (e.text == "bitcast") {
                const auto v = expression(e.args[0]);
                if (v.type == e.type) return v;
                return emit(Op::Builtin, e.type, {v}, "bitcast", 0, e.span);
            }
            if (e.text == "int" || e.text == "float" || e.text == "i8" || e.text == "i16" || e.text == "i32" ||
                e.text == "i64" || e.text == "u8" || e.text == "u16" || e.text == "u32" || e.text == "u64" ||
                e.text == "f32" || e.text == "f64") {
                const auto v = expression(e.args[0]);
                if (v.type == e.type) return v;
                if (e.text == "int" && !program.isNumericType(v.type)) {
                    const auto tag = emit(Op::Tag, Type::Int, {v});
                    const auto* decl = program.declaration(v.type);
                    if (!decl || !decl->hasExplicitEnumValues) return tag;
                    return emit(Op::Builtin, Type::Int, {tag}, "enumInt", static_cast<unsigned>(v.type), e.span);
                }
                return emit(Op::Builtin, e.type, {v}, "castNum", 0, e.span);
            }
            if (e.text == "str") {
                const auto v = expression(e.args[0]);
                if (v.type == Type::Str) return v;
                if (Program::isIntType(v.type)) return track(emit(Op::Builtin, Type::Str, {v}, "strFromInt", 0, e.span));
                if (Program::isFloatType(v.type)) return track(emit(Op::Builtin, Type::Str, {v}, "strFromFloat", 0, e.span));
                return track(emit(Op::Builtin, Type::Str, {v}, "strFromBool", 0, e.span));
            }
            if (e.text == "fnPtr") {
                return emit(Op::FnPtr, e.type, {}, e.labels.at(0), 0, e.span);
            }
            if (const Function* callee = findUserFunction(e.text)) {
                if (callee->isExternC) {
                    std::vector<Value> arguments;
                    arguments.reserve(e.args.size());
                    for (const auto& arg : e.args) arguments.push_back(expression(arg));
                    return emit(Op::ExternCall, e.type, std::move(arguments), callee->name, 0, e.span);
                }
                std::vector<Value> arguments;
                std::vector<unsigned> mutArgs(e.args.size(), ~0u);
                std::vector<std::vector<PlaceStepIR>> mutPaths(e.args.size());
                bool anyMut = false;
                arguments.reserve(e.args.size());
                // Un parámetro por valor es un préstamo de solo lectura. Si el argumento sale de una
                // celda y nada en esta llamada puede modificarla (sin argumentos `mut` ni otras
                // llamadas entre los argumentos), se presta el contenido de la celda sin copiarlo.
                bool canPeek = true;
                for (std::size_t i = 0; i < e.args.size(); ++i) {
                    if (i >= callee->params.size() || callee->params[i].byRef ||
                        !(callFree(e.args[i]) || borrowsCell(e.args[i])))
                        canPeek = false;
                }
                for (std::size_t i = 0; i < e.args.size(); ++i) {
                    if (i < callee->params.size() && callee->params[i].byRef) {
                        anyMut = true;
                        auto [cSlot, pSteps] = resolveMutArg(e.args[i]);
                        mutArgs[i] = cSlot;
                        mutPaths[i] = std::move(pSteps);
                        arguments.push_back(Value{e.args[i].type, nextValue++});
                    } else if (canPeek && borrowsCell(e.args[i]) && !callee->params[i].writable) {
                        arguments.push_back(sharedPeek(e.args[i]));
                    } else {
                        arguments.push_back(expression(e.args[i]));
                    }
                }
                auto value = emit(Op::Call, e.type, std::move(arguments), e.text, 0, e.span);
                if (anyMut) {
                    auto& op = fn.blocks[here].ops.back();
                    op.mutArgSlots = std::move(mutArgs);
                    op.mutArgPaths = std::move(mutPaths);
                }
                return track(value);
            }
            if (e.text == "isSome" || e.text == "isNone" || e.text == "isOk" || e.text == "isErr") {
                const Value cand = expression(e.args[0]);
                const Value tagVal = emit(Op::Tag, Type::Int, {cand});
                const bool wantOne = (e.text == "isSome" || e.text == "isErr");
                const Value targetTag = emit(Op::Int, Type::Int, {}, wantOne ? "1" : "0");
                return emit(Op::Binary, Type::Bool, {tagVal, targetTag}, "==", 0, e.span);
            }
            if (e.text == "unwrapOr") {
                const Value candRaw = expression(e.args[0]);
                const Value fb = expression(e.args[1]);
                const Type candT = e.args[0].type;
                const bool isOpt = program.isOption(candT);
                const Type okT = isOpt ? program.optionInner(candT) : program.resultOk(candT);

                const unsigned candSlot = allocate(candT);
                store(candSlot, own(candRaw, e.args[0].span));

                const Value tagVal = emit(Op::Tag, Type::Int, {load(candSlot)});
                const Value okTag = emit(Op::Int, Type::Int, {}, isOpt ? "1" : "0");
                const Value isOk = emit(Op::Binary, Type::Bool, {tagVal, okTag}, "==", 0, e.span);

                const unsigned resSlot = allocate(okT);
                const unsigned okBlk = block();
                const unsigned elseBlk = block();
                const unsigned contBlk = block();
                branch(isOk, okBlk, elseBlk);

                here = okBlk;
                const Value okPayload = field(load(candSlot), okT, {isOpt ? 2u : 1u, 0u});
                store(resSlot, okPayload);
                if (needsDrop(candT)) emit(Op::ClearOwner, Type::Void, {}, {}, candSlot);
                jump(contBlk);

                here = elseBlk;
                if (needsDrop(candT)) drop(candSlot);
                store(resSlot, own(fb, e.args[1].span));
                jump(contBlk);

                here = contBlk;
                Value res = load(resSlot);
                if (needsDrop(okT)) {
                    temporaries.push_back(resSlot);
                    res.owner = resSlot;
                }
                return res;
            }
            if (e.text == "push" || e.text == "pop" || e.text == "clear" || e.text == "reserve" ||
                e.text == "sbPush" || e.text == "sbPushByte" || e.text == "sbPushInt" ||
                e.text == "sbPushFloat" || e.text == "sbClear" || e.text == "sbTake" ||
                e.text == "mapSet" || e.text == "mapDel" || e.text == "setAdd" ||
                e.text == "setDel" || e.text == "sort" || e.text == "sortStable" ||
                e.text == "poolAdd" || e.text == "poolSet" || e.text == "poolRemove" ||
                e.text == "sharedSet" || e.text == "sharedUpdate" ||
                e.text == "fileRead" || e.text == "fileReadAll" || e.text == "fileReadLine" ||
                e.text == "fileWrite" || e.text == "fileFlush" || e.text == "fileClose" ||
                e.text == "tcpAccept" || e.text == "socketRead" || e.text == "socketWrite" ||
                e.text == "socketClose" || e.text == "listenerClose" ||
                e.text == "mutexLock" || e.text == "mutexTryLock" || e.text == "unlock" ||
                e.text == "taskSpawn" || e.text == "taskSpawnMove" ||
                e.text == "ptrOf" || e.text == "mutPtrOf" || e.text == "mutSlicePtr" ||
                e.text == "regionAlloc" || e.text == "regionAllocAligned" || e.text == "regionReset") {
                if (const Expr* delta = mapUpdateDelta(e)) {
                    // mapSet(m, k, mapGet(m, k).unwrapOr(0) op d): leer y escribir la misma clave es una
                    // sola búsqueda. `k` y `d` son expresiones sin llamadas, así que el orden no se nota.
                    std::vector<unsigned> mutArgs(3, ~0u);
                    std::vector<std::vector<PlaceStepIR>> mutPaths(3);
                    auto [mSlot, mSteps] = resolveMutArg(e.args[0]);
                    mutArgs[0] = mSlot;
                    mutPaths[0] = std::move(mSteps);
                    std::vector<Value> arguments;
                    arguments.push_back(Value{e.args[0].type, nextValue++});
                    arguments.push_back(expression(e.args[1]));
                    arguments.push_back(expression(*delta));
                    auto value = emit(Op::Builtin, e.type, std::move(arguments), "mapUpdate" + e.args[2].text, 0, e.span);
                    auto& op = fn.blocks[here].ops.back();
                    op.mutArgSlots = std::move(mutArgs);
                    op.mutArgPaths = std::move(mutPaths);
                    return track(value);
                }
                std::vector<Value> arguments;
                std::vector<unsigned> mutArgs(e.args.size(), ~0u);
                std::vector<std::vector<PlaceStepIR>> mutPaths(e.args.size());
                auto [cSlot, pSteps] = resolveMutArg(e.args[0]);
                mutArgs[0] = cSlot;
                mutPaths[0] = std::move(pSteps);
                arguments.push_back(Value{e.args[0].type, nextValue++});
                for (std::size_t k = 1; k < e.args.size(); ++k) {
                    Value rhs = expression(e.args[k]);
                    if (e.text == "push" || e.text == "poolAdd" || e.text == "sharedSet" ||
                        ((e.text == "mapSet" || e.text == "poolSet") && k == 2) ||
                        e.text == "taskSpawn" || e.text == "taskSpawnMove")
                        rhs = own(rhs, e.args[k].span);
                    arguments.push_back(rhs);
                }
                auto value = emit(Op::Builtin, e.type, std::move(arguments), e.text, 0, e.span);
                auto& op = fn.blocks[here].ops.back();
                op.mutArgSlots = std::move(mutArgs);
                op.mutArgPaths = std::move(mutPaths);
                return track(value);
            }
            if (e.text == "simdLanes") {
                const unsigned lanes = program.simdLanes(e.typeArgs.at(0));
                return emit(Op::Int, Type::Int, {}, std::to_string(lanes), 0, e.span);
            }
            if (e.text == "simdToArray") {
                std::vector<Value> arguments;
                std::vector<unsigned> mutArgs(e.args.size(), ~0u);
                std::vector<std::vector<PlaceStepIR>> mutPaths(e.args.size());
                arguments.push_back(expression(e.args[0]));
                auto [cSlot, pSteps] = resolveMutArg(e.args[1]);
                mutArgs[1] = cSlot;
                mutPaths[1] = std::move(pSteps);
                arguments.push_back(Value{e.args[1].type, nextValue++});
                arguments.push_back(expression(e.args[2]));
                auto value = emit(Op::Builtin, e.type, std::move(arguments), e.text, 0, e.span);
                auto& op = fn.blocks[here].ops.back();
                op.mutArgSlots = std::move(mutArgs);
                op.mutArgPaths = std::move(mutPaths);
                op.unchecked = (curSafety == SafetyTier::Raw);
                if (op.unchecked) op.proofKind = ProofKind::RawUnchecked;
                return track(value);
            }
            if (e.text == "sharedNew" && zeroConst(e.args[0]) && zeroCellsAllowed()) {
                // Una celda no tiene identidad: todas las que guardan la misma constante pueden ser la
                // misma. Para un contenido todo ceros (`Node { left: None, right: None }`, la hoja de
                // cualquier árbol) se usa una única celda estática que nunca se libera.
                Value cell = emit(Op::Builtin, e.type, {}, "sharedZero", 0, e.span);
                cell.immortal = true;
                return cell;
            }
            if (e.text == "sharedNew" || e.text == "freeze" || e.text == "taskJoin") {
                const Value inner = own(expression(e.args[0]), e.args[0].span);
                return track(emit(Op::Builtin, e.type, {inner}, e.text, 0, e.span));
            }
            if (e.text == "ringPush") {
                const Value qVal = expression(e.args[0]);
                const Value itemVal = own(expression(e.args[1]), e.args[1].span);
                return track(emit(Op::Builtin, e.type, {qVal, itemVal}, e.text, 0, e.span));
            }
            if (e.text == "atomicFence" || e.text == "atomicLoadOrd" || e.text == "atomicStoreOrd" ||
                e.text == "atomicSwapOrd" || e.text == "atomicCasOrd" || e.text == "atomicAddOrd" ||
                e.text == "atomicSubOrd" || e.text == "atomicAndOrd" || e.text == "atomicOrOrd" ||
                e.text == "atomicXorOrd") {
                const std::string& o = e.args.back().text;
                unsigned ordCode = 4u;
                if (o == "relaxed") ordCode = 0u;
                else if (o == "acquire") ordCode = 1u;
                else if (o == "release") ordCode = 2u;
                else if (o == "acqRel") ordCode = 3u;
                std::vector<Value> arguments;
                arguments.reserve(e.args.size() - 1u);
                for (std::size_t k = 0; k + 1u < e.args.size(); ++k) arguments.push_back(expression(e.args[k]));
                auto value = emit(Op::Builtin, e.type, std::move(arguments), e.text, 0, e.span);
                fn.blocks[here].ops.back().fields = {ordCode};
                return track(value);
            }
            std::vector<Value> arguments;
            arguments.reserve(e.args.size());
            for (const auto& arg : e.args) arguments.push_back(expression(arg));
            auto value = emit(Op::Builtin, e.type, std::move(arguments), e.text, 0, e.span);
            if (curSafety == SafetyTier::Raw &&
                (e.text == "ptrAdd" || e.text == "ptrRead" || e.text == "ptrWrite" ||
                 e.text == "ptrCopy" || e.text == "ptrZero" ||
                 e.text == "strFromCStr" || e.text == "strFromPtr" ||
                 e.text == "simdGet" || e.text == "simdSet" || e.text == "simdFromArray" ||
                 e.text == "simdLoad" || e.text == "simdLoadAligned" ||
                 e.text == "simdStore" || e.text == "simdStoreAligned")) {
                fn.blocks[here].ops.back().unchecked = true;
                fn.blocks[here].ops.back().proofKind = ProofKind::RawUnchecked;
            }
            return track(value);
        }
        case ExprKind::BoxDyn: {
            const Value inner = own(expression(e.args.at(0)), e.args.at(0).span);
            Value box = emit(Op::BoxDyn, e.type, {inner}, {}, 0, e.span);
            fn.blocks[here].ops.back().fields = {static_cast<unsigned>(e.assocType)};
            return track(box);
        }
        case ExprKind::DynCall: {
            const Type iface = e.assocType;
            const std::size_t mi = e.fieldIndices.at(0);
            const InterfaceDecl* ifaceDecl = nullptr;
            for (const auto& id : program.interfaces) {
                if (id.type == iface) { ifaceDecl = &id; break; }
            }
            const Function& sig = ifaceDecl->methods.at(mi);
            std::vector<Value> arguments;
            std::vector<unsigned> mutArgs(e.args.size(), ~0u);
            std::vector<std::vector<PlaceStepIR>> mutPaths(e.args.size());
            bool anyMut = false;
            arguments.reserve(e.args.size());
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                if (i < sig.params.size() && sig.params[i].byRef) {
                    anyMut = true;
                    auto [cSlot, pSteps] = resolveMutArg(e.args[i]);
                    mutArgs[i] = cSlot;
                    mutPaths[i] = std::move(pSteps);
                    arguments.push_back(Value{e.args[i].type, nextValue++});
                } else {
                    arguments.push_back(expression(e.args[i]));
                }
            }
            auto value = emit(Op::DynCall, e.type, std::move(arguments), e.text, 0, e.span);
            auto& op = fn.blocks[here].ops.back();
            op.fields = {static_cast<unsigned>(iface), static_cast<unsigned>(mi)};
            if (anyMut) {
                op.mutArgSlots = std::move(mutArgs);
                op.mutArgPaths = std::move(mutPaths);
            }
            return track(value);
        }
        case ExprKind::Closure: {
            const auto& cl = program.closures.at(e.closure->id);
            std::vector<Value> opInputs;
            if (cl.envStruct != Type::Void) {
                std::vector<Value> capVals;
                capVals.reserve(e.args.size());
                for (const auto& arg : e.args) capVals.push_back(own(expression(arg), arg.span));
                const Value envVal = emit(Op::Construct, cl.envStruct, std::move(capVals), {}, 0, e.span);
                auto& cop = fn.blocks[here].ops.back();
                cop.fields.resize(e.args.size());
                for (std::size_t ci = 0; ci < e.args.size(); ++ci) cop.fields[ci] = static_cast<unsigned>(ci);
                opInputs.push_back(envVal);
            }
            Value fnVal = emit(Op::MakeClosure, e.type, std::move(opInputs), {}, 0, e.span);
            fn.blocks[here].ops.back().fields = {static_cast<unsigned>(cl.id)};
            return track(fnVal);
        }
        case ExprKind::ClosureCall: {
            std::vector<Value> arguments;
            arguments.reserve(e.args.size());
            for (const auto& arg : e.args) arguments.push_back(expression(arg));
            auto value = emit(Op::ClosureCall, e.type, std::move(arguments), {}, 0, e.span);
            return track(value);
        }
        case ExprKind::Binary: {
            if (e.text == "&&" || e.text == "||") return lazy(e);
            const auto left = expression(e.args[0]);
            const auto right = expression(e.args[1]);
            if ((e.text == "==" || e.text == "!=") && left.type >= Type::NamedBase &&
                !program.isHandle(left.type) && !program.isAnyPtr(left.type) && !program.isSimd(left.type)) {
                const auto tl = emit(Op::Tag, Type::Int, {left});
                const auto tr = emit(Op::Tag, Type::Int, {right});
                return emit(Op::Binary, Type::Bool, {tl, tr}, e.text, 0, e.span);
            }
            const auto result = emit(Op::Binary, e.type, {left, right}, e.text, 0, e.span);
            if (left.type == Type::Str && e.text == "+") return track(result);
            if (left.type == Type::Int && (e.text == "+" || e.text == "-" || e.text == "*") &&
                (provenAccumulations.contains(&e) || range(e))) {
                fn.blocks[here].ops.back().overflowProven = true;
                fn.blocks[here].ops.back().proofKind = ProofKind::ConstRange;
            }
            return result;
        }
        case ExprKind::Match: return matchExpr(e);
        case ExprKind::If: return ifExprLowering(e);
        case ExprKind::MethodCall:
        case ExprKind::AssocCall:
            break;
        }
        throw std::logic_error("unknown expression in lowering");
    }
    void bind(const Stmt& decl, Value init) {
        const auto value = own(init, decl.exprs.at(0).span);
        if (decl.name == "_") {
            if (needsDrop(value.type) && !value.freshRegion) {
                const auto slot = allocate(value.type);
                store(slot, value);
                drop(slot);
            }
            return;
        }
        const auto& rhs = decl.exprs.at(0);
        std::optional<unsigned> sizeSlot;
        std::optional<Linear> sizeLinear;
        std::optional<SizeProof> sizeProof;
        if (rhs.kind == ExprKind::Call && rhs.text == "repeat" && rhs.type == Type::IntArray && !reassigned.contains(decl.name)) {
            const auto& size = rhs.args.at(1);
            sizeLinear = linear(size);
            if (size.kind == ExprKind::Name && !reassigned.contains(size.text)) sizeSlot = lookup(size.text);
            if (stableSize(size) && !mentions(size, decl.name)) {
                sizeProof = SizeProof{&size, {}, {}};
                collectSizeBindings(size, *sizeProof);
                if (size.kind == ExprKind::Binary && size.text == "*" &&
                    size.args.at(0).kind == ExprKind::Name && size.args.at(1).kind == ExprKind::Name) {
                    const auto left = lookup(size.args.at(0).text);
                    if (left == lookup(size.args.at(1).text)) sizeProof->squareSlot = left;
                }
            }
        }
        const auto slot = declare(decl.name, value.type);
        if (sizeSlot) arraysWithLength.emplace(*sizeSlot, slot);
        if (sizeLinear) arrayLengths.emplace(slot, std::move(*sizeLinear));
        if (sizeProof) repetitionSizes.emplace(slot, std::move(*sizeProof));
        store(slot, value);
        if (needsDrop(value.type) && !value.freshRegion) {
            fn.slotOwned[slot] = true;
            scopes.back().arrays.push_back(slot);
        }
    }
    void sequence(const std::vector<Stmt>& statements) {
        for (std::size_t i = 0; i < statements.size(); ++i) {
            if (fn.blocks[here].exit != Exit::Open) break;
            const Stmt& stmt = statements[i];
            if (i + 1 < statements.size() &&
                (stmt.kind == StmtKind::Let || stmt.kind == StmtKind::Var) &&
                !stmt.exprs.empty() && stmt.exprs[0].type == Type::IntArray) {
                const Stmt& loop = statements[i + 1];
                const Expr* written = fullWriteValue(stmt, loop);
                if (const auto reads = firstFullWrite(stmt, loop, reassigned)) {
                    const auto fill = repeat(stmt.exprs.at(0), false);
                    bind(stmt, fill.array);
                    dropTemporaries();
                    for (const auto* read : *reads) forwardedReads.emplace(read, fill.element);
                    if (written) trackFullWrite(loop, lookup(stmt.name), written);
                    statement(loop);
                    for (const auto* read : *reads) forwardedReads.erase(read);
                    if (written) finishFullWrite(loop);
                    ++i;
                    dropTemporaries();
                    continue;
                }
                if (written) {
                    statement(stmt);
                    dropTemporaries();
                    trackFullWrite(loop, lookup(stmt.name), written);
                    statement(loop);
                    finishFullWrite(loop);
                    ++i;
                    dropTemporaries();
                    continue;
                }
                if (const Expr* update = accumulatorUpdate(stmt, loop)) {
                    statement(stmt);
                    dropTemporaries();
                    if (auto pending = prepareAccumulation(stmt, loop, *update))
                        pendingAccumulations.emplace(&loop, std::move(*pending));
                    statement(loop);
                    ++i;
                    dropTemporaries();
                    continue;
                }
            }
            // Una sentencia suelta que crea celdas y no deja ninguna viva (`print(check(build(n)))`,
            // `total += check(build(n))`) tiene su propia región, igual que la vuelta de un bucle.
            if ((stmt.kind == StmtKind::Print || stmt.kind == StmtKind::Call || stmt.kind == StmtKind::Assign) &&
                statementRegionFits(stmt)) {
                scopes.emplace_back();
                scopes.back().cellMark = emit(Op::Builtin, Type::Int, {}, "cellEnter", 0, stmt.span);
                scopes.back().cellLoopDepth = loopTargets.size();
                statement(stmt);
                dropTemporaries();
                if (fn.blocks[here].exit == Exit::Open) release(scopes.back());
                scopes.pop_back();
                continue;
            }
            statement(stmt);
            dropTemporaries();
        }
    }
    bool statementRegionFits(const Stmt& stmt) const {
        if (program.anyCellMutated || !program.mutatedCells.empty()) return false;
        if (cellRegionInfo(program)->allocs.empty()) return false;
        for (const auto& scope : scopes)
            if (scope.cellMark) return false; // ya hay una región abierta alrededor
        const std::vector<Stmt> one{stmt};
        return cellRegionFits(one);
    }
    // ¿El cuerpo de este bucle puede tener su propia región de celdas? Hace falta que:
    //  - cree celdas (si no, no hay nada que ganar);
    //  - todo lo que ejecuta sea seguro para una región (`CellRegionInfo::safe`);
    //  - no nombre ninguna variable de fuera que pueda contener celdas: así ni se escapa una celda
    //    de la región a una variable que la sobrevive, ni una celda de la región retiene una vieja;
    //  - no pueda devolver celdas con `return` o `?` desde dentro.
    bool cellRegionFits(const std::vector<Stmt>& body) const {
        if (body.empty() || program.anyCellMutated || !program.mutatedCells.empty()) return false;
        const auto info = cellRegionInfo(program);
        if (info->allocs.empty()) return false; // el programa no crea celdas en ningún sitio
        const auto scan = scanCellRegion(body, info->userFns);
        if (scan.blocked) return false;
        bool allocs = scan.allocs;
        for (const auto& callee : scan.calls) {
            if (!info->safe.contains(callee)) return false;
            allocs = allocs || info->allocs.contains(callee);
        }
        if (!allocs) return false;
        std::unordered_set<std::string> declared;
        bool leaves = false, ok = true;
        auto collectExpr = [&](const Expr& e) {
            if (e.kind == ExprKind::Try) leaves = true;
            if (e.matchExpr)
                for (const auto& arm : e.matchExpr->arms)
                    for (const auto& n : arm.names) declared.insert(n);
        };
        auto collectStmt = [&](const Stmt& s) {
            if (s.kind == StmtKind::Let || s.kind == StmtKind::Var || s.kind == StmtKind::For ||
                s.kind == StmtKind::ForEach)
                declared.insert(s.name);
            if (s.kind == StmtKind::Return) leaves = true;
            for (const auto& arm : s.arms)
                for (const auto& n : arm.names) declared.insert(n);
        };
        walkAst(body, collectExpr, collectStmt);
        if (leaves && !holdsNoCells(program, fn.source.result)) return false;
        const auto outerHoldsCells = [&](const std::string& name) {
            const auto slot = findSlot(name);
            return slot && !holdsNoCells(program, fn.slots.at(*slot));
        };
        auto checkExpr = [&](const Expr& e) {
            if (e.kind != ExprKind::Name || holdsNoCells(program, e.type)) return;
            if (!declared.contains(e.text) || outerHoldsCells(e.text)) ok = false;
        };
        auto checkStmt = [&](const Stmt& s) {
            if (s.kind == StmtKind::Assign || s.kind == StmtKind::IndexSet || s.kind == StmtKind::FieldSet ||
                s.kind == StmtKind::PlaceSet) {
                if (outerHoldsCells(s.name)) ok = false;
            }
        };
        walkAst(body, checkExpr, checkStmt);
        return ok;
    }
    void nested(const std::vector<Stmt>& statements, bool loopBody = false) {
        scopes.emplace_back();
        if (loopBody && cellRegionFits(statements)) {
            scopes.back().cellMark = emit(Op::Builtin, Type::Int, {}, "cellEnter", 0, statements.front().span);
            scopes.back().cellLoopDepth = loopTargets.size();
        }
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
    void beginLoopBody(const Stmt& loop) {
        if (const auto pending = pendingAccumulations.find(&loop); pending != pendingAccumulations.end()) {
            if (accumulationProven(pending->second)) provenAccumulations.insert(pending->second.update);
        }
        if (const auto pending = pendingFullWrites.find(&loop); pending != pendingFullWrites.end())
            pending->second.elements = range(*pending->second.value);
    }
    void endLoopBody(const Stmt& loop) {
        if (const auto pending = pendingAccumulations.find(&loop); pending != pendingAccumulations.end()) {
            provenAccumulations.erase(pending->second.update);
            pendingAccumulations.erase(pending);
        }
    }
    void trackFullWrite(const Stmt& loop, unsigned array, const Expr* value) {
        pendingFullWrites.emplace(&loop, PendingFullWrite{array, value, {}});
    }
    void finishFullWrite(const Stmt& loop) {
        const auto pending = pendingFullWrites.find(&loop);
        if (pending == pendingFullWrites.end()) return;
        if (pending->second.elements)
            arrayElementRanges.insert_or_assign(pending->second.array, *pending->second.elements);
        pendingFullWrites.erase(pending);
    }
    static bool hasTargetedContinueInExpr(const Expr& e, const std::string& label, bool innermost) {
        if (e.matchExpr) {
            if (hasTargetedContinueInExpr(e.matchExpr->scrutinee, label, innermost)) return true;
            for (const auto& arm : e.matchExpr->arms) {
                if (hasTargetedContinue(arm.body, label, innermost)) return true;
                if (arm.resultExpr && hasTargetedContinueInExpr(*arm.resultExpr, label, innermost)) return true;
            }
            return false;
        }
        if (e.ifExpr) {
            if (hasTargetedContinueInExpr(e.ifExpr->condition, label, innermost)) return true;
            if (hasTargetedContinue(e.ifExpr->thenBody, label, innermost)) return true;
            if (e.ifExpr->thenResult && hasTargetedContinueInExpr(*e.ifExpr->thenResult, label, innermost)) return true;
            if (hasTargetedContinue(e.ifExpr->elseBody, label, innermost)) return true;
            if (e.ifExpr->elseResult && hasTargetedContinueInExpr(*e.ifExpr->elseResult, label, innermost)) return true;
            return false;
        }
        for (const auto& arg : e.args)
            if (hasTargetedContinueInExpr(arg, label, innermost)) return true;
        return false;
    }

    static bool hasTargetedContinue(const std::vector<Stmt>& body, const std::string& label, bool innermost = true) {
        for (const auto& s : body) {
            if (s.kind == StmtKind::Continue) {
                if (s.label.empty() ? innermost : (s.label == label)) return true;
            }
            for (const auto& e : s.exprs) {
                if (hasTargetedContinueInExpr(e, label, innermost)) return true;
            }
            if (s.kind == StmtKind::While || s.kind == StmtKind::For || s.kind == StmtKind::ForEach) {
                if (!label.empty() && hasTargetedContinue(s.body, label, false)) return true;
                continue;
            }
            if (hasTargetedContinue(s.body, label, innermost)) return true;
            if (hasTargetedContinue(s.otherwise, label, innermost)) return true;
            for (const auto& arm : s.arms)
                if (hasTargetedContinue(arm.body, label, innermost)) return true;
        }
        return false;
    }

    const LoopTarget& findLoopTarget(const std::string& label) const {
        if (label.empty()) return loopTargets.back();
        for (auto it = loopTargets.rbegin(); it != loopTargets.rend(); ++it) {
            if (it->label == label) return *it;
        }
        throw std::logic_error("unresolved loop label in checked program");
    }

    void unwindScopes(std::size_t targetScopeCount) {
        for (std::size_t k = scopes.size(); k > targetScopeCount; --k)
            release(scopes[k - 1]);
    }

    // ---- Adelanto de caché en bucles de mapa ----------------------------------------------------
    // En `for i in a..b { ... m[clave(i)] ... }` las búsquedas de vueltas distintas son independientes,
    // pero cada una espera a que la memoria traiga su trozo de tabla. El compilador sabe qué es un mapa:
    // al empezar cada vuelta calcula la clave de unas vueltas más adelante y pide ya sus líneas de caché.
    // La clave adelantada se evalúa con aritmética envolvente y sin comprobaciones (solo literales,
    // variables enteras y + - *), y el runtime descarta un corte fuera de rango: nada puede fallar por
    // adelantar, aunque esa vuelta no llegue a ejecutarse.
    static constexpr int kMapLookahead = 8;
    bool wrapSafeInt(const Expr& e) const {
        if (e.kind == ExprKind::Integer) return true;
        if (e.kind == ExprKind::Name) return e.type == Type::Int && findSlot(e.text).has_value();
        if (e.kind == ExprKind::Binary && e.args.size() == 2 && (e.text == "+" || e.text == "-" || e.text == "*"))
            return e.type == Type::Int && wrapSafeInt(e.args[0]) && wrapSafeInt(e.args[1]);
        return false;
    }
    Value wrapEval(const Expr& e) {
        if (e.kind != ExprKind::Binary) return expression(e);
        const Value a = wrapEval(e.args[0]);
        const Value b = wrapEval(e.args[1]);
        const char* op = e.text == "+" ? "wrappingAdd" : e.text == "-" ? "wrappingSub" : "wrappingMul";
        return emit(Op::Builtin, Type::Int, {a, b}, op, 0, e.span);
    }
    static bool mentionsName(const Expr& e, const std::string& name) {
        if (e.kind == ExprKind::Name && e.text == name) return true;
        for (const auto& a : e.args)
            if (mentionsName(a, name)) return true;
        return false;
    }
    const Expr* lookaheadTarget(const Stmt& loop) const {
        const Expr* found = nullptr;
        auto onExpr = [&](const Expr& e) {
            if (found || e.kind != ExprKind::Call || e.args.size() < 2) return;
            if (e.text != "mapSet" && e.text != "mapGet" && e.text != "mapHas") return;
            if (findUserFunction(e.text)) return;
            const Expr& m = e.args[0];
            if (m.kind != ExprKind::Name || !program.isMap(m.type) || !findSlot(m.text)) return;
            const Expr& key = e.args[1];
            if (!mentionsName(key, loop.name)) return;
            const Type kt = program.mapKey(m.type);
            if (kt == Type::Int && key.type == Type::Int && wrapSafeInt(key)) {
                found = &e;
            } else if (kt == Type::Str && key.kind == ExprKind::Slice && key.args.size() == 3 &&
                       key.args[0].kind == ExprKind::Name && key.args[0].type == Type::Str &&
                       findSlot(key.args[0].text) && wrapSafeInt(key.args[1]) && wrapSafeInt(key.args[2])) {
                found = &e;
            }
        };
        auto onStmt = [](const Stmt&) {};
        for (const auto& st : loop.body) {
            if (st.kind == StmtKind::While || st.kind == StmtKind::For || st.kind == StmtKind::ForEach) continue;
            for (const auto& e : st.exprs) walkAst(e, onExpr, onStmt);
        }
        return found;
    }
    void emitMapLookahead(const Stmt& loop, unsigned iterator) {
        if (envEnabled("VELOZ_NO_MAP_LOOKAHEAD")) return;
        const Expr* access = lookaheadTarget(loop);
        if (!access) return;
        const Expr& m = access->args[0];
        const Expr& key = access->args[1];
        const Value table = load(lookup(m.text));
        const unsigned ahead = allocate(Type::Int);
        const Value distance = emit(Op::Int, Type::Int, {}, std::to_string(kMapLookahead));
        store(ahead, emit(Op::Builtin, Type::Int, {load(iterator), distance}, "wrappingAdd"));
        scopes.emplace_back();
        scopes.back().names.emplace(loop.name, ahead);
        if (key.kind == ExprKind::Slice) {
            const Value base = load(lookup(key.args[0].text));
            const Value lo = wrapEval(key.args[1]);
            const Value hi = wrapEval(key.args[2]);
            emit(Op::Builtin, Type::Void, {table, base, lo, hi}, "mapPrefetchStr");
        } else {
            emit(Op::Builtin, Type::Void, {table, wrapEval(key)}, "mapPrefetchInt");
        }
        scopes.pop_back();
    }

    void loop(const Stmt& s) {
        std::unordered_set<std::string> mutated;
        collectMutRootsInBlock(s.body, mutated);
        for (const auto& name : mutated) {
            for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
                if (const auto found = scope->names.find(name); found != scope->names.end()) {
                    arrayElementRanges.erase(found->second);
                    break;
                }
            }
        }
        const bool counting = s.kind == StmtKind::For;
        const std::size_t breakScopeCount = scopes.size();
        const std::size_t loopTempCount = temporaries.size();
        std::optional<unsigned> iterator;
        std::optional<IntRange> iteratorValues;
        std::optional<Linear> lowerProof, upperProof;
        Value upper{};
        if (counting) {
            iteratorValues = iteratorRange(s);
            lowerProof = linear(s.exprs[0]);
            upperProof = linear(s.exprs[1]);
            const auto lower = expression(s.exprs[0]);
            upper = expression(s.exprs[1]);
            dropTemporaries();
            scopes.emplace_back();
            iterator = (s.name == "_") ? allocate(Type::Int) : declare(s.name, Type::Int);
            store(*iterator, lower);
            if (iteratorValues) iteratorRanges.insert_or_assign(*iterator, *iteratorValues);
        }
        const std::size_t continueScopeCount = scopes.size();
        const auto header = block();
        const auto body = block();
        const bool needStepBlk = counting && hasTargetedContinue(s.body, s.label);
        const std::optional<unsigned> stepBlk = needStepBlk ? std::optional<unsigned>(block()) : std::nullopt;
        const auto end = block();
        jump(header);
        here = header;
        const auto condition = counting
            ? emit(Op::Binary, Type::Bool, {load(*iterator), upper}, "<")
            : expression(s.exprs[0]);
        dropTemporaries();
        branch(condition, body, end);
        here = body;
        const auto saved = validIndices.size();
        const auto savedRanges = rangeProofs.size();
        if (counting && lowerProof && upperProof)
            rangeProofs.push_back({*iterator, std::move(*lowerProof), std::move(*upperProof)});
        if (counting && s.exprs[0].kind == ExprKind::Integer && s.exprs[0].text == "0") {
            const auto& bound = s.exprs[1];
            // The bounds were evaluated before the iterator was declared.
            // A shadowing iterator can only make this conservative.
            if (bound.kind == ExprKind::Name && bound.text != s.name) {
                const auto [first,last] = arraysWithLength.equal_range(lookup(bound.text));
                for (auto it = first; it != last; ++it) validIndices.emplace_back(it->second, *iterator);
            } else if (bound.kind == ExprKind::Call && bound.text == "len" &&
                       bound.args[0].kind == ExprKind::Name && bound.args[0].type == Type::IntArray &&
                       bound.args[0].text != s.name && !reassigned.contains(bound.args[0].text)) {
                validIndices.emplace_back(lookup(bound.args[0].text), *iterator);
            }
        }
        loopTargets.push_back({s.label, stepBlk ? *stepBlk : header, end, breakScopeCount, continueScopeCount, loopTempCount});
        beginLoopBody(s);
        if (counting && s.name != "_") emitMapLookahead(s, *iterator);
        nested(s.body, true);
        endLoopBody(s);
        loopTargets.pop_back();
        validIndices.resize(saved);
        rangeProofs.resize(savedRanges);
        if (stepBlk) {
            jump(*stepBlk);
            here = *stepBlk;
        }
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
    void forEachLoop(const Stmt& s) {
        std::unordered_set<std::string> mutated;
        collectMutRootsInBlock(s.body, mutated);

        bool ownScope = false;
        unsigned arrSlot = 0;
        const auto& collExpr = s.exprs.at(0);
        if (collExpr.kind == ExprKind::Name && !mutated.contains(collExpr.text)) {
            arrSlot = lookup(collExpr.text);
        } else {
            const auto collVal = own(expression(collExpr), collExpr.span);
            dropTemporaries();
            scopes.emplace_back();
            ownScope = true;
            arrSlot = allocate(collVal.type);
            store(arrSlot, collVal);
            if (needsDrop(collVal.type) && !collVal.freshRegion) scopes.back().arrays.push_back(arrSlot);
        }
        const std::size_t breakScopeCount = scopes.size();

        const auto upper = emit(Op::Length, Type::Int, {load(arrSlot)});
        scopes.emplace_back();
        const unsigned iter = allocate(Type::Int);
        store(iter, emit(Op::Int, Type::Int, {}, "0"));
        const std::size_t continueScopeCount = scopes.size();
        const std::size_t loopTempCount = temporaries.size();

        const auto header = block();
        const auto body = block();
        const bool needStepBlk = hasTargetedContinue(s.body, s.label);
        const std::optional<unsigned> stepBlk = needStepBlk ? std::optional<unsigned>(block()) : std::nullopt;
        const auto end = block();
        jump(header);
        here = header;
        const auto condition = emit(Op::Binary, Type::Bool, {load(iter), upper}, "<");
        branch(condition, body, end);

        here = body;
        scopes.emplace_back();
        if (s.name != "_") {
            const Type elemType = program.elemOf(fn.slots.at(arrSlot));
            const auto elem = emit(Op::Index, elemType, {load(arrSlot), load(iter)}, {}, 0, s.span);
            fn.blocks[here].ops.back().boundsProven = true;
            fn.blocks[here].ops.back().proofKind = ProofKind::LoopBound;
            store(declare(s.name, elemType), elem);
        }
        loopTargets.push_back({s.label, stepBlk ? *stepBlk : header, end, breakScopeCount, continueScopeCount, loopTempCount});
        nested(s.body, true);
        loopTargets.pop_back();
        if (fn.blocks[here].exit == Exit::Open) release(scopes.back());
        scopes.pop_back();
        if (stepBlk) {
            jump(*stepBlk);
            here = *stepBlk;
        }
        if (fn.blocks[here].exit == Exit::Open) {
            const auto n = load(iter);
            const auto one = emit(Op::Int, Type::Int, {}, "1");
            store(iter, emit(Op::Binary, Type::Int, {n, one}, "rangeIncrement"));
        }
        jump(header);
        scopes.pop_back();
        here = end;
        if (ownScope) {
            release(scopes.back());
            scopes.pop_back();
        }
    }
    std::optional<Value> matchImpl(const Expr& scrutinee, const std::vector<MatchArm>& arms,
                                   bool isExpr, Type exprType) {
        const std::size_t prevFloor = tempFloor;
        if (isExpr) tempFloor = temporaries.size();
        // Si lo examinado sale de un sitio que no cambia durante el match, las ramas lo toman
        // prestado: no se copia ni se cuentan referencias. La variable de la que cuelga sigue viva
        // y sin cambios hasta que el match termina.
        const bool borrowed = stablePlace(scrutinee);
        auto candRaw = borrowed && borrowsCell(scrutinee) ? sharedPeek(scrutinee) : expression(scrutinee);
        const bool ownCand = needsDrop(candRaw.type) && !(borrowed && !candRaw.owner);
        Value candidate = candRaw;
        unsigned candSlot = 0;
        if (ownCand) {
            scopes.emplace_back();
            candSlot = allocate(candRaw.type);
            store(candSlot, own(candRaw, scrutinee.span));
            scopes.back().arrays.push_back(candSlot);
            candidate = load(candSlot);
        }
        dropTemporaries();
        const auto& decl = *program.declaration(candidate.type);
        const auto tag = emit(Op::Tag, Type::Int, {candidate});
        const auto dispatch = here;

        bool hasWildcard = false;
        for (const auto& arm : arms) {
            if (arm.variant == "_") { hasWildcard = true; break; }
        }
        const unsigned fallbackBlk = block();
        if (!hasWildcard) fn.blocks[fallbackBlk].exit = Exit::Unreachable;
        const auto end = block();
        fn.blocks[dispatch].exit = Exit::Switch;
        fn.blocks[dispatch].value = tag;
        fn.blocks[dispatch].yes = fallbackBlk;
        fn.blocks[dispatch].targets.resize(decl.variants.size(), fallbackBlk);

        const std::optional<unsigned> resSlot = (isExpr && exprType != Type::Void)
            ? std::optional<unsigned>(allocate(exprType))
            : std::nullopt;

        bool fallsThrough = false;
        for (const auto& arm : arms) {
            if (arm.variant == "_") {
                here = fallbackBlk;
                scopes.emplace_back();
                markRegion(arm.body);
                if (ownCand) {
                    drop(candSlot);
                    emit(Op::ClearOwner, Type::Void, {}, {}, candSlot);
                }
            } else {
                const auto variant = variantOf(decl, arm.variant);
                here = block();
                fn.blocks[dispatch].targets[variant] = here;
                scopes.emplace_back();
                markRegion(arm.body);
                const auto& fields = decl.variants[variant].fields;
                for (unsigned i = 0; i < arm.names.size(); ++i) {
                    auto v = field(candidate, fields.at(i).type, {variant + 1, i});
                    if (arm.names[i] == "_") {
                        if (ownCand && needsDrop(v.type)) {
                            const unsigned fslot = allocate(v.type);
                            store(fslot, v);
                            drop(fslot);
                        }
                    } else {
                        const unsigned fslot = declare(arm.names[i], v.type);
                        store(fslot, v);
                        if (ownCand && needsDrop(v.type)) {
                            fn.slotOwned[fslot] = true;
                            scopes.back().arrays.push_back(fslot);
                        }
                    }
                }
                if (ownCand) emit(Op::ClearOwner, Type::Void, {}, {}, candSlot);
            }
            sequence(arm.body);
            if (isExpr && fn.blocks[here].exit == Exit::Open && arm.resultExpr) {
                Value av = own(expression(*arm.resultExpr), arm.resultExpr->span);
                if (resSlot) store(*resSlot, av);
                dropTemporaries();
            }
            if (fn.blocks[here].exit == Exit::Open) release(scopes.back());
            scopes.pop_back();
            fallsThrough = fallsThrough || fn.blocks[here].exit == Exit::Open;
            jump(end);
        }
        if (isExpr) tempFloor = prevFloor;
        here = end;
        if (ownCand) {
            // Every arm transferred field ownership or dropped candSlot and cleared candSlot.
            scopes.pop_back();
        }
        if (!fallsThrough) fn.blocks[end].exit = Exit::Unreachable;
        if (!isExpr) return std::nullopt;
        if (!resSlot) return Value{exprType, nextValue++};
        Value res = load(*resSlot);
        if (fallsThrough && needsDrop(exprType)) {
            temporaries.push_back(*resSlot);
            res.owner = *resSlot;
        }
        return res;
    }
    void match(const Stmt& s) {
        (void)matchImpl(s.exprs.at(0), s.arms, false, Type::Void);
    }
    Value matchExpr(const Expr& e) {
        return *matchImpl(e.matchExpr->scrutinee, e.matchExpr->arms, true, e.type);
    }
    Value ifExprLowering(const Expr& e) {
        const auto& ie = *e.ifExpr;
        const auto cond = expression(ie.condition);
        dropTemporaries();
        const auto yes = block();
        const auto no = block();
        const auto end = block();
        branch(cond, yes, no);

        std::optional<unsigned> resSlot;
        if (e.type != Type::Void && e.type != Type::Unknown) resSlot = allocate(e.type);
        const std::size_t prevFloor = tempFloor;
        tempFloor = temporaries.size();
        bool fallsThrough = false;

        auto lowerArm = [&](unsigned blkId, const std::vector<Stmt>& body, const std::optional<Expr>& resE) {
            here = blkId;
            scopes.emplace_back();
            markRegion(body);
            sequence(body);
            if (fn.blocks[here].exit == Exit::Open && resE) {
                Value av = own(expression(*resE), resE->span);
                if (resSlot) store(*resSlot, av);
                dropTemporaries();
            }
            if (fn.blocks[here].exit == Exit::Open) release(scopes.back());
            scopes.pop_back();
            fallsThrough = fallsThrough || fn.blocks[here].exit == Exit::Open;
            jump(end);
        };

        lowerArm(yes, ie.thenBody, ie.thenResult);
        lowerArm(no, ie.elseBody, ie.elseResult);
        tempFloor = prevFloor;
        here = end;
        if (!fallsThrough) fn.blocks[end].exit = Exit::Unreachable;
        if (!resSlot) return Value{e.type, nextValue++};
        Value res = load(*resSlot);
        if (fallsThrough && needsDrop(e.type)) {
            temporaries.push_back(*resSlot);
            res.owner = *resSlot;
        }
        return res;
    }
    void statement(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Let:
        case StmtKind::Var: {
            bind(s, expression(s.exprs[0]));
            break;
        }
        case StmtKind::Assign: {
            const auto& rhs = s.exprs[0];
            const auto slot = lookup(s.name);
            if (rhs.kind == ExprKind::Name && lookup(rhs.text) == slot) break;
            const auto v = own(expression(rhs), rhs.span);
            // `acc += e` (también `-=` y `*=`) sobre un escalar flotante local es una reducción: Turi
            // permite agruparla en cualquier orden, igual que el experto reparte la suma en varios
            // acumuladores a mano. Así LLVM puede vectorizarla. El resto de operaciones conserva su orden.
            if (Program::isFloatType(v.type) && rhs.kind == ExprKind::Binary && rhs.args.size() == 2 &&
                (rhs.text == "+" || rhs.text == "-" || rhs.text == "*") &&
                rhs.args[0].kind == ExprKind::Name && rhs.args[0].text == s.name) {
                auto& ops = fn.blocks[here].ops;
                if (!ops.empty() && ops.back().op == Op::Binary && ops.back().result.id == v.id)
                    ops.back().reassoc = true;
            }
            if (needsDrop(v.type)) {
                if (v.type == Type::IntArray) arrayElementRanges.erase(slot);
                drop(slot);
            }
            store(slot, v);
            break;
        }
        case StmtKind::IndexSet: {
            const auto array = lookup(s.name);
            const auto base = load(array);
            const auto index = expression(s.exprs.at(0));
            const auto value = own(expression(s.exprs.at(1)), s.exprs.at(1).span);
            emit(Op::IndexSet, Type::Void, {base, index, value}, {}, 0, s.span);
            if (inBounds(array, s.exprs[0])) {
                fn.blocks[here].ops.back().boundsProven = true;
                fn.blocks[here].ops.back().proofKind = ProofKind::LoopBound;
            }
            if (fn.slots[array] == Type::IntArray) arrayElementRanges.erase(array);
            break;
        }
        case StmtKind::FieldSet: {
            const auto rootSlot = lookup(s.name);
            std::vector<Value> ins;
            if (s.exprs.size() == 2) {
                ins.push_back(expression(s.exprs[0]));
                ins.push_back(own(expression(s.exprs[1]), s.exprs[1].span));
            } else {
                ins.push_back(own(expression(s.exprs[0]), s.exprs[0].span));
            }
            emit(Op::FieldSet, Type::Void, std::move(ins), {}, rootSlot, s.span);
            auto& op = fn.blocks[here].ops.back();
            op.fields.assign(s.fieldIndices.begin(), s.fieldIndices.end());
            if (s.exprs.size() == 1 && !s.fieldTypes.empty() && needsDrop(s.fieldTypes.back())) {
                // Target type is carried in Op::FieldSet's slot/fields lookup in native.cpp.
            }
            break;
        }
        case StmtKind::PlaceSet: {
            const auto rootSlot = lookup(s.name);
            std::vector<Value> ins;
            ins.reserve(s.exprs.size());
            for (std::size_t k = 0; k + 1 < s.exprs.size(); ++k)
                ins.push_back(expression(s.exprs[k]));
            ins.push_back(own(expression(s.exprs.back()), s.exprs.back().span));
            emit(Op::PlaceSet, Type::Void, std::move(ins), {}, rootSlot, s.span);
            auto& op = fn.blocks[here].ops.back();
            op.placeSteps.reserve(s.placeSteps.size());
            for (const auto& st : s.placeSteps)
                op.placeSteps.push_back({st.isIndex, static_cast<unsigned>(st.fieldIdx), 0u});
            if (fn.slots[rootSlot] == Type::IntArray) arrayElementRanges.erase(rootSlot);
            break;
        }
        case StmtKind::Call: expression(s.exprs[0]); break;
        case StmtKind::If: ifStatement(s); break;
        case StmtKind::SafetyBlock: {
            const SafetyTier savedSafety = curSafety;
            curSafety = s.safety;
            nested(s.body);
            curSafety = savedSafety;
            break;
        }
        case StmtKind::While:
        case StmtKind::For: loop(s); break;
        case StmtKind::ForEach: forEachLoop(s); break;
        case StmtKind::Break: {
            const auto& lt = findLoopTarget(s.label);
            emitTemporaryDropsDownTo(lt.tempCount);
            unwindScopes(lt.breakScopeCount);
            jump(lt.breakBlk);
            break;
        }
        case StmtKind::Continue: {
            const auto& lt = findLoopTarget(s.label);
            emitTemporaryDropsDownTo(lt.tempCount);
            unwindScopes(lt.continueScopeCount);
            jump(lt.continueBlk);
            break;
        }
        case StmtKind::Match: match(s); break;
        case StmtKind::Return: {
            std::optional<Value> v;
            if (!s.exprs.empty()) v = returned(s.exprs[0]);
            emitEnsures(v);
            cleanupReturn();
            movedSlot.reset();
            fn.blocks[here].exit = Exit::Return;
            fn.blocks[here].value = v;
            break;
        }
        case StmtKind::Assert: emit(Op::Assert, Type::Void, {expression(s.exprs[0])}, {}, 0, s.span); break;
        case StmtKind::Print: emit(Op::Print, Type::Void, {expression(s.exprs[0])}, {}, 0, s.span); break;
        }
    }
    void emitContractExpr(const Expr& e, const std::string& kind) {
        if (e.kind == ExprKind::Binary && e.text == "&&") {
            emitContractExpr(e.args.at(0), kind);
            emitContractExpr(e.args.at(1), kind);
            return;
        }
        const std::size_t savedTempFloor = tempFloor;
        tempFloor = temporaries.size();
        const Value cond = expression(e);
        emit(Op::Assert, Type::Void, {cond}, kind, 0, e.span);
        auto& op = fn.blocks[here].ops.back();
        if (fn.source.safety == SafetyTier::Raw) {
            op.unchecked = true;
            op.proofKind = ProofKind::RawUnchecked;
        }
        dropTemporaries();
        tempFloor = savedTempFloor;
    }
    void emitEnsures(std::optional<Value>& retVal) {
        if (fn.source.ensuresClauses.empty()) return;
        std::optional<unsigned> retSlot;
        if (retVal) {
            retSlot = allocate(fn.source.result);
            store(*retSlot, *retVal);
        }
        auto savedScopes = std::move(scopes);
        scopes.clear();
        scopes.emplace_back();
        scopes.back().names = paramSlots;
        if (retSlot) scopes.back().names.emplace("result", *retSlot);
        for (const auto& ens : fn.source.ensuresClauses) emitContractExpr(ens, "ensures");
        scopes = std::move(savedScopes);
        if (retSlot) retVal = load(*retSlot);
    }
public:
    Lowerer(const Program& program, const Function& f, const ReturnRanges& returns)
        : program(program), returns(returns), fn{f, {}, {Block{}}}, curSafety(f.safety) {}
    FunctionIR lower() {
        findAssignments(fn.source.body);
        for (const auto& p : fn.source.params) {
            const unsigned slot = declare(p.name, p.type);
            paramSlots[p.name] = slot;
            if (p.byRef) {
                fn.slotByRef[slot] = true;
                reassigned.insert(p.name);
            } else if (p.owned && needsDrop(p.type)) {
                fn.slotOwned[slot] = true;
                scopes.back().arrays.push_back(slot);
            }
        }
        for (const auto& req : fn.source.requiresClauses) emitContractExpr(req, "requires");
        markRegion(fn.source.body);
        sequence(fn.source.body);
        if (fn.blocks[here].exit == Exit::Open) {
            if (fn.source.result == Type::Void) {
                std::optional<Value> noRet;
                emitEnsures(noRet);
            }
            cleanupReturn();
            fn.blocks[here].exit = fn.source.result == Type::Void ? Exit::Return : Exit::Unreachable;
        }
        fn.nextValue = nextValue;
        return std::move(fn);
    }
};

} // namespace veloz::native
