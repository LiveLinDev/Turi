#include "commands.hpp"
#include "native_ir.hpp"
#include "native_opt.hpp"
#include "project.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace veloz {
namespace {
std::string fingerprint(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ull; }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
std::string docLine(std::string_view source, Span span) {
    auto at = source.rfind('\n', span.start);
    if (at == source.npos || !at) return {};
    auto before = source.rfind('\n', at - 1);
    std::string_view previous = source.substr(before == source.npos ? 0 : before+1,
                                              before == source.npos ? at : at-before-1);
    auto i = previous.find_first_not_of(" \t");
    if (i == previous.npos || previous.compare(i, 2, "//")) return {};
    i = previous.find_first_not_of("/ \t", i);
    if (i == previous.npos) return {};
    return std::string(previous.substr(i, previous.find_last_not_of(" \r\t") - i + 1));
}
template<class Statement, class Expression>
void walk(const std::vector<Stmt>& body, Statement& visit, Expression& seeExpr) {
    auto expression = [&](auto& self, const Expr& e) -> void {
        seeExpr(e);
        for (const auto& arg : e.args) self(self, arg);
        if (e.closure) walk(e.closure->body, visit, seeExpr);
        if (e.matchExpr) {
            self(self, e.matchExpr->scrutinee);
            for (const auto& arm : e.matchExpr->arms) {
                walk(arm.body, visit, seeExpr);
                if (arm.resultExpr) self(self, *arm.resultExpr);
            }
        }
        if (e.ifExpr) {
            self(self, e.ifExpr->condition);
            walk(e.ifExpr->thenBody, visit, seeExpr);
            if (e.ifExpr->thenResult) self(self, *e.ifExpr->thenResult);
            walk(e.ifExpr->elseBody, visit, seeExpr);
            if (e.ifExpr->elseResult) self(self, *e.ifExpr->elseResult);
        }
    };
    for (const auto& stmt : body) {
        visit(stmt);
        for (const auto& e : stmt.exprs) expression(expression, e);
        walk(stmt.body, visit, seeExpr);
        walk(stmt.otherwise, visit, seeExpr);
        for (const auto& arm : stmt.arms) walk(arm.body, visit, seeExpr);
    }
}
struct Effects { bool io = false, alloc = false, trap = false; std::set<std::string> calls; };
bool isBuiltinFn(const std::string& n) {
    static constexpr std::array builtins = {
        "len", "cap", "push", "pop", "clear", "truncate", "reserve",
        "repeat", "readByte", "writeByte", "write", "ewrite", "eprint", "flush",
        "readFile", "writeFile", "appendFile", "tryReadFile", "tryWriteFile", "fileExists", "removeFile", "makeDir",
        "fileOpen", "fileRead", "fileReadAll", "fileReadLine", "fileWrite", "fileFlush", "fileClose", "fileIsOpen",
        "tcpListen", "tcpLocalPort", "tcpAccept", "tcpConnect", "socketRead", "socketWrite", "socketClose", "socketIsOpen",
        "listenerClose", "listenerIsOpen",
        "mutexNew", "mutexLock", "mutexTryLock", "unlock", "lockIsHeld",
        "taskScope", "taskSpawn", "taskSpawnMove", "taskJoin", "parallelMap",
        "freeze", "frozenGet", "frozenRead",
        "atomicNew", "atomicLoad", "atomicLoadOrd", "atomicStore", "atomicStoreOrd",
        "atomicSwap", "atomicSwapOrd", "atomicCas", "atomicCasOrd",
        "atomicAdd", "atomicAddOrd", "atomicSub", "atomicSubOrd",
        "atomicAnd", "atomicAndOrd", "atomicOr", "atomicOrOrd", "atomicXor", "atomicXorOrd",
        "atomicFence",
        "ringNew", "ringPush", "ringPop", "ringLen", "ringCap",
        "regionNew", "regionAlloc", "regionAllocAligned", "regionReset", "regionUsed", "regionCapacity",
        "argsCount", "argAt", "envGet", "clockNs", "sleepMs", "execCapture", "execStatus",
        "int", "float", "str", "strFromByte", "parseInt", "parseFloat", "tryParseInt", "tryParseFloat",
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "sizeOf", "alignOf", "bitcast",
        "ptrNull", "mutPtrNull", "ptrIsNull", "ptrAddr", "ptrConst", "ptrMut", "ptrOf", "mutPtrOf",
        "slicePtr", "mutSlicePtr", "ptrCast", "mutPtrCast", "ptrFromAddr", "mutPtrFromAddr",
        "ptrAdd", "ptrRead", "ptrWrite", "ptrAlloc", "ptrAllocAligned", "ptrFree",
        "ptrCopy", "ptrZero", "cstr", "strFromCStr", "strFromPtr", "fnPtr",
        "simdSplat", "simdLanes", "simdGet", "simdSet", "simdFromArray", "simdToArray",
        "simdLoad", "simdLoadAligned", "simdStore", "simdStoreAligned",
        "simdFma", "simdMin", "simdMax", "simdAbs", "simdSqrt",
        "simdSum", "simdReduceMin", "simdReduceMax",
        "simdCmpEq", "simdCmpLt", "simdCmpLe", "simdCmpGt", "simdCmpGe", "simdSelect",
        "clz", "ctz", "popcnt", "bswap", "rotl", "rotr",
        "prefetchRead", "prefetchWrite", "spinHint",
        "strContains", "strStartsWith", "strEndsWith", "strIndexOf",
        "sbNew", "sbWithCap", "sbPush", "sbPushByte", "sbPushInt", "sbPushFloat", "sbClear", "sbStr", "sbTake",
        "mapNew", "mapWithCap", "mapSecure", "mapSet", "mapGet", "mapHas", "mapDel", "mapKeys", "mapValues",
        "setNew", "setWithCap", "setSecure", "setAdd", "setHas", "setDel", "setItems",
        "poolNew", "poolWithCap", "poolAdd", "poolGet", "poolSet", "poolHas", "poolRemove", "poolLen", "poolHandles",
        "sharedNew", "sharedGet", "sharedSet", "sharedUpdate", "sharedRefs",
        "sort", "sortStable",
        "isSome", "isNone", "isOk", "isErr", "unwrapOr",
        "wrappingAdd", "wrappingSub", "wrappingMul",
        "saturatingAdd", "saturatingSub", "saturatingMul",
        "abs", "min", "max", "sqrt", "floor", "ceil", "round", "sin", "cos", "pow",
        "isNaN", "isInf"
    };
    return std::find(builtins.begin(), builtins.end(), n) != builtins.end();
}
template<std::size_t N>
bool oneOfWord(std::string_view word, const std::array<std::string_view, N>& list) {
    return std::find(list.begin(), list.end(), word) != list.end();
}
Effects astEffects(const Program& p, const std::vector<Stmt>& body, const std::string& mod) {
    Effects fx;
    auto visit = [&](const Stmt& s) {
        fx.io |= s.kind == StmtKind::Print;
        bool placeTraps = s.kind == StmtKind::Assert || s.kind == StmtKind::IndexSet ||
                          (s.kind == StmtKind::FieldSet && s.exprs.size() >= 2);
        if (s.kind == StmtKind::PlaceSet) {
            for (const auto& st : s.placeSteps)
                if (st.isIndex) { placeTraps = true; break; }
        }
        fx.trap |= placeTraps;
    };
    auto expr = [&](const Expr& e) {
        static constexpr auto ioBuiltins = std::to_array<std::string_view>({
            "readByte", "writeByte", "write", "ewrite", "eprint", "flush",
            "readFile", "writeFile", "appendFile", "tryReadFile", "tryWriteFile", "fileExists", "removeFile", "makeDir",
            "execCapture", "execStatus", "envGet", "sleepMs", "clockNs", "argsCount", "argAt",
            "fileOpen", "fileRead", "fileReadAll", "fileReadLine", "fileWrite", "fileFlush", "fileClose",
            "tcpListen", "tcpLocalPort", "tcpAccept", "tcpConnect", "socketRead", "socketWrite",
            "socketClose", "listenerClose", "mapSecure", "setSecure"
        });
        static constexpr auto allocBuiltins = std::to_array<std::string_view>({
            "push", "reserve", "repeat", "str", "readFile", "tryReadFile", "tryWriteFile", "argAt", "envGet", "execCapture",
            "sbWithCap", "sbPush", "sbPushByte", "sbPushInt", "sbPushFloat", "sbStr",
            "mapWithCap", "mapSet", "mapKeys", "mapValues", "setWithCap", "setAdd", "setItems",
            "poolWithCap", "poolAdd", "poolHandles", "sharedNew", "sharedSet", "sharedUpdate",
            "sortStable", "fileOpen", "fileRead", "fileReadAll", "fileReadLine", "fileWrite", "fileFlush",
            "tcpListen", "tcpAccept", "tcpConnect", "socketRead", "socketWrite",
            "mutexNew", "mutexLock", "mutexTryLock",
            "taskScope", "taskSpawn", "taskSpawnMove", "parallelMap", "freeze", "frozenGet",
            "atomicNew", "ringNew", "regionNew", "regionAlloc", "regionAllocAligned",
            "ptrAlloc", "ptrAllocAligned", "cstr", "strFromCStr", "strFromPtr"
        });
        static constexpr auto trapBuiltins = std::to_array<std::string_view>({
            "int", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
            "abs", "pop", "push", "reserve", "repeat", "str", "parseInt", "parseFloat",
            "readFile", "tryReadFile", "argAt", "envGet", "execCapture",
            "sbWithCap", "sbPush", "sbPushByte", "sbPushInt", "sbPushFloat", "sbStr",
            "mapWithCap", "mapSet", "mapKeys", "mapValues", "setWithCap", "setAdd", "setItems",
            "poolWithCap", "poolAdd", "poolHandles", "sharedNew", "sharedSet", "sharedUpdate",
            "sortStable", "fileRead", "fileReadAll", "fileReadLine", "socketRead",
            "mutexNew", "mutexLock", "mutexTryLock",
            "taskScope", "taskSpawn", "taskSpawnMove", "parallelMap", "freeze", "frozenGet", "frozenRead",
            "atomicNew", "ringNew", "regionNew", "regionAlloc", "regionAllocAligned",
            "ptrAlloc", "ptrAllocAligned", "ptrCopy", "ptrZero", "cstr", "strFromCStr", "strFromPtr",
            "ptrAdd", "ptrRead", "ptrWrite",
            "simdGet", "simdSet", "simdFromArray", "simdToArray",
            "simdLoad", "simdLoadAligned", "simdStore", "simdStoreAligned"
        });
        fx.trap |= e.kind == ExprKind::Index || e.kind == ExprKind::Slice || e.kind == ExprKind::Interp ||
                   e.kind == ExprKind::BoxDyn || (e.kind == ExprKind::ArrayLit && !e.args.empty());
        if (e.kind == ExprKind::Call) {
            if (oneOfWord(e.text, ioBuiltins)) fx.io = true;
            if (oneOfWord(e.text, allocBuiltins)) fx.alloc = true;
            if (oneOfWord(e.text, trapBuiltins)) fx.trap = true;
            if (e.text == "taskSpawn" || e.text == "taskSpawnMove" ||
                e.text == "parallelMap" || e.text == "frozenRead") {
                const std::size_t fnIdx = (e.text == "taskSpawnMove") ? 2u : 1u;
                if (fnIdx < e.args.size()) {
                    const Expr& fnArg = e.args[fnIdx];
                    if (fnArg.closure && fnArg.closure->id < p.closures.size() &&
                        !p.closures[fnArg.closure->id].fnName.empty()) {
                        fx.calls.insert(p.closures[fnArg.closure->id].fnName);
                    } else if (fnArg.type != Type::Unknown) {
                        for (const auto& cl : p.closures) {
                            if (cl.fnType == fnArg.type && !cl.fnName.empty()) fx.calls.insert(cl.fnName);
                        }
                    }
                }
            }
            if (!isBuiltinFn(e.text)) {
                const auto callee = e.text.find('.') == std::string::npos && !mod.empty()
                    ? mod + "." + e.text : e.text;
                fx.calls.insert(callee);
                fx.trap = true;
            }
        }
        if (e.kind == ExprKind::ArrayLit && !e.args.empty()) fx.alloc = true;
        if (e.kind == ExprKind::Slice || e.kind == ExprKind::Interp || e.kind == ExprKind::BoxDyn) fx.alloc = true;
        if (e.kind == ExprKind::MethodCall || e.kind == ExprKind::AssocCall || e.kind == ExprKind::Closure) {
            fx.trap = true;
        }
        if (e.kind == ExprKind::ClosureCall) {
            fx.trap = true;
            if (!e.args.empty()) {
                const Type fnT = e.args[0].type;
                for (const auto& cl : p.closures) {
                    if (cl.fnType == fnT && !cl.fnName.empty()) fx.calls.insert(cl.fnName);
                }
            }
        }
        if (e.kind == ExprKind::DynCall) {
            fx.trap = true;
            if (!e.fieldIndices.empty()) {
                const auto idx = static_cast<std::size_t>(e.fieldIndices[0]);
                for (const auto& dv : p.dynVtables) {
                    if (dv.iface == e.assocType && idx < dv.methods.size() && !dv.methods[idx].empty()) {
                        fx.calls.insert(dv.methods[idx]);
                    }
                }
            }
        }
        if (e.kind == ExprKind::Binary &&
            (e.text == "+" || e.text == "-" || e.text == "*" || e.text == "/" || e.text == "%" ||
             e.text == "<<" || e.text == ">>" || e.text == ">>>")) {
            fx.trap = true;
            if (e.text == "+" && e.type == Type::Str) fx.alloc = true;
        }
        if (e.kind == ExprKind::Unary && e.text == "-") fx.trap = true;
    };
    walk(body, visit, expr);
    return fx;
}
std::map<std::string, Effects> effects(const Program& p) {
    std::map<std::string, Effects> result;
    for (const auto& f : p.functions) {
        if (f.isTest) continue;
        if (f.isExternDecl) {
            result[f.name] = Effects{true, true, true, {}};
            continue;
        }
        result[f.name] = astEffects(p, f.body, f.module);
    }
    bool changed;
    do {
        changed = false;
        for (auto& [_, fx] : result) {
            for (const auto& callee : fx.calls) {
                const auto found = result.find(callee);
                if (found == result.end()) continue;
                const auto& other = found->second;
                if (!fx.io && other.io) { fx.io = true; changed = true; }
                if (!fx.alloc && other.alloc) { fx.alloc = true; changed = true; }
                if (!fx.trap && other.trap) { fx.trap = true; changed = true; }
            }
        }
    } while (changed);
    return result;
}

std::string extractValueHint(const Program& program, Span span, std::string_view fallback);

struct IrEffects {
    bool io = false, alloc = false, trap = false, clone = false;
    Span ioSpan{}, allocSpan{}, trapSpan{}, cloneSpan{};
    std::string ioReason, allocReason, trapReason, cloneReason;
};

struct EffectEdge {
    std::size_t to = 0;
    Span callSpan{};
    std::string display;
};

std::string fnDisplayName(const Function& f) {
    if (f.isImpl) {
        if (f.name.starts_with("@closure.")) return "closure";
        return f.sourceName.empty() ? f.name : f.sourceName;
    }
    return f.name;
}

std::map<std::string, IrEffects> analyzeProgramEffects(const Program& checked) {
    std::vector<native::FunctionIR> functions;
    const native::ReturnRanges returnRanges(checked);
    for (const auto& f : checked.functions) {
        if (!f.isTest && !f.isExternDecl) functions.push_back(native::Lowerer(checked, f, returnRanges).lower());
    }
    auto safe = native::safeDepths(checked, functions);
    std::deque<Function> closureSpecStorage;
    native::optimizeIR(functions, safe, checked.entryMain, true, nullptr, &checked, &closureSpecStorage);
    safe = native::safeDepths(checked, functions);

    const std::size_t n = functions.size();
    std::unordered_map<std::string, std::size_t> fnIndex;
    fnIndex.reserve(n);
    for (std::size_t i = 0; i < n; ++i) fnIndex.emplace(functions[i].source.name, i);

    std::vector<IrEffects> info(n);
    std::vector<std::vector<EffectEdge>> edges(n);

    static constexpr auto ioOnlyBuiltins = std::to_array<std::string_view>({
        "writeByte", "write", "ewrite", "eprint", "flush",
        "writeFile", "appendFile", "fileExists", "removeFile", "makeDir",
        "fileClose", "listenerClose", "socketClose",
        "tcpLocalPort", "argsCount", "clockNs", "sleepMs", "execStatus",
        "mapSecure", "setSecure"
    });
    static constexpr auto ioAllocTrapBuiltins = std::to_array<std::string_view>({
        "readFile", "tryReadFile", "tryWriteFile", "argAt", "envGet", "execCapture",
        "fileOpen", "fileRead", "fileReadAll", "fileReadLine", "fileWrite", "fileFlush",
        "tcpListen", "tcpAccept", "tcpConnect", "socketRead", "socketWrite"
    });
    static constexpr auto trapOnlyBuiltins = std::to_array<std::string_view>({
        "intFromFloat", "pop", "parseInt", "parseFloat", "ptrCopy", "ptrZero"
    });
    static constexpr auto allocTrapBuiltins = std::to_array<std::string_view>({
        "push", "reserve", "str", "strFromInt", "strFromFloat",
        "sbWithCap", "sbPush", "sbPushByte", "sbPushInt", "sbPushFloat", "sbStr",
        "mapWithCap", "mapSet", "mapKeys", "mapValues",
        "setWithCap", "setAdd", "setItems",
        "poolWithCap", "poolAdd", "poolHandles",
        "sharedNew", "sharedReuse", "sharedSet",
        "sortStable", "mutexNew", "mutexLock", "mutexTryLock",
        "taskScope", "freeze",
        "atomicNew", "ringNew", "regionNew", "regionAlloc", "regionAllocAligned",
        "ptrAlloc", "ptrAllocAligned", "cstr", "strFromCStr", "strFromPtr"
    });

    for (std::size_t i = 0; i < n; ++i) {
        const auto& fn = functions[i];
        std::unordered_map<unsigned, std::size_t> valToClosure;
        for (const auto& block : fn.blocks) {
            for (const auto& op : block.ops) {
                if (op.op == native::Op::MakeClosure && !op.fields.empty()) {
                    valToClosure.emplace(op.result.id, op.fields[0]);
                }
            }
        }
        auto& fx = info[i];
        auto normSpan = [&](Span s) -> Span {
            return (s.start == 0 && s.end == 0 && fn.source.span.end > 0) ? fn.source.span : s;
        };
        auto noteIo = [&](Span s, std::string reason) {
            if (!fx.io) { fx.io = true; fx.ioSpan = normSpan(s); fx.ioReason = std::move(reason); }
        };
        auto noteAlloc = [&](Span s, std::string reason) {
            if (!fx.alloc) { fx.alloc = true; fx.allocSpan = normSpan(s); fx.allocReason = std::move(reason); }
        };
        auto noteTrap = [&](Span s, std::string reason) {
            if (!fx.trap) { fx.trap = true; fx.trapSpan = normSpan(s); fx.trapReason = std::move(reason); }
        };
        auto hasIndexedMutArg = [](const native::Operation& op) {
            for (const auto& path : op.mutArgPaths)
                for (const auto& st : path)
                    if (st.isIndex) return true;
            return false;
        };
        auto addEdge = [&](const std::string& calleeName, Span callSpan, std::string display) {
            if (auto it = fnIndex.find(calleeName); it != fnIndex.end()) {
                edges[i].push_back({it->second, normSpan(callSpan), std::move(display)});
            }
        };

        for (const auto& block : fn.blocks) {
            for (const auto& op : block.ops) {
                switch (op.op) {
                case native::Op::Print:
                    noteIo(op.span, "print");
                    break;
                case native::Op::ReadByte:
                    noteIo(op.span, "readByte");
                    break;
                case native::Op::Assert:
                    if (op.boundsProven || op.unchecked) break;
                    if (op.data == "requires" || op.data == "ensures") {
                        noteTrap(op.span, "contrato " + op.data + " (R_CONTRACT)");
                    } else {
                        noteTrap(op.span, "assert (R_ASSERT)");
                    }
                    break;
                case native::Op::Negate:
                    if (Program::isSignedInt(op.result.type) && !op.overflowProven) {
                        noteTrap(op.span, "negación entera (R_OVERFLOW)");
                    }
                    break;
                case native::Op::AbsDiff:
                    if (!op.overflowProven) {
                        noteTrap(op.span, "diferencia absoluta (R_OVERFLOW)");
                    }
                    break;
                case native::Op::Select:
                    break;
                case native::Op::Binary:
                    if (!op.inputs.empty() && op.inputs[0].type == Type::Str && op.data == "+") {
                        noteAlloc(op.span, "concatenación de cadenas (R_ALLOC)");
                        noteTrap(op.span, "concatenación de cadenas (R_ALLOC)");
                    } else if (!op.inputs.empty() && Program::isIntType(op.inputs[0].type)) {
                        if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                             op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) {
                            noteTrap(op.span, "operación '" + op.data + "' (R_OVERFLOW)");
                        } else if ((op.data == "/" || op.data == "%") && !op.divSafe) {
                            noteTrap(op.span, "operación '" + op.data + "' (R_DIV_ZERO/R_OVERFLOW)");
                        }
                    }
                    break;
                case native::Op::Index:
                case native::Op::IndexSet:
                    if (!op.inputs.empty() && op.inputs[0].type == Type::Str) {
                        noteTrap(op.span, "acceso por byte en cadena (R_BOUNDS)");
                    } else if (!op.boundsProven) {
                        noteTrap(op.span, "acceso por índice (R_BOUNDS)");
                    }
                    break;
                case native::Op::FieldSet:
                    if (op.inputs.size() >= 2) {
                        noteTrap(op.span, "acceso por índice en campo (R_BOUNDS)");
                    }
                    break;
                case native::Op::PlaceSet:
                    for (const auto& st : op.placeSteps) {
                        if (st.isIndex && !st.boundsProven) {
                            noteTrap(op.span, "acceso por índice en lugar asignable (R_BOUNDS)");
                            break;
                        }
                    }
                    break;
                case native::Op::RepeatRegion:
                    noteTrap(op.span, "repeat (R_ARRAY_SIZE/R_ALLOC)");
                    if (!fn.canStackArena || op.result.type != Type::IntArray) {
                        noteAlloc(op.span, "repeat en región dinámica");
                    }
                    break;
                case native::Op::RepeatHeap:
                    noteAlloc(op.span, "repeat en el heap (R_ARRAY_SIZE/R_ALLOC)");
                    noteTrap(op.span, "repeat en el heap (R_ARRAY_SIZE/R_ALLOC)");
                    break;
                case native::Op::ArrayLit:
                    if (!op.inputs.empty()) {
                        noteAlloc(op.span, "literal de arreglo en el heap (R_ALLOC)");
                        noteTrap(op.span, "literal de arreglo en el heap (R_ALLOC)");
                    }
                    break;
                case native::Op::Slice:
                    noteAlloc(op.span, "corte de secuencia (R_BOUNDS/R_ALLOC)");
                    noteTrap(op.span, "corte de secuencia (R_BOUNDS/R_ALLOC)");
                    break;
                case native::Op::Interp:
                    noteAlloc(op.span, "interpolación de cadena (R_ALLOC)");
                    noteTrap(op.span, "interpolación de cadena (R_ALLOC)");
                    break;
                case native::Op::BoxDyn:
                    noteAlloc(op.span, "caja dinámica dyn (R_ALLOC)");
                    noteTrap(op.span, "caja dinámica dyn (R_ALLOC)");
                    break;
                case native::Op::MakeClosure:
                    if (!op.fields.empty() && op.fields[0] < checked.closures.size()) {
                        const auto& cl = checked.closures[op.fields[0]];
                        const auto* ed = cl.envStruct != Type::Void ? checked.declaration(cl.envStruct) : nullptr;
                        if (ed && !ed->variants.empty() && !ed->variants[0].fields.empty()) {
                            noteAlloc(op.span, "entorno capturado de closure (R_ALLOC)");
                            noteTrap(op.span, "entorno capturado de closure (R_ALLOC)");
                        }
                    }
                    break;
                case native::Op::Clone:
                    if (!checked.isShared(op.result.type) && !checked.isFrozen(op.result.type) &&
                        !checked.isAtomic(op.result.type) && !checked.isRingQueue(op.result.type)) {
                        noteAlloc(op.span, "clonación en el heap (R_ALLOC)");
                        noteTrap(op.span, "clonación en el heap (R_ALLOC)");
                        if (!info[i].clone) {
                            info[i].clone = true;
                            info[i].cloneSpan = op.span;
                            info[i].cloneReason = extractValueHint(checked, op.span, "clone");
                        }
                    }
                    break;
                case native::Op::Call:
                    if (i < safe.size() && !safe[i]) {
                        noteTrap(op.span, "profundidad de llamada no acotada estáticamente (R_DEPTH)");
                    }
                    if (hasIndexedMutArg(op)) {
                        noteTrap(op.span, "acceso por índice en argumento mut (R_BOUNDS)");
                    }
                    if (auto it = fnIndex.find(op.data); it != fnIndex.end()) {
                        addEdge(op.data, op.span, fnDisplayName(functions[it->second].source));
                    }
                    break;
                case native::Op::ExternCall:
                    if (auto it = fnIndex.find(op.data); it != fnIndex.end()) {
                        addEdge(op.data, op.span, fnDisplayName(functions[it->second].source));
                    } else {
                        noteIo(op.span, "llamada extern \"C\" '" + op.data + "'");
                        noteAlloc(op.span, "llamada extern \"C\" '" + op.data + "'");
                        noteTrap(op.span, "llamada extern \"C\" '" + op.data + "'");
                    }
                    break;
                case native::Op::FnPtr:
                    break;
                case native::Op::DynCall:
                    if (i < safe.size() && !safe[i]) {
                        noteTrap(op.span, "llamada dinámica con profundidad no acotada (R_DEPTH)");
                    }
                    if (hasIndexedMutArg(op)) {
                        noteTrap(op.span, "acceso por índice en argumento mut (R_BOUNDS)");
                    }
                    if (op.fields.size() >= 2) {
                        const Type iface = static_cast<Type>(op.fields[0]);
                        const std::size_t mi = op.fields[1];
                        for (const auto& dv : checked.dynVtables) {
                            if (dv.iface == iface && mi < dv.methods.size() && !dv.methods[mi].empty()) {
                                addEdge(dv.methods[mi], op.span, "método dyn");
                            }
                        }
                    }
                    break;
                case native::Op::ClosureCall:
                    if (i < safe.size() && !safe[i]) {
                        noteTrap(op.span, "llamada a closure con profundidad no acotada (R_DEPTH)");
                    }
                    if (!op.inputs.empty()) {
                        const Type fnT = op.inputs[0].type;
                        for (const auto& cl : checked.closures) {
                            if (cl.fnType == fnT && !cl.fnName.empty()) {
                                addEdge(cl.fnName, op.span, "closure");
                            }
                        }
                    }
                    break;
                case native::Op::Builtin: {
                    if (hasIndexedMutArg(op)) {
                        noteTrap(op.span, "acceso por índice en argumento mut (R_BOUNDS)");
                    }
                    const std::string& bn = op.data;
                    if (oneOfWord(bn, ioOnlyBuiltins)) {
                        noteIo(op.span, bn);
                    } else if (oneOfWord(bn, ioAllocTrapBuiltins)) {
                        noteIo(op.span, bn);
                        noteAlloc(op.span, bn + " (R_ALLOC)");
                        noteTrap(op.span, bn + " (R_ALLOC)");
                    } else if (bn == "castNum") {
                        if (!op.inputs.empty() && Program::isFloatType(op.inputs[0].type) &&
                            Program::isIntType(op.result.type)) {
                            noteTrap(op.span, "cast flotante a entero (R_OVERFLOW)");
                        }
                    } else if (oneOfWord(bn, trapOnlyBuiltins)) {
                        noteTrap(op.span, bn);
                    } else if (bn == "abs") {
                        if (Program::isSignedInt(op.result.type)) noteTrap(op.span, "abs entero (R_OVERFLOW)");
                    } else if (bn == "ptrRead" || bn == "ptrWrite" || bn == "ptrAdd" ||
                               bn == "simdGet" || bn == "simdSet" || bn == "simdFromArray" || bn == "simdToArray" ||
                               bn == "simdLoad" || bn == "simdLoadAligned" || bn == "simdStore" || bn == "simdStoreAligned") {
                        if (!op.unchecked) noteTrap(op.span, bn + " (R_NULL_PTR/R_BOUNDS/R_ALIGN)");
                    } else if (oneOfWord(bn, allocTrapBuiltins)) {
                        noteAlloc(op.span, bn + " (R_ALLOC)");
                        noteTrap(op.span, bn + " (R_ALLOC)");
                    } else if (bn == "sharedUpdate") {
                        noteAlloc(op.span, "sharedUpdate (R_ALLOC)");
                        noteTrap(op.span, "sharedUpdate (R_ALLOC/R_DEPTH)");
                        if (op.inputs.size() >= 2) {
                            const Type fnT = op.inputs[1].type;
                            for (const auto& cl : checked.closures) {
                                if (cl.fnType == fnT && !cl.fnName.empty()) {
                                    addEdge(cl.fnName, op.span, "closure");
                                }
                            }
                        }
                    } else if (bn == "taskSpawn" || bn == "taskSpawnMove" || bn == "parallelMap") {
                        noteAlloc(op.span, bn + " (R_ALLOC)");
                        noteTrap(op.span, bn + " (R_ALLOC/R_DEPTH)");
                        const std::size_t fnIdx = (bn == "taskSpawnMove") ? 2u : 1u;
                        if (fnIdx < op.inputs.size()) {
                            const auto vit = valToClosure.find(op.inputs[fnIdx].id);
                            if (vit != valToClosure.end() && vit->second < checked.closures.size() &&
                                !checked.closures[vit->second].fnName.empty()) {
                                addEdge(checked.closures[vit->second].fnName, op.span, "closure");
                            } else {
                                const Type fnT = op.inputs[fnIdx].type;
                                for (const auto& cl : checked.closures) {
                                    if (cl.fnType == fnT && !cl.fnName.empty()) {
                                        addEdge(cl.fnName, op.span, "closure");
                                    }
                                }
                            }
                        }
                    } else if (bn == "frozenRead") {
                        if (i < safe.size() && !safe[i]) {
                            noteTrap(op.span, "frozenRead con profundidad no acotada (R_DEPTH)");
                        }
                        if (op.inputs.size() >= 2) {
                            const auto vit = valToClosure.find(op.inputs[1].id);
                            if (vit != valToClosure.end() && vit->second < checked.closures.size() &&
                                !checked.closures[vit->second].fnName.empty()) {
                                addEdge(checked.closures[vit->second].fnName, op.span, "closure");
                            } else {
                                const Type fnT = op.inputs[1].type;
                                for (const auto& cl : checked.closures) {
                                    if (cl.fnType == fnT && !cl.fnName.empty()) {
                                        addEdge(cl.fnName, op.span, "closure");
                                    }
                                }
                            }
                        }
                    } else if (bn == "mapGet" || bn == "poolGet" || bn == "sharedGet" || bn == "frozenGet") {
                        const Type innerT = checked.isOption(op.result.type)
                            ? checked.optionInner(op.result.type) : op.result.type;
                        if (native::needsDropType(checked, innerT)) {
                            noteAlloc(op.span, bn + " clona elemento con dueño (R_ALLOC)");
                            noteTrap(op.span, bn + " clona elemento con dueño (R_ALLOC)");
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
    }

    bool changed = false;
    do {
        changed = false;
        for (std::size_t i = 0; i < n; ++i) {
            auto& fx = info[i];
            for (const auto& edge : edges[i]) {
                const auto& other = info[edge.to];
                if (!fx.io && other.io) {
                    fx.io = true;
                    fx.ioSpan = edge.callSpan;
                    fx.ioReason = "llama a '" + edge.display + "' -> " + other.ioReason;
                    changed = true;
                }
                if (!fx.alloc && other.alloc) {
                    fx.alloc = true;
                    fx.allocSpan = edge.callSpan;
                    fx.allocReason = "llama a '" + edge.display + "' -> " + other.allocReason;
                    changed = true;
                }
                if (!fx.trap && other.trap) {
                    fx.trap = true;
                    fx.trapSpan = edge.callSpan;
                    fx.trapReason = "llama a '" + edge.display + "' -> " + other.trapReason;
                    changed = true;
                }
            }
        }
    } while (changed);

    std::map<std::string, IrEffects> out;
    for (std::size_t i = 0; i < n; ++i) out.emplace(functions[i].source.name, std::move(info[i]));
    return out;
}

std::string safetyName(SafetyTier s) {
    switch (s) {
        case SafetyTier::Trusted: return "trusted";
        case SafetyTier::Raw: return "raw";
        default: return "verified";
    }
}

std::string formatModifiers(const Function& f) {
    std::string out;
    if (f.noCopy) out += "@noCopy ";
    if (f.arenaHint) out += "@arena ";
    if (f.compactHint) out += "@compact ";
    if (f.safety == SafetyTier::Trusted) out += "trusted ";
    else if (f.safety == SafetyTier::Raw) out += "raw ";
    if (f.reqPure) out += "pure ";
    if (f.reqNoAlloc) out += "noalloc ";
    if (f.reqCheckFree) out += "checkfree ";
    if (f.reqIo) out += "io ";
    return out;
}

std::string mutParamsJson(const Function& f) {
    std::string out = "[";
    bool first = true;
    for (const auto& p : f.params) {
        if (!p.byRef) continue;
        if (!first) out += ',';
        first = false;
        out += jsonString(p.name);
    }
    out += "]";
    return out;
}

std::string formatParamsAndReturn(const Program& p, const Function& f) {
    std::string result = "(";
    for (size_t i = 0; i < f.params.size(); ++i) {
        if (i) result += ", ";
        if (i == 0 && f.params[i].name == "self" && p.nameOf(f.params[i].type) == "Self") {
            result += f.params[i].byRef ? "mut self" : "self";
        } else if (f.params[i].byRef) {
            result += f.params[i].name + ": mut " + p.nameOf(f.params[i].type);
        } else {
            if (f.params[i].writable) result += "var ";
            result += f.params[i].name + ": " + p.nameOf(f.params[i].type);
        }
    }
    result += ")";
    if (f.result != Type::Void) result += " -> " + p.nameOf(f.result);
    auto appendContract = [&](std::string_view kw, const Expr& e) {
        const auto owner = p.sourceOf(e.span);
        if (e.span.start < e.span.end && e.span.end <= owner.size()) {
            std::string_view text = owner.substr(e.span.start, e.span.end - e.span.start);
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
                text.remove_suffix(1);
            result += ' ';
            result += kw;
            result += ' ';
            result.append(text);
        }
    };
    for (const auto& r : f.requiresClauses) appendContract("requires", r);
    for (const auto& e : f.ensuresClauses) appendContract("ensures", e);
    return result;
}

std::string signature(const Program& p, const Function& f) {
    std::string result = formatModifiers(f);
    if (f.isExternC) result += "extern \"C\" ";
    result += "fn " + f.name;
    if (!f.generics.empty()) {
        result += "<";
        for (size_t i = 0; i < f.generics.size(); ++i) {
            if (i) result += ", ";
            result += p.nameOf(f.generics[i]);
            if (i < f.constraints.size() && f.constraints[i] != Type::Unknown)
                result += ": " + p.nameOf(f.constraints[i]);
        }
        result += ">";
    }
    result += formatParamsAndReturn(p, f);
    return result;
}

std::string effectsJson(bool io, bool alloc, bool trap) {
    std::string out = "[";
    out += (io ? "\"io\"" : "\"pure\"");
    if (!alloc) out += ",\"noalloc\"";
    if (!trap) out += ",\"checkfree\"";
    out += "]";
    return out;
}

std::string_view stripModulePrefix(std::string_view name) {
    const auto lt = name.find('<');
    const auto dot = name.find('.');
    if (dot != std::string_view::npos && (lt == std::string_view::npos || dot < lt)) {
        return name.substr(dot + 1);
    }
    return name;
}

std::string implMethodCanonicalName(const Program& p, const ImplDecl& imp, const Function& m) {
    const std::string tFull = p.nameOf(imp.target);
    if (imp.interface == Type::Unknown) return tFull + "." + m.sourceName;
    return tFull + "." + p.nameOf(imp.interface) + "." + m.sourceName;
}

bool implMethodMatches(const Program& p, const ImplDecl& imp, const Function& m,
                       std::string_view query, bool& isExact) {
    const std::string tFull = p.nameOf(imp.target);
    const std::string tShort(stripModulePrefix(tFull));
    if (imp.interface == Type::Unknown) {
        if (query == tFull + "." + m.sourceName) { isExact = true; return true; }
        if (query == tShort + "." + m.sourceName) { isExact = false; return true; }
        return false;
    }
    const std::string iFull = p.nameOf(imp.interface);
    const std::string iShort(stripModulePrefix(iFull));
    if (query == tFull + "." + iFull + "." + m.sourceName) { isExact = true; return true; }
    if (query == tShort + "." + iShort + "." + m.sourceName ||
        query == tFull + "." + iShort + "." + m.sourceName ||
        query == tShort + "." + iFull + "." + m.sourceName ||
        query == tFull + "." + m.sourceName ||
        query == tShort + "." + m.sourceName) {
        isExact = false;
        return true;
    }
    return false;
}
// Serialize concurrent veloz edits. A failed precondition leaves the original intact.
struct EditLock {
    fs::path path;
    explicit EditLock(const fs::path& file) : path(fs::canonical(file)) {
        path += ".veloz-lock";
        if (!fs::create_directory(path))
            throw Diagnostic("E_BUSY", "Ya hay una edición de este archivo en curso", {});
    }
    ~EditLock() { std::error_code ignored; fs::remove(path, ignored); }
};
void publishSource(const fs::path& file, const std::string& original, std::string_view replacement) {
    EditLock lock(file);
    if (readFile(file) != original)
        throw Diagnostic("E_STALE", "La fuente cambió durante la edición. Vuelve a leer su firma.", {});
    replaceFile(fs::canonical(file), replacement);
}
struct PatchTarget {
    enum class Kind { Function, Const, Struct, Enum, Interface, Method } kind;
    Span span{};
    std::size_t bodyStart = 0;
    std::string matchName;
    bool isEnum = false;
    std::vector<std::string> implGenerics;
};

const Function& symbol(const Program& p, const std::string& name) {
    const Function* simple = nullptr;
    for (const auto& f : p.functions) {
        if (f.isTest || f.isImpl) continue;
        if (f.name == name) return f;
        if (f.sourceName == name) {
            if (simple) throw Diagnostic("E_SYMBOL", "El nombre simple es ambiguo; usa el nombre cualificado: " + name, {});
            simple = &f;
        }
    }
    if (simple) return *simple;
    throw Diagnostic("E_SYMBOL", "No existe la función " + name, {});
}

PatchTarget resolvePatchTarget(const Program& p, const std::string& query) {
    std::vector<PatchTarget> exact;
    std::vector<PatchTarget> alias;

    for (const auto& f : p.functions) {
        if (f.isTest || f.isImpl) continue;
        PatchTarget t{PatchTarget::Kind::Function, f.span, f.bodyStart, f.sourceName, false, {}};
        if (f.name == query) exact.push_back(std::move(t));
        else if (f.sourceName == query) alias.push_back(std::move(t));
    }
    for (const auto& c : p.consts) {
        PatchTarget t{PatchTarget::Kind::Const, c.span, 0, c.sourceName, false, {}};
        if (c.name == query) exact.push_back(std::move(t));
        else if (c.sourceName == query) alias.push_back(std::move(t));
    }
    for (const auto& d : p.data) {
        const std::string full = p.nameOf(d.type);
        const std::string shortN(stripModulePrefix(full));
        PatchTarget t{d.isEnum ? PatchTarget::Kind::Enum : PatchTarget::Kind::Struct,
                      d.span, 0, shortN, d.isEnum, {}};
        if (full == query) exact.push_back(std::move(t));
        else if (shortN == query) alias.push_back(std::move(t));
    }
    for (const auto& iface : p.interfaces) {
        const std::string full = p.nameOf(iface.type);
        const std::string shortN(stripModulePrefix(full));
        PatchTarget t{PatchTarget::Kind::Interface, iface.span, 0, shortN, false, {}};
        if (full == query) exact.push_back(std::move(t));
        else if (shortN == query) alias.push_back(std::move(t));
    }
    for (const auto& imp : p.impls) {
        std::vector<std::string> genNames;
        for (Type g : imp.generics) genNames.push_back(p.nameOf(g));
        for (const auto& m : imp.methods) {
            bool isExact = false;
            if (implMethodMatches(p, imp, m, query, isExact)) {
                PatchTarget t{PatchTarget::Kind::Method, m.span, m.bodyStart, m.sourceName, false, genNames};
                if (isExact) exact.push_back(std::move(t));
                else alias.push_back(std::move(t));
            }
        }
    }

    if (exact.size() == 1) return exact.front();
    if (exact.size() > 1)
        throw Diagnostic("E_SYMBOL", "El nombre es ambiguo; usa el nombre cualificado completo: " + query, {});
    if (alias.size() == 1) return alias.front();
    if (alias.size() > 1)
        throw Diagnostic("E_SYMBOL", "El nombre simple es ambiguo; usa el nombre cualificado: " + query, {});
    throw Diagnostic("E_SYMBOL", "No existe el símbolo " + query, {});
}

bool sigMatchesAny(const Program& p, std::string_view query) {
    for (const auto& c : p.consts)
        if (c.name == query || c.sourceName == query) return true;
    for (const auto& d : p.data) {
        const std::string full = p.nameOf(d.type);
        if (full == query || stripModulePrefix(full) == query) return true;
    }
    for (const auto& iface : p.interfaces) {
        const std::string full = p.nameOf(iface.type);
        if (full == query || stripModulePrefix(full) == query) return true;
    }
    for (const auto& imp : p.impls) {
        for (const auto& m : imp.methods) {
            bool isExact = false;
            if (implMethodMatches(p, imp, m, query, isExact)) return true;
        }
    }
    for (const auto& f : p.functions) {
        if (!f.isTest && !f.isImpl && (f.name == query || f.sourceName == query)) return true;
    }
    return false;
}

void rebaseSpan(Span& s, std::size_t prefixLen, std::size_t maxLen) {
    const std::size_t lo = prefixLen;
    const std::size_t hi = prefixLen + maxLen;
    s.start = std::clamp(s.start, lo, hi) - prefixLen;
    s.end = std::clamp(s.end, lo, hi) - prefixLen;
}

std::string applyPatch(const std::vector<std::string>& args, const fs::path& file,
                       const std::string&, const Program& original) {
    if (args.size() != 5) throw Diagnostic("E_USAGE", "veloz patch ARCHIVO SÍMBOLO HASH REEMPLAZO", {});
    const auto targetInfo = resolvePatchTarget(original, args[2]);
    if (args[0] == "patch-body" &&
        targetInfo.kind != PatchTarget::Kind::Function &&
        targetInfo.kind != PatchTarget::Kind::Method) {
        throw Diagnostic("E_PATCH", "patch-body solo se admite para funciones y métodos", targetInfo.span);
    }
    const auto target = original.fileOf(targetInfo.span).empty() ? file : pathFrom(original.fileOf(targetInfo.span));
    const std::string targetSource(original.sourceOf(targetInfo.span));
    auto bytes = std::string_view(targetSource).substr(targetInfo.span.start, targetInfo.span.end - targetInfo.span.start);
    if (fingerprint(bytes) != args[3])
        throw Diagnostic("E_STALE", "El hash cambió. Consulta veloz sig.", targetInfo.span);
    const auto replacementFile = pathFrom(args[4]);
    auto replacement = readFile(replacementFile);
    if (args[0] == "patch-body") {
        replacement = targetSource.substr(targetInfo.span.start, targetInfo.bodyStart - targetInfo.span.start)
                    + "\n" + replacement + "\n}";
    }

    std::string parseInput = replacement;
    std::string prefix;
    if (targetInfo.kind == PatchTarget::Kind::Method) {
        prefix = "impl";
        if (!targetInfo.implGenerics.empty()) {
            prefix += "<";
            for (std::size_t i = 0; i < targetInfo.implGenerics.size(); ++i) {
                if (i) prefix += ", ";
                prefix += targetInfo.implGenerics[i];
            }
            prefix += ">";
        }
        prefix += " Dummy {\n";
        parseInput = prefix + replacement + "\n}";
    }

    Program parsed;
    auto parseError = captureDiagnostic([&] { parsed = parse(parseInput); });
    if (parseError) {
        if (args[0] == "patch") {
            if (!prefix.empty()) {
                rebaseSpan(parseError->span, prefix.size(), replacement.size());
                for (auto& rel : parseError->related) rebaseSpan(rel, prefix.size(), replacement.size());
                if (parseError->fix) rebaseSpan(parseError->fix->span, prefix.size(), replacement.size());
            }
            parseError->file = pathText(fs::canonical(replacementFile));
            parseError->source = replacement;
        }
        throw *parseError;
    }

    Span extractedSpan{};
    const bool hasExtra = !parsed.imports.empty();
    switch (targetInfo.kind) {
    case PatchTarget::Kind::Function:
        if (hasExtra || !parsed.consts.empty() || !parsed.data.empty() || !parsed.interfaces.empty() ||
            !parsed.impls.empty() || parsed.functions.size() != 1 || parsed.functions[0].isTest ||
            parsed.functions[0].sourceName != targetInfo.matchName)
            throw Diagnostic("E_PATCH", "El reemplazo debe contener una función con el mismo nombre", targetInfo.span);
        extractedSpan = parsed.functions[0].span;
        break;
    case PatchTarget::Kind::Const:
        if (hasExtra || !parsed.functions.empty() || !parsed.data.empty() || !parsed.interfaces.empty() ||
            !parsed.impls.empty() || parsed.consts.size() != 1 ||
            parsed.consts[0].sourceName != targetInfo.matchName)
            throw Diagnostic("E_PATCH", "El reemplazo debe contener una sola const con el mismo nombre", targetInfo.span);
        extractedSpan = parsed.consts[0].span;
        break;
    case PatchTarget::Kind::Struct:
    case PatchTarget::Kind::Enum:
        if (hasExtra || !parsed.functions.empty() || !parsed.consts.empty() || !parsed.interfaces.empty() ||
            !parsed.impls.empty() || parsed.data.size() != 1 ||
            parsed.data[0].isEnum != targetInfo.isEnum ||
            parsed.nameOf(parsed.data[0].type) != targetInfo.matchName)
            throw Diagnostic("E_PATCH", "El reemplazo debe contener una sola declaración de tipo con el mismo nombre", targetInfo.span);
        extractedSpan = parsed.data[0].span;
        break;
    case PatchTarget::Kind::Interface:
        if (hasExtra || !parsed.functions.empty() || !parsed.consts.empty() || !parsed.data.empty() ||
            !parsed.impls.empty() || parsed.interfaces.size() != 1 ||
            parsed.nameOf(parsed.interfaces[0].type) != targetInfo.matchName)
            throw Diagnostic("E_PATCH", "El reemplazo debe contener una sola interface con el mismo nombre", targetInfo.span);
        extractedSpan = parsed.interfaces[0].span;
        break;
    case PatchTarget::Kind::Method:
        if (hasExtra || !parsed.functions.empty() || !parsed.consts.empty() || !parsed.data.empty() ||
            !parsed.interfaces.empty() || parsed.impls.size() != 1 || parsed.impls[0].methods.size() != 1 ||
            parsed.impls[0].methods[0].sourceName != targetInfo.matchName ||
            parsed.impls[0].methods[0].span.start < prefix.size() ||
            parsed.impls[0].methods[0].span.end > prefix.size() + replacement.size())
            throw Diagnostic("E_PATCH", "El reemplazo debe contener un solo método con el mismo nombre", targetInfo.span);
        extractedSpan = parsed.impls[0].methods[0].span;
        break;
    }

    auto updated = targetSource.substr(0, targetInfo.span.start)
                 + parseInput.substr(extractedSpan.start, extractedSpan.end - extractedSpan.start)
                 + targetSource.substr(targetInfo.span.end);
    SourceOverrides overrides;
    overrides[pathText(fs::canonical(target))] = updated;
    auto candidate = loadProject(file, overrides);
    check(candidate);
    checkEffects(candidate);
    // A failing test must not publish a partially repaired edit.
    struct Sink : std::streambuf {
        int_type overflow(int_type c) override { return traits_type::not_eof(c); }
        std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
    } sink;
    std::ostream quiet(&sink);
    runTests(candidate, quiet);
    publishSource(target, targetSource, updated);
    return {};
}
struct Page {
    std::size_t offset, seen = 0;
    std::string text;
    std::optional<std::size_t> next;
    std::size_t maxBytes = 7168;
    bool customBudget = false;
    std::string hintPrefix;
    void add(std::string line) {
        auto i = seen++;
        if (i < offset || next) return;
        if (!text.empty() && (text.size() + line.size() + 1 > maxBytes || (!customBudget && i - offset >= 60))) {
            next = i;
            return;
        }
        text += line + "\n";
    }
    std::string finish() {
        if (next) {
            const std::size_t remaining = seen > *next ? (seen - *next) : 0;
            const std::string hint = hintPrefix + " --offset " + std::to_string(*next);
            text += "{\"nextOffset\":" + std::to_string(*next) +
                    ",\"remaining\":" + std::to_string(remaining) +
                    ",\"hint\":" + jsonString(hint) + "}\n";
        }
        return text;
    }
};

struct MemorySite {
    std::string function, kind, file, reason, valueHint;
    Span span;
    int level = 0;
};

struct CheckSite {
    std::string function, check, status, file, reason;
    Span span;
};

std::string proofKindReason(native::ProofKind pk, std::string_view fallback) {
    switch (pk) {
        case native::ProofKind::ContractRequires:
            return "Eliminada estáticamente por contrato requires/ensures.";
        case native::ProofKind::LoopBound:
            return "Eliminada estáticamente por invariante de bucle (for).";
        case native::ProofKind::ConstRange:
            return "Eliminada estáticamente mediante rangos o literales constantes.";
        case native::ProofKind::DiffGraph:
            return "Eliminada estáticamente por análisis de rangos y grafo de diferencias.";
        case native::ProofKind::SentinelLoop:
            return "Eliminada estáticamente por patrón centinela en bucle.";
        case native::ProofKind::ModuloBound:
            return "Eliminada estáticamente por límite superior de módulo (%).";
        case native::ProofKind::RawUnchecked:
            return "Omitida explícitamente en modo raw.";
        case native::ProofKind::None:
            return std::string(fallback);
    }
    return std::string(fallback);
}

std::vector<CheckSite> collectCheckSites(const Program& checked, std::string_view filter) {
    std::vector<native::FunctionIR> functions;
    const native::ReturnRanges returnRanges(checked);
    for (const auto& f : checked.functions) {
        if (!f.isTest && !f.isExternDecl) functions.push_back(native::Lowerer(checked, f, returnRanges).lower());
    }
    auto safe = native::safeDepths(checked, functions);
    std::deque<Function> closureSpecStorage;
    native::optimizeIR(functions, safe, checked.entryMain, true, nullptr, &checked, &closureSpecStorage);
    safe = native::safeDepths(checked, functions);

    const std::string genPrefix = filter.empty() ? std::string{} : (std::string(filter) + ".g");
    std::vector<CheckSite> sites;
    for (std::size_t fi = 0; fi < functions.size(); ++fi) {
        const auto& ir = functions[fi];
        const auto& f = ir.source;
        if (f.name.starts_with("@fnref.")) continue;
        if (!filter.empty()) {
            if (f.isImpl || (f.name != filter && f.sourceName != filter && !f.name.starts_with(genPrefix)))
                continue;
        }
        const std::string fnName = f.isImpl ? fnDisplayName(f) : f.name;
        const bool depthSafe = fi < safe.size() && safe[fi];

        auto emit = [&](std::string check, std::string status, Span sp, std::string reason) {
            if (sp.start == 0 && sp.end == 0 && f.span.end > 0) sp = f.span;
            sites.push_back(CheckSite{
                fnName, std::move(check), std::move(status), checked.fileOf(sp), std::move(reason), sp
            });
        };

        for (const auto& block : ir.blocks) {
            for (const auto& op : block.ops) {
                switch (op.op) {
                case native::Op::Index:
                case native::Op::IndexSet:
                    if (!op.inputs.empty() && op.inputs[0].type == Type::Str) {
                        emit("bounds", "kept", op.span,
                             "Acceso por byte en str; se conserva guarda R_BOUNDS (0 <= i < len(s)).");
                    } else if (op.boundsProven) {
                        emit("bounds", "eliminated", op.span,
                             proofKindReason(op.proofKind, "Índice en rango probado estáticamente."));
                    } else {
                        emit("bounds", "kept", op.span,
                             "No se demostró 0 <= i < len(a); añade 'requires i >= 0 && i < len(a)' o itera 'for i in 0..len(a)'.");
                    }
                    break;
                case native::Op::FieldSet:
                    if (op.inputs.size() >= 2) {
                        emit("bounds", "kept", op.span,
                             "Índice en asignación de campo de struct; se conserva guarda R_BOUNDS.");
                    }
                    break;
                case native::Op::PlaceSet:
                    for (const auto& st : op.placeSteps) {
                        if (!st.isIndex) continue;
                        if (st.boundsProven) {
                            emit("bounds", "eliminated", op.span,
                                 "Índice del lugar asignable en rango probado estáticamente.");
                        } else {
                            emit("bounds", "kept", op.span,
                                 "Índice en lugar asignable compuesto; se conserva guarda R_BOUNDS.");
                        }
                    }
                    break;
                case native::Op::Negate:
                    if (Program::isSignedInt(op.result.type)) {
                        if (op.overflowProven) {
                            emit("overflow", "eliminated", op.span,
                                 proofKindReason(op.proofKind, "Negación libre de desbordamiento probada."));
                        } else {
                            emit("overflow", "kept", op.span,
                                 "La negación podría recibir el valor mínimo; acota con 'requires' o 'if'.");
                        }
                    }
                    break;
                case native::Op::AbsDiff:
                    if (op.overflowProven) {
                        emit("overflow", "eliminated", op.span,
                             proofKindReason(op.proofKind, "absDiff en rango probado estáticamente."));
                    } else {
                        emit("overflow", "kept", op.span,
                             "absDiff podría desbordar; acota los operandos con 'requires' o 'if'.");
                    }
                    break;
                case native::Op::Select:
                    break;
                case native::Op::Binary:
                    if (!op.inputs.empty() && Program::isIntType(op.inputs[0].type)) {
                        if (op.data == "rangeIncrement") break;
                        if (op.data == "+" || op.data == "-" || op.data == "*" ||
                            op.data == "<<" || op.data == ">>" || op.data == ">>>") {
                            if (op.overflowProven) {
                                emit("overflow", "eliminated", op.span,
                                     proofKindReason(op.proofKind, "Operación '" + op.data + "' libre de desbordamiento probada."));
                            } else {
                                emit("overflow", "kept", op.span,
                                     "Posible desbordamiento en '" + op.data + "'; acota con 'requires' o usa wrappingAdd/Sub/Mul.");
                            }
                        } else if (op.data == "/" || op.data == "%") {
                            if (op.divSafe) {
                                emit("div_zero", "eliminated", op.span,
                                     proofKindReason(op.proofKind, "División '" + op.data + "' segura probada."));
                            } else {
                                emit("div_zero", "kept", op.span,
                                     "Divisor potencialmente cero (o MIN / -1) en '" + op.data + "'; añade 'requires d > 0'.");
                            }
                        }
                    }
                    break;
                case native::Op::Assert: {
                    const bool isContract = (op.data == "requires" || op.data == "ensures");
                    const std::string ck = isContract ? "contract" : "assert";
                    if (op.unchecked) {
                        emit(ck, "eliminated", op.span, "Comprobación omitida en función raw (asunción estática).");
                    } else if (op.boundsProven) {
                        emit(ck, "eliminated", op.span,
                             proofKindReason(op.proofKind, "Condición demostrada verdadera en tiempo de compilación."));
                    } else {
                        emit(ck, "kept", op.span,
                             isContract ? ("Contrato " + op.data + " verificado en ejecución (R_CONTRACT).")
                                        : "Aserción verificada en ejecución (R_ASSERT).");
                    }
                    break;
                }
                case native::Op::Call:
                case native::Op::DynCall:
                case native::Op::ClosureCall:
                    if (depthSafe) {
                        emit("depth", "eliminated", op.span,
                             "Profundidad máxima de llamada acotada estáticamente (< 256 marcos).");
                    } else {
                        emit("depth", "kept", op.span,
                             "Recursión o llamada indirecta sin cota estática; se conserva guarda R_DEPTH.");
                    }
                    break;
                case native::Op::Builtin: {
                    const std::string& bn = op.data;
                    if (bn == "simdLoadAligned" || bn == "simdStoreAligned") {
                        emit("align", op.unchecked ? "eliminated" : "kept", op.span,
                             op.unchecked ? "Alineación SIMD omitida en modo raw."
                                          : ("Comprobación de alineación R_ALIGN en " + bn + "; usa raw o ptrAllocAligned."));
                    } else if (bn == "simdGet" || bn == "simdSet" || bn == "simdFromArray" || bn == "simdToArray") {
                        emit("bounds", op.unchecked ? "eliminated" : "kept", op.span,
                             op.unchecked ? "Comprobación de carril SIMD omitida en modo raw."
                                          : ("Comprobación de índice/longitud en " + bn + " (R_BOUNDS)."));
                    } else if (bn == "ptrRead" || bn == "ptrWrite" || bn == "ptrAdd" ||
                               bn == "ptrCopy" || bn == "ptrZero" ||
                               bn == "strFromCStr" || bn == "strFromPtr" ||
                               bn == "simdLoad" || bn == "simdStore" || bn == "mutSlicePtr") {
                        emit("null_ptr", op.unchecked ? "eliminated" : "kept", op.span,
                             op.unchecked ? ("Comprobación de puntero omitida en modo raw (" + bn + ").")
                                          : ("Comprobación R_NULL_PTR en " + bn + "; usa bloque raw tras validar."));
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
    }
    return sites;
}

std::string extractValueHint(const Program& program, Span span, std::string_view fallback) {
    const auto src = program.sourceOf(span);
    std::string hint;
    if (span.start < span.end && span.end <= src.size()) {
        std::string_view raw = src.substr(span.start, span.end - span.start);
        while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '\r' || raw.front() == '\n'))
            raw.remove_prefix(1);
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n'))
            raw.remove_suffix(1);
        for (char ch : raw) {
            if (ch == '\r' || ch == '\n' || ch == '\t') {
                if (!hint.empty() && hint.back() != ' ') hint.push_back(' ');
            } else {
                hint.push_back(ch);
            }
            if (hint.size() >= 48) {
                hint.append("...");
                break;
            }
        }
    }
    return hint.empty() ? std::string(fallback) : hint;
}

std::vector<MemorySite> collectMemorySites(const Program& checked, std::string_view filter) {
    std::vector<MemorySite> sites;
    const native::ReturnRanges returnRanges(checked);
    const std::string genPrefix = filter.empty() ? std::string{} : (std::string(filter) + ".g");
    for (const auto& f : checked.functions) {
        if (f.isTest || f.isImpl || f.isExternDecl ||
            (!filter.empty() && f.name != filter && f.sourceName != filter &&
             !f.name.starts_with(genPrefix))) continue;
        // Inspect the post-move/monotonic/Perceus-reuse operations that reach LLVM.
        auto ir = native::Lowerer(checked, f, returnRanges).lower();
        native::elideLastUseMoves(ir, checked);
        native::analyzeMonotonicPools(ir, checked);
        native::fusePerceusReuse(ir, checked);
        for (const auto& block : ir.blocks) {
            for (const auto& op : block.ops) {
                int level = 0;
                std::string kind, reason;
                switch (op.op) {
                    case native::Op::RepeatRegion:
                        level = 2; kind = "region";
                        reason = "El análisis demuestra que el arreglo permanece en este ámbito.";
                        break;
                    case native::Op::RepeatHeap:
                        level = 3; kind = "owner";
                        reason = "Este valor usa un dueño único para conservar su contenido fuera de la región.";
                        break;
                    case native::Op::ArrayLit:
                        if (op.inputs.empty()) continue;
                        level = 3; kind = "owner";
                        reason = "Literal de arreglo con dueño único en el heap.";
                        break;
                    case native::Op::Slice:
                        level = 3; kind = "owner";
                        reason = "Corte de secuencia con dueño único en el heap.";
                        break;
                    case native::Op::Interp:
                        level = 3; kind = "owner";
                        reason = "Interpolación de cadena con dueño único en el heap.";
                        break;
                    case native::Op::BoxDyn:
                        level = 3; kind = "owner";
                        reason = "Caja dinámica dyn con dueño único en el heap.";
                        break;
                    case native::Op::MakeClosure: {
                        if (op.fields.empty() || op.fields[0] >= checked.closures.size()) continue;
                        const auto& cl = checked.closures[op.fields[0]];
                        const auto* ed = (cl.envStruct != Type::Void) ? checked.declaration(cl.envStruct) : nullptr;
                        if (!ed || ed->variants.empty() || ed->variants[0].fields.empty()) continue;
                        level = 3; kind = "owner";
                        reason = "Entorno capturado de closure con dueño único en el heap.";
                        break;
                    }
                    case native::Op::Clone:
                        level = 3; kind = "clone";
                        reason = "Una copia independiente conserva la semántica de valor.";
                        break;
                    case native::Op::Builtin:
                        if (op.data == "mapNew" || op.data == "mapWithCap" || op.data == "mapSecure" ||
                            op.data == "setNew" || op.data == "setWithCap" || op.data == "setSecure" ||
                            op.data == "sbNew" || op.data == "sbWithCap") {
                            level = 3; kind = "owner";
                            reason = "Colección dinámica con dueño único en el heap.";
                        } else if (op.data == "poolNew" || op.data == "poolWithCap") {
                            level = 4; kind = "pool";
                            reason = "Pool generacional con asas seguras para grafos o estructuras cíclicas.";
                            if (op.boundsProven) reason += " Comprobación generacional eliminada estáticamente (pool monótono).";
                        } else if (op.data == "sharedNew") {
                            level = 5; kind = "rc";
                            reason = "Celda compartida acíclica con conteo de referencias Perceus.";
                        } else if (op.data == "sharedReuse") {
                            level = 5; kind = "rc-reuse";
                            reason = "Celda compartida Perceus con reutilización guiada por caída.";
                        } else if (op.data == "freeze") {
                            level = 5; kind = "frozen";
                            reason = "Dato inmutable compartido entre hilos con conteo de referencias atómico.";
                        } else if (op.data == "atomicNew") {
                            level = 5; kind = "atomic";
                            reason = "Celda atómica lock-free compartida entre hilos con conteo de referencias atómico.";
                        } else if (op.data == "ringNew") {
                            level = 5; kind = "ring-queue";
                            reason = "Cola circular MPMC acotada y libre de bloqueos (Vyukov) compartida entre hilos.";
                        } else if (op.data == "regionNew") {
                            level = 2; kind = "region";
                            reason = "Región explícita con asignación bump-pointer y liberación en bloque RAII.";
                        } else if (op.data == "taskScope" || op.data == "taskSpawn" ||
                                   op.data == "taskSpawnMove" || op.data == "parallelMap") {
                            level = 3; kind = "task";
                            reason = "Concurrencia estructurada por ámbito con robo de trabajo y cierre determinista.";
                        } else if (op.data == "fileOpen" || op.data == "tcpListen" ||
                                   op.data == "tcpAccept" || op.data == "tcpConnect" ||
                                   op.data == "mutexNew" || op.data == "mutexLock" ||
                                   op.data == "mutexTryLock") {
                            level = 3; kind = "resource";
                            reason = "Recurso con dueño único y cierre determinista RAII al salir del ámbito.";
                        } else {
                            continue;
                        }
                        break;
                    default:
                        continue;
                }
                sites.push_back(MemorySite{
                    f.name, kind, checked.fileOf(op.span), reason,
                    extractValueHint(checked, op.span, kind), op.span, level
                });
            }
        }
    }
    return sites;
}

void verifyMemoryLevels(const std::vector<MemorySite>& sites, int maxLevel) {
    for (const auto& site : sites) {
        if (site.level > maxLevel) {
            Diagnostic d("E_MEMORY_LEVEL",
                         "El valor '" + site.valueHint + "' (" + site.kind + ") en la función '" + site.function +
                             "' requiere nivel de memoria " + std::to_string(site.level) +
                             ", superior al límite --max-level " + std::to_string(maxLevel),
                         site.span);
            d.example = "veloz costes ARCHIVO " + site.function;
            throw d;
        }
    }
}
}
void checkMemoryLevels(const Program& checked, int maxLevel, std::string_view filter) {
    verifyMemoryLevels(collectMemorySites(checked, filter), maxLevel);
}
void checkEffects(const Program& checked) {
    const bool anyRequired = std::any_of(checked.functions.begin(), checked.functions.end(), [](const Function& f) {
        return !f.isTest && (f.reqPure || f.reqNoAlloc || f.reqCheckFree || f.noCopy);
    });
    if (!anyRequired) return;
    const auto fxMap = analyzeProgramEffects(checked);
    for (const auto& f : checked.functions) {
        if (f.isTest || (!f.reqPure && !f.reqNoAlloc && !f.reqCheckFree && !f.noCopy)) continue;
        const auto it = fxMap.find(f.name);
        if (it == fxMap.end()) continue;
        const auto& fx = it->second;
        const std::string display = fnDisplayName(f);
        if (f.noCopy && fx.clone) {
            Diagnostic d("E_NOCOPY",
                         "La función '@noCopy' '" + display + "' realiza una copia en heap/valor (" + fx.cloneReason + "). Usa mut o región.",
                         fx.cloneSpan);
            d.related.push_back(f.span);
            d.example = "veloz costes ARCHIVO " + display;
            throw d;
        }
        if (f.reqPure && fx.io) {
            Diagnostic d("E_EFFECT",
                         "La función '" + display + "' declara pure pero realiza E/S: " + fx.ioReason,
                         fx.ioSpan);
            d.related.push_back(f.span);
            d.example = "veloz sig ARCHIVO " + display;
            throw d;
        }
        if (f.reqNoAlloc && fx.alloc) {
            Diagnostic d("E_EFFECT",
                         "La función '" + display + "' declara noalloc pero asigna en el heap: " + fx.allocReason,
                         fx.allocSpan);
            d.related.push_back(f.span);
            d.example = "veloz costes ARCHIVO " + display;
            throw d;
        }
        if (f.reqCheckFree && fx.trap) {
            Diagnostic d("E_EFFECT",
                         "La función '" + display + "' declara checkfree pero conserva comprobaciones en ejecución: " + fx.trapReason,
                         fx.trapSpan);
            d.related.push_back(f.span);
            d.example = "veloz sig ARCHIVO " + display;
            throw d;
        }
    }
}
void emitSymbolSlice(const Program& program, const Program& checked, const std::string& filter, Page& page) {
    const auto targetInfo = resolvePatchTarget(program, filter);
    std::unordered_set<std::string> seenData, seenIfaces, seenConsts, seenFns;
    std::vector<const DataDecl*> depData;
    std::vector<const InterfaceDecl*> depIfaces;
    std::vector<const ConstDecl*> depConsts;
    struct DepFn {
        std::string kind, name, signature, mutParams, file, hash;
    };
    std::vector<DepFn> depFns;

    auto sameSpan = [](Span a, Span b) {
        return a.file == b.file && a.start == b.start && a.end == b.end;
    };
    auto spanView = [&](const Program& p, Span sp) -> std::string_view {
        const auto owner = p.sourceOf(sp);
        if (sp.start <= sp.end && sp.end <= owner.size()) return owner.substr(sp.start, sp.end - sp.start);
        return {};
    };
    auto findProgramData = [&](Span sp, const std::string& fullName) -> const DataDecl* {
        for (const auto& d : program.data) if (sameSpan(d.span, sp)) return &d;
        for (const auto& d : program.data) if (program.nameOf(d.type) == fullName) return &d;
        return nullptr;
    };

    std::string targetDataName, targetIfaceName, targetConstName, targetFnName;

    auto addTypeDep = [&](auto& self, Type t, int depth) -> void {
        if (depth > 16 || t == Type::Unknown || t == Type::Void) return;
        if (checked.isArray(t)) {
            self(self, checked.elemOf(t), depth + 1);
            return;
        }
        if (const auto* app = checked.typeAppOf(t)) {
            for (Type a : app->args) self(self, a, depth + 1);
            if (!checked.declaration(t)) {
                for (const auto& d : program.data) {
                    const std::string dName = program.nameOf(d.type);
                    const auto lt = dName.find('<');
                    const std::string base = (lt == std::string::npos) ? dName : dName.substr(0, lt);
                    if (base == app->baseName || stripModulePrefix(base) == app->baseName) {
                        if (dName != targetDataName && seenData.insert(dName).second) {
                            depData.push_back(&d);
                            for (const auto& v : d.variants)
                                for (const auto& fld : v.fields) self(self, fld.type, depth + 1);
                        }
                        break;
                    }
                }
            }
        }
        if (const DataDecl* dd = checked.declaration(t)) {
            const std::string full = checked.nameOf(dd->type);
            const DataDecl* srcDecl = findProgramData(dd->span, full);
            const std::string key = srcDecl ? program.nameOf(srcDecl->type) : full;
            if (key != targetDataName && seenData.insert(key).second) {
                depData.push_back(srcDecl ? srcDecl : dd);
                for (const auto& v : dd->variants)
                    for (const auto& fld : v.fields) self(self, fld.type, depth + 1);
            }
        }
        for (const auto& iface : checked.interfaces) {
            if (iface.type != t) continue;
            const std::string full = checked.nameOf(iface.type);
            const InterfaceDecl* srcIface = &iface;
            for (const auto& pi : program.interfaces) {
                if (sameSpan(pi.span, iface.span)) { srcIface = &pi; break; }
            }
            const std::string key = program.nameOf(srcIface->type);
            if (key != targetIfaceName && seenIfaces.insert(key).second) {
                depIfaces.push_back(srcIface);
                for (const auto& m : iface.methods) {
                    for (const auto& p : m.params) self(self, p.type, depth + 1);
                    self(self, m.result, depth + 1);
                }
            }
        }
    };

    auto collectConst = [&](const std::string& name, const std::string& mod) {
        if (name.empty()) return;
        const std::string modName = mod.empty() ? name : (mod + "." + name);
        for (const auto& c : checked.consts) {
            if (c.name != name && c.name != modName && c.sourceName != name) continue;
            if (c.name == targetConstName || !seenConsts.insert(c.name).second) return;
            depConsts.push_back(&c);
            addTypeDep(addTypeDep, c.type, 0);
            return;
        }
    };

    auto recordCalledFn = [&](const std::string& calleeName, Span calleeSpan, bool isMethod) {
        if (calleeName.empty() || calleeName == targetFnName || isBuiltinFn(calleeName)) return;
        for (const auto& f : program.functions) {
            if (f.isTest || f.isImpl) continue;
            if (sameSpan(f.span, calleeSpan) || f.name == calleeName) {
                if (f.name == targetFnName || !seenFns.insert(f.name).second) return;
                depFns.push_back({
                    "fn", f.name, signature(program, f), mutParamsJson(f),
                    program.fileOf(f.span), fingerprint(spanView(program, f.span))
                });
                return;
            }
        }
        for (const auto& imp : program.impls) {
            for (const auto& m : imp.methods) {
                if (sameSpan(m.span, calleeSpan) || implMethodCanonicalName(program, imp, m) == calleeName) {
                    const std::string canonical = implMethodCanonicalName(program, imp, m);
                    if (canonical == targetFnName || !seenFns.insert(canonical).second) return;
                    depFns.push_back({
                        "method", canonical, formatModifiers(m) + "fn " + canonical + formatParamsAndReturn(program, m),
                        mutParamsJson(m), program.fileOf(m.span), fingerprint(spanView(program, m.span))
                    });
                    addTypeDep(addTypeDep, imp.target, 0);
                    return;
                }
            }
        }
        if (!isMethod && calleeName.find(".g") == std::string::npos) {
            for (const auto& f : checked.functions) {
                if (f.name != calleeName || f.isTest) continue;
                if (f.name == targetFnName || !seenFns.insert(f.name).second) return;
                depFns.push_back({
                    f.isImpl ? "method" : "fn", f.name, signature(checked, f), mutParamsJson(f),
                    checked.fileOf(f.span), fingerprint(spanView(checked, f.span))
                });
                return;
            }
        }
    };

    std::function<void(const std::vector<Stmt>&, const std::string&)> walkSliceBody;
    walkSliceBody = [&](const std::vector<Stmt>& body, const std::string& mod) {
        auto visitStmt = [&](const Stmt& s) {
            addTypeDep(addTypeDep, s.annotation, 0);
            for (Type ft : s.fieldTypes) addTypeDep(addTypeDep, ft, 0);
        };
        auto visitExpr = [&](const Expr& e) {
            addTypeDep(addTypeDep, e.type, 0);
            addTypeDep(addTypeDep, e.assocType, 0);
            for (Type ta : e.typeArgs) addTypeDep(addTypeDep, ta, 0);
            if (e.kind == ExprKind::Name) collectConst(e.text, mod);
            if (e.kind == ExprKind::Closure && e.closure) {
                for (const auto& p : e.closure->params) addTypeDep(addTypeDep, p.type, 0);
                addTypeDep(addTypeDep, e.closure->result, 0);
            }
            if (e.kind == ExprKind::Call || e.kind == ExprKind::MethodCall || e.kind == ExprKind::AssocCall) {
                bool matched = false;
                for (const auto& cf : checked.functions) {
                    if (cf.isTest) continue;
                    if (cf.name == e.text ||
                        ((e.kind == ExprKind::MethodCall || e.kind == ExprKind::AssocCall) && cf.isImpl && cf.sourceName == e.text)) {
                        for (const auto& p : cf.params) addTypeDep(addTypeDep, p.type, 0);
                        addTypeDep(addTypeDep, cf.result, 0);
                        recordCalledFn(cf.name, cf.span, cf.isImpl);
                        matched = true;
                        break;
                    }
                }
                if (!matched && e.kind == ExprKind::Call) recordCalledFn(e.text, Span{}, false);
            }
        };
        walk(body, visitStmt, visitExpr);
    };

    if (targetInfo.kind == PatchTarget::Kind::Function) {
        const Function* orig = nullptr;
        for (const auto& f : program.functions) {
            if (!f.isTest && !f.isImpl && sameSpan(f.span, targetInfo.span)) { orig = &f; break; }
        }
        if (!orig) throw Diagnostic("E_SYMBOL", "No existe el símbolo " + filter, {});
        targetFnName = orig->name;
        const auto view = spanView(program, orig->span);
        page.add("{\"role\":\"target\",\"kind\":\"fn\",\"name\":" + jsonString(orig->name) +
                 ",\"file\":" + jsonString(program.fileOf(orig->span)) +
                 ",\"signature\":" + jsonString(signature(program, *orig)) +
                 ",\"mutParams\":" + mutParamsJson(*orig) +
                 ",\"source\":" + jsonString(view) +
                 ",\"hash\":" + jsonString(fingerprint(view)) + "}");
        bool anyChecked = false;
        for (const auto& cf : checked.functions) {
            if (cf.isTest || !sameSpan(cf.span, orig->span)) continue;
            anyChecked = true;
            for (const auto& p : cf.params) addTypeDep(addTypeDep, p.type, 0);
            addTypeDep(addTypeDep, cf.result, 0);
            walkSliceBody(cf.body, cf.module);
        }
        if (!anyChecked) {
            for (const auto& p : orig->params) addTypeDep(addTypeDep, p.type, 0);
            addTypeDep(addTypeDep, orig->result, 0);
            walkSliceBody(orig->body, orig->module);
        }
    } else if (targetInfo.kind == PatchTarget::Kind::Method) {
        const ImplDecl* origImp = nullptr;
        const Function* origM = nullptr;
        for (const auto& imp : program.impls) {
            for (const auto& m : imp.methods) {
                if (sameSpan(m.span, targetInfo.span)) { origImp = &imp; origM = &m; break; }
            }
            if (origM) break;
        }
        if (!origM || !origImp) throw Diagnostic("E_SYMBOL", "No existe el símbolo " + filter, {});
        const std::string canonical = implMethodCanonicalName(program, *origImp, *origM);
        targetFnName = canonical;
        const auto view = spanView(program, origM->span);
        page.add("{\"role\":\"target\",\"kind\":\"method\",\"name\":" + jsonString(canonical) +
                 ",\"file\":" + jsonString(program.fileOf(origM->span)) +
                 ",\"signature\":" + jsonString(formatModifiers(*origM) + "fn " + canonical + formatParamsAndReturn(program, *origM)) +
                 ",\"mutParams\":" + mutParamsJson(*origM) +
                 ",\"source\":" + jsonString(view) +
                 ",\"hash\":" + jsonString(fingerprint(view)) + "}");
        addTypeDep(addTypeDep, origImp->target, 0);
        addTypeDep(addTypeDep, origImp->interface, 0);
        for (const auto& cf : checked.functions) {
            if (!cf.isImpl || !sameSpan(cf.span, origM->span)) continue;
            for (const auto& p : cf.params) addTypeDep(addTypeDep, p.type, 0);
            addTypeDep(addTypeDep, cf.result, 0);
            walkSliceBody(cf.body, cf.module);
        }
    } else if (targetInfo.kind == PatchTarget::Kind::Struct || targetInfo.kind == PatchTarget::Kind::Enum) {
        const DataDecl* orig = nullptr;
        for (const auto& d : program.data) if (sameSpan(d.span, targetInfo.span)) { orig = &d; break; }
        if (!orig) throw Diagnostic("E_SYMBOL", "No existe el símbolo " + filter, {});
        const std::string full = program.nameOf(orig->type);
        targetDataName = full;
        const auto view = spanView(program, orig->span);
        page.add("{\"role\":\"target\",\"kind\":" + jsonString(orig->isEnum ? "enum" : "struct") +
                 ",\"name\":" + jsonString(full) +
                 ",\"file\":" + jsonString(program.fileOf(orig->span)) +
                 ",\"source\":" + jsonString(view) +
                 ",\"hash\":" + jsonString(fingerprint(view)) + "}");
        for (const auto& d : checked.data) {
            if (!sameSpan(d.span, orig->span)) continue;
            for (const auto& v : d.variants)
                for (const auto& fld : v.fields) addTypeDep(addTypeDep, fld.type, 0);
        }
    } else if (targetInfo.kind == PatchTarget::Kind::Interface) {
        const InterfaceDecl* orig = nullptr;
        for (const auto& i : program.interfaces) if (sameSpan(i.span, targetInfo.span)) { orig = &i; break; }
        if (!orig) throw Diagnostic("E_SYMBOL", "No existe el símbolo " + filter, {});
        const std::string full = program.nameOf(orig->type);
        targetIfaceName = full;
        const auto view = spanView(program, orig->span);
        page.add("{\"role\":\"target\",\"kind\":\"interface\",\"name\":" + jsonString(full) +
                 ",\"file\":" + jsonString(program.fileOf(orig->span)) +
                 ",\"source\":" + jsonString(view) +
                 ",\"hash\":" + jsonString(fingerprint(view)) + "}");
        for (const auto& i : checked.interfaces) {
            if (!sameSpan(i.span, orig->span)) continue;
            for (const auto& m : i.methods) {
                for (const auto& p : m.params) addTypeDep(addTypeDep, p.type, 0);
                addTypeDep(addTypeDep, m.result, 0);
            }
        }
    } else if (targetInfo.kind == PatchTarget::Kind::Const) {
        const ConstDecl* orig = nullptr;
        for (const auto& c : program.consts) if (sameSpan(c.span, targetInfo.span)) { orig = &c; break; }
        if (!orig) throw Diagnostic("E_SYMBOL", "No existe el símbolo " + filter, {});
        targetConstName = orig->name;
        const auto view = spanView(program, orig->span);
        page.add("{\"role\":\"target\",\"kind\":\"const\",\"name\":" + jsonString(orig->name) +
                 ",\"file\":" + jsonString(program.fileOf(orig->span)) +
                 ",\"source\":" + jsonString(view) +
                 ",\"hash\":" + jsonString(fingerprint(view)) + "}");
        for (const auto& c : checked.consts) {
            if (!sameSpan(c.span, orig->span)) continue;
            addTypeDep(addTypeDep, c.type, 0);
        }
    }

    for (const auto* d : depData) {
        const auto view = spanView(program, d->span);
        page.add("{\"role\":\"dep\",\"kind\":" + jsonString(d->isEnum ? "enum" : "struct") +
                 ",\"name\":" + jsonString(program.nameOf(d->type)) +
                 ",\"file\":" + jsonString(program.fileOf(d->span)) +
                 ",\"source\":" + jsonString(view) +
                 ",\"hash\":" + jsonString(fingerprint(view)) + "}");
    }
    for (const auto* iface : depIfaces) {
        const auto view = spanView(program, iface->span);
        page.add("{\"role\":\"dep\",\"kind\":\"interface\",\"name\":" + jsonString(program.nameOf(iface->type)) +
                 ",\"file\":" + jsonString(program.fileOf(iface->span)) +
                 ",\"source\":" + jsonString(view) +
                 ",\"hash\":" + jsonString(fingerprint(view)) + "}");
    }
    for (const auto* c : depConsts) {
        const auto view = spanView(program, c->span);
        page.add("{\"role\":\"dep\",\"kind\":\"const\",\"name\":" + jsonString(c->name) +
                 ",\"file\":" + jsonString(program.fileOf(c->span)) +
                 ",\"source\":" + jsonString(view) +
                 ",\"hash\":" + jsonString(fingerprint(view)) + "}");
    }
    for (const auto& fn : depFns) {
        page.add("{\"role\":\"dep\",\"kind\":" + jsonString(fn.kind) +
                 ",\"name\":" + jsonString(fn.name) +
                 ",\"file\":" + jsonString(fn.file) +
                 ",\"signature\":" + jsonString(fn.signature) +
                 ",\"mutParams\":" + fn.mutParams +
                 ",\"hash\":" + jsonString(fn.hash) + "}");
    }
}

std::string agentCommand(const std::vector<std::string>& args, const fs::path& file,
                         const std::string& source, Program& program) {
    auto& command = args[0];
    const bool isCosts = (command == "costes" || command == "costs");
    if (command == "fmt") {
        bool verify = args.size() == 3 && args[2] == "--check";
        if (!verify && args.size() != 2) throw Diagnostic("E_USAGE", "veloz fmt ARCHIVO [--check]", {});
        auto formatted = formatSource(source);
        if (formatted != source) {
            if (verify) throw Diagnostic("E_FORMAT", "Ejecuta veloz fmt para normalizar el formato", {});
            parse(formatted);
            publishSource(file, source, formatted);
        }
        return {};
    }
    if (command == "patch" || command == "patch-body") return applyPatch(args, file, source, program);
    auto checked = program;
    check(checked);
    std::string filter;
    std::size_t offset = 0;
    bool hasOffset = false;
    bool explainChecks = false;
    std::optional<int> maxLevel;
    std::optional<std::size_t> maxTokens;
    for (std::size_t arg = 2; arg < args.size(); ++arg) {
        if (args[arg] == "--offset") {
            if (hasOffset || arg + 1 >= args.size())
                throw Diagnostic("E_USAGE", "Usa [SÍMBOLO] [--max-level N] [--explain-checks] [--max-tokens N] [--offset N]", {});
            const auto& value = args[++arg];
            auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), offset);
            if (ec != std::errc{} || end != value.data() + value.size())
                throw Diagnostic("E_USAGE", "Offset inválido", {});
            hasOffset = true;
        } else if (args[arg] == "--explain-checks") {
            if (!isCosts)
                throw Diagnostic("E_USAGE", "La opción --explain-checks solo se admite con costes", {});
            if (explainChecks)
                throw Diagnostic("E_USAGE", "Usa [SÍMBOLO] [--max-level N] [--explain-checks] [--max-tokens N] [--offset N]", {});
            explainChecks = true;
        } else if (args[arg] == "--max-tokens") {
            if (maxTokens || arg + 1 >= args.size())
                throw Diagnostic("E_USAGE", "Usa [SÍMBOLO] [--max-level N] [--max-tokens N] [--offset N]", {});
            const auto& value = args[++arg];
            std::size_t parsed = 0;
            auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec != std::errc{} || end != value.data() + value.size() || parsed == 0)
                throw Diagnostic("E_USAGE", "El límite --max-tokens debe ser un entero positivo", {});
            maxTokens = parsed;
        } else if (args[arg] == "--max-level") {
            if (!isCosts)
                throw Diagnostic("E_USAGE", "La opción --max-level solo se admite con costes, check y build", {});
            if (maxLevel || arg + 1 >= args.size())
                throw Diagnostic("E_USAGE", "Usa [SÍMBOLO] [--max-level N] [--max-tokens N] [--offset N]", {});
            const auto& value = args[++arg];
            int parsed = -1;
            auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec != std::errc{} || end != value.data() + value.size() || parsed < 0 || parsed > 5)
                throw Diagnostic("E_USAGE", "El límite --max-level debe ser un entero entre 0 y 5", {});
            maxLevel = parsed;
        } else if (args[arg].starts_with("--") || !filter.empty()) {
            throw Diagnostic("E_USAGE", "Usa [SÍMBOLO] [--max-level N] [--max-tokens N] [--offset N]", {});
        } else {
            filter = args[arg];
        }
    }
    if (command == "slice" && filter.empty())
        throw Diagnostic("E_USAGE", "veloz slice ARCHIVO SÍMBOLO [--max-tokens N]", {});
    if (!filter.empty()) {
        if (command == "sig" || command == "slice") {
            if (!sigMatchesAny(program, filter))
                throw Diagnostic("E_SYMBOL", "No existe el símbolo " + filter, {});
        } else {
            symbol(program, filter);
        }
    }
    Page page{offset, 0, {}, {}, 7168, false, {}};
    if (maxTokens) {
        page.maxBytes = *maxTokens * 4;
        page.customBudget = true;
    }
    page.hintPrefix = "veloz " + command + " " + args[1];
    if (!filter.empty()) page.hintPrefix += " " + filter;
    if (explainChecks) page.hintPrefix += " --explain-checks";
    if (maxLevel) page.hintPrefix += " --max-level " + std::to_string(*maxLevel);
    if (maxTokens) page.hintPrefix += " --max-tokens " + std::to_string(*maxTokens);

    if (command == "sig") {
        const auto declaredEffects = effects(program);
        const auto irFx = analyzeProgramEffects(checked);

        for (const auto& c : program.consts) {
            if (!filter.empty() && c.name != filter && c.sourceName != filter) continue;
            const auto owner = program.sourceOf(c.span);
            auto view = owner.substr(c.span.start, c.span.end - c.span.start);
            page.add("{\"name\":" + jsonString(c.name) + ",\"kind\":\"const\",\"signature\":" + jsonString(view) +
                     ",\"doc\":" + jsonString(docLine(owner, c.span)) +
                     ",\"hash\":" + jsonString(fingerprint(view)) + "}");
        }
        for (const auto& d : program.data) {
            const std::string full = program.nameOf(d.type);
            if (!filter.empty() && full != filter && stripModulePrefix(full) != filter) continue;
            // All fields and payloads are part of the module's public signature.
            const auto owner = program.sourceOf(d.span);
            auto view = owner.substr(d.span.start, d.span.end - d.span.start);
            page.add("{\"name\":" + jsonString(full) + ",\"kind\":" + jsonString(d.isEnum ? "enum" : "struct") +
                     ",\"signature\":" + jsonString(view) +
                     ",\"doc\":" + jsonString(docLine(owner, d.span)) +
                     ",\"hash\":" + jsonString(fingerprint(view)) + "}");
        }
        for (const auto& declaration : program.interfaces) {
            const std::string full = program.nameOf(declaration.type);
            if (!filter.empty() && full != filter && stripModulePrefix(full) != filter) continue;
            const auto owner = program.sourceOf(declaration.span);
            auto view = owner.substr(declaration.span.start, declaration.span.end - declaration.span.start);
            page.add("{\"name\":" + jsonString(full) + ",\"kind\":\"interface\",\"signature\":" + jsonString(view) +
                     ",\"doc\":" + jsonString(docLine(owner, declaration.span)) +
                     ",\"hash\":" + jsonString(fingerprint(view)) + "}");
        }
        for (const auto& imp : program.impls) {
            for (const auto& m : imp.methods) {
                bool isExact = false;
                if (!filter.empty() && !implMethodMatches(program, imp, m, filter, isExact)) continue;
                bool io = false, alloc = false, trap = false, foundConcrete = false;
                for (const auto& cf : checked.functions) {
                    if (!cf.isImpl || cf.span.file != m.span.file ||
                        cf.span.start != m.span.start || cf.span.end != m.span.end) continue;
                    if (auto it = irFx.find(cf.name); it != irFx.end()) {
                        io |= it->second.io;
                        alloc |= it->second.alloc;
                        trap |= it->second.trap;
                        foundConcrete = true;
                    }
                }
                if (!foundConcrete) {
                    const auto afx = astEffects(program, m.body, imp.module);
                    io = afx.io;
                    alloc = afx.alloc;
                    trap = afx.trap;
                }
                const std::string canonical = implMethodCanonicalName(program, imp, m);
                const std::string sigStr = formatModifiers(m) + "fn " + canonical + formatParamsAndReturn(program, m);
                const auto owner = program.sourceOf(m.span);
                auto view = owner.substr(m.span.start, m.span.end - m.span.start);
                page.add("{\"name\":" + jsonString(canonical) + ",\"kind\":\"method\",\"signature\":" + jsonString(sigStr) +
                         ",\"safety\":" + jsonString(safetyName(m.safety)) +
                         ",\"doc\":" + jsonString(docLine(owner, m.span)) +
                         ",\"io\":" + (io ? "true" : "false") +
                         ",\"mayTrap\":" + (trap ? "true" : "false") +
                         ",\"effects\":" + effectsJson(io, alloc, trap) +
                         ",\"inferredEffects\":" + effectsJson(io, alloc, trap) +
                         ",\"mutParams\":" + mutParamsJson(m) +
                         ",\"hash\":" + jsonString(fingerprint(view)) + "}");
            }
        }
        for (const auto& f : program.functions) {
            if (f.isTest || f.isImpl ||
                (!filter.empty() && f.name != filter && f.sourceName != filter)) continue;
            bool io = false, alloc = false, trap = false;
            if (f.isExternDecl) {
                io = alloc = trap = true;
            } else if (f.generics.empty()) {
                if (auto it = irFx.find(f.name); it != irFx.end()) {
                    io = it->second.io;
                    alloc = it->second.alloc;
                    trap = it->second.trap;
                } else if (auto ait = declaredEffects.find(f.name); ait != declaredEffects.end()) {
                    io = ait->second.io;
                    alloc = ait->second.alloc;
                    trap = ait->second.trap;
                }
            } else {
                bool foundInstance = false;
                for (const auto& instance : checked.functions) {
                    if (!instance.isSpecialization || !instance.name.starts_with(f.name + ".g")) continue;
                    if (auto it = irFx.find(instance.name); it != irFx.end()) {
                        io |= it->second.io;
                        alloc |= it->second.alloc;
                        trap |= it->second.trap;
                        foundInstance = true;
                    }
                }
                if (!foundInstance) {
                    const auto& afx = declaredEffects.at(f.name);
                    io = afx.io;
                    alloc = afx.alloc;
                    trap = afx.trap;
                }
            }
            const auto owner = program.sourceOf(f.span);
            auto hash = fingerprint(owner.substr(f.span.start, f.span.end - f.span.start));
            page.add("{\"name\":" + jsonString(f.name) + ",\"kind\":\"fn\",\"signature\":" + jsonString(signature(program, f)) +
                ",\"safety\":" + jsonString(safetyName(f.safety)) +
                ",\"doc\":" + jsonString(docLine(owner, f.span)) + ",\"io\":" + (io ? "true" : "false") +
                ",\"mayTrap\":" + (trap ? "true" : "false") + ",\"effects\":" + effectsJson(io, alloc, trap) +
                ",\"inferredEffects\":" + effectsJson(io, alloc, trap) +
                ",\"mutParams\":" + mutParamsJson(f) +
                ",\"hash\":" + jsonString(hash) + "}");
        }
    } else if (command == "slice") {
        emitSymbolSlice(program, checked, filter, page);
    } else if (isCosts) {
        const auto sites = collectMemorySites(checked, filter);
        if (maxLevel) verifyMemoryLevels(sites, *maxLevel);
        if (explainChecks) {
            const auto checks = collectCheckSites(checked, filter);
            for (const auto& ck : checks) {
                page.add("{\"function\":" + jsonString(ck.function) + ",\"kind\":\"check\",\"check\":" + jsonString(ck.check) +
                         ",\"status\":" + jsonString(ck.status) + ",\"file\":" + jsonString(ck.file) +
                         ",\"start\":" + std::to_string(ck.span.start) + ",\"end\":" + std::to_string(ck.span.end) +
                         ",\"reason\":" + jsonString(ck.reason) + "}");
            }
        } else {
            for (const auto& site : sites) {
                page.add("{\"function\":" + jsonString(site.function) + ",\"kind\":" + jsonString(site.kind) +
                         ",\"file\":" + jsonString(site.file) +
                         ",\"start\":" + std::to_string(site.span.start) + ",\"end\":" + std::to_string(site.span.end) +
                         ",\"level\":" + std::to_string(site.level) + ",\"reason\":" + jsonString(site.reason) + "}");
            }
        }
    } else {
        for (const auto& f : checked.functions) {
            if ((f.isImpl && filter.empty()) || (!filter.empty() && (f.isTest ||
                (f.name != filter && f.sourceName != filter && !f.name.starts_with(filter+".g"))))) continue;
            auto emit = [&](Span s, Type t) {
                page.add("{\"function\":" + jsonString(f.name) + ",\"file\":" + jsonString(checked.fileOf(s)) +
                    ",\"start\":" + std::to_string(s.start) +
                    ",\"end\":" + std::to_string(s.end) + ",\"type\":" + jsonString(checked.nameOf(t)) + "}");
            };
            for (const auto& p : f.params) emit(p.span, p.type);
            auto visit = [](const Stmt&) {};
            auto expr = [&](const Expr& e){ emit(e.span, e.type); };
            walk(f.body, visit, expr);
        }
    }
    return page.finish();
}
std::string explain(std::string_view topic) {
    static const std::map<std::string_view, std::string_view> index{
        {"E_PYTHON_DEF","Copia la firma con fn. Después del tipo de retorno abre una llave, escribe return para devolver el valor y cierra la llave."},
        {"R_ARRAY_SIZE","La longitud de repeat es un entero mayor o igual que cero."},
        {"R_ALLOC","La implementación admite hasta 2²⁴ enteros por arreglo y comprueba el resultado del asignador."},
        {"R_BOUNDS","Verifica que el índice cumple 0 <= i y que i es estrictamente menor que len(a)."},
        {"E_ARRAY_FIELD","Los arreglos de este núcleo se admiten como variables, parámetros y resultados de función."},
        {"E_ARITY","Cuenta los parámetros de la firma y los argumentos de la llamada. La posición de la firma aparece en related."},
        {"E_BUSY","Otra edición mantiene el bloqueo del archivo. Espera a que termine y vuelve a leer el símbolo."},
        {"E_CHAR","Revisa el carácter indicado por span. Las palabras y operadores de la gramática usan ASCII."},
        {"E_CLANG","Comprueba que el driver LLVM de VELOZ_CLANG puede enlazar un programa para este equipo."},
        {"E_CONSTRUCTOR","Usa Point { x: 1 } para un struct y Tipo.Variante(valor) para la variante declarada de un enum."},
        {"E_DEPTH","Divide la expresión o el bloque en partes para mantenerse dentro del máximo de 96 niveles de sintaxis."},
        {"E_DUP_ARM","Un brazo anterior ya cubre esta variante. Cada variante aparece una sola vez en el match."},
        {"E_DUP_FIELD","Cada campo tiene un nombre único y se inicializa una sola vez en el constructor."},
        {"E_DUP_FUNCTION","Cada función o prueba necesita un nombre único en su espacio de nombres. Revisa el tramo relacionado."},
        {"E_DUP_GENERIC","Cada parámetro de tipo debe declararse una sola vez entre los signos de menor y mayor."},
        {"E_DUP_NAME","Dos nombres coinciden dentro de un bloque. Consulta related para localizar la primera declaración."},
        {"E_DUP_TYPE","Los tipos nominales comparten un espacio de nombres. Elige un nombre único para el struct o enum."},
        {"E_DUP_VARIANT","Cada variante de un enum requiere un nombre diferente."},
        {"E_ENUM_EMPTY","Declara al menos una variante, por ejemplo enum State { Ready }."},
        {"E_EQUALITY","Compara explícitamente los campos escalares que definen la igualdad de tu tipo nominal."},
        {"E_ESCAPE","Dentro de una cadena puedes escapar las comillas, barra inversa, tabulador o los saltos de línea y carro."},
        {"E_FIELD","Revisa los campos declarados con veloz sig ARCHIVO TIPO y usa su nombre exacto."},
        {"E_FORMAT","Ejecuta veloz fmt sobre el archivo y vuelve a pasar la comprobación."},
        {"E_FRAME","Reduce los valores intermedios o divide la función. El presupuesto conservador por función es de 16 KiB."},
        {"E_FUNCTION","Comprueba el nombre de la función con veloz sig. Declara una función de nivel superior si falta."},
        {"E_HOST","Falló una operación del sistema anfitrión. El campo message contiene la causa disponible."},
        {"E_INT_RANGE","Los literales enteros se escriben en decimal dentro de [-9223372036854775808, 9223372036854775807]."},
        {"E_IO","Verifica existencia, permisos, espacio y disponibilidad del archivo indicado en message."},
        {"E_MAIN","Para run o build incluye fn main() {} con cero parámetros y retorno void."},
        {"E_MATCH_TYPE","El discriminante del match y el nombre de tipo de cada variante deben pertenecer al mismo enum."},
        {"E_OUTPUT","Elige un nombre de ejecutable distinto de la fuente de entrada."},
        {"E_PATCH","El archivo de reemplazo debe contener un solo símbolo compatible (función, método, const, struct, enum o interface) con el mismo nombre."},
        {"E_PATTERN_ARITY","El patrón requiere un nombre por campo de la variante, en el orden de su declaración."},
        {"E_EFFECT","La función viola un modificador de efecto requerido (pure sin E/S, noalloc sin heap o checkfree sin comprobaciones en ejecución). Consulta veloz sig ARCHIVO o veloz costes ARCHIVO."},
        {"E_PRINT","print admite int, bool y str. Para imprimir un agregado selecciona un campo escalar."},
        {"E_RESERVED","Elige un identificador que no sea una palabra reservada ni una primitiva de la biblioteca."},
        {"E_STRING","Cierra el literal en la misma línea. Representa los controles con secuencias de escape."},
        {"E_SYMBOL","Consulta veloz sig ARCHIVO para obtener nombres declarados y después filtra por el símbolo exacto."},
        {"E_TOPIC","Usa el identificador estable que aparece en el campo id del diagnóstico."},
        {"E_UNKNOWN_TYPE","Declara el tipo nominal en el mismo archivo o elige un escalar del núcleo."},
        {"E_USAGE","veloz --help muestra los argumentos y el orden de cada comando."},
        {"E_UTF8","El archivo de origen debe contener una secuencia válida de bytes UTF-8."},
        {"E_VALUE","Esta posición requiere una expresión que devuelva un valor. Una llamada void puede ir como sentencia."},
        {"E_TYPE","Comprueba el tipo esperado, el obtenido y el tramo relacionado. No hay conversiones implícitas."},
        {"E_NAME","Declara el nombre en el ámbito actual o en uno exterior. Los valores usan camelCase ASCII."},
        {"E_PARSE","Revisa expected, actual y span. Separa las sentencias con un salto de línea."},
        {"E_IMMUTABLE","Declara con var el local o parámetro escalar que necesitas reasignar. El índice de for es de solo lectura."},
        {"E_PARAM_VAR","var en una firma declara una copia local modificable de un int o un bool."},
        {"E_LET_MUT","Sustituye let mut por var. La edición exacta va en fix."},
        {"E_LENGTH","len(texto) devuelve el número de bytes UTF-8."},
        {"E_SEMICOLON","Separa dos sentencias con un salto de línea. Elimina el punto y coma final."},
        {"E_PRINTLN","print(valor) escribe el valor y un salto de línea."},
        {"E_ELIF","Escribe } else if condición { en la misma línea."},
        {"E_RETURN","Cada camino de una función con resultado debe devolver un valor del tipo declarado."},
        {"E_GENERIC","Declara fn id<T>(x: T) -> T. Cada tipo genérico se deduce de los argumentos y debe funcionar como tipo opaco. Añade <T: modulo.Interfaz> para exigir una impl."},
        {"E_PROJECT","Coloca las fuentes bajo src y declara package y version en veloz.pkg; un archivo independiente no admite imports."},
        {"E_MANIFEST","Usa package NOMBRE, version MAJOR.MINOR.PATCH y require NOMBRE VERSION \"RUTA_RELATIVA\", sin líneas adicionales."},
        {"E_PACKAGE_PATH","La ruta require debe ser relativa al manifiesto, existir y contener otro veloz.pkg."},
        {"E_PACKAGE_VERSION","El nombre y la versión del manifiesto destino deben coincidir exactamente con require."},
        {"E_PACKAGE_CONFLICT","Todas las rutas del diamante deben resolver el mismo paquete y versión."},
        {"E_PACKAGE_CYCLE","Divide el ciclo de dependencias de paquetes antes de compilar."},
        {"E_PACKAGE_DUP","Declara una sola línea require por paquete."},
        {"E_MODULE_NAME","El archivo src/nombre.turi usa un nombre camelCase ASCII."},
        {"E_DUP_MODULE","Cada nombre de módulo es único en todo el grafo, también entre paquetes."},
        {"E_DUP_IMPORT","Elimina el import repetido."},
        {"E_SIMD_TYPE","Simd<T, N> requiere un tipo numérico escalar (i8..u64, f32, float), N en {2, 4, 8, 16, 32} y tamaño total entre 8 y 64 bytes (ejemplo: Simd<f32, 4>)."},
        {"E_VEC_NAME","Turi usa [T] para arreglos dinámicos (o Simd<T, N> para vectores SIMD) en lugar de Vec."},
        {"R_ALIGN","La dirección pasada a simdLoadAligned/simdStoreAligned debe ser múltiplo de la alineación del vector; reserva el búfer con ptrAllocAligned."},
        {"E_IMPORT","Declara el import al inicio y solo para un módulo del paquete o de una dependencia directa."},
        {"E_IMPORT_TRANSITIVE","Añade una require directa y exacta al paquete que contiene el módulo."},
        {"E_IMPORT_CYCLE","Elimina una arista del ciclo de imports entre módulos."},
        {"E_IMPORT_ORDER","Mueve todos los imports antes de cualquier declaración."},
        {"E_INTERFACE","Los métodos no son genéricos; Self aparece en parámetros y no en el resultado."},
        {"E_DUP_INTERFACE","Cada interfaz tiene un nombre único en su módulo."},
        {"E_INTERFACE_UNKNOWN","Importa y escribe el nombre cualificado de una interfaz declarada."},
        {"E_DUP_IMPL","Solo puede existir una impl para cada par interfaz y tipo."},
        {"E_IMPL_METHOD_EXTRA","Retira el método adicional: una impl reproduce exactamente el conjunto de la interfaz."},
        {"E_IMPL_METHOD_MISSING","Añade el método indicado con la firma obtenida al sustituir Self."},
        {"E_IMPL_SIGNATURE","Iguala aridad, var, tipos y resultado tras sustituir Self por el destino."},
        {"E_IMPL_MISSING","Declara una impl explícita de la interfaz constraint para el tipo concreto instanciado."},
        {"E_OVERLOAD","No hay sobrecarga: haz que el nombre del método identifique un único constraint."},
        {"E_EXHAUSTIVE","Incluye un brazo por variante del enum. El compilador comprueba la cobertura incluso si el valor parece constante."},
        {"E_LAYOUT","Reduce la profundidad o el tamaño de los tipos contenidos por valor."},
        {"E_TYPE_CYCLE","Un ciclo de campos por valor necesitaría tamaño infinito. Rompe el ciclo usando Handle<T> con Pool<T>, o Shared<T> para estructuras persistentes acíclicas."},
        {"R_STEPS","El intérprete agotó su presupuesto. veloz run archivo --steps N permite cambiarlo."},
        {"R_DEPTH","La llamada superó 256 marcos activos. Reescribe la recursión como bucle."},
        {"R_OVERFLOW","El resultado matemático queda fuera de int64. Comprueba el rango antes de la operación."},
        {"R_DIV_ZERO","El segundo operando de / o % debe ser distinto de cero."},
        {"R_ASSERT","La expresión de assert dio false. Reproduce el caso usando veloz check o veloz run."},
        {"E_STALE","Obtén el hash actual con veloz sig y revisa la nueva versión antes de editar."},
        {"E_CONST_TYPE","Toda constante global necesita tipo explícito: const limite: int = 100."},
        {"E_DUP_CONST","Ya existe otra constante o función con ese nombre en el módulo."},
        {"E_CONST_CYCLE","Una constante no puede depender directa ni indirectamente de sí misma."},
        {"E_ENUM_VALUE","Solo las variantes sin campos pueden asignar un valor entero constante."},
        {"E_DUP_ENUM_VALUE","Cada variante de un enum con valores enteros debe tener un número único."},
        {"E_LOOP_CONTROL","break y continue requieren estar dentro de un bucle y coincidir con su 'etiqueta si se indica."},
        {"E_DUP_LABEL","Usa nombres distintos para etiquetas de bucles anidados."},
        {"E_GOTO","Sustituye goto por break 'etiqueta o continue 'etiqueta."},
        {"E_PARAM_MUT","Declara parámetros por referencia como nombre: mut Tipo."},
        {"E_REF_MUT","En la llamada pasa mut nombre directamente sin &."},
        {"E_MUT_REQUIRED","Añade mut delante del argumento correspondiente al parámetro mut."},
        {"E_MUT_ARG","Solo pasa mut cuando el parámetro de la firma lo declara explícitamente."},
        {"E_ALIAS_MUT","Una variable o subcampo pasado como mut no puede aparecer repetido ni solaparse con otro argumento del mismo dueño en la misma llamada; revisa el tramo related."},
        {"E_EMPTY_ARRAY","Especifica el tipo de un arreglo vacío, por ejemplo let a: [int] = []."},
        {"E_FSTRING","En Turi las cadenas con comillas dobles interpolan directamente con \"{x}\"; quita el prefijo f."},
        {"E_NULL","Turi no tiene null; usa None con un tipo opcional T? (por ejemplo let x: int? = None)."},
        {"E_EXCEPTIONS","Turi no usa try/catch ni throw; devuelve T? o Result<T, E> y propaga con match o ?."},
        {"E_INFER_TYPE","No se pudo inferir algún argumento de tipo; anota la variable o especifícalo explícitamente entre <...>."},
        {"E_TRY_TYPE","El operador ? requiere un valor de tipo T? o Result<T, E>."},
        {"E_TRY_RETURN","Para usar ? aquí, el resultado de la función debe ser compatible (T? o Result<_, E>)."},
        {"E_TRY_ERR","El tipo de error E del Result usado con ? no coincide con el Err declarado en la función."},
        {"R_PARSE","El texto no tiene un formato válido para convertirse al tipo numérico solicitado."},
        {"E_KEY_TYPE","Map<K, V> y Set<T> requieren claves hashables: int, str, bool, enum sin campos o Handle<T>."},
        {"E_MAP_INDEX","En Turi Map<K, V> usa mapGet(m, k) y mapSet(mut m, k, v) en lugar de corchetes m[k]."},
        {"E_MAP_NAME","Usa Map<K, V> con mapNew() / mapWithCap(n) en lugar de HashMap o Dict."},
        {"E_SET_NAME","Usa Set<T> con setNew() / setWithCap(n) en lugar de HashSet."},
        {"E_POOL_INDEX","En Turi Pool<T> usa poolGet(p, h) y poolSet(mut p, h, v) en lugar de corchetes p[h]."},
        {"E_POOL_NAME","Usa Pool<T> con poolNew() / poolWithCap(n) y Handle<T> en lugar de Arena, SlotMap o Slab."},
        {"E_RC_NAME","Usa Shared<T> con sharedNew(v) / sharedGet(s) / sharedSet(mut s, v) en lugar de Rc, Arc o SharedPtr."},
        {"E_MEMORY_LEVEL","Un valor supera el límite indicado con --max-level N. Consulta veloz costes ARCHIVO FUNCIÓN para ver cada nivel (2=region, 3=owner/clone, 4=pool, 5=rc)."},
        {"E_DYN_TYPE","dyn solo acepta una interfaz no genérica como dyn Interfaz."},
        {"E_DYN_SAFETY","Una interfaz usada como dyn no puede tener métodos asociados sin self ni recibir/devolver Self salvo mutación por self."},
        {"E_SELF_MUT","Usa self o mut self en parámetros de métodos dentro de impl/interface, no &self ni &mut self."},
        {"E_METHOD_UNKNOWN","No existe ese método ni impl compatible para este tipo; usa las funciones estándar o decláralo en un impl."},
        {"E_METHOD_STATIC","Este método es asociado y requiere llamarse como Tipo.metodo(...) en lugar de sobre un valor."},
        {"E_IMPL_COHERENCE","Ya existe una implementación de esta interfaz para este tipo, o el tipo no pertenece a este módulo."},
        {"E_IMPL_INHERENT","Ya existe un método inherente con ese nombre sobre este tipo, o un impl viola las reglas del módulo."},
        {"E_CLOSURE_PARAM","Las closures capturan por valor y sus parámetros no admiten mut ni mut self."},
        {"E_CLOSURE_SYNTAX","En Turi las closures se escriben como |x: int| expr o || expr (sin lambda, => ni fn anónimo)."},
        {"E_CLOSURE_CAPTURE_MUT","Las variables capturadas en una closure son inmutables; crea 'var copia = ...' dentro si necesitas mutarla."},
        {"E_CALL_TYPE","Solo se pueden invocar funciones o valores de tipo fn(...)."},
        {"E_USE_AFTER_MOVE","Un recurso movido (File, TcpListener, Socket, Mutex, Lock, Scope, Task o struct con alguno) no puede volver a usarse ni moverse hasta reasignar la variable."},
        {"E_RESOURCE_COPY","Los recursos y los valores que los contienen no se pueden copiar, repetir en arreglos ni meter en Map, Pool, Shared o Frozen; pásalos por referencia (f(x)) o muévelos una sola vez."},
        {"E_RESOURCE_NAME","Usa los tipos de recurso de Turi: File, TcpListener, Socket, Mutex, Lock y Scope."},
        {"E_SEND","Solo los valores enviables entre hilos pueden capturarse o devolverse en tareas (usa freeze(x), Mutex o taskSpawnMove)."},
        {"E_CONC_NAME","Usa concurrencia estructurada de Turi: taskScope(), taskSpawn, taskSpawnMove, taskJoin, parallelMap y freeze."},
        {"E_ATOMIC_TYPE","Atomic<T> solo admite bool o tipos enteros (i8..u64, int), y las operaciones aritméticas atómicas requieren tipo entero."},
        {"E_ATOMIC_NAME","Turi usa Atomic<T> con atomicNew(val) en lugar de AtomicInt/AtomicU64/AtomicBool."},
        {"E_ORDERING","Los ordenamientos de memoria válidos son literales de cadena: \"relaxed\", \"acquire\", \"release\", \"acqRel\" y \"seqCst\" (según la operación)."},
        {"E_CONTRACT","requires/ensures van tras -> Tipo antes de {, evalúan a bool sin mutar variables, mover recursos ni usar '?' ; result solo existe en ensures de funciones con retorno."},
        {"R_CONTRACT","Falló en ejecución una precondición requires o postcondición ensures. Usa veloz costes ARCHIVO --explain-checks para ver qué guardas se conservan."},
        {"E_NOCOPY","La función @noCopy realiza una copia en heap o valor no elidida; pasa por referencia, usa mut o elimina copias innecesarias."},
        {"E_IF_EXPR","Un if usado como expresión requiere rama else con bloques del mismo tipo."},
        {"E_EXTREME","--extreme requiere Python y tools/superopt/superopt.py (o VELOZ_SUPEROPT) y conserva siempre un respaldo verificado."},
        {"E_GPU","No se pudo inicializar OpenCL ni compilar los kernels GPU en este equipo; comprueba disponibilidad con gpu.disponible()."}
    };
    if (auto found = index.find(topic); found != index.end()) return std::string(found->second);
    throw Diagnostic("E_TOPIC", "No hay una explicación registrada para " + std::string(topic), {});
}
}
