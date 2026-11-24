#pragma once

#include "native_ir.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace veloz::native {

inline constexpr std::int64_t kMinInt = std::numeric_limits<std::int64_t>::min();
inline constexpr std::int64_t kMaxInt = std::numeric_limits<std::int64_t>::max();

inline std::optional<std::int64_t> parseConstInt(const std::string& text) {
    std::int64_t v{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc{} || ptr != text.data() + text.size()) return {};
    return v;
}

inline std::optional<std::int64_t> checkedAdd64(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > kMaxInt - b) || (b < 0 && a < kMinInt - b)) return {};
    return a + b;
}

inline std::optional<std::int64_t> checkedSub64(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a < kMinInt + b) || (b < 0 && a > kMaxInt + b)) return {};
    return a - b;
}

inline std::optional<std::int64_t> checkedMul64(std::int64_t a, std::int64_t b) {
    if (a > 0 && ((b > 0 && a > kMaxInt / b) || (b < 0 && b < kMinInt / a))) return {};
    if (a < 0 && ((b > 0 && a < kMinInt / b) || (b < 0 && a < kMaxInt / b))) return {};
    return a * b;
}

inline bool isConstantRange(const IntRange& r, std::int64_t& out) {
    if (r.lower == r.upper) {
        out = r.lower;
        return true;
    }
    return false;
}

inline bool fitsSigned16(const IntRange& r) { return r.lower >= -32768 && r.upper <= 32767; }
inline bool fitsSigned32(const IntRange& r) { return r.lower >= -2147483648LL && r.upper <= 2147483647LL; }
inline bool fitsUnsigned32(const IntRange& r) { return r.lower >= 0 && r.upper <= 4294967295LL; }

// ---------------------------------------------------------------------------
// Pass 1a: Recognize low / high / dist 3-block (or trampoline-normalized) return diamonds as Min/Max/AbsDiff
// ---------------------------------------------------------------------------
inline void recognizeMinMaxAbs(FunctionIR& fn) {
    if (fn.blocks.size() < 3 || fn.blocks.size() > 6 || fn.source.params.size() != 2 ||
        fn.source.params[0].byRef || fn.source.params[1].byRef ||
        fn.source.params[0].type != Type::Int || fn.source.params[1].type != Type::Int ||
        fn.source.result != Type::Int) return;
    auto resolveTarget = [&](unsigned start) -> unsigned {
        unsigned cur = start;
        for (int step = 0; step < 4 && cur < fn.blocks.size(); ++step) {
            if (fn.blocks[cur].ops.empty() && fn.blocks[cur].exit == Exit::Jump &&
                fn.blocks[cur].yes != cur) {
                cur = fn.blocks[cur].yes;
            } else break;
        }
        return cur;
    };
    const auto& b0 = fn.blocks[0];
    if (b0.exit != Exit::Branch || !b0.value || b0.ops.empty()) return;
    const unsigned yIdx = resolveTarget(b0.yes);
    const unsigned nIdx = resolveTarget(b0.no);
    if (yIdx == nIdx || yIdx >= fn.blocks.size() || nIdx >= fn.blocks.size() || yIdx == 0 || nIdx == 0)
        return;
    for (unsigned bi = 1; bi < fn.blocks.size(); ++bi) {
        if (bi == yIdx || bi == nIdx) continue;
        if (!fn.blocks[bi].ops.empty() || fn.blocks[bi].exit != Exit::Jump) return;
    }
    const auto& b1 = fn.blocks[yIdx];
    const auto& b2 = fn.blocks[nIdx];
    if (b1.exit != Exit::Return || !b1.value || b2.exit != Exit::Return || !b2.value) return;

    const Operation* cmp = nullptr;
    for (const auto& op : b0.ops) {
        if (op.op == Op::Load) continue;
        if (op.op == Op::Binary && op.result.id == b0.value->id) { cmp = &op; continue; }
        return;
    }
    if (!cmp || cmp->inputs.size() != 2) return;

    auto loadedSlot = [&](const Block& b, unsigned valId) -> std::optional<unsigned> {
        for (const auto& op : b.ops)
            if (op.op == Op::Load && op.result.id == valId && op.slot < 2) return op.slot;
        return {};
    };
    const auto cmpL = loadedSlot(b0, cmp->inputs[0].id);
    const auto cmpR = loadedSlot(b0, cmp->inputs[1].id);
    if (!cmpL || !cmpR || *cmpL == *cmpR) return;

    auto simpleReturnSlot = [&](const Block& b) -> std::optional<unsigned> {
        if (b.ops.size() != 1) return {};
        return loadedSlot(b, b.value->id);
    };
    if (auto s1 = simpleReturnSlot(b1), s2 = simpleReturnSlot(b2); s1 && s2 && *s1 != *s2) {
        const auto& pred = cmp->data;
        const bool minMatch =
            ((pred == "<" || pred == "<=") && *s1 == *cmpL && *s2 == *cmpR) ||
            ((pred == ">" || pred == ">=") && *s1 == *cmpR && *s2 == *cmpL);
        const bool maxMatch =
            ((pred == ">" || pred == ">=") && *s1 == *cmpL && *s2 == *cmpR) ||
            ((pred == "<" || pred == "<=") && *s1 == *cmpR && *s2 == *cmpL);
        if (minMatch || maxMatch) {
            Block single;
            Value a{Type::Int, 0}, b{Type::Int, 1}, res{Type::Int, 2};
            single.ops.push_back({Op::Load, a, {}, {}, 0, fn.source.span});
            single.ops.push_back({Op::Load, b, {}, {}, 1, fn.source.span});
            single.ops.push_back({minMatch ? Op::Min : Op::Max, res, {a, b}, {}, 0, cmp->span});
            single.exit = Exit::Return;
            single.value = res;
            fn.blocks.clear();
            fn.blocks.push_back(std::move(single));
            fn.slots.resize(2);
            fn.nextValue = 3;
            return;
        }
    }

    auto diffSlots = [&](const Block& b) -> std::optional<std::pair<unsigned, unsigned>> {
        if (b.ops.size() != 3) return {};
        const Operation* sub = nullptr;
        for (const auto& op : b.ops) {
            if (op.op == Op::Load) continue;
            if (op.op == Op::Binary && op.data == "-" && op.result.id == b.value->id) { sub = &op; continue; }
            return {};
        }
        if (!sub) return {};
        auto l = loadedSlot(b, sub->inputs[0].id);
        auto r = loadedSlot(b, sub->inputs[1].id);
        if (!l || !r || *l == *r) return {};
        return std::pair{*l, *r};
    };
    if (auto d1 = diffSlots(b1), d2 = diffSlots(b2); d1 && d2) {
        const auto& pred = cmp->data;
        const bool absMatch =
            ((pred == ">=" || pred == ">") && d1->first == *cmpL && d1->second == *cmpR &&
             d2->first == *cmpR && d2->second == *cmpL) ||
            ((pred == "<=" || pred == "<") && d1->first == *cmpR && d1->second == *cmpL &&
             d2->first == *cmpL && d2->second == *cmpR);
        if (absMatch) {
            Block single;
            Value a{Type::Int, 0}, b{Type::Int, 1}, res{Type::Int, 2};
            single.ops.push_back({Op::Load, a, {}, {}, 0, fn.source.span});
            single.ops.push_back({Op::Load, b, {}, {}, 1, fn.source.span});
            single.ops.push_back({Op::AbsDiff, res, {a, b}, {}, 0, b1.ops.back().span});
            single.exit = Exit::Return;
            single.value = res;
            fn.blocks.clear();
            fn.blocks.push_back(std::move(single));
            fn.slots.resize(2);
            fn.nextValue = 3;
        }
    }
}

// ---------------------------------------------------------------------------
// Pass 1b: Small-function inliner (respecting safeDepths for R_DEPTH parity)
// ---------------------------------------------------------------------------
inline bool isScalarType(Type t) {
    return t == Type::Int || t == Type::Bool || t == Type::Void;
}

inline bool isInlineCandidate(const FunctionIR& fn, std::size_t idx,
                              const std::vector<bool>& safeDepths,
                              const std::string& entryMain) {
    if (fn.source.isExternC || idx >= safeDepths.size() || !safeDepths[idx] || fn.source.name == entryMain || fn.usesRegion) return false;
    if (!isScalarType(fn.source.result)) return false;
    for (const auto& p : fn.source.params)
        if (p.byRef || !isScalarType(p.type) || p.type == Type::Void) return false;
    for (auto t : fn.slots)
        if (!isScalarType(t)) return false;
    if (fn.blocks.size() > 16) return false;
    std::size_t ops = 0;
    for (const auto& b : fn.blocks) {
        if (b.exit == Exit::Open) return false;
        ops += b.ops.size();
        for (const auto& op : b.ops) {
            switch (op.op) {
            case Op::Call:
            case Op::ExternCall:
            case Op::FnPtr:
            case Op::Builtin:
            case Op::BoxDyn:
            case Op::DynCall:
            case Op::MakeClosure:
            case Op::ClosureCall:
            case Op::ArrayLit:
            case Op::Interp:
            case Op::Slice:
            case Op::FieldSet:
            case Op::PlaceSet:
            case Op::RepeatHeap:
            case Op::RepeatRegion:
            case Op::Clone:
            case Op::Drop:
            case Op::ClearOwner:
            case Op::ClearField:
            case Op::ClearIndex:
            case Op::RegionMark:
            case Op::RegionRewind:
            case Op::Construct:
            case Op::Field:
            case Op::Tag:
            case Op::Index:
            case Op::IndexSet:
            case Op::Length:
            case Op::TileRepeat:
            case Op::FindFirst:
            case Op::GemmF32:
            case Op::MatMulMod:
            case Op::FloydStepU16:
            case Op::ArgMinU16:
                return false;
            default: break;
            }
        }
    }
    if (ops > 48 || fn.blocks.empty()) return false;
    // Do not inline looped functions so compute kernels retain dedicated .fast clones.
    std::vector<std::uint8_t> color(fn.blocks.size(), 0);
    auto hasCycle = [&](auto& self, unsigned u) -> bool {
        color[u] = 1;
        auto edge = [&](unsigned v) -> bool {
            if (v >= fn.blocks.size()) return false;
            if (color[v] == 1) return true;
            return color[v] == 0 && self(self, v);
        };
        const auto& b = fn.blocks[u];
        if (b.exit == Exit::Jump && edge(b.yes)) return true;
        if (b.exit == Exit::Branch && (edge(b.yes) || edge(b.no))) return true;
        if (b.exit == Exit::Switch) {
            if (edge(b.yes)) return true;
            for (unsigned t : b.targets)
                if (edge(t)) return true;
        }
        color[u] = 2;
        return false;
    };
    return !hasCycle(hasCycle, 0);
}

inline void replaceValueIdInFunction(FunctionIR& fn, unsigned oldId, unsigned newId) {
    auto fix = [&](Value& v) {
        if (v.id == oldId) v.id = newId;
    };
    for (auto& b : fn.blocks) {
        for (auto& op : b.ops) {
            for (auto& in : op.inputs) fix(in);
        }
        if (b.value) fix(*b.value);
    }
}

inline void inlineSmallFunctions(std::vector<FunctionIR>& functions,
                                 const std::vector<bool>& safeDepths,
                                 const std::string& entryMain) {
    for (auto& fn : functions) recognizeMinMaxAbs(fn);

    std::unordered_map<std::string, std::size_t> byName;
    for (std::size_t i = 0; i < functions.size(); ++i)
        byName.emplace(functions[i].source.name, i);

    for (int round = 0; round < 3; ++round) {
        std::vector<bool> eligible(functions.size(), false);
        for (std::size_t i = 0; i < functions.size(); ++i)
            eligible[i] = isInlineCandidate(functions[i], i, safeDepths, entryMain);

        bool changed = false;
        for (std::size_t fi = 0; fi < functions.size(); ++fi) {
            FunctionIR& caller = functions[fi];
            std::size_t addedOps = 0;
            for (std::size_t bi = 0; bi < caller.blocks.size() && addedOps < 4096; ++bi) {
                std::size_t oi = 0;
                while (oi < caller.blocks[bi].ops.size() && addedOps < 4096) {
                    const Operation callOp = caller.blocks[bi].ops[oi];
                    if (callOp.op != Op::Call) { ++oi; continue; }
                    auto it = byName.find(callOp.data);
                    if (it == byName.end() || it->second == fi || !eligible[it->second]) { ++oi; continue; }
                    const FunctionIR& callee = functions[it->second];

                    const std::size_t nParams = callee.source.params.size();
                    if (callOp.inputs.size() != nParams) { ++oi; continue; }

                    // Single-block Return callee fast path (with direct param forwarding if read-only)
                    if (callee.blocks.size() == 1 && callee.blocks[0].exit == Exit::Return) {
                        std::vector<bool> paramWritten(nParams, false);
                        for (const auto& op : callee.blocks[0].ops) {
                            if (op.op == Op::Store && op.slot < nParams) paramWritten[op.slot] = true;
                        }
                        std::vector<unsigned> slotMap(callee.slots.size(), 0);
                        std::vector<Operation> spliced;
                        for (std::size_t s = 0; s < callee.slots.size(); ++s) {
                            if (s < nParams && !paramWritten[s]) continue;
                            slotMap[s] = static_cast<unsigned>(caller.slots.size());
                            caller.slots.push_back(callee.slots[s]);
                        }
                        for (std::size_t p = 0; p < nParams; ++p) {
                            if (!paramWritten[p]) continue;
                            spliced.push_back({Op::Store, {Type::Void, caller.nextValue++},
                                               {callOp.inputs[p]}, {}, slotMap[p], callOp.span});
                        }
                        std::unordered_map<unsigned, Value> valMap;
                        for (const auto& cop : callee.blocks[0].ops) {
                            if (cop.op == Op::Load && cop.slot < nParams && !paramWritten[cop.slot]) {
                                valMap.emplace(cop.result.id, callOp.inputs[cop.slot]);
                                continue;
                            }
                            Operation cloned = cop;
                            cloned.result.id = caller.nextValue++;
                            if (cop.op == Op::Load || cop.op == Op::Store || cop.op == Op::Drop ||
                                cop.op == Op::ClearOwner || cop.op == Op::ClearField) {
                                cloned.slot = cop.slot < slotMap.size() ? slotMap[cop.slot] : cop.slot;
                            }
                            for (auto& in : cloned.inputs) {
                                auto found = valMap.find(in.id);
                                if (found != valMap.end()) in = found->second;
                            }
                            valMap.emplace(cop.result.id, cloned.result);
                            spliced.push_back(std::move(cloned));
                        }
                        std::optional<Value> retVal;
                        if (callee.blocks[0].value) {
                            auto rv = *callee.blocks[0].value;
                            auto found = valMap.find(rv.id);
                            if (found != valMap.end()) rv = found->second;
                            retVal = rv;
                        }
                        const std::size_t splicedCount = spliced.size();
                        addedOps += splicedCount;
                        auto& ops = caller.blocks[bi].ops;
                        ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(oi));
                        ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(oi),
                                   std::make_move_iterator(spliced.begin()),
                                   std::make_move_iterator(spliced.end()));
                        if (retVal && callOp.result.type != Type::Void) {
                            replaceValueIdInFunction(caller, callOp.result.id, retVal->id);
                        }
                        oi += splicedCount;
                        changed = true;
                        continue;
                    }

                    // General multi-block inline
                    std::vector<unsigned> slotMap(callee.slots.size());
                    for (std::size_t s = 0; s < callee.slots.size(); ++s) {
                        slotMap[s] = static_cast<unsigned>(caller.slots.size());
                        caller.slots.push_back(callee.slots[s]);
                        if (caller.slotByRef.size() < caller.slots.size())
                            caller.slotByRef.push_back(s < callee.slotByRef.size() ? callee.slotByRef[s] : false);
                        if (caller.slotOwned.size() < caller.slots.size())
                            caller.slotOwned.push_back(s < callee.slotOwned.size() ? callee.slotOwned[s] : false);
                    }
                    std::optional<unsigned> retSlot;
                    if (callOp.result.type != Type::Void) {
                        retSlot = static_cast<unsigned>(caller.slots.size());
                        caller.slots.push_back(callOp.result.type);
                        if (caller.slotByRef.size() < caller.slots.size()) caller.slotByRef.push_back(false);
                        if (caller.slotOwned.size() < caller.slots.size()) caller.slotOwned.push_back(false);
                    }

                    const unsigned contIdx = static_cast<unsigned>(caller.blocks.size());
                    caller.blocks.emplace_back();
                    const unsigned calleeBase = static_cast<unsigned>(caller.blocks.size());
                    caller.blocks.resize(calleeBase + callee.blocks.size());

                    Block& prefix = caller.blocks[bi];
                    Block& cont = caller.blocks[contIdx];
                    cont.exit = prefix.exit;
                    cont.value = prefix.value;
                    cont.yes = prefix.yes;
                    cont.no = prefix.no;
                    cont.targets = std::move(prefix.targets);
                    if (retSlot) {
                        cont.ops.push_back({Op::Load, callOp.result, {}, {}, *retSlot, callOp.span});
                    }
                    for (std::size_t k = oi + 1; k < prefix.ops.size(); ++k)
                        cont.ops.push_back(std::move(prefix.ops[k]));
                    prefix.ops.resize(oi);
                    for (std::size_t p = 0; p < nParams; ++p) {
                        prefix.ops.push_back({Op::Store, {Type::Void, caller.nextValue++},
                                              {callOp.inputs[p]}, {}, slotMap[p], callOp.span});
                    }
                    prefix.exit = Exit::Jump;
                    prefix.yes = calleeBase;

                    std::unordered_map<unsigned, Value> valMap;
                    for (std::size_t cb = 0; cb < callee.blocks.size(); ++cb) {
                        const auto& src = callee.blocks[cb];
                        Block& dst = caller.blocks[calleeBase + cb];
                        for (const auto& cop : src.ops) {
                            Operation cloned = cop;
                            cloned.result.id = caller.nextValue++;
                            if (cop.op == Op::Load || cop.op == Op::Store || cop.op == Op::Drop ||
                                cop.op == Op::ClearOwner || cop.op == Op::ClearField) {
                                cloned.slot = cop.slot < slotMap.size() ? slotMap[cop.slot] : cop.slot;
                            }
                            valMap.emplace(cop.result.id, cloned.result);
                            dst.ops.push_back(std::move(cloned));
                        }
                        addedOps += dst.ops.size();
                    }
                    for (std::size_t cb = 0; cb < callee.blocks.size(); ++cb) {
                        const auto& src = callee.blocks[cb];
                        Block& dst = caller.blocks[calleeBase + cb];
                        for (auto& op : dst.ops) {
                            for (auto& in : op.inputs) {
                                auto found = valMap.find(in.id);
                                if (found != valMap.end()) in = found->second;
                            }
                        }
                        auto mapVal = [&](Value v) {
                            auto found = valMap.find(v.id);
                            return found != valMap.end() ? found->second : v;
                        };
                        dst.exit = src.exit;
                        if (src.exit == Exit::Return) {
                            if (retSlot && src.value) {
                                dst.ops.push_back({Op::Store, {Type::Void, caller.nextValue++},
                                                   {mapVal(*src.value)}, {}, *retSlot, callOp.span});
                            }
                            dst.exit = Exit::Jump;
                            dst.yes = contIdx;
                        } else if (src.exit == Exit::Jump) {
                            dst.yes = calleeBase + src.yes;
                        } else if (src.exit == Exit::Branch) {
                            dst.value = mapVal(*src.value);
                            dst.yes = calleeBase + src.yes;
                            dst.no = calleeBase + src.no;
                        } else if (src.exit == Exit::Switch) {
                            dst.value = mapVal(*src.value);
                            dst.yes = calleeBase + src.yes;
                            for (auto t : src.targets) dst.targets.push_back(calleeBase + t);
                        }
                    }
                    changed = true;
                    break; // prefix block bi is now terminated by Jump; contIdx will be visited in loop!
                }
            }
        }
        if (!changed) break;
    }
}

// ---------------------------------------------------------------------------
// Pass 2: CFG / Dominator / Difference Logic / Range & Bounds Prover
// ---------------------------------------------------------------------------
namespace detail {

struct Sym {
    std::int64_t constant = 0;
    std::map<unsigned, std::int64_t> linear; // slot -> coeff
    // Optional product term: (slotA + offA) * (slotB + offB) * prodCoeff
    bool hasProduct = false;
    unsigned prodSlotA = 0, prodSlotB = 0;
    std::int64_t prodOffA = 0, prodOffB = 0, prodCoeff = 0;

    static Sym makeConst(std::int64_t c) { return Sym{c, {}}; }
    static Sym makeSlot(unsigned s) { return Sym{0, {{s, 1}}}; }

    bool isPureLinear() const { return !hasProduct; }
    bool isConst() const { return !hasProduct && linear.empty(); }
    bool sameExpr(const Sym& o) const {
        if (constant != o.constant || linear != o.linear || hasProduct != o.hasProduct) return false;
        if (!hasProduct) return true;
        if (prodCoeff != o.prodCoeff) return false;
        return (prodSlotA == o.prodSlotA && prodOffA == o.prodOffA &&
                prodSlotB == o.prodSlotB && prodOffB == o.prodOffB) ||
               (prodSlotA == o.prodSlotB && prodOffA == o.prodOffB &&
                prodSlotB == o.prodSlotA && prodOffB == o.prodOffA);
    }
};

inline std::optional<Sym> addSym(const Sym& a, const Sym& b, std::int64_t scaleB = 1) {
    if (a.hasProduct && b.hasProduct) return {};
    Sym out = a;
    if (b.hasProduct) {
        if (auto c = checkedMul64(b.prodCoeff, scaleB)) {
            out.hasProduct = true;
            out.prodSlotA = b.prodSlotA;
            out.prodSlotB = b.prodSlotB;
            out.prodOffA = b.prodOffA;
            out.prodOffB = b.prodOffB;
            out.prodCoeff = *c;
        } else return {};
    }
    auto bc = checkedMul64(b.constant, scaleB);
    if (!bc) return {};
    auto nc = checkedAdd64(out.constant, *bc);
    if (!nc) return {};
    out.constant = *nc;
    for (const auto& [s, coeff] : b.linear) {
        auto sc = checkedMul64(coeff, scaleB);
        if (!sc) return {};
        auto sum = checkedAdd64(out.linear[s], *sc);
        if (!sum) return {};
        if (*sum == 0) out.linear.erase(s);
        else out.linear[s] = *sum;
    }
    return out;
}

inline std::optional<Sym> mulSym(const Sym& a, const Sym& b) {
    if (a.isConst()) {
        if (a.constant == 0) return Sym::makeConst(0);
        if (a.constant == 1) return b;
        Sym out = b;
        auto nc = checkedMul64(out.constant, a.constant);
        if (!nc) return {};
        out.constant = *nc;
        for (auto& [s, c] : out.linear) {
            auto sc = checkedMul64(c, a.constant);
            if (!sc) return {};
            c = *sc;
        }
        if (out.hasProduct) {
            auto pc = checkedMul64(out.prodCoeff, a.constant);
            if (!pc) return {};
            out.prodCoeff = *pc;
        }
        return out;
    }
    if (b.isConst()) return mulSym(b, a);
    if (!a.hasProduct && !b.hasProduct && a.linear.size() == 1 && b.linear.size() == 1) {
        auto [sa, ca] = *a.linear.begin();
        auto [sb, cb] = *b.linear.begin();
        if (ca != 0 && cb != 0 && (a.constant % ca == 0) && (b.constant % cb == 0)) {
            if (auto pc = checkedMul64(ca, cb)) {
                Sym out;
                out.hasProduct = true;
                out.prodSlotA = sa;
                out.prodSlotB = sb;
                out.prodOffA = a.constant / ca;
                out.prodOffB = b.constant / cb;
                out.prodCoeff = *pc;
                return out;
            }
        }
    }
    return {};
}

// Compact Difference Logic graph over <= 48 tracked nodes:
// node 0 = ZERO; node i = DiffKey (Slot(s) or ArrLen(s)).
struct DiffKey {
    bool isLen = false;
    unsigned slot = 0;
    auto operator<=>(const DiffKey&) const = default;
};

struct DiffGraph {
    static constexpr std::size_t kMaxNodes = 48;
    static constexpr std::int64_t kInf = kMaxInt / 4;

    std::vector<DiffKey> keys;
    std::unordered_map<unsigned, std::size_t> slotNode, lenNode;
    std::vector<std::vector<std::int64_t>> d; // d[u][v] means u - v <= d[u][v]

    DiffGraph() { reset(); }
    void reset() {
        keys.clear();
        keys.push_back({false, 0}); // 0 = ZERO sentinel
        slotNode.clear();
        lenNode.clear();
        d.assign(1, std::vector<std::int64_t>(1, 0));
    }

    std::optional<std::size_t> nodeOf(DiffKey k) {
        auto& mp = k.isLen ? lenNode : slotNode;
        if (auto it = mp.find(k.slot); it != mp.end()) return it->second;
        if (keys.size() >= kMaxNodes) return {};
        const std::size_t idx = keys.size();
        keys.push_back(k);
        mp.emplace(k.slot, idx);
        for (auto& row : d) row.push_back(kInf);
        d.emplace_back(keys.size(), kInf);
        d[idx][idx] = 0;
        return idx;
    }

    std::optional<std::size_t> findNode(DiffKey k) const {
        const auto& mp = k.isLen ? lenNode : slotNode;
        if (auto it = mp.find(k.slot); it != mp.end()) return it->second;
        return {};
    }

    // Clamp addition to [-kInf, kInf] safely without signed overflow
    static std::int64_t clampAdd(std::int64_t a, std::int64_t b) {
        if (a >= kInf || b >= kInf) return kInf;
        if (auto s = checkedAdd64(a, b)) return std::clamp(*s, -kInf, kInf);
        return (a > 0 && b > 0) ? kInf : -kInf;
    }

    void addDiff(std::size_t u, std::size_t v, std::int64_t c) {
        if (u == v || c >= d[u][v]) return;
        const std::size_t n = keys.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (d[i][u] >= kInf) continue;
            for (std::size_t j = 0; j < n; ++j) {
                if (d[v][j] >= kInf) continue;
                const auto cand = clampAdd(clampAdd(d[i][u], c), d[v][j]);
                if (cand < d[i][j]) d[i][j] = cand;
            }
        }
    }

    void addSlotDiff(DiffKey a, DiffKey b, std::int64_t c) {
        auto u = nodeOf(a), v = nodeOf(b);
        if (u && v) addDiff(*u, *v, c);
    }

    void addUpper(DiffKey a, std::int64_t hi) {
        if (auto u = nodeOf(a)) addDiff(*u, 0, hi);
    }
    void addLower(DiffKey a, std::int64_t lo) {
        if (lo == kMinInt) return;
        if (auto u = nodeOf(a)) addDiff(0, *u, -lo);
    }

    void forgetKey(DiffKey k) {
        if (auto u = findNode(k)) {
            const std::size_t n = keys.size();
            for (std::size_t i = 0; i < n; ++i) {
                if (i == *u) continue;
                d[*u][i] = kInf;
                d[i][*u] = kInf;
            }
        }
    }
    void forgetSlot(unsigned slot) { forgetKey({false, slot}); }
    void forgetLen(unsigned slot) { forgetKey({true, slot}); }

    std::optional<IntRange> rangeOf(DiffKey k) const {
        auto u = findNode(k);
        if (!u) return {};
        const auto loNeg = d[0][*u];
        const auto hi = d[*u][0];
        if (loNeg >= kInf && hi >= kInf) return {};
        return IntRange{loNeg >= kInf ? kMinInt : -loNeg, hi >= kInf ? kMaxInt : hi};
    }

    std::optional<std::int64_t> maxDiff(DiffKey a, DiffKey b) const {
        auto u = findNode(a), v = findNode(b);
        if (!u || !v || d[*u][*v] >= kInf) return {};
        return d[*u][*v];
    }
};

struct CFGInfo {
    std::vector<std::vector<unsigned>> preds, succs;
    std::vector<unsigned> rpo;
    std::vector<int> rpoPos;
    std::vector<int> idom;
    std::vector<std::vector<unsigned>> domTree;
    struct Loop {
        unsigned header;
        std::unordered_set<unsigned> body;
        std::vector<unsigned> preheaders;
    };
    std::vector<Loop> loops;
    std::unordered_map<unsigned, std::size_t> headerToLoop;
    std::vector<int> loopOfBlock; // -1 if outside all loops
};

inline CFGInfo buildCFG(const FunctionIR& fn) {
    const unsigned n = static_cast<unsigned>(fn.blocks.size());
    CFGInfo cfg;
    cfg.preds.resize(n);
    cfg.succs.resize(n);
    for (unsigned b = 0; b < n; ++b) {
        const auto& blk = fn.blocks[b];
        auto addEdge = [&](unsigned to) {
            if (to < n) {
                cfg.succs[b].push_back(to);
                cfg.preds[to].push_back(b);
            }
        };
        if (blk.exit == Exit::Jump) addEdge(blk.yes);
        else if (blk.exit == Exit::Branch) { addEdge(blk.yes); addEdge(blk.no); }
        else if (blk.exit == Exit::Switch) {
            addEdge(blk.yes);
            for (auto t : blk.targets) addEdge(t);
        }
    }

    std::vector<bool> visited(n, false);
    std::vector<unsigned> post;
    auto dfs = [&](auto& self, unsigned u) -> void {
        visited[u] = true;
        for (unsigned v : cfg.succs[u]) if (!visited[v]) self(self, v);
        post.push_back(u);
    };
    dfs(dfs, 0);
    cfg.rpo.assign(post.rbegin(), post.rend());
    cfg.rpoPos.assign(n, -1);
    for (std::size_t i = 0; i < cfg.rpo.size(); ++i) cfg.rpoPos[cfg.rpo[i]] = static_cast<int>(i);

    cfg.idom.assign(n, -1);
    cfg.idom[0] = 0;
    auto intersect = [&](int b1, int b2) {
        int f1 = b1, f2 = b2;
        while (f1 != f2) {
            while (cfg.rpoPos[f1] > cfg.rpoPos[f2]) f1 = cfg.idom[f1];
            while (cfg.rpoPos[f2] > cfg.rpoPos[f1]) f2 = cfg.idom[f2];
        }
        return f1;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (unsigned b : cfg.rpo) {
            if (b == 0) continue;
            int newIdom = -1;
            for (unsigned p : cfg.preds[b]) {
                if (cfg.idom[p] != -1) {
                    newIdom = newIdom == -1 ? static_cast<int>(p) : intersect(static_cast<int>(p), newIdom);
                }
            }
            if (newIdom != -1 && newIdom != cfg.idom[b]) {
                cfg.idom[b] = newIdom;
                changed = true;
            }
        }
    }
    cfg.domTree.resize(n);
    for (unsigned b = 1; b < n; ++b)
        if (cfg.idom[b] >= 0) cfg.domTree[cfg.idom[b]].push_back(b);

    auto dominates = [&](unsigned a, unsigned b) {
        if (cfg.rpoPos[b] < 0) return false;
        int cur = static_cast<int>(b);
        while (cur > 0 && cur != static_cast<int>(a)) cur = cfg.idom[cur];
        return cur == static_cast<int>(a);
    };

    for (unsigned b = 0; b < n; ++b) {
        if (cfg.rpoPos[b] < 0) continue;
        for (unsigned h : cfg.succs[b]) {
            if (dominates(h, b)) {
                auto [it, inserted] = cfg.headerToLoop.emplace(h, cfg.loops.size());
                if (inserted) cfg.loops.push_back({h, {h}, {}});
                auto& lp = cfg.loops[it->second];
                std::vector<unsigned> work;
                if (lp.body.insert(b).second) work.push_back(b);
                while (!work.empty()) {
                    unsigned cur = work.back();
                    work.pop_back();
                    for (unsigned p : cfg.preds[cur]) {
                        if (cfg.rpoPos[p] >= 0 && lp.body.insert(p).second) work.push_back(p);
                    }
                }
            }
        }
    }
    for (auto& lp : cfg.loops) {
        for (unsigned p : cfg.preds[lp.header])
            if (!lp.body.contains(p)) lp.preheaders.push_back(p);
    }
    cfg.loopOfBlock.assign(n, -1);
    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        for (unsigned b : cfg.loops[li].body) {
            int cur = cfg.loopOfBlock[b];
            if (cur < 0 || cfg.loops[li].body.size() < cfg.loops[cur].body.size())
                cfg.loopOfBlock[b] = static_cast<int>(li);
        }
    }
    return cfg;
}

inline bool isAnyArrayType(const Program* prog, Type t) {
    return t == Type::IntArray || (prog && prog->isArray(t));
}

class FunctionProver {
    FunctionIR& fn;
    const std::unordered_map<std::string, IntRange>& fnReturnRanges;
    std::vector<std::optional<IntRange>> initialParamRanges;
    const Program* programPtr = nullptr;
    CFGInfo cfg;
    std::vector<unsigned> storeCount;
    std::vector<bool> slotWritable;
    std::vector<std::optional<Value>> singleSlotVal;
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> loadSlot;
    std::unordered_map<unsigned, std::optional<IntRange>> globalElemRanges;
    std::map<std::pair<std::size_t, unsigned>, IntRange> accLoopRanges; // (loopIdx, slot) -> sound range inside & after loop
    std::unordered_map<std::size_t, std::vector<std::tuple<unsigned, Value, bool>>> loopAccOps; // loopIdx -> (slot, stepVal, isSub)
    struct MultiAccOp {
        unsigned slot = 0;
        std::vector<std::pair<Value, bool>> steps;
        std::int64_t maxPerIter = 1;
    };
    std::unordered_map<std::size_t, std::vector<MultiAccOp>> loopMultiAccOps;
    std::map<std::pair<std::size_t, unsigned>, IntRange> loopPreAccRanges;
    std::map<std::size_t, std::int64_t> loopMaxTrips;
    struct SentinelLoop {
        unsigned arrSlot = 0;
        unsigned jSlot = 0;
    };
    std::unordered_map<std::size_t, SentinelLoop> sentinelLoops;
    struct SqrtLoop {
        unsigned dSlot = 0;
        Value boundVal{};
        std::optional<unsigned> andExitBlk;
    };
    std::unordered_map<std::size_t, SqrtLoop> sqrtLoops;
    std::map<std::size_t, std::int64_t> sqrtLoopUpper;
    std::map<std::size_t, std::pair<unsigned, IntRange>> sqrtLoopBoundRange;
    struct HeronLoop {
        unsigned xSlot = 0;
        unsigned ySlot = 0;
        Value aVal{};
    };
    std::unordered_map<std::size_t, HeronLoop> heronLoops;
    std::map<std::size_t, IntRange> activeHeronRanges;
    struct EuclidLoop {
        unsigned aSlot = 0;
        unsigned bSlot = 0;
    };
    std::unordered_map<std::size_t, EuclidLoop> euclidLoops;
    std::map<std::size_t, std::pair<std::int64_t, std::int64_t>> activeEuclidUpper;
    struct BoundedSelector {
        unsigned slot = 0;
        unsigned boundSlot = 0;
    };
    std::unordered_map<std::size_t, std::vector<BoundedSelector>> boundedSelectorLoops;
    std::map<std::size_t, std::vector<BoundedSelector>> activeBoundedSelectors;
    std::map<std::pair<std::size_t, unsigned>, std::unordered_set<unsigned>> loopPreModLtDiv;
    std::map<std::size_t, std::pair<unsigned, IntRange>> divShrinkBoundRange;
    std::unordered_set<std::size_t> activeTwoWayPartitions;
    std::map<std::size_t, std::int64_t> activeUniqueZeroMaxN;
    struct BinarySearchLoop {
        unsigned loSlot = 0;
        unsigned hiSlot = 0;
    };
    std::unordered_map<std::size_t, BinarySearchLoop> binarySearchLoops;
    std::map<std::size_t, std::int64_t> bsLoopUpper;
    struct BSArrDiff { unsigned arrSlot; std::int64_t diff; };
    std::map<std::size_t, std::vector<BSArrDiff>> bsLoopArrDiffs;
    struct MaxClampAcc {
        unsigned slot = 0;
        Value clampVal{};
        Value stepTerm{};
        bool isSub = false;
        Value subTerm{};
        bool hasSubTerm = false;
    };
    std::unordered_map<std::size_t, std::vector<MaxClampAcc>> loopMaxClampOps;
    struct CoupledMaxPair {
        unsigned prevSlot = 0;
        unsigned bestSlot = 0;
        Value deltaVal{};
        bool isSub = false;
    };
    std::unordered_map<std::size_t, std::vector<CoupledMaxPair>> loopCoupledMaxOps;
    struct RunningExtremeOp {
        unsigned slot = 0;
        unsigned srcSlot = 0;
        Value srcVal{};
        bool hasSrcSlot = false;
        bool isMin = false;
    };
    std::unordered_map<std::size_t, std::vector<RunningExtremeOp>> loopRunningExtremeOps;
    struct ModLoopOp {
        unsigned slot = 0;
        Value divVal{};
        Value remResult{};
        Value exprRoot{};
        bool isLiveIn = true;
    };
    std::unordered_map<std::size_t, std::vector<ModLoopOp>> loopModOps;
    struct CopyChainOp {
        unsigned dstSlot = 0;
        unsigned srcSlot = 0;
        bool isLiveIn = true;
        bool beforeIterInc = false;
    };
    std::unordered_map<std::size_t, std::vector<CopyChainOp>> loopCopyOps;
    std::unordered_map<std::size_t, IntRange> loopIterRanges;
    std::unordered_map<std::size_t, std::vector<std::pair<unsigned, unsigned>>> modSlotLtDiv;
    struct DivShrinkLoop {
        unsigned slot = 0;
        std::int64_t minConstDiv = 0;
        std::optional<unsigned> divSlot;
    };
    std::unordered_map<std::size_t, DivShrinkLoop> divShrinkLoops;
    struct GuardedCounterLoop {
        unsigned slot = 0;
        unsigned boundSlot = 0;
    };
    std::unordered_map<std::size_t, std::vector<GuardedCounterLoop>> guardedCounterLoops;
    std::map<std::size_t, std::vector<GuardedCounterLoop>> activeGuardedCounters;
    struct TwoWayPartitionLoop {
        unsigned iterSlot = 0;
        unsigned leftSlot = 0;
        unsigned rightSlot = 0;
        unsigned boundSlot = 0;
        unsigned eqCheckBlock = 0;
        unsigned ltCheckBlock = 0;
        unsigned thenBlock = 0;
        unsigned elseBlock = 0;
    };
    std::unordered_map<std::size_t, TwoWayPartitionLoop> twoWayPartitionLoops;
    struct UniqueZeroSetLoop {
        unsigned seenArr = 0;
        unsigned queueArr = 0;
        unsigned countSlot = 0;
        std::optional<unsigned> headSlot;
        unsigned guardBlock = 0;
        unsigned queueWriteBlk = 0;
        std::int64_t initCount = 0;
    };
    std::unordered_map<std::size_t, UniqueZeroSetLoop> uniqueZeroSetLoops;
    std::unordered_map<unsigned, unsigned> arrElemLtSlot; // IntArray slot -> immutable N (all elements in [0, N - 1])
    std::unordered_map<unsigned, unsigned> globalElemMaxConst;
    struct SmallLoopResult {
        std::unordered_map<unsigned, IntRange> slotEnvelope;
        std::unordered_map<unsigned, IntRange> slotPostEnvelope;
        std::unordered_map<unsigned, IntRange> accDelta;
    };
    std::unordered_map<std::size_t, SmallLoopResult> smallLoopResults;
    struct LoopMeanInfo {
        unsigned upperSlot = 0;
        IntRange stepRange{};
    };
    std::unordered_map<std::size_t, std::unordered_map<unsigned, LoopMeanInfo>> loopSumSources;
    std::unordered_map<unsigned, std::pair<unsigned, std::int64_t>> squareArraySizes; // arrSlot -> (nSlot, offset)
    struct RectShape {
        unsigned rSlot = 0;
        unsigned cSlot = 0;
        std::int64_t rOff = 0;
        std::int64_t cOff = 0;
        bool operator==(const RectShape&) const = default;
    };
    std::unordered_map<unsigned, RectShape> rectArraySizes; // arrSlot -> (rSlot+rOff) * (cSlot+cOff)
    std::unordered_map<unsigned, Sym> linearArraySizes; // arrSlot -> size Sym
    std::unordered_map<unsigned, Sym> fullArraySizes;   // arrSlot -> size Sym (including immutable products like n*n)

    struct MonotoneSlot {
        unsigned slot;
        bool nonDecreasing = false; // slot >= entry
        bool nonIncreasing = false; // slot <= entry
        std::optional<unsigned> decBySlot; // nonIncreasing when this loop-invariant slot >= 0
        bool nonIncIfNonNeg = false;       // nonIncreasing when slot >= 0 at loop entry
        bool nonNegPreserved = false;      // slot >= 0 preserved by dominating decrement guard
        std::optional<unsigned> decDivSlot; // slot = slot / d or slot - slot / d (bounded in [0, entry] if d >= 1)
        MonotoneSlot(unsigned s, bool nd = false, bool ni = false,
                     std::optional<unsigned> dbs = std::nullopt, bool ninn = false, bool nnp = false,
                     std::optional<unsigned> dds = std::nullopt)
            : slot(s), nonDecreasing(nd), nonIncreasing(ni), decBySlot(dbs),
              nonIncIfNonNeg(ninn), nonNegPreserved(nnp), decDivSlot(dds) {}
    };
    std::vector<std::vector<MonotoneSlot>> loopMonotone;
    std::vector<std::unordered_set<unsigned>> loopWritten;
    struct ForHeaderInfo {
        bool valid = false;
        unsigned iterSlot = 0;
        Value lower{}, upper{};
        unsigned bodyBlock = 0;
    };
    std::unordered_map<unsigned, ForHeaderInfo> forHeaders;
    std::unordered_map<unsigned, std::pair<Value, Value>> shortCircuitAnd, shortCircuitOr;

    struct Env {
        DiffGraph diffs;
        std::unordered_map<unsigned, IntRange> valRanges;
        std::vector<unsigned> slotEpoch;
        std::unordered_map<unsigned, unsigned> loadEpoch;
        std::unordered_map<unsigned, Value> slotVal;
        std::unordered_set<unsigned> nonZeroSlots;
        std::unordered_set<unsigned> contractSlots;
        std::unordered_set<unsigned> contractArrLens;
        std::unordered_map<unsigned, std::pair<unsigned, IntRange>> meanSources;
        std::unordered_map<unsigned, unsigned> remValLtSlot;
        std::unordered_map<unsigned, unsigned> idxElemLtSlot;
        std::unordered_map<unsigned, RectShape> rectArrShapes;
    };

    static void invalidateSlot(Env& env, unsigned s) {
        env.diffs.forgetSlot(s);
        env.slotVal.erase(s);
        env.nonZeroSlots.erase(s);
        env.meanSources.erase(s);
        env.rectArrShapes.erase(s);
        if (s < env.slotEpoch.size()) ++env.slotEpoch[s];
    }

    bool isCurrentLoad(unsigned valId, unsigned slot, const Env& env) const {
        if (slot >= env.slotEpoch.size()) return false;
        auto it = env.loadEpoch.find(valId);
        return it != env.loadEpoch.end() && it->second == env.slotEpoch[slot];
    }

    static std::optional<IntRange> intersectRange(std::optional<IntRange> a, std::optional<IntRange> b) {
        if (!a) return b;
        if (!b) return a;
        if (a->lower > b->upper || b->lower > a->upper) return IntRange{a->lower, a->upper};
        return IntRange{std::max(a->lower, b->lower), std::min(a->upper, b->upper)};
    }

    std::optional<Sym> buildSym(unsigned valId, const Env& env, int depth = 0) const {
        if (depth > 32) return {};
        std::optional<Sym> res;
        auto dit = defOp.find(valId);
        if (dit != defOp.end()) {
            const Operation* op = dit->second;
            switch (op->op) {
            case Op::Int:
                if (op->result.type == Type::Int) {
                    if (auto c = parseConstInt(op->data)) res = Sym::makeConst(*c);
                }
                break;
            case Op::Load:
                if (!slotWritable[op->slot] && singleSlotVal[op->slot] &&
                    singleSlotVal[op->slot]->id != valId) {
                    res = buildSym(singleSlotVal[op->slot]->id, env, depth + 1);
                }
                if (!res) {
                    if (!slotWritable[op->slot] || isCurrentLoad(valId, op->slot, env))
                        res = Sym::makeSlot(op->slot);
                }
                break;
            case Op::Negate:
                if (op->result.type == Type::Int) {
                    if (auto c = buildSym(op->inputs[0].id, env, depth + 1)) {
                        Sym zero = Sym::makeConst(0);
                        res = addSym(zero, *c, -1);
                    }
                }
                break;
            case Op::Binary:
                if (op->inputs[0].type == Type::Int && op->inputs[1].type == Type::Int &&
                    (op->data == "+" || op->data == "-" || op->data == "*")) {
                    auto l = buildSym(op->inputs[0].id, env, depth + 1);
                    auto r = buildSym(op->inputs[1].id, env, depth + 1);
                    if (l && r) {
                        if (op->data == "+") res = addSym(*l, *r, 1);
                        else if (op->data == "-") res = addSym(*l, *r, -1);
                        else res = mulSym(*l, *r);
                    }
                }
                break;
            default: break;
            }
        }
        return res;
    }

    // Also provide direct slot-level symbol without chasing past mutable/param slots
    struct LinearAtom {
        bool isArrLen = false;
        unsigned slot = 0;
        std::int64_t offset = 0;
    };

    std::optional<LinearAtom> extractLinearAtom(unsigned valId, const Env& env, int depth = 0) const {
        if (depth > 16) return {};
        auto dit = defOp.find(valId);
        if (dit == defOp.end()) return {};
        const Operation* op = dit->second;
        if (op->op == Op::Load) {
            if (!slotWritable[op->slot] && op->slot >= fn.source.params.size() &&
                singleSlotVal[op->slot] && singleSlotVal[op->slot]->id != valId) {
                if (auto sub = extractLinearAtom(singleSlotVal[op->slot]->id, env, depth + 1))
                    return sub;
            }
            if (!slotWritable[op->slot] || isCurrentLoad(valId, op->slot, env))
                return LinearAtom{false, op->slot, 0};
            return {};
        }
        if (op->op == Op::Length && !op->inputs.empty() &&
            op->inputs[0].type != Type::Str && op->inputs[0].type != Type::StringBuilder) {
            if (auto ls = loadSlot.find(op->inputs[0].id); ls != loadSlot.end()) {
                if (!slotWritable[ls->second] || isCurrentLoad(op->inputs[0].id, ls->second, env))
                    return LinearAtom{true, ls->second, 0};
            }
            return {};
        }
        if (op->op == Op::Binary && op->inputs.size() == 2 &&
            op->inputs[0].type == Type::Int && op->inputs[1].type == Type::Int &&
            (op->data == "+" || op->data == "-")) {
            auto rdef = defOp.find(op->inputs[1].id);
            if (rdef != defOp.end() && rdef->second->op == Op::Int) {
                if (auto c = parseConstInt(rdef->second->data)) {
                    if (auto base = extractLinearAtom(op->inputs[0].id, env, depth + 1)) {
                        auto newOff = (op->data == "+") ? checkedAdd64(base->offset, *c)
                                                        : checkedSub64(base->offset, *c);
                        if (newOff) return LinearAtom{base->isArrLen, base->slot, *newOff};
                    }
                }
            }
            if (op->data == "+") {
                auto ldef = defOp.find(op->inputs[0].id);
                if (ldef != defOp.end() && ldef->second->op == Op::Int) {
                    if (auto c = parseConstInt(ldef->second->data)) {
                        if (auto base = extractLinearAtom(op->inputs[1].id, env, depth + 1)) {
                            if (auto newOff = checkedAdd64(base->offset, *c))
                                return LinearAtom{base->isArrLen, base->slot, *newOff};
                        }
                    }
                }
            }
        }
        return {};
    }

    std::optional<Sym> shallowSym(unsigned valId, const Env& env, int depth = 0) const {
        if (depth > 24) return {};
        auto dit = defOp.find(valId);
        if (dit == defOp.end()) return {};
        const Operation* op = dit->second;
        if (op->op == Op::Int) {
            if (op->result.type != Type::Int) return {};
            if (auto c = parseConstInt(op->data)) return Sym::makeConst(*c);
            return {};
        }
        if (op->op == Op::Load) {
            // If it's a single-assignment non-param local storing a constant/affine expr of earlier slots, expand if pure linear
            if (!slotWritable[op->slot] && op->slot >= fn.source.params.size() &&
                singleSlotVal[op->slot] && singleSlotVal[op->slot]->id != valId) {
                if (auto sub = shallowSym(singleSlotVal[op->slot]->id, env, depth + 1)) return sub;
            }
            if (!slotWritable[op->slot] || isCurrentLoad(valId, op->slot, env))
                return Sym::makeSlot(op->slot);
            return {};
        }
        if (op->op == Op::Negate) {
            if (op->result.type != Type::Int) return {};
            if (auto c = shallowSym(op->inputs[0].id, env, depth + 1))
                return addSym(Sym::makeConst(0), *c, -1);
            return {};
        }
        if (op->op == Op::Binary) {
            if (op->inputs[0].type == Type::Int && op->inputs[1].type == Type::Int &&
                (op->data == "+" || op->data == "-" || op->data == "*")) {
                auto l = shallowSym(op->inputs[0].id, env, depth + 1);
                auto r = shallowSym(op->inputs[1].id, env, depth + 1);
                if (!l || !r) return {};
                if (op->data == "+") return addSym(*l, *r, 1);
                if (op->data == "-") return addSym(*l, *r, -1);
                return mulSym(*l, *r);
            }
        }
        return {};
    }

    std::optional<IntRange> evalSymRange(const Sym& s, const Env& env) const {
        IntRange acc{s.constant, s.constant};
        for (const auto& [slot, coeff] : s.linear) {
            auto r = env.diffs.rangeOf({false, slot});
            if (!r) return {};
            auto term = ReturnRanges::multiplyRanges(*r, {coeff, coeff});
            if (!term) return {};
            auto sum = ReturnRanges::addRanges(acc, *term);
            if (!sum) return {};
            acc = *sum;
        }
        if (s.hasProduct) {
            auto ra = env.diffs.rangeOf({false, s.prodSlotA});
            auto rb = env.diffs.rangeOf({false, s.prodSlotB});
            if (!ra || !rb) return {};
            auto sa = ReturnRanges::addRanges(*ra, {s.prodOffA, s.prodOffA});
            auto sb = ReturnRanges::addRanges(*rb, {s.prodOffB, s.prodOffB});
            if (!sa || !sb) return {};
            auto p = ReturnRanges::multiplyRanges(*sa, *sb);
            if (!p) return {};
            auto cp = ReturnRanges::multiplyRanges(*p, {s.prodCoeff, s.prodCoeff});
            if (!cp) return {};
            auto sum = ReturnRanges::addRanges(acc, *cp);
            if (!sum) return {};
            acc = *sum;
        }
        return acc;
    }

    std::optional<IntRange> rangeOf(const Value& v, const Env& env) const {
        std::optional<IntRange> res;
        if (auto it = env.valRanges.find(v.id); it != env.valRanges.end()) res = it->second;
        if (auto lit = loadSlot.find(v.id); lit != loadSlot.end()) {
            if (!slotWritable[lit->second] || isCurrentLoad(v.id, lit->second, env)) {
                if (auto sr = env.diffs.rangeOf({false, lit->second}))
                    res = intersectRange(res, sr);
            }
        }
        return res;
    }

    void analyzeStructure() {
        const std::size_t nSlots = fn.slots.size();
        storeCount.assign(nSlots, 0);
        slotWritable.assign(nSlots, false);
        singleSlotVal.assign(nSlots, std::nullopt);
        for (std::size_t i = 0; i < fn.source.params.size() && i < nSlots; ++i) ++storeCount[i];

        for (unsigned b = 0; b < fn.blocks.size(); ++b) {
            for (const auto& op : fn.blocks[b].ops) {
                if (op.result.type != Type::Void) defOp[op.result.id] = &op;
                if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
                if (op.op == Op::Store && !op.inputs.empty()) {
                    ++storeCount[op.slot];
                    singleSlotVal[op.slot] = op.inputs[0];
                }
                if (op.op == Op::FieldSet || op.op == Op::PlaceSet) {
                    ++storeCount[op.slot];
                }
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Builtin || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                    for (unsigned ms : op.mutArgSlots) {
                        if (ms < nSlots) ++storeCount[ms];
                    }
                }
            }
        }
        for (std::size_t s = 0; s < nSlots; ++s) {
            slotWritable[s] = (storeCount[s] > 1) || (s < fn.slotByRef.size() && fn.slotByRef[s]);
            if (slotWritable[s]) singleSlotVal[s].reset();
        }

        // Loop info & For headers (Pass 1) then monotone/acc/while analyses (Pass 2 so outer while loops see inner forHeaders)
        const std::size_t nLoops = cfg.loops.size();
        loopWritten.resize(nLoops);
        loopMonotone.resize(nLoops);
        for (std::size_t li = 0; li < nLoops; ++li) {
            const auto& lp = cfg.loops[li];
            for (unsigned b : lp.body) {
                for (const auto& op : fn.blocks[b].ops) {
                    if (op.op == Op::Store || op.op == Op::FieldSet || op.op == Op::PlaceSet) loopWritten[li].insert(op.slot);
                    if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Builtin || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                        for (unsigned ms : op.mutArgSlots) {
                            if (ms < nSlots) loopWritten[li].insert(ms);
                        }
                    }
                }
            }
            // Detect For loop header
            const auto& hb = fn.blocks[lp.header];
            if (hb.exit == Exit::Branch && hb.value && lp.body.contains(hb.yes) && !lp.body.contains(hb.no)) {
                auto condIt = defOp.find(hb.value->id);
                if (condIt != defOp.end() && condIt->second->op == Op::Binary && condIt->second->data == "<") {
                    const Value lhs = condIt->second->inputs[0], rhs = condIt->second->inputs[1];
                    if (auto lslot = loadSlot.find(lhs.id); lslot != loadSlot.end()) {
                        unsigned iter = lslot->second;
                        // Verify iter only written by rangeIncrement on backedge(s) inside loop
                        bool onlyRangeInc = true;
                        int incCount = 0;
                        for (unsigned b : lp.body) {
                            for (const auto& op : fn.blocks[b].ops) {
                                if (op.op == Op::Store && op.slot == iter) {
                                    auto rhsDef = defOp.find(op.inputs[0].id);
                                    if (rhsDef != defOp.end() && rhsDef->second->op == Op::Binary &&
                                        rhsDef->second->data == "rangeIncrement") {
                                        ++incCount;
                                    } else {
                                        onlyRangeInc = false;
                                    }
                                }
                            }
                        }
                        if (onlyRangeInc && incCount >= 1 && lp.preheaders.size() == 1) {
                            // Find initial store in preheader
                            std::optional<Value> lower;
                            for (const auto& op : fn.blocks[lp.preheaders[0]].ops) {
                                if (op.op == Op::Store && op.slot == iter) lower = op.inputs[0];
                            }
                            if (lower) {
                                forHeaders[lp.header] = ForHeaderInfo{true, iter, *lower, rhs, hb.yes};
                            }
                        }
                    }
                }
            }
        }
        // Detect Lowerer::lazy() short-circuit && / || Bool scratch slots
        for (unsigned lhsBlk = 0; lhsBlk < fn.blocks.size(); ++lhsBlk) {
            const auto& lb = fn.blocks[lhsBlk];
            if (lb.exit != Exit::Branch || !lb.value || lb.ops.empty()) continue;
            const auto& lastOp = lb.ops.back();
            if (lastOp.op != Op::Store || lastOp.inputs.empty() || lastOp.inputs[0].id != lb.value->id) continue;
            const unsigned bs = lastOp.slot;
            if (bs >= nSlots || fn.slots[bs] != Type::Bool || storeCount[bs] != 2) continue;
            for (int isAnd = 1; isAnd >= 0; --isAnd) {
                const unsigned rhsBlk = isAnd ? lb.yes : lb.no;
                const unsigned endBlk = isAnd ? lb.no : lb.yes;
                if (rhsBlk == endBlk || rhsBlk >= fn.blocks.size() || endBlk >= fn.blocks.size()) continue;
                const auto& rb = fn.blocks[rhsBlk];
                if (rb.exit != Exit::Jump || rb.yes != endBlk ||
                    cfg.preds[rhsBlk].size() != 1 || cfg.preds[rhsBlk][0] != lhsBlk)
                    continue;
                bool cleanRhs = true;
                std::optional<Value> secondVal;
                for (const auto& rop : rb.ops) {
                    if (rop.op == Op::Call || rop.op == Op::ExternCall || rop.op == Op::DynCall || rop.op == Op::ClosureCall ||
                        rop.op == Op::IndexSet || rop.op == Op::ClearOwner ||
                        rop.op == Op::ClearField || rop.op == Op::ClearIndex) {
                        cleanRhs = false; break;
                    }
                    if (rop.op == Op::Store) {
                        if (rop.slot >= nSlots || fn.slots[rop.slot] != Type::Bool) { cleanRhs = false; break; }
                        if (rop.slot == bs && !rop.inputs.empty()) secondVal = rop.inputs[0];
                    }
                }
                if (cleanRhs && secondVal) {
                    if (isAnd) shortCircuitAnd.emplace(bs, std::pair{*lb.value, *secondVal});
                    else shortCircuitOr.emplace(bs, std::pair{*lb.value, *secondVal});
                    break;
                }
            }
        }

        std::vector<std::unordered_set<unsigned>> loopArrayWritten(nLoops);
        std::vector<bool> loopUnknownIndexSet(nLoops, false);
        for (std::size_t li = 0; li < nLoops; ++li) {
            for (unsigned b : cfg.loops[li].body) {
                for (const auto& op : fn.blocks[b].ops) {
                    if (op.op == Op::IndexSet && !op.inputs.empty()) {
                        if (auto als = loadSlot.find(op.inputs[0].id); als != loadSlot.end())
                            loopArrayWritten[li].insert(als->second);
                        else
                            loopUnknownIndexSet[li] = true;
                    }
                }
            }
        }

        for (std::size_t li = 0; li < nLoops; ++li) {
            const auto& lp = cfg.loops[li];

            // Detect monotone var updates (s += c, s -= c, s -= posSlot, s /= posDiv, s -= s / posDiv)
            for (unsigned slot : loopWritten[li]) {
                bool decOk = true, incOk = true;
                bool decIfNonNeg = false;
                std::optional<unsigned> decStepSlot;
                std::optional<unsigned> decDivSlot;
                std::optional<std::int64_t> constDecMin;
                bool badDecStep = false;
                int slotStoreCnt = 0;
                unsigned lastStoreBlk = 0;
                for (unsigned b : lp.body) {
                    for (const auto& op : fn.blocks[b].ops) {
                        if (op.op != Op::Store || op.slot != slot) continue;
                        ++slotStoreCnt;
                        lastStoreBlk = b;
                        auto dit = defOp.find(op.inputs[0].id);
                        if (dit == defOp.end()) { decOk = incOk = false; badDecStep = true; continue; }
                        const Operation* valOp = dit->second;
                        if (valOp->op == Op::Binary && valOp->data == "rangeIncrement") {
                            auto ls = loadSlot.find(valOp->inputs[0].id);
                            if (ls != loadSlot.end() && ls->second == slot) {
                                decOk = false;
                                badDecStep = true;
                                continue;
                            }
                        } else if (valOp->op == Op::Binary && valOp->data == "+") {
                            auto ls = loadSlot.find(valOp->inputs[0].id);
                            if (ls != loadSlot.end() && ls->second == slot) {
                                auto rdef = defOp.find(valOp->inputs[1].id);
                                if (rdef != defOp.end() && rdef->second->op == Op::Int) {
                                    if (auto c = parseConstInt(rdef->second->data)) {
                                        if (*c < 0) incOk = false;
                                        if (*c > 0) decOk = false;
                                        const std::int64_t decAmt = (*c <= 0 && *c > kMinInt) ? -*c : -1;
                                        if (decAmt >= 0)
                                            constDecMin = constDecMin ? std::max(*constDecMin, decAmt) : decAmt;
                                        continue;
                                    }
                                }
                            }
                        } else if (valOp->op == Op::Binary && valOp->data == "-") {
                            auto ls = loadSlot.find(valOp->inputs[0].id);
                            auto rdef = defOp.find(valOp->inputs[1].id);
                            if (ls != loadSlot.end() && ls->second == slot) {
                                if (rdef != defOp.end() && rdef->second->op == Op::Int) {
                                    if (auto c = parseConstInt(rdef->second->data)) {
                                        if (*c < 0) decOk = false;
                                        if (*c > 0) incOk = false;
                                        if (*c >= 0)
                                            constDecMin = constDecMin ? std::max(*constDecMin, *c) : *c;
                                        continue;
                                    }
                                }
                                if (rdef != defOp.end() && rdef->second->op == Op::Load &&
                                    !loopWritten[li].contains(rdef->second->slot)) {
                                    incOk = false;
                                    if (!decStepSlot) decStepSlot = rdef->second->slot;
                                    else if (*decStepSlot != rdef->second->slot) badDecStep = true;
                                    continue;
                                }
                                // slot = slot - (slot / d): non-increasing & non-negative when slot >= 0 and d >= 1
                                if (rdef != defOp.end() && rdef->second->op == Op::Binary && rdef->second->data == "/") {
                                    auto numLs = loadSlot.find(rdef->second->inputs[0].id);
                                    auto ddef = defOp.find(rdef->second->inputs[1].id);
                                    if (numLs != loadSlot.end() && numLs->second == slot && ddef != defOp.end()) {
                                        if (ddef->second->op == Op::Int) {
                                            if (auto dc = parseConstInt(ddef->second->data); dc && *dc >= 1) {
                                                incOk = false;
                                                decIfNonNeg = true;
                                                continue;
                                            }
                                        } else if (ddef->second->op == Op::Load) {
                                            incOk = false;
                                            decIfNonNeg = true;
                                            if (!decDivSlot) decDivSlot = ddef->second->slot;
                                            else badDecStep = true;
                                            continue;
                                        }
                                    }
                                }
                            }
                            decOk = incOk = false;
                            badDecStep = true;
                            continue;
                        } else if (valOp->op == Op::Binary && valOp->data == "/") {
                            auto ls = loadSlot.find(valOp->inputs[0].id);
                            auto rdef = defOp.find(valOp->inputs[1].id);
                            if (ls != loadSlot.end() && ls->second == slot && rdef != defOp.end()) {
                                if (rdef->second->op == Op::Int) {
                                    if (auto c = parseConstInt(rdef->second->data); c && *c >= 1) {
                                        incOk = false;
                                        decIfNonNeg = true;
                                        continue;
                                    }
                                } else if (rdef->second->op == Op::Load) {
                                    incOk = false;
                                    decIfNonNeg = true;
                                    if (!decDivSlot) decDivSlot = rdef->second->slot;
                                    else badDecStep = true;
                                    continue;
                                }
                            }
                        }
                        decOk = incOk = false;
                        badDecStep = true;
                    }
                }
                if (incOk || decOk || (decIfNonNeg && !incOk && !badDecStep) || (decStepSlot && !badDecStep)) {
                    MonotoneSlot ms{slot, incOk, decOk && !decIfNonNeg && !decStepSlot, std::nullopt,
                                    decIfNonNeg && !badDecStep, false, badDecStep ? std::nullopt : decDivSlot};
                    if (!decOk && decStepSlot && !badDecStep) ms.decBySlot = decStepSlot;
                    else if (decOk && decIfNonNeg) ms.nonIncreasing = false;
                    else if (decOk && decStepSlot) { ms.nonIncreasing = false; ms.decBySlot = decStepSlot; }
                    if (forHeaders.find(lp.header) == forHeaders.end() && slotStoreCnt == 1 && !badDecStep && !incOk &&
                        !decIfNonNeg && (ms.decBySlot || (ms.nonIncreasing && constDecMin && *constDecMin >= 1))) {
                        auto guardProvesNonNeg = [&](Value cond) -> bool {
                            auto cdit = defOp.find(cond.id);
                            if (cdit == defOp.end()) return false;
                            const Operation* cop = cdit->second;
                            if (cop->op == Op::Load) {
                                auto scIt = shortCircuitAnd.find(cop->slot);
                                if (scIt == shortCircuitAnd.end()) return false;
                                cdit = defOp.find(scIt->second.first.id);
                                if (cdit == defOp.end()) return false;
                                cop = cdit->second;
                            }
                            if (cop->op != Op::Binary || cop->inputs.size() != 2) return false;
                            Value L = cop->inputs[0], R = cop->inputs[1];
                            std::string cmp = cop->data;
                            if (cmp == "<" || cmp == "<=") { std::swap(L, R); cmp = (cmp == "<") ? ">" : ">="; }
                            if (cmp != ">" && cmp != ">=") return false;
                            auto lls = loadSlot.find(L.id);
                            if (lls == loadSlot.end() || lls->second != slot) return false;
                            if (ms.decBySlot) {
                                auto rls = loadSlot.find(R.id);
                                return rls != loadSlot.end() && rls->second == *ms.decBySlot;
                            }
                            auto rdit = defOp.find(R.id);
                            if (rdit != defOp.end() && rdit->second->op == Op::Int) {
                                if (auto kc = parseConstInt(rdit->second->data)) {
                                    if (cmp == ">=") return *kc >= *constDecMin;
                                    if (cmp == ">" && *kc < kMaxInt) return (*kc + 1) >= *constDecMin;
                                }
                            }
                            return false;
                        };
                        unsigned cur = lastStoreBlk;
                        for (int hops = 0; hops < 32 && lp.body.contains(cur); ++hops) {
                            if (cfg.preds[cur].size() == 1) {
                                const unsigned p = cfg.preds[cur][0];
                                const auto& pb = fn.blocks[p];
                                if (pb.exit == Exit::Branch && pb.value && pb.yes == cur && pb.no != cur &&
                                    guardProvesNonNeg(*pb.value)) {
                                    ms.nonNegPreserved = true;
                                    break;
                                }
                            }
                            if (cur == lp.header || cfg.idom[cur] < 0) break;
                            cur = static_cast<unsigned>(cfg.idom[cur]);
                        }
                    }
                    loopMonotone[li].push_back(ms);
                }
            }

            // Detect single-store scalar accumulator slots acc = acc +/- step inside valid For loops
            auto fhIt = forHeaders.find(lp.header);
            if (fhIt != forHeaders.end() && fhIt->second.valid) {
                for (unsigned slot : loopWritten[li]) {
                    if (slot == fhIt->second.iterSlot || slot >= fn.slots.size() || fn.slots[slot] != Type::Int)
                        continue;
                    int storeCnt = 0;
                    unsigned storeBlk = 0;
                    const Operation* storeOp = nullptr;
                    bool allInDirectLoop = true;
                    std::vector<const Operation*> accStores;
                    for (unsigned b : lp.body) {
                        for (const auto& op : fn.blocks[b].ops) {
                            if (op.op == Op::Store && op.slot == slot) {
                                ++storeCnt;
                                storeBlk = b;
                                storeOp = &op;
                                accStores.push_back(&op);
                                if (cfg.loopOfBlock[b] != static_cast<int>(li)) allInDirectLoop = false;
                            }
                        }
                    }
                    if (!allInDirectLoop || !storeOp) continue;

                    // Ensure stepVal does not depend transitively on Load(slot) or cyclically on loop-written arrays
                    auto checkAccStep = [&](Value sv) -> bool {
                        std::unordered_set<unsigned> visitedVals;
                        std::unordered_set<unsigned> visitedSlots;
                        std::unordered_set<unsigned> visitedArrs;
                        auto rec = [&](auto& self, unsigned vid, int depth) -> bool {
                            if (depth > 32 || !visitedVals.insert(vid).second) return false;
                            auto it = defOp.find(vid);
                            if (it == defOp.end()) return false;
                            const Operation* o = it->second;
                            if (o->op == Op::Load) {
                                if (o->slot == slot) return true;
                                if (slotWritable[o->slot] && o->slot != fhIt->second.iterSlot &&
                                    loopWritten[li].contains(o->slot)) {
                                    if (o->slot >= fn.slots.size() || fn.slots[o->slot] != Type::Int) return true;
                                    if (visitedSlots.insert(o->slot).second) {
                                        for (unsigned b : lp.body) {
                                            for (const auto& sop : fn.blocks[b].ops) {
                                                if (sop.op == Op::Store && sop.slot == o->slot) {
                                                    if (sop.inputs.empty() || self(self, sop.inputs[0].id, depth + 1))
                                                        return true;
                                                }
                                            }
                                        }
                                    }
                                    return false;
                                }
                                if (!slotWritable[o->slot] && o->slot >= fn.source.params.size() && singleSlotVal[o->slot]) {
                                    if (self(self, singleSlotVal[o->slot]->id, depth + 1)) return true;
                                }
                                return false;
                            }
                            if (o->op == Op::Call || o->op == Op::ExternCall || o->op == Op::DynCall || o->op == Op::ClosureCall) return true;
                            if (o->op == Op::Index || o->op == Op::ReadByte) {
                                if (loopUnknownIndexSet[li] || o->op == Op::ReadByte || o->inputs.empty()) return true;
                                auto als = loadSlot.find(o->inputs[0].id);
                                if (als == loadSlot.end()) return true;
                                if (loopArrayWritten[li].contains(als->second)) {
                                    if (visitedArrs.insert(als->second).second) {
                                        for (const auto& ablk : fn.blocks) {
                                            for (const auto& iop : ablk.ops) {
                                                if (iop.op == Op::IndexSet && iop.inputs.size() >= 3) {
                                                    auto ws = loadSlot.find(iop.inputs[0].id);
                                                    if (ws != loadSlot.end() && ws->second == als->second &&
                                                        self(self, iop.inputs[2].id, depth + 1)) {
                                                        return true;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                return false;
                            }
                            for (const auto& in : o->inputs) {
                                if (self(self, in.id, depth + 1)) return true;
                            }
                            return false;
                        };
                        return rec(rec, sv.id, 0);
                    };

                    if (storeCnt == 1 && !storeOp->inputs.empty()) {
                        (void)storeBlk;
                        auto dit = defOp.find(storeOp->inputs[0].id);
                        if (dit == defOp.end() || dit->second->op != Op::Binary ||
                            (dit->second->data != "+" && dit->second->data != "-"))
                            continue;
                        const Operation* bin = dit->second;
                        auto ls = loadSlot.find(bin->inputs[0].id);
                        if (ls == loadSlot.end() || ls->second != slot) continue;
                        const Value stepVal = bin->inputs[1];
                        const bool isSub = (bin->data == "-");
                        if (!checkAccStep(stepVal)) {
                            loopAccOps[li].emplace_back(slot, stepVal, isSub);
                        }
                    } else if (storeCnt >= 2 && storeCnt <= 4) {
                        std::vector<std::pair<Value, bool>> steps;
                        bool okMulti = true;
                        for (const Operation* st : accStores) {
                            if (st->inputs.empty()) { okMulti = false; break; }
                            auto dit = defOp.find(st->inputs[0].id);
                            if (dit == defOp.end() || dit->second->op != Op::Binary ||
                                (dit->second->data != "+" && dit->second->data != "-")) {
                                okMulti = false; break;
                            }
                            auto ls = loadSlot.find(dit->second->inputs[0].id);
                            if (ls == loadSlot.end() || ls->second != slot) { okMulti = false; break; }
                            const Value stepVal = dit->second->inputs[1];
                            if (checkAccStep(stepVal)) { okMulti = false; break; }
                            steps.emplace_back(stepVal, dit->second->data == "-");
                        }
                        if (!okMulti || steps.empty()) continue;
                        std::unordered_map<unsigned, int> visState;
                        std::unordered_map<unsigned, std::int64_t> maxStoresMemo;
                        auto dfsMaxStores = [&](auto& self, unsigned b) -> std::int64_t {
                            auto sit = visState.find(b);
                            if (sit != visState.end()) {
                                return (sit->second == 1) ? -1 : maxStoresMemo[b];
                            }
                            visState[b] = 1;
                            std::int64_t here = 0;
                            for (const auto& op : fn.blocks[b].ops) {
                                if (op.op == Op::Store && op.slot == slot) ++here;
                            }
                            std::int64_t childMax = 0;
                            const auto& blk = fn.blocks[b];
                            auto visitSucc = [&](unsigned succ) -> bool {
                                if (!lp.body.contains(succ) || succ == lp.header) return true;
                                const std::int64_t sub = self(self, succ);
                                if (sub < 0) return false;
                                childMax = std::max(childMax, sub);
                                return true;
                            };
                            bool okDfs = true;
                            if (blk.exit == Exit::Jump) {
                                okDfs = visitSucc(blk.yes);
                            } else if (blk.exit == Exit::Branch) {
                                okDfs = visitSucc(blk.yes) && visitSucc(blk.no);
                            } else if (blk.exit == Exit::Switch) {
                                okDfs = visitSucc(blk.yes);
                                for (unsigned t : blk.targets) okDfs = okDfs && visitSucc(t);
                            }
                            visState[b] = 2;
                            if (!okDfs) { maxStoresMemo[b] = -1; return -1; }
                            maxStoresMemo[b] = here + childMax;
                            return here + childMax;
                        };
                        const std::int64_t maxPerIter = dfsMaxStores(dfsMaxStores, lp.header);
                        if (maxPerIter >= 1) {
                            loopMultiAccOps[li].push_back({slot, std::move(steps), maxPerIter});
                        }
                    }
                }
            }
        }
        // Also detect nested scalar acc = acc +/- step where slot is written once across a chain of valid For loops
        for (std::size_t li = 0; li < nLoops; ++li) {
            auto fhIt = forHeaders.find(cfg.loops[li].header);
            if (fhIt == forHeaders.end() || !fhIt->second.valid) continue;
            for (unsigned slot : loopWritten[li]) {
                if (slot == fhIt->second.iterSlot || slot >= fn.slots.size() || fn.slots[slot] != Type::Int)
                    continue;
                bool alreadyAdded = false;
                if (auto it = loopAccOps.find(li); it != loopAccOps.end()) {
                    for (const auto& [as, sv, isb] : it->second)
                        if (as == slot) alreadyAdded = true;
                }
                if (alreadyAdded) continue;
                int storeCnt = 0;
                unsigned storeBlk = 0;
                const Operation* storeOp = nullptr;
                for (unsigned b : cfg.loops[li].body) {
                    for (const auto& op : fn.blocks[b].ops) {
                        if (op.op == Op::Store && op.slot == slot) {
                            ++storeCnt; storeBlk = b; storeOp = &op;
                        }
                    }
                }
                if (storeCnt != 1 || cfg.loopOfBlock[storeBlk] == static_cast<int>(li) ||
                    !storeOp || storeOp->inputs.empty())
                    continue;
                // Ensure every loop inside li containing storeBlk is also a valid For loop
                bool allFor = true;
                for (std::size_t sub = 0; sub < nLoops; ++sub) {
                    if (cfg.loops[sub].body.contains(storeBlk) && cfg.loops[li].body.contains(cfg.loops[sub].header)) {
                        auto sf = forHeaders.find(cfg.loops[sub].header);
                        if (sf == forHeaders.end() || !sf->second.valid) { allFor = false; break; }
                    }
                }
                if (!allFor) continue;
                auto dit = defOp.find(storeOp->inputs[0].id);
                if (dit == defOp.end() || dit->second->op != Op::Binary ||
                    (dit->second->data != "+" && dit->second->data != "-"))
                    continue;
                auto ls = loadSlot.find(dit->second->inputs[0].id);
                if (ls == loadSlot.end() || ls->second != slot) continue;
                const Value stepVal = dit->second->inputs[1];
                const bool isSub = (dit->second->data == "-");
                std::unordered_set<unsigned> visVals;
                std::unordered_set<unsigned> visSlots;
                auto depCheck = [&](auto& self, unsigned vid, int d) -> bool {
                    if (d > 32 || !visVals.insert(vid).second) return false;
                    auto it2 = defOp.find(vid);
                    if (it2 == defOp.end()) return false;
                    const Operation* o = it2->second;
                    if (o->op == Op::Load) {
                        if (o->slot == slot) return true;
                        if (!slotWritable[o->slot] && o->slot < singleSlotVal.size() &&
                            o->slot >= fn.source.params.size() && singleSlotVal[o->slot]) {
                            return self(self, singleSlotVal[o->slot]->id, d + 1);
                        }
                        if (slotWritable[o->slot]) {
                            // Allowed only if an enclosing For iterSlot OR every Store inside li is acyclic and independent of `slot`.
                            bool isIter = false;
                            for (std::size_t sub = 0; sub < nLoops; ++sub) {
                                if (cfg.loops[sub].body.contains(storeBlk)) {
                                    auto sf = forHeaders.find(cfg.loops[sub].header);
                                    if (sf != forHeaders.end() && sf->second.iterSlot == o->slot) isIter = true;
                                }
                            }
                            if (isIter) return false;
                            if (!loopWritten[li].contains(o->slot)) return false;
                            if (!visSlots.insert(o->slot).second) return true;
                            for (unsigned b2 : cfg.loops[li].body) {
                                for (const auto& op2 : fn.blocks[b2].ops) {
                                    if (op2.op == Op::Store && op2.slot == o->slot && !op2.inputs.empty()) {
                                        if (self(self, op2.inputs[0].id, d + 1)) return true;
                                    }
                                }
                            }
                            visSlots.erase(o->slot);
                            return false;
                        }
                        return false;
                    }
                    if (o->op == Op::Call || o->op == Op::ExternCall || o->op == Op::DynCall || o->op == Op::ClosureCall) return true;
                    // Bounded modulo `expr % M` where M is loop-invariant doesn't depend on acc values!
                    if (o->op == Op::Binary && o->data == "%") {
                        auto mls = loadSlot.find(o->inputs[1].id);
                        bool invM = (mls == loadSlot.end()) || !loopWritten[li].contains(mls->second);
                        if (invM && !self(self, o->inputs[1].id, d + 1)) return false;
                    }
                    for (const auto& in : o->inputs)
                        if (self(self, in.id, d + 1)) return true;
                    return false;
                };
                if (!depCheck(depCheck, stepVal.id, 0)) {
                    loopAccOps[li].emplace_back(slot, stepVal, isSub);
                }
            }
        }
        for (std::size_t li = 0; li < nLoops; ++li) {
            const auto& lp = cfg.loops[li];
            auto fhIt = forHeaders.find(lp.header);

            // 1) Detect modulo-bounded & shift-copy slots across ALL loops (for & while): s = ... % M, s = src
            {
                auto isSlotLiveInLoop = [&](unsigned slot) -> bool {
                    if (slot < fn.source.params.size()) return true;
                    // Header cannot have a guaranteed write-before-read for a loop-local temp unless written before read; check if lp.header reads it before writing.
                    for (const auto& op : fn.blocks[lp.header].ops) {
                        if (op.op == Op::Store && op.slot == slot) break;
                        if (op.op == Op::Load && op.slot == slot) return true;
                    }
                    // Must find an unconditional block `defBlk` (in lp.body, dominating all loop uses of slot) that writes slot before reading it.
                    auto doms = [&](unsigned a, unsigned b) -> bool {
                        unsigned cur = b;
                        for (int hops = 0; hops < 128 && cur != a; ++hops) {
                            if (cur == 0 || cfg.idom[cur] < 0) return false;
                            cur = static_cast<unsigned>(cfg.idom[cur]);
                        }
                        return cur == a;
                    };
                    std::optional<unsigned> defBlk;
                    for (unsigned b : lp.body) {
                        if (b == lp.header) continue;
                        bool sawStore = false, loadBeforeStore = false;
                        for (const auto& op : fn.blocks[b].ops) {
                            if (op.op == Op::Load && op.slot == slot && !sawStore) { loadBeforeStore = true; break; }
                            if (op.op == Op::Store && op.slot == slot) { sawStore = true; break; }
                        }
                        if (sawStore && !loadBeforeStore) {
                            // Ensure `b` dominates every other block in `lp.body` that loads/stores `slot` AND dominates all backedges to `lp.header`
                            bool domsAllUses = true;
                            for (unsigned ob : lp.body) {
                                if (ob == b) continue;
                                bool uses = false;
                                for (const auto& op : fn.blocks[ob].ops)
                                    if ((op.op == Op::Load || op.op == Op::Store) && op.slot == slot) { uses = true; break; }
                                if (uses && !doms(b, ob)) { domsAllUses = false; break; }
                            }
                            if (domsAllUses) { defBlk = b; break; }
                        }
                    }
                    return !defBlk.has_value();
                };

                auto resolveTempStoreExpr = [&](unsigned storeBlk, const Operation* opNode) -> const Operation* {
                    if (!opNode || opNode->op != Op::Load) return opNode;
                    const unsigned ts = opNode->slot;
                    int tsCnt = 0, tsPos = -1, ldPos = -1;
                    const Operation* tsStore = nullptr;
                    bool otherBlk = false;
                    for (unsigned ob : lp.body) {
                        if (ob == storeBlk) continue;
                        for (const auto& op : fn.blocks[ob].ops)
                            if (op.op == Op::Store && op.slot == ts) otherBlk = true;
                    }
                    if (otherBlk) return opNode;
                    const auto& bOps = fn.blocks[storeBlk].ops;
                    for (int oi = 0; oi < static_cast<int>(bOps.size()); ++oi) {
                        if (bOps[oi].op == Op::Store && bOps[oi].slot == ts) { ++tsCnt; tsStore = &bOps[oi]; tsPos = oi; }
                        if (bOps[oi].op == Op::Load && bOps[oi].result.id == opNode->result.id) ldPos = oi;
                    }
                    if (tsCnt == 1 && tsStore && !tsStore->inputs.empty() && tsPos >= 0 && ldPos > tsPos) {
                        auto inner = defOp.find(tsStore->inputs[0].id);
                        if (inner != defOp.end()) return inner->second;
                    }
                    return opNode;
                };

                for (unsigned slot : loopWritten[li]) {
                    if (fhIt != forHeaders.end() && fhIt->second.valid && slot == fhIt->second.iterSlot) continue;
                    if (slot >= fn.slots.size() || fn.slots[slot] != Type::Int) continue;
                    int storeCnt = 0;
                    bool allModInLoop = true;
                    std::optional<unsigned> singleStoreBlk;
                    const Operation* singleStoreOp = nullptr;
                    std::vector<ModLoopOp> candMods;
                    const bool liveIn = isSlotLiveInLoop(slot);
                    for (unsigned b : lp.body) {
                        for (const auto& op : fn.blocks[b].ops) {
                            if (op.op != Op::Store || op.slot != slot) continue;
                            ++storeCnt;
                            singleStoreBlk = b;
                            singleStoreOp = &op;
                            if (cfg.loopOfBlock[b] != static_cast<int>(li) || op.inputs.empty()) {
                                allModInLoop = false;
                                continue;
                            }
                            auto dit = defOp.find(op.inputs[0].id);
                            const Operation* chased = (dit != defOp.end()) ? resolveTempStoreExpr(b, dit->second) : nullptr;
                            if (chased && chased->op == Op::Binary && chased->data == "%") {
                                Value modDiv = chased->inputs[1];
                                auto mls = loadSlot.find(modDiv.id);
                                if (mls == loadSlot.end() || !loopWritten[li].contains(mls->second)) {
                                    candMods.push_back({slot, modDiv, chased->result, chased->inputs[0], liveIn});
                                    continue;
                                }
                            }
                            allModInLoop = false;
                        }
                    }
                    if (allModInLoop && !candMods.empty()) {
                        for (auto& cm : candMods) loopModOps[li].push_back(cm);
                        continue;
                    }
                    if (storeCnt == 1 && singleStoreBlk && cfg.loopOfBlock[*singleStoreBlk] == static_cast<int>(li) &&
                        singleStoreOp && !singleStoreOp->inputs.empty()) {
                        auto dit = defOp.find(singleStoreOp->inputs[0].id);
                        if (dit != defOp.end() && dit->second->op == Op::Load && dit->second->slot != slot &&
                            dit->second->slot < fn.slots.size() && fn.slots[dit->second->slot] == Type::Int &&
                            !(dit->second->slot < fn.slotByRef.size() && fn.slotByRef[dit->second->slot])) {
                            bool beforeIterInc = false;
                            if (fhIt != forHeaders.end() && fhIt->second.valid &&
                                dit->second->slot == fhIt->second.iterSlot) {
                                const unsigned iterSlot = fhIt->second.iterSlot;
                                bool allIncsAtLatchEnd = true;
                                bool seenIterIncBeforeStore = false;
                                for (unsigned lb : lp.body) {
                                    const auto& lbOps = fn.blocks[lb].ops;
                                    for (std::size_t oi = 0; oi < lbOps.size(); ++oi) {
                                        const auto& lop = lbOps[oi];
                                        if (lop.op == Op::Store && lop.slot == iterSlot) {
                                            const bool isLatch =
                                                (fn.blocks[lb].exit == Exit::Jump && fn.blocks[lb].yes == lp.header);
                                            const bool isLast = (oi + 1 == lbOps.size());
                                            if (!isLatch || !isLast) allIncsAtLatchEnd = false;
                                            if (lb == *singleStoreBlk && &lop < singleStoreOp) {
                                                seenIterIncBeforeStore = true;
                                            }
                                        }
                                    }
                                }
                                beforeIterInc = allIncsAtLatchEnd && !seenIterIncBeforeStore;
                            }
                            loopCopyOps[li].push_back({slot, dit->second->slot, liveIn, beforeIterInc});
                        }
                    }
                }
            }

            // Detect max-clamped accumulators (s = max(C, s +/- delta))
            // and coupled shift-max (prev = best; best = max(best, prev +/- delta)) in valid For loops.
            if (fhIt != forHeaders.end() && fhIt->second.valid) {
                auto freeOfLoop = [&](unsigned targetSlot, Value root) {
                    std::unordered_set<unsigned> visitedVals;
                    auto rec = [&](auto& self, unsigned vid, int depth) -> bool {
                        if (depth > 32 || !visitedVals.insert(vid).second) return false;
                        auto it = defOp.find(vid);
                        if (it == defOp.end()) return false;
                        const Operation* o = it->second;
                        if (o->op == Op::Load) {
                            if (o->slot == targetSlot) return true;
                            if (slotWritable[o->slot] && o->slot != fhIt->second.iterSlot &&
                                loopWritten[li].contains(o->slot))
                                return true;
                            if (!slotWritable[o->slot] && o->slot < singleSlotVal.size() &&
                                o->slot >= fn.source.params.size() && singleSlotVal[o->slot]) {
                                if (self(self, singleSlotVal[o->slot]->id, depth + 1)) return true;
                            }
                            return false;
                        }
                        if (o->op == Op::Call || o->op == Op::ExternCall || o->op == Op::DynCall || o->op == Op::ClosureCall) return true;
                        for (const auto& in : o->inputs)
                            if (self(self, in.id, depth + 1)) return true;
                        return false;
                    };
                    return !rec(rec, root.id, 0);
                };

                for (unsigned slot : loopWritten[li]) {
                    if (slot == fhIt->second.iterSlot || slot >= fn.slots.size() || fn.slots[slot] != Type::Int)
                        continue;
                    int storeCnt = 0;
                    unsigned storeBlk = 0;
                    const Operation* storeOp = nullptr;
                    for (unsigned b : lp.body) {
                        for (const auto& op : fn.blocks[b].ops) {
                            if (op.op == Op::Store && op.slot == slot) {
                                ++storeCnt; storeBlk = b; storeOp = &op;
                            }
                        }
                    }
                    if (storeCnt != 1 || cfg.loopOfBlock[storeBlk] != static_cast<int>(li) ||
                        !storeOp || storeOp->inputs.empty())
                        continue;
                    auto dit = defOp.find(storeOp->inputs[0].id);
                    if (dit == defOp.end()) continue;
                    // 2) s = max(C, expr) where expr is Load(s) +/- stepVal (or (Load(s) + step) - C),
                    //    OR coupled shift-max: best = max(best, take) where take = Load(prev) +/- delta and prev = best.
                    bool matchedMaxMin = false;
                    if (dit->second->op == Op::Max && dit->second->inputs.size() == 2) {
                        for (int side = 0; side < 2; ++side) {
                            Value clampV = dit->second->inputs[1 - side];
                            Value exprV = dit->second->inputs[side];
                            auto edef = defOp.find(exprV.id);
                            if (freeOfLoop(slot, clampV)) {
                                if (edef != defOp.end() && edef->second->op == Op::Binary &&
                                    (edef->second->data == "+" || edef->second->data == "-")) {
                                    auto ls0 = loadSlot.find(edef->second->inputs[0].id);
                                    if (ls0 != loadSlot.end() && ls0->second == slot &&
                                        freeOfLoop(slot, edef->second->inputs[1])) {
                                        loopMaxClampOps[li].push_back({slot, clampV, edef->second->inputs[1], edef->second->data == "-"});
                                        matchedMaxMin = true;
                                        break;
                                    }
                                }
                                // Also handle max(0, (s + v) - 503): exprV = (Load(s) + x) - c
                                if (edef != defOp.end() && edef->second->op == Op::Binary && edef->second->data == "-") {
                                    auto inner = defOp.find(edef->second->inputs[0].id);
                                    auto innerC = edef->second->inputs[1];
                                    if (inner != defOp.end() && inner->second->op == Op::Binary && inner->second->data == "+" &&
                                        loadSlot.count(inner->second->inputs[0].id) &&
                                        loadSlot.at(inner->second->inputs[0].id) == slot &&
                                        freeOfLoop(slot, inner->second->inputs[1]) && freeOfLoop(slot, innerC)) {
                                        loopMaxClampOps[li].push_back({slot, clampV, inner->second->inputs[1], false, innerC, true});
                                        matchedMaxMin = true;
                                        break;
                                    }
                                }
                            }
                            // 3) Coupled shift-max in single body block: best = max(best, take); prev = best;
                            auto bSelf = loadSlot.find(clampV.id);
                            if (bSelf != loadSlot.end() && bSelf->second == slot && lp.body.size() <= 2) {
                                const Operation* takeBin = nullptr;
                                int takeIdx = -1;
                                const auto& bOps = fn.blocks[storeBlk].ops;
                                if (edef != defOp.end() && edef->second->op == Op::Binary &&
                                    (edef->second->data == "+" || edef->second->data == "-")) {
                                    takeBin = edef->second;
                                } else if (edef != defOp.end() && edef->second->op == Op::Load) {
                                    // Chase single-store local temp in storeBlk (e.g. `var vTake = vPrev + vVal(...)`)
                                    const unsigned tmpS = edef->second->slot;
                                    if (tmpS != slot) {
                                        int tStores = 0;
                                        const Operation* tStoreOp = nullptr;
                                        int tStorePos = -1, tLoadPos = -1;
                                        for (int oi = 0; oi < static_cast<int>(bOps.size()); ++oi) {
                                            if (bOps[oi].op == Op::Store && bOps[oi].slot == tmpS) {
                                                ++tStores; tStoreOp = &bOps[oi]; tStorePos = oi;
                                            }
                                            if (bOps[oi].op == Op::Load && bOps[oi].result.id == exprV.id) tLoadPos = oi;
                                        }
                                        bool otherTmp = false;
                                        for (unsigned ob : lp.body) {
                                            if (ob == storeBlk) continue;
                                            for (const auto& op : fn.blocks[ob].ops)
                                                if (op.op == Op::Store && op.slot == tmpS) otherTmp = true;
                                        }
                                        if (!otherTmp && tStores == 1 && tStoreOp && !tStoreOp->inputs.empty() &&
                                            tStorePos >= 0 && tLoadPos > tStorePos) {
                                            auto td = defOp.find(tStoreOp->inputs[0].id);
                                            if (td != defOp.end() && td->second->op == Op::Binary &&
                                                (td->second->data == "+" || td->second->data == "-")) {
                                                takeBin = td->second;
                                                takeIdx = tStorePos;
                                            }
                                        }
                                    }
                                }
                                if (takeBin) {
                                    auto pLs = loadSlot.find(takeBin->inputs[0].id);
                                    Value deltaV = takeBin->inputs[1];
                                    const bool isSubTake = (takeBin->data == "-");
                                    if (pLs != loadSlot.end() && pLs->second != slot && freeOfLoop(slot, deltaV)) {
                                        const unsigned pSlot = pLs->second;
                                        int pStores = 0;
                                        bool okOrder = false;
                                        int loadIdx = -1, pStoreIdx = -1;
                                        for (int oi = 0; oi < static_cast<int>(bOps.size()); ++oi) {
                                            if (bOps[oi].op == Op::Load && bOps[oi].result.id == takeBin->inputs[0].id) loadIdx = oi;
                                            if (bOps[oi].op == Op::Store && bOps[oi].slot == pSlot) {
                                                ++pStores;
                                                pStoreIdx = oi;
                                                auto psd = loadSlot.find(bOps[oi].inputs[0].id);
                                                if (psd != loadSlot.end() && psd->second == slot && loadIdx >= 0 &&
                                                    loadIdx < pStoreIdx && (takeIdx < 0 || takeIdx < pStoreIdx))
                                                    okOrder = true;
                                            }
                                        }
                                        for (unsigned ob : lp.body) {
                                            if (ob == storeBlk) continue;
                                            for (const auto& op : fn.blocks[ob].ops)
                                                if (op.op == Op::Store && op.slot == pSlot) okOrder = false;
                                        }
                                        if (pStores == 1 && okOrder) {
                                            loopCoupledMaxOps[li].push_back({pSlot, slot, deltaV, isSubTake});
                                            matchedMaxMin = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (!matchedMaxMin &&
                        (dit->second->op == Op::Max || dit->second->op == Op::Min) &&
                        dit->second->inputs.size() == 2) {
                        const bool isMinOp = (dit->second->op == Op::Min);
                        for (int side = 0; side < 2; ++side) {
                            auto selfLs = loadSlot.find(dit->second->inputs[side].id);
                            if (selfLs == loadSlot.end() || selfLs->second != slot) continue;
                            const Value other = dit->second->inputs[1 - side];
                            if (auto oLs = loadSlot.find(other.id); oLs != loadSlot.end() && oLs->second != slot) {
                                loopRunningExtremeOps[li].push_back({slot, oLs->second, other, true, isMinOp});
                                break;
                            } else if (freeOfLoop(slot, other)) {
                                loopRunningExtremeOps[li].push_back({slot, 0, other, false, isMinOp});
                                break;
                            }
                        }
                    }
                }
            }

            // Detect decreasing while-loop upper bound (e.g. bubbleEarly: while edge > 1 { var last = 0; for j in 1..edge { last = j }; edge = last })
            if (fhIt == forHeaders.end()) {
                const auto& whb = fn.blocks[lp.header];
                if (whb.exit == Exit::Branch && whb.value && lp.body.contains(whb.yes) && !lp.body.contains(whb.no)) {
                    for (unsigned slot : loopWritten[li]) {
                        if (slot >= fn.slots.size() || fn.slots[slot] != Type::Int) continue;
                        bool alreadyMonotone = false;
                        for (const auto& ms : loopMonotone[li])
                            if (ms.slot == slot) alreadyMonotone = true;
                        if (alreadyMonotone) continue;
                        // Check header ensures slot >= 1 (e.g. slot > 0, slot > 1, slot >= 1)
                        bool positiveInBody = false;
                        if (auto cdit = defOp.find(whb.value->id); cdit != defOp.end() && cdit->second->op == Op::Binary) {
                            const auto* cmp = cdit->second;
                            auto ls0 = loadSlot.find(cmp->inputs[0].id);
                            if (ls0 != loadSlot.end() && ls0->second == slot) {
                                if (auto c = parseConstInt(defOp.count(cmp->inputs[1].id) ? defOp.at(cmp->inputs[1].id)->data : "")) {
                                    if ((cmp->data == ">" && *c >= 0) || (cmp->data == ">=" && *c >= 1))
                                        positiveInBody = true;
                                }
                            }
                        }
                        if (!positiveInBody) continue;
                        // Verify every store to `slot` in lp.body loads a candidate `lastSlot` and is outside any inner loop referencing `slot`
                        std::optional<unsigned> lastSlot;
                        bool okEdge = true;
                        for (unsigned b : lp.body) {
                            for (const auto& op : fn.blocks[b].ops) {
                                if (op.op != Op::Store || op.slot != slot || op.inputs.empty()) continue;
                                auto dit = defOp.find(op.inputs[0].id);
                                if (dit != defOp.end() && dit->second->op == Op::Load) {
                                    if (!lastSlot) lastSlot = dit->second->slot;
                                    else if (*lastSlot != dit->second->slot) okEdge = false;
                                } else {
                                    okEdge = false;
                                }
                            }
                        }
                        if (!okEdge || !lastSlot || *lastSlot == slot) continue;
                        // Verify every store to `lastSlot` in lp.body is either `0` or iterator of an inner For(lo..slot)
                        bool okLast = false;
                        for (unsigned b : lp.body) {
                            for (const auto& op : fn.blocks[b].ops) {
                                if (op.op != Op::Store || op.slot != *lastSlot || op.inputs.empty()) continue;
                                auto dit = defOp.find(op.inputs[0].id);
                                if (dit == defOp.end()) { okLast = false; break; }
                                if (dit->second->op == Op::Int) {
                                    auto c = parseConstInt(dit->second->data);
                                    if (!c || *c < 0 || *c > 0) { okLast = false; break; }
                                    okLast = true;
                                } else if (dit->second->op == Op::Load) {
                                    const unsigned candIter = dit->second->slot;
                                    bool matchedInnerFor = false;
                                    for (unsigned ib : lp.body) {
                                        if (auto ifh = forHeaders.find(ib); ifh != forHeaders.end() && ifh->second.valid &&
                                            ifh->second.iterSlot == candIter) {
                                            auto uls = loadSlot.find(ifh->second.upper.id);
                                            if (uls != loadSlot.end() && uls->second == slot) {
                                                // Ensure `slot` itself is never written inside inner loop `ib`
                                                const std::size_t ili = cfg.headerToLoop.at(ib);
                                                if (!loopWritten[ili].contains(slot)) matchedInnerFor = true;
                                            }
                                        }
                                    }
                                    if (!matchedInnerFor) { okLast = false; break; }
                                    okLast = true;
                                } else {
                                    okLast = false;
                                    break;
                                }
                            }
                            if (!okLast && !lp.body.contains(b)) break;
                        }
                        if (okLast) loopMonotone[li].push_back({slot, false, true});
                    }
                }
            }

            // Detect sentinel while loop: preheader sets arr[sentIdx] = target & j = 0; while arr[j] != target { j += 1 }
            if (fhIt == forHeaders.end() && lp.preheaders.size() == 1) {
                const auto& whb = fn.blocks[lp.header];
                if (whb.exit == Exit::Branch && whb.value && lp.body.contains(whb.yes) && !lp.body.contains(whb.no)) {
                    const Operation* cmp = defOp.count(whb.value->id) ? defOp.at(whb.value->id) : nullptr;
                    bool condNotEq = (cmp && cmp->op == Op::Binary && cmp->data == "!=");
                    if (cmp && cmp->op == Op::Not) {
                        auto inner = defOp.find(cmp->inputs[0].id);
                        if (inner != defOp.end() && inner->second->op == Op::Binary && inner->second->data == "==") {
                            cmp = inner->second;
                            condNotEq = true;
                        }
                    }
                    if (condNotEq && cmp && cmp->inputs.size() == 2) {
                        const Operation* idxOp = nullptr;
                        std::optional<unsigned> targetSlot;
                        for (int side = 0; side < 2; ++side) {
                            auto dL = defOp.find(cmp->inputs[side].id);
                            auto dR = defOp.find(cmp->inputs[1 - side].id);
                            if (dL != defOp.end() && dL->second->op == Op::Index && dR != defOp.end() && dR->second->op == Op::Load) {
                                idxOp = dL->second;
                                targetSlot = dR->second->slot;
                                break;
                            }
                        }
                        if (idxOp && targetSlot && !loopWritten[li].contains(*targetSlot)) {
                            auto aLs = loadSlot.find(idxOp->inputs[0].id);
                            auto jLs = loadSlot.find(idxOp->inputs[1].id);
                            if (aLs != loadSlot.end() && jLs != loadSlot.end()) {
                                const unsigned arrSlot = aLs->second, jSlot = jLs->second;
                                // Verify loop body has no IndexSet, doesn't modify arrSlot, and only increments jSlot by 1 outside header
                                bool okBody = !loopWritten[li].contains(arrSlot);
                                int jStores = 0;
                                for (unsigned b : lp.body) {
                                    for (const auto& op : fn.blocks[b].ops) {
                                        if (op.op == Op::IndexSet || op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall || op.op == Op::ClosureCall) { okBody = false; break; }
                                        if (op.op == Op::Store && op.slot == jSlot) {
                                            if (b == lp.header) { okBody = false; break; }
                                            ++jStores;
                                            auto dit = defOp.find(op.inputs[0].id);
                                            if (dit == defOp.end() || dit->second->op != Op::Binary || dit->second->data != "+") {
                                                okBody = false; break;
                                            }
                                            auto jl = loadSlot.find(dit->second->inputs[0].id);
                                            auto rc = parseConstInt(defOp.count(dit->second->inputs[1].id) ? defOp.at(dit->second->inputs[1].id)->data : "");
                                            if (jl == loadSlot.end() || jl->second != jSlot || rc != 1) { okBody = false; break; }
                                        }
                                    }
                                    if (!okBody) break;
                                }
                                if (okBody && jStores == 1) {
                                    // Verify preheader sets arr[sentIdx] = Load(targetSlot) and jSlot = 0 without subsequent array/target writes
                                    const auto& preOps = fn.blocks[lp.preheaders[0]].ops;
                                    bool sawSentSet = false, sawJZero = false, clean = true;
                                    for (auto it = preOps.rbegin(); it != preOps.rend(); ++it) {
                                        if (it->op == Op::Store && it->slot == jSlot) {
                                            if (!sawJZero) {
                                                auto d0 = defOp.find(it->inputs[0].id);
                                                if (d0 != defOp.end() && d0->second->op == Op::Int && parseConstInt(d0->second->data) == 0)
                                                    sawJZero = true;
                                                else { clean = false; break; }
                                            }
                                        } else if (it->op == Op::IndexSet && !it->inputs.empty()) {
                                            auto als2 = loadSlot.find(it->inputs[0].id);
                                            if (als2 != loadSlot.end() && als2->second == arrSlot) {
                                                if (!sawSentSet) {
                                                    auto tls2 = loadSlot.find(it->inputs[2].id);
                                                    if (tls2 != loadSlot.end() && tls2->second == *targetSlot) sawSentSet = true;
                                                    else { clean = false; break; }
                                                }
                                            }
                                        } else if (it->op == Op::Store && (it->slot == *targetSlot || it->slot == arrSlot)) {
                                            if (!sawSentSet) { clean = false; break; }
                                        }
                                        if (sawSentSet && sawJZero) break;
                                    }
                                    if (clean && sawSentSet && sawJZero) {
                                        sentinelLoops.emplace(li, SentinelLoop{arrSlot, jSlot});
                                        loopMonotone[li].push_back({jSlot, true, false});
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Detect sqrt while loop: while d * d <= x { ... d = d + 1 } (direct or &&-short-circuit LHS)
            if (fhIt == forHeaders.end() && lp.preheaders.size() == 1) {
                const auto& whb = fn.blocks[lp.header];
                std::optional<Value> sqrtCondVal;
                std::optional<unsigned> andExitBlk;
                if (whb.exit == Exit::Branch && whb.value && lp.body.contains(whb.yes)) {
                    if (!lp.body.contains(whb.no)) {
                        sqrtCondVal = *whb.value;
                    } else if (whb.no < fn.blocks.size()) {
                        const auto& eb = fn.blocks[whb.no];
                        if (eb.exit == Exit::Branch && eb.value && lp.body.contains(eb.yes) && !lp.body.contains(eb.no) &&
                            eb.ops.size() == 1 && eb.ops[0].op == Op::Load && eb.ops[0].result.id == eb.value->id) {
                            if (auto scIt = shortCircuitAnd.find(eb.ops[0].slot);
                                scIt != shortCircuitAnd.end() && scIt->second.first.id == whb.value->id) {
                                sqrtCondVal = scIt->second.first;
                                andExitBlk = whb.no;
                            }
                        }
                    }
                }
                if (sqrtCondVal) {
                    auto cdit = defOp.find(sqrtCondVal->id);
                    if (cdit != defOp.end() && cdit->second->op == Op::Binary &&
                        (cdit->second->data == "<=" || cdit->second->data == "<")) {
                        auto ldit = defOp.find(cdit->second->inputs[0].id);
                        if (ldit != defOp.end() && ldit->second->op == Op::Binary && ldit->second->data == "*") {
                            auto ls0 = loadSlot.find(ldit->second->inputs[0].id);
                            auto ls1 = loadSlot.find(ldit->second->inputs[1].id);
                            if (ls0 != loadSlot.end() && ls1 != loadSlot.end() && ls0->second == ls1->second) {
                                const unsigned dSlot = ls0->second;
                                const Value boundVal = cdit->second->inputs[1];
                                auto bls = loadSlot.find(boundVal.id);
                                bool boundInvariant = (bls == loadSlot.end()) || !loopWritten[li].contains(bls->second);
                                // Also allow x = x / dSlot inside the loop (monotone nonIncreasing non-negative x, as in totientBatch)
                                bool boundDivMonotone = false;
                                if (!boundInvariant && bls != loadSlot.end()) {
                                    bool onlyDivByD = true;
                                    for (unsigned b : lp.body) {
                                        for (const auto& op : fn.blocks[b].ops) {
                                            if (op.op != Op::Store || op.slot != bls->second || op.inputs.empty()) continue;
                                            auto dOpIt = defOp.find(op.inputs[0].id);
                                            if (dOpIt == defOp.end() || dOpIt->second->op != Op::Binary || dOpIt->second->data != "/") {
                                                onlyDivByD = false; break;
                                            }
                                            auto lhsX = loadSlot.find(dOpIt->second->inputs[0].id);
                                            auto rhsD = loadSlot.find(dOpIt->second->inputs[1].id);
                                            if (lhsX == loadSlot.end() || lhsX->second != bls->second ||
                                                rhsD == loadSlot.end() || rhsD->second != dSlot) {
                                                onlyDivByD = false; break;
                                            }
                                        }
                                        if (!onlyDivByD) break;
                                    }
                                    boundDivMonotone = onlyDivByD;
                                }
                                int dStores = 0;
                                bool okStep = boundInvariant || boundDivMonotone;
                                for (unsigned b : lp.body) {
                                    for (const auto& op : fn.blocks[b].ops) {
                                        if (op.op == Op::Store && op.slot == dSlot) {
                                            if (b == lp.header) { okStep = false; break; }
                                            ++dStores;
                                            auto dit = defOp.find(op.inputs[0].id);
                                            if (dit == defOp.end() || dit->second->op != Op::Binary || dit->second->data != "+") {
                                                okStep = false; break;
                                            }
                                            auto dls = loadSlot.find(dit->second->inputs[0].id);
                                            auto rc = parseConstInt(defOp.count(dit->second->inputs[1].id) ? defOp.at(dit->second->inputs[1].id)->data : "");
                                            if (dls == loadSlot.end() || dls->second != dSlot || rc != 1) { okStep = false; break; }
                                        }
                                    }
                                    if (!okStep) break;
                                }
                                if (okStep && dStores == 1) {
                                    sqrtLoops.emplace(li, SqrtLoop{dSlot, boundVal, andExitBlk});
                                    loopMonotone[li].push_back({dSlot, true, false});
                                }
                            }
                        }
                    }
                }
            }

            // Detect binary-search loop: while lo < hi { var mid = (lo + hi) / 2; ... lo = mid + 1 | hi = mid }
            if (fhIt == forHeaders.end() && lp.preheaders.size() == 1) {
                const auto& whb = fn.blocks[lp.header];
                if (whb.exit == Exit::Branch && whb.value && lp.body.contains(whb.yes) && !lp.body.contains(whb.no)) {
                    auto cdit = defOp.find(whb.value->id);
                    if (cdit != defOp.end() && cdit->second->op == Op::Binary && cdit->second->data == "<") {
                        auto loLs = loadSlot.find(cdit->second->inputs[0].id);
                        auto hiLs = loadSlot.find(cdit->second->inputs[1].id);
                        if (loLs != loadSlot.end() && hiLs != loadSlot.end() && loLs->second != hiLs->second) {
                            const unsigned loS = loLs->second, hiS = hiLs->second;
                            auto isMidExpr = [&](Value v) -> bool {
                                // v can be Load(midSlot) storing (Load(loS) + Load(hiS)) / 2 or direct / 2
                                Value cur = v;
                                if (auto mls = loadSlot.find(cur.id); mls != loadSlot.end() &&
                                    !loopWritten[li].contains(mls->second))
                                    return false;
                                auto cdef = defOp.find(cur.id);
                                if (cdef != defOp.end() && cdef->second->op == Op::Load) {
                                    const unsigned ms = cdef->second->slot;
                                    int mCnt = 0;
                                    for (unsigned b : lp.body) {
                                        for (const auto& op : fn.blocks[b].ops) {
                                            if (op.op == Op::Store && op.slot == ms && !op.inputs.empty()) {
                                                ++mCnt;
                                                cur = op.inputs[0];
                                            }
                                        }
                                    }
                                    if (mCnt != 1) return false;
                                    cdef = defOp.find(cur.id);
                                }
                                if (cdef == defOp.end() || cdef->second->op != Op::Binary || cdef->second->data != "/")
                                    return false;
                                auto two = parseConstInt(defOp.count(cdef->second->inputs[1].id) ? defOp.at(cdef->second->inputs[1].id)->data : "");
                                if (two != 2) return false;
                                auto sumDef = defOp.find(cdef->second->inputs[0].id);
                                if (sumDef == defOp.end() || sumDef->second->op != Op::Binary || sumDef->second->data != "+")
                                    return false;
                                auto aLs = loadSlot.find(sumDef->second->inputs[0].id);
                                auto bLs = loadSlot.find(sumDef->second->inputs[1].id);
                                return aLs != loadSlot.end() && bLs != loadSlot.end() &&
                                       ((aLs->second == loS && bLs->second == hiS) ||
                                        (aLs->second == hiS && bLs->second == loS));
                            };
                            bool okBS = true;
                            int loStores = 0, hiStores = 0;
                            for (unsigned b : lp.body) {
                                for (const auto& op : fn.blocks[b].ops) {
                                    if (op.op != Op::Store || op.inputs.empty()) continue;
                                    if (op.slot == loS) {
                                        ++loStores;
                                        auto d = defOp.find(op.inputs[0].id);
                                        if (d == defOp.end() || d->second->op != Op::Binary || d->second->data != "+") {
                                            okBS = false; break;
                                        }
                                        auto c1 = parseConstInt(defOp.count(d->second->inputs[1].id) ? defOp.at(d->second->inputs[1].id)->data : "");
                                        if (c1 != 1 || !isMidExpr(d->second->inputs[0])) { okBS = false; break; }
                                    } else if (op.slot == hiS) {
                                        ++hiStores;
                                        if (!isMidExpr(op.inputs[0])) { okBS = false; break; }
                                    }
                                }
                                if (!okBS) break;
                            }
                            if (okBS && loStores >= 1 && hiStores >= 1) {
                                binarySearchLoops.emplace(li, BinarySearchLoop{loS, hiS});
                            }
                        }
                    }
                }
            }

            // Detect geometric-shrinking while loops (while x > 0 / while x % d == 0 { ... x = x / D })
            if (fhIt == forHeaders.end() && lp.preheaders.size() == 1) {
                const auto& whb = fn.blocks[lp.header];
                if (whb.exit == Exit::Branch && whb.value && lp.body.contains(whb.yes) && !lp.body.contains(whb.no)) {
                    auto cdit = defOp.find(whb.value->id);
                    if (cdit != defOp.end() && cdit->second->op == Op::Binary) {
                        std::optional<unsigned> shrinkSlot;
                        std::optional<unsigned> hdrModDivSlot;
                        std::optional<std::int64_t> hdrModDivConst;
                        if (cdit->second->data == ">" || cdit->second->data == ">=") {
                            if (auto ls0 = loadSlot.find(cdit->second->inputs[0].id); ls0 != loadSlot.end()) {
                                if (auto rc = parseConstInt(defOp.count(cdit->second->inputs[1].id) ? defOp.at(cdit->second->inputs[1].id)->data : "")) {
                                    if ((cdit->second->data == ">" && *rc >= 0) || (cdit->second->data == ">=" && *rc >= 1))
                                        shrinkSlot = ls0->second;
                                }
                            }
                        } else if (cdit->second->data == "==") {
                            for (int side = 0; side < 2; ++side) {
                                auto rc = parseConstInt(defOp.count(cdit->second->inputs[1 - side].id) ? defOp.at(cdit->second->inputs[1 - side].id)->data : "");
                                auto md = defOp.find(cdit->second->inputs[side].id);
                                if (rc == 0 && md != defOp.end() && md->second->op == Op::Binary && md->second->data == "%") {
                                    if (auto ls0 = loadSlot.find(md->second->inputs[0].id); ls0 != loadSlot.end()) {
                                        shrinkSlot = ls0->second;
                                        if (auto dc = parseConstInt(defOp.count(md->second->inputs[1].id) ? defOp.at(md->second->inputs[1].id)->data : "")) {
                                            if (*dc >= 2) hdrModDivConst = *dc;
                                        } else if (auto dls = loadSlot.find(md->second->inputs[1].id); dls != loadSlot.end() && !loopWritten[li].contains(dls->second)) {
                                            hdrModDivSlot = dls->second;
                                        }
                                    }
                                }
                            }
                        }
                        if (shrinkSlot && loopWritten[li].contains(*shrinkSlot)) {
                            const unsigned sSlot = *shrinkSlot;
                            int divStores = 0;
                            bool okShrink = true;
                            std::int64_t minC = 1LL << 60;
                            std::optional<unsigned> dSlot;
                            for (unsigned b : lp.body) {
                                for (const auto& op : fn.blocks[b].ops) {
                                    if (op.op != Op::Store || op.slot != sSlot || op.inputs.empty()) continue;
                                    ++divStores;
                                    auto d = defOp.find(op.inputs[0].id);
                                    if (d == defOp.end() || d->second->op != Op::Binary || d->second->data != "/") {
                                        okShrink = false; break;
                                    }
                                    auto lhs = loadSlot.find(d->second->inputs[0].id);
                                    if (lhs == loadSlot.end() || lhs->second != sSlot) { okShrink = false; break; }
                                    if (auto dc = parseConstInt(defOp.count(d->second->inputs[1].id) ? defOp.at(d->second->inputs[1].id)->data : "")) {
                                        if (*dc < 2) { okShrink = false; break; }
                                        minC = std::min(minC, *dc);
                                    } else if (auto ds = loadSlot.find(d->second->inputs[1].id);
                                               ds != loadSlot.end() && !loopWritten[li].contains(ds->second)) {
                                        if (!dSlot) dSlot = ds->second;
                                        else if (*dSlot != ds->second) { okShrink = false; break; }
                                    } else {
                                        okShrink = false; break;
                                    }
                                }
                                if (!okShrink) break;
                            }
                            if (okShrink && divStores >= 1 && (hdrModDivSlot || hdrModDivConst || cdit->second->data != "==")) {
                                if (!hdrModDivSlot && !hdrModDivConst) {
                                    divShrinkLoops.emplace(li, DivShrinkLoop{sSlot, dSlot ? 0 : minC, dSlot});
                                } else if ((hdrModDivConst && !dSlot) || (hdrModDivSlot && dSlot == hdrModDivSlot)) {
                                    divShrinkLoops.emplace(li, DivShrinkLoop{sSlot, dSlot ? 0 : minC, dSlot});
                                }
                            }
                        }
                    }
                }
            }

            // Detect guarded monotone counters: s initialized <= M and incremented (+1) only when s != M or s < M
            for (unsigned s : loopWritten[li]) {
                if (s >= fn.slots.size() || fn.slots[s] != Type::Int) continue;
                if (fhIt != forHeaders.end() && fhIt->second.valid && s == fhIt->second.iterSlot) continue;
                bool okCnt = true;
                int cntStores = 0;
                std::vector<unsigned> storeBlks;
                for (unsigned b : lp.body) {
                    for (const auto& op : fn.blocks[b].ops) {
                        if (op.op != Op::Store || op.slot != s) continue;
                        ++cntStores;
                        storeBlks.push_back(b);
                        if (op.inputs.empty()) { okCnt = false; break; }
                        auto d = defOp.find(op.inputs[0].id);
                        if (d == defOp.end() || d->second->op != Op::Binary || d->second->data != "+") { okCnt = false; break; }
                        auto ls0 = loadSlot.find(d->second->inputs[0].id);
                        auto rc = parseConstInt(defOp.count(d->second->inputs[1].id) ? defOp.at(d->second->inputs[1].id)->data : "");
                        if (ls0 == loadSlot.end() || ls0->second != s || rc != 1) { okCnt = false; break; }
                    }
                    if (!okCnt) break;
                }
                if (!okCnt || cntStores == 0) continue;
                // Find an invariant boundSlot checked by a dominating branch (s != M or s < M) for EVERY storeBlk
                auto findGuardBound = [&](unsigned blk) -> std::optional<unsigned> {
                    unsigned cur = blk;
                    for (int hops = 0; hops < 32 && lp.body.contains(cur); ++hops) {
                        if (cfg.preds[cur].size() == 1) {
                            const unsigned p = cfg.preds[cur][0];
                            const auto& pb = fn.blocks[p];
                            if (pb.exit == Exit::Branch && pb.value && pb.yes != pb.no) {
                                const bool tookYes = (pb.yes == cur);
                                if (auto cdit = defOp.find(pb.value->id); cdit != defOp.end() && cdit->second->op == Op::Binary) {
                                    const auto* cop = cdit->second;
                                    const bool isNe = (cop->data == "!=" && tookYes) || (cop->data == "==" && !tookYes);
                                    const bool isLt = (cop->data == "<" && tookYes) || (cop->data == ">=" && !tookYes);
                                    const bool isGt = (cop->data == ">" && tookYes) || (cop->data == "<=" && !tookYes);
                                    if (isNe || isLt) {
                                        for (int side = 0; side < (isNe ? 2 : 1); ++side) {
                                            auto lsL = loadSlot.find(cop->inputs[side].id);
                                            auto lsR = loadSlot.find(cop->inputs[1 - side].id);
                                            if (lsL != loadSlot.end() && lsL->second == s &&
                                                lsR != loadSlot.end() && !loopWritten[li].contains(lsR->second))
                                                return lsR->second;
                                        }
                                    } else if (isGt) {
                                        auto lsL = loadSlot.find(cop->inputs[0].id);
                                        auto lsR = loadSlot.find(cop->inputs[1].id);
                                        if (lsR != loadSlot.end() && lsR->second == s &&
                                            lsL != loadSlot.end() && !loopWritten[li].contains(lsL->second))
                                            return lsL->second;
                                    }
                                }
                            }
                        }
                        if (cur == lp.header || cfg.idom[cur] < 0) break;
                        cur = static_cast<unsigned>(cfg.idom[cur]);
                    }
                    return {};
                };
                std::optional<unsigned> bnd;
                for (unsigned sb : storeBlks) {
                    auto gb = findGuardBound(sb);
                    if (!gb || (bnd && *bnd != *gb)) { bnd.reset(); break; }
                    bnd = gb;
                }
                if (bnd) guardedCounterLoops[li].push_back({s, *bnd});
            }

            // Detect two-way partition for k in 0..(2*N): left/right counters incremented exclusively on if/else
            if (fhIt != forHeaders.end() && fhIt->second.valid) {
                const auto& fh = fhIt->second;
                auto loC = parseConstInt(defOp.count(fh.lower.id) ? defOp.at(fh.lower.id)->data : "");
                if (loC == 0) {
                    if (auto us = shallowSym(fh.upper.id, Env{}); us && us->isPureLinear() &&
                        us->linear.size() == 1 && us->constant == 0 && us->linear.begin()->second == 2) {
                        const unsigned bndSlot = us->linear.begin()->first;
                        if (!loopWritten[li].contains(bndSlot)) {
                            const unsigned eqBlk = fh.bodyBlock;
                            const auto& eqB = fn.blocks[eqBlk];
                            // Look for dispatch branch (slotX) -> (thenBlk, elseBlk), each updating one counter += 1 and jumping to single latch -> hdr
                            for (unsigned dispBlk : lp.body) {
                                const auto& db = fn.blocks[dispBlk];
                                if (db.exit != Exit::Branch || !db.value || db.yes == db.no ||
                                    !lp.body.contains(db.yes) || !lp.body.contains(db.no))
                                    continue;
                                const unsigned tb = db.yes, eb = db.no;
                                if (tb == eqBlk || eb == eqBlk || cfg.preds[tb].size() != 1 || cfg.preds[eb].size() != 1) continue;
                                if (fn.blocks[tb].exit != Exit::Jump || fn.blocks[eb].exit != Exit::Jump) continue;
                                if (fn.blocks[tb].yes != fn.blocks[eb].yes) continue;
                                const unsigned latch = fn.blocks[tb].yes;
                                if (latch == lp.header || !lp.body.contains(latch) || fn.blocks[latch].exit != Exit::Jump ||
                                    fn.blocks[latch].yes != lp.header)
                                    continue;
                                // Verify dispBlk dominates latch & eqBlk is initial body
                                if (eqB.exit == Exit::Branch && eqB.value && lp.body.contains(eqB.no) &&
                                    eqB.yes == dispBlk && eqB.no != dispBlk) {
                                    auto findOnlyInc = [&](unsigned b) -> std::optional<unsigned> {
                                        std::optional<unsigned> s;
                                        for (const auto& op : fn.blocks[b].ops) {
                                            if (op.op == Op::Store && op.slot != fh.iterSlot) {
                                                // Match s = s + 1
                                                auto d = defOp.find(op.inputs[0].id);
                                                if (d != defOp.end() && d->second->op == Op::Binary && d->second->data == "+") {
                                                    auto ls = loadSlot.find(d->second->inputs[0].id);
                                                    auto c1 = parseConstInt(defOp.count(d->second->inputs[1].id) ? defOp.at(d->second->inputs[1].id)->data : "");
                                                    if (ls != loadSlot.end() && ls->second == op.slot && c1 == 1) {
                                                        if (!s) s = op.slot;
                                                        else return {};
                                                    }
                                                }
                                            }
                                        }
                                        return s;
                                    };
                                    auto lS = findOnlyInc(tb);
                                    auto rS = findOnlyInc(eb);
                                    if (lS && rS && *lS != *rS) {
                                        // Check eqBlk tests (*rS == bndSlot), and eqB.no tests (*lS < bndSlot)
                                        auto cEq = defOp.find(eqB.value->id);
                                        const unsigned ltBlk = eqB.no;
                                        const auto& ltB = fn.blocks[ltBlk];
                                        if (cEq != defOp.end() && cEq->second->op == Op::Binary && cEq->second->data == "==" &&
                                            ltB.exit == Exit::Branch && ltB.value) {
                                            auto eqL = loadSlot.find(cEq->second->inputs[0].id);
                                            auto eqR = loadSlot.find(cEq->second->inputs[1].id);
                                            bool eqMatch = (eqL != loadSlot.end() && eqR != loadSlot.end() &&
                                                ((eqL->second == *rS && eqR->second == bndSlot) ||
                                                 (eqR->second == *rS && eqL->second == bndSlot)));
                                            auto cLt = defOp.find(ltB.value->id);
                                            bool ltMatch = false;
                                            if (cLt != defOp.end() && cLt->second->op == Op::Binary && cLt->second->data == "<") {
                                                auto ltL = loadSlot.find(cLt->second->inputs[0].id);
                                                auto ltR = loadSlot.find(cLt->second->inputs[1].id);
                                                ltMatch = (ltL != loadSlot.end() && ltR != loadSlot.end() &&
                                                    ltL->second == *lS && ltR->second == bndSlot &&
                                                    lp.body.contains(ltB.no));
                                            }
                                            if (eqMatch && ltMatch) {
                                                // Verify tb and eb are the ONLY writes to *lS and *rS inside lp.body
                                                int lW = 0, rW = 0;
                                                for (unsigned ob : lp.body) {
                                                    for (const auto& op : fn.blocks[ob].ops) {
                                                        if (op.op == Op::Store && op.slot == *lS) ++lW;
                                                        if (op.op == Op::Store && op.slot == *rS) ++rW;
                                                    }
                                                }
                                                if (lW == 1 && rW == 1) {
                                                    twoWayPartitionLoops[li] = {fh.iterSlot, *lS, *rS, bndSlot, eqBlk, ltBlk, tb, eb};
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // Detect unique 0 -> >=1 set counter while loops (e.g. breadthFirst: while head < tail { ... if seen[w] == 0 { seen[w] = ...; q[tail] = w; tail += 1 } })
        for (std::size_t li = 0; li < nLoops; ++li) {
            const auto& lp = cfg.loops[li];
            if (forHeaders.find(lp.header) != forHeaders.end() || lp.preheaders.size() != 1) continue;
            const auto& whb = fn.blocks[lp.header];
            if (whb.exit != Exit::Branch || !whb.value || !lp.body.contains(whb.yes) || lp.body.contains(whb.no)) continue;
            auto hCmp = defOp.find(whb.value->id);
            if (hCmp == defOp.end() || hCmp->second->op != Op::Binary || hCmp->second->data != "<") continue;
            auto hLs = loadSlot.find(hCmp->second->inputs[0].id);
            auto cLs = loadSlot.find(hCmp->second->inputs[1].id);
            if (hLs == loadSlot.end() || cLs == loadSlot.end() || hLs->second == cLs->second) continue;
            const unsigned headS = hLs->second, cntS = cLs->second;
            // Verify cntS initialized to small const == count of pre-loop IndexSet on seenArr, and only incremented by +1 in one guardBlock
            std::vector<unsigned> cntWriteBlks;
            bool okCntInc = true;
            for (unsigned b : lp.body) {
                for (const auto& op : fn.blocks[b].ops) {
                    if (op.op == Op::Store && op.slot == cntS) {
                        cntWriteBlks.push_back(b);
                        auto d = defOp.find(op.inputs[0].id);
                        auto rc = (d != defOp.end() && d->second->op == Op::Binary && d->second->data == "+")
                            ? parseConstInt(defOp.count(d->second->inputs[1].id) ? defOp.at(d->second->inputs[1].id)->data : "")
                            : std::nullopt;
                        auto ls0 = (d != defOp.end() && d->second->op == Op::Binary) ? loadSlot.find(d->second->inputs[0].id) : loadSlot.end();
                        if (ls0 == loadSlot.end() || ls0->second != cntS || rc != 1) okCntInc = false;
                    }
                }
            }
            if (!okCntInc || cntWriteBlks.size() != 1) continue;
            const unsigned guardB = cntWriteBlks[0];
            if (cfg.preds[guardB].size() != 1) continue;
            const unsigned pChk = cfg.preds[guardB][0];
            const auto& pcb = fn.blocks[pChk];
            if (pcb.exit != Exit::Branch || !pcb.value || pcb.yes != guardB || pcb.no == guardB) continue;
            auto c0 = defOp.find(pcb.value->id);
            if (c0 == defOp.end() || c0->second->op != Op::Binary || c0->second->data != "==") continue;
            std::optional<unsigned> seenS, idxSlot;
            for (int side = 0; side < 2; ++side) {
                auto zc = parseConstInt(defOp.count(c0->second->inputs[1 - side].id) ? defOp.at(c0->second->inputs[1 - side].id)->data : "");
                auto iDef = defOp.find(c0->second->inputs[side].id);
                if (zc == 0 && iDef != defOp.end() && iDef->second->op == Op::Index) {
                    auto as = loadSlot.find(iDef->second->inputs[0].id);
                    auto is = loadSlot.find(iDef->second->inputs[1].id);
                    if (as != loadSlot.end() && is != loadSlot.end()) {
                        seenS = as->second;
                        idxSlot = is->second;
                    }
                }
            }
            if (!seenS || !idxSlot) continue;
            // Verify EVERY write to seenS across the whole function is repeat(0, ...), pre-loop writes, or guardB writes storing >= 1 to seenS[idxSlot]
            std::optional<Value> repeatCntVal;
            int repeatWrites = 0, preLoopWrites = 0;
            bool okSeen = true;
            for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
                for (const auto& op : fn.blocks[bi].ops) {
                    if (op.op == Op::Store && op.slot == *seenS) {
                        auto d = defOp.find(op.inputs[0].id);
                        if (d != defOp.end() && (d->second->op == Op::RepeatRegion || d->second->op == Op::RepeatHeap) &&
                            parseConstInt(defOp.count(d->second->inputs[0].id) ? defOp.at(d->second->inputs[0].id)->data : "") == 0) {
                            ++repeatWrites;
                            repeatCntVal = d->second->inputs[1];
                        } else okSeen = false;
                    } else if (op.op == Op::IndexSet && !op.inputs.empty()) {
                        auto as = loadSlot.find(op.inputs[0].id);
                        if (as != loadSlot.end() && as->second == *seenS) {
                            if (cfg.loopOfBlock[bi] < 0 && !lp.body.contains(bi)) {
                                ++preLoopWrites;
                            } else if (bi == guardB) {
                                auto wsIdx = loadSlot.find(op.inputs[1].id);
                                if (wsIdx == loadSlot.end() || wsIdx->second != *idxSlot) okSeen = false;
                                // Written value must be >= 1 (e.g. seen[v] + 1 where const > 0)
                                auto vd = defOp.find(op.inputs[2].id);
                                if (vd == defOp.end() || vd->second->op != Op::Binary || vd->second->data != "+") okSeen = false;
                                else if (auto rc = parseConstInt(defOp.count(vd->second->inputs[1].id) ? defOp.at(vd->second->inputs[1].id)->data : ""); !rc || *rc <= 0) okSeen = false;
                            } else {
                                okSeen = false;
                            }
                        }
                    }
                }
            }
            if (!okSeen || repeatWrites != 1 || !repeatCntVal || preLoopWrites == 0) continue;
            // Verify cntS initialized to preLoopWrites outside loop (in preheader or entry path) and headS starts at 0 and only increments by +1
            std::optional<std::int64_t> initCntVal;
            for (const auto& op : fn.blocks[lp.preheaders[0]].ops) {
                if (op.op == Op::Store && op.slot == cntS) {
                    initCntVal = parseConstInt(defOp.count(op.inputs[0].id) ? defOp.at(op.inputs[0].id)->data : "");
                }
            }
            if (initCntVal != preLoopWrites) continue;
            // Check optional queue array written only at queueArr[cntS] inside guardB (before cntS += 1) and having same repeat size as seenArr
            unsigned qArr = *seenS;
            bool sawQ = false;
            int cntPos = -1, qPos = -1;
            for (int oi = 0; oi < static_cast<int>(fn.blocks[guardB].ops.size()); ++oi) {
                const auto& op = fn.blocks[guardB].ops[oi];
                if (op.op == Op::Store && op.slot == cntS) cntPos = oi;
                if (op.op == Op::IndexSet && !op.inputs.empty()) {
                    auto as = loadSlot.find(op.inputs[0].id);
                    auto is = loadSlot.find(op.inputs[1].id);
                    if (as != loadSlot.end() && as->second != *seenS && is != loadSlot.end() && is->second == cntS) {
                        qArr = as->second;
                        qPos = oi;
                        sawQ = true;
                    }
                }
            }
            bool sameLenQ = false;
            if (sawQ && qPos < cntPos) {
                for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
                    for (const auto& op : fn.blocks[bi].ops) {
                        if (op.op == Op::Store && op.slot == qArr) {
                            auto d = defOp.find(op.inputs[0].id);
                            if (d != defOp.end() && (d->second->op == Op::RepeatRegion || d->second->op == Op::RepeatHeap)) {
                                auto ls1 = loadSlot.find(d->second->inputs[1].id);
                                auto ls2 = loadSlot.find(repeatCntVal->id);
                                if (ls1 != loadSlot.end() && ls2 != loadSlot.end() && ls1->second == ls2->second)
                                    sameLenQ = true;
                            }
                        }
                    }
                }
            }
            bool headMono = true;
            for (unsigned b : lp.body) {
                for (const auto& op : fn.blocks[b].ops) {
                    if (op.op == Op::Store && op.slot == headS) {
                        auto d = defOp.find(op.inputs[0].id);
                        auto rc = (d != defOp.end() && d->second->op == Op::Binary && d->second->data == "+")
                            ? parseConstInt(defOp.count(d->second->inputs[1].id) ? defOp.at(d->second->inputs[1].id)->data : "")
                            : std::nullopt;
                        auto ls0 = (d != defOp.end() && d->second->op == Op::Binary) ? loadSlot.find(d->second->inputs[0].id) : loadSlot.end();
                        if (ls0 == loadSlot.end() || ls0->second != headS || rc != 1) headMono = false;
                    }
                }
            }
            uniqueZeroSetLoops[li] = UniqueZeroSetLoop{
                *seenS, sameLenQ ? qArr : *seenS, cntS,
                headMono ? std::optional<unsigned>(headS) : std::nullopt,
                guardB, sameLenQ ? guardB : 0, *initCntVal};
        }

        // Also include scalar accumulators updated inside bounded inner while loops (sqrtLoops / divShrinkLoops)
        for (std::size_t li = 0; li < nLoops; ++li) {
            auto fhIt = forHeaders.find(cfg.loops[li].header);
            if (fhIt == forHeaders.end() || !fhIt->second.valid) continue;
            for (unsigned slot : loopWritten[li]) {
                if (slot == fhIt->second.iterSlot || slot >= fn.slots.size() || fn.slots[slot] != Type::Int)
                    continue;
                bool alreadyAdded = false;
                if (auto it = loopAccOps.find(li); it != loopAccOps.end()) {
                    for (const auto& [as, sv, isb] : it->second)
                        if (as == slot) alreadyAdded = true;
                }
                if (alreadyAdded) continue;
                int storeCnt = 0;
                unsigned storeBlk = 0;
                const Operation* storeOp = nullptr;
                for (unsigned b : cfg.loops[li].body) {
                    for (const auto& op : fn.blocks[b].ops) {
                        if (op.op == Op::Store && op.slot == slot) {
                            ++storeCnt; storeBlk = b; storeOp = &op;
                        }
                    }
                }
                if (storeCnt != 1 || cfg.loopOfBlock[storeBlk] == static_cast<int>(li) ||
                    !storeOp || storeOp->inputs.empty())
                    continue;
                bool allBounded = true;
                for (std::size_t sub = 0; sub < nLoops; ++sub) {
                    if (cfg.loops[sub].body.contains(storeBlk) && cfg.loops[li].body.contains(cfg.loops[sub].header)) {
                        const bool isBounded = (forHeaders.count(cfg.loops[sub].header) && forHeaders.at(cfg.loops[sub].header).valid) ||
                                               divShrinkLoops.contains(sub) || sqrtLoops.contains(sub);
                        if (!isBounded) { allBounded = false; break; }
                    }
                }
                if (!allBounded) continue;
                auto dit = defOp.find(storeOp->inputs[0].id);
                if (dit == defOp.end() || dit->second->op != Op::Binary ||
                    (dit->second->data != "+" && dit->second->data != "-"))
                    continue;
                auto ls = loadSlot.find(dit->second->inputs[0].id);
                if (ls == loadSlot.end() || ls->second != slot) continue;
                const Value stepVal = dit->second->inputs[1];
                const bool isSub = (dit->second->data == "-");
                std::unordered_set<unsigned> visVals;
                auto depCheck = [&](auto& self, unsigned vid, int d) -> bool {
                    if (d > 32 || !visVals.insert(vid).second) return false;
                    auto it2 = defOp.find(vid);
                    if (it2 == defOp.end()) return false;
                    const Operation* o = it2->second;
                    if (o->op == Op::Load) return o->slot == slot || (slotWritable[o->slot] && !loopWritten[li].contains(o->slot));
                    if (o->op == Op::Call || o->op == Op::ExternCall || o->op == Op::DynCall || o->op == Op::ClosureCall) return true;
                    if (o->op == Op::Binary && o->data == "%") {
                        auto mls = loadSlot.find(o->inputs[1].id);
                        bool invM = (mls == loadSlot.end()) || !loopWritten[li].contains(mls->second);
                        if (invM && !self(self, o->inputs[1].id, d + 1)) return false;
                    }
                    for (const auto& in : o->inputs)
                        if (self(self, in.id, d + 1)) return true;
                    return false;
                };
                if (!depCheck(depCheck, stepVal.id, 0)) {
                    loopAccOps[li].emplace_back(slot, stepVal, isSub);
                }
            }
        }

        // Detect Heron convergent while loop: while (y < x) { x = y; y = (x + a / x) / 2 }
        for (std::size_t li = 0; li < nLoops; ++li) {
            if (forHeaders.count(cfg.loops[li].header) || cfg.loops[li].preheaders.size() != 1) continue;
            const auto& lp = cfg.loops[li];
            const auto& whb = fn.blocks[lp.header];
            if (lp.body.size() != 2 || whb.exit != Exit::Branch || !whb.value ||
                !lp.body.contains(whb.yes) || lp.body.contains(whb.no))
                continue;
            const unsigned body = whb.yes;
            if (fn.blocks[body].exit != Exit::Jump || fn.blocks[body].yes != lp.header) continue;
            auto cmp = defOp.find(whb.value->id);
            if (cmp == defOp.end() || cmp->second->op != Op::Binary ||
                (cmp->second->data != "<" && cmp->second->data != ">"))
                continue;
            auto yLs = loadSlot.find(cmp->second->inputs[cmp->second->data == "<" ? 0 : 1].id);
            auto xLs = loadSlot.find(cmp->second->inputs[cmp->second->data == "<" ? 1 : 0].id);
            if (yLs == loadSlot.end() || xLs == loadSlot.end() || yLs->second == xLs->second) continue;
            const unsigned xS = xLs->second, yS = yLs->second;
            int xPos = -1, yPos = -1, xCnt = 0, yCnt = 0;
            bool okBody = true;
            for (int oi = 0; oi < static_cast<int>(fn.blocks[body].ops.size()); ++oi) {
                const auto& op = fn.blocks[body].ops[oi];
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall ||
                    op.op == Op::ClosureCall || op.op == Op::IndexSet || op.op == Op::Builtin) {
                    okBody = false; break;
                }
                if (op.op == Op::Store) {
                    if (op.slot == xS) { ++xCnt; xPos = oi; }
                    else if (op.slot == yS) { ++yCnt; yPos = oi; }
                    else { okBody = false; break; }
                }
            }
            if (!okBody || xCnt != 1 || yCnt != 1 || xPos < 0 || yPos < 0 || xPos > yPos) continue;
            auto xRhs = fn.blocks[body].ops[xPos].inputs[0];
            auto xLoad = loadSlot.find(xRhs.id);
            if (xLoad == loadSlot.end() || xLoad->second != yS) continue;
            auto yDef = defOp.find(fn.blocks[body].ops[yPos].inputs[0].id);
            if (yDef == defOp.end() || yDef->second->op != Op::Binary || yDef->second->data != "/") continue;
            if (parseConstInt(defOp.count(yDef->second->inputs[1].id) ? defOp.at(yDef->second->inputs[1].id)->data : "") != 2)
                continue;
            auto addDef = defOp.find(yDef->second->inputs[0].id);
            if (addDef == defOp.end() || addDef->second->op != Op::Binary || addDef->second->data != "+") continue;
            std::optional<Value> aValFound;
            for (int side = 0; side < 2 && !aValFound; ++side) {
                auto lsX = loadSlot.find(addDef->second->inputs[side].id);
                auto divDef = defOp.find(addDef->second->inputs[1 - side].id);
                if (lsX != loadSlot.end() && lsX->second == xS &&
                    divDef != defOp.end() && divDef->second->op == Op::Binary && divDef->second->data == "/") {
                    auto divDen = loadSlot.find(divDef->second->inputs[1].id);
                    if (divDen != loadSlot.end() && divDen->second == xS) {
                        const Value candA = divDef->second->inputs[0];
                        auto als = loadSlot.find(candA.id);
                        if (als == loadSlot.end() || (als->second != xS && als->second != yS && !loopWritten[li].contains(als->second)))
                            aValFound = candA;
                    }
                }
            }
            if (aValFound) {
                heronLoops.emplace(li, HeronLoop{xS, yS, *aValFound});
            }
        }

        // Detect Euclidean modulo-reduction loop: while (b > 0 / b != 0) { r = a % b; a = b; b = r }
        for (std::size_t li = 0; li < nLoops; ++li) {
            if (forHeaders.count(cfg.loops[li].header) || cfg.loops[li].preheaders.size() != 1) continue;
            const auto& lp = cfg.loops[li];
            const auto& whb = fn.blocks[lp.header];
            if (lp.body.size() != 2 || whb.exit != Exit::Branch || !whb.value ||
                !lp.body.contains(whb.yes) || lp.body.contains(whb.no))
                continue;
            const unsigned body = whb.yes;
            if (fn.blocks[body].exit != Exit::Jump || fn.blocks[body].yes != lp.header) continue;
            auto cmp = defOp.find(whb.value->id);
            if (cmp == defOp.end() || cmp->second->op != Op::Binary ||
                (cmp->second->data != ">" && cmp->second->data != "!="))
                continue;
            auto bLs = loadSlot.find(cmp->second->inputs[0].id);
            auto zC = parseConstInt(defOp.count(cmp->second->inputs[1].id) ? defOp.at(cmp->second->inputs[1].id)->data : "");
            if (bLs == loadSlot.end() || zC != 0) continue;
            const unsigned bS = bLs->second;
            std::optional<unsigned> aS, rS;
            int aPos = -1, bPos = -1, rPos = -1;
            bool okEuc = true;
            for (int oi = 0; oi < static_cast<int>(fn.blocks[body].ops.size()); ++oi) {
                const auto& op = fn.blocks[body].ops[oi];
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall ||
                    op.op == Op::ClosureCall || op.op == Op::IndexSet || op.op == Op::Builtin) {
                    okEuc = false; break;
                }
                if (op.op == Op::Store) {
                    if (op.slot == bS) {
                        if (bPos >= 0) { okEuc = false; break; }
                        bPos = oi;
                    } else if (!op.inputs.empty()) {
                        auto d = defOp.find(op.inputs[0].id);
                        if (d != defOp.end() && d->second->op == Op::Binary && d->second->data == "%" && rPos < 0) {
                            rS = op.slot;
                            rPos = oi;
                        } else if (!aS) {
                            aS = op.slot;
                            aPos = oi;
                        } else {
                            okEuc = false; break;
                        }
                    }
                }
            }
            if (!okEuc || !aS || *aS == bS || aPos < 0 || bPos < 0) continue;
            if (loopWritten[li].size() > (rS ? 3U : 2U)) continue;
            auto aLoad = loadSlot.find(fn.blocks[body].ops[aPos].inputs[0].id);
            if (aLoad == loadSlot.end() || aLoad->second != bS) continue;
            Value modVal = fn.blocks[body].ops[bPos].inputs[0];
            if (rS) {
                auto ml = loadSlot.find(modVal.id);
                if (ml == loadSlot.end() || ml->second != *rS || rPos > aPos || rPos > bPos) continue;
                modVal = fn.blocks[body].ops[rPos].inputs[0];
            } else if (bPos < aPos) {
                // Direct b = a % b only safe if mod evaluated before a = b
                continue;
            }
            auto md = defOp.find(modVal.id);
            if (md == defOp.end() || md->second->op != Op::Binary || md->second->data != "%") continue;
            auto ml = loadSlot.find(md->second->inputs[0].id);
            auto mr = loadSlot.find(md->second->inputs[1].id);
            if (ml != loadSlot.end() && ml->second == *aS && mr != loadSlot.end() && mr->second == bS) {
                euclidLoops.emplace(li, EuclidLoop{*aS, bS});
            }
        }

        // Detect bounded-selector slots: slot only written with Load(forIter) whose upper is Load(boundSlot) and lower >= 0
        auto exprKnownNonNegStatic = [&](auto& self, Value v, int d) -> bool {
            if (d > 10) return false;
            if (auto opIt = defOp.find(v.id); opIt != defOp.end()) {
                const Operation* o = opIt->second;
                if (o->op == Op::Int) {
                    auto c = parseConstInt(o->data);
                    return c && *c >= 0;
                }
                if (o->op == Op::Load) {
                    for (const auto& [fhHdr, fhInfo] : forHeaders) {
                        (void)fhHdr;
                        if (fhInfo.valid && fhInfo.iterSlot == o->slot) {
                            return self(self, fhInfo.lower, d + 1);
                        }
                    }
                    return false;
                }
                if (o->op == Op::Binary && o->data == "+") {
                    return self(self, o->inputs[0], d + 1) && self(self, o->inputs[1], d + 1);
                }
            }
            return false;
        };
        for (std::size_t li = 0; li < nLoops; ++li) {
            auto fhIt = forHeaders.find(cfg.loops[li].header);
            for (unsigned s : loopWritten[li]) {
                if (fhIt != forHeaders.end() && fhIt->second.valid && s == fhIt->second.iterSlot) continue;
                if (s >= fn.slots.size() || fn.slots[s] != Type::Int) continue;
                std::optional<unsigned> commonBnd;
                bool okSel = true;
                int cntSel = 0;
                for (unsigned b : cfg.loops[li].body) {
                    for (const auto& op : fn.blocks[b].ops) {
                        if (op.op != Op::Store || op.slot != s || op.inputs.empty()) continue;
                        ++cntSel;
                        auto ls = loadSlot.find(op.inputs[0].id);
                        if (ls == loadSlot.end()) { okSel = false; break; }
                        std::optional<unsigned> bndForSrc;
                        for (std::size_t fl = 0; fl < nLoops; ++fl) {
                            auto fit = forHeaders.find(cfg.loops[fl].header);
                            if (fit == forHeaders.end() || !fit->second.valid || fit->second.iterSlot != ls->second) continue;
                            if (!cfg.loops[fl].body.contains(b)) continue;
                            auto uls = loadSlot.find(fit->second.upper.id);
                            if (uls == loadSlot.end() || loopWritten[li].contains(uls->second) ||
                                loopWritten[fl].contains(uls->second))
                                continue;
                            if (!exprKnownNonNegStatic(exprKnownNonNegStatic, fit->second.lower, 0)) continue;
                            bndForSrc = uls->second;
                            break;
                        }
                        if (!bndForSrc) { okSel = false; break; }
                        if (!commonBnd) commonBnd = bndForSrc;
                        else if (*commonBnd != *bndForSrc) { okSel = false; break; }
                    }
                    if (!okSel) break;
                }
                if (okSel && cntSel >= 1 && commonBnd) {
                    boundedSelectorLoops[li].push_back({s, *commonBnd});
                }
            }
        }
    }

    // Track array lengths and global element range invariants
    void analyzeArrays() {
        const std::size_t nSlots = fn.slots.size();
        std::vector<int> arrayOriginCount(nSlots, 0);
        std::vector<bool> arrayEscapesOrAliased(nSlots, false);
        std::vector<const Operation*> repeatSite(nSlots, nullptr);
        std::vector<std::optional<unsigned>> movedSource(nSlots);

        for (const auto& blk : fn.blocks) {
            for (const auto& op : blk.ops) {
                if (op.op == Op::Store && !op.inputs.empty() && isAnyArrayType(programPtr, fn.slots[op.slot])) {
                    auto def = defOp.find(op.inputs[0].id);
                    if (def != defOp.end() && (def->second->op == Op::RepeatRegion ||
                        def->second->op == Op::RepeatHeap || def->second->op == Op::Clone)) {
                        ++arrayOriginCount[op.slot];
                        if (def->second->op == Op::RepeatRegion || def->second->op == Op::RepeatHeap)
                            repeatSite[op.slot] = def->second;
                        else if (def->second->op == Op::Clone && !def->second->inputs.empty()) {
                            if (auto ls = loadSlot.find(def->second->inputs[0].id); ls != loadSlot.end())
                                movedSource[op.slot] = ls->second;
                        }
                    } else if (def != defOp.end() && def->second->op == Op::Load) {
                        ++arrayOriginCount[op.slot];
                        movedSource[op.slot] = def->second->slot;
                    } else {
                        arrayEscapesOrAliased[op.slot] = true;
                    }
                }
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Builtin || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                    for (const auto& in : op.inputs) {
                        if (isAnyArrayType(programPtr, in.type)) {
                            if (auto ls = loadSlot.find(in.id); ls != loadSlot.end())
                                arrayEscapesOrAliased[ls->second] = true;
                        }
                    }
                    for (unsigned ms : op.mutArgSlots) {
                        if (ms < nSlots) {
                            arrayEscapesOrAliased[ms] = true;
                            ++arrayOriginCount[ms];
                            movedSource[ms].reset();
                        }
                    }
                }
                if (op.op == Op::ClearOwner || op.op == Op::ClearField ||
                    (op.op == Op::PlaceSet && op.slot < nSlots))
                    arrayEscapesOrAliased[op.slot] = true;
                if (op.op == Op::ClearIndex && !op.inputs.empty()) {
                    if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end())
                        arrayEscapesOrAliased[ls->second] = true;
                }
            }
            if (blk.exit == Exit::Return && blk.value && isAnyArrayType(programPtr, blk.value->type)) {
                if (auto ls = loadSlot.find(blk.value->id); ls != loadSlot.end())
                    arrayEscapesOrAliased[ls->second] = true;
            }
        }

        // Propagate origin Repeat* across ClearOwner moves for static array-shape recognition (length never mutates)
        for (int pass = 0; pass < 3; ++pass) {
            for (std::size_t s = 0; s < nSlots; ++s) {
                if (!repeatSite[s] && arrayOriginCount[s] == 1 && movedSource[s]) {
                    const unsigned src = *movedSource[s];
                    if (src < nSlots && repeatSite[src]) repeatSite[s] = repeatSite[src];
                }
            }
        }

        Env staticEnv;
        staticEnv.slotEpoch.assign(nSlots, 0);
        for (std::size_t s = 0; s < nSlots; ++s) {
            // Even when source had ClearOwner, size shape of single-origin slot s is immutable and safe for bounds proof.
            if (isAnyArrayType(programPtr, fn.slots[s]) && arrayOriginCount[s] == 1 && repeatSite[s] &&
                (!arrayEscapesOrAliased[s] || !slotWritable[s] || movedSource[s])) {
                if (auto sz = shallowSym(repeatSite[s]->inputs[1].id, staticEnv)) {
                    if (sz->hasProduct && sz->prodCoeff == 1 && sz->constant == 0 && sz->linear.empty()) {
                        if (sz->prodSlotA == sz->prodSlotB && sz->prodOffA == sz->prodOffB) {
                            squareArraySizes.emplace(static_cast<unsigned>(s),
                                                     std::pair{sz->prodSlotA, sz->prodOffA});
                        }
                        if (!slotWritable[sz->prodSlotA] && !slotWritable[sz->prodSlotB]) {
                            rectArraySizes.emplace(static_cast<unsigned>(s),
                                                   RectShape{sz->prodSlotA, sz->prodSlotB, sz->prodOffA, sz->prodOffB});
                        }
                    } else if (sz->isPureLinear()) {
                        linearArraySizes.emplace(static_cast<unsigned>(s), *sz);
                    }
                }
                if (auto fsz = buildSym(repeatSite[s]->inputs[1].id, staticEnv)) {
                    bool allImmutable = true;
                    for (const auto& [ls, lc] : fsz->linear)
                        if (slotWritable[ls]) allImmutable = false;
                    if (fsz->hasProduct && (slotWritable[fsz->prodSlotA] || slotWritable[fsz->prodSlotB]))
                        allImmutable = false;
                    if (allImmutable) fullArraySizes.emplace(static_cast<unsigned>(s), *fsz);
                }
            }
        }
    }

    // Refine Env on a relational branch (e.g. a < b, a >= 1, etc.)
    void refineFromCondition(Env& env, Value rootCond, bool rootTruth, bool fromContract = false) const {
        std::vector<std::pair<Value, bool>> stack;
        stack.push_back({rootCond, rootTruth});
        std::unordered_set<std::uint64_t> seen;
        while (!stack.empty()) {
            auto [cond, truth] = stack.back();
            stack.pop_back();
            const std::uint64_t key = (static_cast<std::uint64_t>(cond.id) << 1) | (truth ? 1ULL : 0ULL);
            if (!seen.insert(key).second) continue;
            auto dit = defOp.find(cond.id);
            if (dit == defOp.end()) continue;
            const Operation* op = dit->second;
            if (op->op == Op::Load) {
                if (truth) {
                    if (auto it = shortCircuitAnd.find(op->slot); it != shortCircuitAnd.end()) {
                        stack.push_back({it->second.second, true});
                        stack.push_back({it->second.first, true});
                    }
                } else {
                    if (auto it = shortCircuitOr.find(op->slot); it != shortCircuitOr.end()) {
                        stack.push_back({it->second.second, false});
                        stack.push_back({it->second.first, false});
                    }
                }
                continue;
            }
            if (op->op == Op::Not) {
                stack.push_back({op->inputs[0], !truth});
                continue;
            }
            if (op->op != Op::Binary || op->inputs.size() != 2) continue;
            refineBinaryCondition(env, op, truth, fromContract);
        }
    }

    void refineBinaryCondition(Env& env, const Operation* op, bool truth, bool fromContract) const {
        auto markContractNode = [&](bool isArrLen, unsigned slot) {
            if (!fromContract) return;
            if (isArrLen) env.contractArrLens.insert(slot);
            else env.contractSlots.insert(slot);
        };
        std::string cmp = op->data;
        if (!truth) {
            if (cmp == "<") cmp = ">=";
            else if (cmp == "<=") cmp = ">";
            else if (cmp == ">") cmp = "<=";
            else if (cmp == ">=") cmp = "<";
            else if (cmp == "==") cmp = "!=";
            else if (cmp == "!=") cmp = "==";
            else return;
        }
        Value L = op->inputs[0], R = op->inputs[1];
        if (cmp == ">" || cmp == ">=") {
            std::swap(L, R);
            cmp = (cmp == ">") ? "<" : "<=";
        }
        // Now cmp is "<", "<=", "==", or "!="
        const auto lr = rangeOf(L, env);
        const auto rr = rangeOf(R, env);

        if (cmp == "!=") {
            std::int64_t rc{};
            std::int64_t lc{};
            if (rr && isConstantRange(*rr, rc)) {
                if (rc == 0) {
                    if (auto ls = loadSlot.find(L.id); ls != loadSlot.end() &&
                        (!slotWritable[ls->second] || isCurrentLoad(L.id, ls->second, env))) {
                        env.nonZeroSlots.insert(ls->second);
                        markContractNode(false, ls->second);
                    }
                }
                if (lr && lr->lower == rc && lr->lower < lr->upper) {
                    env.valRanges[L.id] = IntRange{rc + 1, lr->upper};
                    if (auto ls = loadSlot.find(L.id); ls != loadSlot.end() &&
                        (!slotWritable[ls->second] || isCurrentLoad(L.id, ls->second, env))) {
                        env.diffs.addLower({false, ls->second}, rc + 1);
                        markContractNode(false, ls->second);
                    }
                } else if (lr && lr->upper == rc && lr->lower < lr->upper) {
                    env.valRanges[L.id] = IntRange{lr->lower, rc - 1};
                    if (auto ls = loadSlot.find(L.id); ls != loadSlot.end() &&
                        (!slotWritable[ls->second] || isCurrentLoad(L.id, ls->second, env))) {
                        env.diffs.addUpper({false, ls->second}, rc - 1);
                        markContractNode(false, ls->second);
                    }
                }
            } else if (lr && isConstantRange(*lr, lc) && lc == 0) {
                if (auto rs = loadSlot.find(R.id); rs != loadSlot.end() &&
                    (!slotWritable[rs->second] || isCurrentLoad(R.id, rs->second, env))) {
                    env.nonZeroSlots.insert(rs->second);
                    markContractNode(false, rs->second);
                }
            } else {
                auto lls = loadSlot.find(L.id);
                auto rls = loadSlot.find(R.id);
                if (lls != loadSlot.end() && rls != loadSlot.end() && lls->second != rls->second &&
                    (!slotWritable[lls->second] || isCurrentLoad(L.id, lls->second, env)) &&
                    (!slotWritable[rls->second] || isCurrentLoad(R.id, rls->second, env))) {
                    if (auto d = env.diffs.maxDiff({false, lls->second}, {false, rls->second}); d && *d <= 0)
                        env.diffs.addSlotDiff({false, lls->second}, {false, rls->second}, -1);
                    if (auto d = env.diffs.maxDiff({false, rls->second}, {false, lls->second}); d && *d <= 0)
                        env.diffs.addSlotDiff({false, rls->second}, {false, lls->second}, -1);
                }
            }
            return;
        }

        const auto al = extractLinearAtom(L.id, env);
        const auto ar = extractLinearAtom(R.id, env);
        auto recordRect = [&](const Value& lenSide, const Value& prodSide) {
            auto a = extractLinearAtom(lenSide.id, env);
            if (!a || !a->isArrLen || a->offset != 0) return;
            if (auto ps = shallowSym(prodSide.id, env);
                ps && ps->hasProduct && ps->prodCoeff == 1 && ps->constant == 0 && ps->linear.empty() &&
                !slotWritable[ps->prodSlotA] && !slotWritable[ps->prodSlotB]) {
                env.rectArrShapes[a->slot] = RectShape{ps->prodSlotA, ps->prodSlotB, ps->prodOffA, ps->prodOffB};
                markContractNode(true, a->slot);
            }
        };

        if (cmp == "<" || cmp == "<=") {
            const std::int64_t off = (cmp == "<") ? -1 : 0;
            // Refine value ranges
            if (rr) {
                const std::int64_t hiCap = (off == -1 && rr->upper > kMinInt) ? rr->upper - 1 : rr->upper;
                auto newL = intersectRange(lr, IntRange{kMinInt, hiCap});
                if (newL) env.valRanges[L.id] = *newL;
            }
            if (lr) {
                const std::int64_t loCap = (off == -1 && lr->lower < kMaxInt) ? lr->lower + 1 : lr->lower;
                auto newR = intersectRange(rr, IntRange{loCap, kMaxInt});
                if (newR) env.valRanges[R.id] = *newR;
            }
            if (al && ar && !(al->isArrLen == ar->isArrLen && al->slot == ar->slot)) {
                if (auto d = checkedSub64(ar->offset, al->offset)) {
                    if (auto bound = checkedAdd64(*d, off)) {
                        env.diffs.addSlotDiff({al->isArrLen, al->slot}, {ar->isArrLen, ar->slot}, *bound);
                        markContractNode(al->isArrLen, al->slot);
                        markContractNode(ar->isArrLen, ar->slot);
                    }
                }
            }
            if (al && rr && rr->upper < kMaxInt) {
                if (auto b1 = checkedAdd64(rr->upper, off)) {
                    if (auto bound = checkedSub64(*b1, al->offset)) {
                        env.diffs.addUpper({al->isArrLen, al->slot}, *bound);
                        markContractNode(al->isArrLen, al->slot);
                    }
                }
            }
            if (ar && lr && lr->lower > kMinInt) {
                if (auto b1 = checkedSub64(lr->lower, off)) {
                    if (auto bound = checkedSub64(*b1, ar->offset)) {
                        env.diffs.addLower({ar->isArrLen, ar->slot}, *bound);
                        markContractNode(ar->isArrLen, ar->slot);
                    }
                }
            }
            // If 0 <= lhs < num / K (where K >= 1 constant, num >= 0), then num / K >= 1 => num >= K
            if (cmp == "<" && lr && lr->lower >= 0) {
                Value rhsV = R;
                if (auto dR0 = defOp.find(rhsV.id); dR0 != defOp.end() && dR0->second->op == Op::Load &&
                    (!slotWritable[dR0->second->slot] || isCurrentLoad(rhsV.id, dR0->second->slot, env))) {
                    if (auto svIt = env.slotVal.find(dR0->second->slot); svIt != env.slotVal.end()) rhsV = svIt->second;
                }
                if (auto rd = defOp.find(rhsV.id); rd != defOp.end() && rd->second->op == Op::Binary && rd->second->data == "/") {
                    if (auto kc = parseConstInt(defOp.count(rd->second->inputs[1].id) ? defOp.at(rd->second->inputs[1].id)->data : "");
                        kc && *kc >= 1) {
                        if (auto nls = loadSlot.find(rd->second->inputs[0].id); nls != loadSlot.end() &&
                            (!slotWritable[nls->second] || isCurrentLoad(rd->second->inputs[0].id, nls->second, env))) {
                            if (auto nr = env.diffs.rangeOf({false, nls->second}); nr && nr->lower >= 0) {
                                env.diffs.addLower({false, nls->second}, *kc);
                            }
                        }
                    }
                }
            }
            recordRect(R, L);
            // Refine difference graph on linear expressions!
            auto sl = shallowSym(L.id, env);
            auto sr = shallowSym(R.id, env);
            if (sl && sr && sl->isPureLinear() && sr->isPureLinear()) {
                // sl - sr <= off
                if (auto diff = addSym(*sl, *sr, -1)) {
                    // Check for single slot vs const: slot - c <= off => slot <= off + c
                    if (diff->linear.size() == 1) {
                        auto [slot, coeff] = *diff->linear.begin();
                        if (coeff == 1) {
                            if (auto bound = checkedSub64(off, diff->constant)) {
                                env.diffs.addUpper({false, slot}, *bound);
                                markContractNode(false, slot);
                            }
                        } else if (coeff == -1) {
                            if (auto bound = checkedSub64(diff->constant, off)) {
                                env.diffs.addLower({false, slot}, *bound);
                                markContractNode(false, slot);
                            }
                        }
                    } else if (diff->linear.size() == 2) {
                        auto it = diff->linear.begin();
                        auto [s1, c1] = *it++;
                        auto [s2, c2] = *it;
                        if (c1 == 1 && c2 == -1) {
                            if (auto bound = checkedSub64(off, diff->constant)) {
                                env.diffs.addSlotDiff({false, s1}, {false, s2}, *bound);
                                markContractNode(false, s1);
                                markContractNode(false, s2);
                            }
                        } else if (c1 == -1 && c2 == 1) {
                            if (auto bound = checkedSub64(off, diff->constant)) {
                                env.diffs.addSlotDiff({false, s2}, {false, s1}, *bound);
                                markContractNode(false, s1);
                                markContractNode(false, s2);
                            }
                        }
                    }
                }
            }
        } else if (cmp == "==") {
            recordRect(L, R);
            recordRect(R, L);
            auto merged = intersectRange(lr, rr);
            if (merged) {
                env.valRanges[L.id] = *merged;
                env.valRanges[R.id] = *merged;
            }
            if (al && ar && !(al->isArrLen == ar->isArrLen && al->slot == ar->slot)) {
                if (auto d1 = checkedSub64(ar->offset, al->offset)) {
                    if (auto d2 = checkedSub64(al->offset, ar->offset)) {
                        env.diffs.addSlotDiff({al->isArrLen, al->slot}, {ar->isArrLen, ar->slot}, *d1);
                        env.diffs.addSlotDiff({ar->isArrLen, ar->slot}, {al->isArrLen, al->slot}, *d2);
                        markContractNode(al->isArrLen, al->slot);
                        markContractNode(ar->isArrLen, ar->slot);
                    }
                }
            }
            if (al && merged) {
                if (merged->lower > kMinInt) {
                    if (auto lo = checkedSub64(merged->lower, al->offset)) {
                        env.diffs.addLower({al->isArrLen, al->slot}, *lo);
                        markContractNode(al->isArrLen, al->slot);
                    }
                }
                if (merged->upper < kMaxInt) {
                    if (auto hi = checkedSub64(merged->upper, al->offset)) {
                        env.diffs.addUpper({al->isArrLen, al->slot}, *hi);
                        markContractNode(al->isArrLen, al->slot);
                    }
                }
            }
            if (ar && merged) {
                if (merged->lower > kMinInt) {
                    if (auto lo = checkedSub64(merged->lower, ar->offset)) {
                        env.diffs.addLower({ar->isArrLen, ar->slot}, *lo);
                        markContractNode(ar->isArrLen, ar->slot);
                    }
                }
                if (merged->upper < kMaxInt) {
                    if (auto hi = checkedSub64(merged->upper, ar->offset)) {
                        env.diffs.addUpper({ar->isArrLen, ar->slot}, *hi);
                        markContractNode(ar->isArrLen, ar->slot);
                    }
                }
            }
            auto sl = shallowSym(L.id, env);
            auto sr = shallowSym(R.id, env);
            if (sl && sr && sl->isPureLinear() && sr->isPureLinear()) {
                if (auto diff = addSym(*sl, *sr, -1)) {
                    if (diff->linear.size() == 1) {
                        auto [slot, coeff] = *diff->linear.begin();
                        if (coeff == 1 && diff->constant != kMinInt) {
                            env.diffs.addUpper({false, slot}, -diff->constant);
                            env.diffs.addLower({false, slot}, -diff->constant);
                            markContractNode(false, slot);
                        } else if (coeff == -1) {
                            env.diffs.addUpper({false, slot}, diff->constant);
                            env.diffs.addLower({false, slot}, diff->constant);
                            markContractNode(false, slot);
                        }
                    } else if (diff->linear.size() == 2) {
                        auto it = diff->linear.begin();
                        auto [s1, c1] = *it++;
                        auto [s2, c2] = *it;
                        if (c1 == 1 && c2 == -1 && diff->constant != kMinInt) {
                            env.diffs.addSlotDiff({false, s1}, {false, s2}, -diff->constant);
                            env.diffs.addSlotDiff({false, s2}, {false, s1}, diff->constant);
                            markContractNode(false, s1);
                            markContractNode(false, s2);
                        } else if (c1 == -1 && c2 == 1 && diff->constant != kMinInt) {
                            env.diffs.addSlotDiff({false, s2}, {false, s1}, -diff->constant);
                            env.diffs.addSlotDiff({false, s1}, {false, s2}, diff->constant);
                            markContractNode(false, s1);
                            markContractNode(false, s2);
                        }
                    }
                }
            }
        }
    }

    bool proveConditionTrue(Value cond, const Env& env, int depth = 0) const {
        if (depth > 16) return false;
        auto dit = defOp.find(cond.id);
        if (dit == defOp.end()) return false;
        const Operation* op = dit->second;
        if (op->op == Op::Bool) return op->data == "true";
        if (op->op == Op::Load) {
            if (auto it = shortCircuitAnd.find(op->slot); it != shortCircuitAnd.end()) {
                return proveConditionTrue(it->second.first, env, depth + 1) &&
                       proveConditionTrue(it->second.second, env, depth + 1);
            }
            return false;
        }
        auto proveRel = [&](const Operation* cmpOp, bool negate) -> bool {
            if (cmpOp->op != Op::Binary || cmpOp->inputs.size() != 2 ||
                cmpOp->inputs[0].type != Type::Int || cmpOp->inputs[1].type != Type::Int)
                return false;
            std::string cmp = cmpOp->data;
            if (negate) {
                if (cmp == "<") cmp = ">=";
                else if (cmp == "<=") cmp = ">";
                else if (cmp == ">") cmp = "<=";
                else if (cmp == ">=") cmp = "<";
                else if (cmp == "==") cmp = "!=";
                else if (cmp == "!=") cmp = "==";
                else return false;
            }
            Value L = cmpOp->inputs[0], R = cmpOp->inputs[1];
            if (cmp == ">" || cmp == ">=") {
                std::swap(L, R);
                cmp = (cmp == ">") ? "<" : "<=";
            }
            const auto lr = rangeOf(L, env);
            const auto rr = rangeOf(R, env);
            if (lr && rr) {
                if (cmp == "<" && lr->upper < rr->lower) return true;
                if (cmp == "<=" && lr->upper <= rr->lower) return true;
                if (cmp == "==" && lr->lower == lr->upper && rr->lower == rr->upper && lr->lower == rr->lower)
                    return true;
                if (cmp == "!=" && (lr->upper < rr->lower || rr->upper < lr->lower)) return true;
            }
            const auto al = extractLinearAtom(L.id, env);
            const auto ar = extractLinearAtom(R.id, env);
            auto proveAtomLe = [&](const LinearAtom& a, const LinearAtom& b, std::int64_t off) -> bool {
                if (a.isArrLen == b.isArrLen && a.slot == b.slot) {
                    auto diffOff = checkedSub64(a.offset, b.offset);
                    return diffOff && *diffOff <= off;
                }
                if (auto d = env.diffs.maxDiff({a.isArrLen, a.slot}, {b.isArrLen, b.slot})) {
                    if (auto rhs = checkedSub64(b.offset, a.offset)) {
                        if (auto limit = checkedAdd64(*rhs, off)) return *d <= *limit;
                    }
                }
                return false;
            };
            if (al && ar) {
                if (cmp == "<") return proveAtomLe(*al, *ar, -1);
                if (cmp == "<=") return proveAtomLe(*al, *ar, 0);
                if (cmp == "==") return proveAtomLe(*al, *ar, 0) && proveAtomLe(*ar, *al, 0);
                if (cmp == "!=") return proveAtomLe(*al, *ar, -1) || proveAtomLe(*ar, *al, -1);
            }
            return false;
        };
        if (op->op == Op::Not && !op->inputs.empty()) {
            if (auto inner = defOp.find(op->inputs[0].id); inner != defOp.end())
                return proveRel(inner->second, true);
            return false;
        }
        return proveRel(op, false);
    }

    bool usesContractFact(const Operation& op, const Env& env) const {
        if (env.contractSlots.empty() && env.contractArrLens.empty()) return false;
        auto checkVal = [&](auto& self, unsigned vid, int depth) -> bool {
            if (depth > 8) return false;
            if (auto ls = loadSlot.find(vid); ls != loadSlot.end()) {
                if (env.contractSlots.contains(ls->second) || env.contractArrLens.contains(ls->second))
                    return true;
                if (!slotWritable[ls->second] && ls->second >= fn.source.params.size() &&
                    singleSlotVal[ls->second] && singleSlotVal[ls->second]->id != vid) {
                    if (self(self, singleSlotVal[ls->second]->id, depth + 1)) return true;
                }
            }
            if (auto it = defOp.find(vid); it != defOp.end()) {
                for (const auto& in : it->second->inputs) {
                    if (self(self, in.id, depth + 1)) return true;
                }
            }
            return false;
        };
        for (const auto& in : op.inputs) {
            if (checkVal(checkVal, in.id, 0)) return true;
        }
        return false;
    }

    // Prove 0 <= expr < len(arrSlot) via interval, Difference Logic, 2D affine product, or modulo!
    bool proveInBounds(unsigned curBlock, unsigned arrSlot, const Value& idxVal, const Env& env,
                       ProofKind* outKind = nullptr) {
        auto finish = [&](ProofKind k) {
            if (outKind) *outKind = k;
            return true;
        };
        if (cfg.loopOfBlock[curBlock] >= 0) {
            const std::size_t curLi = static_cast<std::size_t>(cfg.loopOfBlock[curBlock]);
            if (auto sit = sentinelLoops.find(curLi); sit != sentinelLoops.end() &&
                sit->second.arrSlot == arrSlot && curBlock == cfg.loops[curLi].header) {
                if (auto ls = loadSlot.find(idxVal.id); ls != loadSlot.end() && ls->second == sit->second.jSlot)
                    return finish(ProofKind::SentinelLoop);
            }
        }
        auto isEnclosingForHeader = [&](unsigned hb) -> bool {
            auto lit = cfg.headerToLoop.find(hb);
            return lit != cfg.headerToLoop.end() && cfg.loops[lit->second].body.contains(curBlock);
        };
        const auto idxRange = rangeOf(idxVal, env);
        const auto lenRange = env.diffs.rangeOf({true, arrSlot});
        // 1. Concrete interval proof
        if (idxRange && lenRange && idxRange->lower >= 0 && idxRange->upper < lenRange->lower)
            return finish(env.contractArrLens.contains(arrSlot) ? ProofKind::ContractRequires : ProofKind::ConstRange);

        // 1b. Direct array-element index proof: idx = arr2[...] where 0 <= arr2[k] < N <= len(arr)
        if (auto eltIt = env.idxElemLtSlot.find(idxVal.id); eltIt != env.idxElemLtSlot.end()) {
            const bool nn = idxRange && idxRange->lower >= 0;
            if (nn) {
                if (auto d = env.diffs.maxDiff({false, eltIt->second}, {true, arrSlot}); d && *d <= 0)
                    return finish(ProofKind::DiffGraph);
            }
        }

        // 2. Symbolic linear difference proof: idx >= 0 and idx <= len(arr) - 1
        if (auto sym = shallowSym(idxVal.id, env); sym && sym->isPureLinear()) {
            bool nonNeg = idxRange && idxRange->lower >= 0;
            bool underLen = false;
            if (sym->linear.empty()) {
                nonNeg = sym->constant >= 0;
                underLen = lenRange && sym->constant < lenRange->lower;
            } else if (sym->linear.size() == 1) {
                auto [s, c] = *sym->linear.begin();
                if (c == 1) {
                    if (!nonNeg) {
                        if (auto sr = env.diffs.rangeOf({false, s}); sr && sr->lower != kMinInt) {
                            if (auto lo = checkedAdd64(sr->lower, sym->constant)) nonNeg = (*lo >= 0);
                        }
                    }
                    if (auto d = env.diffs.maxDiff({false, s}, {true, arrSlot})) {
                        if (auto bound = checkedAdd64(*d, sym->constant)) underLen = (*bound <= -1);
                    }
                    if (!underLen && sym->constant == 0) {
                        if (auto fIt = fullArraySizes.find(arrSlot); fIt != fullArraySizes.end()) {
                            if (auto sr = env.diffs.rangeOf({false, s}); sr && sr->lower >= 0) {
                                for (const auto& [hb, fh] : forHeaders) {
                                    if (!fh.valid || fh.iterSlot != s || !isEnclosingForHeader(hb)) continue;
                                    if (auto us = buildSym(fh.upper.id, env); us && us->sameExpr(fIt->second)) {
                                        underLen = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                } else if (c > 1 && sym->constant >= 0 && sym->constant < c) {
                    // Strided 1D loop index `i * stride + off` bounded by either:
                    // (a) `(n + uCst) / K` with `0 <= n <= len(arr)` and `uCst + off <= K - 1` (`K >= c`)
                    // (b) `len(arr) / K` directly (`Op::Length` on `arrSlot`, `K >= c`)
                    if (auto sr = env.diffs.rangeOf({false, s}); sr && sr->lower >= 0) {
                        for (const auto& [hb, fh] : forHeaders) {
                            if (!fh.valid || fh.iterSlot != s || !isEnclosingForHeader(hb)) continue;
                            auto udit = defOp.find(fh.upper.id);
                            if (udit != defOp.end() && udit->second->op == Op::Binary && udit->second->data == "/") {
                                auto kConst = parseConstInt(defOp.count(udit->second->inputs[1].id) ? defOp.at(udit->second->inputs[1].id)->data : "");
                                if (kConst && *kConst >= c) {
                                    if (auto uAtom = extractLinearAtom(udit->second->inputs[0].id, env);
                                        uAtom && uAtom->isArrLen && uAtom->slot == arrSlot &&
                                        uAtom->offset <= 0 && sym->constant <= *kConst - 1) {
                                        return finish(ProofKind::LoopBound);
                                    }
                                    if (auto numSym = shallowSym(udit->second->inputs[0].id, env);
                                        numSym && numSym->isPureLinear() && numSym->linear.size() == 1) {
                                        auto [ns, nc] = *numSym->linear.begin();
                                        if (nc == 1 && !slotWritable[ns] && numSym->constant >= 0 &&
                                            numSym->constant + sym->constant <= *kConst - 1) {
                                            if (auto nr = env.diffs.rangeOf({false, ns}); nr && nr->lower >= 0) {
                                                if (auto d = env.diffs.maxDiff({false, ns}, {true, arrSlot})) {
                                                    if (*d + numSym->constant + sym->constant - *kConst <= -1)
                                                        return finish(ProofKind::LoopBound);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (c == -1 && idxRange && idxRange->lower >= 0) {
                    // constant - s >= 0 checked; need constant - s <= len(arr) - 1, i.e., -s - len <= -1 - constant
                    if (auto sr = env.diffs.rangeOf({false, s})) {
                        if (sr->lower >= 0 && lenRange) {
                            if (auto mx = checkedSub64(sym->constant, sr->lower))
                                underLen = (*mx < lenRange->lower);
                        }
                    }
                    // Also check constant - s < len when constant == len + k -> arrLen - s + k < arrLen => -s + k < 0 => s > k
                    if (!underLen && lenRange) {
                        if (auto d = env.diffs.maxDiff({true, arrSlot}, {false, s})) {
                            // len - s <= d; wait we need (constant - s) - len <= -1 -> if s >= constant - len + 1
                            // Check if constant == 0? No, usually n - i or n - 1 - i where n == len!
                            if (auto linearSz = linearArraySizes.find(arrSlot);
                                linearSz != linearArraySizes.end() && linearSz->second.linear.size() == 1) {
                                auto [ns, nc] = *linearSz->second.linear.begin();
                                if (nc == 1) {
                                    // sym is constant - s; len is ns + linearSz->second.constant
                                    // diff = (constant - linearSz->constant) - (s + ns)
                                    // Actually if sym is -s + constant and s >= const - ns + 1 ...
                                }
                            }
                        }
                    }
                }
            } else if (sym->linear.size() == 2) {
                // Handle s1 - s2 + c (e.g. i - j, j - w, p - 1)!
                auto it = sym->linear.begin();
                unsigned s1 = it->first;
                std::int64_t c1 = it->second;
                ++it;
                unsigned s2 = it->first;
                std::int64_t c2 = it->second;
                if (c2 == 1 && c1 == -1) { std::swap(s1, s2); std::swap(c1, c2); }
                if (c1 == 1 && c2 == -1) {
                    // expr = s1 - s2 + c
                    if (!nonNeg) {
                        if (auto d = env.diffs.maxDiff({false, s2}, {false, s1})) {
                            // s2 - s1 <= d => s1 - s2 >= -d => expr >= c - d
                            if (auto lo = checkedSub64(sym->constant, *d)) nonNeg = (*lo >= 0);
                        }
                    }
                    // Upper: s1 - s2 + c <= len - 1  <==> (s1 - len) + (c - s2) <= -1
                    if (auto d1 = env.diffs.maxDiff({false, s1}, {true, arrSlot})) {
                        if (auto r2 = env.diffs.rangeOf({false, s2}); r2 && r2->lower > kMinInt) {
                            // s1 - len <= d1 AND -s2 <= -r2->lower => s1 - s2 + c - len <= d1 - r2->lower + c
                            if (auto t1 = checkedSub64(*d1, r2->lower))
                                if (auto t2 = checkedAdd64(*t1, sym->constant)) underLen = (*t2 <= -1);
                        }
                    }
                    // Also if linearArraySizes says len(arr) == nSlot + k and s2 == nSlot?
                    // Handled above since ArrLen == nSlot + k is in diffs!
                }
            }
            if (nonNeg && underLen) {
                return finish(env.contractArrLens.contains(arrSlot) ? ProofKind::ContractRequires : ProofKind::DiffGraph);
            }
        }

        // 2b. Scaled affine floor-division index: (iter / K) + off where iter in 0..(K * N + B)
        {
            Value baseV = idxVal;
            std::int64_t constOff = 0;
            bool okOff = true;
            if (auto d0 = defOp.find(baseV.id); d0 != defOp.end() && d0->second->op == Op::Binary &&
                (d0->second->data == "+" || d0->second->data == "-")) {
                auto c1 = parseConstInt(defOp.count(d0->second->inputs[1].id) ? defOp.at(d0->second->inputs[1].id)->data : "");
                if (c1) {
                    constOff = (d0->second->data == "+") ? *c1 : -*c1;
                    baseV = d0->second->inputs[0];
                } else okOff = false;
            }
            if (okOff) {
                if (auto dL = defOp.find(baseV.id); dL != defOp.end() && dL->second->op == Op::Load &&
                    (!slotWritable[dL->second->slot] || isCurrentLoad(baseV.id, dL->second->slot, env))) {
                    if (auto svIt = env.slotVal.find(dL->second->slot); svIt != env.slotVal.end()) baseV = svIt->second;
                    else if (!slotWritable[dL->second->slot] && dL->second->slot < singleSlotVal.size() && singleSlotVal[dL->second->slot])
                        baseV = *singleSlotVal[dL->second->slot];
                }
                if (auto dDiv = defOp.find(baseV.id); dDiv != defOp.end() && dDiv->second->op == Op::Binary && dDiv->second->data == "/") {
                    auto kc = parseConstInt(defOp.count(dDiv->second->inputs[1].id) ? defOp.at(dDiv->second->inputs[1].id)->data : "");
                    auto ils = loadSlot.find(dDiv->second->inputs[0].id);
                    if (kc && *kc >= 2 && ils != loadSlot.end()) {
                        const unsigned iSlot = ils->second;
                        if (auto ir = env.diffs.rangeOf({false, iSlot}); ir && ir->lower >= 0 && constOff >= 0) {
                            for (const auto& [hb, fh] : forHeaders) {
                                if (!fh.valid || fh.iterSlot != iSlot || !isEnclosingForHeader(hb)) continue;
                                auto loC = parseConstInt(defOp.count(fh.lower.id) ? defOp.at(fh.lower.id)->data : "");
                                if (!loC || *loC < 0) continue;
                                if (auto uSym = shallowSym(fh.upper.id, env); uSym && uSym->isPureLinear() &&
                                    uSym->linear.size() == 1 && uSym->linear.begin()->second == *kc) {
                                    const unsigned nSlot = uSym->linear.begin()->first;
                                    const std::int64_t numMaxConst = uSym->constant - 1; // iter <= K * nSlot + numMaxConst
                                    const std::int64_t q = numMaxConst / *kc - ((numMaxConst % *kc) < 0 ? 1 : 0);
                                    if (auto d = env.diffs.maxDiff({false, nSlot}, {true, arrSlot})) {
                                        if (*d + q + constOff <= -1) return finish(ProofKind::LoopBound);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // 3. Modulo index: x % M where x >= 0 and 1 <= M <= len(arr) (chasing immutable locals in env)
        Value modCand = idxVal;
        if (auto dit0 = defOp.find(modCand.id); dit0 != defOp.end() && dit0->second->op == Op::Load &&
            (!slotWritable[dit0->second->slot] || isCurrentLoad(modCand.id, dit0->second->slot, env))) {
            if (auto svIt = env.slotVal.find(dit0->second->slot); svIt != env.slotVal.end()) {
                modCand = svIt->second;
            } else if (!slotWritable[dit0->second->slot] && dit0->second->slot < singleSlotVal.size() &&
                       singleSlotVal[dit0->second->slot]) {
                modCand = *singleSlotVal[dit0->second->slot];
            }
        }
        if (auto dit = defOp.find(modCand.id); dit != defOp.end() && dit->second->op == Op::Binary &&
            dit->second->data == "%") {
            const auto lhsR = rangeOf(dit->second->inputs[0], env);
            const auto modVal = dit->second->inputs[1];
            const auto modR = rangeOf(modVal, env);
            if (lhsR && lhsR->lower >= 0 && modR && modR->lower >= 1) {
                if (lenRange && modR->upper <= lenRange->lower) return finish(ProofKind::ModuloBound);
                if (auto mSym = shallowSym(modVal.id, env); mSym && mSym->isPureLinear() && mSym->linear.size() == 1) {
                    auto [ms, mc] = *mSym->linear.begin();
                    if (mc == 1) {
                        if (auto d = env.diffs.maxDiff({false, ms}, {true, arrSlot})) {
                            if (auto bound = checkedAdd64(*d, mSym->constant))
                                if (*bound <= 0) return finish(ProofKind::ModuloBound);
                        }
                    }
                }
            }
        }

        // 3b. Exact immutable size shape minus positive offset: idx == fullArraySizes[arr] - delta (delta >= 1) and idx >= 0
        if (idxRange && idxRange->lower >= 0) {
            if (auto fIt = fullArraySizes.find(arrSlot); fIt != fullArraySizes.end()) {
                if (auto full = buildSym(idxVal.id, env)) {
                    const Sym& sz = fIt->second;
                    bool onlyImmutable = true;
                    for (const auto& [ls, lc] : full->linear)
                        if (slotWritable[ls]) { (void)lc; onlyImmutable = false; break; }
                    if (full->hasProduct && (slotWritable[full->prodSlotA] || slotWritable[full->prodSlotB]))
                        onlyImmutable = false;
                    if (onlyImmutable && full->linear == sz.linear && full->hasProduct == sz.hasProduct &&
                        (!full->hasProduct || (full->prodCoeff == sz.prodCoeff &&
                                               full->prodSlotA == sz.prodSlotA && full->prodSlotB == sz.prodSlotB &&
                                               full->prodOffA == sz.prodOffA && full->prodOffB == sz.prodOffB)) &&
                        full->constant < sz.constant) {
                        return finish(ProofKind::DiffGraph);
                    }
                }
            }
        }

            // 4. 2D Square index: len(arr) == (nSlot + off)^2
        if (auto sqIt = squareArraySizes.find(arrSlot); sqIt != squareArraySizes.end() && !slotWritable[sqIt->second.first]) {
            const unsigned nSlot = sqIt->second.first;
            const std::int64_t wOff = sqIt->second.second;
            // Reconstruct expanded affine expression over (row) * W + (col)
            // Build Sym using buildSym (expands p = i * n + j!)
            if (auto full = buildSym(idxVal.id, env); full && full->hasProduct) {
                // Normalize full = k * W + rem? Check simple forms:
                // Case A: prod is (slotA + offA) * (nSlot + wOff) * coeff (with slotA != or == nSlot)
                // We look for full = rowSym * W + colSym where 0 <= rowSym <= W - 1 and 0 <= colSym <= W - 1!
                const bool matchB = (full->prodSlotB == nSlot && full->prodOffB == wOff && full->prodCoeff > 0);
                const bool matchA = (!matchB && full->prodSlotA == nSlot && full->prodOffA == wOff && full->prodCoeff > 0);
                if (matchA || matchB) {
                    const unsigned rowBase = matchB ? full->prodSlotA : full->prodSlotB;
                    const std::int64_t rowBaseOff = matchB ? full->prodOffA : full->prodOffB;
                    const std::int64_t rowScale = full->prodCoeff; // e.g., 1 or 2

                    // Remaining linear terms: may contain multiples of nSlot (like +n or -n in fivePoint/sobel/lifeOneStep!)
                    std::int64_t rowCarry = 0;
                    Sym colSym = Sym::makeConst(full->constant);
                    for (const auto& [ls, lc] : full->linear) {
                        if (ls == nSlot) {
                            rowCarry += lc; // lc * n = lc * W - lc * wOff
                            colSym.constant -= lc * wOff;
                        } else {
                            colSym.linear[ls] = lc;
                        }
                    }
                    // Check if colSym can be bounded in [0, W - 1]
                    // Wait: row = rowScale * rowBase + rowScale * rowBaseOff + rowCarry
                    // Check if row is in [0, W - 1]:
                    auto checkVarIn0ToW = [&](unsigned s, std::int64_t scale, std::int64_t cst) -> bool {
                        // Need: 0 <= scale * s + cst AND scale * s + cst <= nSlot + wOff - 1
                        auto sr = env.diffs.rangeOf({false, s});
                        if (scale == 1) {
                            bool ge0 = sr && sr->lower != kMinInt && (*checkedAdd64(sr->lower, cst) >= 0);
                            if (!ge0) return false;
                            if (auto d = env.diffs.maxDiff({false, s}, {false, nSlot})) {
                                // s - nSlot <= d => (s + cst) - (nSlot + wOff) <= d + cst - wOff <= -1
                                if (*d + cst - wOff <= -1) return true;
                            }
                            return false;
                        }
                        if (scale == 2) {
                            // E.g. 2*i where 0 <= i < n/2 (or 2*i + 1 where i < n/2)
                            if (!sr || sr->lower < 0 || 2 * sr->lower + cst < 0) return false;
                            // Check if upper bound of s in env came from n / 2 or W / 2!
                            // Look for s < boundVal where boundVal is nSlot / 2
                            auto nr = env.diffs.rangeOf({false, nSlot});
                            if (nr && nr->upper <= 4096) {
                                // Check if s <= (n - 2)/2 via for loop upper bound:
                                // Walk defOp of upper bound of s? Even simpler: check Sym or valRanges!
                            }
                        }
                        return false;
                    };

                    // Check colSym
                    auto checkSymIn0ToW = [&](const Sym& cs) -> bool {
                        if (cs.linear.empty()) {
                            auto nr = env.diffs.rangeOf({false, nSlot});
                            return cs.constant >= 0 && nr && nr->lower + wOff > cs.constant;
                        }
                        if (cs.linear.size() == 1) {
                            auto [s, c] = *cs.linear.begin();
                            return checkVarIn0ToW(s, c, cs.constant);
                        }
                        if (cs.linear.size() == 2) {
                            // e.g. i + d (diagonalEnergy) or j + x - 1 (lifeOneStep)!
                            auto it = cs.linear.begin();
                            auto [s1, c1] = *it++;
                            auto [s2, c2] = *it;
                            if (c1 == 1 && c2 == 1) {
                                auto r1 = env.diffs.rangeOf({false, s1});
                                auto r2 = env.diffs.rangeOf({false, s2});
                                if (!r1 || !r2) return false;
                                auto lo12 = checkedAdd64(r1->lower, r2->lower);
                                auto loAll = lo12 ? checkedAdd64(*lo12, cs.constant) : std::optional<std::int64_t>{};
                                if (!loAll || *loAll < 0) return false;
                                // Check s1 + s2 + cst <= nSlot + wOff - 1
                                // Case 1: s1 <= nSlot - s2 - 1 (from `for i in 0..n - d`)!
                                // In `for i in 0..n - d`, env has i <= n - d - 1? Wait: DiffGraph only stores 2-var diffs,
                                // EXCEPT if s1's loop upper bound was `nSlot - s2`, then `valRanges[upper]` or `defOp` has `n - d`!
                                if (r2->upper <= 4096) {
                                    // Check if upper bound of loop for s1 was `nSlot - s2`!
                                    for (const auto& [hb, fh] : forHeaders) {
                                        if (fh.valid && fh.iterSlot == s1 && isEnclosingForHeader(hb)) {
                                            if (auto us = shallowSym(fh.upper.id, env)) {
                                                // If us == nSlot - s2 + uCst: then s1 <= nSlot - s2 + uCst - 1 => s1 + s2 + cs.constant <= nSlot + uCst - 1 + cs.constant!
                                                if (us->linear.size() == 2 && us->linear.count(nSlot) && us->linear[nSlot] == 1 &&
                                                    us->linear.count(s2) && us->linear[s2] == -1) {
                                                    if (us->constant - 1 + cs.constant <= wOff - 1) return true;
                                                }
                                            }
                                        }
                                    }
                                    // Case 2: r2 is small constant (e.g. x in 0..3 -> r2->upper <= 2, as in lifeOneStep!), and s1 <= nSlot - 2!
                                    if (auto d1 = env.diffs.maxDiff({false, s1}, {false, nSlot})) {
                                        if (*d1 + r2->upper + cs.constant <= wOff - 1) return true;
                                    }
                                    if (auto d2 = env.diffs.maxDiff({false, s2}, {false, nSlot})) {
                                        if (*d2 + r1->upper + cs.constant <= wOff - 1) return true;
                                    }
                                }
                            }
                        }
                        return false;
                    };

                    // Also check row = rowScale * rowBase + (rowScale * rowBaseOff + rowCarry)
                    auto checkRow = [&]() -> bool {
                        const std::int64_t rowCst = rowScale * rowBaseOff + rowCarry;
                        if (rowScale == 1) {
                            if (checkVarIn0ToW(rowBase, 1, rowCst)) return true;
                            // Handle lifeOneStep: rowBase may be an immutable temp? No, buildSym expanded p,
                            // wait what about `(y - 1) * n` + `i * n`? Both share `n` so buildSym may not merge `i*n + (y-1)*n` into 1 product!
                        }
                        // Check maxPool/strided pooling: k * i + off where i's loop upper bound is W / k
                        if (rowScale >= 2 && rowCst >= 0 && rowCst < rowScale) {
                            auto rB = env.diffs.rangeOf({false, rowBase});
                            if (rB && rB->lower >= 0) {
                                for (const auto& [hb, fh] : forHeaders) {
                                    if (fh.valid && fh.iterSlot == rowBase && isEnclosingForHeader(hb)) {
                                        auto dit2 = defOp.find(fh.upper.id);
                                        if (dit2 != defOp.end() && dit2->second->op == Op::Binary && dit2->second->data == "/") {
                                            auto rs = shallowSym(dit2->second->inputs[0].id, env);
                                            auto rv = parseConstInt(defOp.count(dit2->second->inputs[1].id) ? defOp.at(dit2->second->inputs[1].id)->data : "");
                                            if (rs && rv == rowScale && rs->linear.size() == 1 &&
                                                rs->linear.count(nSlot) && rs->linear.at(nSlot) == 1 && rs->constant == wOff) {
                                                // rowBase <= (W / k) - 1 => k * rowBase + {0..k-1} <= W - 1!
                                                return true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        return false;
                    };

                    // Also handle colSym with scale >= 2 (e.g. maxPool 2 * j + {0, 1})!
                    auto checkCol = [&]() -> bool {
                        if (checkSymIn0ToW(colSym)) return true;
                        if (colSym.linear.size() == 1) {
                            auto [cs, cc] = *colSym.linear.begin();
                            if (cc >= 2 && colSym.constant >= 0 && colSym.constant < cc) {
                                auto rC = env.diffs.rangeOf({false, cs});
                                if (rC && rC->lower >= 0) {
                                    for (const auto& [hb, fh] : forHeaders) {
                                        if (fh.valid && fh.iterSlot == cs && isEnclosingForHeader(hb)) {
                                            auto dit2 = defOp.find(fh.upper.id);
                                            if (dit2 != defOp.end() && dit2->second->op == Op::Binary && dit2->second->data == "/") {
                                                auto rs = shallowSym(dit2->second->inputs[0].id, env);
                                                auto rv = parseConstInt(defOp.count(dit2->second->inputs[1].id) ? defOp.at(dit2->second->inputs[1].id)->data : "");
                                                if (rs && rv == cc && rs->linear.size() == 1 &&
                                                    rs->linear.count(nSlot) && rs->linear.at(nSlot) == 1 && rs->constant == wOff) {
                                                    return true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        return false;
                    };

                    if (checkRow() && checkCol()) return finish(ProofKind::DiffGraph);
                }
            }
            // Also check lifeOneStep: p + (y - 1) * n + x - 1 = i*n + j + (y-1)*n + x - 1
            // Because buildSym bails on two products `i*n + (y-1)*n`, check direct sum of two expressions!
            if (auto dit = defOp.find(idxVal.id); dit != defOp.end()) {
                // Decompose top-level + / - into additive leaves and collect coeff of W vs non-W!
                std::vector<std::pair<Value, std::int64_t>> addends;
                auto collect = [&](auto& self, Value v, std::int64_t sgn) -> void {
                    auto it = defOp.find(v.id);
                    if (it != defOp.end() && it->second->op == Op::Binary &&
                        (it->second->data == "+" || it->second->data == "-")) {
                        self(self, it->second->inputs[0], sgn);
                        self(self, it->second->inputs[1], it->second->data == "+" ? sgn : -sgn);
                    } else if (it != defOp.end() && it->second->op == Op::Load && !slotWritable[it->second->slot] &&
                               it->second->slot >= fn.source.params.size() && singleSlotVal[it->second->slot]) {
                        self(self, *singleSlotVal[it->second->slot], sgn);
                    } else {
                        addends.emplace_back(v, sgn);
                    }
                };
                collect(collect, idxVal, 1);
                // Extract row terms (multiplied by nSlot + wOff) and col terms (rest)!
                std::vector<std::pair<unsigned, std::int64_t>> rowSlots, colSlots;
                std::int64_t rowCst = 0, colCst = 0;
                bool okDecomp = true;
                for (const auto& [av, sgn] : addends) {
                    if (auto sm = shallowSym(av.id, env)) {
                        if (sm->isConst()) { colCst += sgn * sm->constant; continue; }
                        if (sm->hasProduct && sm->prodCoeff == 1 &&
                            ((sm->prodSlotB == nSlot && sm->prodOffB == wOff) ||
                             (sm->prodSlotA == nSlot && sm->prodOffA == wOff))) {
                            const unsigned rs = (sm->prodSlotB == nSlot && sm->prodOffB == wOff) ? sm->prodSlotA : sm->prodSlotB;
                            const std::int64_t ro = (sm->prodSlotB == nSlot && sm->prodOffB == wOff) ? sm->prodOffA : sm->prodOffB;
                            rowSlots.emplace_back(rs, sgn);
                            rowCst += sgn * ro;
                            continue;
                        }
                        if (sm->isPureLinear() && sm->linear.size() == 1 && sm->constant == 0 &&
                            sm->linear.begin()->first == nSlot && wOff == 0) {
                            rowCst += sgn * sm->linear.begin()->second;
                            continue;
                        }
                        if (sm->isPureLinear()) {
                            colCst += sgn * sm->constant;
                            for (const auto& [cs, cc] : sm->linear) colSlots.emplace_back(cs, sgn * cc);
                            continue;
                        }
                    }
                    okDecomp = false;
                    break;
                }
                if (okDecomp && rowSlots.size() == 2 && rowSlots[0].second == 1 && rowSlots[1].second == 1 &&
                    colSlots.size() == 2 && colSlots[0].second == 1 && colSlots[1].second == 1) {
                    // Two positive terms for row (i + y + rowCst) and two for col (j + x + colCst)
                    auto checkPair = [&](unsigned s1, unsigned s2, std::int64_t cst) -> bool {
                        auto r1 = env.diffs.rangeOf({false, s1});
                        auto r2 = env.diffs.rangeOf({false, s2});
                        if (!r1 || !r2) return false;
                        auto lo12 = checkedAdd64(r1->lower, r2->lower);
                        auto loAll = lo12 ? checkedAdd64(*lo12, cst) : std::optional<std::int64_t>{};
                        if (!loAll || *loAll < 0) return false;
                        if (auto d1 = env.diffs.maxDiff({false, s1}, {false, nSlot})) {
                            if (r2->upper <= 4096 && *d1 <= 4096 && *d1 + r2->upper + cst <= wOff - 1) return true;
                        }
                        if (auto d2 = env.diffs.maxDiff({false, s2}, {false, nSlot})) {
                            if (r1->upper <= 4096 && *d2 <= 4096 && *d2 + r1->upper + cst <= wOff - 1) return true;
                        }
                        return false;
                    };
                    if (checkPair(rowSlots[0].first, rowSlots[1].first, rowCst) &&
                        checkPair(colSlots[0].first, colSlots[1].first, colCst))
                        return finish(ProofKind::DiffGraph);
                }
            }
        }

        // 5. General rectangular 2D index: len(arr) >= (rSlot + rOff) * (cSlot + cOff)
        std::optional<RectShape> rectShape;
        if (auto er = env.rectArrShapes.find(arrSlot); er != env.rectArrShapes.end()) {
            rectShape = er->second;
        } else if (auto sr = rectArraySizes.find(arrSlot); sr != rectArraySizes.end()) {
            rectShape = sr->second;
        }
        if (rectShape && !slotWritable[rectShape->rSlot] && !slotWritable[rectShape->cSlot]) {
            if (auto full = buildSym(idxVal.id, env); full && full->hasProduct && full->prodCoeff == 1) {
                auto tryRectOrient = [&](unsigned rowBSlot, std::int64_t rowBOff,
                                         unsigned colBSlot, std::int64_t colBOff) -> bool {
                    const bool matchB = (full->prodSlotB == colBSlot && full->prodOffB == colBOff);
                    const bool matchA = (!matchB && full->prodSlotA == colBSlot && full->prodOffA == colBOff);
                    if (!matchA && !matchB) return false;
                    const unsigned rowBase = matchB ? full->prodSlotA : full->prodSlotB;
                    const std::int64_t rowBaseOff = matchB ? full->prodOffA : full->prodOffB;

                    std::int64_t rowCarry = 0;
                    Sym colSym = Sym::makeConst(full->constant);
                    for (const auto& [ls, lc] : full->linear) {
                        if (ls == colBSlot) {
                            rowCarry += lc;
                            colSym.constant -= lc * colBOff;
                        } else {
                            colSym.linear[ls] = lc;
                        }
                    }

                    auto checkBounded = [&](unsigned s, std::int64_t cst, unsigned boundSlot, std::int64_t boundOff) -> bool {
                        auto sr = env.diffs.rangeOf({false, s});
                        if (!sr || sr->lower == kMinInt) return false;
                        auto lo = checkedAdd64(sr->lower, cst);
                        if (!lo || *lo < 0) return false;
                        if (auto d = env.diffs.maxDiff({false, s}, {false, boundSlot})) {
                            auto diff = checkedAdd64(*d, cst - boundOff);
                            return diff && *diff <= -1;
                        }
                        return false;
                    };

                    if (!checkBounded(rowBase, rowBaseOff + rowCarry, rowBSlot, rowBOff)) return false;

                    if (colSym.linear.empty()) {
                        auto br = env.diffs.rangeOf({false, colBSlot});
                        return colSym.constant >= 0 && br && br->lower != kMinInt && br->lower + colBOff > colSym.constant;
                    }
                    if (colSym.linear.size() == 1) {
                        auto [cs, cc] = *colSym.linear.begin();
                        return cc == 1 && checkBounded(cs, colSym.constant, colBSlot, colBOff);
                    }
                    return false;
                };

                if (tryRectOrient(rectShape->rSlot, rectShape->rOff, rectShape->cSlot, rectShape->cOff) ||
                    tryRectOrient(rectShape->cSlot, rectShape->cOff, rectShape->rSlot, rectShape->rOff)) {
                    return finish(env.contractArrLens.contains(arrSlot) ? ProofKind::ContractRequires : ProofKind::DiffGraph);
                }
            }
        }
        return false;
    }

public:
    std::unordered_map<unsigned, IntRange> observedValRanges;
    std::unordered_map<unsigned, IntRange> prevObservedValRanges;
    std::unordered_map<unsigned, IntRange> arraySizeRanges;

    FunctionProver(FunctionIR& f, const std::unordered_map<std::string, IntRange>& fnRets,
                   std::vector<std::optional<IntRange>> initParams = {},
                   const Program* prog = nullptr)
        : fn(f), fnReturnRanges(fnRets), initialParamRanges(std::move(initParams)), programPtr(prog), cfg(buildCFG(f)) {
        analyzeStructure();
        analyzeArrays();
    }

    void run() {
        // Four passes: Pass 0/1 resolve primary and downstream array element ranges (e.g. copy/transpose),
        // Pass 1/2 compute loop-step bounds for scalar accumulators and nested small loops,
        // Pass 2/3 propagate final bounds & widths.
        prevObservedValRanges.clear();
        smallLoopResults.clear();
        loopSumSources.clear();
        modSlotLtDiv.clear();
        for (int pass = 0; pass < 5; ++pass) {
            if (pass > 0) prevObservedValRanges = observedValRanges;
            observedValRanges.clear();
            arraySizeRanges.clear();
            loopPreAccRanges.clear();
            loopPreModLtDiv.clear();
            loopMaxTrips.clear();
            sqrtLoopUpper.clear();
            sqrtLoopBoundRange.clear();
            activeHeronRanges.clear();
            activeEuclidUpper.clear();
            activeBoundedSelectors.clear();
            divShrinkBoundRange.clear();
            activeGuardedCounters.clear();
            activeTwoWayPartitions.clear();
            activeUniqueZeroMaxN.clear();
            bsLoopUpper.clear();
            bsLoopArrDiffs.clear();
            std::vector<bool> visited(fn.blocks.size(), false);
            Env root;
            root.slotEpoch.assign(fn.slots.size(), 0);
            for (std::size_t i = 0; i < fn.source.params.size() && i < initialParamRanges.size(); ++i) {
                if (initialParamRanges[i]) {
                    root.diffs.addLower({false, static_cast<unsigned>(i)}, initialParamRanges[i]->lower);
                    root.diffs.addUpper({false, static_cast<unsigned>(i)}, initialParamRanges[i]->upper);
                }
            }
            processBlockCollecting(0, std::move(root), visited);
            if (pass <= 3) updateGlobalElemRanges();
            if (pass >= 1 && pass <= 3) updateLoopAccRanges();
        }
    }

    void applyCompaction(bool allowCompaction) {
        const std::size_t nSlots = fn.slots.size();
        fn.slotElemBits.assign(nSlots, 64);
        fn.slotElemUnsigned.assign(nSlots, false);
        for (auto& blk : fn.blocks) {
            for (auto& op : blk.ops) {
                if (op.op == Op::RepeatRegion || op.op == Op::RepeatHeap ||
                    op.op == Op::Clone || op.op == Op::Index || op.op == Op::IndexSet) {
                    op.elemBits = 64;
                    op.elemUnsigned = false;
                }
            }
        }
        if (allowCompaction) {
            std::vector<bool> notCompactable(nSlots, false);
            std::vector<int> defCount(nSlots, 0);
            std::unordered_map<unsigned, unsigned> repeatDest;
            std::vector<std::pair<unsigned, unsigned>> clonePairs;
            for (const auto& blk : fn.blocks) {
                for (const auto& op : blk.ops) {
                    if (op.op == Op::Store && !op.inputs.empty() && op.slot < nSlots && fn.slots[op.slot] == Type::IntArray) {
                        if (auto dit = defOp.find(op.inputs[0].id); dit != defOp.end() &&
                            (dit->second->op == Op::RepeatRegion || dit->second->op == Op::RepeatHeap ||
                             dit->second->op == Op::Clone)) {
                            if (repeatDest.contains(dit->second->result.id)) {
                                notCompactable[repeatDest[dit->second->result.id]] = true;
                                notCompactable[op.slot] = true;
                            } else {
                                repeatDest[dit->second->result.id] = op.slot;
                            }
                        }
                    }
                }
            }
            for (std::size_t s = 0; s < nSlots; ++s) {
                if (s < fn.source.params.size() || (s < fn.slotByRef.size() && fn.slotByRef[s]) ||
                    fn.slots[s] != Type::IntArray || !globalElemRanges.count(static_cast<unsigned>(s)))
                    notCompactable[s] = true;
            }
            for (const auto& blk : fn.blocks) {
                for (const auto& op : blk.ops) {
                    if ((op.op == Op::ClearOwner || op.op == Op::ClearField || op.op == Op::PlaceSet) && op.slot < nSlots) notCompactable[op.slot] = true;
                    if (op.op == Op::ClearIndex && !op.inputs.empty()) {
                        if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end() && ls->second < nSlots)
                            notCompactable[ls->second] = true;
                    }
                    if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Builtin || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                        for (unsigned ms : op.mutArgSlots) {
                            if (ms < nSlots) notCompactable[ms] = true;
                        }
                    }
                    if (op.op == Op::Store && !op.inputs.empty() && op.slot < nSlots && fn.slots[op.slot] == Type::IntArray) {
                        auto def = defOp.find(op.inputs[0].id);
                        if (def != defOp.end() &&
                            (def->second->op == Op::RepeatRegion || def->second->op == Op::RepeatHeap ||
                             def->second->op == Op::Clone)) {
                            ++defCount[op.slot];
                            if (def->second->op == Op::Clone) {
                                if (!def->second->inputs.empty()) {
                                    if (auto srcLs = loadSlot.find(def->second->inputs[0].id); srcLs != loadSlot.end()) {
                                        clonePairs.emplace_back(op.slot, srcLs->second);
                                    } else {
                                        notCompactable[op.slot] = true;
                                    }
                                } else {
                                    notCompactable[op.slot] = true;
                                }
                            }
                        } else {
                            notCompactable[op.slot] = true;
                        }
                    }
                    for (std::size_t ii = 0; ii < op.inputs.size(); ++ii) {
                        const Value& in = op.inputs[ii];
                        if (in.type != Type::IntArray) continue;
                        const bool whitelistedLoad =
                            ((op.op == Op::Index || op.op == Op::IndexSet || op.op == Op::Length || op.op == Op::Clone) && ii == 0);
                        if (!whitelistedLoad) {
                            if (auto ls = loadSlot.find(in.id); ls != loadSlot.end())
                                notCompactable[ls->second] = true;
                        }
                        const bool whitelistedRepeat = (op.op == Op::Store && ii == 0);
                        if (!whitelistedRepeat) {
                            if (auto rit = repeatDest.find(in.id); rit != repeatDest.end())
                                notCompactable[rit->second] = true;
                        }
                    }
                }
                if (blk.exit == Exit::Return && blk.value && blk.value->type == Type::IntArray) {
                    if (auto ls = loadSlot.find(blk.value->id); ls != loadSlot.end())
                        notCompactable[ls->second] = true;
                    if (auto rit = repeatDest.find(blk.value->id); rit != repeatDest.end())
                        notCompactable[rit->second] = true;
                }
            }
            std::vector<std::optional<IntRange>> effRanges(nSlots);
            for (std::size_t s = 0; s < nSlots; ++s) {
                if (auto git = globalElemRanges.find(static_cast<unsigned>(s));
                    git != globalElemRanges.end() && git->second)
                    effRanges[s] = *git->second;
            }
            bool cloneChanged = true;
            while (cloneChanged) {
                cloneChanged = false;
                for (const auto& [dstS, srcS] : clonePairs) {
                    if (notCompactable[dstS] || notCompactable[srcS] ||
                        defCount[dstS] != 1 || defCount[srcS] != 1 ||
                        !effRanges[dstS] || !effRanges[srcS]) {
                        if (!notCompactable[dstS]) { notCompactable[dstS] = true; cloneChanged = true; }
                        if (!notCompactable[srcS]) { notCompactable[srcS] = true; cloneChanged = true; }
                        continue;
                    }
                    const IntRange env = ReturnRanges::envelope(*effRanges[dstS], *effRanges[srcS]);
                    if (effRanges[dstS]->lower != env.lower || effRanges[dstS]->upper != env.upper) {
                        effRanges[dstS] = env; cloneChanged = true;
                    }
                    if (effRanges[srcS]->lower != env.lower || effRanges[srcS]->upper != env.upper) {
                        effRanges[srcS] = env; cloneChanged = true;
                    }
                }
            }
            std::unordered_map<unsigned, std::pair<std::uint8_t, bool>> chosen;
            for (std::size_t s = 0; s < nSlots; ++s) {
                if (notCompactable[s] || defCount[s] != 1 || !effRanges[s]) continue;
                const IntRange r = *effRanges[s];
                std::uint8_t bits = 64;
                bool uns = false;
                if (r.lower >= 0 && r.upper <= 255) { bits = 8; uns = true; }
                else if (r.lower >= -128 && r.upper <= 127) { bits = 8; uns = false; }
                else if (r.lower >= 0 && r.upper <= 32767) { bits = 16; uns = false; }
                else if (r.lower >= 0 && r.upper <= 65535) { bits = 16; uns = true; }
                else if (r.lower >= -32768 && r.upper <= 32767) { bits = 16; uns = false; }
                else if (r.lower >= 0 && r.upper <= 4294967295LL) { bits = 32; uns = true; }
                else if (r.lower >= -2147483648LL && r.upper <= 2147483647LL) { bits = 32; uns = false; }
                if (bits < 64) {
                    fn.slotElemBits[s] = bits;
                    fn.slotElemUnsigned[s] = uns;
                    chosen[static_cast<unsigned>(s)] = {bits, uns};
                }
            }
            if (!chosen.empty()) {
                for (auto& blk : fn.blocks) {
                    for (auto& op : blk.ops) {
                        if (op.op == Op::RepeatRegion || op.op == Op::RepeatHeap || op.op == Op::Clone) {
                            if (auto rit = repeatDest.find(op.result.id); rit != repeatDest.end()) {
                                if (auto cit = chosen.find(rit->second); cit != chosen.end()) {
                                    op.elemBits = cit->second.first;
                                    op.elemUnsigned = cit->second.second;
                                }
                            }
                        } else if ((op.op == Op::Index || op.op == Op::IndexSet) && !op.inputs.empty()) {
                            if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end()) {
                                if (auto cit = chosen.find(ls->second); cit != chosen.end()) {
                                    op.elemBits = cit->second.first;
                                    op.elemUnsigned = cit->second.second;
                                }
                            }
                        }
                    }
                }
            }
        }
        cleanupNarrowWidths();
    }

    void cleanupNarrowWidths() {
        const std::size_t nSlots = fn.slots.size();
        std::vector<bool> loopCarrySlot(nSlots, false);
        for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
            const auto& lp = cfg.loops[li];
            for (unsigned ws : loopWritten[li]) {
                if (ws >= nSlots || fn.slots[ws] != Type::Int) continue;
                bool loadedInHeader = false;
                for (const auto& op : fn.blocks[lp.header].ops) {
                    if (op.op == Op::Load && op.slot == ws) { loadedInHeader = true; break; }
                }
                if (loadedInHeader) { loopCarrySlot[ws] = true; continue; }
                int blocksUsing = 0;
                bool readBeforeWrite = false;
                for (unsigned b : lp.body) {
                    bool sawLoad = false, sawStore = false;
                    for (const auto& op : fn.blocks[b].ops) {
                        if (op.op == Op::Load && op.slot == ws) {
                            sawLoad = true;
                            if (!sawStore) readBeforeWrite = true;
                        } else if (op.op == Op::Store && op.slot == ws) {
                            sawStore = true;
                        }
                    }
                    if (sawLoad || sawStore) ++blocksUsing;
                }
                if (readBeforeWrite || blocksUsing > 1) loopCarrySlot[ws] = true;
            }
        }

        std::unordered_set<unsigned> mustBe64Vals;
        std::unordered_set<unsigned> mustBe64Slots;
        for (std::size_t s = 0; s < nSlots; ++s) {
            if (loopCarrySlot[s]) mustBe64Slots.insert(static_cast<unsigned>(s));
        }

        for (auto& blk : fn.blocks) {
            for (auto& op : blk.ops) {
                if (op.op == Op::Binary &&
                    (op.data == "==" || op.data == "!=" || op.data == "<" ||
                     op.data == "<=" || op.data == ">" || op.data == ">=")) {
                    op.width = 64;
                }
                if ((op.op == Op::Index || op.op == Op::IndexSet ||
                     op.op == Op::RepeatRegion || op.op == Op::RepeatHeap) &&
                    op.inputs.size() >= 2) {
                    mustBe64Vals.insert(op.inputs[1].id);
                } else if (op.op == Op::PlaceSet && op.inputs.size() >= 2) {
                    for (std::size_t k = 0; k + 1 < op.inputs.size(); ++k)
                        mustBe64Vals.insert(op.inputs[k].id);
                }
                for (const auto& path : op.mutArgPaths) {
                    for (const auto& step : path)
                        if (step.isIndex) mustBe64Slots.insert(step.idxSlot);
                }
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& blk : fn.blocks) {
                std::unordered_map<unsigned, unsigned> curSlotVal;
                for (const auto& op : blk.ops) {
                    if (op.op == Op::Store && !op.inputs.empty()) curSlotVal[op.slot] = op.inputs[0].id;
                    else if (op.op == Op::Load) {
                        if (auto it = curSlotVal.find(op.slot); it != curSlotVal.end()) {
                            if (mustBe64Vals.contains(op.result.id) && mustBe64Vals.insert(it->second).second)
                                changed = true;
                        }
                    }
                }
                for (auto it = blk.ops.rbegin(); it != blk.ops.rend(); ++it) {
                    const Operation& op = *it;
                    if (op.op == Op::Store && !op.inputs.empty() && mustBe64Slots.contains(op.slot)) {
                        if (mustBe64Vals.insert(op.inputs[0].id).second) changed = true;
                    } else if (op.op == Op::Load && mustBe64Vals.contains(op.result.id)) {
                        if (op.slot < singleSlotVal.size() && singleSlotVal[op.slot]) {
                            const unsigned targetId = singleSlotVal[op.slot]->id;
                            for (const auto& sop : blk.ops) {
                                if (sop.result.id == targetId) {
                                    if (mustBe64Vals.insert(targetId).second) changed = true;
                                    break;
                                }
                            }
                        }
                    } else if (op.result.type == Type::Int && mustBe64Vals.contains(op.result.id) &&
                               (op.op == Op::Negate || (op.op == Op::Binary &&
                                (op.data == "+" || op.data == "-" || op.data == "*")))) {
                        for (const auto& in : op.inputs) {
                            if (in.type == Type::Int && mustBe64Vals.insert(in.id).second) changed = true;
                        }
                    }
                }
            }
        }

        auto isArithNarrow = [](const Operation& op) {
            return op.result.type == Type::Int && op.width < 64 &&
                   (op.op == Op::Negate || (op.op == Op::Binary &&
                    (op.data == "+" || op.data == "-" || op.data == "*")));
        };
        for (auto& blk : fn.blocks) {
            for (auto& op : blk.ops) {
                if (isArithNarrow(op) && mustBe64Vals.contains(op.result.id)) op.width = 64;
            }
        }

        std::unordered_set<unsigned> feedsNarrowSink;
        std::unordered_set<unsigned> narrowSource;
        for (const auto& blk : fn.blocks) {
            for (const auto& op : blk.ops) {
                if (op.op == Op::Int && op.result.type == Type::Int) narrowSource.insert(op.result.id);
                else if (op.op == Op::Index && op.elemBits < 64) narrowSource.insert(op.result.id);
                else if (op.width < 64 && (op.op == Op::Min || op.op == Op::Max || op.op == Op::AbsDiff ||
                         (op.op == Op::Binary && (op.data == "/" || op.data == "%")))) {
                    narrowSource.insert(op.result.id);
                }
            }
        }
        bool sinkChanged = true;
        while (sinkChanged) {
            sinkChanged = false;
            for (const auto& blk : fn.blocks) {
                std::unordered_map<unsigned, std::vector<unsigned>> slotLoadedAs;
                for (const auto& op : blk.ops) {
                    if (op.op == Op::Load) slotLoadedAs[op.slot].push_back(op.result.id);
                    const bool isSink =
                        ((op.op == Op::Min || op.op == Op::Max || op.op == Op::AbsDiff) && op.width < 64) ||
                        (op.op == Op::Binary && (op.data == "/" || op.data == "%") && op.width < 64);
                    if (isSink) {
                        for (const auto& in : op.inputs)
                            if (feedsNarrowSink.insert(in.id).second) sinkChanged = true;
                    } else if (op.op == Op::IndexSet && op.elemBits < 64 && op.inputs.size() >= 3) {
                        if (feedsNarrowSink.insert(op.inputs[2].id).second) sinkChanged = true;
                    } else if (isArithNarrow(op) && feedsNarrowSink.contains(op.result.id)) {
                        for (const auto& in : op.inputs)
                            if (feedsNarrowSink.insert(in.id).second) sinkChanged = true;
                    }
                }
                for (const auto& op : blk.ops) {
                    if (op.op == Op::Store && !op.inputs.empty()) {
                        for (unsigned lid : slotLoadedAs[op.slot]) {
                            if (feedsNarrowSink.contains(lid) && feedsNarrowSink.insert(op.inputs[0].id).second)
                                sinkChanged = true;
                        }
                    }
                }
            }
        }
        bool srcChanged = true;
        while (srcChanged) {
            srcChanged = false;
            for (const auto& blk : fn.blocks) {
                for (const auto& op : blk.ops) {
                    if (!isArithNarrow(op) || narrowSource.contains(op.result.id)) continue;
                    bool allNarrowIn = !op.inputs.empty();
                    for (const auto& in : op.inputs) {
                        if (!narrowSource.contains(in.id)) { allNarrowIn = false; break; }
                    }
                    if (allNarrowIn && narrowSource.insert(op.result.id).second) srcChanged = true;
                }
            }
        }
        for (auto& blk : fn.blocks) {
            for (auto& op : blk.ops) {
                if (isArithNarrow(op) &&
                    !feedsNarrowSink.contains(op.result.id) &&
                    !narrowSource.contains(op.result.id)) {
                    op.width = 64;
                }
            }
        }
        std::unordered_set<unsigned> prefer32Vals;
        for (const auto& blk : fn.blocks) {
            for (const auto& op : blk.ops) {
                if (op.op == Op::Binary && op.modSubSteps > 0) {
                    for (const auto& in : op.inputs) prefer32Vals.insert(in.id);
                } else if (op.op == Op::Store && !op.inputs.empty() &&
                           op.slot < nSlots && loopCarrySlot[op.slot]) {
                    prefer32Vals.insert(op.inputs[0].id);
                } else if ((op.op == Op::Negate || op.op == Op::Min || op.op == Op::Max ||
                            (op.op == Op::Binary &&
                             (op.data == "+" || op.data == "-" || op.data == "*" ||
                              op.data == "/" || op.data == "%"))) &&
                           op.width == 32) {
                    for (const auto& in : op.inputs) prefer32Vals.insert(in.id);
                }
            }
        }
        for (auto& blk : fn.blocks) {
            for (auto& op : blk.ops) {
                if ((isArithNarrow(op) || op.op == Op::Min || op.op == Op::Max) &&
                    op.width == 16 && prefer32Vals.contains(op.result.id)) {
                    op.width = 32;
                }
            }
        }
    }

    std::optional<IntRange> computeReturnRange() const {
        std::optional<IntRange> out;
        bool any = false;
        for (const auto& b : fn.blocks) {
            if (b.exit != Exit::Return || !b.value) continue;
            any = true;
            auto it = observedValRanges.find(b.value->id);
            if (it == observedValRanges.end()) return {};
            out = out ? ReturnRanges::envelope(*out, it->second) : it->second;
        }
        return any ? out : std::optional<IntRange>{};
    }

private:
    void processBlockCollecting(unsigned rootBlock, Env rootEnv, std::vector<bool>& visited) {
        std::vector<std::pair<unsigned, Env>> worklist;
        worklist.emplace_back(rootBlock, std::move(rootEnv));
        std::vector<std::pair<unsigned, Env>> localChildren;
        while (!worklist.empty()) {
            auto item = std::move(worklist.back());
            worklist.pop_back();
            localChildren.clear();
            processBlockCollectingStep(item.first, std::move(item.second), visited, localChildren);
            for (auto it = localChildren.rbegin(); it != localChildren.rend(); ++it) {
                worklist.push_back(std::move(*it));
            }
        }
    }

    void processBlockCollectingStep(unsigned b, Env env, std::vector<bool>& visited,
                                    std::vector<std::pair<unsigned, Env>>& outChildren) {
        visited[b] = true;
        auto& blk = fn.blocks[b];
        if (cfg.idom[b] >= 0) {
            const unsigned dom = static_cast<unsigned>(cfg.idom[b]);
            if (auto fit = forHeaders.find(dom); fit != forHeaders.end() && fit->second.valid &&
                fit->second.bodyBlock == b && cfg.preds[b].size() == 1) {
                const auto& fh = fit->second;
                if (auto lr = rangeOf(fh.lower, env)) env.diffs.addLower({false, fh.iterSlot}, lr->lower);
                if (auto ur = rangeOf(fh.upper, env)) {
                    if (ur->upper > kMinInt) env.diffs.addUpper({false, fh.iterSlot}, ur->upper - 1);
                }
                if (auto ls = shallowSym(fh.lower.id, env); ls && ls->isPureLinear() && ls->linear.size() == 1) {
                    auto [s, c] = *ls->linear.begin();
                    if (c == 1 && !slotWritable[s] && ls->constant != kMinInt)
                        env.diffs.addSlotDiff({false, s}, {false, fh.iterSlot}, -ls->constant);
                }
                auto canRelateUpperSlot = [&](unsigned s) -> bool {
                    if (!slotWritable[s]) return true;
                    const int curLi = cfg.loopOfBlock[b];
                    if (curLi < 0 || loopWritten[curLi].contains(s)) return false;
                    bool inOuterLoop = false;
                    for (std::size_t oli = 0; oli < cfg.loops.size(); ++oli) {
                        if (oli != static_cast<std::size_t>(curLi) && cfg.loops[oli].body.contains(dom) &&
                            loopWritten[oli].contains(s)) {
                            bool monoDec = false;
                            for (const auto& ms : loopMonotone[oli]) {
                                if (ms.slot != s) continue;
                                if (ms.nonIncreasing) monoDec = true;
                                else if (ms.nonIncIfNonNeg) {
                                    if (auto r = env.diffs.rangeOf({false, s}); r && r->lower >= 0) monoDec = true;
                                }
                            }
                            if (!monoDec) return false;
                            inOuterLoop = true;
                        }
                    }
                    return inOuterLoop && isCurrentLoad(fh.upper.id, s, env);
                };
                if (auto us = shallowSym(fh.upper.id, env); us && us->isPureLinear() && us->linear.size() == 1) {
                    auto [s, c] = *us->linear.begin();
                    if (c == 1 && canRelateUpperSlot(s)) {
                        if (auto bound = checkedAdd64(us->constant, -1))
                            env.diffs.addSlotDiff({false, fh.iterSlot}, {false, s}, *bound);
                    }
                } else if (auto uAtom = extractLinearAtom(fh.upper.id, env); uAtom && uAtom->isArrLen) {
                    if (auto bound = checkedAdd64(uAtom->offset, -1))
                        env.diffs.addSlotDiff({false, fh.iterSlot}, {true, uAtom->slot}, *bound);
                } else if (auto udit = defOp.find(fh.upper.id); udit != defOp.end() && udit->second->op == Op::Length &&
                           udit->second->inputs[0].type != Type::Str &&
                           udit->second->inputs[0].type != Type::StringBuilder) {
                    if (auto als = loadSlot.find(udit->second->inputs[0].id); als != loadSlot.end() &&
                        (!slotWritable[als->second] || isCurrentLoad(udit->second->inputs[0].id, als->second, env))) {
                        env.diffs.addSlotDiff({false, fh.iterSlot}, {true, als->second}, -1);
                    }
                }
            }
        }

        for (auto& op : blk.ops) {
            op.width = 64;
            switch (op.op) {
            case Op::Int:
                if (op.result.type == Type::Int) {
                    if (auto c = parseConstInt(op.data)) env.valRanges[op.result.id] = IntRange{*c, *c};
                }
                break;
            case Op::Length:
                if (op.inputs[0].type == Type::Str || op.inputs[0].type == Type::StringBuilder) {
                    env.valRanges[op.result.id] = IntRange{0, maxStringLength};
                } else {
                    IntRange lr{0, maxArrayLength};
                    if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end()) {
                        if (auto dr = env.diffs.rangeOf({true, ls->second}))
                            lr = *intersectRange(lr, dr);
                    }
                    env.valRanges[op.result.id] = lr;
                }
                break;
            case Op::ReadByte:
                env.valRanges[op.result.id] = IntRange{-1, 255};
                break;
            case Op::Load:
                if (op.slot < env.slotEpoch.size()) env.loadEpoch[op.result.id] = env.slotEpoch[op.slot];
                if (auto sr = env.diffs.rangeOf({false, op.slot})) env.valRanges[op.result.id] = *sr;
                break;
            case Op::Store: {
                const auto vr = rangeOf(op.inputs[0], env);
                std::optional<Sym> storeSym;
                if (fn.slots[op.slot] == Type::Int) {
                    storeSym = shallowSym(op.inputs[0].id, env);
                }
                invalidateSlot(env, op.slot);
                if (fn.slots[op.slot] == Type::Int) env.slotVal[op.slot] = op.inputs[0];
                if (vr) {
                    env.diffs.addLower({false, op.slot}, vr->lower);
                    env.diffs.addUpper({false, op.slot}, vr->upper);
                }
                if (auto rlt = env.remValLtSlot.find(op.inputs[0].id); rlt != env.remValLtSlot.end()) {
                    env.diffs.addSlotDiff({false, op.slot}, {false, rlt->second}, -1);
                }
                if (auto ilt = env.idxElemLtSlot.find(op.inputs[0].id); ilt != env.idxElemLtSlot.end()) {
                    env.diffs.addLower({false, op.slot}, 0);
                    env.diffs.addSlotDiff({false, op.slot}, {false, ilt->second}, -1);
                }
                // Integer midpoint lemma: mid = (lo + hi) / 2 with 0 <= lo < hi => lo <= mid <= hi - 1
                if (fn.slots[op.slot] == Type::Int) {
                    if (auto md = defOp.find(op.inputs[0].id); md != defOp.end() &&
                        md->second->op == Op::Binary && md->second->data == "/") {
                        auto two = parseConstInt(defOp.count(md->second->inputs[1].id)
                                                 ? defOp.at(md->second->inputs[1].id)->data : "");
                        if (two == 2) {
                            if (auto sd = defOp.find(md->second->inputs[0].id); sd != defOp.end() &&
                                sd->second->op == Op::Binary && sd->second->data == "+") {
                                auto als = loadSlot.find(sd->second->inputs[0].id);
                                auto bls = loadSlot.find(sd->second->inputs[1].id);
                                if (als != loadSlot.end() && bls != loadSlot.end() &&
                                    als->second != op.slot && bls->second != op.slot &&
                                    isCurrentLoad(sd->second->inputs[0].id, als->second, env) &&
                                    isCurrentLoad(sd->second->inputs[1].id, bls->second, env)) {
                                    unsigned aS = als->second, bS = bls->second;
                                    auto tryOrder = [&](unsigned loS, unsigned hiS) -> bool {
                                        auto lr = env.diffs.rangeOf({false, loS});
                                        auto d = env.diffs.maxDiff({false, loS}, {false, hiS});
                                        if (lr && lr->lower >= 0 && d && *d <= -1) {
                                            env.diffs.addSlotDiff({false, loS}, {false, op.slot}, 0);
                                            env.diffs.addSlotDiff({false, op.slot}, {false, hiS}, -1);
                                            return true;
                                        }
                                        return false;
                                    };
                                    if (!tryOrder(aS, bS)) tryOrder(bS, aS);
                                }
                            }
                        }
                    }
                }
                if (storeSym && storeSym->isPureLinear() && storeSym->linear.size() == 1) {
                    auto [ss, sc] = *storeSym->linear.begin();
                    if (sc == 1 && ss != op.slot) {
                        // shallowSym already verified isCurrentLoad(..., env) before invalidateSlot.
                        env.diffs.addSlotDiff({false, op.slot}, {false, ss}, storeSym->constant);
                        if (storeSym->constant != kMinInt)
                            env.diffs.addSlotDiff({false, ss}, {false, op.slot}, -storeSym->constant);
                    }
                }
                if (fn.slots[op.slot] != Type::Int && fn.slots[op.slot] != Type::Bool) {
                    env.diffs.forgetLen(op.slot);
                }
                if (isAnyArrayType(programPtr, fn.slots[op.slot])) {
                    auto dit = defOp.find(op.inputs[0].id);
                    if (dit != defOp.end() && (dit->second->op == Op::RepeatRegion || dit->second->op == Op::RepeatHeap)) {
                        const auto szVal = dit->second->inputs[1];
                        env.diffs.addLower({true, op.slot}, 0);
                        env.diffs.addUpper({true, op.slot}, maxArrayLength);
                        if (auto szR = rangeOf(szVal, env)) {
                            env.diffs.addLower({true, op.slot}, std::max<std::int64_t>(0, szR->lower));
                            env.diffs.addUpper({true, op.slot}, std::min<std::int64_t>(maxArrayLength, szR->upper));
                        }
                        if (auto dr = env.diffs.rangeOf({true, op.slot})) arraySizeRanges[op.slot] = *dr;
                        if (auto szSym = shallowSym(szVal.id, env); szSym && szSym->isPureLinear() && szSym->linear.size() == 1) {
                            auto [ns, nc] = *szSym->linear.begin();
                            if (nc == 1 && !slotWritable[ns]) {
                                env.diffs.addSlotDiff({true, op.slot}, {false, ns}, szSym->constant);
                                if (szSym->constant != kMinInt)
                                    env.diffs.addSlotDiff({false, ns}, {true, op.slot}, -szSym->constant);
                                if (auto loN = checkedSub64(0, szSym->constant)) env.diffs.addLower({false, ns}, *loN);
                                if (auto hiN = checkedSub64(maxArrayLength, szSym->constant)) env.diffs.addUpper({false, ns}, *hiN);
                            } else if (nc == 2 && szSym->constant == 0 && !slotWritable[ns]) {
                                env.diffs.addLower({false, ns}, 0);
                                env.diffs.addUpper({false, ns}, maxArrayLength / 2);
                            }
                        } else if (auto sqIt = squareArraySizes.find(op.slot); sqIt != squareArraySizes.end()) {
                            const unsigned ns = sqIt->second.first;
                            const std::int64_t wOff = sqIt->second.second;
                            if (!slotWritable[ns]) {
                                if (auto lo = checkedSub64(-4096, wOff)) env.diffs.addLower({false, ns}, *lo);
                                if (auto hi = checkedSub64(4096, wOff)) env.diffs.addUpper({false, ns}, *hi);
                            }
                        }
                    } else if (dit != defOp.end() && dit->second->op == Op::Load) {
                        const unsigned src = dit->second->slot;
                        if (src != op.slot) {
                            env.diffs.addSlotDiff({true, op.slot}, {true, src}, 0);
                            env.diffs.addSlotDiff({true, src}, {true, op.slot}, 0);
                            if (auto dr = env.diffs.rangeOf({true, op.slot})) arraySizeRanges[op.slot] = *dr;
                        }
                    } else if (dit != defOp.end() && dit->second->op == Op::Clone && !dit->second->inputs.empty()) {
                        if (auto ls = loadSlot.find(dit->second->inputs[0].id); ls != loadSlot.end() && ls->second != op.slot) {
                            env.diffs.addSlotDiff({true, op.slot}, {true, ls->second}, 0);
                            env.diffs.addSlotDiff({true, ls->second}, {true, op.slot}, 0);
                            if (auto dr = env.diffs.rangeOf({true, op.slot})) arraySizeRanges[op.slot] = *dr;
                        }
                    }
                }
                break;
            }
            case Op::FieldSet:
            case Op::PlaceSet:
                if (op.slot < fn.slots.size()) invalidateSlot(env, op.slot);
                break;
            case Op::Builtin:
                if (op.data == "ptrWrite" || op.data == "ptrCopy" || op.data == "ptrZero" ||
                    op.data == "simdStore" || op.data == "simdStoreAligned") {
                    for (unsigned s = 0; s < fn.slots.size(); ++s) {
                        invalidateSlot(env, s);
                        if (fn.slots[s] != Type::Int && fn.slots[s] != Type::Bool) env.diffs.forgetLen(s);
                    }
                    break;
                }
                [[fallthrough]];
            case Op::DynCall:
            case Op::ClosureCall:
                for (unsigned ms : op.mutArgSlots) {
                    if (ms < fn.slots.size()) {
                        invalidateSlot(env, ms);
                        if (fn.slots[ms] != Type::Int && fn.slots[ms] != Type::Bool) env.diffs.forgetLen(ms);
                    }
                }
                break;
            case Op::ExternCall:
                for (unsigned s = 0; s < fn.slots.size(); ++s) {
                    invalidateSlot(env, s);
                    if (fn.slots[s] != Type::Int && fn.slots[s] != Type::Bool) env.diffs.forgetLen(s);
                }
                break;
            case Op::Call:
                for (unsigned ms : op.mutArgSlots) {
                    if (ms < fn.slots.size()) {
                        invalidateSlot(env, ms);
                        if (fn.slots[ms] != Type::Int && fn.slots[ms] != Type::Bool) env.diffs.forgetLen(ms);
                    }
                }
                if (auto it = fnReturnRanges.find(op.data); it != fnReturnRanges.end()) {
                    env.valRanges[op.result.id] = it->second;
                }
                break;
            case Op::Assert:
                if (!op.inputs.empty()) {
                    if (!op.boundsProven && proveConditionTrue(op.inputs[0], env)) {
                        op.boundsProven = true;
                        if (op.proofKind == ProofKind::None) {
                            op.proofKind = usesContractFact(op, env) ? ProofKind::ContractRequires : ProofKind::DiffGraph;
                        }
                    }
                    refineFromCondition(env, op.inputs[0], true, op.data == "requires");
                }
                break;
            case Op::Negate:
                if (op.inputs[0].type != Type::Int) break;
                if (auto cr = rangeOf(op.inputs[0], env)) {
                    if (auto nr = ReturnRanges::negateRange(*cr)) {
                        op.overflowProven = true;
                        if (op.proofKind == ProofKind::None) {
                            op.proofKind = usesContractFact(op, env) ? ProofKind::ContractRequires
                                : (cr->lower == cr->upper ? ProofKind::ConstRange : ProofKind::DiffGraph);
                        }
                        env.valRanges[op.result.id] = *nr;
                        if (fitsSigned16(*cr) && fitsSigned16(*nr)) op.width = 16;
                        else if (fitsSigned32(*cr) && fitsSigned32(*nr)) op.width = 32;
                    } else if (cr->lower == kMinInt && cr->upper <= 0 && cr->upper > kMinInt) {
                        // Post-trap checked negate guarantees x != INT64_MIN, so -x >= -cr->upper.
                        env.valRanges[op.result.id] = IntRange{-cr->upper, kMaxInt};
                    }
                }
                break;
            case Op::Min:
            case Op::Max: {
                if (op.result.type != Type::Int) break;
                auto lr = rangeOf(op.inputs[0], env);
                auto rr = rangeOf(op.inputs[1], env);
                if (lr && rr) {
                    env.valRanges[op.result.id] = (op.op == Op::Min)
                        ? IntRange{std::min(lr->lower, rr->lower), std::min(lr->upper, rr->upper)}
                        : IntRange{std::max(lr->lower, rr->lower), std::max(lr->upper, rr->upper)};
                    if (lr->lower >= 0 && rr->lower >= 0) op.elemUnsigned = true;
                    if ((op.elemUnsigned && lr->upper <= 65535 && rr->upper <= 65535) ||
                        (fitsSigned16(*lr) && fitsSigned16(*rr))) op.width = 16;
                    else if ((op.elemUnsigned && fitsUnsigned32(*lr) && fitsUnsigned32(*rr)) ||
                             (fitsSigned32(*lr) && fitsSigned32(*rr))) op.width = 32;
                }
                break;
            }
            case Op::AbsDiff: {
                if (op.result.type != Type::Int) break;
                auto lr = rangeOf(op.inputs[0], env);
                auto rr = rangeOf(op.inputs[1], env);
                if (lr && rr) {
                    auto d1 = checkedSub64(lr->upper, rr->lower);
                    auto d2 = checkedSub64(rr->upper, lr->lower);
                    if (d1 && d2) {
                        op.overflowProven = true;
                        if (op.proofKind == ProofKind::None) {
                            op.proofKind = usesContractFact(op, env) ? ProofKind::ContractRequires : ProofKind::DiffGraph;
                        }
                        std::int64_t hi = std::max(*d1, *d2);
                        std::int64_t lo = 0;
                        if (lr->lower > rr->upper) lo = lr->lower - rr->upper;
                        else if (rr->lower > lr->upper) lo = rr->lower - lr->upper;
                        const IntRange resR{lo, hi};
                        env.valRanges[op.result.id] = resR;
                        if (fitsSigned16(*lr) && fitsSigned16(*rr) && fitsSigned16(resR)) op.width = 16;
                        else if (fitsSigned32(*lr) && fitsSigned32(*rr) && fitsSigned32(resR)) op.width = 32;
                    }
                }
                break;
            }
            case Op::Select: {
                if (op.result.type == Type::Int && op.inputs.size() == 3) {
                    auto tr = rangeOf(op.inputs[1], env);
                    auto fr = rangeOf(op.inputs[2], env);
                    if (tr && fr) {
                        const IntRange envR = ReturnRanges::envelope(*tr, *fr);
                        env.valRanges[op.result.id] = envR;
                        if (fitsSigned16(envR)) op.width = 16;
                        else if (fitsSigned32(envR)) op.width = 32;
                    }
                }
                break;
            }
            case Op::Binary: {
                if (op.data == "rangeIncrement") {
                    op.overflowProven = true;
                    if (auto lr = rangeOf(op.inputs[0], env)) {
                        if (lr->upper < kMaxInt)
                            env.valRanges[op.result.id] = IntRange{lr->lower < kMaxInt ? lr->lower + 1 : kMaxInt, lr->upper + 1};
                    }
                    break;
                }
                if (op.inputs[0].type != Type::Int || op.inputs[1].type != Type::Int) break;
                auto lr = rangeOf(op.inputs[0], env);
                auto rr = rangeOf(op.inputs[1], env);
                if (op.data == "+" || op.data == "-" || op.data == "*") {
                    if (lr && rr) {
                        std::optional<IntRange> out;
                        if (op.data == "+") out = ReturnRanges::addRanges(*lr, *rr);
                        else if (op.data == "-") {
                            out = ReturnRanges::subtractRanges(*lr, *rr);
                            if (out) {
                                // Tighten `(a [+ c]) - b` using DiffGraph `b - a <= d => (a + c) - b >= c - d`
                                std::int64_t lCst = 0;
                                Value lBase = op.inputs[0];
                                if (auto ld = defOp.find(lBase.id); ld != defOp.end() && ld->second->op == Op::Binary && ld->second->data == "+") {
                                    if (auto c = parseConstInt(defOp.count(ld->second->inputs[1].id) ? defOp.at(ld->second->inputs[1].id)->data : "")) {
                                        lCst = *c;
                                        lBase = ld->second->inputs[0];
                                    }
                                }
                                auto als = loadSlot.find(lBase.id);
                                auto bls = loadSlot.find(op.inputs[1].id);
                                if (als != loadSlot.end() && bls != loadSlot.end() &&
                                    (!slotWritable[als->second] || isCurrentLoad(lBase.id, als->second, env)) &&
                                    (!slotWritable[bls->second] || isCurrentLoad(op.inputs[1].id, bls->second, env))) {
                                    if (auto d = env.diffs.maxDiff({false, bls->second}, {false, als->second})) {
                                        if (auto lb = checkedSub64(lCst, *d)) {
                                            if (*lb <= out->upper) out->lower = std::max(out->lower, *lb);
                                        }
                                    }
                                }
                            }
                        } else out = ReturnRanges::multiplyRanges(*lr, *rr);
                        // Self-quotient decrement identity: for a >= 0, d >= 1, 0 <= a - (a / d) <= a
                        if (!out && op.data == "-" && lr && lr->lower >= 0) {
                            if (auto dSub = defOp.find(op.inputs[1].id); dSub != defOp.end() &&
                                dSub->second->op == Op::Binary && dSub->second->data == "/") {
                                auto ls0 = loadSlot.find(op.inputs[0].id);
                                auto ls1 = loadSlot.find(dSub->second->inputs[0].id);
                                auto dr = rangeOf(dSub->second->inputs[1], env);
                                if (ls0 != loadSlot.end() && ls1 != loadSlot.end() && ls0->second == ls1->second &&
                                    (!slotWritable[ls0->second] ||
                                     (isCurrentLoad(op.inputs[0].id, ls0->second, env) &&
                                      isCurrentLoad(dSub->second->inputs[0].id, ls1->second, env))) &&
                                    dr && dr->lower >= 1) {
                                    out = IntRange{0, lr->upper};
                                }
                            }
                        }
                        if (out) {
                            op.overflowProven = true;
                            if (op.proofKind == ProofKind::None) {
                                op.proofKind = usesContractFact(op, env) ? ProofKind::ContractRequires
                                    : ((lr->lower == lr->upper && rr->lower == rr->upper) ? ProofKind::ConstRange : ProofKind::DiffGraph);
                            }
                            env.valRanges[op.result.id] = *out;
                        }
                    }
                    if (!op.overflowProven && op.data == "+" && cfg.loopOfBlock[b] >= 0) {
                        if (auto sit = sentinelLoops.find(static_cast<std::size_t>(cfg.loopOfBlock[b]));
                            sit != sentinelLoops.end() && b != cfg.loops[sit->first].header) {
                            auto jl = loadSlot.find(op.inputs[0].id);
                            auto rc = parseConstInt(defOp.count(op.inputs[1].id) ? defOp.at(op.inputs[1].id)->data : "");
                            if (jl != loadSlot.end() && jl->second == sit->second.jSlot && rc == 1) {
                                op.overflowProven = true;
                                if (op.proofKind == ProofKind::None) op.proofKind = ProofKind::SentinelLoop;
                                env.valRanges[op.result.id] = IntRange{1, maxArrayLength};
                            }
                        }
                    }
                    if (!op.overflowProven) {
                        if (auto full = buildSym(op.result.id, env); full && full->isPureLinear()) {
                            if (auto sr = evalSymRange(*full, env)) {
                                op.overflowProven = true;
                                if (op.proofKind == ProofKind::None) {
                                    op.proofKind = usesContractFact(op, env) ? ProofKind::ContractRequires : ProofKind::DiffGraph;
                                }
                                env.valRanges[op.result.id] = *sr;
                            }
                        } else if (full && full->hasProduct) {
                            auto ra = env.diffs.rangeOf({false, full->prodSlotA});
                            auto rb = env.diffs.rangeOf({false, full->prodSlotB});
                            if (ra && rb && ra->lower >= -4096 && ra->upper <= 4096 &&
                                rb->lower >= -4096 && rb->upper <= 4096) {
                                if (auto sr = evalSymRange(*full, env)) {
                                    op.overflowProven = true;
                                    if (op.proofKind == ProofKind::None) {
                                        op.proofKind = usesContractFact(op, env) ? ProofKind::ContractRequires : ProofKind::DiffGraph;
                                    }
                                    env.valRanges[op.result.id] = *sr;
                                }
                            }
                        }
                    }
                    if (op.overflowProven && lr && rr) {
                        if (auto rit = env.valRanges.find(op.result.id); rit != env.valRanges.end()) {
                            if (fitsSigned16(*lr) && fitsSigned16(*rr) && fitsSigned16(rit->second))
                                op.width = 16;
                            else if (fitsSigned32(*lr) && fitsSigned32(*rr) && fitsSigned32(rit->second))
                                op.width = 32;
                        }
                    }
                } else if (op.data == "/" || op.data == "%") {
                    if (rr) {
                        const bool posDiv = rr->lower >= 1;
                        const bool negDiv = rr->upper <= -1 && ((lr && lr->lower > kMinInt) || rr->upper < -1);
                        if (posDiv || negDiv) {
                            op.divSafe = true;
                            if (posDiv && lr && lr->lower >= 0) op.unsignedDiv = true;
                            if (op.proofKind == ProofKind::None) {
                                op.proofKind = usesContractFact(op, env) ? ProofKind::ContractRequires
                                    : (rr->lower == rr->upper ? ProofKind::ConstRange : ProofKind::DiffGraph);
                            }
                        }
                        if (op.data == "/") {
                            if (lr && (posDiv || negDiv)) {
                                if (auto q = ReturnRanges::divideRanges(*lr, *rr))
                                    env.valRanges[op.result.id] = *q;
                            }
                        } else {
                            if (auto r = ReturnRanges::remainderRanges(lr, *rr))
                                env.valRanges[op.result.id] = *r;
                        }
                        if (op.data == "%" && op.divSafe && rr->lower >= 1 && lr && lr->lower >= 0) {
                            if (lr->upper < rr->lower) {
                                op.modNoOp = true;
                                op.modSubSteps = 0;
                            } else if (auto twice = checkedMul64(2, rr->lower); twice && lr->upper < *twice) {
                                op.modSubSteps = 1;
                                op.modNoOp = false;
                            } else if (auto dls = loadSlot.find(op.inputs[1].id);
                                       dls != loadSlot.end() &&
                                       (!slotWritable[dls->second] || isCurrentLoad(op.inputs[1].id, dls->second, env))) {
                                const unsigned mSlot = dls->second;
                                std::vector<Value> addTerms;
                                auto collectAdd = [&](auto& self, Value v, int depth) -> bool {
                                    if (depth > 4) return false;
                                    auto dit = defOp.find(v.id);
                                    if (dit != defOp.end() && dit->second->op == Op::Binary && dit->second->data == "+") {
                                        return self(self, dit->second->inputs[0], depth + 1) &&
                                               self(self, dit->second->inputs[1], depth + 1);
                                    }
                                    if (addTerms.size() >= 6) return false;
                                    addTerms.push_back(v);
                                    return true;
                                };
                                auto isSlotLtM = [&](Value v, std::optional<unsigned>& outSlot) -> bool {
                                    auto vls = loadSlot.find(v.id);
                                    if (vls == loadSlot.end()) return false;
                                    if (slotWritable[vls->second] && !isCurrentLoad(v.id, vls->second, env)) return false;
                                    auto vr = env.diffs.rangeOf({false, vls->second});
                                    auto md = env.diffs.maxDiff({false, vls->second}, {false, mSlot});
                                    if (vr && vr->lower >= 0 && md && *md <= -1) {
                                        outSlot = vls->second;
                                        return true;
                                    }
                                    return false;
                                };
                                if (collectAdd(collectAdd, op.inputs[0], 0)) {
                                    if (addTerms.size() == 2) {
                                        std::optional<unsigned> s0, s1;
                                        if (isSlotLtM(addTerms[0], s0) && isSlotLtM(addTerms[1], s1)) {
                                            op.modSubSteps = 1;
                                            op.modNoOp = false;
                                        }
                                    }
                                    if (op.modSubSteps == 0) {
                                        std::int64_t kMax = 0;
                                        bool foundScaled = false, okSym = true;
                                        std::int64_t cLo = 0, cHi = 0;
                                        for (const Value& t : addTerms) {
                                            std::optional<unsigned> baseS;
                                            if (!foundScaled && isSlotLtM(t, baseS)) {
                                                foundScaled = true; kMax = 1; continue;
                                            }
                                            if (!foundScaled) {
                                                auto td = defOp.find(t.id);
                                                if (td != defOp.end() && td->second->op == Op::Binary && td->second->data == "*") {
                                                    std::optional<IntRange> kr;
                                                    if (isSlotLtM(td->second->inputs[0], baseS)) kr = rangeOf(td->second->inputs[1], env);
                                                    else if (isSlotLtM(td->second->inputs[1], baseS)) kr = rangeOf(td->second->inputs[0], env);
                                                    if (baseS && kr && kr->lower >= 0 && kr->upper <= 3) {
                                                        foundScaled = true; kMax = kr->upper; continue;
                                                    }
                                                }
                                            }
                                            auto tr = rangeOf(t, env);
                                            if (!tr || tr->lower < 0) { okSym = false; break; }
                                            auto nLo = checkedAdd64(cLo, tr->lower), nHi = checkedAdd64(cHi, tr->upper);
                                            if (!nLo || !nHi) { okSym = false; break; }
                                            cLo = *nLo; cHi = *nHi;
                                        }
                                        if (okSym && foundScaled) {
                                            if (kMax <= 1 && cHi <= rr->lower) {
                                                op.modSubSteps = 1; op.modNoOp = false;
                                            } else if (kMax >= 2 && kMax <= 3 && cHi <= kMax - 1) {
                                                op.modSubSteps = static_cast<std::uint8_t>(kMax - 1);
                                                op.modNoOp = false;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (op.divSafe && lr) {
                            if (op.unsignedDiv && rr && lr->upper <= 65535 && rr->upper <= 65535 &&
                                op.modSubSteps == 0) {
                                op.width = 16;
                            } else if (op.unsignedDiv && rr && fitsUnsigned32(*lr) && fitsUnsigned32(*rr)) {
                                op.width = 32;
                            } else if (rr && fitsSigned32(*lr) && fitsSigned32(*rr) &&
                                       (lr->lower > std::numeric_limits<std::int32_t>::min() ||
                                        rr->upper < -1 || rr->lower > -1)) {
                                op.width = 32;
                                op.unsignedDiv = false;
                            }
                            if (op.unsignedDiv && lr->lower >= 0) op.lhsUpper = lr->upper;
                        } else if (!op.divSafe && (op.data == "/" || op.data == "%") &&
                                   lr && lr->lower >= 0 && rr && rr->lower >= 0 && rr->upper >= 1) {
                            // Reaching past @tz.div/@tz.rem guarantees divisor != 0 (hence >= 1 since rr->lower >= 0).
                            if (op.data == "/") {
                                if (auto q = ReturnRanges::divideRanges(*lr, IntRange{1, rr->upper}))
                                    env.valRanges[op.result.id] = *q;
                            } else {
                                if (auto r = ReturnRanges::remainderRanges(*lr, IntRange{1, rr->upper}))
                                    env.valRanges[op.result.id] = *r;
                            }
                            if (auto rls = loadSlot.find(op.inputs[1].id); rls != loadSlot.end() &&
                                (!slotWritable[rls->second] || isCurrentLoad(op.inputs[1].id, rls->second, env))) {
                                env.diffs.addLower({false, rls->second}, 1);
                                if (op.data == "%") env.remValLtSlot[op.result.id] = rls->second;
                            }
                        }
                        if (op.data == "%" && lr && lr->lower >= 0 && rr && rr->lower >= 1) {
                            if (auto rls = loadSlot.find(op.inputs[1].id); rls != loadSlot.end() &&
                                (!slotWritable[rls->second] || isCurrentLoad(op.inputs[1].id, rls->second, env))) {
                                env.remValLtSlot[op.result.id] = rls->second;
                            }
                        }
                        // Mean-value theorem for counted loop sums: (sum_{0..N-1} a_i) / N in [sLo, sHi]
                        if (op.data == "/") {
                            auto lls = loadSlot.find(op.inputs[0].id);
                            auto rls = loadSlot.find(op.inputs[1].id);
                            if (lls != loadSlot.end() && rls != loadSlot.end() && !slotWritable[rls->second] &&
                                isCurrentLoad(op.inputs[0].id, lls->second, env)) {
                                if (auto msIt = env.meanSources.find(lls->second);
                                    msIt != env.meanSources.end() && msIt->second.first == rls->second) {
                                    const IntRange& sr = msIt->second.second;
                                    IntRange meanR = (rr->lower >= 1)
                                        ? sr
                                        : IntRange{std::min<std::int64_t>(0, sr.lower), std::max<std::int64_t>(0, sr.upper)};
                                    auto curIt = env.valRanges.find(op.result.id);
                                    env.valRanges[op.result.id] = (curIt == env.valRanges.end())
                                        ? meanR : *intersectRange(curIt->second, meanR);
                                }
                            }
                        }
                    }
                    if (!op.divSafe && lr && lr->lower > kMinInt) {
                        if (auto rls = loadSlot.find(op.inputs[1].id); rls != loadSlot.end() &&
                            (!slotWritable[rls->second] || isCurrentLoad(op.inputs[1].id, rls->second, env)) &&
                            env.nonZeroSlots.contains(rls->second)) {
                            op.divSafe = true;
                            if (op.proofKind == ProofKind::None) {
                                op.proofKind = env.contractSlots.contains(rls->second) ? ProofKind::ContractRequires : ProofKind::DiffGraph;
                            }
                        }
                    }
                } else if (op.data == "<<" || op.data == ">>" || op.data == ">>>") {
                    if (rr && rr->lower >= 0 && rr->upper <= 63) {
                        op.overflowProven = true;
                        if (op.proofKind == ProofKind::None) {
                            op.proofKind = usesContractFact(op, env) ? ProofKind::ContractRequires
                                : (rr->lower == rr->upper ? ProofKind::ConstRange : ProofKind::DiffGraph);
                        }
                    }
                } else if (op.data == "&") {
                    if (lr && lr->lower >= 0 && rr && rr->lower >= 0) {
                        env.valRanges[op.result.id] = IntRange{0, std::min(lr->upper, rr->upper)};
                    } else if (lr && lr->lower >= 0) {
                        env.valRanges[op.result.id] = IntRange{0, lr->upper};
                    } else if (rr && rr->lower >= 0) {
                        env.valRanges[op.result.id] = IntRange{0, rr->upper};
                    }
                } else if (op.data == "==" || op.data == "!=" || op.data == "<" ||
                           op.data == "<=" || op.data == ">" || op.data == ">=") {
                    if (lr && rr) {
                        if (fitsSigned16(*lr) && fitsSigned16(*rr)) op.width = 16;
                        else if (fitsSigned32(*lr) && fitsSigned32(*rr)) op.width = 32;
                    }
                }
                break;
            }
            case Op::Index:
            case Op::IndexSet: {
                if (!op.inputs.empty() && op.inputs[0].type != Type::Str) {
                    if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end()) {
                        const unsigned arrSlot = ls->second;
                        ProofKind pk = ProofKind::None;
                        if (!op.boundsProven && proveInBounds(b, arrSlot, op.inputs[1], env, &pk)) {
                            op.boundsProven = true;
                            if (op.proofKind == ProofKind::None) {
                                if (pk == ProofKind::SentinelLoop || pk == ProofKind::ModuloBound) op.proofKind = pk;
                                else if (usesContractFact(op, env)) op.proofKind = ProofKind::ContractRequires;
                                else op.proofKind = pk;
                            }
                        }
                        if (op.op == Op::Index && op.result.type == Type::Int && fn.slots[arrSlot] == Type::IntArray) {
                            if (auto git = globalElemRanges.find(arrSlot); git != globalElemRanges.end() && git->second)
                                env.valRanges[op.result.id] = *git->second;
                            if (auto ltIt = arrElemLtSlot.find(arrSlot); ltIt != arrElemLtSlot.end()) {
                                env.idxElemLtSlot[op.result.id] = ltIt->second;
                                if (auto nr = env.diffs.rangeOf({false, ltIt->second}); nr && nr->upper >= 1) {
                                    IntRange r{0, nr->upper - 1};
                                    auto cur = env.valRanges.find(op.result.id);
                                    env.valRanges[op.result.id] = (cur == env.valRanges.end())
                                        ? r : *intersectRange(cur->second, r);
                                }
                            }
                        }
                    }
                }
                break;
            }
            default: break;
            }
            if (op.result.type == Type::Int) {
                if (auto r = rangeOf(op.result, env)) {
                    auto it = observedValRanges.find(op.result.id);
                    observedValRanges[op.result.id] = (it == observedValRanges.end())
                        ? *r : ReturnRanges::envelope(it->second, *r);
                }
            }
        }
        if (blk.exit == Exit::Return && blk.value && blk.value->type == Type::Int) {
            if (auto r = rangeOf(*blk.value, env)) {
                auto it = observedValRanges.find(blk.value->id);
                observedValRanges[blk.value->id] = (it == observedValRanges.end())
                    ? *r : ReturnRanges::envelope(it->second, *r);
            }
        }

        for (unsigned child : cfg.domTree[b]) {
            if (visited[child]) continue;
            Env childEnv = env;
            if (auto lit = cfg.headerToLoop.find(child); lit != cfg.headerToLoop.end()) {
                const std::size_t li = lit->second;
                auto recordPre = [&](unsigned s) {
                    if (auto preAcc = env.diffs.rangeOf({false, s})) {
                        auto [it, ins] = loopPreAccRanges.emplace(std::pair{li, s}, *preAcc);
                        if (!ins) it->second = ReturnRanges::envelope(it->second, *preAcc);
                    }
                };
                if (auto rIt = loopModOps.find(li); rIt != loopModOps.end()) {
                    for (const auto& mp : rIt->second) {
                        recordPre(mp.slot);
                        if (auto dls = loadSlot.find(mp.divVal.id);
                            dls != loadSlot.end() && !loopWritten[li].contains(dls->second)) {
                            const unsigned dslot = dls->second;
                            bool ltOk = false;
                            if (auto d = env.diffs.maxDiff({false, mp.slot}, {false, dslot}); d && *d <= -1)
                                ltOk = true;
                            else if (auto pr = env.diffs.rangeOf({false, mp.slot}),
                                     dr = env.diffs.rangeOf({false, dslot});
                                     pr && dr && pr->upper < dr->lower)
                                ltOk = true;
                            if (ltOk) loopPreModLtDiv[{li, mp.slot}].insert(dslot);
                        }
                    }
                }
                if (uniqueZeroSetLoops.contains(li)) {
                    for (unsigned s : loopWritten[li]) {
                        if (s < fn.slots.size() && fn.slots[s] == Type::Int) recordPre(s);
                    }
                }
                if (auto cpIt = loopCopyOps.find(li); cpIt != loopCopyOps.end()) {
                    for (const auto& cp : cpIt->second) recordPre(cp.dstSlot);
                }
                if (auto dsIt = divShrinkLoops.find(li); dsIt != divShrinkLoops.end()) {
                    recordPre(dsIt->second.slot);
                }
                if (auto fhIt = forHeaders.find(child); fhIt != forHeaders.end() && fhIt->second.valid) {
                    auto preLo = rangeOf(fhIt->second.lower, env);
                    auto preHi = rangeOf(fhIt->second.upper, env);
                    if (preLo && preHi && preHi->upper <= maxArrayLength * 4) {
                        if (auto d = checkedSub64(preHi->upper, preLo->lower)) {
                            const std::int64_t trips = std::max<std::int64_t>(0, *d);
                            if (trips <= maxArrayLength * 4) {
                                auto [it, ins] = loopMaxTrips.emplace(li, trips);
                                if (!ins) it->second = std::max(it->second, trips);
                            }
                        }
                    }
                    if (preLo && preHi && preHi->upper > preLo->lower) {
                        const IntRange ir{preLo->lower, preHi->upper - 1};
                        auto [it, ins] = loopIterRanges.emplace(li, ir);
                        if (!ins) it->second = ReturnRanges::envelope(it->second, ir);
                    }
                    if (auto accIt = loopAccOps.find(li); accIt != loopAccOps.end()) {
                        for (const auto& [accSlot, stepV, isSub] : accIt->second) recordPre(accSlot);
                    }
                    if (auto maIt = loopMultiAccOps.find(li); maIt != loopMultiAccOps.end()) {
                        for (const auto& ma : maIt->second) recordPre(ma.slot);
                    }
                    if (auto mIt = loopMaxClampOps.find(li); mIt != loopMaxClampOps.end()) {
                        for (const auto& mc : mIt->second) recordPre(mc.slot);
                    }
                    if (auto cIt = loopCoupledMaxOps.find(li); cIt != loopCoupledMaxOps.end()) {
                        for (const auto& cp : cIt->second) {
                            recordPre(cp.prevSlot);
                            recordPre(cp.bestSlot);
                        }
                    }
                    if (auto rIt = loopRunningExtremeOps.find(li); rIt != loopRunningExtremeOps.end()) {
                        for (const auto& re : rIt->second) recordPre(re.slot);
                    }
                    // Capture outer pre-loop values for accumulators updated inside direct nested small loops
                    for (std::size_t sub = 0; sub < cfg.loops.size(); ++sub) {
                        if (sub == li || !cfg.loops[li].body.contains(cfg.loops[sub].header)) continue;
                        auto sRes = smallLoopResults.find(sub);
                        if (sRes == smallLoopResults.end()) continue;
                        for (const auto& [as, dr] : sRes->second.accDelta) { (void)dr; recordPre(as); }
                    }
                    // Bounded-trip abstract simulation for small pure For loops (<= 32 iterations)
                    const auto& fh = fhIt->second;
                    const auto& lp = cfg.loops[li];
                    if (lp.body.size() == 2 && fn.blocks[fh.bodyBlock].exit == Exit::Jump &&
                        fn.blocks[fh.bodyBlock].yes == child && cfg.loopOfBlock[fh.bodyBlock] == static_cast<int>(li) &&
                        preLo && preHi && preLo->lower == preLo->upper && preHi->upper > preLo->lower &&
                        preHi->upper - preLo->lower <= 32) {
                        bool pureBody = true;
                        for (const auto& op : fn.blocks[fh.bodyBlock].ops) {
                            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr ||
                                op.op == Op::Builtin || op.op == Op::BoxDyn || op.op == Op::DynCall ||
                                op.op == Op::MakeClosure || op.op == Op::ClosureCall || op.op == Op::Print ||
                                op.op == Op::IndexSet || op.op == Op::ArrayLit || op.op == Op::Interp ||
                                op.op == Op::Slice || op.op == Op::FieldSet || op.op == Op::PlaceSet ||
                                op.op == Op::RepeatHeap || op.op == Op::RepeatRegion || op.op == Op::Clone ||
                                op.op == Op::ClearOwner || op.op == Op::ClearField || op.op == Op::ClearIndex) {
                                pureBody = false;
                                break;
                            }
                        }
                        if (pureBody) {
                            const std::int64_t cLo = preLo->lower;
                            const std::int64_t maxTrips = preHi->upper - cLo;
                            SmallLoopResult simRes;
                            std::unordered_map<unsigned, IntRange> curSR;
                            for (std::size_t s = 0; s < fn.slots.size(); ++s) {
                                if (fn.slots[s] != Type::Int) continue;
                                if (auto r = env.diffs.rangeOf({false, static_cast<unsigned>(s)}))
                                    curSR[static_cast<unsigned>(s)] = *r;
                            }
                            if (preHi->lower <= cLo) {
                                for (unsigned ws : loopWritten[li]) {
                                    if (ws != fh.iterSlot && curSR.count(ws)) {
                                        simRes.slotEnvelope[ws] = curSR[ws];
                                        simRes.slotPostEnvelope[ws] = curSR[ws];
                                    }
                                }
                            }
                            std::optional<std::pair<unsigned, std::int64_t>> uLin;
                            if (auto us = buildSym(fh.upper.id, env); us && us->isPureLinear() && us->linear.size() == 1) {
                                auto [s, c] = *us->linear.begin();
                                if (c == 1 && !loopWritten[li].contains(s)) uLin = std::pair{s, us->constant};
                            }
                            const auto origUR = uLin ? curSR.find(uLin->first) != curSR.end()
                                ? std::optional<IntRange>{curSR[uLin->first]} : std::nullopt : std::nullopt;
                            std::unordered_map<unsigned, std::optional<IntRange>> accSum;
                            for (const auto& op : fn.blocks[fh.bodyBlock].ops) {
                                if (op.op != Op::Store || op.slot == fh.iterSlot || op.inputs.empty()) continue;
                                auto dt = defOp.find(op.inputs[0].id);
                                if (dt != defOp.end() && dt->second->op == Op::Binary && dt->second->data == "+") {
                                    auto l0 = loadSlot.find(dt->second->inputs[0].id);
                                    auto l1 = loadSlot.find(dt->second->inputs[1].id);
                                    if (l0 != loadSlot.end() && l0->second == op.slot)
                                        accSum[op.slot] = IntRange{0, 0};
                                    else if (l1 != loadSlot.end() && l1->second == op.slot)
                                        accSum[op.slot] = IntRange{0, 0};
                                }
                            }
                            for (std::int64_t t = 0; t < maxTrips; ++t) {
                                const std::int64_t iterVal = cLo + t;
                                curSR[fh.iterSlot] = {iterVal, iterVal};
                                if (uLin && origUR) {
                                    const std::int64_t reqLo = std::max(origUR->lower, iterVal + 1 - uLin->second);
                                    if (reqLo > origUR->upper) break;
                                    curSR[uLin->first] = {reqLo, origUR->upper};
                                }
                                std::unordered_map<unsigned, IntRange> valR;
                                for (const auto& op : fn.blocks[fh.bodyBlock].ops) {
                                    auto getR = [&](const Value& v) -> std::optional<IntRange> {
                                        if (auto vit = valR.find(v.id); vit != valR.end()) return vit->second;
                                        if (auto rit = observedValRanges.find(v.id); rit != observedValRanges.end()) return rit->second;
                                        if (auto lit2 = defOp.find(v.id); lit2 != defOp.end() && lit2->second->op == Op::Int) {
                                            if (auto c = parseConstInt(lit2->second->data)) return IntRange{*c, *c};
                                        }
                                        return std::nullopt;
                                    };
                                    if (op.op == Op::Int && op.result.type == Type::Int) {
                                        if (auto c = parseConstInt(op.data)) valR[op.result.id] = {*c, *c};
                                    } else if (op.op == Op::Load && fn.slots[op.slot] == Type::Int) {
                                        if (auto sr = curSR.find(op.slot); sr != curSR.end()) valR[op.result.id] = sr->second;
                                    } else if (op.op == Op::Negate && op.inputs[0].type == Type::Int) {
                                        if (auto lr = getR(op.inputs[0]))
                                            if (auto nr = ReturnRanges::negateRange(*lr)) valR[op.result.id] = *nr;
                                    } else if ((op.op == Op::Min || op.op == Op::Max) && op.result.type == Type::Int) {
                                        auto r0 = getR(op.inputs[0]);
                                        auto r1 = getR(op.inputs[1]);
                                        if (r0 && r1) {
                                            valR[op.result.id] = (op.op == Op::Min)
                                                ? IntRange{std::min(r0->lower, r1->lower), std::min(r0->upper, r1->upper)}
                                                : IntRange{std::max(r0->lower, r1->lower), std::max(r0->upper, r1->upper)};
                                        }
                                    } else if (op.op == Op::Index && op.result.type == Type::Int &&
                                               op.inputs[0].type == Type::IntArray) {
                                        if (auto als = loadSlot.find(op.inputs[0].id); als != loadSlot.end()) {
                                            if (auto git = globalElemRanges.find(als->second);
                                                git != globalElemRanges.end() && git->second)
                                                valR[op.result.id] = *git->second;
                                        }
                                    } else if (op.op == Op::Binary && op.inputs[0].type == Type::Int &&
                                               op.inputs[1].type == Type::Int) {
                                        auto l0 = getR(op.inputs[0]);
                                        auto r0 = getR(op.inputs[1]);
                                        if (op.data == "rangeIncrement" && l0 && l0->upper < kMaxInt) {
                                            valR[op.result.id] = {l0->lower < kMaxInt ? l0->lower + 1 : kMaxInt, l0->upper + 1};
                                        } else if (l0 && r0) {
                                            std::optional<IntRange> outR;
                                            if (op.data == "+") outR = ReturnRanges::addRanges(*l0, *r0);
                                            else if (op.data == "-") outR = ReturnRanges::subtractRanges(*l0, *r0);
                                            else if (op.data == "*") outR = ReturnRanges::multiplyRanges(*l0, *r0);
                                            else if (op.data == "/" && (r0->lower >= 1 || r0->upper <= -1))
                                                outR = ReturnRanges::divideRanges(*l0, *r0);
                                            else if (op.data == "%") outR = ReturnRanges::remainderRanges(l0, *r0);
                                            if (outR) valR[op.result.id] = *outR;
                                        }
                                    } else if (op.op == Op::Store && fn.slots[op.slot] == Type::Int && !op.inputs.empty()) {
                                        if (auto asIt = accSum.find(op.slot); asIt != accSum.end() && asIt->second) {
                                            auto dt = defOp.find(op.inputs[0].id);
                                            if (dt != defOp.end() && dt->second->op == Op::Binary && dt->second->data == "+") {
                                                auto l0 = loadSlot.find(dt->second->inputs[0].id);
                                                Value other = (l0 != loadSlot.end() && l0->second == op.slot)
                                                    ? dt->second->inputs[1] : dt->second->inputs[0];
                                                if (auto dr = getR(other)) {
                                                    if (auto nxt = ReturnRanges::addRanges(*asIt->second, *dr))
                                                        asIt->second = nxt;
                                                    else asIt->second.reset();
                                                } else asIt->second.reset();
                                            }
                                        }
                                        if (auto vr = getR(op.inputs[0])) {
                                            curSR[op.slot] = *vr;
                                            auto itEnv = simRes.slotEnvelope.find(op.slot);
                                            simRes.slotEnvelope[op.slot] = (itEnv == simRes.slotEnvelope.end())
                                                ? *vr : ReturnRanges::envelope(itEnv->second, *vr);
                                            if (iterVal + 1 >= preHi->lower) {
                                                auto itPost = simRes.slotPostEnvelope.find(op.slot);
                                                simRes.slotPostEnvelope[op.slot] = (itPost == simRes.slotPostEnvelope.end())
                                                    ? *vr : ReturnRanges::envelope(itPost->second, *vr);
                                            }
                                        } else {
                                            curSR.erase(op.slot);
                                        }
                                    }
                                }
                            }
                            for (const auto& [as, sr] : accSum) {
                                if (!sr) continue;
                                IntRange r = *sr;
                                if (preHi->lower <= cLo) r = ReturnRanges::envelope(r, IntRange{0, 0});
                                simRes.accDelta[as] = r;
                            }
                            if (!simRes.slotEnvelope.empty()) smallLoopResults[li] = std::move(simRes);
                        }
                    }
                }
                if (auto bsIt = binarySearchLoops.find(li); bsIt != binarySearchLoops.end()) {
                    auto preLo = env.diffs.rangeOf({false, bsIt->second.loSlot});
                    auto preHi = env.diffs.rangeOf({false, bsIt->second.hiSlot});
                    if (preLo && preLo->lower >= 0 && preHi && preHi->upper <= maxArrayLength) {
                        bsLoopUpper[li] = preHi->upper;
                        for (std::size_t a = 0; a < fn.slots.size(); ++a) {
                            if (fn.slots[a] == Type::IntArray && !loopWritten[li].contains(static_cast<unsigned>(a))) {
                                if (auto d = env.diffs.maxDiff({false, bsIt->second.hiSlot}, {true, static_cast<unsigned>(a)}))
                                    bsLoopArrDiffs[li].push_back({static_cast<unsigned>(a), *d});
                            }
                        }
                    }
                }
                if (auto sqIt = sqrtLoops.find(li); sqIt != sqrtLoops.end()) {
                    auto preD = env.diffs.rangeOf({false, sqIt->second.dSlot});
                    auto bls0 = loadSlot.find(sqIt->second.boundVal.id);
                    auto preX = (bls0 != loadSlot.end())
                        ? env.diffs.rangeOf({false, bls0->second})
                        : rangeOf(sqIt->second.boundVal, env);
                    if (preD && preD->lower >= 1 && preD->upper <= 1000000000LL &&
                        preX && preX->lower >= 0 && preX->upper <= 1000000000LL) {
                        const std::int64_t maxD = std::max(
                            preD->upper,
                            static_cast<std::int64_t>(std::sqrt(static_cast<double>(preX->upper))) + 1);
                        sqrtLoopUpper[li] = maxD;
                        loopMaxTrips[li] = std::max<std::int64_t>(0, maxD - preD->lower + 1);
                        if (bls0 != loadSlot.end() && loopWritten[li].contains(bls0->second)) {
                            sqrtLoopBoundRange[li] = {bls0->second, IntRange{0, preX->upper}};
                        }
                    }
                }
                if (auto hrIt = heronLoops.find(li); hrIt != heronLoops.end()) {
                    auto ar = rangeOf(hrIt->second.aVal, env);
                    auto xr = env.diffs.rangeOf({false, hrIt->second.xSlot});
                    auto yr = env.diffs.rangeOf({false, hrIt->second.ySlot});
                    if (ar && xr && yr && ar->lower >= 1 && xr->lower >= 1 && yr->lower >= 1 &&
                        ar->upper <= kMaxInt / 2 && xr->upper <= ar->upper && yr->upper <= ar->upper) {
                        activeHeronRanges[li] = IntRange{1, ar->upper};
                    }
                }
                if (auto euIt = euclidLoops.find(li); euIt != euclidLoops.end()) {
                    auto ar = env.diffs.rangeOf({false, euIt->second.aSlot});
                    auto br = env.diffs.rangeOf({false, euIt->second.bSlot});
                    if (ar && br && ar->lower >= 0 && br->lower >= 0 &&
                        ar->upper < kMaxInt && br->upper < kMaxInt) {
                        activeEuclidUpper[li] = {std::max(ar->upper, br->upper), br->upper};
                    }
                }
                if (auto bsSelIt = boundedSelectorLoops.find(li); bsSelIt != boundedSelectorLoops.end()) {
                    for (const auto& bs : bsSelIt->second) {
                        auto sr = env.diffs.rangeOf({false, bs.slot});
                        auto d = env.diffs.maxDiff({false, bs.slot}, {false, bs.boundSlot});
                        if (sr && sr->lower >= 0 && d && *d <= -1)
                            activeBoundedSelectors[li].push_back(bs);
                    }
                }
                if (auto dsIt = divShrinkLoops.find(li); dsIt != divShrinkLoops.end()) {
                    auto preX = env.diffs.rangeOf({false, dsIt->second.slot});
                    std::int64_t minD = dsIt->second.minConstDiv;
                    if (dsIt->second.divSlot) {
                        minD = 0;
                        const unsigned dS = *dsIt->second.divSlot;
                        if (auto dr = env.diffs.rangeOf({false, dS}); dr && dr->lower >= 2) minD = dr->lower;
                    }
                    if (preX && preX->lower >= 0 && preX->upper < kMaxInt && minD >= 2) {
                        divShrinkBoundRange[li] = {dsIt->second.slot, IntRange{0, preX->upper}};
                        std::int64_t v = preX->upper, trips = 0;
                        while (v > 0) { v /= minD; ++trips; }
                        auto [itT, insT] = loopMaxTrips.emplace(li, trips);
                        if (!insT) itT->second = std::min(itT->second, trips);
                    }
                }
                if (auto gcIt = guardedCounterLoops.find(li); gcIt != guardedCounterLoops.end()) {
                    for (const auto& gc : gcIt->second) {
                        if (auto d = env.diffs.maxDiff({false, gc.slot}, {false, gc.boundSlot}); d && *d <= 0)
                            activeGuardedCounters[li].push_back(gc);
                    }
                }
                if (auto twIt = twoWayPartitionLoops.find(li); twIt != twoWayPartitionLoops.end()) {
                    const auto& tw = twIt->second;
                    auto rL = env.diffs.rangeOf({false, tw.leftSlot});
                    auto rR = env.diffs.rangeOf({false, tw.rightSlot});
                    auto rB = env.diffs.rangeOf({false, tw.boundSlot});
                    if (rL && rL->lower == 0 && rL->upper == 0 &&
                        rR && rR->lower == 0 && rR->upper == 0 && rB && rB->lower >= 0) {
                        activeTwoWayPartitions.insert(li);
                    }
                }
                if (auto uzIt = uniqueZeroSetLoops.find(li); uzIt != uniqueZeroSetLoops.end()) {
                    const auto& uz = uzIt->second;
                    auto rC = env.diffs.rangeOf({false, uz.countSlot});
                    auto rLen = env.diffs.rangeOf({true, uz.seenArr});
                    bool okH = true;
                    if (uz.headSlot) {
                        auto rH = env.diffs.rangeOf({false, *uz.headSlot});
                        okH = rH && rH->lower == 0 && rH->upper == 0;
                    }
                    if (rC && rC->lower == uz.initCount && rC->upper == uz.initCount &&
                        rLen && rLen->lower >= uz.initCount && rLen->upper <= maxArrayLength && okH) {
                        activeUniqueZeroMaxN[li] = rLen->upper;
                    }
                }
                auto slotIsNonIncreasing = [&](const MonotoneSlot& ms) -> bool {
                    if (ms.nonIncreasing) return true;
                    if (ms.decDivSlot) {
                        auto dr = env.diffs.rangeOf({false, *ms.decDivSlot});
                        const bool dSafe = dr && dr->lower >= 1 &&
                            (!loopWritten[li].contains(*ms.decDivSlot) ||
                             (sqrtLoops.contains(li) && sqrtLoops.at(li).dSlot == *ms.decDivSlot));
                        if (!dSafe) return false;
                        if (auto pr = env.diffs.rangeOf({false, ms.slot}); pr && pr->lower >= 0) return true;
                        return false;
                    }
                    if (ms.nonIncIfNonNeg) {
                        if (auto pr = env.diffs.rangeOf({false, ms.slot}); pr && pr->lower >= 0) return true;
                    }
                    if (ms.decBySlot) {
                        if (auto dr = env.diffs.rangeOf({false, *ms.decBySlot}); dr && dr->lower >= 0) return true;
                    }
                    return false;
                };
                // Capture pre-loop slot - ArrLen diffs for monotonically nonIncreasing slots (e.g. bubbleEarly's edge, ShellSort vJ)
                struct SavedArrDiff { unsigned ws; unsigned arrSlot; std::int64_t diff; };
                std::vector<SavedArrDiff> savedArrDiffs;
                for (const auto& ms : loopMonotone[li]) {
                    if (!slotIsNonIncreasing(ms)) continue;
                    for (std::size_t a = 0; a < fn.slots.size(); ++a) {
                        if (fn.slots[a] == Type::IntArray && !loopWritten[li].contains(static_cast<unsigned>(a))) {
                            if (auto d = env.diffs.maxDiff({false, ms.slot}, {true, static_cast<unsigned>(a)}))
                                savedArrDiffs.push_back({ms.slot, static_cast<unsigned>(a), *d});
                        }
                    }
                }
                for (unsigned ws : loopWritten[li]) {
                    auto preRange = env.diffs.rangeOf({false, ws});
                    invalidateSlot(childEnv, ws);
                    if (ws < fn.slots.size() && fn.slots[ws] != Type::Int && fn.slots[ws] != Type::Bool)
                        childEnv.diffs.forgetLen(ws);
                    for (const auto& ms : loopMonotone[li]) {
                        if (ms.slot == ws && preRange) {
                            if (ms.nonDecreasing && preRange->lower > kMinInt)
                                childEnv.diffs.addLower({false, ws}, preRange->lower);
                            if (ms.nonIncIfNonNeg && preRange->lower >= 0) {
                                childEnv.diffs.addLower({false, ws}, 0);
                                if (preRange->upper < kMaxInt) childEnv.diffs.addUpper({false, ws}, preRange->upper);
                            } else if (slotIsNonIncreasing(ms)) {
                                if (ms.nonNegPreserved && preRange->lower >= 0) childEnv.diffs.addLower({false, ws}, 0);
                                if (preRange->upper < kMaxInt) childEnv.diffs.addUpper({false, ws}, preRange->upper);
                            }
                        }
                    }
                    if (auto bsU = bsLoopUpper.find(li); bsU != bsLoopUpper.end()) {
                        const auto& bs = binarySearchLoops.at(li);
                        if (ws == bs.loSlot || ws == bs.hiSlot) {
                            childEnv.diffs.addLower({false, ws}, 0);
                            childEnv.diffs.addUpper({false, ws}, bsU->second);
                        }
                    }
                    if (auto accR = accLoopRanges.find({li, ws}); accR != accLoopRanges.end()) {
                        childEnv.diffs.addLower({false, ws}, accR->second.lower);
                        childEnv.diffs.addUpper({false, ws}, accR->second.upper);
                    } else if (auto sRes = smallLoopResults.find(li); sRes != smallLoopResults.end()) {
                        if (auto se = sRes->second.slotEnvelope.find(ws); se != sRes->second.slotEnvelope.end()) {
                            childEnv.diffs.addLower({false, ws}, se->second.lower);
                            childEnv.diffs.addUpper({false, ws}, se->second.upper);
                        }
                    }
                    if (auto sit = sentinelLoops.find(li); sit != sentinelLoops.end() && sit->second.jSlot == ws) {
                        childEnv.diffs.addUpper({false, ws}, maxArrayLength - 1);
                    }
                    if (auto sqIt = sqrtLoops.find(li); sqIt != sqrtLoops.end() && sqIt->second.dSlot == ws) {
                        if (auto uIt = sqrtLoopUpper.find(li); uIt != sqrtLoopUpper.end())
                            childEnv.diffs.addUpper({false, ws}, uIt->second);
                    }
                    if (auto sbIt = sqrtLoopBoundRange.find(li); sbIt != sqrtLoopBoundRange.end() && sbIt->second.first == ws) {
                        childEnv.diffs.addLower({false, ws}, sbIt->second.second.lower);
                        childEnv.diffs.addUpper({false, ws}, sbIt->second.second.upper);
                    }
                    if (auto dsbIt = divShrinkBoundRange.find(li); dsbIt != divShrinkBoundRange.end() && dsbIt->second.first == ws) {
                        childEnv.diffs.addLower({false, ws}, dsbIt->second.second.lower);
                        childEnv.diffs.addUpper({false, ws}, dsbIt->second.second.upper);
                    }
                }
                if (auto gcIt = activeGuardedCounters.find(li); gcIt != activeGuardedCounters.end()) {
                    for (const auto& gc : gcIt->second)
                        childEnv.diffs.addSlotDiff({false, gc.slot}, {false, gc.boundSlot}, 0);
                }
                if (auto mldIt = modSlotLtDiv.find(li); mldIt != modSlotLtDiv.end()) {
                    for (const auto& [mslot, dslot] : mldIt->second)
                        childEnv.diffs.addSlotDiff({false, mslot}, {false, dslot}, -1);
                }
                if (activeTwoWayPartitions.contains(li)) {
                    const auto& tw = twoWayPartitionLoops.at(li);
                    childEnv.diffs.addLower({false, tw.leftSlot}, 0);
                    childEnv.diffs.addLower({false, tw.rightSlot}, 0);
                    childEnv.diffs.addSlotDiff({false, tw.leftSlot}, {false, tw.boundSlot}, 0);
                    childEnv.diffs.addSlotDiff({false, tw.rightSlot}, {false, tw.boundSlot}, 0);
                }
                for (const auto& [uli, maxN] : activeUniqueZeroMaxN) {
                    (void)maxN;
                    if (!cfg.loops[uli].body.contains(child)) continue;
                    const auto& uz = uniqueZeroSetLoops.at(uli);
                    childEnv.diffs.addLower({false, uz.countSlot}, uz.initCount);
                    childEnv.diffs.addSlotDiff({false, uz.countSlot}, {true, uz.seenArr}, 0);
                    if (uz.queueArr != uz.seenArr)
                        childEnv.diffs.addSlotDiff({false, uz.countSlot}, {true, uz.queueArr}, 0);
                    if (uz.headSlot) {
                        childEnv.diffs.addLower({false, *uz.headSlot}, 0);
                        childEnv.diffs.addSlotDiff({false, *uz.headSlot}, {false, uz.countSlot}, 0);
                    }
                }
                if (auto hrIt = activeHeronRanges.find(li); hrIt != activeHeronRanges.end()) {
                    const auto& hr = heronLoops.at(li);
                    for (unsigned hs : {hr.xSlot, hr.ySlot}) {
                        childEnv.diffs.addLower({false, hs}, hrIt->second.lower);
                        childEnv.diffs.addUpper({false, hs}, hrIt->second.upper);
                    }
                }
                if (auto euIt = activeEuclidUpper.find(li); euIt != activeEuclidUpper.end()) {
                    const auto& eu = euclidLoops.at(li);
                    childEnv.diffs.addLower({false, eu.aSlot}, 0);
                    childEnv.diffs.addUpper({false, eu.aSlot}, euIt->second.first);
                    childEnv.diffs.addLower({false, eu.bSlot}, 0);
                    childEnv.diffs.addUpper({false, eu.bSlot}, euIt->second.second);
                }
                if (auto absIt = activeBoundedSelectors.find(li); absIt != activeBoundedSelectors.end()) {
                    for (const auto& bs : absIt->second) {
                        childEnv.diffs.addLower({false, bs.slot}, 0);
                        childEnv.diffs.addSlotDiff({false, bs.slot}, {false, bs.boundSlot}, -1);
                    }
                }
                if (auto bsU = bsLoopUpper.find(li); bsU != bsLoopUpper.end()) {
                    const auto& bs = binarySearchLoops.at(li);
                    childEnv.diffs.addSlotDiff({false, bs.loSlot}, {false, bs.hiSlot}, 0);
                    if (auto adIt = bsLoopArrDiffs.find(li); adIt != bsLoopArrDiffs.end()) {
                        for (const auto& ad : adIt->second)
                            childEnv.diffs.addSlotDiff({false, bs.hiSlot}, {true, ad.arrSlot}, ad.diff);
                    }
                }
                for (const auto& sd : savedArrDiffs) {
                    childEnv.diffs.addSlotDiff({false, sd.ws}, {true, sd.arrSlot}, sd.diff);
                }
            } else if (cfg.preds[child].size() == 1 && cfg.preds[child][0] == b && blk.exit == Exit::Branch && blk.value) {
                if (blk.yes == child && blk.no != child) refineFromCondition(childEnv, *blk.value, true);
                else if (blk.no == child && blk.yes != child) refineFromCondition(childEnv, *blk.value, false);
            } else if (cfg.preds[child].size() > 1) {
                // Only invalidate slots actually written along some CFG path between idom(b) and child!
                std::unordered_set<unsigned> branchWritten;
                std::unordered_set<unsigned> seen;
                bool canJoinAcyclic = (cfg.idom[child] >= 0 &&
                    cfg.loopOfBlock[static_cast<unsigned>(cfg.idom[child])] == cfg.loopOfBlock[child]);
                unsigned idomBlk = 0;
                if (cfg.idom[child] >= 0) {
                    idomBlk = static_cast<unsigned>(cfg.idom[child]);
                    std::vector<unsigned> work;
                    for (unsigned p : cfg.preds[child]) {
                        if (p != idomBlk && seen.insert(p).second) work.push_back(p);
                        if (p == idomBlk) {
                            // direct edge from idomBlk to child (nothing stored on edge)
                        }
                    }
                    while (!work.empty()) {
                        const unsigned u = work.back();
                        work.pop_back();
                        if (cfg.headerToLoop.contains(u) || cfg.loopOfBlock[u] != cfg.loopOfBlock[child])
                            canJoinAcyclic = false;
                        for (const auto& op : fn.blocks[u].ops) {
                            if (op.op == Op::Store) branchWritten.insert(op.slot);
                            for (unsigned ms : op.mutArgSlots) {
                                if (ms < fn.slots.size()) branchWritten.insert(ms);
                            }
                        }
                        for (unsigned p : cfg.preds[u]) {
                            if (p != idomBlk && cfg.rpoPos[p] > cfg.rpoPos[idomBlk] && seen.insert(p).second)
                                work.push_back(p);
                        }
                    }
                } else {
                    for (std::size_t s = 0; s < fn.slots.size(); ++s)
                        if (slotWritable[s]) branchWritten.insert(static_cast<unsigned>(s));
                }
                for (unsigned s : branchWritten) {
                    if (s >= fn.slots.size()) continue;
                    const int loopIdx = cfg.loopOfBlock[child];
                    std::optional<IntRange> kept;
                    if (canJoinAcyclic && fn.slots[s] == Type::Int &&
                        !(s < fn.slotByRef.size() && fn.slotByRef[s]) && !prevObservedValRanges.empty()) {
                        bool okJoin = true;
                        std::optional<IntRange> joined;
                        auto blockWritesDirect = [&](unsigned blkIdx) -> bool {
                            for (const auto& op : fn.blocks[blkIdx].ops) {
                                if (op.op == Op::Store && op.slot == s) return true;
                            }
                            return false;
                        };
                        bool needsPre = false;
                        std::unordered_set<unsigned> reachVisited;
                        std::vector<unsigned> reachWork;
                        for (unsigned p : cfg.preds[child]) {
                            if (p == idomBlk) { needsPre = true; continue; }
                            if (!blockWritesDirect(p) && reachVisited.insert(p).second) reachWork.push_back(p);
                        }
                        while (!reachWork.empty() && !needsPre) {
                            const unsigned u = reachWork.back();
                            reachWork.pop_back();
                            for (unsigned p : cfg.preds[u]) {
                                if (p == idomBlk) { needsPre = true; break; }
                                if (seen.contains(p) && !blockWritesDirect(p) && reachVisited.insert(p).second)
                                    reachWork.push_back(p);
                            }
                        }
                        if (needsPre) {
                            if (auto preR = env.diffs.rangeOf({false, s})) joined = *preR;
                            else okJoin = false;
                        }
                        for (unsigned u : seen) {
                            if (!okJoin) break;
                            for (const auto& op : fn.blocks[u].ops) {
                                for (unsigned ms : op.mutArgSlots) {
                                    if (ms == s) { okJoin = false; break; }
                                }
                                if (!okJoin) break;
                                if (op.op == Op::Store && op.slot == s && !op.inputs.empty()) {
                                    auto rit = prevObservedValRanges.find(op.inputs[0].id);
                                    if (rit == prevObservedValRanges.end()) { okJoin = false; break; }
                                    joined = joined ? ReturnRanges::envelope(*joined, rit->second) : rit->second;
                                }
                            }
                        }
                        if (okJoin && joined) kept = joined;
                    }
                    if (loopIdx >= 0) {
                        const std::size_t li = static_cast<std::size_t>(loopIdx);
                        for (const auto& ms : loopMonotone[li]) {
                            if (ms.slot == s) {
                                if (auto cur = env.diffs.rangeOf({false, s})) {
                                    bool nonInc = ms.nonIncreasing ||
                                        (ms.nonIncIfNonNeg && cur->lower >= 0) ||
                                        (ms.decBySlot && env.diffs.rangeOf({false, *ms.decBySlot}) &&
                                         env.diffs.rangeOf({false, *ms.decBySlot})->lower >= 0);
                                    if (ms.nonDecreasing && nonInc) kept = cur;
                                    else if (ms.nonDecreasing) kept = intersectRange(kept, IntRange{cur->lower, kMaxInt});
                                    else if ((ms.decDivSlot || ms.nonIncIfNonNeg || ms.nonNegPreserved) && cur->lower >= 0) {
                                        bool divOk = !ms.decDivSlot;
                                        if (ms.decDivSlot) {
                                            auto dr = env.diffs.rangeOf({false, *ms.decDivSlot});
                                            divOk = dr && dr->lower >= 1 &&
                                                (!loopWritten[li].contains(*ms.decDivSlot) ||
                                                 (sqrtLoops.contains(li) && sqrtLoops.at(li).dSlot == *ms.decDivSlot));
                                        }
                                        if (divOk) kept = intersectRange(kept, IntRange{0, cur->upper});
                                        else kept = intersectRange(kept, IntRange{kMinInt, cur->upper});
                                    } else if (nonInc) kept = intersectRange(kept, IntRange{kMinInt, cur->upper});
                                }
                            }
                        }
                        if (auto bsU = bsLoopUpper.find(li); bsU != bsLoopUpper.end()) {
                            const auto& bs = binarySearchLoops.at(li);
                            if (s == bs.loSlot || s == bs.hiSlot)
                                kept = intersectRange(kept, IntRange{0, bsU->second});
                        }
                        if (auto accR = accLoopRanges.find({li, s}); accR != accLoopRanges.end()) {
                            kept = intersectRange(kept, accR->second);
                        } else if (auto sRes = smallLoopResults.find(li); sRes != smallLoopResults.end()) {
                            if (auto se = sRes->second.slotEnvelope.find(s); se != sRes->second.slotEnvelope.end())
                                kept = intersectRange(kept, se->second);
                        }
                        if (auto sqIt = sqrtLoops.find(li); sqIt != sqrtLoops.end() && sqIt->second.dSlot == s) {
                            if (auto uIt = sqrtLoopUpper.find(li); uIt != sqrtLoopUpper.end())
                                kept = intersectRange(kept, IntRange{1, uIt->second});
                        }
                        if (auto sbIt = sqrtLoopBoundRange.find(li); sbIt != sqrtLoopBoundRange.end() && sbIt->second.first == s) {
                            kept = intersectRange(kept, sbIt->second.second);
                        }
                        if (auto dsbIt = divShrinkBoundRange.find(li); dsbIt != divShrinkBoundRange.end() && dsbIt->second.first == s) {
                            kept = intersectRange(kept, dsbIt->second.second);
                        }
                    }
                    invalidateSlot(childEnv, s);
                    if (fn.slots[s] != Type::Int && fn.slots[s] != Type::Bool) childEnv.diffs.forgetLen(s);
                    if (kept) {
                        if (kept->lower > kMinInt) childEnv.diffs.addLower({false, s}, kept->lower);
                        if (kept->upper < kMaxInt) childEnv.diffs.addUpper({false, s}, kept->upper);
                    }
                }
                if (cfg.loopOfBlock[child] >= 0) {
                    const std::size_t li = static_cast<std::size_t>(cfg.loopOfBlock[child]);
                    if (auto gcIt = activeGuardedCounters.find(li); gcIt != activeGuardedCounters.end()) {
                        for (const auto& gc : gcIt->second)
                            childEnv.diffs.addSlotDiff({false, gc.slot}, {false, gc.boundSlot}, 0);
                    }
                    if (auto mldIt = modSlotLtDiv.find(li); mldIt != modSlotLtDiv.end()) {
                        for (const auto& [mslot, dslot] : mldIt->second)
                            childEnv.diffs.addSlotDiff({false, mslot}, {false, dslot}, -1);
                    }
                    if (activeTwoWayPartitions.contains(li)) {
                        const auto& tw = twoWayPartitionLoops.at(li);
                        childEnv.diffs.addLower({false, tw.leftSlot}, 0);
                        childEnv.diffs.addLower({false, tw.rightSlot}, 0);
                        childEnv.diffs.addSlotDiff({false, tw.leftSlot}, {false, tw.boundSlot}, 0);
                        childEnv.diffs.addSlotDiff({false, tw.rightSlot}, {false, tw.boundSlot}, 0);
                    }
                    for (const auto& [uli, maxN] : activeUniqueZeroMaxN) {
                        (void)maxN;
                        if (!cfg.loops[uli].body.contains(child)) continue;
                        const auto& uz = uniqueZeroSetLoops.at(uli);
                        childEnv.diffs.addLower({false, uz.countSlot}, uz.initCount);
                        childEnv.diffs.addSlotDiff({false, uz.countSlot}, {true, uz.seenArr}, 0);
                        if (uz.queueArr != uz.seenArr)
                            childEnv.diffs.addSlotDiff({false, uz.countSlot}, {true, uz.queueArr}, 0);
                        if (uz.headSlot) {
                            childEnv.diffs.addLower({false, *uz.headSlot}, 0);
                            childEnv.diffs.addSlotDiff({false, *uz.headSlot}, {false, uz.countSlot}, 0);
                        }
                    }
                    if (auto hrIt = activeHeronRanges.find(li); hrIt != activeHeronRanges.end()) {
                        const auto& hr = heronLoops.at(li);
                        for (unsigned hs : {hr.xSlot, hr.ySlot}) {
                            childEnv.diffs.addLower({false, hs}, hrIt->second.lower);
                            childEnv.diffs.addUpper({false, hs}, hrIt->second.upper);
                        }
                    }
                    if (auto euIt = activeEuclidUpper.find(li); euIt != activeEuclidUpper.end()) {
                        const auto& eu = euclidLoops.at(li);
                        childEnv.diffs.addLower({false, eu.aSlot}, 0);
                        childEnv.diffs.addUpper({false, eu.aSlot}, euIt->second.first);
                        childEnv.diffs.addLower({false, eu.bSlot}, 0);
                        childEnv.diffs.addUpper({false, eu.bSlot}, euIt->second.second);
                    }
                    if (auto absIt = activeBoundedSelectors.find(li); absIt != activeBoundedSelectors.end()) {
                        for (const auto& bs : absIt->second) {
                            childEnv.diffs.addLower({false, bs.slot}, 0);
                            childEnv.diffs.addSlotDiff({false, bs.slot}, {false, bs.boundSlot}, -1);
                        }
                    }
                }
            }
            if (cfg.loopOfBlock[child] >= 0) {
                const std::size_t cli = static_cast<std::size_t>(cfg.loopOfBlock[child]);
                if (activeTwoWayPartitions.contains(cli)) {
                    const auto& tw = twoWayPartitionLoops.at(cli);
                    if (child == tw.thenBlock) {
                        childEnv.diffs.addSlotDiff({false, tw.leftSlot}, {false, tw.boundSlot}, -1);
                    } else if (child == tw.elseBlock || child == tw.ltCheckBlock) {
                        childEnv.diffs.addSlotDiff({false, tw.rightSlot}, {false, tw.boundSlot}, -1);
                    }
                }
                for (const auto& [uli, maxN] : activeUniqueZeroMaxN) {
                    (void)maxN;
                    const auto& uz = uniqueZeroSetLoops.at(uli);
                    if (child == uz.guardBlock) {
                        childEnv.diffs.addSlotDiff({false, uz.countSlot}, {true, uz.seenArr}, -1);
                        if (uz.queueArr != uz.seenArr)
                            childEnv.diffs.addSlotDiff({false, uz.countSlot}, {true, uz.queueArr}, -1);
                    }
                }
            }
            if (auto litExit = cfg.headerToLoop.find(b); litExit != cfg.headerToLoop.end() && child == fn.blocks[b].no) {
                const std::size_t exitLi = litExit->second;
                if (auto sqIt = sqrtLoops.find(exitLi); sqIt != sqrtLoops.end() && !sqIt->second.andExitBlk) {
                    if (auto uIt = sqrtLoopUpper.find(exitLi); uIt != sqrtLoopUpper.end())
                        childEnv.diffs.addUpper({false, sqIt->second.dSlot}, uIt->second);
                }
                if (auto hrIt = activeHeronRanges.find(exitLi); hrIt != activeHeronRanges.end()) {
                    const auto& hr = heronLoops.at(exitLi);
                    for (unsigned hs : {hr.xSlot, hr.ySlot}) {
                        childEnv.diffs.addLower({false, hs}, hrIt->second.lower);
                        childEnv.diffs.addUpper({false, hs}, hrIt->second.upper);
                    }
                }
                if (auto euIt = activeEuclidUpper.find(exitLi); euIt != activeEuclidUpper.end()) {
                    const auto& eu = euclidLoops.at(exitLi);
                    childEnv.diffs.addLower({false, eu.aSlot}, 0);
                    childEnv.diffs.addUpper({false, eu.aSlot}, euIt->second.first);
                    childEnv.diffs.addLower({false, eu.bSlot}, 0);
                    childEnv.diffs.addUpper({false, eu.bSlot}, euIt->second.second);
                }
                if (auto absIt = activeBoundedSelectors.find(exitLi); absIt != activeBoundedSelectors.end()) {
                    for (const auto& bs : absIt->second) {
                        childEnv.diffs.addLower({false, bs.slot}, 0);
                        childEnv.diffs.addSlotDiff({false, bs.slot}, {false, bs.boundSlot}, -1);
                    }
                }
                if (auto srIt = smallLoopResults.find(exitLi); srIt != smallLoopResults.end()) {
                    for (const auto& [ws, postR] : srIt->second.slotPostEnvelope) {
                        invalidateSlot(childEnv, ws);
                        if (postR.lower > kMinInt) childEnv.diffs.addLower({false, ws}, postR.lower);
                        if (postR.upper < kMaxInt) childEnv.diffs.addUpper({false, ws}, postR.upper);
                    }
                }
                if (auto smIt = loopSumSources.find(exitLi); smIt != loopSumSources.end()) {
                    for (const auto& [sumSlot, mi] : smIt->second) {
                        childEnv.meanSources[sumSlot] = {mi.upperSlot, mi.stepRange};
                    }
                }
            }
            for (const auto& [sli, sq] : sqrtLoops) {
                if (sq.andExitBlk && *sq.andExitBlk == b && child == fn.blocks[b].no) {
                    if (auto uIt = sqrtLoopUpper.find(sli); uIt != sqrtLoopUpper.end()) {
                        childEnv.diffs.addLower({false, sq.dSlot}, 1);
                        childEnv.diffs.addUpper({false, sq.dSlot}, uIt->second);
                    }
                }
            }
            outChildren.emplace_back(child, std::move(childEnv));
        }
    }

    void updateLoopAccRanges() {
        accLoopRanges.clear();
        modSlotLtDiv.clear();
        auto storeAccRange = [&](std::size_t li, unsigned s, IntRange r) {
            auto [it, ins] = accLoopRanges.emplace(std::pair{li, s}, r);
            if (!ins) it->second = ReturnRanges::envelope(it->second, r);
        };
        for (const auto& [li, ops] : loopAccOps) {
            auto tripIt = loopMaxTrips.find(li);
            if (tripIt == loopMaxTrips.end()) continue;
            for (const auto& [accSlot, stepVal, isSub] : ops) {
                auto preIt = loopPreAccRanges.find({li, accSlot});
                auto stepIt = observedValRanges.find(stepVal.id);
                if (preIt == loopPreAccRanges.end() || stepIt == observedValRanges.end()) continue;
                // Find total enclosing trips if op sits in nested For loops within li
                std::int64_t trips = tripIt->second;
                // Locate block storing accSlot in li
                int storeBlk = -1;
                for (unsigned b : cfg.loops[li].body) {
                    for (const auto& op : fn.blocks[b].ops)
                        if (op.op == Op::Store && op.slot == accSlot) storeBlk = static_cast<int>(b);
                }
                if (storeBlk >= 0 && cfg.loopOfBlock[storeBlk] != static_cast<int>(li)) {
                    for (std::size_t sub = 0; sub < cfg.loops.size(); ++sub) {
                        if (sub != li && cfg.loops[sub].body.contains(static_cast<unsigned>(storeBlk)) &&
                            cfg.loops[li].body.contains(cfg.loops[sub].header)) {
                            auto subT = loopMaxTrips.find(sub);
                            if (subT == loopMaxTrips.end()) { trips = -1; break; }
                            auto p = checkedMul64(trips, subT->second);
                            if (!p) { trips = -1; break; }
                            trips = *p;
                        }
                    }
                }
                if (trips < 0) continue;
                std::optional<IntRange> sr = isSub ? ReturnRanges::negateRange(stepIt->second) : stepIt->second;
                if (!sr) continue;
                auto delta = ReturnRanges::multiplyRanges({0, trips}, *sr);
                auto total = delta ? ReturnRanges::addRanges(preIt->second, *delta) : std::optional<IntRange>{};
                if (total) {
                    storeAccRange(li, accSlot, *total);
                    if (storeBlk >= 0) {
                        for (std::size_t sub = 0; sub < cfg.loops.size(); ++sub) {
                            if (sub != li && cfg.loops[sub].body.contains(static_cast<unsigned>(storeBlk)) &&
                                cfg.loops[li].body.contains(cfg.loops[sub].header)) {
                                storeAccRange(sub, accSlot, *total);
                            }
                        }
                    }
                }
            }
        }
        for (const auto& [li, ops] : loopMultiAccOps) {
            auto tripIt = loopMaxTrips.find(li);
            if (tripIt == loopMaxTrips.end()) continue;
            const std::int64_t trips = tripIt->second;
            for (const auto& ma : ops) {
                auto preIt = loopPreAccRanges.find({li, ma.slot});
                if (preIt == loopPreAccRanges.end()) continue;
                std::optional<IntRange> envStep = IntRange{0, 0};
                bool allStepsKnown = true;
                for (const auto& [stepVal, isSub] : ma.steps) {
                    auto stepIt = observedValRanges.find(stepVal.id);
                    if (stepIt == observedValRanges.end()) { allStepsKnown = false; break; }
                    auto sr = isSub ? ReturnRanges::negateRange(stepIt->second)
                                    : std::optional<IntRange>{stepIt->second};
                    if (!sr) { allStepsKnown = false; break; }
                    envStep = ReturnRanges::envelope(*envStep, *sr);
                }
                if (!allStepsKnown) continue;
                auto totalUpdates = checkedMul64(trips, ma.maxPerIter);
                if (!totalUpdates) continue;
                auto delta = ReturnRanges::multiplyRanges({0, *totalUpdates}, *envStep);
                if (!delta) continue;
                if (auto total = ReturnRanges::addRanges(preIt->second, *delta)) {
                    storeAccRange(li, ma.slot, *total);
                }
            }
        }
        for (const auto& [li, ops] : loopMaxClampOps) {
            auto tripIt = loopMaxTrips.find(li);
            if (tripIt == loopMaxTrips.end()) continue;
            const std::int64_t trips = tripIt->second;
            for (const auto& mc : ops) {
                auto preIt = loopPreAccRanges.find({li, mc.slot});
                auto cIt = observedValRanges.find(mc.clampVal.id);
                auto sIt = observedValRanges.find(mc.stepTerm.id);
                if (preIt == loopPreAccRanges.end() || cIt == observedValRanges.end() || sIt == observedValRanges.end())
                    continue;
                std::optional<IntRange> sr = mc.isSub ? ReturnRanges::negateRange(sIt->second) : sIt->second;
                if (!sr) continue;
                if (mc.hasSubTerm) {
                    auto subIt = observedValRanges.find(mc.subTerm.id);
                    if (subIt == observedValRanges.end()) continue;
                    sr = ReturnRanges::subtractRanges(*sr, subIt->second);
                    if (!sr) continue;
                }
                const std::int64_t hiDelta = std::max<std::int64_t>(0, sr->upper);
                if (auto addMax = ReturnRanges::multiplyRanges({0, trips}, {0, hiDelta})) {
                    const std::int64_t baseHi = std::max(preIt->second.upper, cIt->second.upper);
                    if (auto hiAll = checkedAdd64(baseHi, addMax->upper)) {
                        const std::int64_t loAll = std::min(preIt->second.lower, cIt->second.lower);
                        storeAccRange(li, mc.slot, IntRange{loAll, *hiAll});
                    }
                }
            }
        }
        for (const auto& [li, ops] : loopCoupledMaxOps) {
            auto tripIt = loopMaxTrips.find(li);
            if (tripIt == loopMaxTrips.end()) continue;
            const std::int64_t trips = tripIt->second;
            for (const auto& cp : ops) {
                auto pIt = loopPreAccRanges.find({li, cp.prevSlot});
                auto bIt = loopPreAccRanges.find({li, cp.bestSlot});
                auto dIt = observedValRanges.find(cp.deltaVal.id);
                if (pIt == loopPreAccRanges.end() || bIt == loopPreAccRanges.end() || dIt == observedValRanges.end())
                    continue;
                std::optional<IntRange> dr = cp.isSub ? ReturnRanges::negateRange(dIt->second) : dIt->second;
                if (!dr) continue;
                const std::int64_t hiDelta = std::max<std::int64_t>(0, dr->upper);
                if (auto addMax = ReturnRanges::multiplyRanges({0, trips}, {0, hiDelta})) {
                    const std::int64_t baseHi = std::max(pIt->second.upper, bIt->second.upper);
                    if (auto hiAll = checkedAdd64(baseHi, addMax->upper)) {
                        const std::int64_t loAll = std::min(pIt->second.lower, bIt->second.lower);
                        IntRange rng{loAll, *hiAll};
                        storeAccRange(li, cp.prevSlot, rng);
                        storeAccRange(li, cp.bestSlot, rng);
                    }
                }
            }
        }
        for (int repPass = 0; repPass < 2; ++repPass) {
            for (const auto& [li, ops] : loopRunningExtremeOps) {
                for (const auto& re : ops) {
                    auto preIt = loopPreAccRanges.find({li, re.slot});
                    if (preIt == loopPreAccRanges.end()) continue;
                    std::optional<IntRange> srcRng;
                    if (re.hasSrcSlot) {
                        if (auto aIt = accLoopRanges.find({li, re.srcSlot}); aIt != accLoopRanges.end())
                            srcRng = aIt->second;
                        else if (auto vIt = observedValRanges.find(re.srcVal.id); vIt != observedValRanges.end())
                            srcRng = vIt->second;
                    } else if (auto vIt = observedValRanges.find(re.srcVal.id); vIt != observedValRanges.end()) {
                        srcRng = vIt->second;
                    }
                    if (!srcRng) continue;
                    const IntRange rng = re.isMin
                        ? IntRange{std::min(preIt->second.lower, srcRng->lower), preIt->second.upper}
                        : IntRange{preIt->second.lower, std::max(preIt->second.upper, srcRng->upper)};
                    storeAccRange(li, re.slot, rng);
                }
            }
        }
        for (const auto& [li, ops] : loopModOps) {
            // Per-slot greatest-fixed-point non-negativity induction across modulo & shift-copy slots
            std::unordered_map<unsigned, bool> slotNonNeg;
            std::unordered_map<unsigned, bool> modReady;
            for (const auto& mp : ops) {
                auto dIt = observedValRanges.find(mp.divVal.id);
                const bool divOk = (dIt != observedValRanges.end() && dIt->second.lower >= 1);
                bool preOk = !mp.isLiveIn;
                if (mp.isLiveIn) {
                    auto preIt = loopPreAccRanges.find({li, mp.slot});
                    preOk = (preIt != loopPreAccRanges.end() && preIt->second.lower >= 0);
                }
                auto [it, ins] = slotNonNeg.emplace(mp.slot, divOk && preOk);
                if (!ins) it->second = it->second && divOk && preOk;
                auto [mIt, mIns] = modReady.emplace(mp.slot, divOk);
                if (!mIns) mIt->second = mIt->second && divOk;
            }
            if (auto cpIt = loopCopyOps.find(li); cpIt != loopCopyOps.end()) {
                for (const auto& cp : cpIt->second) {
                    bool preOk = !cp.isLiveIn;
                    if (cp.isLiveIn) {
                        auto preIt = loopPreAccRanges.find({li, cp.dstSlot});
                        preOk = (preIt != loopPreAccRanges.end() && preIt->second.lower >= 0);
                    }
                    auto [it, ins] = slotNonNeg.emplace(cp.dstSlot, preOk);
                    if (!ins) it->second = it->second && preOk;
                }
            }
            auto exprIsNonNeg = [&](auto& self, Value v, int depth) -> bool {
                if (depth > 24) return false;
                auto rIt = observedValRanges.find(v.id);
                if (rIt != observedValRanges.end() && rIt->second.lower >= 0) return true;
                auto dit = defOp.find(v.id);
                if (dit == defOp.end()) return false;
                const Operation* o = dit->second;
                if (o->op == Op::Int) {
                    auto c = parseConstInt(o->data);
                    return c && *c >= 0;
                }
                if (o->op == Op::Load) {
                    auto nit = slotNonNeg.find(o->slot);
                    return nit != slotNonNeg.end() && nit->second;
                }
                if (o->op == Op::Binary && (o->data == "+" || o->data == "*" || o->data == "%")) {
                    if (o->data == "%") {
                        auto dIt = observedValRanges.find(o->inputs[1].id);
                        return dIt != observedValRanges.end() && dIt->second.lower >= 1 &&
                               self(self, o->inputs[0], depth + 1);
                    }
                    return self(self, o->inputs[0], depth + 1) && self(self, o->inputs[1], depth + 1);
                }
                return false;
            };
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& mp : ops) {
                    if (slotNonNeg[mp.slot] && !exprIsNonNeg(exprIsNonNeg, mp.exprRoot, 0)) {
                        slotNonNeg[mp.slot] = false;
                        changed = true;
                    }
                }
                if (auto cpIt = loopCopyOps.find(li); cpIt != loopCopyOps.end()) {
                    for (const auto& cp : cpIt->second) {
                        if (slotNonNeg[cp.dstSlot]) {
                            auto sit = slotNonNeg.find(cp.srcSlot);
                            if (sit == slotNonNeg.end() || !sit->second) {
                                slotNonNeg[cp.dstSlot] = false;
                                changed = true;
                            }
                        }
                    }
                }
            }
            for (const auto& mp : ops) {
                if (!modReady[mp.slot]) continue;
                auto dIt = observedValRanges.find(mp.divVal.id);
                if (dIt == observedValRanges.end() || dIt->second.lower <= 0) continue;
                std::int64_t bound = std::max<std::int64_t>(0, dIt->second.upper - 1);
                const bool nn = slotNonNeg[mp.slot];
                std::int64_t remLo = nn ? 0 : -bound;
                if (auto rObs = observedValRanges.find(mp.remResult.id);
                    rObs != observedValRanges.end() && rObs->second.lower >= 0) {
                    remLo = 0;
                }
                IntRange r{remLo, bound};
                if (mp.isLiveIn) {
                    auto preIt = loopPreAccRanges.find({li, mp.slot});
                    if (preIt == loopPreAccRanges.end()) continue;
                    r = ReturnRanges::envelope(preIt->second, r);
                }
                storeAccRange(li, mp.slot, r);
                if (nn) {
                    if (auto dls = loadSlot.find(mp.divVal.id);
                        dls != loadSlot.end() && !loopWritten[li].contains(dls->second)) {
                        const bool preOk = !mp.isLiveIn ||
                            (loopPreModLtDiv.count({li, mp.slot}) &&
                             loopPreModLtDiv.at({li, mp.slot}).contains(dls->second));
                        if (preOk) modSlotLtDiv[li].emplace_back(mp.slot, dls->second);
                    }
                }
            }
        }
        // Propagate copy-chain slots (e.g. Fibonacci shift vA = vB, argmax vBest = vK) after modulo/accumulator ranges settle
        for (int pass = 0; pass < 3; ++pass) {
            for (const auto& [li, ops] : loopCopyOps) {
                auto fhIt = forHeaders.find(cfg.loops[li].header);
                for (const auto& cp : ops) {
                    std::optional<IntRange> srcRange;
                    if (auto srcIt = accLoopRanges.find({li, cp.srcSlot}); srcIt != accLoopRanges.end()) {
                        srcRange = srcIt->second;
                    } else if (cp.beforeIterInc && fhIt != forHeaders.end() && fhIt->second.valid &&
                               fhIt->second.iterSlot == cp.srcSlot) {
                        if (auto irIt = loopIterRanges.find(li); irIt != loopIterRanges.end()) {
                            srcRange = irIt->second;
                        }
                    }
                    if (!srcRange) continue;
                    IntRange r = *srcRange;
                    if (cp.isLiveIn) {
                        auto preIt = loopPreAccRanges.find({li, cp.dstSlot});
                        if (preIt == loopPreAccRanges.end()) continue;
                        r = ReturnRanges::envelope(preIt->second, r);
                    }
                    storeAccRange(li, cp.dstSlot, r);
                }
            }
        }
        // Propagate uniqueZeroSetLoops accumulator bounds (guardBlock executes at most maxN times)
        for (const auto& [li, maxN] : activeUniqueZeroMaxN) {
            const auto& uz = uniqueZeroSetLoops.at(li);
            for (unsigned s : loopWritten[li]) {
                if (s == uz.countSlot || uz.headSlot == s || s >= fn.slots.size() || fn.slots[s] != Type::Int) continue;
                int sCnt = 0;
                const Operation* sOp = nullptr;
                bool onlyGuard = true;
                for (unsigned b : cfg.loops[li].body) {
                    for (const auto& op : fn.blocks[b].ops) {
                        if (op.op == Op::Store && op.slot == s) {
                            ++sCnt; sOp = &op;
                            if (b != uz.guardBlock) onlyGuard = false;
                        }
                    }
                }
                if (!onlyGuard || sCnt != 1 || !sOp || sOp->inputs.empty()) continue;
                auto d = defOp.find(sOp->inputs[0].id);
                if (d == defOp.end() || d->second->op != Op::Binary || d->second->data != "+") continue;
                auto ls = loadSlot.find(d->second->inputs[0].id);
                if (ls == loadSlot.end() || ls->second != s) continue;
                IntRange preR{0, maxArrayLength};
                if (auto preIt = observedValRanges.find(d->second->inputs[1].id); preIt != observedValRanges.end()) {
                    if (auto prod = ReturnRanges::multiplyRanges({0, maxN}, preIt->second)) {
                        if (auto preAcc = loopPreAccRanges.find({li, s}); preAcc != loopPreAccRanges.end()) {
                            if (auto tot = ReturnRanges::addRanges(preAcc->second, *prod)) {
                                storeAccRange(li, s, *tot);
                                for (std::size_t sub = 0; sub < cfg.loops.size(); ++sub) {
                                    if (sub != li && cfg.loops[li].body.contains(cfg.loops[sub].header) &&
                                        cfg.loops[sub].body.contains(uz.guardBlock)) {
                                        storeAccRange(sub, s, *tot);
                                    }
                                }
                            }
                        } else if (auto pAdd = ReturnRanges::addRanges(IntRange{0, 0}, *prod)) {
                            // Still record pre-loop 0 + delta if s was initialized to 0 before loop
                            (void)pAdd;
                        }
                    }
                }
                (void)preR;
            }
        }
        // Propagate small nested loop accumulator deltas (e.g. binomialRows inner loop -> outer vTotal)
        for (std::size_t sub = 0; sub < cfg.loops.size(); ++sub) {
            auto sRes = smallLoopResults.find(sub);
            if (sRes == smallLoopResults.end()) continue;
            for (std::size_t outer = 0; outer < cfg.loops.size(); ++outer) {
                if (outer == sub || !cfg.loops[outer].body.contains(cfg.loops[sub].header)) continue;
                auto tripIt = loopMaxTrips.find(outer);
                if (tripIt == loopMaxTrips.end()) continue;
                for (const auto& [accSlot, deltaR] : sRes->second.accDelta) {
                    auto preIt = loopPreAccRanges.find({outer, accSlot});
                    if (preIt == loopPreAccRanges.end()) continue;
                    // Verify outer loop modifies accSlot ONLY inside `sub`
                    bool extraWrite = false;
                    for (unsigned ob : cfg.loops[outer].body) {
                        if (cfg.loops[sub].body.contains(ob)) continue;
                        for (const auto& op : fn.blocks[ob].ops)
                            if (op.op == Op::Store && op.slot == accSlot) extraWrite = true;
                    }
                    if (extraWrite) continue;
                    if (auto prod = ReturnRanges::multiplyRanges({0, tripIt->second}, deltaR)) {
                        if (auto total = ReturnRanges::addRanges(preIt->second, *prod)) {
                            storeAccRange(outer, accSlot, *total);
                            storeAccRange(sub, accSlot, *total);
                        }
                    }
                }
            }
        }
        // Collect counted-loop zero-based mean-value quotient sources: for i in 0..N { sum += step }
        loopSumSources.clear();
        for (const auto& [li, ops] : loopAccOps) {
            const auto& lp = cfg.loops[li];
            auto fhIt = forHeaders.find(lp.header);
            if (fhIt == forHeaders.end() || !fhIt->second.valid || lp.body.size() != 2) continue;
            auto loConst = parseConstInt(defOp.count(fhIt->second.lower.id) ? defOp.at(fhIt->second.lower.id)->data : "");
            if (loConst != 0) continue;
            auto uSym = buildSym(fhIt->second.upper.id, Env{});
            if (!uSym || !uSym->isPureLinear() || uSym->linear.size() != 1 || uSym->constant != 0 ||
                uSym->linear.begin()->second != 1) continue;
            const unsigned uSlot = uSym->linear.begin()->first;
            if (slotWritable[uSlot]) continue;
            for (const auto& [accSlot, stepVal, isSub] : ops) {
                if (isSub || storeCount[accSlot] != 2) continue;
                auto preIt = loopPreAccRanges.find({li, accSlot});
                auto stepIt = observedValRanges.find(stepVal.id);
                if (preIt == loopPreAccRanges.end() || stepIt == observedValRanges.end()) continue;
                if (preIt->second.lower == 0 && preIt->second.upper == 0) {
                    loopSumSources[li][accSlot] = {uSlot, stepIt->second};
                }
            }
        }
    }

    void updateGlobalElemRanges() {
        const std::size_t nSlots = fn.slots.size();
        std::vector<int> originCount(nSlots, 0);
        std::vector<bool> bad(nSlots, false);
        std::vector<const Operation*> repeatOp(nSlots, nullptr);
        std::vector<std::optional<unsigned>> cloneSrc(nSlots);
        struct W { unsigned block; unsigned arr; const Operation* op; };
        std::vector<W> writes;

        for (unsigned b = 0; b < fn.blocks.size(); ++b) {
            for (const auto& op : fn.blocks[b].ops) {
                if (op.op == Op::Store && !op.inputs.empty() && isAnyArrayType(programPtr, fn.slots[op.slot])) {
                    auto def = defOp.find(op.inputs[0].id);
                    if (def != defOp.end() && (def->second->op == Op::RepeatRegion || def->second->op == Op::RepeatHeap)) {
                        ++originCount[op.slot];
                        repeatOp[op.slot] = def->second;
                    } else if (def != defOp.end() && def->second->op == Op::Clone && !def->second->inputs.empty()) {
                        if (auto srcLs = loadSlot.find(def->second->inputs[0].id); srcLs != loadSlot.end()) {
                            ++originCount[op.slot];
                            cloneSrc[op.slot] = srcLs->second;
                        } else bad[op.slot] = true;
                    } else bad[op.slot] = true;
                }
                if (op.op == Op::IndexSet) {
                    if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end())
                        writes.push_back({b, ls->second, &op});
                }
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                    for (const auto& in : op.inputs)
                        if (in.type == Type::IntArray)
                            if (auto ls = loadSlot.find(in.id); ls != loadSlot.end())
                                bad[ls->second] = true;
                }
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Builtin || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                    for (unsigned ms : op.mutArgSlots)
                        if (ms < nSlots) bad[ms] = true;
                }
                if (op.op == Op::ClearOwner || op.op == Op::ClearField) bad[op.slot] = true;
                if (op.op == Op::ClearIndex && !op.inputs.empty()) {
                    if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end()) bad[ls->second] = true;
                }
            }
            if (fn.blocks[b].exit == Exit::Return && fn.blocks[b].value && fn.blocks[b].value->type == Type::IntArray)
                if (auto ls = loadSlot.find(fn.blocks[b].value->id); ls != loadSlot.end())
                    bad[ls->second] = true;
        }

        // Exact single-pass element-range synthesis:
        // 1) Seed from initial fill + assignWrites,
        // 2) Apply For-loop chain incWrites ONCE (multiplicatively bounded by enclosing For trips),
        // 3) Apply monotone minWrites & proven acyclic DP recurrences (knapsack/rodCut/LIS/coinMin).
        auto enclosingForChain = [&](unsigned block, int skipLoop = -1) -> std::optional<std::pair<std::vector<int>, std::int64_t>> {
            std::vector<int> chain;
            std::int64_t prod = 1;
            for (std::size_t oi = 0; oi < cfg.loops.size(); ++oi) {
                if (static_cast<int>(oi) == skipLoop || !cfg.loops[oi].body.contains(block)) continue;
                auto fit = forHeaders.find(cfg.loops[oi].header);
                if (fit == forHeaders.end() || !fit->second.valid) return {};
                auto lr = observedValRanges.find(fit->second.lower.id);
                auto ur = observedValRanges.find(fit->second.upper.id);
                if (lr == observedValRanges.end() || ur == observedValRanges.end() ||
                    lr->second.lower < 0 || ur->second.upper > maxArrayLength)
                    return {};
                const std::int64_t t = std::max<std::int64_t>(0, ur->second.upper - lr->second.lower);
                auto mp = checkedMul64(prod, t);
                if (!mp || *mp > 1000000000LL) return {};
                prod = *mp;
                chain.push_back(static_cast<int>(oi));
            }
            if (chain.empty()) return {};
            return std::pair{std::move(chain), prod};
        };

        for (std::size_t s = 0; s < nSlots; ++s) {
            if (globalElemRanges.count(static_cast<unsigned>(s))) continue;
            if (fn.slots[s] != Type::IntArray || originCount[s] != 1 || bad[s] || (!repeatOp[s] && !cloneSrc[s])) continue;
            auto isSelfLoad = [&](Value v) -> bool {
                auto dit = defOp.find(v.id);
                if (dit == defOp.end() || dit->second->op != Op::Index) return false;
                auto ls = loadSlot.find(dit->second->inputs[0].id);
                return ls != loadSlot.end() && ls->second == s;
            };
            auto isSelfAtSlot = [&](Value v, unsigned idxSlot) -> bool {
                auto dit = defOp.find(v.id);
                if (dit == defOp.end() || dit->second->op != Op::Index) return false;
                auto als = loadSlot.find(dit->second->inputs[0].id);
                auto ils = loadSlot.find(dit->second->inputs[1].id);
                return als != loadSlot.end() && als->second == s && ils != loadSlot.end() && ils->second == idxSlot;
            };
            auto isSelfCopy = [&](Value v) -> bool {
                if (isSelfLoad(v)) return true;
                auto ls = loadSlot.find(v.id);
                if (ls == loadSlot.end() || ls->second < fn.source.params.size()) return false;
                const unsigned tmpSlot = ls->second;
                int tmpStores = 0;
                for (const auto& blk : fn.blocks) {
                    for (const auto& op : blk.ops) {
                        if (op.op == Op::Store && op.slot == tmpSlot) {
                            ++tmpStores;
                            if (op.inputs.empty() || !isSelfLoad(op.inputs[0])) return false;
                        }
                    }
                }
                return tmpStores >= 1;
            };

            std::vector<W> assignWrites, minWrites;
            struct IncW { std::vector<int> loops; IntRange deltaRange; };
            std::vector<IncW> incWrites;
            // Upper-bound increments for max/min-plus DP: max(0, outerTrips * deltaHi)
            struct DPBound { std::vector<int> loops; std::int64_t upperBump; bool requiresNonNegDelta; std::int64_t minDeltaLo; };
            std::vector<DPBound> dpBounds;
            std::unordered_set<int> nonAssignLoops;
            bool requiresNonNegMinSub = false;
            std::optional<IntRange> dpExtraInit;
            struct RefillPair { std::int64_t minT; std::int64_t maxD; IntRange rRng; };
            std::vector<RefillPair> refillPairs;
            std::unordered_set<const Operation*> handledRefillOps;
            bool ok = true;

            for (const auto& w : writes) {
                if (w.arr != s) continue;
                const Value rhs = w.op->inputs[2];
                if (isSelfCopy(rhs)) continue;
                auto rdef = defOp.find(rhs.id);
                if (rdef != defOp.end() && (rdef->second->op == Op::Max || rdef->second->op == Op::Min) &&
                    rdef->second->inputs.size() == 2 &&
                    isSelfCopy(rdef->second->inputs[0]) && isSelfCopy(rdef->second->inputs[1])) {
                    continue;
                }
                // Plain minWrite on self load
                bool handled = false;
                if (rdef != defOp.end() && rdef->second->op == Op::Min &&
                    (isSelfLoad(rdef->second->inputs[0]) || isSelfLoad(rdef->second->inputs[1]))) {
                    // Check if DP-3: d[i] = min(d[i], d[i - c] + delta) inside forward-prefix for i
                    const Value otherMin = isSelfLoad(rdef->second->inputs[0])
                        ? rdef->second->inputs[1] : rdef->second->inputs[0];
                    auto odef = defOp.find(otherMin.id);
                    auto idxLs = loadSlot.find(w.op->inputs[1].id);
                    if (idxLs != loadSlot.end() && cfg.loopOfBlock[w.block] >= 0 &&
                        odef != defOp.end() && odef->second->op == Op::Binary && odef->second->data == "+") {
                        // Look for For loop in enclosing chain whose iterSlot == *idxLs
                        int forILi = -1;
                        std::int64_t iTrips = -1;
                        std::vector<int> chainLoops;
                        bool allValidFor = true;
                        for (std::size_t oi = 0; oi < cfg.loops.size(); ++oi) {
                            if (!cfg.loops[oi].body.contains(w.block)) continue;
                            auto fit = forHeaders.find(cfg.loops[oi].header);
                            if (fit == forHeaders.end() || !fit->second.valid) { allValidFor = false; break; }
                            chainLoops.push_back(static_cast<int>(oi));
                            if (fit->second.iterSlot == idxLs->second) {
                                auto lr = observedValRanges.find(fit->second.lower.id);
                                auto ur = observedValRanges.find(fit->second.upper.id);
                                if (lr != observedValRanges.end() && ur != observedValRanges.end() &&
                                    lr->second.lower >= 0 && ur->second.upper <= maxArrayLength) {
                                    forILi = static_cast<int>(oi);
                                    iTrips = std::max<std::int64_t>(0, ur->second.upper - lr->second.lower);
                                }
                            }
                        }
                        if (allValidFor && forILi >= 0 && iTrips >= 0) {
                            Value prevV{}, deltaV{};
                            bool hasSum = false;
                            if (isSelfLoad(odef->second->inputs[0])) { prevV = odef->second->inputs[0]; deltaV = odef->second->inputs[1]; hasSum = true; }
                            else if (isSelfLoad(odef->second->inputs[1])) { prevV = odef->second->inputs[1]; deltaV = odef->second->inputs[0]; hasSum = true; }
                            if (hasSum) {
                                auto pDef = defOp.find(prevV.id);
                                auto idxPrev = (pDef != defOp.end() && pDef->second->op == Op::Index) ? pDef->second->inputs[1] : Value{};
                                auto ipDef = defOp.find(idxPrev.id);
                                auto dit = observedValRanges.find(deltaV.id);
                                if (dit != observedValRanges.end() && dit->second.lower >= 0 &&
                                    ipDef != defOp.end() && ipDef->second->op == Op::Binary && ipDef->second->data == "-") {
                                    auto lhsS = loadSlot.find(ipDef->second->inputs[0].id);
                                    auto rhsR = observedValRanges.find(ipDef->second->inputs[1].id);
                                    if (lhsS != loadSlot.end() && lhsS->second == idxLs->second &&
                                        rhsR != observedValRanges.end() && rhsR->second.lower >= 1) {
                                        // Only safe if every write to s inside forILi targets Load(idxLs->second)
                                        bool onlyIdxWrite = true;
                                        for (const auto& w2 : writes) {
                                            if (w2.arr != s || !cfg.loops[forILi].body.contains(w2.block)) continue;
                                            auto ls2 = loadSlot.find(w2.op->inputs[1].id);
                                            if (ls2 == loadSlot.end() || ls2->second != idxLs->second) { onlyIdxWrite = false; break; }
                                        }
                                        if (onlyIdxWrite) {
                                            bool ancestorOk = true;
                                            std::int64_t totalTrips = iTrips;
                                            for (std::size_t oi = 0; oi < cfg.loops.size(); ++oi) {
                                                if (static_cast<int>(oi) == forILi) continue;
                                                if (cfg.loops[oi].body.contains(cfg.loops[forILi].header)) {
                                                    auto ofit = forHeaders.find(cfg.loops[oi].header);
                                                    auto olr = ofit != forHeaders.end() ? observedValRanges.find(ofit->second.lower.id) : observedValRanges.end();
                                                    auto our = ofit != forHeaders.end() ? observedValRanges.find(ofit->second.upper.id) : observedValRanges.end();
                                                    if (ofit == forHeaders.end() || !ofit->second.valid ||
                                                        olr == observedValRanges.end() || our == observedValRanges.end() ||
                                                        olr->second.lower < 0 || our->second.upper > maxArrayLength) {
                                                        ancestorOk = false; break;
                                                    }
                                                    auto ot = std::max<std::int64_t>(0, our->second.upper - olr->second.lower);
                                                    auto mul = checkedMul64(totalTrips, ot);
                                                    if (!mul || *mul > 1000000000LL) { ancestorOk = false; break; }
                                                    totalTrips = *mul;
                                                }
                                            }
                                            if (ancestorOk) {
                                                if (auto bump = checkedMul64(totalTrips, std::max<std::int64_t>(0, dit->second.upper))) {
                                                    dpBounds.push_back({chainLoops, *bump, true, dit->second.lower});
                                                    for (int cl : chainLoops) nonAssignLoops.insert(cl);
                                                    handled = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (!handled) minWrites.push_back(w);
                    continue;
                }

                if (handledRefillOps.contains(w.op)) continue;

                // Self-min subtraction: s[k] = s[k] - min(C, s[k]) with C >= 0 preserves [0, cur.upper]
                if (rdef != defOp.end() && rdef->second->op == Op::Binary && rdef->second->data == "-" &&
                    isSelfLoad(rdef->second->inputs[0])) {
                    auto idxLs = loadSlot.find(w.op->inputs[1].id);
                    auto selfDef = defOp.find(rdef->second->inputs[0].id);
                    auto selfIdxLs = (selfDef != defOp.end() && selfDef->second->op == Op::Index)
                        ? loadSlot.find(selfDef->second->inputs[1].id) : loadSlot.end();
                    if (idxLs != loadSlot.end() && selfIdxLs != loadSlot.end() && idxLs->second == selfIdxLs->second) {
                        const Operation* minOpPtr = nullptr;
                        auto rhsDef = defOp.find(rdef->second->inputs[1].id);
                        if (rhsDef != defOp.end() && rhsDef->second->op == Op::Min) {
                            minOpPtr = rhsDef->second;
                        } else if (rhsDef != defOp.end() && rhsDef->second->op == Op::Load) {
                            const unsigned ms = rhsDef->second->slot;
                            const auto& bOps = fn.blocks[w.block].ops;
                            int wPos = -1, mPos = -1;
                            for (int oi = 0; oi < static_cast<int>(bOps.size()); ++oi) {
                                if (&bOps[oi] == w.op) { wPos = oi; break; }
                                if (bOps[oi].op == Op::Store && bOps[oi].slot == ms && !bOps[oi].inputs.empty()) {
                                    auto md = defOp.find(bOps[oi].inputs[0].id);
                                    if (md != defOp.end() && md->second->op == Op::Min) {
                                        minOpPtr = md->second; mPos = oi;
                                    } else minOpPtr = nullptr;
                                }
                                if (mPos >= 0 && ((bOps[oi].op == Op::Store && bOps[oi].slot == idxLs->second) ||
                                                   bOps[oi].op == Op::IndexSet)) {
                                    minOpPtr = nullptr;
                                }
                            }
                            if (wPos < 0 || mPos < 0) minOpPtr = nullptr;
                        }
                        if (minOpPtr && minOpPtr->inputs.size() == 2) {
                            for (int side = 0; side < 2; ++side) {
                                if (isSelfAtSlot(minOpPtr->inputs[side], idxLs->second)) {
                                    auto cr = observedValRanges.find(minOpPtr->inputs[1 - side].id);
                                    if (cr != observedValRanges.end() && cr->second.lower >= 0) {
                                        requiresNonNegMinSub = true;
                                        handled = true;
                                        break;
                                    }
                                }
                            }
                            if (handled) continue;
                        }
                    }
                }

                // Self-refill clamped array: b1: s[k] = s[k] - dec; if (s[k] < T) -> b2: s[k] = s[k] + R
                if (rdef != defOp.end() && rdef->second->op == Op::Binary && rdef->second->data == "-" &&
                    isSelfLoad(rdef->second->inputs[0]) && cfg.loopOfBlock[w.block] >= 0) {
                    auto decIt = observedValRanges.find(rdef->second->inputs[1].id);
                    auto idxLs = loadSlot.find(w.op->inputs[1].id);
                    auto selfDef = defOp.find(rdef->second->inputs[0].id);
                    auto selfIdxLs = (selfDef != defOp.end() && selfDef->second->op == Op::Index)
                        ? loadSlot.find(selfDef->second->inputs[1].id) : loadSlot.end();
                    const auto& blk1 = fn.blocks[w.block];
                    if (decIt != observedValRanges.end() && decIt->second.lower >= 0 && decIt->second.upper < kMaxInt &&
                        idxLs != loadSlot.end() && selfIdxLs != loadSlot.end() && idxLs->second == selfIdxLs->second &&
                        blk1.exit == Exit::Branch && blk1.value && blk1.yes < fn.blocks.size() &&
                        blk1.yes != blk1.no && cfg.preds[blk1.yes].size() == 1 && cfg.preds[blk1.yes][0] == w.block) {
                        const unsigned refillB = blk1.yes;
                        auto cmp = defOp.find(blk1.value->id);
                        std::optional<std::int64_t> minT;
                        if (cmp != defOp.end() && cmp->second->op == Op::Binary &&
                            (cmp->second->data == "<" || cmp->second->data == "<=") &&
                            isSelfAtSlot(cmp->second->inputs[0], idxLs->second)) {
                            if (auto tc = parseConstInt(defOp.count(cmp->second->inputs[1].id)
                                                        ? defOp.at(cmp->second->inputs[1].id)->data : "")) {
                                minT = (cmp->second->data == "<") ? *tc : (*tc + 1);
                            }
                        }
                        bool clean1 = false;
                        for (const auto& op1 : blk1.ops) {
                            if (&op1 == w.op) { clean1 = true; continue; }
                            if (clean1 && ((op1.op == Op::Store && op1.slot == idxLs->second) || op1.op == Op::IndexSet)) {
                                clean1 = false; break;
                            }
                        }
                        const Operation* wRefill = nullptr;
                        bool clean2 = true;
                        for (const auto& op2 : fn.blocks[refillB].ops) {
                            if (op2.op == Op::Store && op2.slot == idxLs->second) { clean2 = false; break; }
                            if (op2.op == Op::IndexSet && !op2.inputs.empty()) {
                                auto a2 = loadSlot.find(op2.inputs[0].id);
                                if (a2 != loadSlot.end() && a2->second == s) {
                                    if (wRefill) { clean2 = false; break; }
                                    wRefill = &op2;
                                }
                            }
                        }
                        if (clean1 && clean2 && minT && *minT >= decIt->second.upper && wRefill) {
                            auto rLs = loadSlot.find(wRefill->inputs[1].id);
                            auto rDef = defOp.find(wRefill->inputs[2].id);
                            if (rLs != loadSlot.end() && rLs->second == idxLs->second &&
                                rDef != defOp.end() && rDef->second->op == Op::Binary && rDef->second->data == "+" &&
                                isSelfAtSlot(rDef->second->inputs[0], idxLs->second)) {
                                auto rRng = observedValRanges.find(rDef->second->inputs[1].id);
                                if (rRng != observedValRanges.end() && rRng->second.lower >= decIt->second.upper &&
                                    checkedSub64(*minT, decIt->second.upper) &&
                                    checkedAdd64(*minT - 1, rRng->second.upper)) {
                                    handledRefillOps.insert(wRefill);
                                    refillPairs.push_back({*minT, decIt->second.upper, rRng->second});
                                    nonAssignLoops.insert(cfg.loopOfBlock[w.block]);
                                    nonAssignLoops.insert(cfg.loopOfBlock[refillB]);
                                    continue;
                                }
                            }
                        }
                    }
                }

                // Nested-For incWrite: s[idx] = s[idx] +/- delta
                if (rdef != defOp.end() && rdef->second->op == Op::Binary &&
                    (rdef->second->data == "+" || rdef->second->data == "-") &&
                    isSelfLoad(rdef->second->inputs[0]) && cfg.loopOfBlock[w.block] >= 0) {
                    if (auto chain = enclosingForChain(w.block)) {
                        auto incIt = observedValRanges.find(rdef->second->inputs[1].id);
                        if (incIt != observedValRanges.end()) {
                            auto step = (rdef->second->data == "-")
                                ? ReturnRanges::negateRange(incIt->second)
                                : std::optional<IntRange>{incIt->second};
                            auto prod = step ? ReturnRanges::multiplyRanges({0, chain->second}, *step) : std::optional<IntRange>{};
                            if (prod) {
                                incWrites.push_back({chain->first, *prod});
                                for (int cl : chain->first) nonAssignLoops.insert(cl);
                                continue;
                            }
                        }
                    }
                    // Also check DP-3 forward prefix: d[i] = d[i - 1] + delta inside forILi (with delta >= 0 and all writes in forILi at i)
                    auto idxLs = loadSlot.find(w.op->inputs[1].id);
                    if (rdef->second->data == "+" && idxLs != loadSlot.end()) {
                        int forILi = -1;
                        std::int64_t iTrips = -1;
                        std::vector<int> chainLoops;
                        bool allValidFor = true;
                        for (std::size_t oi = 0; oi < cfg.loops.size(); ++oi) {
                            if (!cfg.loops[oi].body.contains(w.block)) continue;
                            auto fit = forHeaders.find(cfg.loops[oi].header);
                            if (fit == forHeaders.end() || !fit->second.valid) { allValidFor = false; break; }
                            chainLoops.push_back(static_cast<int>(oi));
                            if (fit->second.iterSlot == idxLs->second) {
                                auto lr = observedValRanges.find(fit->second.lower.id);
                                auto ur = observedValRanges.find(fit->second.upper.id);
                                if (lr != observedValRanges.end() && ur != observedValRanges.end() &&
                                    lr->second.lower >= 0 && ur->second.upper <= maxArrayLength) {
                                    forILi = static_cast<int>(oi);
                                    iTrips = std::max<std::int64_t>(0, ur->second.upper - lr->second.lower);
                                }
                            }
                        }
                        if (allValidFor && forILi >= 0 && iTrips >= 0) {
                            auto pDef = defOp.find(rdef->second->inputs[0].id);
                            auto idxPrev = (pDef != defOp.end() && pDef->second->op == Op::Index) ? pDef->second->inputs[1] : Value{};
                            auto ipDef = defOp.find(idxPrev.id);
                            auto dit = observedValRanges.find(rdef->second->inputs[1].id);
                            if (dit != observedValRanges.end() && dit->second.lower >= 0 &&
                                ipDef != defOp.end() && ipDef->second->op == Op::Binary && ipDef->second->data == "-") {
                                auto lhsS = loadSlot.find(ipDef->second->inputs[0].id);
                                auto rhsR = observedValRanges.find(ipDef->second->inputs[1].id);
                                if (lhsS != loadSlot.end() && lhsS->second == idxLs->second &&
                                    rhsR != observedValRanges.end() && rhsR->second.lower >= 1) {
                                    bool onlyIdxWrite = true;
                                    for (const auto& w2 : writes) {
                                        if (w2.arr != s || !cfg.loops[forILi].body.contains(w2.block)) continue;
                                        auto ls2 = loadSlot.find(w2.op->inputs[1].id);
                                        if (ls2 == loadSlot.end() || ls2->second != idxLs->second) { onlyIdxWrite = false; break; }
                                    }
                                    if (onlyIdxWrite) {
                                        bool ancestorOk = true;
                                        std::int64_t totalTrips = iTrips;
                                        for (std::size_t oi = 0; oi < cfg.loops.size(); ++oi) {
                                            if (static_cast<int>(oi) == forILi) continue;
                                            if (cfg.loops[oi].body.contains(cfg.loops[forILi].header)) {
                                                auto ofit = forHeaders.find(cfg.loops[oi].header);
                                                auto olr = ofit != forHeaders.end() ? observedValRanges.find(ofit->second.lower.id) : observedValRanges.end();
                                                auto our = ofit != forHeaders.end() ? observedValRanges.find(ofit->second.upper.id) : observedValRanges.end();
                                                if (ofit == forHeaders.end() || !ofit->second.valid ||
                                                    olr == observedValRanges.end() || our == observedValRanges.end() ||
                                                    olr->second.lower < 0 || our->second.upper > maxArrayLength) {
                                                    ancestorOk = false; break;
                                                }
                                                auto ot = std::max<std::int64_t>(0, our->second.upper - olr->second.lower);
                                                auto mul = checkedMul64(totalTrips, ot);
                                                if (!mul || *mul > 1000000000LL) { ancestorOk = false; break; }
                                                totalTrips = *mul;
                                            }
                                        }
                                        if (ancestorOk) {
                                            if (auto bump = checkedMul64(totalTrips, std::max<std::int64_t>(0, dit->second.upper))) {
                                                dpBounds.push_back({chainLoops, *bump, true, dit->second.lower});
                                                for (int cl : chainLoops) nonAssignLoops.insert(cl);
                                                continue;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ok = false;
                    break;
                }

                // DP-1 (backward-while max-plus) & DP-2 (forward-prefix for max-plus)
                if (rdef != defOp.end() && rdef->second->op == Op::Max &&
                    cfg.loopOfBlock[w.block] >= 0) {
                    auto idxLs = loadSlot.find(w.op->inputs[1].id);
                    if (idxLs != loadSlot.end()) {
                        const unsigned idxSlot = idxLs->second;
                        for (int side = 0; side < 2 && !handled; ++side) {
                            if (!isSelfAtSlot(rdef->second->inputs[side], idxSlot)) continue;
                            const Value cand = rdef->second->inputs[1 - side];
                            Value prevLoad{};
                            std::optional<IntRange> deltaRng;
                            if (isSelfLoad(cand)) {
                                prevLoad = cand;
                                deltaRng = IntRange{0, 0};
                            } else if (auto cdef = defOp.find(cand.id); cdef != defOp.end() &&
                                       cdef->second->op == Op::Binary && cdef->second->data == "+") {
                                if (isSelfLoad(cdef->second->inputs[0])) {
                                    if (auto dr = observedValRanges.find(cdef->second->inputs[1].id); dr != observedValRanges.end()) {
                                        prevLoad = cdef->second->inputs[0];
                                        deltaRng = dr->second;
                                    }
                                } else if (isSelfLoad(cdef->second->inputs[1])) {
                                    if (auto dr = observedValRanges.find(cdef->second->inputs[0].id); dr != observedValRanges.end()) {
                                        prevLoad = cdef->second->inputs[1];
                                        deltaRng = dr->second;
                                    }
                                }
                            }
                            if (!deltaRng) continue;
                            auto pIdxOp = defOp.find(prevLoad.id);
                            if (pIdxOp == defOp.end() || pIdxOp->second->op != Op::Index) continue;
                            const Value prevIdx = pIdxOp->second->inputs[1];
                            const int liW = cfg.loopOfBlock[w.block];

                            // DP-1: inner while strictly decreasing idxSlot by 1, prevIdx = idxSlot - step (>= 1), outer loops all valid For
                            if (!forHeaders.count(cfg.loops[liW].header)) {
                                // Verify idxSlot strictly decreases by 1 inside liW, no other IndexSet on s inside liW
                                bool okJ = true;
                                int jStores = 0;
                                for (unsigned lb : cfg.loops[liW].body) {
                                    for (const auto& op : fn.blocks[lb].ops) {
                                        if (op.op == Op::Store && op.slot == idxSlot && !op.inputs.empty()) {
                                            if (lb == cfg.loops[liW].header) { okJ = false; break; }
                                            ++jStores;
                                            auto jd = defOp.find(op.inputs[0].id);
                                            auto one = jd != defOp.end() && jd->second->inputs.size() == 2
                                                ? parseConstInt(defOp.count(jd->second->inputs[1].id) ? defOp.at(jd->second->inputs[1].id)->data : "")
                                                : std::optional<std::int64_t>{};
                                            auto jls = jd != defOp.end() && jd->second->inputs.size() >= 1 ? loadSlot.find(jd->second->inputs[0].id) : loadSlot.end();
                                            if (jd == defOp.end() || jd->second->op != Op::Binary || jd->second->data != "-" ||
                                                jls == loadSlot.end() || jls->second != idxSlot || one != 1) {
                                                okJ = false; break;
                                            }
                                        }
                                    }
                                    if (!okJ) break;
                                }
                                int wCntInLi = 0;
                                for (const auto& w2 : writes)
                                    if (w2.arr == s && cfg.loops[liW].body.contains(w2.block)) ++wCntInLi;
                                auto piDef = defOp.find(prevIdx.id);
                                if (okJ && jStores == 1 && wCntInLi == 1 &&
                                    piDef != defOp.end() && piDef->second->op == Op::Binary && piDef->second->data == "-") {
                                    auto jLs2 = loadSlot.find(piDef->second->inputs[0].id);
                                    auto stepR = observedValRanges.find(piDef->second->inputs[1].id);
                                    auto stepLs = loadSlot.find(piDef->second->inputs[1].id);
                                    const bool stepInv = (stepLs == loadSlot.end() || !loopWritten[liW].contains(stepLs->second));
                                    if (jLs2 != loadSlot.end() && jLs2->second == idxSlot && stepInv &&
                                        stepR != observedValRanges.end() && stepR->second.lower >= 1) {
                                        if (auto outerChain = enclosingForChain(cfg.loops[liW].header, liW)) {
                                            bool cleanOuter = true;
                                            for (const auto& w2 : writes) {
                                                if (w2.arr != s || cfg.loops[liW].body.contains(w2.block)) continue;
                                                for (int ol : outerChain->first) {
                                                    if (cfg.loops[ol].body.contains(w2.block)) { cleanOuter = false; break; }
                                                }
                                                if (!cleanOuter) break;
                                            }
                                            if (cleanOuter) {
                                                if (auto bump = checkedMul64(outerChain->second, std::max<std::int64_t>(0, deltaRng->upper))) {
                                                    std::vector<int> allL = outerChain->first;
                                                    allL.push_back(liW);
                                                    dpBounds.push_back({allL, *bump, false, 0});
                                                    for (int cl : allL) nonAssignLoops.insert(cl);
                                                    handled = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                // DP-2: forward-prefix For loop on idxSlot, prevIdx < idxSlot, all writes in forILi at idxSlot
                                int forILi = -1;
                                std::int64_t iTrips = -1;
                                std::vector<int> chainLoops;
                                bool allValidFor = true;
                                for (std::size_t oi = 0; oi < cfg.loops.size(); ++oi) {
                                    if (!cfg.loops[oi].body.contains(w.block)) continue;
                                    auto fit = forHeaders.find(cfg.loops[oi].header);
                                    if (fit == forHeaders.end() || !fit->second.valid) { allValidFor = false; break; }
                                    chainLoops.push_back(static_cast<int>(oi));
                                    if (fit->second.iterSlot == idxSlot) {
                                        auto lr = observedValRanges.find(fit->second.lower.id);
                                        auto ur = observedValRanges.find(fit->second.upper.id);
                                        if (lr != observedValRanges.end() && ur != observedValRanges.end() &&
                                            lr->second.lower >= 0 && ur->second.upper <= maxArrayLength) {
                                            forILi = static_cast<int>(oi);
                                            iTrips = std::max<std::int64_t>(0, ur->second.upper - lr->second.lower);
                                        }
                                    }
                                }
                                if (allValidFor && forILi >= 0 && iTrips >= 0) {
                                    bool onlyIdxWrite = true;
                                    for (const auto& w2 : writes) {
                                        if (w2.arr != s || !cfg.loops[forILi].body.contains(w2.block)) continue;
                                        auto ls2 = loadSlot.find(w2.op->inputs[1].id);
                                        if (ls2 == loadSlot.end() || ls2->second != idxSlot) { onlyIdxWrite = false; break; }
                                    }
                                    bool prevStrictlyLt = false;
                                    if (auto piDef = defOp.find(prevIdx.id); piDef != defOp.end() &&
                                        piDef->second->op == Op::Binary && piDef->second->data == "-") {
                                        auto lhsS = loadSlot.find(piDef->second->inputs[0].id);
                                        auto rhsR = observedValRanges.find(piDef->second->inputs[1].id);
                                        if (lhsS != loadSlot.end() && lhsS->second == idxSlot &&
                                            rhsR != observedValRanges.end() && rhsR->second.lower >= 1)
                                            prevStrictlyLt = true;
                                    } else if (auto pls = loadSlot.find(prevIdx.id); pls != loadSlot.end()) {
                                        for (std::size_t oi = 0; oi < cfg.loops.size(); ++oi) {
                                            if (!cfg.loops[oi].body.contains(w.block)) continue;
                                            auto fit = forHeaders.find(cfg.loops[oi].header);
                                            if (fit != forHeaders.end() && fit->second.valid && fit->second.iterSlot == pls->second) {
                                                auto uls = loadSlot.find(fit->second.upper.id);
                                                if (uls != loadSlot.end() && uls->second == idxSlot) prevStrictlyLt = true;
                                            }
                                        }
                                    }
                                    if (onlyIdxWrite && prevStrictlyLt) {
                                        bool ancestorOk = true;
                                        std::int64_t totalTrips = iTrips;
                                        for (std::size_t oi = 0; oi < cfg.loops.size(); ++oi) {
                                            if (static_cast<int>(oi) == forILi) continue;
                                            if (cfg.loops[oi].body.contains(cfg.loops[forILi].header)) {
                                                auto ofit = forHeaders.find(cfg.loops[oi].header);
                                                auto olr = ofit != forHeaders.end() ? observedValRanges.find(ofit->second.lower.id) : observedValRanges.end();
                                                auto our = ofit != forHeaders.end() ? observedValRanges.find(ofit->second.upper.id) : observedValRanges.end();
                                                if (ofit == forHeaders.end() || !ofit->second.valid ||
                                                    olr == observedValRanges.end() || our == observedValRanges.end() ||
                                                    olr->second.lower < 0 || our->second.upper > maxArrayLength) {
                                                    ancestorOk = false; break;
                                                }
                                                auto ot = std::max<std::int64_t>(0, our->second.upper - olr->second.lower);
                                                auto mul = checkedMul64(totalTrips, ot);
                                                if (!mul || *mul > 1000000000LL) { ancestorOk = false; break; }
                                                totalTrips = *mul;
                                            }
                                        }
                                        if (ancestorOk) {
                                            if (auto bump = checkedMul64(totalTrips, std::max<std::int64_t>(0, deltaRng->upper))) {
                                                dpBounds.push_back({chainLoops, *bump, false, 0});
                                                for (int cl : chainLoops) nonAssignLoops.insert(cl);
                                                handled = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (handled) continue;
                    }
                }

                // Row-Carry DP: nested for outer / inner j { old = s[j]; s[j] = carry + C; ...; carry = old }
                if (rdef != defOp.end() && rdef->second->op == Op::Binary && rdef->second->data == "+" &&
                    cfg.loopOfBlock[w.block] >= 0) {
                    auto idxLs = loadSlot.find(w.op->inputs[1].id);
                    if (idxLs != loadSlot.end()) {
                        int innerLi = -1, outerLi = -1;
                        std::vector<int> chainLoops;
                        bool validNest = true;
                        for (std::size_t oi = 0; oi < cfg.loops.size(); ++oi) {
                            if (!cfg.loops[oi].body.contains(w.block)) continue;
                            auto fit = forHeaders.find(cfg.loops[oi].header);
                            if (fit == forHeaders.end() || !fit->second.valid) { validNest = false; break; }
                            chainLoops.push_back(static_cast<int>(oi));
                            if (fit->second.iterSlot == idxLs->second) innerLi = static_cast<int>(oi);
                            else outerLi = static_cast<int>(oi);
                        }
                        if (validNest && chainLoops.size() == 2 && innerLi >= 0 && outerLi >= 0 &&
                            cfg.loops[outerLi].body.contains(cfg.loops[innerLi].header)) {
                            const auto& innerFh = forHeaders.at(cfg.loops[innerLi].header);
                            const auto& outerFh = forHeaders.at(cfg.loops[outerLi].header);
                            auto olr = observedValRanges.find(outerFh.lower.id);
                            auto our = observedValRanges.find(outerFh.upper.id);
                            if (olr != observedValRanges.end() && our != observedValRanges.end() &&
                                olr->second.lower >= 0 && our->second.upper <= maxArrayLength) {
                                const std::int64_t outerTrips = std::max<std::int64_t>(0, our->second.upper - olr->second.lower);
                                for (int side = 0; side < 2 && !handled; ++side) {
                                    auto cls = loadSlot.find(rdef->second->inputs[side].id);
                                    auto dr = observedValRanges.find(rdef->second->inputs[1 - side].id);
                                    if (cls == loadSlot.end() || dr == observedValRanges.end() || dr->second.lower < 0)
                                        continue;
                                    const unsigned carryS = cls->second;
                                    bool onlyIdxWrite = true;
                                    for (const auto& w2 : writes) {
                                        if (w2.arr != s || !cfg.loops[innerLi].body.contains(w2.block)) continue;
                                        auto ls2 = loadSlot.find(w2.op->inputs[1].id);
                                        if (ls2 == loadSlot.end() || ls2->second != idxLs->second) { onlyIdxWrite = false; break; }
                                    }
                                    if (!onlyIdxWrite) continue;
                                    std::optional<IntRange> initCarry;
                                    int outerStores = 0, innerStores = 0;
                                    std::optional<unsigned> oldSlot;
                                    for (unsigned ob : cfg.loops[outerLi].body) {
                                        const bool inInner = cfg.loops[innerLi].body.contains(ob);
                                        for (const auto& op : fn.blocks[ob].ops) {
                                            if (op.op != Op::Store || op.slot != carryS || op.inputs.empty()) continue;
                                            if (!inInner) {
                                                ++outerStores;
                                                auto ir = observedValRanges.find(op.inputs[0].id);
                                                if (ir == observedValRanges.end()) { initCarry.reset(); outerStores = 99; break; }
                                                initCarry = initCarry ? ReturnRanges::envelope(*initCarry, ir->second) : ir->second;
                                            } else {
                                                ++innerStores;
                                                auto ols = loadSlot.find(op.inputs[0].id);
                                                if (ols == loadSlot.end()) innerStores = 99;
                                                else oldSlot = ols->second;
                                            }
                                        }
                                    }
                                    if (outerStores < 1 || outerStores >= 99 || innerStores != 1 || !initCarry || !oldSlot)
                                        continue;
                                    // Verify oldSlot has 1 store in innerLi inside innerFh.bodyBlock BEFORE any IndexSet(s, ...)
                                    int oldStores = 0;
                                    bool okOld = false;
                                    for (unsigned ib : cfg.loops[innerLi].body) {
                                        for (const auto& op : fn.blocks[ib].ops) {
                                            if (op.op == Op::Store && op.slot == *oldSlot && !op.inputs.empty()) {
                                                ++oldStores;
                                                if (ib == innerFh.bodyBlock && isSelfAtSlot(op.inputs[0], idxLs->second)) {
                                                    okOld = true;
                                                    for (const auto& opPrev : fn.blocks[ib].ops) {
                                                        if (&opPrev == &op) break;
                                                        if (opPrev.op == Op::IndexSet) { okOld = false; break; }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    if (okOld && oldStores == 1) {
                                        if (auto bump = checkedMul64(outerTrips, std::max<std::int64_t>(0, dr->second.upper))) {
                                            dpExtraInit = dpExtraInit
                                                ? ReturnRanges::envelope(*dpExtraInit, *initCarry) : *initCarry;
                                            dpBounds.push_back({chainLoops, *bump, true, dr->second.lower});
                                            for (int cl : chainLoops) nonAssignLoops.insert(cl);
                                            handled = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (handled) continue;
                    }
                }
                assignWrites.push_back(w);
            }
            if (!ok) continue;

            // Reject if any assignWrite occurs inside an incWrite/DP loop
            for (const auto& w : assignWrites) {
                const int wLi = cfg.loopOfBlock[w.block];
                if (wLi >= 0 && nonAssignLoops.contains(wLi)) { ok = false; break; }
            }
            if (!ok) continue;

            std::optional<IntRange> cur;
            if (cloneSrc[s]) {
                auto cit = globalElemRanges.find(*cloneSrc[s]);
                if (cit == globalElemRanges.end() || !cit->second) continue;
                cur = *cit->second;
            } else if (repeatOp[s]->initialize) {
                auto it = observedValRanges.find(repeatOp[s]->inputs[0].id);
                if (it == observedValRanges.end()) continue;
                cur = it->second;
            }
            if (dpExtraInit) {
                cur = cur ? ReturnRanges::envelope(*cur, *dpExtraInit) : *dpExtraInit;
            }
            for (const auto& w : assignWrites) {
                auto vit = observedValRanges.find(w.op->inputs[2].id);
                if (vit == observedValRanges.end()) { ok = false; break; }
                cur = cur ? ReturnRanges::envelope(*cur, vit->second) : vit->second;
            }
            if (!ok || !cur) continue;
            if (requiresNonNegMinSub) {
                if (cur->lower < 0) continue;
                cur->lower = 0; // x - min(C, x) reaches 0 whenever x <= C
            }

            for (const auto& iw : incWrites) {
                auto sum = ReturnRanges::addRanges(*cur, iw.deltaRange);
                if (!sum) { ok = false; break; }
                cur = ReturnRanges::envelope(*cur, *sum);
            }
            if (!ok) continue;

            for (const auto& db : dpBounds) {
                if (db.requiresNonNegDelta && db.minDeltaLo < 0) { ok = false; break; }
                if (auto up = checkedAdd64(cur->upper, db.upperBump)) cur->upper = *up;
                else { ok = false; break; }
            }
            if (!ok) continue;

            for (const auto& rp : refillPairs) {
                if (cur->lower < rp.minT) { ok = false; break; }
                auto loC = checkedSub64(rp.minT, rp.maxD);
                auto hiC = checkedAdd64(rp.minT - 1, rp.rRng.upper);
                if (!loC || !hiC) { ok = false; break; }
                cur->lower = std::min(cur->lower, *loC);
                cur->upper = std::max(cur->upper, *hiC);
            }
            if (!ok) continue;

            for (const auto& w : minWrites) {
                auto rdef = defOp.find(w.op->inputs[2].id);
                Value other = isSelfLoad(rdef->second->inputs[0])
                    ? rdef->second->inputs[1] : rdef->second->inputs[0];
                std::optional<IntRange> orng;
                if (auto oit = observedValRanges.find(other.id); oit != observedValRanges.end()) {
                    orng = oit->second;
                } else if (auto odef = defOp.find(other.id); odef != defOp.end() && odef->second->op == Op::Binary &&
                           odef->second->data == "+" && isSelfLoad(odef->second->inputs[0]) &&
                           isSelfLoad(odef->second->inputs[1])) {
                    orng = ReturnRanges::addRanges(*cur, *cur);
                }
                if (!orng) { ok = false; break; }
                cur = IntRange{std::min(cur->lower, orng->lower), cur->upper};
            }
            if (ok && cur) globalElemRanges[static_cast<unsigned>(s)] = *cur;
        }
        // Fixed-point arrElemLtSlot: IntArrays initialized to 0 where every write stores 0 or (non-neg expr) % same immutable N
        {
            std::unordered_map<unsigned, unsigned> candModN;
            for (std::size_t s = 0; s < nSlots; ++s) {
                if (fn.slots[s] != Type::IntArray || originCount[s] != 1 || bad[s] || !repeatOp[s]) continue;
                auto initC = parseConstInt(defOp.count(repeatOp[s]->inputs[0].id) ? defOp.at(repeatOp[s]->inputs[0].id)->data : "");
                if (initC != 0) continue;
                std::optional<unsigned> commonDiv;
                bool okShape = true;
                for (const auto& w : writes) {
                    if (w.arr != s) continue;
                    const Value rhs = w.op->inputs[2];
                    auto rObs = observedValRanges.find(rhs.id);
                    if (rObs != observedValRanges.end() && rObs->second.lower == 0 && rObs->second.upper == 0) continue;
                    auto rd = defOp.find(rhs.id);
                    Value mv = rhs;
                    if (rd != defOp.end() && rd->second->op == Op::Load) {
                        const Operation* singleSt = nullptr;
                        int stCnt = 0;
                        for (const auto& blk : fn.blocks)
                            for (const auto& op : blk.ops)
                                if (op.op == Op::Store && op.slot == rd->second->slot) { ++stCnt; singleSt = &op; }
                        if (stCnt == 1 && singleSt && !singleSt->inputs.empty()) {
                            mv = singleSt->inputs[0];
                            rd = defOp.find(mv.id);
                        }
                    }
                    if (rd != defOp.end() && rd->second->op == Op::Binary && rd->second->data == "%") {
                        auto dls = loadSlot.find(rd->second->inputs[1].id);
                        if (dls != loadSlot.end() && !slotWritable[dls->second]) {
                            if (!commonDiv) commonDiv = dls->second;
                            else if (*commonDiv != dls->second) { okShape = false; break; }
                            continue;
                        }
                    }
                    okShape = false; break;
                }
                if (okShape && commonDiv) candModN[static_cast<unsigned>(s)] = *commonDiv;
            }
            auto exprIsNonNeg = [&](auto& self, Value v, int depth, std::unordered_set<unsigned>& activeSlots) -> bool {
                if (depth > 16) return false;
                if (auto o = observedValRanges.find(v.id); o != observedValRanges.end() && o->second.lower >= 0)
                    return true;
                auto d = defOp.find(v.id);
                if (d == defOp.end()) return false;
                const Operation* op = d->second;
                if (op->op == Op::Int) { auto c = parseConstInt(op->data); return c && *c >= 0; }
                if (op->op == Op::Index && !op->inputs.empty()) {
                    if (auto als = loadSlot.find(op->inputs[0].id); als != loadSlot.end()) {
                        if (candModN.contains(als->second)) return true;
                        if (auto g = globalElemRanges.find(als->second);
                            g != globalElemRanges.end() && g->second && g->second->lower >= 0)
                            return true;
                    }
                    return false;
                }
                if (op->op == Op::Load) {
                    if (!activeSlots.insert(op->slot).second) return false;
                    bool okSt = false;
                    int cnt = 0;
                    for (const auto& blk : fn.blocks) {
                        for (const auto& sop : blk.ops) {
                            if (sop.op == Op::Store && sop.slot == op->slot && !sop.inputs.empty()) {
                                ++cnt;
                                if (!self(self, sop.inputs[0], depth + 1, activeSlots)) {
                                    activeSlots.erase(op->slot);
                                    return false;
                                }
                                okSt = true;
                            }
                        }
                    }
                    activeSlots.erase(op->slot);
                    return okSt && cnt >= 1;
                }
                if (op->op == Op::Binary && (op->data == "+" || op->data == "*" || op->data == "%")) {
                    if (op->data == "%") {
                        auto dls = loadSlot.find(op->inputs[1].id);
                        const bool divNn = (observedValRanges.count(op->inputs[1].id) && observedValRanges.at(op->inputs[1].id).lower >= 0) ||
                                           (dls != loadSlot.end() && !slotWritable[dls->second]);
                        return divNn && self(self, op->inputs[0], depth + 1, activeSlots);
                    }
                    return self(self, op->inputs[0], depth + 1, activeSlots) &&
                           self(self, op->inputs[1], depth + 1, activeSlots);
                }
                return false;
            };
            bool changed = true;
            while (changed && !candModN.empty()) {
                changed = false;
                for (auto it = candModN.begin(); it != candModN.end(); ) {
                    const unsigned s = it->first;
                    bool keep = true;
                    for (const auto& w : writes) {
                        if (w.arr != s) continue;
                        const Value rhs = w.op->inputs[2];
                        auto rObs = observedValRanges.find(rhs.id);
                        if (rObs != observedValRanges.end() && rObs->second.lower == 0 && rObs->second.upper == 0) continue;
                        std::unordered_set<unsigned> activeSlots;
                        if (!exprIsNonNeg(exprIsNonNeg, rhs, 0, activeSlots)) { keep = false; break; }
                    }
                    if (!keep) { it = candModN.erase(it); changed = true; }
                    else ++it;
                }
            }
            for (const auto& [s, divS] : candModN) arrElemLtSlot[s] = divS;
        }
        // Set tight element ranges on uniqueZeroSetLoops seen-marker arrays (each non-zero slot created <= N times)
        for (const auto& [li, maxN] : activeUniqueZeroMaxN) {
            const auto& uz = uniqueZeroSetLoops.at(li);
            if (!globalElemRanges.count(uz.seenArr)) {
                globalElemRanges[uz.seenArr] = IntRange{0, uz.initCount + maxN};
            }
        }
        // Keep prover state member accessor available for post-prover loop transforms
    }
public:
    const std::unordered_map<unsigned, std::optional<IntRange>>& elemRanges() const { return globalElemRanges; }
    const std::unordered_map<unsigned, IntRange>& valRanges() const { return observedValRanges; }
    const std::unordered_map<std::size_t, std::vector<std::pair<unsigned, unsigned>>>& modLtDiv() const { return modSlotLtDiv; }
};

} // namespace detail

// ---------------------------------------------------------------------------
// Wave 3B Post-Prover Loop Transforms (Strict Parity-Preserving)
// ---------------------------------------------------------------------------
inline void optimizeMinPlusInnerLoops(
    FunctionIR& fn,
    const std::unordered_map<unsigned, std::optional<IntRange>>& elemRanges) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::unordered_map<unsigned, std::pair<unsigned, std::size_t>> defPos;
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, unsigned> loadSlot;
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        for (std::size_t oi = 0; oi < fn.blocks[bi].ops.size(); ++oi) {
            const auto& op = fn.blocks[bi].ops[oi];
            if (op.result.type != Type::Void) {
                defPos[op.result.id] = {bi, oi};
                defOp[op.result.id] = &op;
                defBlock[op.result.id] = bi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
        }
    }

    struct Plan {
        unsigned preBlk = 0, hdrBlk = 0, bodyBlk = 0, exitBlk = 0;
        unsigned arrSlot = 0, jSlot = 0;
        Value jLower{}, jUpper{};
        Span loopSpan{};
        std::size_t dikIndexPos = 0;
        Operation dikIndexTemplate{};
        Value invIdx{};
        Value baseI{}, baseK{};
        std::unordered_set<unsigned> loopWritten;
        bool disjointRows = false;
    };
    std::vector<Plan> plans;

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lp = cfg.loops[li];
        if (lp.body.size() != 2 || lp.preheaders.size() != 1) continue;
        const unsigned hdr = lp.header;
        const unsigned pre = lp.preheaders[0];
        const auto& hblk = fn.blocks[hdr];
        if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
            continue;
        if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != hdr) continue;
        const unsigned body = hblk.yes;
        const unsigned exitBlk = hblk.no;
        if (fn.blocks[body].exit != Exit::Jump || fn.blocks[body].yes != hdr) continue;

        auto cdit = defOp.find(hblk.value->id);
        if (cdit == defOp.end() || cdit->second->op != Op::Binary || cdit->second->data != "<") continue;
        auto jls = loadSlot.find(cdit->second->inputs[0].id);
        if (jls == loadSlot.end()) continue;
        const unsigned jSlot = jls->second;
        const Value jUpper = cdit->second->inputs[1];
        if (defBlock.count(jUpper.id) && lp.body.contains(defBlock[jUpper.id])) continue;

        // Verify jSlot initialized in pre and updated only by rangeIncrement(jSlot, 1)
        std::optional<Value> jLower;
        for (const auto& op : fn.blocks[pre].ops)
            if (op.op == Op::Store && op.slot == jSlot && !op.inputs.empty()) jLower = op.inputs[0];
        if (!jLower || (defBlock.count(jLower->id) && lp.body.contains(defBlock[jLower->id]))) continue;

        std::unordered_set<unsigned> written;
        int jIncs = 0;
        bool jOnlyInc = true;
        for (unsigned lb : lp.body) {
            for (const auto& op : fn.blocks[lb].ops) {
                if (op.op == Op::Store) {
                    written.insert(op.slot);
                    if (op.slot == jSlot) {
                        ++jIncs;
                        auto jd = defOp.find(op.inputs[0].id);
                        if (jd == defOp.end() || jd->second->op != Op::Binary || jd->second->data != "rangeIncrement")
                            jOnlyInc = false;
                    }
                }
            }
        }
        if (!jOnlyInc || jIncs != 1) continue;

        // Verify body has no Calls, all checks proven, and exactly one IndexSet (Min-Plus on non-negative array)
        bool safeBody = true;
        int idxSetCount = 0;
        std::size_t idxSetPos = 0;
        for (std::size_t oi = 0; oi < fn.blocks[body].ops.size(); ++oi) {
            const auto& op = fn.blocks[body].ops[oi];
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin || op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                op.op == Op::Print || (op.op == Op::Assert && !op.boundsProven && !op.unchecked) ||
                op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice || op.op == Op::FieldSet || op.op == Op::PlaceSet) { safeBody = false; break; }
            if (op.op == Op::Index) {
                if (!op.boundsProven || op.inputs[0].type != Type::IntArray) { safeBody = false; break; }
            } else if (op.op == Op::IndexSet) {
                ++idxSetCount;
                idxSetPos = oi;
                if (!op.boundsProven || op.inputs[0].type != Type::IntArray) { safeBody = false; break; }
            } else if (op.op == Op::Negate) {
                if (op.result.type != Type::Int || !op.overflowProven) { safeBody = false; break; }
            } else if (op.op == Op::Binary) {
                if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                     op.data == "<<" || op.data == ">>" || op.data == ">>>") &&
                    (op.inputs[0].type != Type::Int || !op.overflowProven)) {
                    safeBody = false; break;
                }
                if (op.data == "/" || op.data == "%") { safeBody = false; break; }
            }
        }
        if (!safeBody || idxSetCount != 1) continue;

        const Operation& setOp = fn.blocks[body].ops[idxSetPos];
        auto als = loadSlot.find(setOp.inputs[0].id);
        if (als == loadSlot.end()) continue;
        const unsigned arrSlot = als->second;
        auto erIt = elemRanges.find(arrSlot);
        if (erIt == elemRanges.end() || !erIt->second || erIt->second->lower < 0) continue;

        auto usesInvariantInLoop = [&](auto& self, Value v, int depth) -> bool {
            if (depth > 32) return false;
            auto dit = defOp.find(v.id);
            if (dit == defOp.end()) return false;
            const Operation* o = dit->second;
            if (o->op == Op::Int || o->op == Op::Bool) return true;
            if (o->op == Op::Load) return !written.contains(o->slot) && o->slot != arrSlot;
            if (o->op == Op::Negate || (o->op == Op::Binary &&
                (o->data == "+" || o->data == "-" || o->data == "*"))) {
                if (defBlock[v.id] != body) return false;
                for (const auto& in : o->inputs)
                    if (!self(self, in, depth + 1)) return false;
                return true;
            }
            return false;
        };

        // Resolve idxTarget -> base_i + Load(jSlot)
        auto splitBasePlusJ = [&](Value idx) -> std::optional<Value> {
            Value cur = idx;
            auto dit = defOp.find(cur.id);
            if (dit != defOp.end() && dit->second->op == Op::Load) {
                // Chase single-store local in body
                const unsigned ts = dit->second->slot;
                int sc = 0;
                for (const auto& op : fn.blocks[body].ops) {
                    if (op.op == Op::Store && op.slot == ts && !op.inputs.empty()) {
                        ++sc; cur = op.inputs[0];
                    }
                }
                if (sc != 1) return {};
                dit = defOp.find(cur.id);
            }
            if (dit == defOp.end() || dit->second->op != Op::Binary || dit->second->data != "+") return {};
            for (int side = 0; side < 2; ++side) {
                auto j2 = loadSlot.find(dit->second->inputs[side].id);
                if (j2 != loadSlot.end() && j2->second == jSlot) {
                    Value base = dit->second->inputs[1 - side];
                    if (usesInvariantInLoop(usesInvariantInLoop, base, 0)) return base;
                }
            }
            return {};
        };

        auto baseI = splitBasePlusJ(setOp.inputs[1]);
        if (!baseI) continue;

        auto minDef = defOp.find(setOp.inputs[2].id);
        if (minDef == defOp.end() || minDef->second->op != Op::Min || minDef->second->inputs.size() != 2) continue;
        std::optional<Value> baseK, invIdx;
        std::size_t dikPos = 0;
        Operation dikOpTemplate{};
        bool matchedMin = false;

        for (int side = 0; side < 2 && !matchedMin; ++side) {
            auto tDef = defOp.find(minDef->second->inputs[side].id);
            auto sDef = defOp.find(minDef->second->inputs[1 - side].id);
            if (tDef == defOp.end() || tDef->second->op != Op::Index ||
                sDef == defOp.end() || sDef->second->op != Op::Binary || sDef->second->data != "+")
                continue;
            auto tArr = loadSlot.find(tDef->second->inputs[0].id);
            if (tArr == loadSlot.end() || tArr->second != arrSlot) continue;
            auto tBase = splitBasePlusJ(tDef->second->inputs[1]);
            if (!tBase || tBase->id != baseI->id) continue; // target reads d[base_i + j]

            // sDef = d[invIdx] + d[base_k + j]
            for (int ss = 0; ss < 2; ++ss) {
                auto aDef = defOp.find(sDef->second->inputs[ss].id);
                auto bDef = defOp.find(sDef->second->inputs[1 - ss].id);
                if (aDef == defOp.end() || aDef->second->op != Op::Index ||
                    bDef == defOp.end() || bDef->second->op != Op::Index)
                    continue;
                auto aArr = loadSlot.find(aDef->second->inputs[0].id);
                auto bArr = loadSlot.find(bDef->second->inputs[0].id);
                if (aArr == loadSlot.end() || aArr->second != arrSlot ||
                    bArr == loadSlot.end() || bArr->second != arrSlot)
                    continue;
                if (!usesInvariantInLoop(usesInvariantInLoop, aDef->second->inputs[1], 0)) continue;
                auto bk = splitBasePlusJ(bDef->second->inputs[1]);
                if (!bk) continue;

                // Locate position of aDef in body.ops
                for (std::size_t kPos = 0; kPos < fn.blocks[body].ops.size(); ++kPos) {
                    if (&fn.blocks[body].ops[kPos] == aDef->second) {
                        dikPos = kPos;
                        dikOpTemplate = *aDef->second;
                        invIdx = aDef->second->inputs[1];
                        baseK = bk;
                        matchedMin = true;
                        break;
                    }
                }
                if (matchedMin) break;
            }
        }
        if (matchedMin && baseK && invIdx) {
            auto hasStrideFactor = [&](Value v, unsigned strideSlot) -> bool {
                auto dit = defOp.find(v.id);
                if (dit == defOp.end() || dit->second->op != Op::Binary || dit->second->data != "*") return false;
                for (int side = 0; side < 2; ++side) {
                    auto ls = loadSlot.find(dit->second->inputs[side].id);
                    if (ls != loadSlot.end() && ls->second == strideSlot) return true;
                }
                return false;
            };
            auto loD = defOp.find(jLower->id);
            const bool loZero = (loD != defOp.end() && loD->second->op == Op::Int && loD->second->data == "0");
            auto uSlotIt = loadSlot.find(jUpper.id);
            const bool disjointRows = loZero && uSlotIt != loadSlot.end() && !written.contains(uSlotIt->second) &&
                hasStrideFactor(*baseI, uSlotIt->second) && hasStrideFactor(*baseK, uSlotIt->second);
            plans.push_back({pre, hdr, body, exitBlk, arrSlot, jSlot, *jLower, jUpper, setOp.span,
                             dikPos, dikOpTemplate, *invIdx, *baseI, *baseK, std::move(written), disjointRows});
        }
    }

    int nextAg = 0;
    for (const auto& blk : fn.blocks) {
        nextAg = std::max(nextAg, blk.loopAccessGroup + 1);
        for (const auto& op : blk.ops) nextAg = std::max(nextAg, op.loopAccessGroup + 1);
    }

    for (const auto& p : plans) {
        if (fn.blocks[p.preBlk].exit != Exit::Jump || fn.blocks[p.preBlk].yes != p.hdrBlk) continue;
        const unsigned dikSlot = static_cast<unsigned>(fn.slots.size());
        fn.slots.push_back(Type::Int);
        if (fn.slotElemBits.size() == dikSlot) {
            fn.slotElemBits.push_back(64);
            fn.slotElemUnsigned.push_back(false);
        }
        if (fn.slotAliasGroup.size() == dikSlot) fn.slotAliasGroup.push_back(-1);
        if (fn.slotByRef.size() == dikSlot) fn.slotByRef.push_back(false);
        if (fn.slotOwned.size() == dikSlot) fn.slotOwned.push_back(false);

        const unsigned hoistBlk = static_cast<unsigned>(fn.blocks.size());
        fn.blocks.resize(hoistBlk + 1);

        auto cloneExpr = [&](auto& self, Value v, std::vector<Operation>& dstOps,
                             std::unordered_map<unsigned, Value>& memo, Span fallbackSpan) -> Value {
            if (auto it = memo.find(v.id); it != memo.end()) return it->second;
            auto dpit = defPos.find(v.id);
            if (dpit == defPos.end()) return v;
            const Operation src = fn.blocks[dpit->second.first].ops[dpit->second.second];
            if ((src.op != Op::Load && src.op != Op::Int && dpit->second.first != p.bodyBlk)) {
                return v;
            }
            Operation cloned = src;
            for (auto& in : cloned.inputs) in = self(self, in, dstOps, memo, fallbackSpan);
            cloned.result.id = fn.nextValue++;
            dstOps.push_back(cloned);
            memo.emplace(v.id, cloned.result);
            return cloned.result;
        };

        // Guard zero-trip loops (jLower < jUpper) before reading arr[invIdx].
        // Since elemRanges[arrSlot] >= 0 is globally proven, even if invIdx == baseI + j on one iteration k,
        // min(d[invIdx], d[invIdx] + d[baseK + k]) == d[invIdx], so d[invIdx] is invariant across the loop.
        const Value cmpLoHi{Type::Bool, fn.nextValue++};
        fn.blocks[p.preBlk].ops.push_back({Op::Binary, cmpLoHi, {p.jLower, p.jUpper}, "<", 0, p.loopSpan});
        fn.blocks[p.preBlk].exit = Exit::Branch;
        fn.blocks[p.preBlk].value = cmpLoHi;
        fn.blocks[p.preBlk].yes = hoistBlk;
        fn.blocks[p.preBlk].no = p.exitBlk;

        // hoistBlk: load arr[invIdx] -> Store(dikSlot), Jump(hdrBlk)
        std::unordered_map<unsigned, Value> memo2;
        Value idxV = cloneExpr(cloneExpr, p.invIdx, fn.blocks[hoistBlk].ops, memo2, p.dikIndexTemplate.span);
        const Value arrV{Type::IntArray, fn.nextValue++};
        fn.blocks[hoistBlk].ops.push_back({Op::Load, arrV, {}, {}, p.arrSlot, p.dikIndexTemplate.span});
        Operation idxOp = p.dikIndexTemplate;
        idxOp.result = {Type::Int, fn.nextValue++};
        idxOp.inputs = {arrV, idxV};
        idxOp.boundsProven = true;
        fn.blocks[hoistBlk].ops.push_back(idxOp);
        fn.blocks[hoistBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {idxOp.result}, {}, dikSlot, idxOp.span});
        fn.blocks[hoistBlk].exit = Exit::Jump;
        fn.blocks[hoistBlk].yes = p.hdrBlk;

        // Replace dikIndexOp in bodyBlk with Load(dikSlot)
        Operation& target = fn.blocks[p.bodyBlk].ops[p.dikIndexPos];
        target = {Op::Load, target.result, {}, {}, dikSlot, target.span};

        // Prove disjoint-or-equal row access (baseI = rowA * N, baseK = rowB * N, 0 <= j < N):
        // either rowA == rowB (same index j) or |baseI - baseK| >= N (non-overlapping rows).
        if (p.disjointRows) {
            const int ag = nextAg++;
            for (auto& op : fn.blocks[p.bodyBlk].ops) {
                if (op.op == Op::Index || op.op == Op::IndexSet) op.loopAccessGroup = ag;
            }
            fn.blocks[p.bodyBlk].loopHint = LoopHint::VectorizeParallel;
            fn.blocks[p.bodyBlk].loopAccessGroup = ag;
            fn.blocks[p.bodyBlk].noInterleave = true;
        }
    }
}

inline void promoteInvariantArrayElement(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    const std::size_t nSlots = fn.slots.size();
    std::vector<int> originCount(nSlots, 0);
    std::vector<bool> badArr(nSlots, false);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, unsigned> loadSlot;

    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const auto& op : fn.blocks[bi].ops) {
            if (op.result.type != Type::Void) {
                defOp[op.result.id] = &op;
                defBlock[op.result.id] = bi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
            if (op.op == Op::Store && !op.inputs.empty() && op.slot < nSlots && fn.slots[op.slot] == Type::IntArray) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && (d->second->op == Op::RepeatRegion || d->second->op == Op::RepeatHeap)) {
                    ++originCount[op.slot];
                } else {
                    badArr[op.slot] = true;
                    if (auto src = loadSlot.find(op.inputs[0].id); src != loadSlot.end()) badArr[src->second] = true;
                }
            }
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                for (const auto& in : op.inputs)
                    if (in.type == Type::IntArray)
                        if (auto ls = loadSlot.find(in.id); ls != loadSlot.end()) badArr[ls->second] = true;
            }
            if ((op.op == Op::ClearOwner || op.op == Op::ClearField) && op.slot < nSlots) badArr[op.slot] = true;
            if (op.op == Op::ClearIndex && !op.inputs.empty()) {
                if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end() && ls->second < nSlots)
                    badArr[ls->second] = true;
            }
        }
        if (fn.blocks[bi].exit == Exit::Return && fn.blocks[bi].value && fn.blocks[bi].value->type == Type::IntArray) {
            if (auto ls = loadSlot.find(fn.blocks[bi].value->id); ls != loadSlot.end()) badArr[ls->second] = true;
        }
    }

    struct Plan {
        unsigned preBlk = 0, hdrBlk = 0, exitBlk = 0;
        unsigned arrSlot = 0, iSlot = 0, jSlot = 0;
        Value jLower{}, jUpper{};
        Span hdrSpan{};
        std::unordered_set<unsigned> bodyBlocks;
        Operation setTemplate{};
    };
    std::vector<Plan> plans;

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lp = cfg.loops[li];
        if (lp.preheaders.size() != 1) continue;
        const unsigned hdr = lp.header;
        const unsigned pre = lp.preheaders[0];
        const auto& hblk = fn.blocks[hdr];
        if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
            continue;
        if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != hdr) continue;
        const unsigned exitBlk = hblk.no;

        auto cdit = defOp.find(hblk.value->id);
        if (cdit == defOp.end() || cdit->second->op != Op::Binary || cdit->second->data != "<") continue;
        auto jls = loadSlot.find(cdit->second->inputs[0].id);
        if (jls == loadSlot.end()) continue;
        const unsigned jSlot = jls->second;
        const Value jUpper = cdit->second->inputs[1];
        if (defBlock.count(jUpper.id) && lp.body.contains(defBlock[jUpper.id])) continue;

        std::optional<Value> jLower;
        for (const auto& op : fn.blocks[pre].ops)
            if (op.op == Op::Store && op.slot == jSlot && !op.inputs.empty()) jLower = op.inputs[0];
        if (!jLower || (defBlock.count(jLower->id) && lp.body.contains(defBlock[jLower->id]))) continue;

        // Verify jSlot updated only via rangeIncrement and record all written slots in lp.body
        std::unordered_set<unsigned> written;
        int jIncs = 0;
        bool cleanLoop = true;
        for (unsigned lb : lp.body) {
            for (const auto& op : fn.blocks[lb].ops) {
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin || op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                    op.op == Op::Print || (op.op == Op::Assert && !op.boundsProven && !op.unchecked) ||
                    op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice || op.op == Op::FieldSet || op.op == Op::PlaceSet ||
                    op.op == Op::ClearOwner || op.op == Op::ClearField || op.op == Op::ClearIndex) { cleanLoop = false; break; }
                if (op.op == Op::Store) {
                    written.insert(op.slot);
                    if (op.slot == jSlot) {
                        ++jIncs;
                        auto jd = defOp.find(op.inputs[0].id);
                        if (jd == defOp.end() || jd->second->op != Op::Binary || jd->second->data != "rangeIncrement")
                            cleanLoop = false;
                    }
                }
            }
            if (!cleanLoop) break;
        }
        if (!cleanLoop || jIncs != 1) continue;

        // Check constant lower of jSlot (if >= 1) and whether upper == Load(upperSlot)
        auto jLowConst = [&](Value v) -> std::optional<std::int64_t> {
            auto d = defOp.find(v.id);
            if (d != defOp.end() && d->second->op == Op::Int) return parseConstInt(d->second->data);
            return {};
        };
        const auto lowerC = jLowConst(*jLower);
        std::optional<unsigned> upperSlot;
        if (auto uls = loadSlot.find(jUpper.id); uls != loadSlot.end()) upperSlot = uls->second;

        // Inspect array Index/IndexSet ops inside lp.body
        std::optional<unsigned> targetArr, targetISlot;
        Operation setTemplate{};
        bool okPromo = true;
        int setCount = 0;

        for (unsigned lb : lp.body) {
            for (const auto& op : fn.blocks[lb].ops) {
                if (op.op != Op::IndexSet) continue;
                if (lb != hblk.yes) { okPromo = false; break; }
                auto als = loadSlot.find(op.inputs[0].id);
                if (als == loadSlot.end() || !op.boundsProven) { okPromo = false; break; }
                const unsigned as = als->second;
                if (as >= nSlots || originCount[as] != 1 || badArr[as] || written.contains(as)) { okPromo = false; break; }
                auto ils = loadSlot.find(op.inputs[1].id);
                if (ils == loadSlot.end() || ils->second == jSlot || written.contains(ils->second)) { okPromo = false; break; }
                if (!targetArr) {
                    targetArr = as;
                    targetISlot = ils->second;
                    setTemplate = op;
                } else if (*targetArr != as || *targetISlot != ils->second) {
                    okPromo = false; break;
                }
                ++setCount;
            }
            if (!okPromo) break;
        }
        if (!okPromo || setCount < 1 || !targetArr || !targetISlot) continue;
        // Require no potentially-trapping earlier operations in hblk.yes before setOp, so hoisting loadBlk is parity-safe.
        for (const auto& op : fn.blocks[hblk.yes].ops) {
            if (&op == &fn.blocks[hblk.yes].ops.back()) break;
            if (op.op == Op::IndexSet) break;
            if ((op.op == Op::Index && !op.boundsProven) ||
                ((op.op == Op::Negate || op.op == Op::AbsDiff) && op.result.type == Type::Int && !op.overflowProven) ||
                (op.op == Op::Binary && op.inputs[0].type == Type::Int &&
                 ((op.data == "+" || op.data == "-" || op.data == "*" ||
                   op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven)) ||
                (op.op == Op::Binary && ((op.data == "/" || op.data == "%") && !op.divSafe))) {
                okPromo = false; break;
            }
        }
        if (!okPromo) continue;

        // Ensure iSlot is never compared in lp.body except hdr's j < upper
        for (unsigned lb : lp.body) {
            for (const auto& op : fn.blocks[lb].ops) {
                if (op.op == Op::Binary &&
                    (op.data == "<" || op.data == "<=" || op.data == ">" || op.data == ">=" ||
                     op.data == "==" || op.data == "!=") &&
                    &op != cdit->second) {
                    for (const auto& in : op.inputs) {
                        if (auto ls = loadSlot.find(in.id); ls != loadSlot.end() && ls->second == *targetISlot) {
                            okPromo = false; break;
                        }
                    }
                }
                if (!okPromo) break;
            }
            if (!okPromo) break;
        }
        if (!okPromo) continue;

        // Every Index on *targetArr in lp.body must either use *targetISlot OR be provably != *targetISlot
        for (unsigned lb : lp.body) {
            for (const auto& op : fn.blocks[lb].ops) {
                if (op.op != Op::Index) continue;
                auto als = loadSlot.find(op.inputs[0].id);
                if (als == loadSlot.end() || als->second != *targetArr) continue;
                const Value iv = op.inputs[1];
                if (auto ils = loadSlot.find(iv.id); ils != loadSlot.end()) {
                    if (ils->second == *targetISlot) continue; // replaced with accSlot!
                    if (ils->second == jSlot && upperSlot && *upperSlot == *targetISlot) continue; // j < i
                    okPromo = false; break;
                }
                if (auto idf = defOp.find(iv.id); idf != defOp.end() && idf->second->op == Op::Binary && idf->second->data == "-") {
                    auto lhsS = loadSlot.find(idf->second->inputs[0].id);
                    if (lhsS != loadSlot.end() && lhsS->second == *targetISlot) {
                        auto rLs = loadSlot.find(idf->second->inputs[1].id);
                        auto rC = jLowConst(idf->second->inputs[1]);
                        if ((rC && *rC >= 1) || (rLs != loadSlot.end() && rLs->second == jSlot && lowerC && *lowerC >= 1))
                            continue; // i - pos < i
                    }
                }
                okPromo = false;
                break;
            }
            if (!okPromo) break;
        }
        if (okPromo) {
            plans.push_back({pre, hdr, exitBlk, *targetArr, *targetISlot, jSlot,
                             *jLower, jUpper, cdit->second->span, lp.body, setTemplate});
        }
    }

    for (const auto& p : plans) {
        if (fn.blocks[p.preBlk].exit != Exit::Jump || fn.blocks[p.preBlk].yes != p.hdrBlk) continue;
        const unsigned accSlot = static_cast<unsigned>(fn.slots.size());
        fn.slots.push_back(Type::Int);
        if (fn.slotElemBits.size() == accSlot) {
            fn.slotElemBits.push_back(64);
            fn.slotElemUnsigned.push_back(false);
        }
        if (fn.slotAliasGroup.size() == accSlot) fn.slotAliasGroup.push_back(-1);

        const unsigned loadBlk = static_cast<unsigned>(fn.blocks.size());
        const unsigned storeBlk = loadBlk + 1;
        fn.blocks.resize(storeBlk + 1);

        // preBlk: if jLower < jUpper -> loadBlk else exitBlk
        const Value cmp{Type::Bool, fn.nextValue++};
        fn.blocks[p.preBlk].ops.push_back({Op::Binary, cmp, {p.jLower, p.jUpper}, "<", 0, p.hdrSpan});
        fn.blocks[p.preBlk].exit = Exit::Branch;
        fn.blocks[p.preBlk].value = cmp;
        fn.blocks[p.preBlk].yes = loadBlk;
        fn.blocks[p.preBlk].no = p.exitBlk;

        // loadBlk: accSlot = arr[iSlot] -> hdrBlk
        const Value a0{Type::IntArray, fn.nextValue++};
        const Value i0{Type::Int, fn.nextValue++};
        const Value elemV{Type::Int, fn.nextValue++};
        Operation idxOp{Op::Index, elemV, {a0, i0}, "", 0, p.setTemplate.span};
        idxOp.boundsProven = true;
        idxOp.elemBits = p.setTemplate.elemBits;
        idxOp.elemUnsigned = p.setTemplate.elemUnsigned;
        idxOp.aliasGroup = p.setTemplate.aliasGroup;
        fn.blocks[loadBlk].ops.push_back({Op::Load, a0, {}, {}, p.arrSlot, p.setTemplate.span});
        fn.blocks[loadBlk].ops.push_back({Op::Load, i0, {}, {}, p.iSlot, p.setTemplate.span});
        fn.blocks[loadBlk].ops.push_back(idxOp);
        fn.blocks[loadBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {elemV}, {}, accSlot, p.setTemplate.span});
        fn.blocks[loadBlk].exit = Exit::Jump;
        fn.blocks[loadBlk].yes = p.hdrBlk;

        // storeBlk: arr[iSlot] = accSlot -> exitBlk
        const Value a1{Type::IntArray, fn.nextValue++};
        const Value i1{Type::Int, fn.nextValue++};
        const Value accV{Type::Int, fn.nextValue++};
        Operation setOp = p.setTemplate;
        setOp.result = {Type::Void, fn.nextValue++};
        setOp.inputs = {a1, i1, accV};
        setOp.boundsProven = true;
        fn.blocks[storeBlk].ops.push_back({Op::Load, a1, {}, {}, p.arrSlot, p.setTemplate.span});
        fn.blocks[storeBlk].ops.push_back({Op::Load, i1, {}, {}, p.iSlot, p.setTemplate.span});
        fn.blocks[storeBlk].ops.push_back({Op::Load, accV, {}, {}, accSlot, p.setTemplate.span});
        fn.blocks[storeBlk].ops.push_back(setOp);
        fn.blocks[storeBlk].exit = Exit::Jump;
        fn.blocks[storeBlk].yes = p.exitBlk;

        fn.blocks[p.hdrBlk].no = storeBlk;

        // Rewrite Index(arr, iSlot) / IndexSet(arr, iSlot, v) inside loop body to accSlot
        for (unsigned lb : p.bodyBlocks) {
            for (auto& op : fn.blocks[lb].ops) {
                if (op.op == Op::Index) {
                    auto als = loadSlot.find(op.inputs[0].id);
                    auto ils = loadSlot.find(op.inputs[1].id);
                    if (als != loadSlot.end() && als->second == p.arrSlot &&
                        ils != loadSlot.end() && ils->second == p.iSlot) {
                        op = {Op::Load, op.result, {}, {}, accSlot, op.span};
                    }
                } else if (op.op == Op::IndexSet) {
                    auto als = loadSlot.find(op.inputs[0].id);
                    auto ils = loadSlot.find(op.inputs[1].id);
                    if (als != loadSlot.end() && als->second == p.arrSlot &&
                        ils != loadSlot.end() && ils->second == p.iSlot) {
                        op = {Op::Store, {Type::Void, fn.nextValue++}, {op.inputs[2]}, {}, accSlot, op.span};
                    }
                }
            }
        }
    }
}

/// For 3-level nested loops (`i`, `j`, `k`) reading an invariant `IntArray` `b[k * stride + j]`,
/// specialize large-enough cases with a region-scoped transpose `bt[j * K + k] = b[k * stride + j]`
/// before loop `i` so `unrollAndJamMatMul` vectorizes unit-stride `bt` loads instead of strided gathers.
inline void transposeStridedMatMul(
    FunctionIR& fn,
    std::unordered_map<unsigned, std::optional<IntRange>>& elemRanges) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    const std::size_t nSlots = fn.slots.size();
    std::vector<int> originCount(nSlots, 0);
    std::vector<bool> badArr(nSlots, false);
    std::unordered_map<unsigned, Operation> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, unsigned> loadSlot;

    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const auto& op : fn.blocks[bi].ops) {
            if (op.result.type != Type::Void) {
                defOp.emplace(op.result.id, op);
                defBlock[op.result.id] = bi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
            if (op.op == Op::Store && !op.inputs.empty() && op.slot < nSlots && fn.slots[op.slot] == Type::IntArray) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && (d->second.op == Op::RepeatRegion || d->second.op == Op::RepeatHeap)) {
                    ++originCount[op.slot];
                } else {
                    badArr[op.slot] = true;
                    if (auto src = loadSlot.find(op.inputs[0].id); src != loadSlot.end()) badArr[src->second] = true;
                }
            }
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                for (const auto& in : op.inputs) {
                    if (in.type == Type::IntArray) {
                        if (auto ls = loadSlot.find(in.id); ls != loadSlot.end()) badArr[ls->second] = true;
                    }
                }
            }
            if ((op.op == Op::ClearOwner || op.op == Op::ClearField) && op.slot < nSlots) badArr[op.slot] = true;
            if (op.op == Op::ClearIndex && !op.inputs.empty()) {
                if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end() && ls->second < nSlots)
                    badArr[ls->second] = true;
            }
        }
        if (fn.blocks[bi].exit == Exit::Return && fn.blocks[bi].value && fn.blocks[bi].value->type == Type::IntArray) {
            if (auto ls = loadSlot.find(fn.blocks[bi].value->id); ls != loadSlot.end()) badArr[ls->second] = true;
        }
    }

    struct BReadRef {
        unsigned blk = 0;
        std::size_t opIdx = 0;
        Operation opTemplate{};
    };
    struct Plan {
        unsigned preI = 0, hdrI = 0, iSlot = 0, jSlot = 0, kSlot = 0, bSlot = 0;
        Value iLower{}, iUpper{}, jUpper{}, kUpper{}, strideVal{};
        Span span{};
        std::unordered_set<unsigned> lpIBody;
        std::vector<BReadRef> bReads;
    };
    std::vector<Plan> plans;
    std::unordered_set<unsigned> usedOuterHeaders;

    for (std::size_t liI = 0; liI < cfg.loops.size(); ++liI) {
        const auto& lpI = cfg.loops[liI];
        if (lpI.preheaders.size() != 1) continue;
        const unsigned hdrI = lpI.header;
        if (usedOuterHeaders.contains(hdrI)) continue;
        const unsigned preI = lpI.preheaders[0];
        if (cfg.loopOfBlock[preI] >= 0) continue; // Only outermost loops for single-per-call region alloc
        bool insideAnotherLoop = false;
        for (std::size_t other = 0; other < cfg.loops.size(); ++other) {
            if (other != liI && cfg.loops[other].body.contains(hdrI)) {
                insideAnotherLoop = true;
                break;
            }
        }
        if (insideAnotherLoop) continue;
        if (fn.blocks[preI].exit != Exit::Jump || fn.blocks[preI].yes != hdrI) continue;
        const auto& hIBlk = fn.blocks[hdrI];
        if (hIBlk.exit != Exit::Branch || !hIBlk.value || !lpI.body.contains(hIBlk.yes) || lpI.body.contains(hIBlk.no))
            continue;
        auto cI = defOp.find(hIBlk.value->id);
        if (cI == defOp.end() || cI->second.op != Op::Binary || cI->second.data != "<") continue;
        auto ils = loadSlot.find(cI->second.inputs[0].id);
        if (ils == loadSlot.end()) continue;
        const unsigned iSlot = ils->second;
        const Value iUpper = cI->second.inputs[1];

        std::optional<Value> iLower;
        for (const auto& op : fn.blocks[preI].ops) {
            if (op.op == Op::Store && op.slot == iSlot && !op.inputs.empty()) iLower = op.inputs[0];
        }
        if (!iLower) continue;

        std::unordered_set<unsigned> writtenInI;
        bool cleanI = true;
        for (unsigned b : lpI.body) {
            for (const auto& op : fn.blocks[b].ops) {
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin ||
                    op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                    op.op == Op::IndexSet || op.op == Op::RepeatHeap || op.op == Op::RepeatRegion || op.op == Op::Clone ||
                    op.op == Op::ClearOwner || op.op == Op::ClearField || op.op == Op::ClearIndex ||
                    op.op == Op::FieldSet || op.op == Op::PlaceSet) {
                    cleanI = false;
                    break;
                }
                if (op.op == Op::Store) writtenInI.insert(op.slot);
            }
            if (!cleanI) break;
        }
        if (!cleanI) continue;

        auto isInvariantExpr = [&](auto& self, Value v, int depth) -> bool {
            if (depth > 16) return false;
            auto db = defBlock.find(v.id);
            if (db != defBlock.end() && lpI.body.contains(db->second)) {
                auto dop = defOp.find(v.id);
                if (dop == defOp.end()) return false;
                const Operation& o = dop->second;
                if (o.op == Op::Int || o.op == Op::Bool) return true;
                if (o.op == Op::Load) return !writtenInI.contains(o.slot);
                if (o.op == Op::Negate || (o.op == Op::Binary && (o.data == "+" || o.data == "-" || o.data == "*"))) {
                    for (const auto& in : o.inputs)
                        if (!self(self, in, depth + 1)) return false;
                    return true;
                }
                return false;
            }
            return true;
        };
        if (!isInvariantExpr(isInvariantExpr, iUpper, 0) || !isInvariantExpr(isInvariantExpr, *iLower, 0)) continue;

        // Locate 2-nested child loop lpJ -> lpK inside lpI
        bool foundPlan = false;
        for (std::size_t liJ = 0; liJ < cfg.loops.size() && !foundPlan; ++liJ) {
            if (liJ == liI) continue;
            const auto& lpJ = cfg.loops[liJ];
            const unsigned hdrJ = lpJ.header;
            if (!lpI.body.contains(hdrJ) || lpJ.preheaders.size() != 1) continue;
            const unsigned preJ = lpJ.preheaders[0];
            if (!lpI.body.contains(preJ) || fn.blocks[preJ].exit != Exit::Jump || fn.blocks[preJ].yes != hdrJ) continue;
            const auto& hJBlk = fn.blocks[hdrJ];
            if (hJBlk.exit != Exit::Branch || !hJBlk.value || !lpJ.body.contains(hJBlk.yes) || lpJ.body.contains(hJBlk.no))
                continue;
            auto cJ = defOp.find(hJBlk.value->id);
            if (cJ == defOp.end() || cJ->second.op != Op::Binary || cJ->second.data != "<") continue;
            auto jls = loadSlot.find(cJ->second.inputs[0].id);
            if (jls == loadSlot.end() || jls->second == iSlot) continue;
            const unsigned jSlot = jls->second;
            const Value jUpper = cJ->second.inputs[1];
            if (!isInvariantExpr(isInvariantExpr, jUpper, 0)) continue;

            std::optional<std::int64_t> jLowC;
            for (const auto& op : fn.blocks[preJ].ops) {
                if (op.op == Op::Store && op.slot == jSlot && !op.inputs.empty()) {
                    auto d = defOp.find(op.inputs[0].id);
                    if (d != defOp.end() && d->second.op == Op::Int) jLowC = parseConstInt(d->second.data);
                }
            }
            if (jLowC != 0) continue;

            const unsigned jInit = hJBlk.yes;
            if (fn.blocks[jInit].exit != Exit::Jump) continue;
            const unsigned hdrK = fn.blocks[jInit].yes;
            auto litK = cfg.headerToLoop.find(hdrK);
            if (litK == cfg.headerToLoop.end() || litK->second == liJ || litK->second == liI) continue;
            const auto& lpK = cfg.loops[litK->second];
            if (lpK.body.size() != 2 || lpK.preheaders.size() != 1 || lpK.preheaders[0] != jInit) continue;
            const auto& hKBlk = fn.blocks[hdrK];
            if (hKBlk.exit != Exit::Branch || !hKBlk.value || !lpK.body.contains(hKBlk.yes) || lpK.body.contains(hKBlk.no))
                continue;
            const unsigned bodyK = hKBlk.yes;
            if (!lpJ.body.contains(bodyK) || fn.blocks[bodyK].exit != Exit::Jump || fn.blocks[bodyK].yes != hdrK)
                continue;

            auto cK = defOp.find(hKBlk.value->id);
            if (cK == defOp.end() || cK->second.op != Op::Binary || cK->second.data != "<") continue;
            auto kls = loadSlot.find(cK->second.inputs[0].id);
            if (kls == loadSlot.end() || kls->second == iSlot || kls->second == jSlot) continue;
            const unsigned kSlot = kls->second;
            const Value kUpper = cK->second.inputs[1];
            if (!isInvariantExpr(isInvariantExpr, kUpper, 0)) continue;

            std::optional<std::int64_t> kLowC;
            for (const auto& op : fn.blocks[jInit].ops) {
                if (op.op == Op::Store && op.slot == kSlot && !op.inputs.empty()) {
                    auto d = defOp.find(op.inputs[0].id);
                    if (d != defOp.end() && d->second.op == Op::Int) kLowC = parseConstInt(d->second.data);
                }
            }
            if (kLowC != 0) continue;

            // Check bodyK is completely check-free so full B[0..K, 0..J] range executes without trapping earlier.
            bool okBodyK = true;
            for (const auto& op : fn.blocks[bodyK].ops) {
                if (op.op == Op::Index && !op.boundsProven) { okBodyK = false; break; }
                if (op.op == Op::Assert && !op.boundsProven && !op.unchecked) { okBodyK = false; break; }
                if (op.op == Op::Negate && op.result.type == Type::Int && !op.overflowProven) { okBodyK = false; break; }
                if (op.op == Op::AbsDiff && !op.overflowProven) { okBodyK = false; break; }
                if (op.op == Op::Binary && op.inputs.size() >= 2 && op.inputs[0].type == Type::Int) {
                    if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                         op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) {
                        okBodyK = false; break;
                    }
                    if ((op.data == "/" || op.data == "%") && !op.divSafe) { okBodyK = false; break; }
                }
            }
            if (!okBodyK) continue;

            // Match Index(b, k * stride + j) in bodyK where b is untouched IntArray
            auto matchKSJ = [&](Value idxV, unsigned& outBSlot, Value& outStride) -> bool {
                auto addIt = defOp.find(idxV.id);
                if (addIt == defOp.end() || addIt->second.op != Op::Binary || addIt->second.data != "+")
                    return false;
                auto resolvesToSlot = [&](Value v, unsigned targetSlot) -> bool {
                    if (auto ls = loadSlot.find(v.id); ls != loadSlot.end()) return ls->second == targetSlot;
                    return false;
                };
                for (int side = 0; side < 2; ++side) {
                    const Value mulCand = addIt->second.inputs[side];
                    const Value jCand = addIt->second.inputs[1 - side];
                    if (!resolvesToSlot(jCand, jSlot)) continue;
                    auto mulIt = defOp.find(mulCand.id);
                    if (mulIt == defOp.end() || mulIt->second.op != Op::Binary || mulIt->second.data != "*")
                        continue;
                    for (int ms = 0; ms < 2; ++ms) {
                        if (resolvesToSlot(mulIt->second.inputs[ms], kSlot)) {
                            const Value strideCand = mulIt->second.inputs[1 - ms];
                            if (isInvariantExpr(isInvariantExpr, strideCand, 0)) {
                                outStride = strideCand;
                                (void)outBSlot;
                                return true;
                            }
                        }
                    }
                }
                return false;
            };

            std::optional<unsigned> chosenBSlot;
            std::optional<Value> chosenStride;
            std::vector<BReadRef> bReads;
            for (std::size_t oi = 0; oi < fn.blocks[bodyK].ops.size(); ++oi) {
                const auto& op = fn.blocks[bodyK].ops[oi];
                if (op.op != Op::Index || !op.boundsProven || op.inputs.size() < 2 || op.inputs[0].type != Type::IntArray)
                    continue;
                auto bls = loadSlot.find(op.inputs[0].id);
                if (bls == loadSlot.end()) continue;
                const unsigned bSlot = bls->second;
                if (bSlot >= nSlots || originCount[bSlot] != 1 || badArr[bSlot] || writtenInI.contains(bSlot))
                    continue;
                unsigned dummy = 0;
                Value strideV{};
                if (!matchKSJ(op.inputs[1], dummy, strideV)) continue;
                if (!chosenBSlot) {
                    chosenBSlot = bSlot;
                    chosenStride = strideV;
                    bReads.push_back({bodyK, oi, op});
                } else if (*chosenBSlot == bSlot && chosenStride->id == strideV.id) {
                    bReads.push_back({bodyK, oi, op});
                }
            }
            if (!chosenBSlot || bReads.empty()) continue;

            // Ensure hdrI.yes reaches hdrJ without skipping unconditionally or complex side branches
            if (fn.blocks[hIBlk.yes].exit == Exit::Jump && fn.blocks[hIBlk.yes].yes == hdrJ) {
                usedOuterHeaders.insert(hdrI);
                plans.push_back({preI, hdrI, iSlot, jSlot, kSlot, *chosenBSlot, *iLower, iUpper, jUpper, kUpper,
                                 *chosenStride, cJ->second.span, lpI.body, std::move(bReads)});
                foundPlan = true;
            }
        }
    }

    for (const auto& p : plans) {
        if (fn.blocks[p.preI].exit != Exit::Jump || fn.blocks[p.preI].yes != p.hdrI) continue;
        const std::uint8_t bBits = p.bReads.front().opTemplate.elemBits;
        const bool bUns = p.bReads.front().opTemplate.elemUnsigned;
        const Span sp = p.span;

        if (!fn.usesRegion) {
            const Value markV{Type::Void, fn.nextValue++};
            fn.blocks[0].ops.insert(fn.blocks[0].ops.begin(), {Op::RegionMark, markV, {}, {}, 0, sp});
            for (auto& blk : fn.blocks) {
                if (blk.exit == Exit::Return) {
                    blk.ops.push_back({Op::RegionRewind, {Type::Void, fn.nextValue++}, {markV}, {}, 0, sp});
                }
            }
            fn.usesRegion = true;
        }

        auto addSlot = [&](Type t, std::uint8_t bits, bool uns) -> unsigned {
            const unsigned s = static_cast<unsigned>(fn.slots.size());
            fn.slots.push_back(t);
            if (!fn.slotByRef.empty()) fn.slotByRef.push_back(false);
            if (!fn.slotElemBits.empty()) fn.slotElemBits.push_back(bits);
            if (!fn.slotElemUnsigned.empty()) fn.slotElemUnsigned.push_back(uns);
            if (!fn.slotAliasGroup.empty()) fn.slotAliasGroup.push_back(-1);
            if (!fn.slotOwned.empty()) fn.slotOwned.push_back(false);
            return s;
        };
        const unsigned btSlot = addSlot(Type::IntArray, bBits, bUns);
        const unsigned kStrideSlot = addSlot(Type::Int, 64, false);
        const unsigned tkSlot = addSlot(Type::Int, 64, false);
        const unsigned tjSlot = addSlot(Type::Int, 64, false);
        if (auto it = elemRanges.find(p.bSlot); it != elemRanges.end() && it->second) {
            elemRanges[btSlot] = *it->second;
        }

        // Helper to emit an invariant value into a target block (cloning ops defined in fn outside lpI if needed)
        auto emitInv = [&](Value v, std::vector<Operation>& outOps, std::unordered_map<unsigned, Value>& memo) -> Value {
            auto cloneRec = [&](auto& self, Value cur, int depth) -> Value {
                if (depth > 16) return cur;
                if (auto mit = memo.find(cur.id); mit != memo.end()) return mit->second;
                auto db = defBlock.find(cur.id);
                if (db == defBlock.end()) return cur;
                if (db->second != p.preI && !p.lpIBody.contains(db->second)) return cur;
                auto dop = defOp.find(cur.id);
                if (dop == defOp.end()) return cur;
                Operation copy = dop->second;
                if (copy.op != Op::Int && copy.op != Op::Bool && copy.op != Op::Load &&
                    copy.op != Op::Negate && copy.op != Op::Binary) {
                    return cur;
                }
                for (auto& in : copy.inputs) in = self(self, in, depth + 1);
                copy.result.id = fn.nextValue++;
                outOps.push_back(copy);
                memo[cur.id] = copy.result;
                return copy.result;
            };
            return cloneRec(cloneRec, v, 0);
        };

        const unsigned transAllocBlk = static_cast<unsigned>(fn.blocks.size());
        const unsigned transKHdr = transAllocBlk + 1;
        const unsigned transJHdr = transAllocBlk + 2;
        const unsigned transJBody = transAllocBlk + 3;
        const unsigned transKTail = transAllocBlk + 4;
        fn.blocks.resize(transKTail + 1);

        // Guard in preI: iLower >= 0 && iUpper >= 8 && iLower <= iUpper - 8 &&
        //                jUpper >= 16 && jUpper <= 512 && kUpper >= 16 && kUpper <= 512 && jUpper <= stride
        auto emitBranchChain = [&](unsigned curBlk, Value condVal, unsigned nextCheckBlk) {
            fn.blocks[curBlk].exit = Exit::Branch;
            fn.blocks[curBlk].value = condVal;
            fn.blocks[curBlk].yes = nextCheckBlk;
            fn.blocks[curBlk].no = p.hdrI;
        };

        std::unordered_map<unsigned, Value> preMemo;
        const Value iUpV = emitInv(p.iUpper, fn.blocks[p.preI].ops, preMemo);
        const Value iLoV = emitInv(p.iLower, fn.blocks[p.preI].ops, preMemo);
        const Value jUpV = emitInv(p.jUpper, fn.blocks[p.preI].ops, preMemo);
        const Value kUpV = emitInv(p.kUpper, fn.blocks[p.preI].ops, preMemo);
        const Value stV = emitInv(p.strideVal, fn.blocks[p.preI].ops, preMemo);

        const Value c0{Type::Int, fn.nextValue++};
        const Value c8{Type::Int, fn.nextValue++};
        const Value c16{Type::Int, fn.nextValue++};
        const Value c512{Type::Int, fn.nextValue++};
        fn.blocks[p.preI].ops.push_back({Op::Int, c0, {}, "0", 0, sp});
        fn.blocks[p.preI].ops.push_back({Op::Int, c8, {}, "8", 0, sp});
        fn.blocks[p.preI].ops.push_back({Op::Int, c16, {}, "16", 0, sp});
        fn.blocks[p.preI].ops.push_back({Op::Int, c512, {}, "512", 0, sp});

        const Value g1{Type::Bool, fn.nextValue++};
        const Value g2{Type::Bool, fn.nextValue++};
        const Value g12{Type::Bool, fn.nextValue++};
        fn.blocks[p.preI].ops.push_back({Op::Binary, g1, {iLoV, c0}, ">=", 0, sp});
        fn.blocks[p.preI].ops.push_back({Op::Binary, g2, {iUpV, c8}, ">=", 0, sp});
        fn.blocks[p.preI].ops.push_back({Op::Binary, g12, {g1, g2}, "&", 0, sp});

        const unsigned guard2Blk = static_cast<unsigned>(fn.blocks.size());
        fn.blocks.emplace_back();
        emitBranchChain(p.preI, g12, guard2Blk);

        // Inside guard2Blk: iUpper >= 8 is now true, so iUpper - 8 cannot underflow
        const Value iUpM8{Type::Int, fn.nextValue++};
        Operation sub8{Op::Binary, iUpM8, {iUpV, c8}, "-", 0, sp};
        sub8.overflowProven = true;
        const Value g3{Type::Bool, fn.nextValue++};
        const Value g4{Type::Bool, fn.nextValue++};
        const Value g5{Type::Bool, fn.nextValue++};
        const Value g6{Type::Bool, fn.nextValue++};
        const Value g7{Type::Bool, fn.nextValue++};
        const Value g8{Type::Bool, fn.nextValue++};
        const Value g34{Type::Bool, fn.nextValue++};
        const Value g56{Type::Bool, fn.nextValue++};
        const Value g78{Type::Bool, fn.nextValue++};
        const Value g3456{Type::Bool, fn.nextValue++};
        const Value gAll{Type::Bool, fn.nextValue++};
        fn.blocks[guard2Blk].ops.push_back(sub8);
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g3, {iLoV, iUpM8}, "<=", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g4, {jUpV, c16}, ">=", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g5, {jUpV, c512}, "<=", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g6, {kUpV, c16}, ">=", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g7, {kUpV, c512}, "<=", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g8, {jUpV, stV}, "<=", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g34, {g3, g4}, "&", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g56, {g5, g6}, "&", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g78, {g7, g8}, "&", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, g3456, {g34, g56}, "&", 0, sp});
        fn.blocks[guard2Blk].ops.push_back({Op::Binary, gAll, {g3456, g78}, "&", 0, sp});
        emitBranchChain(guard2Blk, gAll, transAllocBlk);

        // Clone lpI.body blocks (2 passes for SSA Value mapping)
        std::unordered_map<unsigned, unsigned> blkMap;
        std::unordered_map<unsigned, Value> valMap;
        for (unsigned ob : p.lpIBody) {
            const unsigned nb = static_cast<unsigned>(fn.blocks.size());
            fn.blocks.emplace_back();
            blkMap[ob] = nb;
            for (const auto& op : fn.blocks[ob].ops) {
                if (op.result.type != Type::Void) {
                    Value nv = op.result;
                    nv.id = fn.nextValue++;
                    valMap[op.result.id] = nv;
                }
            }
        }
        std::unordered_set<unsigned> targetBReadIds;
        for (const auto& br : p.bReads) targetBReadIds.insert(br.opTemplate.result.id);

        auto remapVal = [&](Value v) -> Value {
            if (auto it = valMap.find(v.id); it != valMap.end()) return it->second;
            return v;
        };
        auto remapBlk = [&](unsigned ob) -> unsigned {
            if (auto it = blkMap.find(ob); it != blkMap.end()) return it->second;
            return ob;
        };
        for (unsigned ob : p.lpIBody) {
            const unsigned nb = blkMap[ob];
            Block& dst = fn.blocks[nb];
            const Block src = fn.blocks[ob];
            dst.exit = src.exit;
            dst.yes = remapBlk(src.yes);
            dst.no = remapBlk(src.no);
            for (unsigned t : src.targets) dst.targets.push_back(remapBlk(t));
            if (src.value) dst.value = remapVal(*src.value);
            dst.loopHint = src.loopHint;
            dst.coldBranchYes = src.coldBranchYes;
            dst.unrollCount = src.unrollCount;
            for (const auto& op : src.ops) {
                if (op.op == Op::Index && targetBReadIds.contains(op.result.id)) {
                    const Value btArr{Type::IntArray, fn.nextValue++};
                    const Value jv{Type::Int, fn.nextValue++};
                    const Value ks{Type::Int, fn.nextValue++};
                    const Value rowOff{Type::Int, fn.nextValue++};
                    const Value kv{Type::Int, fn.nextValue++};
                    const Value btIdx{Type::Int, fn.nextValue++};
                    Operation mulJK{Op::Binary, rowOff, {jv, ks}, "*", 0, op.span};
                    mulJK.overflowProven = true;
                    Operation addK{Op::Binary, btIdx, {rowOff, kv}, "+", 0, op.span};
                    addK.overflowProven = true;
                    Operation idxBt = op;
                    idxBt.result = valMap[op.result.id];
                    idxBt.inputs = {btArr, btIdx};
                    idxBt.boundsProven = true;
                    dst.ops.push_back({Op::Load, btArr, {}, {}, btSlot, op.span});
                    dst.ops.push_back({Op::Load, jv, {}, {}, p.jSlot, op.span});
                    dst.ops.push_back({Op::Load, ks, {}, {}, kStrideSlot, op.span});
                    dst.ops.push_back(std::move(mulJK));
                    dst.ops.push_back({Op::Load, kv, {}, {}, p.kSlot, op.span});
                    dst.ops.push_back(std::move(addK));
                    dst.ops.push_back(std::move(idxBt));
                    continue;
                }
                Operation copy = op;
                if (op.result.type != Type::Void) copy.result = valMap[op.result.id];
                for (auto& in : copy.inputs) in = remapVal(in);
                dst.ops.push_back(std::move(copy));
            }
        }
        const unsigned hdrITrans = blkMap[p.hdrI];

        // Build transAllocBlk + 2D transpose loops: for kk in 0..kUpper: for jj in 0..jUpper: bt[jj*kUpper + kk] = b[kk*stride + jj]
        {
            const Value totSz{Type::Int, fn.nextValue++};
            const Value zFill{Type::Int, fn.nextValue++};
            const Value btArr{Type::IntArray, fn.nextValue++};
            const Value zStart{Type::Int, fn.nextValue++};
            Operation mulSz{Op::Binary, totSz, {jUpV, kUpV}, "*", 0, sp};
            mulSz.overflowProven = true;
            Operation repBt{Op::RepeatRegion, btArr, {zFill, totSz}, {}, 0, sp};
            repBt.initialize = false;
            repBt.elemBits = bBits;
            repBt.elemUnsigned = bUns;
            fn.blocks[transAllocBlk].ops.push_back(mulSz);
            fn.blocks[transAllocBlk].ops.push_back({Op::Int, zFill, {}, "0", 0, sp});
            fn.blocks[transAllocBlk].ops.push_back(repBt);
            fn.blocks[transAllocBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {btArr}, {}, btSlot, sp});
            fn.blocks[transAllocBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {kUpV}, {}, kStrideSlot, sp});
            fn.blocks[transAllocBlk].ops.push_back({Op::Int, zStart, {}, "0", 0, sp});
            fn.blocks[transAllocBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {zStart}, {}, tkSlot, sp});
            fn.blocks[transAllocBlk].exit = Exit::Jump;
            fn.blocks[transAllocBlk].yes = transKHdr;
        }
        {
            const Value kk{Type::Int, fn.nextValue++};
            const Value condK{Type::Bool, fn.nextValue++};
            const Value zJ{Type::Int, fn.nextValue++};
            fn.blocks[transKHdr].ops.push_back({Op::Load, kk, {}, {}, tkSlot, sp});
            fn.blocks[transKHdr].ops.push_back({Op::Binary, condK, {kk, kUpV}, "<", 0, sp});
            fn.blocks[transKHdr].ops.push_back({Op::Int, zJ, {}, "0", 0, sp});
            fn.blocks[transKHdr].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {zJ}, {}, tjSlot, sp});
            fn.blocks[transKHdr].exit = Exit::Branch;
            fn.blocks[transKHdr].value = condK;
            fn.blocks[transKHdr].yes = transJHdr;
            fn.blocks[transKHdr].no = hdrITrans;
        }
        {
            const Value jj{Type::Int, fn.nextValue++};
            const Value condJ{Type::Bool, fn.nextValue++};
            fn.blocks[transJHdr].ops.push_back({Op::Load, jj, {}, {}, tjSlot, sp});
            fn.blocks[transJHdr].ops.push_back({Op::Binary, condJ, {jj, jUpV}, "<", 0, sp});
            fn.blocks[transJHdr].exit = Exit::Branch;
            fn.blocks[transJHdr].value = condJ;
            fn.blocks[transJHdr].yes = transJBody;
            fn.blocks[transJHdr].no = transKTail;
        }
        {
            const Value kk{Type::Int, fn.nextValue++};
            const Value jj{Type::Int, fn.nextValue++};
            const Value bArr{Type::IntArray, fn.nextValue++};
            const Value srcRow{Type::Int, fn.nextValue++};
            const Value srcIdx{Type::Int, fn.nextValue++};
            const Value val{Type::Int, fn.nextValue++};
            const Value btArr{Type::IntArray, fn.nextValue++};
            const Value dstRow{Type::Int, fn.nextValue++};
            const Value dstIdx{Type::Int, fn.nextValue++};
            const Value one{Type::Int, fn.nextValue++};
            const Value jNext{Type::Int, fn.nextValue++};
            Operation mSrc{Op::Binary, srcRow, {kk, stV}, "*", 0, sp};
            mSrc.overflowProven = true;
            Operation aSrc{Op::Binary, srcIdx, {srcRow, jj}, "+", 0, sp};
            aSrc.overflowProven = true;
            Operation rd{Op::Index, val, {bArr, srcIdx}, {}, 0, sp};
            rd.boundsProven = true;
            rd.elemBits = bBits;
            rd.elemUnsigned = bUns;
            Operation mDst{Op::Binary, dstRow, {jj, kUpV}, "*", 0, sp};
            mDst.overflowProven = true;
            Operation aDst{Op::Binary, dstIdx, {dstRow, kk}, "+", 0, sp};
            aDst.overflowProven = true;
            Operation wr{Op::IndexSet, {Type::Void, fn.nextValue++}, {btArr, dstIdx, val}, {}, 0, sp};
            wr.boundsProven = true;
            wr.elemBits = bBits;
            wr.elemUnsigned = bUns;
            Operation incJ{Op::Binary, jNext, {jj, one}, "rangeIncrement", 0, sp};
            incJ.overflowProven = true;
            fn.blocks[transJBody].ops.push_back({Op::Load, kk, {}, {}, tkSlot, sp});
            fn.blocks[transJBody].ops.push_back({Op::Load, jj, {}, {}, tjSlot, sp});
            fn.blocks[transJBody].ops.push_back({Op::Load, bArr, {}, {}, p.bSlot, sp});
            fn.blocks[transJBody].ops.push_back(mSrc);
            fn.blocks[transJBody].ops.push_back(aSrc);
            fn.blocks[transJBody].ops.push_back(rd);
            fn.blocks[transJBody].ops.push_back({Op::Load, btArr, {}, {}, btSlot, sp});
            fn.blocks[transJBody].ops.push_back(mDst);
            fn.blocks[transJBody].ops.push_back(aDst);
            fn.blocks[transJBody].ops.push_back(wr);
            fn.blocks[transJBody].ops.push_back({Op::Int, one, {}, "1", 0, sp});
            fn.blocks[transJBody].ops.push_back(incJ);
            fn.blocks[transJBody].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jNext}, {}, tjSlot, sp});
            fn.blocks[transJBody].exit = Exit::Jump;
            fn.blocks[transJBody].yes = transJHdr;
        }
        {
            const Value kk{Type::Int, fn.nextValue++};
            const Value one{Type::Int, fn.nextValue++};
            const Value kNext{Type::Int, fn.nextValue++};
            Operation incK{Op::Binary, kNext, {kk, one}, "rangeIncrement", 0, sp};
            incK.overflowProven = true;
            fn.blocks[transKTail].ops.push_back({Op::Load, kk, {}, {}, tkSlot, sp});
            fn.blocks[transKTail].ops.push_back({Op::Int, one, {}, "1", 0, sp});
            fn.blocks[transKTail].ops.push_back(incK);
            fn.blocks[transKTail].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {kNext}, {}, tkSlot, sp});
            fn.blocks[transKTail].exit = Exit::Jump;
            fn.blocks[transKTail].yes = transKHdr;
        }
    }
}

inline void unrollAndJamMatMul(
    FunctionIR& fn,
    const std::unordered_map<unsigned, std::optional<IntRange>>& elemRanges) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    const std::size_t nSlots = fn.slots.size();
    std::vector<int> originCount(nSlots, 0);
    std::vector<bool> badArr(nSlots, false);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, unsigned> loadSlot;
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const auto& op : fn.blocks[bi].ops) {
            if (op.result.type != Type::Void) {
                defOp[op.result.id] = &op;
                defBlock[op.result.id] = bi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
            if (op.op == Op::Store && !op.inputs.empty() && op.slot < nSlots && fn.slots[op.slot] == Type::IntArray) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && (d->second->op == Op::RepeatRegion || d->second->op == Op::RepeatHeap)) {
                    ++originCount[op.slot];
                } else {
                    badArr[op.slot] = true;
                    if (auto src = loadSlot.find(op.inputs[0].id); src != loadSlot.end()) badArr[src->second] = true;
                }
            }
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                for (const auto& in : op.inputs)
                    if (in.type == Type::IntArray)
                        if (auto ls = loadSlot.find(in.id); ls != loadSlot.end()) badArr[ls->second] = true;
            }
            if ((op.op == Op::ClearOwner || op.op == Op::ClearField) && op.slot < nSlots) badArr[op.slot] = true;
            if (op.op == Op::ClearIndex && !op.inputs.empty()) {
                if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end() && ls->second < nSlots)
                    badArr[ls->second] = true;
            }
        }
        if (fn.blocks[bi].exit == Exit::Return && fn.blocks[bi].value && fn.blocks[bi].value->type == Type::IntArray) {
            if (auto ls = loadSlot.find(fn.blocks[bi].value->id); ls != loadSlot.end()) badArr[ls->second] = true;
        }
    }

    struct Plan {
        unsigned preJ = 0, hdrJ = 0, jInit = 0, hdrK = 0, bodyK = 0, jTail = 0;
        unsigned jSlot = 0, kSlot = 0;
        Value jUpper{}, kUpper{};
        Span hdrJSpan{}, initSpan{}, hdrKSpan{}, tailSpan{};
        std::unordered_set<unsigned> laneSlots; // scalar slots written in jInit or bodyK (excl. kSlot)
        std::int64_t safeMaxK32 = 0;
        bool nonNegLanes32 = false;
        std::vector<Operation> guardKOps;
        Value guardKVal{};
    };
    std::vector<Plan> plans;

    for (std::size_t liJ = 0; liJ < cfg.loops.size(); ++liJ) {
        const auto& lpJ = cfg.loops[liJ];
        if (lpJ.preheaders.size() != 1) continue;
        const unsigned preJ = lpJ.preheaders[0];
        const unsigned hdrJ = lpJ.header;
        if (fn.blocks[preJ].exit != Exit::Jump || fn.blocks[preJ].yes != hdrJ) continue;
        const auto& hJBlk = fn.blocks[hdrJ];
        if (hJBlk.exit != Exit::Branch || !hJBlk.value || !lpJ.body.contains(hJBlk.yes) || lpJ.body.contains(hJBlk.no))
            continue;
        auto cJ = defOp.find(hJBlk.value->id);
        if (cJ == defOp.end() || cJ->second->op != Op::Binary || cJ->second->data != "<") continue;
        auto jls = loadSlot.find(cJ->second->inputs[0].id);
        if (jls == loadSlot.end()) continue;
        const unsigned jSlot = jls->second;
        const Value jUpper = cJ->second->inputs[1];
        if (defBlock.count(jUpper.id) && lpJ.body.contains(defBlock[jUpper.id])) continue;

        // Require jLower == 0 constant in preJ
        std::optional<std::int64_t> jLowC;
        for (const auto& op : fn.blocks[preJ].ops) {
            if (op.op == Op::Store && op.slot == jSlot && !op.inputs.empty()) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && d->second->op == Op::Int) jLowC = parseConstInt(d->second->data);
            }
        }
        if (jLowC != 0) continue;

        const unsigned jInit = hJBlk.yes;
        if (fn.blocks[jInit].exit != Exit::Jump) continue;
        const unsigned hdrK = fn.blocks[jInit].yes;
        auto litK = cfg.headerToLoop.find(hdrK);
        if (litK == cfg.headerToLoop.end() || litK->second == liJ) continue;
        const auto& lpK = cfg.loops[litK->second];
        if (lpK.body.size() != 2 || lpK.preheaders.size() != 1 || lpK.preheaders[0] != jInit) continue;
        const auto& hKBlk = fn.blocks[hdrK];
        if (hKBlk.exit != Exit::Branch || !hKBlk.value || !lpK.body.contains(hKBlk.yes) || lpK.body.contains(hKBlk.no))
            continue;
        const unsigned bodyK = hKBlk.yes;
        const unsigned jTail = hKBlk.no;
        if (!lpJ.body.contains(bodyK) || !lpJ.body.contains(jTail) || lpJ.body.size() != 5) continue;
        if (fn.blocks[bodyK].exit != Exit::Jump || fn.blocks[bodyK].yes != hdrK) continue;
        if (fn.blocks[jTail].exit != Exit::Jump || fn.blocks[jTail].yes != hdrJ) continue;

        auto cK = defOp.find(hKBlk.value->id);
        if (cK == defOp.end() || cK->second->op != Op::Binary || cK->second->data != "<") continue;
        auto kls = loadSlot.find(cK->second->inputs[0].id);
        if (kls == loadSlot.end() || kls->second == jSlot) continue;
        const unsigned kSlot = kls->second;
        const Value kUpper = cK->second->inputs[1];
        if (kUpper.type != Type::Int) continue;

        // Require kLower == 0 constant in jInit, kUpper invariant of jSlot, no Load(jSlot), and pure scalar jInit
        bool okInit = true;
        std::optional<std::int64_t> kLowC;
        std::unordered_set<unsigned> laneSlots;
        for (const auto& op : fn.blocks[jInit].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall || op.op == Op::Index || op.op == Op::IndexSet ||
                op.op == Op::RepeatHeap || op.op == Op::RepeatRegion || op.op == Op::Clone) {
                okInit = false; break;
            }
            if (op.op == Op::Load && op.slot == jSlot) { okInit = false; break; }
            if (op.op == Op::Store) {
                if (op.slot == jSlot) { okInit = false; break; }
                if (op.slot == kSlot) {
                    auto d = defOp.find(op.inputs[0].id);
                    if (d != defOp.end() && d->second->op == Op::Int) kLowC = parseConstInt(d->second->data);
                } else {
                    if (fn.slots[op.slot] != Type::Int && fn.slots[op.slot] != Type::Bool) { okInit = false; break; }
                    laneSlots.insert(op.slot);
                }
            }
        }
        if (!okInit || kLowC != 0 || laneSlots.empty()) continue;
        if (defBlock.count(kUpper.id) && (defBlock[kUpper.id] == jInit || lpK.body.contains(defBlock[kUpper.id]))) {
            // Check kUpper does not read jSlot or any laneSlot
            std::unordered_set<unsigned> seenV;
            auto depJ = [&](auto& self, Value v, int d) -> bool {
                if (d > 24 || !seenV.insert(v.id).second) return false;
                auto it = defOp.find(v.id);
                if (it == defOp.end()) return false;
                if (it->second->op == Op::Load) return it->second->slot == jSlot || laneSlots.contains(it->second->slot);
                for (const auto& in : it->second->inputs)
                    if (self(self, in, d + 1)) return true;
                return false;
            };
            if (depJ(depJ, kUpper, 0)) continue;
        }

        // Require completely check-free scalar-updating bodyK (no IndexSet, Call, untrapped ops)
        bool okBodyK = true;
        int jReadsInBodyK = 0;
        int kIncs = 0;
        std::unordered_set<unsigned> readArrsInBodyK;
        for (const auto& op : fn.blocks[bodyK].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin || op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                op.op == Op::Print || (op.op == Op::Assert && !op.boundsProven && !op.unchecked) ||
                op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice || op.op == Op::FieldSet || op.op == Op::PlaceSet ||
                op.op == Op::IndexSet || op.op == Op::RepeatHeap ||
                op.op == Op::RepeatRegion || op.op == Op::Clone ||
                op.op == Op::ClearOwner || op.op == Op::ClearField || op.op == Op::ClearIndex) {
                okBodyK = false; break;
            }
            if (op.op == Op::Index) {
                if (!op.boundsProven || op.inputs[0].type != Type::IntArray) { okBodyK = false; break; }
                if (auto als = loadSlot.find(op.inputs[0].id); als != loadSlot.end()) readArrsInBodyK.insert(als->second);
            }
            if (op.op == Op::Negate && op.result.type == Type::Int && !op.overflowProven) { okBodyK = false; break; }
            if (op.op == Op::AbsDiff && !op.overflowProven) { okBodyK = false; break; }
            if (op.op == Op::Binary && op.inputs[0].type == Type::Int) {
                if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                     op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) { okBodyK = false; break; }
                if ((op.data == "/" || op.data == "%") && !op.divSafe) { okBodyK = false; break; }
            }
            if (op.op == Op::Load && op.slot == jSlot) ++jReadsInBodyK;
            if (op.op == Op::Store) {
                if (op.slot == jSlot) { okBodyK = false; break; }
                if (op.slot == kSlot) {
                    ++kIncs;
                    auto kd = defOp.find(op.inputs[0].id);
                    if (kd == defOp.end() || kd->second->op != Op::Binary || kd->second->data != "rangeIncrement") {
                        okBodyK = false; break;
                    }
                } else {
                    if (fn.slots[op.slot] != Type::Int && fn.slots[op.slot] != Type::Bool) { okBodyK = false; break; }
                    laneSlots.insert(op.slot);
                }
            }
        }
        if (!okBodyK || jReadsInBodyK == 0 || kIncs != 1) continue;

        // Check jTail: no calls/allocs/nested control; allow proven IndexSet only on unaliased non-bodyK-read arrays; jSlot updated once by rangeIncrement
        bool okTail = true;
        int jIncs = 0;
        std::unordered_set<unsigned> tailWrittenArrs, tailReadArrs;
        for (const auto& op : fn.blocks[jTail].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin || op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                op.op == Op::Print || (op.op == Op::Assert && !op.boundsProven && !op.unchecked) ||
                op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice || op.op == Op::FieldSet || op.op == Op::PlaceSet ||
                op.op == Op::RepeatHeap ||
                op.op == Op::RepeatRegion || op.op == Op::Clone ||
                op.op == Op::ClearOwner || op.op == Op::ClearField || op.op == Op::ClearIndex) {
                okTail = false; break;
            }
            if (op.op == Op::Index) {
                if (!op.boundsProven || op.inputs[0].type != Type::IntArray) { okTail = false; break; }
                if (auto als = loadSlot.find(op.inputs[0].id); als != loadSlot.end()) tailReadArrs.insert(als->second);
            } else if (op.op == Op::IndexSet) {
                if (!op.boundsProven || op.inputs[0].type != Type::IntArray) { okTail = false; break; }
                auto als = loadSlot.find(op.inputs[0].id);
                if (als == loadSlot.end() || als->second >= nSlots || originCount[als->second] != 1 ||
                    badArr[als->second] || readArrsInBodyK.contains(als->second)) {
                    okTail = false; break;
                }
                tailWrittenArrs.insert(als->second);
            } else if ((op.op == Op::Negate || op.op == Op::AbsDiff) &&
                       op.result.type == Type::Int && !op.overflowProven) {
                okTail = false; break;
            } else if (op.op == Op::Binary && op.inputs[0].type == Type::Int) {
                if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                     op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) { okTail = false; break; }
                if ((op.data == "/" || op.data == "%") && !op.divSafe) { okTail = false; break; }
            }
            for (const auto& in : op.inputs) {
                if (defBlock.count(in.id) && lpK.body.contains(defBlock[in.id])) { okTail = false; break; }
            }
            if (!okTail) break;
            if (op.op == Op::Store && op.slot == jSlot) {
                ++jIncs;
                auto d = defOp.find(op.inputs[0].id);
                if (d == defOp.end() || d->second->op != Op::Binary || d->second->data != "rangeIncrement") okTail = false;
            } else if (op.op == Op::Store && laneSlots.contains(op.slot)) {
                okTail = false; break;
            } else if (op.op == Op::Store) {
                // Reject if scalar written in jTail is read in jInit, hdrK, or bodyK
                for (unsigned blk : {jInit, hdrK, bodyK}) {
                    for (const auto& bo : fn.blocks[blk].ops) {
                        if (bo.op == Op::Load && bo.slot == op.slot) { okTail = false; break; }
                    }
                    if (!okTail) break;
                }
                if (!okTail) break;
            }
        }
        for (unsigned wa : tailWrittenArrs) {
            if (tailReadArrs.contains(wa)) { okTail = false; break; }
        }
        if (!okTail || jIncs != 1) continue;

        const Span hdrJSpan = cJ->second->span;
        const Span initSpan = fn.blocks[jInit].ops.empty() ? hdrJSpan : fn.blocks[jInit].ops.front().span;
        const Span hdrKSpan = cK->second->span;
        const Span tailSpan = fn.blocks[jTail].ops.empty() ? hdrJSpan : fn.blocks[jTail].ops.back().span;

        // Check whether lane accumulators fit cleanly in signed i32 for small-to-medium K (kUpper <= safeMaxK32)
        std::int64_t safeMaxK32 = 0;
        bool nonNegLanes32 = true;
        {
            bool initZeroOk = !laneSlots.empty();
            std::unordered_set<unsigned> initSeen;
            for (const auto& op : fn.blocks[jInit].ops) {
                if (op.op == Op::Store && laneSlots.contains(op.slot)) {
                    initSeen.insert(op.slot);
                    auto d = (!op.inputs.empty()) ? defOp.find(op.inputs[0].id) : defOp.end();
                    auto c = (d != defOp.end() && d->second->op == Op::Int) ? parseConstInt(d->second->data) : std::nullopt;
                    if (c != 0) { initZeroOk = false; break; }
                }
            }
            if (initZeroOk && initSeen.size() == laneSlots.size()) {
                std::int64_t maxStepAbs = 0;
                bool okSteps = true;
                std::unordered_set<unsigned> updatedLanes;
                std::function<std::optional<IntRange>(unsigned, int)> evalTermRange = [&](unsigned vid, int depth) -> std::optional<IntRange> {
                    if (depth > 16) return {};
                    auto dit = defOp.find(vid);
                    if (dit == defOp.end()) return {};
                    const Operation* o = dit->second;
                    if (o->op == Op::Int) {
                        if (auto c = parseConstInt(o->data)) return IntRange{*c, *c};
                        return {};
                    }
                    if (o->op == Op::Index && !o->inputs.empty()) {
                        if (auto als = loadSlot.find(o->inputs[0].id); als != loadSlot.end()) {
                            if (auto eit = elemRanges.find(als->second); eit != elemRanges.end() && eit->second)
                                return *eit->second;
                        }
                        return {};
                    }
                    if (o->op == Op::Negate && !o->inputs.empty()) {
                        if (auto r = evalTermRange(o->inputs[0].id, depth + 1))
                            return ReturnRanges::negateRange(*r);
                        return {};
                    }
                    if (o->op == Op::Binary && o->inputs.size() == 2) {
                        auto lr = evalTermRange(o->inputs[0].id, depth + 1);
                        auto rr = evalTermRange(o->inputs[1].id, depth + 1);
                        if (!lr || !rr) return {};
                        if (o->data == "+") return ReturnRanges::addRanges(*lr, *rr);
                        if (o->data == "-") return ReturnRanges::subtractRanges(*lr, *rr);
                        if (o->data == "*") return ReturnRanges::multiplyRanges(*lr, *rr);
                    }
                    return {};
                };
                for (const auto& op : fn.blocks[bodyK].ops) {
                    if (op.op == Op::Store && laneSlots.contains(op.slot)) {
                        if (!updatedLanes.insert(op.slot).second || op.inputs.empty()) { okSteps = false; break; }
                        auto d = defOp.find(op.inputs[0].id);
                        if (d == defOp.end() || d->second->op != Op::Binary ||
                            (d->second->data != "+" && d->second->data != "-")) { okSteps = false; break; }
                        auto l0 = loadSlot.find(d->second->inputs[0].id);
                        if (l0 == loadSlot.end() || l0->second != op.slot) { okSteps = false; break; }
                        auto tr = evalTermRange(d->second->inputs[1].id, 0);
                        if (!tr) { okSteps = false; break; }
                        if (d->second->data == "-") tr = ReturnRanges::negateRange(*tr);
                        if (!tr) { okSteps = false; break; }
                        if (tr->lower < 0) nonNegLanes32 = false;
                        const std::int64_t aLo = (tr->lower < 0) ? -tr->lower : tr->lower;
                        const std::int64_t aHi = (tr->upper < 0) ? -tr->upper : tr->upper;
                        maxStepAbs = std::max({maxStepAbs, aLo, aHi});
                    }
                }
                if (okSteps && updatedLanes.size() == laneSlots.size() && maxStepAbs > 0) {
                    const std::int64_t mk = 2147483647LL / maxStepAbs;
                    if (mk >= 32) safeMaxK32 = mk;
                }
            }
        }
        std::vector<Operation> guardKOps;
        Value guardKVal = kUpper;
        if (safeMaxK32 > 0 && defBlock.count(kUpper.id) && lpJ.body.contains(defBlock[kUpper.id])) {
            std::unordered_map<unsigned, Value> gMemo;
            auto cloneForGuard = [&](auto& self, Value v, int depth) -> std::optional<Value> {
                if (depth > 16) return {};
                if (auto it = gMemo.find(v.id); it != gMemo.end()) return it->second;
                auto dbIt = defBlock.find(v.id);
                if (dbIt == defBlock.end() || !lpJ.body.contains(dbIt->second)) return v;
                if (dbIt->second != jInit) return {};
                auto opIt = defOp.find(v.id);
                if (opIt == defOp.end()) return {};
                const Operation* o = opIt->second;
                if (o->op == Op::Int) {
                    Operation c = *o;
                    c.result.id = fn.nextValue++;
                    guardKOps.push_back(c);
                    gMemo[v.id] = c.result;
                    return c.result;
                }
                if (o->op == Op::Load) {
                    for (unsigned lb : lpJ.body) {
                        for (const auto& bop : fn.blocks[lb].ops)
                            if (bop.op == Op::Store && bop.slot == o->slot) return {};
                    }
                    Operation c = *o;
                    c.result.id = fn.nextValue++;
                    guardKOps.push_back(c);
                    gMemo[v.id] = c.result;
                    return c.result;
                }
                if (o->op == Op::Negate || (o->op == Op::Binary && (o->data == "+" || o->data == "-" || o->data == "*"))) {
                    Operation c = *o;
                    c.result.id = fn.nextValue++;
                    for (auto& in : c.inputs) {
                        auto ni = self(self, in, depth + 1);
                        if (!ni) return {};
                        in = *ni;
                    }
                    guardKOps.push_back(c);
                    gMemo[v.id] = c.result;
                    return c.result;
                }
                return {};
            };
            if (auto gkv = cloneForGuard(cloneForGuard, kUpper, 0)) guardKVal = *gkv;
            else safeMaxK32 = 0;
        }
        plans.push_back({preJ, hdrJ, jInit, hdrK, bodyK, jTail, jSlot, kSlot, jUpper, kUpper,
                         hdrJSpan, initSpan, hdrKSpan, tailSpan, std::move(laneSlots), safeMaxK32, nonNegLanes32,
                         std::move(guardKOps), guardKVal});
    }

    for (const auto& p : plans) {
        if (fn.blocks[p.preJ].exit != Exit::Jump || fn.blocks[p.preJ].yes != p.hdrJ) continue;
        constexpr int UF = 8;
        std::array<unsigned, UF> jColSlots{};
        std::array<std::unordered_map<unsigned, unsigned>, UF> laneSlotMap;
        auto allocSlot = [&](Type t) -> unsigned {
            const unsigned s = static_cast<unsigned>(fn.slots.size());
            fn.slots.push_back(t);
            if (fn.slotElemBits.size() == s) {
                fn.slotElemBits.push_back(64);
                fn.slotElemUnsigned.push_back(false);
            }
            if (fn.slotAliasGroup.size() == s) fn.slotAliasGroup.push_back(-1);
            if (fn.slotByRef.size() == s) fn.slotByRef.push_back(false);
            if (fn.slotOwned.size() == s) fn.slotOwned.push_back(false);
            return s;
        };
        for (int m = 0; m < UF; ++m) {
            jColSlots[m] = allocSlot(Type::Int);
            for (unsigned ls : p.laneSlots) laneSlotMap[m][ls] = allocSlot(fn.slots[ls]);
        }

        // Precompute backward set of accumulator ALU ops (stopping at Op::Index) in p.bodyK for narrow32 clone
        std::unordered_set<std::size_t> bodyKAccAluIdx;
        if (p.safeMaxK32 > 0) {
            std::unordered_set<unsigned> accValIds;
            const auto& bkOps = fn.blocks[p.bodyK].ops;
            for (const auto& op : bkOps) {
                if (op.op == Op::Store && p.laneSlots.contains(op.slot) && !op.inputs.empty())
                    accValIds.insert(op.inputs[0].id);
            }
            bool chg = true;
            while (chg) {
                chg = false;
                for (std::size_t oi = 0; oi < bkOps.size(); ++oi) {
                    const auto& op = bkOps[oi];
                    if (op.result.type == Type::Int && accValIds.contains(op.result.id) &&
                        (op.op == Op::Negate || op.op == Op::AbsDiff ||
                         (op.op == Op::Binary && (op.data == "+" || op.data == "-" || op.data == "*")))) {
                        if (bodyKAccAluIdx.insert(oi).second) chg = true;
                        for (const auto& in : op.inputs)
                            if (accValIds.insert(in.id).second) chg = true;
                    }
                }
            }
        }

        const unsigned jamPre = static_cast<unsigned>(fn.blocks.size());
        const bool hasGuard = (p.safeMaxK32 > 0);
        const unsigned jamGuard = hasGuard ? (jamPre + 1) : 0;

        auto emitJammedPath = [&](bool narrow32) -> unsigned {
            const unsigned jamHdr = static_cast<unsigned>(fn.blocks.size());
            const unsigned jamInit = jamHdr + 1;
            const unsigned jamHdrK = jamHdr + 2;
            const unsigned jamBodyK = jamHdr + 3;
            const unsigned jamTail = jamHdr + 4;
            fn.blocks.resize(jamTail + 1);

            // jamHdr: curJ < (jUpper - 7) -> jamInit else hdrJ
            const Value curJ{Type::Int, fn.nextValue++};
            const Value c7{Type::Int, fn.nextValue++};
            const Value lim7{Type::Int, fn.nextValue++};
            const Value condJ{Type::Bool, fn.nextValue++};
            Operation subLim{Op::Binary, lim7, {p.jUpper, c7}, "-", 0, p.hdrJSpan};
            subLim.overflowProven = true;
            fn.blocks[jamHdr].ops.push_back({Op::Load, curJ, {}, {}, p.jSlot, p.hdrJSpan});
            fn.blocks[jamHdr].ops.push_back({Op::Int, c7, {}, "7", 0, p.hdrJSpan});
            fn.blocks[jamHdr].ops.push_back(subLim);
            fn.blocks[jamHdr].ops.push_back({Op::Binary, condJ, {curJ, lim7}, "<", 0, p.hdrJSpan});
            fn.blocks[jamHdr].exit = Exit::Branch;
            fn.blocks[jamHdr].value = condJ;
            fn.blocks[jamHdr].yes = jamInit;
            fn.blocks[jamHdr].no = p.hdrJ;

            // jamInit
            const Value baseJ{Type::Int, fn.nextValue++};
            fn.blocks[jamInit].ops.push_back({Op::Load, baseJ, {}, {}, p.jSlot, p.initSpan});
            for (int m = 0; m < UF; ++m) {
                if (m == 0) {
                    fn.blocks[jamInit].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {baseJ}, {}, jColSlots[0], p.initSpan});
                } else {
                    const Value cm{Type::Int, fn.nextValue++};
                    const Value jm{Type::Int, fn.nextValue++};
                    Operation addJ{Op::Binary, jm, {baseJ, cm}, "+", 0, p.initSpan};
                    addJ.overflowProven = true;
                    fn.blocks[jamInit].ops.push_back({Op::Int, cm, {}, std::to_string(m), 0, p.initSpan});
                    fn.blocks[jamInit].ops.push_back(addJ);
                    fn.blocks[jamInit].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jm}, {}, jColSlots[m], p.initSpan});
                }
            }
            std::unordered_map<unsigned, Value> initSharedMap;
            std::array<std::unordered_map<unsigned, Value>, UF> initLaneMaps;
            auto remap = [&](const Operation& src, const std::unordered_map<unsigned, Value>& vm,
                             const std::unordered_map<unsigned, unsigned>& sm) -> Operation {
                Operation dst = src;
                dst.result.id = fn.nextValue++;
                if (dst.op == Op::Load || dst.op == Op::Store) {
                    if (auto it = sm.find(src.slot); it != sm.end()) dst.slot = it->second;
                }
                for (auto& in : dst.inputs) {
                    if (auto it = vm.find(in.id); it != vm.end()) in = it->second;
                }
                return dst;
            };
            for (const auto& op : fn.blocks[p.jInit].ops) {
                if (op.op == Op::Store && p.laneSlots.contains(op.slot)) {
                    for (int m = 0; m < UF; ++m) {
                        Operation c = op;
                        c.result.id = fn.nextValue++;
                        c.slot = laneSlotMap[m][op.slot];
                        for (auto& in : c.inputs) {
                            if (auto it = initLaneMaps[m].find(in.id); it != initLaneMaps[m].end()) in = it->second;
                            else if (auto it2 = initSharedMap.find(in.id); it2 != initSharedMap.end()) in = it2->second;
                        }
                        fn.blocks[jamInit].ops.push_back(c);
                    }
                } else {
                    Operation c = remap(op, initSharedMap, {});
                    if (op.result.type != Type::Void) initSharedMap[op.result.id] = c.result;
                    fn.blocks[jamInit].ops.push_back(c);
                }
            }
            Value jamKUpper = p.kUpper;
            if (auto it = initSharedMap.find(p.kUpper.id); it != initSharedMap.end()) jamKUpper = it->second;
            fn.blocks[jamInit].exit = Exit::Jump;
            fn.blocks[jamInit].yes = jamHdrK;

            // jamHdrK
            const Value curK{Type::Int, fn.nextValue++};
            const Value condK{Type::Bool, fn.nextValue++};
            fn.blocks[jamHdrK].ops.push_back({Op::Load, curK, {}, {}, p.kSlot, p.hdrKSpan});
            fn.blocks[jamHdrK].ops.push_back({Op::Binary, condK, {curK, jamKUpper}, "<", 0, p.hdrKSpan});
            fn.blocks[jamHdrK].exit = Exit::Branch;
            fn.blocks[jamHdrK].value = condK;
            fn.blocks[jamHdrK].yes = jamBodyK;
            fn.blocks[jamHdrK].no = jamTail;

            // jamBodyK
            std::unordered_set<unsigned> jDepVals;
            for (const auto& op : fn.blocks[p.bodyK].ops) {
                bool dep = false;
                if (op.op == Op::Load && (op.slot == p.jSlot || p.laneSlots.contains(op.slot))) dep = true;
                for (const auto& in : op.inputs)
                    if (jDepVals.contains(in.id)) dep = true;
                if (dep && op.result.type != Type::Void) jDepVals.insert(op.result.id);
            }
            std::unordered_map<unsigned, Value> sharedBK;
            std::array<std::unordered_map<unsigned, Value>, UF> laneBK;
            const auto& srcBodyKOps = fn.blocks[p.bodyK].ops;
            for (std::size_t oi = 0; oi < srcBodyKOps.size(); ++oi) {
                const auto& op = srcBodyKOps[oi];
                const bool isAccAlu = narrow32 && bodyKAccAluIdx.contains(oi);
                const bool isDep =
                    (op.result.type != Type::Void && jDepVals.contains(op.result.id)) ||
                    (op.op == Op::Store && p.laneSlots.contains(op.slot));
                if (!isDep) {
                    Operation c = remap(op, sharedBK, {});
                    if (isAccAlu) c.width = 32;
                    if (op.result.type != Type::Void) sharedBK[op.result.id] = c.result;
                    fn.blocks[jamBodyK].ops.push_back(c);
                } else {
                    for (int m = 0; m < UF; ++m) {
                        Operation c = op;
                        c.result.id = fn.nextValue++;
                        if (isAccAlu) c.width = 32;
                        if (c.op == Op::Load || c.op == Op::Store) {
                            if (op.slot == p.jSlot) c.slot = jColSlots[m];
                            else if (auto it = laneSlotMap[m].find(op.slot); it != laneSlotMap[m].end()) c.slot = it->second;
                        }
                        for (auto& in : c.inputs) {
                            if (auto it = laneBK[m].find(in.id); it != laneBK[m].end()) in = it->second;
                            else if (auto it2 = sharedBK.find(in.id); it2 != sharedBK.end()) in = it2->second;
                        }
                        if (op.result.type != Type::Void) laneBK[m][op.result.id] = c.result;
                        fn.blocks[jamBodyK].ops.push_back(c);
                    }
                }
            }
            fn.blocks[jamBodyK].exit = Exit::Jump;
            fn.blocks[jamBodyK].yes = jamHdrK;
            fn.blocks[jamBodyK].loopHint = LoopHint::Vectorize;

            // jamTail
            for (int m = 0; m < UF; ++m) {
                std::unordered_map<unsigned, Value> tailVM;
                for (const auto& op : fn.blocks[p.jTail].ops) {
                    if (op.op == Op::Store && op.slot == p.jSlot) continue;
                    Operation c = op;
                    c.result.id = fn.nextValue++;
                    if (c.op == Op::Load || c.op == Op::Store) {
                        if (op.slot == p.jSlot) c.slot = jColSlots[m];
                        else if (auto it = laneSlotMap[m].find(op.slot); it != laneSlotMap[m].end()) c.slot = it->second;
                    }
                    for (auto& in : c.inputs) {
                        if (auto it = tailVM.find(in.id); it != tailVM.end()) in = it->second;
                        else if (auto it2 = initSharedMap.find(in.id); it2 != initSharedMap.end()) in = it2->second;
                    }
                    if (narrow32 && p.nonNegLanes32 && c.op == Op::Binary && c.data == "%" && c.divSafe) {
                        c.unsignedDiv = true;
                        c.width = 32;
                        c.lhsUpper = 2147483647LL;
                    }
                    if (op.result.type != Type::Void) tailVM[op.result.id] = c.result;
                    fn.blocks[jamTail].ops.push_back(c);
                }
            }
            const Value curJ2{Type::Int, fn.nextValue++};
            const Value c8step{Type::Int, fn.nextValue++};
            const Value nextJ8{Type::Int, fn.nextValue++};
            Operation add8{Op::Binary, nextJ8, {curJ2, c8step}, "+", 0, p.tailSpan};
            add8.overflowProven = true;
            fn.blocks[jamTail].ops.push_back({Op::Load, curJ2, {}, {}, p.jSlot, p.tailSpan});
            fn.blocks[jamTail].ops.push_back({Op::Int, c8step, {}, "8", 0, p.tailSpan});
            fn.blocks[jamTail].ops.push_back(add8);
            fn.blocks[jamTail].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {nextJ8}, {}, p.jSlot, p.tailSpan});
            fn.blocks[jamTail].exit = Exit::Jump;
            fn.blocks[jamTail].yes = jamHdr;
            return jamHdr;
        };

        fn.blocks.resize(hasGuard ? (jamGuard + 1) : (jamPre + 1));
        fn.blocks[p.preJ].yes = jamPre;

        // jamPre: if jUpper >= 8 -> (jamGuard or jamHdr64) else hdrJ
        const Value c8{Type::Int, fn.nextValue++};
        const Value cmp8{Type::Bool, fn.nextValue++};
        fn.blocks[jamPre].ops.push_back({Op::Int, c8, {}, "8", 0, p.hdrJSpan});
        fn.blocks[jamPre].ops.push_back({Op::Binary, cmp8, {p.jUpper, c8}, ">=", 0, p.hdrJSpan});
        fn.blocks[jamPre].exit = Exit::Branch;
        fn.blocks[jamPre].value = cmp8;
        fn.blocks[jamPre].no = p.hdrJ;

        if (hasGuard) {
            const unsigned jamHdr32 = emitJammedPath(true);
            const unsigned jamHdr64 = emitJammedPath(false);
            for (const auto& gop : p.guardKOps) fn.blocks[jamGuard].ops.push_back(gop);
            const Value cMaxK{Type::Int, fn.nextValue++};
            const Value cmpK{Type::Bool, fn.nextValue++};
            fn.blocks[jamGuard].ops.push_back({Op::Int, cMaxK, {}, std::to_string(p.safeMaxK32), 0, p.hdrKSpan});
            fn.blocks[jamGuard].ops.push_back({Op::Binary, cmpK, {p.guardKVal, cMaxK}, "<=", 0, p.hdrKSpan});
            fn.blocks[jamGuard].exit = Exit::Branch;
            fn.blocks[jamGuard].value = cmpK;
            fn.blocks[jamGuard].yes = jamHdr32;
            fn.blocks[jamGuard].no = jamHdr64;
            fn.blocks[jamPre].yes = jamGuard;
        } else {
            const unsigned jamHdr64 = emitJammedPath(false);
            fn.blocks[jamPre].yes = jamHdr64;
        }
    }
}

// ---------------------------------------------------------------------------
// Pass 3 (Alias Groups & Stack Arena Eligibility)
// ---------------------------------------------------------------------------
inline void assignAliasGroupAndStackArena(FunctionIR& fn, bool depthKnownSafe,
                                          const Program* programPtr = nullptr,
                                          bool calleesUseRegion = true) {
    bool hasUnsafeRegionCall = false;
    bool hasUnsafeRawWrite = false;
    for (const auto& b : fn.blocks) {
        for (const auto& op : b.ops) {
            if (op.op == Op::FnPtr || op.op == Op::BoxDyn || op.op == Op::DynCall ||
                op.op == Op::MakeClosure || op.op == Op::ClosureCall) {
                hasUnsafeRegionCall = true;
            } else if (op.op == Op::Call) {
                if (calleesUseRegion) hasUnsafeRegionCall = true;
            } else if (op.op == Op::Builtin &&
                       op.data != "clockNs" && op.data != "cpuCount") {
                hasUnsafeRegionCall = true;
            }
            if (op.op == Op::Builtin &&
                (op.data == "ptrWrite" || op.data == "ptrCopy" || op.data == "ptrZero" ||
                 op.data == "simdStore" || op.data == "simdStoreAligned" ||
                 op.data == "regionReset")) {
                hasUnsafeRawWrite = true;
            }
        }
    }
    // Only stack-allocate an arena chunk when no callee can re-enter a Turi region frame.
    fn.canStackArena = fn.usesRegion && !hasUnsafeRegionCall && (depthKnownSafe || fn.source.arenaHint);

    const std::size_t nSlots = fn.slots.size();
    fn.slotAliasGroup.assign(nSlots, -1);
    if (hasUnsafeRawWrite) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::vector<int> originCount(nSlots, 0);
    std::vector<bool> aliased(nSlots, false);
    std::vector<bool> defInLoop(nSlots, false);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, unsigned> loadSlot;

    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const auto& op : fn.blocks[bi].ops) {
            if (op.result.type != Type::Void) {
                defOp[op.result.id] = &op;
                defBlock[op.result.id] = bi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
        }
    }
    for (std::size_t s = 0; s < fn.source.params.size() && s < nSlots; ++s) {
        if (detail::isAnyArrayType(programPtr, fn.slots[s])) aliased[s] = true; // read-only or byRef params
    }
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const auto& op : fn.blocks[bi].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Builtin ||
                op.op == Op::DynCall || op.op == Op::ClosureCall) {
                for (unsigned ms : op.mutArgSlots) {
                    if (ms < nSlots) aliased[ms] = true;
                }
                for (const auto& in : op.inputs) {
                    if (detail::isAnyArrayType(programPtr, in.type)) {
                        if (auto ls = loadSlot.find(in.id); ls != loadSlot.end()) aliased[ls->second] = true;
                    }
                }
            }
            if (op.op == Op::Store && !op.inputs.empty() && detail::isAnyArrayType(programPtr, fn.slots[op.slot])) {
                auto def = defOp.find(op.inputs[0].id);
                if (def != defOp.end() && (def->second->op == Op::RepeatRegion ||
                    def->second->op == Op::RepeatHeap || def->second->op == Op::Clone)) {
                    ++originCount[op.slot];
                    const unsigned dBlk = defBlock[op.inputs[0].id];
                    if (cfg.loopOfBlock[bi] >= 0 || cfg.loopOfBlock[dBlk] >= 0) {
                        defInLoop[op.slot] = true;
                    }
                } else {
                    aliased[op.slot] = true;
                }
            }
        }
    }
    int nextGroup = 0;
    for (std::size_t s = 0; s < nSlots; ++s) {
        if (detail::isAnyArrayType(programPtr, fn.slots[s]) && !aliased[s] && originCount[s] == 1 && !defInLoop[s]) {
            fn.slotAliasGroup[s] = nextGroup++;
        }
    }
    for (auto& b : fn.blocks) {
        for (auto& op : b.ops) {
            if ((op.op == Op::Index || op.op == Op::IndexSet) && !op.inputs.empty()) {
                if (auto ls = loadSlot.find(op.inputs[0].id); ls != loadSlot.end()) {
                    op.aliasGroup = fn.slotAliasGroup[ls->second];
                }
            }
        }
    }
}

inline void assignAliasGroupsAndStackArena(std::vector<FunctionIR>& functions,
                                           const std::vector<bool>& safeDepths,
                                           const Program* programPtr = nullptr) {
    std::unordered_map<std::string, std::size_t> fnByName;
    for (std::size_t i = 0; i < functions.size(); ++i) fnByName[functions[i].source.name] = i;
    std::vector<bool> subtreeRegion(functions.size(), false);
    for (std::size_t i = 0; i < functions.size(); ++i) subtreeRegion[i] = functions[i].usesRegion;
    bool chg = true;
    while (chg) {
        chg = false;
        for (std::size_t i = 0; i < functions.size(); ++i) {
            if (subtreeRegion[i]) continue;
            for (const auto& blk : functions[i].blocks) {
                for (const auto& op : blk.ops) {
                    if (op.op == Op::Call) {
                        if (auto it = fnByName.find(op.data); it != fnByName.end() && subtreeRegion[it->second]) {
                            subtreeRegion[i] = true; chg = true; break;
                        }
                    } else if (op.op == Op::DynCall || op.op == Op::ClosureCall) {
                        subtreeRegion[i] = true; chg = true; break;
                    }
                }
                if (subtreeRegion[i]) break;
            }
        }
    }
    for (std::size_t i = 0; i < functions.size(); ++i) {
        bool calleesUseReg = false;
        for (const auto& blk : functions[i].blocks) {
            for (const auto& op : blk.ops) {
                if (op.op == Op::Call) {
                    auto it = fnByName.find(op.data);
                    if (it == fnByName.end() || subtreeRegion[it->second]) { calleesUseReg = true; break; }
                } else if (op.op == Op::DynCall || op.op == Op::ClosureCall) {
                    calleesUseReg = true; break;
                }
            }
            if (calleesUseReg) break;
        }
        assignAliasGroupAndStackArena(functions[i], i < safeDepths.size() ? safeDepths[i] : true,
                                      programPtr, calleesUseReg);
    }
}

struct FastClone {
    std::string origName;
    std::string fastName;
    bool depthSafe = true;
    bool hasLoop = false;
    FunctionIR ir;
};

namespace detail {
inline bool closureStructNeedsDrop(const Program& p, Type t, std::unordered_set<Type>& seen) {
    if (t == Type::IntArray || t == Type::Str || t == Type::StringBuilder ||
        p.isArray(t) || p.isMap(t) || p.isSet(t) || p.isDyn(t) || p.isFn(t))
        return true;
    if (t < Type::NamedBase || !seen.insert(t).second) return false;
    if (const auto* decl = p.declaration(t)) {
        for (const auto& v : decl->variants) {
            for (const auto& f : v.fields) {
                if (closureStructNeedsDrop(p, f.type, seen)) return true;
            }
        }
    }
    return false;
}

inline void replaceValueWithTypeInFunction(FunctionIR& fn, unsigned oldId, Value rep) {
    auto fix = [&](Value& v) {
        if (v.id == oldId) {
            v.id = rep.id;
            v.type = rep.type;
        }
    };
    for (auto& b : fn.blocks) {
        for (auto& op : b.ops) {
            for (auto& in : op.inputs) fix(in);
        }
        if (b.value) fix(*b.value);
    }
}
} // namespace detail

inline void devirtualizeNonEscapingClosures(const Program& program,
                                            std::vector<FunctionIR>& functions,
                                            std::deque<Function>& specStorage) {
    if (program.closures.empty()) return;
    auto needsDropStruct = [&](Type t) {
        std::unordered_set<Type> seen;
        return detail::closureStructNeedsDrop(program, t, seen);
    };
    auto isNonEscapingParam = [&](const FunctionIR& fn, std::size_t pIdx) -> bool {
        if (fn.source.isTest || pIdx >= fn.source.params.size()) return false;
        const Param& pr = fn.source.params[pIdx];
        if (pr.byRef || !program.isFn(pr.type)) return false;
        std::unordered_set<unsigned> loadIds;
        for (const auto& b : fn.blocks) {
            for (const auto& op : b.ops) {
                if ((op.op == Op::Store || op.op == Op::Drop || op.op == Op::ClearOwner) && op.slot == pIdx)
                    return false;
                for (unsigned ms : op.mutArgSlots)
                    if (ms == pIdx) return false;
                if (op.op == Op::Load && op.slot == pIdx) loadIds.insert(op.result.id);
            }
        }
        if (loadIds.empty()) return true;
        for (const auto& b : fn.blocks) {
            if (b.value && loadIds.contains(b.value->id)) return false;
            for (const auto& op : b.ops) {
                for (std::size_t k = 0; k < op.inputs.size(); ++k) {
                    if (!loadIds.contains(op.inputs[k].id)) continue;
                    if (op.op != Op::ClosureCall || k != 0) return false;
                }
            }
        }
        return true;
    };

    std::unordered_map<std::string, std::size_t> byName;
    for (std::size_t i = 0; i < functions.size(); ++i)
        byName.emplace(functions[i].source.name, i);

    std::vector<std::vector<bool>> nonEscParams(functions.size());
    for (std::size_t i = 0; i < functions.size(); ++i) {
        const std::size_t np = functions[i].source.params.size();
        nonEscParams[i].resize(np, false);
        for (std::size_t p = 0; p < np; ++p)
            nonEscParams[i][p] = isNonEscapingParam(functions[i], p);
    }

    struct SpecKey {
        std::string origName;
        std::vector<std::pair<std::size_t, std::size_t>> paramClosures;
        bool operator==(const SpecKey& o) const = default;
        bool operator<(const SpecKey& o) const {
            if (origName != o.origName) return origName < o.origName;
            return paramClosures < o.paramClosures;
        }
    };
    std::map<SpecKey, std::size_t> specCache;
    std::size_t specCounter = 0;
    const std::size_t origCount = functions.size();
    std::vector<FunctionIR> pendingSpecs;

    auto ensureSpec = [&](std::size_t calleeIdx,
                          const std::vector<std::pair<std::size_t, std::size_t>>& bindings) -> std::optional<std::size_t> {
        const FunctionIR& baseIR = calleeIdx < origCount ? functions[calleeIdx] : pendingSpecs[calleeIdx - origCount];
        SpecKey key{baseIR.source.name, bindings};
        if (auto it = specCache.find(key); it != specCache.end()) return it->second;
        if (specCounter >= 64) return std::nullopt;
        Function specFn = baseIR.source;
        specFn.name += ".cl" + std::to_string(specCounter++);
        specFn.sourceName = specFn.name;
        specFn.isSpecialization = true;
        std::unordered_map<std::size_t, std::size_t> pToCid;
        for (const auto& [pi, cid] : bindings) {
            const auto& cl = program.closures.at(cid);
            const Type repT = cl.envStruct != Type::Void ? cl.envStruct : Type::Int;
            specFn.params[pi].type = repT;
            pToCid[pi] = cid;
        }
        specStorage.push_back(std::move(specFn));
        FunctionIR specIR{specStorage.back(), baseIR.slots, baseIR.blocks,
                          baseIR.usesRegion, baseIR.canStackArena,
                          baseIR.nextValue, baseIR.slotElemBits,
                          baseIR.slotElemUnsigned, baseIR.slotAliasGroup,
                          baseIR.slotByRef};
        for (const auto& [pi, cid] : bindings) {
            const auto& cl = program.closures.at(cid);
            specIR.slots[pi] = cl.envStruct != Type::Void ? cl.envStruct : Type::Int;
        }
        std::unordered_map<unsigned, std::size_t> loadValToParam;
        for (auto& b : specIR.blocks) {
            for (auto& op : b.ops) {
                if (op.op == Op::Load && pToCid.contains(op.slot)) {
                    const auto& cl = program.closures.at(pToCid.at(op.slot));
                    op.result.type = cl.envStruct != Type::Void ? cl.envStruct : Type::Int;
                    loadValToParam[op.result.id] = op.slot;
                }
            }
        }
        for (auto& b : specIR.blocks) {
            for (auto& op : b.ops) {
                if (op.op == Op::ClosureCall && !op.inputs.empty()) {
                    if (auto lt = loadValToParam.find(op.inputs[0].id); lt != loadValToParam.end()) {
                        const auto& cl = program.closures.at(pToCid.at(lt->second));
                        op.op = Op::Call;
                        op.data = cl.fnName;
                        if (cl.envStruct != Type::Void) {
                            op.inputs[0].type = cl.envStruct;
                        } else {
                            op.inputs.erase(op.inputs.begin());
                        }
                    }
                }
            }
        }
        const std::size_t newIdx = origCount + pendingSpecs.size();
        specCache.emplace(std::move(key), newIdx);
        byName.emplace(specIR.source.name, newIdx);
        nonEscParams.emplace_back(specIR.source.params.size(), false);
        pendingSpecs.push_back(std::move(specIR));
        return newIdx;
    };

    for (std::size_t fi = 0; fi < origCount; ++fi) {
        FunctionIR& fn = functions[fi];
        if (fn.source.isTest) continue;

        struct Use { unsigned bi; unsigned oi; int idx; };
        std::unordered_map<unsigned, std::vector<Use>> valUses;
        std::unordered_map<unsigned, std::vector<std::pair<unsigned, unsigned>>> slotStores;
        std::unordered_map<unsigned, std::vector<std::pair<unsigned, unsigned>>> slotLoads;
        std::unordered_set<unsigned> dirtySlots;
        struct McSite { unsigned bi; unsigned oi; unsigned resId; std::size_t cid; };
        std::vector<McSite> mcSites;

        for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
            const auto& b = fn.blocks[bi];
            if (b.value) valUses[b.value->id].push_back({bi, ~0u, -2});
            for (unsigned oi = 0; oi < b.ops.size(); ++oi) {
                const auto& op = b.ops[oi];
                for (std::size_t k = 0; k < op.inputs.size(); ++k)
                    valUses[op.inputs[k].id].push_back({bi, oi, static_cast<int>(k)});
                if (op.op == Op::Store) slotStores[op.slot].emplace_back(bi, oi);
                if (op.op == Op::Load) slotLoads[op.slot].emplace_back(bi, oi);
                for (unsigned ms : op.mutArgSlots)
                    if (ms < fn.slots.size()) dirtySlots.insert(ms);
                if ((op.op == Op::FieldSet || op.op == Op::PlaceSet) && op.slot < fn.slots.size()) dirtySlots.insert(op.slot);
                if (op.op == Op::MakeClosure && !op.fields.empty()) {
                    const std::size_t cid = op.fields[0];
                    if (cid < program.closures.size() && !program.closures[cid].fnName.empty())
                        mcSites.push_back({bi, oi, op.result.id, cid});
                }
            }
        }
        if (mcSites.empty()) continue;

        struct Group {
            McSite site;
            std::unordered_set<unsigned> valIds;
            std::unordered_set<unsigned> slotIds;
            std::vector<std::tuple<unsigned, unsigned, std::size_t>> specCalls; // (bi, oi, argIdx)
        };
        std::vector<Group> okGroups;
        for (const auto& mc : mcSites) {
            Group g{mc, {mc.resId}, {}, {}};
            std::vector<unsigned> work{mc.resId};
            bool escapes = false;
            while (!work.empty() && !escapes) {
                const unsigned vid = work.back();
                work.pop_back();
                const auto uit = valUses.find(vid);
                if (uit == valUses.end()) continue;
                for (const auto& u : uit->second) {
                    if (u.idx == -2) { escapes = true; break; }
                    const auto& uop = fn.blocks[u.bi].ops[u.oi];
                    if (uop.op == Op::ClosureCall) {
                        if (u.idx != 0) { escapes = true; break; }
                        continue;
                    }
                    if (uop.op == Op::Store) {
                        const unsigned s = uop.slot;
                        if (s < fn.source.params.size() || dirtySlots.contains(s)) { escapes = true; break; }
                        bool badSlot = false;
                        for (const auto& [sbi, soi] : slotStores[s]) {
                            const auto& sop = fn.blocks[sbi].ops[soi];
                            if (sop.inputs.size() != 1) { badSlot = true; break; }
                            if (!g.valIds.contains(sop.inputs[0].id)) { badSlot = true; break; }
                        }
                        if (badSlot) { escapes = true; break; }
                        if (g.slotIds.insert(s).second) {
                            for (const auto& [lbi, loi] : slotLoads[s]) {
                                const unsigned lid = fn.blocks[lbi].ops[loi].result.id;
                                if (g.valIds.insert(lid).second) work.push_back(lid);
                            }
                        }
                        continue;
                    }
                    if (uop.op == Op::Call) {
                        auto cit = byName.find(uop.data);
                        if (cit == byName.end() || cit->second == fi) { escapes = true; break; }
                        const std::size_t pIdx = static_cast<std::size_t>(u.idx);
                        if (pIdx >= nonEscParams[cit->second].size() || !nonEscParams[cit->second][pIdx]) {
                            escapes = true;
                            break;
                        }
                        g.specCalls.emplace_back(u.bi, u.oi, pIdx);
                        continue;
                    }
                    escapes = true;
                    break;
                }
            }
            if (!escapes) {
                // Re-verify all stores to slots in g.slotIds only come from g.valIds (handles Store before Load)
                bool slotClean = true;
                for (unsigned s : g.slotIds) {
                    for (const auto& [sbi, soi] : slotStores[s]) {
                        const auto& sop = fn.blocks[sbi].ops[soi];
                        if (!g.valIds.contains(sop.inputs[0].id)) { slotClean = false; break; }
                    }
                    if (!slotClean) break;
                }
                if (slotClean) okGroups.push_back(std::move(g));
            }
        }
        if (okGroups.empty()) continue;

        // Collect per-call-site parameter bindings only if every participating group can specialize
        struct CallSiteKey {
            unsigned bi; unsigned oi;
            bool operator<(const CallSiteKey& o) const { return bi != o.bi ? bi < o.bi : oi < o.oi; }
        };
        std::map<CallSiteKey, std::vector<std::tuple<std::size_t, std::size_t, std::size_t>>> siteArgs; // (paramIdx, cid, groupIdx)
        for (std::size_t gi = 0; gi < okGroups.size(); ++gi) {
            for (const auto& [bi, oi, pIdx] : okGroups[gi].specCalls) {
                siteArgs[{bi, oi}].emplace_back(pIdx, okGroups[gi].site.cid, gi);
            }
        }
        std::vector<bool> groupCancelled(okGroups.size(), false);
        std::map<CallSiteKey, std::size_t> finalizedCalls;
        for (auto& [ck, entries] : siteArgs) {
            std::sort(entries.begin(), entries.end());
            std::vector<std::pair<std::size_t, std::size_t>> bindings;
            for (const auto& [pIdx, cid, gi] : entries) {
                (void)gi;
                if (!bindings.empty() && bindings.back().first == pIdx) continue;
                bindings.emplace_back(pIdx, cid);
            }
            const auto calleeIt = byName.find(fn.blocks[ck.bi].ops[ck.oi].data);
            if (calleeIt == byName.end()) {
                for (const auto& e : entries) groupCancelled[std::get<2>(e)] = true;
                continue;
            }
            if (auto specIdx = ensureSpec(calleeIt->second, bindings)) {
                finalizedCalls.emplace(ck, *specIdx);
            } else {
                for (const auto& e : entries) groupCancelled[std::get<2>(e)] = true;
            }
        }

        for (const auto& [ck, specIdx] : finalizedCalls) {
            bool allActive = true;
            for (const auto& e : siteArgs.at(ck)) {
                if (groupCancelled[std::get<2>(e)]) { allActive = false; break; }
            }
            if (!allActive) {
                for (const auto& e : siteArgs.at(ck)) groupCancelled[std::get<2>(e)] = true;
                continue;
            }
            const FunctionIR& sIR = specIdx < origCount ? functions[specIdx] : pendingSpecs[specIdx - origCount];
            fn.blocks[ck.bi].ops[ck.oi].data = sIR.source.name;
        }

        std::vector<std::vector<bool>> eraseOp(fn.blocks.size());
        for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) eraseOp[bi].assign(fn.blocks[bi].ops.size(), false);

        for (std::size_t gi = 0; gi < okGroups.size(); ++gi) {
            if (groupCancelled[gi]) continue;
            const Group& g = okGroups[gi];
            const auto& cl = program.closures.at(g.site.cid);
            const Type repT = cl.envStruct != Type::Void ? cl.envStruct : Type::Int;
            const bool dropEnv = cl.envStruct != Type::Void && needsDropStruct(cl.envStruct);

            for (unsigned s : g.slotIds) fn.slots[s] = repT;

            // Patch Load/Store/Drop/ClearOwner on slotIds
            for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
                for (unsigned oi = 0; oi < fn.blocks[bi].ops.size(); ++oi) {
                    auto& op = fn.blocks[bi].ops[oi];
                    if (op.op == Op::Load && g.slotIds.contains(op.slot)) {
                        op.result.type = repT;
                    } else if ((op.op == Op::Drop || op.op == Op::ClearOwner) && g.slotIds.contains(op.slot)) {
                        if (!dropEnv) eraseOp[bi][oi] = true;
                    }
                }
            }

            // Rewrite ClosureCall on valIds into direct Call
            for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
                for (unsigned oi = 0; oi < fn.blocks[bi].ops.size(); ++oi) {
                    auto& op = fn.blocks[bi].ops[oi];
                    if (op.op == Op::ClosureCall && !op.inputs.empty() && g.valIds.contains(op.inputs[0].id)) {
                        op.op = Op::Call;
                        op.data = cl.fnName;
                        if (cl.envStruct != Type::Void) op.inputs[0].type = repT;
                        else op.inputs.erase(op.inputs.begin());
                    }
                }
            }

            // Eliminate MakeClosure op
            auto& mcOp = fn.blocks[g.site.bi].ops[g.site.oi];
            if (cl.envStruct != Type::Void) {
                const Value envVal = mcOp.inputs.at(0);
                detail::replaceValueWithTypeInFunction(fn, mcOp.result.id, envVal);
                eraseOp[g.site.bi][g.site.oi] = true;
            } else {
                mcOp.op = Op::Int;
                mcOp.data = "0";
                mcOp.result.type = Type::Int;
                mcOp.inputs.clear();
                mcOp.fields.clear();
                detail::replaceValueWithTypeInFunction(fn, mcOp.result.id, mcOp.result);
            }
        }

        for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
            auto& ops = fn.blocks[bi].ops;
            for (std::size_t oi = ops.size(); oi > 0; --oi) {
                if (eraseOp[bi][oi - 1]) ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(oi - 1));
            }
        }
    }
    for (auto& spec : pendingSpecs) functions.push_back(std::move(spec));
}

namespace detail {

struct SlotLiveness {
    std::size_t nSlots = 0;
    std::size_t words = 0;
    std::vector<std::vector<std::uint64_t>> liveIn;
    std::vector<std::vector<std::uint64_t>> liveOut;

    static void setBit(std::vector<std::uint64_t>& bs, unsigned slot) {
        bs[slot >> 6] |= (1ULL << (slot & 63u));
    }
    static void clearBit(std::vector<std::uint64_t>& bs, unsigned slot) {
        bs[slot >> 6] &= ~(1ULL << (slot & 63u));
    }
    static bool testBit(const std::vector<std::uint64_t>& bs, unsigned slot) {
        return slot < bs.size() * 64u && ((bs[slot >> 6] >> (slot & 63u)) & 1ULL) != 0ULL;
    }
};

inline SlotLiveness computeSlotLiveness(const FunctionIR& fn, const CFGInfo& cfg) {
    SlotLiveness liveness;
    const std::size_t nSlots = fn.slots.size();
    const std::size_t nb = fn.blocks.size();
    const std::size_t words = (nSlots + 63u) / 64u;
    liveness.nSlots = nSlots;
    liveness.words = words;
    liveness.liveIn.assign(nb, std::vector<std::uint64_t>(words, 0ULL));
    liveness.liveOut.assign(nb, std::vector<std::uint64_t>(words, 0ULL));
    if (words == 0 || nb == 0) return liveness;

    std::vector<std::vector<std::uint64_t>> upward(nb, std::vector<std::uint64_t>(words, 0ULL));
    std::vector<std::vector<std::uint64_t>> killed(nb, std::vector<std::uint64_t>(words, 0ULL));
    for (std::size_t bi = 0; bi < nb; ++bi) {
        const auto& ops = fn.blocks[bi].ops;
        for (std::size_t oi = ops.size(); oi > 0; --oi) {
            const auto& op = ops[oi - 1];
            if (op.op == Op::Store || op.op == Op::ClearOwner) {
                if (op.slot < nSlots) {
                    SlotLiveness::clearBit(upward[bi], op.slot);
                    SlotLiveness::setBit(killed[bi], op.slot);
                }
            }
            if (op.op == Op::Load || op.op == Op::FieldSet || op.op == Op::PlaceSet) {
                if (op.slot < nSlots) SlotLiveness::setBit(upward[bi], op.slot);
            }
            for (unsigned ms : op.mutArgSlots) {
                if (ms < nSlots) SlotLiveness::setBit(upward[bi], ms);
            }
            for (const auto& path : op.mutArgPaths) {
                for (const auto& step : path)
                    if (step.isIndex && step.idxSlot < nSlots) SlotLiveness::setBit(upward[bi], step.idxSlot);
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t r = cfg.rpo.size(); r > 0; --r) {
            const unsigned bi = cfg.rpo[r - 1];
            auto& out = liveness.liveOut[bi];
            std::fill(out.begin(), out.end(), 0ULL);
            for (unsigned succ : cfg.succs[bi]) {
                if (succ < nb) {
                    for (std::size_t w = 0; w < words; ++w) out[w] |= liveness.liveIn[succ][w];
                }
            }
            for (std::size_t w = 0; w < words; ++w) {
                const std::uint64_t nextIn = upward[bi][w] | (out[w] & ~killed[bi][w]);
                if (nextIn != liveness.liveIn[bi][w]) {
                    liveness.liveIn[bi][w] = nextIn;
                    changed = true;
                }
            }
        }
    }
    return liveness;
}

inline bool fieldPathsOverlap(const std::vector<unsigned>& a, const std::vector<unsigned>& b) {
    const std::size_t m = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < m; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

struct SlotBorrowInfo {
    std::unordered_map<unsigned, unsigned> borrowRoot;
    std::unordered_map<unsigned, std::size_t> defBlock;
    std::unordered_map<unsigned, std::size_t> defIdx;
};

inline SlotBorrowInfo computeSlotBorrowInfo(const FunctionIR& fn) {
    SlotBorrowInfo info;
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto& ops = fn.blocks[bi].ops;
        for (std::size_t oi = 0; oi < ops.size(); ++oi) {
            const auto& op = ops[oi];
            if (op.result.type != Type::Void) {
                info.defBlock[op.result.id] = bi;
                info.defIdx[op.result.id] = oi;
                if (op.op == Op::Load) info.borrowRoot[op.result.id] = op.slot;
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& blk : fn.blocks) {
            for (const auto& op : blk.ops) {
                if ((op.op == Op::Field || op.op == Op::Index) &&
                    !op.inputs.empty() && op.result.type != Type::Void) {
                    auto it = info.borrowRoot.find(op.inputs[0].id);
                    if (it != info.borrowRoot.end() && !info.borrowRoot.contains(op.result.id)) {
                        info.borrowRoot[op.result.id] = it->second;
                        changed = true;
                    }
                }
            }
        }
    }
    return info;
}

inline bool hasLiveBorrowAcross(const FunctionIR& fn,
                                std::size_t bi,
                                std::size_t idx,
                                unsigned S,
                                const SlotBorrowInfo& info) {
    auto isCrossUse = [&](unsigned vid, std::size_t useBlk) -> bool {
        auto bit = info.borrowRoot.find(vid);
        if (bit == info.borrowRoot.end() || bit->second != S) return false;
        auto db = info.defBlock.find(vid);
        auto di = info.defIdx.find(vid);
        if (db == info.defBlock.end() || di == info.defIdx.end()) return false;
        if (useBlk == bi) return db->second != bi || di->second < idx;
        return db->second != useBlk;
    };
    const auto& curBlk = fn.blocks[bi];
    for (std::size_t m = idx + 1; m < curBlk.ops.size(); ++m) {
        for (const auto& in : curBlk.ops[m].inputs) {
            if (isCrossUse(in.id, bi)) return true;
        }
    }
    if (curBlk.value && isCrossUse(curBlk.value->id, bi)) return true;
    for (std::size_t b2 = 0; b2 < fn.blocks.size(); ++b2) {
        if (b2 == bi) continue;
        const auto& other = fn.blocks[b2];
        for (const auto& op : other.ops) {
            for (const auto& in : op.inputs) {
                if (isCrossUse(in.id, b2)) return true;
            }
        }
        if (other.value && isCrossUse(other.value->id, b2)) return true;
    }
    return false;
}

} // namespace detail

// Convert Clone(v) into zero-clone move (ClearOwner / ClearField / ClearIndex) at proven last uses.
inline void elideLastUseMoves(FunctionIR& fn, const Program& program) {
    const std::size_t nSlots = fn.slots.size();
    if (nSlots == 0 || fn.blocks.empty()) return;

    auto isStructType = [&](Type t) -> bool {
        if (t < Type::NamedBase) return false;
        const auto* d = program.declaration(t);
        return d && !d->isEnum && !d->variants.empty();
    };

    const detail::CFGInfo cfg = detail::buildCFG(fn);
    const detail::SlotLiveness lv = detail::computeSlotLiveness(fn, cfg);
    const detail::SlotBorrowInfo borrowInfo = detail::computeSlotBorrowInfo(fn);

    std::unordered_map<unsigned, std::size_t> useCount;
    for (const auto& b : fn.blocks) {
        for (const auto& op : b.ops) {
            for (const auto& in : op.inputs) ++useCount[in.id];
        }
        if (b.value) ++useCount[b.value->id];
    }

    auto replaceUse = [&](unsigned oldId, unsigned newId) {
        replaceValueIdInFunction(fn, oldId, newId);
        if (oldId < fn.nextValue) useCount[newId] += (useCount[oldId] > 0 ? useCount[oldId] - 1 : 0);
    };

    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto& ops = fn.blocks[bi].ops;
        std::vector<std::uint64_t> live = lv.liveOut[bi];

        std::unordered_map<unsigned, std::size_t> defInBlk;
        std::unordered_map<unsigned, std::pair<std::size_t, const Operation*>> consumerInBlk;
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (ops[i].result.type != Type::Void) defInBlk[ops[i].result.id] = i;
            for (const auto& in : ops[i].inputs) {
                if (!consumerInBlk.contains(in.id)) consumerInBlk.emplace(in.id, std::pair{i, &ops[i]});
            }
        }

        auto isOwnedLocal = [&](unsigned slot) -> bool {
            return slot < fn.slotOwned.size() && fn.slotOwned[slot];
        };

        auto opMutatesOrDropsSlot = [&](const Operation& op, unsigned S) -> bool {
            if ((op.op == Op::Store || op.op == Op::Drop || op.op == Op::ClearOwner ||
                 op.op == Op::FieldSet || op.op == Op::PlaceSet || op.op == Op::ClearField) && op.slot == S)
                return true;
            if (op.op == Op::IndexSet || op.op == Op::ClearIndex) {
                if (!op.inputs.empty()) {
                    if (auto ls = defInBlk.find(op.inputs[0].id);
                        ls != defInBlk.end() && ops[ls->second].op == Op::Load && ops[ls->second].slot == S)
                        return true;
                }
            }
            for (unsigned ms : op.mutArgSlots)
                if (ms == S) return true;
            return false;
        };

        for (std::size_t oi = ops.size(); oi > 0; --oi) {
            const std::size_t idx = oi - 1;
            Operation& op = ops[idx];

            if (op.op == Op::Clone && !op.inputs.empty() && useCount[op.inputs[0].id] == 1) {
                const Value clonedSrc = op.inputs[0];
                const unsigned srcId = clonedSrc.id;

                auto defIt = defInBlk.find(srcId);
                if (defIt != defInBlk.end()) {
                    const std::size_t defIdx = defIt->second;
                    const Operation& srcOp = ops[defIdx];

                    // Case A: Direct local move Load(S) -> Clone
                    if (srcOp.op == Op::Load) {
                        const unsigned S = srcOp.slot;
                        if (isOwnedLocal(S) && !detail::SlotLiveness::testBit(live, S) &&
                            !detail::hasLiveBorrowAcross(fn, bi, idx, S, borrowInfo)) {
                            bool cleanBetween = true;
                            for (std::size_t k = defIdx + 1; k < idx; ++k) {
                                if (opMutatesOrDropsSlot(ops[k], S)) { cleanBetween = false; break; }
                            }
                            if (cleanBetween) {
                                const Span sp = op.span;
                                replaceUse(op.result.id, srcId);
                                op = Operation{Op::ClearOwner, {Type::Void, op.result.id}, {}, {}, S, sp};
                                continue;
                            }
                        }
                    }

                    // Case B: Disjoint/dead struct field chain move Load(S) -> Field... -> Clone
                    if (srcOp.op == Op::Field) {
                        std::vector<unsigned> pathRev;
                        unsigned curId = srcId;
                        bool chainOk = true;
                        std::optional<std::size_t> loadIdx;
                        while (true) {
                            auto dit = defInBlk.find(curId);
                            if (dit == defInBlk.end()) { chainOk = false; break; }
                            const Operation& step = ops[dit->second];
                            if (step.op == Op::Load) { loadIdx = dit->second; break; }
                            if (step.op != Op::Field || step.fields.size() != 1 || step.inputs.empty() ||
                                useCount[curId] != 1 || !isStructType(step.inputs[0].type)) {
                                chainOk = false;
                                break;
                            }
                            pathRev.push_back(step.fields[0]);
                            curId = step.inputs[0].id;
                        }
                        if (chainOk && loadIdx && !pathRev.empty() && useCount[ops[*loadIdx].result.id] == 1) {
                            const std::vector<unsigned> path(pathRev.rbegin(), pathRev.rend());
                            const unsigned S = ops[*loadIdx].slot;
                            if (isOwnedLocal(S) && !detail::hasLiveBorrowAcross(fn, bi, idx, S, borrowInfo)) {
                                bool cleanBetween = true;
                                for (std::size_t k = *loadIdx + 1; k < idx; ++k) {
                                    if (ops[k].op == Op::ClearField && ops[k].slot == S) {
                                        if (detail::fieldPathsOverlap(path, ops[k].fields)) { cleanBetween = false; break; }
                                        continue;
                                    }
                                    if (opMutatesOrDropsSlot(ops[k], S)) { cleanBetween = false; break; }
                                }
                                bool safeAfter = !detail::SlotLiveness::testBit(live, S);
                                if (cleanBetween && !safeAfter && !detail::SlotLiveness::testBit(lv.liveOut[bi], S)) {
                                    // Verify every post-idx use in this block is a disjoint struct field read.
                                    bool okAfter = true;
                                    for (std::size_t k = idx + 1; k < ops.size(); ++k) {
                                        const Operation& aop = ops[k];
                                        if ((aop.op == Op::Store || aop.op == Op::ClearOwner) && aop.slot == S) break;
                                        if (aop.op == Op::Drop && aop.slot == S) continue;
                                        if (aop.op == Op::ClearField && aop.slot == S) {
                                            if (detail::fieldPathsOverlap(path, aop.fields)) { okAfter = false; break; }
                                            continue;
                                        }
                                        if (opMutatesOrDropsSlot(aop, S)) { okAfter = false; break; }
                                        if (aop.op == Op::Load && aop.slot == S) {
                                            unsigned rid = aop.result.id;
                                            std::vector<unsigned> rpath;
                                            bool diverged = false;
                                            while (useCount[rid] == 1) {
                                                auto cit = consumerInBlk.find(rid);
                                                if (cit == consumerInBlk.end()) break;
                                                const Operation* cop = cit->second.second;
                                                if (cop->op != Op::Field || cop->fields.size() != 1 ||
                                                    !isStructType(cop->inputs[0].type)) break;
                                                rpath.push_back(cop->fields[0]);
                                                if (!detail::fieldPathsOverlap(path, rpath)) { diverged = true; break; }
                                                rid = cop->result.id;
                                            }
                                            if (!diverged) { okAfter = false; break; }
                                        }
                                    }
                                    safeAfter = okAfter;
                                }
                                if (cleanBetween && safeAfter) {
                                    const Span sp = op.span;
                                    replaceUse(op.result.id, srcId);
                                    op = Operation{Op::ClearField, {Type::Void, op.result.id}, {}, {}, S, sp, path};
                                    continue;
                                }
                            }
                        }
                    }

                    // Case C: Nested collection element move Load(S) -> Index(base, index) -> Clone
                    if (srcOp.op == Op::Index && srcOp.inputs.size() == 2 &&
                        program.isArray(srcOp.inputs[0].type) && needsDropType(program, clonedSrc.type)) {
                        const Value arrBase = srcOp.inputs[0];
                        const Value arrIdx = srcOp.inputs[1];
                        auto lit = defInBlk.find(arrBase.id);
                        if (lit != defInBlk.end() && ops[lit->second].op == Op::Load) {
                            const unsigned S = ops[lit->second].slot;
                            if (isOwnedLocal(S) && !detail::SlotLiveness::testBit(live, S) &&
                                !detail::hasLiveBorrowAcross(fn, bi, idx, S, borrowInfo)) {
                                bool cleanBetween = true;
                                for (std::size_t k = lit->second + 1; k < idx; ++k) {
                                    if (opMutatesOrDropsSlot(ops[k], S)) { cleanBetween = false; break; }
                                }
                                if (cleanBetween) {
                                    const Span sp = op.span;
                                    replaceUse(op.result.id, srcId);
                                    op = Operation{Op::ClearIndex, {Type::Void, op.result.id}, {arrBase, arrIdx}, {}, 0, sp};
                                    op.boundsProven = true;
                                    continue;
                                }
                            }
                        }
                    }
                }
            }

            // Update backward liveness at op (unmodified op).
            if ((op.op == Op::Store || op.op == Op::ClearOwner) && op.slot < nSlots)
                detail::SlotLiveness::clearBit(live, op.slot);
            if ((op.op == Op::Load || op.op == Op::FieldSet || op.op == Op::PlaceSet) && op.slot < nSlots)
                detail::SlotLiveness::setBit(live, op.slot);
            for (unsigned ms : op.mutArgSlots)
                if (ms < nSlots) detail::SlotLiveness::setBit(live, ms);
            for (const auto& path : op.mutArgPaths) {
                for (const auto& step : path)
                    if (step.isIndex && step.idxSlot < nSlots) detail::SlotLiveness::setBit(live, step.idxSlot);
            }
        }
    }
}

// Identify Pool<T> locals with no poolRemove/clear/mut-escape so poolGet/poolHas elide meta[] loads.
inline void analyzeMonotonicPools(FunctionIR& fn, const Program& program) {
    const std::size_t nSlots = fn.slots.size();
    if (nSlots == 0) return;

    std::vector<bool> hasPoolOrigin(nSlots, false);
    std::vector<bool> badPool(nSlots, false);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> loadSlot;

    for (std::size_t s = 0; s < nSlots; ++s) {
        if (!program.isPool(fn.slots[s]) || s < fn.source.params.size() ||
            (s < fn.slotByRef.size() && fn.slotByRef[s])) {
            badPool[s] = true;
        }
    }

    for (const auto& blk : fn.blocks) {
        for (const auto& op : blk.ops) {
            if (op.result.type != Type::Void) defOp[op.result.id] = &op;
            if (op.op == Op::Load && op.slot < nSlots) loadSlot[op.result.id] = op.slot;
        }
    }

    for (const auto& blk : fn.blocks) {
        for (const auto& op : blk.ops) {
            if (op.op == Op::Store && op.slot < nSlots && program.isPool(fn.slots[op.slot]) && !op.inputs.empty()) {
                auto dit = defOp.find(op.inputs[0].id);
                if (dit != defOp.end() && dit->second->op == Op::Builtin &&
                    (dit->second->data == "poolNew" || dit->second->data == "poolWithCap")) {
                    hasPoolOrigin[op.slot] = true;
                } else {
                    badPool[op.slot] = true;
                }
            } else if (op.op == Op::ClearField && op.slot < nSlots) {
                badPool[op.slot] = true;
            } else if (op.op == Op::Builtin) {
                const bool safePoolMut =
                    (op.data == "poolAdd" || op.data == "poolSet" || op.data == "reserve");
                for (unsigned ms : op.mutArgSlots) {
                    if (ms < nSlots && (!safePoolMut || op.data == "poolRemove" || op.data == "clear"))
                        badPool[ms] = true;
                }
            } else if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall || op.op == Op::ClosureCall) {
                for (unsigned ms : op.mutArgSlots)
                    if (ms < nSlots) badPool[ms] = true;
            }
        }
    }

    std::unordered_set<unsigned> monotonicCreateIds, nonMonotonicCreateIds;
    for (auto& blk : fn.blocks) {
        for (auto& op : blk.ops) {
            if (op.op == Op::Store) {
                if (!op.inputs.empty()) {
                    if (op.slot < nSlots && hasPoolOrigin[op.slot] && !badPool[op.slot])
                        monotonicCreateIds.insert(op.inputs[0].id);
                    else
                        nonMonotonicCreateIds.insert(op.inputs[0].id);
                }
            } else {
                for (const auto& in : op.inputs) nonMonotonicCreateIds.insert(in.id);
            }
            if (op.op == Op::Builtin && (op.data == "poolGet" || op.data == "poolHas") && !op.inputs.empty()) {
                auto ls = loadSlot.find(op.inputs[0].id);
                if (ls != loadSlot.end() && ls->second < nSlots &&
                    hasPoolOrigin[ls->second] && !badPool[ls->second]) {
                    op.boundsProven = true;
                }
            }
        }
        if (blk.value) nonMonotonicCreateIds.insert(blk.value->id);
    }
    for (auto& blk : fn.blocks) {
        for (auto& op : blk.ops) {
            if (op.op == Op::Builtin && (op.data == "poolNew" || op.data == "poolWithCap") &&
                monotonicCreateIds.contains(op.result.id) &&
                !nonMonotonicCreateIds.contains(op.result.id)) {
                op.boundsProven = true;
            }
        }
    }
}

// Perceus drop-guided reuse: replace sharedNew when an owned Shared<T> of identical type is dead immediately after.
inline void fusePerceusReuse(FunctionIR& fn, const Program& program) {
    const std::size_t nSlots = fn.slots.size();
    const std::size_t nb = fn.blocks.size();
    if (nSlots == 0 || nb == 0) return;

    bool hasSharedNew = false;
    for (const auto& blk : fn.blocks) {
        for (const auto& op : blk.ops) {
            if (op.op == Op::Builtin && op.data == "sharedNew") { hasSharedNew = true; break; }
        }
        if (hasSharedNew) break;
    }
    if (!hasSharedNew) return;

    const detail::CFGInfo cfg = detail::buildCFG(fn);
    const detail::SlotLiveness lv = detail::computeSlotLiveness(fn, cfg);
    detail::SlotBorrowInfo borrowInfo = detail::computeSlotBorrowInfo(fn);
    const std::size_t words = lv.words;

    // Forward definite-initialized bitset for owned Shared<T> local slots.
    std::vector<std::uint64_t> allSharedMask(words, 0ULL);
    for (std::size_t s = 0; s < nSlots && s < fn.slotOwned.size(); ++s) {
        if (fn.slotOwned[s] && program.isShared(fn.slots[s]))
            detail::SlotLiveness::setBit(allSharedMask, static_cast<unsigned>(s));
    }
    bool anySharedSlot = false;
    for (auto w : allSharedMask) if (w != 0ULL) anySharedSlot = true;
    if (!anySharedSlot) return;

    std::vector<std::vector<std::uint64_t>> defIn(nb, allSharedMask);
    std::vector<std::vector<std::uint64_t>> defOut(nb, allSharedMask);
    std::fill(defIn[0].begin(), defIn[0].end(), 0ULL);

    bool changed = true;
    while (changed) {
        changed = false;
        for (unsigned bi : cfg.rpo) {
            if (bi != 0) {
                std::vector<std::uint64_t> acc = allSharedMask;
                bool anyPred = false;
                for (unsigned pred : cfg.preds[bi]) {
                    if (cfg.rpoPos[pred] < 0) continue;
                    if (!anyPred) { acc = defOut[pred]; anyPred = true; }
                    else for (std::size_t w = 0; w < words; ++w) acc[w] &= defOut[pred][w];
                }
                if (!anyPred) std::fill(acc.begin(), acc.end(), 0ULL);
                defIn[bi] = std::move(acc);
            }
            std::vector<std::uint64_t> cur = defIn[bi];
            for (const auto& op : fn.blocks[bi].ops) {
                if (op.op == Op::Store && op.slot < nSlots &&
                    detail::SlotLiveness::testBit(allSharedMask, op.slot)) {
                    detail::SlotLiveness::setBit(cur, op.slot);
                } else if ((op.op == Op::Drop || op.op == Op::ClearOwner) && op.slot < nSlots) {
                    detail::SlotLiveness::clearBit(cur, op.slot);
                }
            }
            if (cur != defOut[bi]) {
                defOut[bi] = std::move(cur);
                changed = true;
            }
        }
    }

    for (std::size_t bi = 0; bi < nb; ++bi) {
        auto& ops = fn.blocks[bi].ops;
        if (ops.empty()) continue;

        // Precompute backward live-after bitsets per op in block bi.
        std::vector<std::vector<std::uint64_t>> liveAfter(ops.size(), lv.liveOut[bi]);
        std::vector<std::uint64_t> live = lv.liveOut[bi];
        for (std::size_t oi = ops.size(); oi > 0; --oi) {
            const std::size_t idx = oi - 1;
            liveAfter[idx] = live;
            const auto& op = ops[idx];
            if ((op.op == Op::Store || op.op == Op::ClearOwner) && op.slot < nSlots)
                detail::SlotLiveness::clearBit(live, op.slot);
            if ((op.op == Op::Load || op.op == Op::FieldSet || op.op == Op::PlaceSet) && op.slot < nSlots)
                detail::SlotLiveness::setBit(live, op.slot);
            for (unsigned ms : op.mutArgSlots)
                if (ms < nSlots) detail::SlotLiveness::setBit(live, ms);
            for (const auto& path : op.mutArgPaths) {
                for (const auto& step : path)
                    if (step.isIndex && step.idxSlot < nSlots) detail::SlotLiveness::setBit(live, step.idxSlot);
            }
        }

        std::vector<std::uint64_t> defCur = defIn[bi];
        std::vector<bool> claimed(nSlots, false);
        for (std::size_t idx = 0; idx < ops.size(); ++idx) {
            if (ops[idx].op == Op::Builtin && ops[idx].data == "sharedNew" && !ops[idx].inputs.empty()) {
                const Type shT = ops[idx].result.type;
                std::optional<unsigned> chosen;
                for (std::size_t s = 0; s < nSlots; ++s) {
                    if (!detail::SlotLiveness::testBit(allSharedMask, static_cast<unsigned>(s))) continue;
                    if (fn.slots[s] != shT || claimed[s]) continue;
                    if (detail::SlotLiveness::testBit(defCur, static_cast<unsigned>(s)) &&
                        !detail::SlotLiveness::testBit(liveAfter[idx], static_cast<unsigned>(s)) &&
                        !detail::hasLiveBorrowAcross(fn, bi, idx, static_cast<unsigned>(s), borrowInfo)) {
                        chosen = static_cast<unsigned>(s);
                        break;
                    }
                }
                if (chosen) {
                    const unsigned S = *chosen;
                    claimed[S] = true;
                    detail::SlotLiveness::clearBit(defCur, S);
                    const Span sp = ops[idx].span;
                    const Value oldVal{shT, fn.nextValue++};
                    ops[idx].data = "sharedReuse";
                    ops[idx].inputs.insert(ops[idx].inputs.begin(), oldVal);
                    ops[idx].boundsProven = true;
                    Operation loadOld{Op::Load, oldVal, {}, {}, S, sp};
                    Operation clearOld{Op::ClearOwner, {Type::Void, fn.nextValue++}, {}, {}, S, sp};
                    ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(idx), clearOld);
                    ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(idx), loadOld);
                    liveAfter.insert(liveAfter.begin() + static_cast<std::ptrdiff_t>(idx), 2, liveAfter[idx]);
                    borrowInfo.defBlock[oldVal.id] = bi;
                    borrowInfo.defIdx[oldVal.id] = idx;
                    borrowInfo.borrowRoot[oldVal.id] = S;
                    for (std::size_t oi = idx + 2; oi < ops.size(); ++oi) {
                        if (ops[oi].result.type != Type::Void) {
                            borrowInfo.defBlock[ops[oi].result.id] = bi;
                            borrowInfo.defIdx[ops[oi].result.id] = oi;
                        }
                    }
                    idx += 2;
                    continue;
                }
            }
            const auto& op = ops[idx];
            if (op.op == Op::Store && op.slot < nSlots && detail::SlotLiveness::testBit(allSharedMask, op.slot)) {
                detail::SlotLiveness::setBit(defCur, op.slot);
                claimed[op.slot] = false;
            } else if ((op.op == Op::Drop || op.op == Op::ClearOwner) && op.slot < nSlots) {
                detail::SlotLiveness::clearBit(defCur, op.slot);
            }
        }
    }
}

// A node reached through a cycle never leaves the pending set. Every other
// node has an exact static upper bound on active frames, including isolated roots.
inline std::vector<bool> safeDepths(const Program& program, const std::vector<FunctionIR>& functions) {
    const auto n = functions.size();
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < n; ++i) index.emplace(functions[i].source.name, i);
    std::vector<std::vector<std::size_t>> edges(n);
    std::vector<std::size_t> pending(n), depth(n, 1);
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto& block : functions[i].blocks) {
            for (const auto& op : block.ops) {
                if (op.op == Op::Call) {
                    auto to = index.at(op.data);
                    edges[i].push_back(to);
                    ++pending[to];
                } else if (op.op == Op::DynCall) {
                    const Type iface = static_cast<Type>(op.fields.at(0));
                    const std::size_t mi = op.fields.at(1);
                    bool added = false;
                    for (const auto& dv : program.dynVtables) {
                        if (dv.iface == iface && mi < dv.methods.size() && !dv.methods[mi].empty()) {
                            const auto it = index.find(dv.methods[mi]);
                            if (it != index.end()) {
                                edges[i].push_back(it->second);
                                ++pending[it->second];
                                added = true;
                            }
                        }
                    }
                    if (!added) {
                        edges[i].push_back(i);
                        ++pending[i];
                    }
                } else if (op.op == Op::ClosureCall ||
                           (op.op == Op::Builtin &&
                            ((op.data == "sharedUpdate" && op.inputs.size() >= 2) ||
                             (op.data == "frozenRead" && op.inputs.size() >= 2) ||
                             (op.data == "taskSpawn" && op.inputs.size() >= 2) ||
                             (op.data == "taskSpawnMove" && op.inputs.size() >= 3) ||
                             (op.data == "parallelMap" && op.inputs.size() >= 2)))) {
                    const Type fnT = (op.op == Op::ClosureCall) ? op.inputs.at(0).type
                                   : (op.data == "taskSpawnMove" ? op.inputs.at(2).type : op.inputs.at(1).type);
                    bool added = false;
                    for (const auto& cl : program.closures) {
                        if (cl.fnType == fnT && !cl.fnName.empty()) {
                            const auto it = index.find(cl.fnName);
                            if (it != index.end()) {
                                edges[i].push_back(it->second);
                                ++pending[it->second];
                                added = true;
                            }
                        }
                    }
                    if (!added) {
                        edges[i].push_back(i);
                        ++pending[i];
                    }
                }
            }
        }
    }
    std::queue<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i) if (pending[i] == 0) ready.push(i);
    std::vector<bool> safe(n, false);
    while (!ready.empty()) {
        auto i = ready.front();
        ready.pop();
        safe[i] = depth[i] < 256;
        for (auto to : edges[i]) {
            depth[to] = std::max(depth[to], depth[i] + 1);
            if (--pending[to] == 0) ready.push(to);
        }
    }
    return safe;
}

/// Cache `arr[bestSlot]` into a scalar temporary across argmin/argmax `for` loops
/// (e.g. selection sort) so the inner loop keeps the running extremum in a register.
inline void cacheArgExtremeValue(
    FunctionIR& fn,
    const std::unordered_map<unsigned, std::optional<IntRange>>& elemRanges = {}) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, unsigned> loadSlot;
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const auto& op : fn.blocks[bi].ops) {
            if (op.result.type != Type::Void) {
                defOp[op.result.id] = &op;
                defBlock[op.result.id] = bi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
        }
    }

    auto dominates = [&](unsigned a, unsigned b) -> bool {
        unsigned cur = b;
        for (std::size_t steps = 0; steps <= fn.blocks.size(); ++steps) {
            if (cur == a) return true;
            if (cur == 0 || cur >= cfg.idom.size() || cfg.idom[cur] < 0) return false;
            cur = static_cast<unsigned>(cfg.idom[cur]);
        }
        return false;
    };

    auto isSafeToHoistPast = [](const Operation& op) -> bool {
        if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin ||
            op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
            op.op == Op::Print || op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice ||
            op.op == Op::FieldSet || op.op == Op::PlaceSet || op.op == Op::RepeatHeap ||
            op.op == Op::RepeatRegion || op.op == Op::Clone || op.op == Op::ClearOwner ||
            op.op == Op::ClearField || op.op == Op::ClearIndex || op.op == Op::IndexSet ||
            ((op.op == Op::Index) && !op.boundsProven) ||
            (op.op == Op::Assert && !op.boundsProven && !op.unchecked)) {
            return false;
        }
        if ((op.op == Op::Negate || op.op == Op::AbsDiff) && op.result.type == Type::Int && !op.overflowProven)
            return false;
        if (op.op == Op::Binary && op.inputs.size() >= 2 && op.inputs[0].type == Type::Int) {
            if (op.data == "/" || op.data == "%" || op.data == "<<" || op.data == ">>" || op.data == ">>>")
                return false;
            if ((op.data == "+" || op.data == "-" || op.data == "*") && !op.overflowProven)
                return false;
        }
        return true;
    };

    struct Plan {
        unsigned preBlk = 0, hdrBlk = 0, cmpBlk = 0, tbBlk = 0, ebBlk = 0, latchBlk = 0, exitBlk = 0;
        unsigned arrSlot = 0, bestSlot = 0, iterSlot = 0;
        Value jLower{}, jUpper{};
        Span hdrSpan{};
        unsigned idxBestId = 0;
        Value idxIterVal{};
        Span idxIterSpan{};
        Operation idxTemplate{};
        std::string cmpOp;
        bool pureTwoPhase = false;
    };
    std::vector<Plan> plans;
    std::unordered_set<unsigned> usedPreBlocks;

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lp = cfg.loops[li];
        if (lp.preheaders.size() != 1) continue;
        const unsigned hdr = lp.header;
        const unsigned pre = lp.preheaders[0];
        if (usedPreBlocks.contains(pre)) continue;
        const auto& hblk = fn.blocks[hdr];
        if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
            continue;
        if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != hdr) continue;
        const unsigned cmpBlk = hblk.yes;
        const unsigned exitBlk = hblk.no;

        auto hCmp = defOp.find(hblk.value->id);
        if (hCmp == defOp.end() || hCmp->second->op != Op::Binary || hCmp->second->data != "<") continue;
        auto jls = loadSlot.find(hCmp->second->inputs[0].id);
        if (jls == loadSlot.end()) continue;
        const unsigned iterSlot = jls->second;
        if (iterSlot >= fn.slots.size() || fn.slots[iterSlot] != Type::Int ||
            (iterSlot < fn.slotByRef.size() && fn.slotByRef[iterSlot]))
            continue;
        const Value jUpper = hCmp->second->inputs[1];
        if (defBlock.count(jUpper.id) && (lp.body.contains(defBlock[jUpper.id]) || !dominates(defBlock[jUpper.id], pre)))
            continue;

        std::optional<Value> jLower;
        for (const auto& op : fn.blocks[pre].ops) {
            if (op.op == Op::Store && op.slot == iterSlot && !op.inputs.empty()) jLower = op.inputs[0];
        }
        if (!jLower || (defBlock.count(jLower->id) &&
                        (lp.body.contains(defBlock[jLower->id]) || !dominates(defBlock[jLower->id], pre))))
            continue;

        int backPreds = 0;
        unsigned latch = 0;
        for (unsigned p : cfg.preds[hdr]) {
            if (lp.body.contains(p)) { ++backPreds; latch = p; }
        }
        if (backPreds != 1 || latch == hdr || fn.blocks[latch].exit != Exit::Jump || fn.blocks[latch].yes != hdr)
            continue;

        const auto& cblk = fn.blocks[cmpBlk];
        if (cblk.exit != Exit::Branch || !cblk.value || cblk.yes == cblk.no) continue;
        const unsigned tb = cblk.yes, eb = cblk.no;
        if (!lp.body.contains(tb) || !lp.body.contains(eb)) continue;
        if (cfg.preds[tb].size() != 1 || cfg.preds[eb].size() != 1) continue;
        const auto& tblk = fn.blocks[tb];
        const auto& eblk = fn.blocks[eb];
        if (tblk.exit != Exit::Jump || eblk.exit != Exit::Jump ||
            tblk.yes != latch || eblk.yes != latch || !eblk.ops.empty())
            continue;

        auto cmpIt = defOp.find(cblk.value->id);
        if (cmpIt == defOp.end() || cmpIt->second->op != Op::Binary || cmpIt->second->inputs.size() != 2) continue;
        const std::string& cmpData = cmpIt->second->data;
        if (cmpData != "<" && cmpData != ">" && cmpData != "<=" && cmpData != ">=") continue;
        auto dA = defOp.find(cmpIt->second->inputs[0].id);
        auto dB = defOp.find(cmpIt->second->inputs[1].id);
        if (dA == defOp.end() || dB == defOp.end()) continue;
        const Operation* opA = dA->second;
        const Operation* opB = dB->second;
        if (opA->op != Op::Index || opB->op != Op::Index || !opA->boundsProven || !opB->boundsProven ||
            opA->inputs.size() < 2 || opB->inputs.size() < 2 ||
            opA->elemBits != opB->elemBits || opA->elemUnsigned != opB->elemUnsigned)
            continue;
        if (!defBlock.count(opA->result.id) || defBlock[opA->result.id] != cmpBlk ||
            !defBlock.count(opB->result.id) || defBlock[opB->result.id] != cmpBlk)
            continue;

        auto alsA = loadSlot.find(opA->inputs[0].id);
        auto alsB = loadSlot.find(opB->inputs[0].id);
        if (alsA == loadSlot.end() || alsB == loadSlot.end() || alsA->second != alsB->second) continue;
        const unsigned arrSlot = alsA->second;
        if (arrSlot >= fn.slots.size() || fn.slots[arrSlot] != Type::IntArray) continue;

        auto idxSlotA = loadSlot.find(opA->inputs[1].id);
        auto idxSlotB = loadSlot.find(opB->inputs[1].id);
        if (idxSlotA == loadSlot.end() || idxSlotB == loadSlot.end()) continue;
        const Operation* idxIterOp = nullptr;
        const Operation* idxBestOp = nullptr;
        unsigned bestSlot = 0;
        std::string canonicalCmp; // normalized as: a[j] <cmp> a[best]
        if (idxSlotA->second == iterSlot && idxSlotB->second != iterSlot) {
            idxIterOp = opA; idxBestOp = opB; bestSlot = idxSlotB->second;
            canonicalCmp = cmpData;
        } else if (idxSlotB->second == iterSlot && idxSlotA->second != iterSlot) {
            idxIterOp = opB; idxBestOp = opA; bestSlot = idxSlotA->second;
            if (cmpData == "<") canonicalCmp = ">";
            else if (cmpData == ">") canonicalCmp = "<";
            else if (cmpData == "<=") canonicalCmp = ">=";
            else canonicalCmp = "<=";
        } else {
            continue;
        }
        if (bestSlot >= fn.slots.size() || fn.slots[bestSlot] != Type::Int ||
            (bestSlot < fn.slotByRef.size() && fn.slotByRef[bestSlot]))
            continue;

        bool safeCmp = true;
        bool cmpHasAnyStore = false;
        for (const auto& op : cblk.ops) {
            if (!isSafeToHoistPast(op)) { safeCmp = false; break; }
            if (op.op == Op::Store) {
                cmpHasAnyStore = true;
                if (op.slot == arrSlot || op.slot == bestSlot || op.slot == iterSlot) {
                    safeCmp = false; break;
                }
            }
        }
        if (!safeCmp) continue;

        bool tbOnlyBest = !tblk.ops.empty();
        int tbBestStore = 0;
        std::unordered_set<unsigned> tbIterLoads;
        for (const auto& op : tblk.ops) {
            if (op.op == Op::Load && op.slot == iterSlot) {
                tbIterLoads.insert(op.result.id);
            } else if (op.op == Op::Store && op.slot == bestSlot && !op.inputs.empty() &&
                       tbIterLoads.contains(op.inputs[0].id)) {
                ++tbBestStore;
            } else {
                tbOnlyBest = false;
                break;
            }
        }
        if (!tbOnlyBest || tbBestStore != 1) continue;

        bool cleanBody = true;
        int iterStores = 0;
        int bestStores = 0;
        int bestLoads = 0;
        bool otherStores = false;
        bool iterStepOne = false;
        for (unsigned b : lp.body) {
            for (const auto& op : fn.blocks[b].ops) {
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin ||
                    op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                    op.op == Op::IndexSet || op.op == Op::ClearOwner || op.op == Op::ClearField || op.op == Op::ClearIndex ||
                    op.op == Op::FieldSet || op.op == Op::PlaceSet) {
                    cleanBody = false; break;
                }
                if (op.op == Op::Store) {
                    if (op.slot == arrSlot) { cleanBody = false; break; }
                    if (op.slot == iterSlot) {
                        ++iterStores;
                        auto jd = defOp.find(op.inputs[0].id);
                        if (jd == defOp.end() || jd->second->op != Op::Binary || jd->second->data != "rangeIncrement")
                            cleanBody = false;
                        else if (b == latch && jd->second->inputs.size() == 2) {
                            auto jl = loadSlot.find(jd->second->inputs[0].id);
                            auto st = defOp.find(jd->second->inputs[1].id);
                            if (jl != loadSlot.end() && jl->second == iterSlot &&
                                st != defOp.end() && st->second->op == Op::Int && st->second->data == "1") {
                                iterStepOne = true;
                            }
                        }
                    } else if (op.slot == bestSlot) {
                        ++bestStores;
                        if (b != tb) cleanBody = false;
                    } else {
                        otherStores = true;
                    }
                } else if (op.op == Op::Load && op.slot == bestSlot) {
                    ++bestLoads;
                    if (op.result.id != idxBestOp->inputs[1].id) cleanBody = false;
                }
            }
            if (!cleanBody) break;
        }
        if (!cleanBody || iterStores != 1 || bestStores != 1 || bestLoads != 1) continue;

        bool pureTwoPhase = (lp.body.size() == 5 && !cmpHasAnyStore && !otherStores && iterStepOne);
        if (pureTwoPhase) {
            // Ensure hdr and latch have only the standard canonical ops and safe operations.
            for (const auto& op : hblk.ops) {
                if (!isSafeToHoistPast(op)) { pureTwoPhase = false; break; }
            }
            for (const auto& op : fn.blocks[latch].ops) {
                if (!isSafeToHoistPast(op)) { pureTwoPhase = false; break; }
            }
        }

        usedPreBlocks.insert(pre);
        plans.push_back({pre, hdr, cmpBlk, tb, eb, latch, exitBlk, arrSlot, bestSlot, iterSlot, *jLower, jUpper,
                         hCmp->second->span, idxBestOp->result.id, idxIterOp->result, idxIterOp->span, *idxBestOp,
                         canonicalCmp, pureTwoPhase});
    }

    for (const auto& p : plans) {
        if (fn.blocks[p.preBlk].exit != Exit::Jump || fn.blocks[p.preBlk].yes != p.hdrBlk) continue;
        auto addIntSlot = [&]() -> unsigned {
            const unsigned s = static_cast<unsigned>(fn.slots.size());
            fn.slots.push_back(Type::Int);
            if (!fn.slotByRef.empty()) fn.slotByRef.push_back(false);
            if (!fn.slotElemBits.empty()) fn.slotElemBits.push_back(64);
            if (!fn.slotElemUnsigned.empty()) fn.slotElemUnsigned.push_back(false);
            if (!fn.slotAliasGroup.empty()) fn.slotAliasGroup.push_back(-1);
            if (!fn.slotOwned.empty()) fn.slotOwned.push_back(false);
            return s;
        };

        if (p.pureTwoPhase) {
            std::uint8_t narrowW = 64;
            if (p.idxTemplate.elemBits == 8 || p.idxTemplate.elemBits == 16 || p.idxTemplate.elemBits == 32) {
                narrowW = p.idxTemplate.elemBits;
            }
            bool narrowUns = (narrowW < 64) && p.idxTemplate.elemUnsigned;
            if (narrowW < 64) {
                if (auto eit = elemRanges.find(p.arrSlot); eit != elemRanges.end() && eit->second && eit->second->lower >= 0) {
                    narrowUns = true;
                }
            }

            const bool isMin = (p.cmpOp == "<" || p.cmpOp == "<=");
            const bool isStrict = (p.cmpOp == "<" || p.cmpOp == ">");
            if (isMin && isStrict && narrowW == 16 && narrowUns) {
                const Span sp = p.idxIterSpan;
                const unsigned argMinBlk = static_cast<unsigned>(fn.blocks.size());
                fn.blocks.emplace_back();

                const Value cmpPre{Type::Bool, fn.nextValue++};
                fn.blocks[p.preBlk].ops.push_back({Op::Binary, cmpPre, {p.jLower, p.jUpper}, "<", 0, p.hdrSpan});
                fn.blocks[p.preBlk].exit = Exit::Branch;
                fn.blocks[p.preBlk].value = cmpPre;
                fn.blocks[p.preBlk].yes = argMinBlk;
                fn.blocks[p.preBlk].no = p.exitBlk;

                const Value af{Type::IntArray, fn.nextValue++};
                const Value initBest{Type::Int, fn.nextValue++};
                const Value bestJ{Type::Int, fn.nextValue++};
                Operation am{Op::ArgMinU16, bestJ, {af, p.jLower, p.jUpper, initBest}, {}, 0, sp};
                am.elemBits = 16;
                am.elemUnsigned = true;
                fn.blocks[argMinBlk].ops.push_back({Op::Load, af, {}, {}, p.arrSlot, sp});
                fn.blocks[argMinBlk].ops.push_back({Op::Load, initBest, {}, {}, p.bestSlot, sp});
                fn.blocks[argMinBlk].ops.push_back(std::move(am));
                fn.blocks[argMinBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {bestJ}, {}, p.bestSlot, sp});
                fn.blocks[argMinBlk].exit = Exit::Jump;
                fn.blocks[argMinBlk].yes = p.exitBlk;
                continue;
            }

            const unsigned bestValSlot = addIntSlot();
            const unsigned initValSlot = addIntSlot();
            const unsigned scanIterSlot = addIntSlot();
            const Span sp = p.idxTemplate.span;

            const unsigned initBlk = static_cast<unsigned>(fn.blocks.size());
            const unsigned minHdrBlk = initBlk + 1;
            const unsigned minBodyBlk = initBlk + 2;
            const unsigned checkBlk = initBlk + 3;
            const unsigned findInitBlk = initBlk + 4;
            const unsigned findBodyBlk = isStrict ? 0u : (initBlk + 5);
            const unsigned hitBlk = isStrict ? 0u : (initBlk + 6);
            const unsigned findStepBlk = isStrict ? 0u : (initBlk + 7);
            fn.blocks.resize(isStrict ? (findInitBlk + 1) : (findStepBlk + 1));

            // preBlk: if jLower < jUpper -> initBlk else exitBlk
            const Value cmpPre{Type::Bool, fn.nextValue++};
            fn.blocks[p.preBlk].ops.push_back({Op::Binary, cmpPre, {p.jLower, p.jUpper}, "<", 0, p.hdrSpan});
            fn.blocks[p.preBlk].exit = Exit::Branch;
            fn.blocks[p.preBlk].value = cmpPre;
            fn.blocks[p.preBlk].yes = initBlk;
            fn.blocks[p.preBlk].no = p.exitBlk;

            // initBlk: initVal = a[bestSlot]; bestValSlot = initVal; initValSlot = initVal; scanIterSlot = jLower -> minHdrBlk
            {
                const Value a0{Type::IntArray, fn.nextValue++};
                const Value k0{Type::Int, fn.nextValue++};
                const Value initV{Type::Int, fn.nextValue++};
                Operation idx0 = p.idxTemplate;
                idx0.result = initV;
                idx0.inputs = {a0, k0};
                idx0.boundsProven = true;
                fn.blocks[initBlk].ops.push_back({Op::Load, a0, {}, {}, p.arrSlot, sp});
                fn.blocks[initBlk].ops.push_back({Op::Load, k0, {}, {}, p.bestSlot, sp});
                fn.blocks[initBlk].ops.push_back(std::move(idx0));
                fn.blocks[initBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {initV}, {}, bestValSlot, sp});
                fn.blocks[initBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {initV}, {}, initValSlot, sp});
                fn.blocks[initBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {p.jLower}, {}, scanIterSlot, p.hdrSpan});
                fn.blocks[initBlk].exit = Exit::Jump;
                fn.blocks[initBlk].yes = minHdrBlk;
            }

            // minHdrBlk: j1 < jUpper -> minBodyBlk else checkBlk
            {
                const Value j1{Type::Int, fn.nextValue++};
                const Value c1{Type::Bool, fn.nextValue++};
                fn.blocks[minHdrBlk].ops.push_back({Op::Load, j1, {}, {}, scanIterSlot, p.hdrSpan});
                fn.blocks[minHdrBlk].ops.push_back({Op::Binary, c1, {j1, p.jUpper}, "<", 0, p.hdrSpan});
                fn.blocks[minHdrBlk].exit = Exit::Branch;
                fn.blocks[minHdrBlk].value = c1;
                fn.blocks[minHdrBlk].yes = minBodyBlk;
                fn.blocks[minHdrBlk].no = checkBlk;
            }

            // minBodyBlk: bestVal = min/max(bestVal, a[j1]) with narrowW; j1 += 1 -> minHdrBlk (vectorizable!)
            {
                const Value a1{Type::IntArray, fn.nextValue++};
                const Value j1{Type::Int, fn.nextValue++};
                const Value ej{Type::Int, fn.nextValue++};
                const Value bv{Type::Int, fn.nextValue++};
                const Value nxt{Type::Int, fn.nextValue++};
                const Value one{Type::Int, fn.nextValue++};
                const Value jn{Type::Int, fn.nextValue++};
                Operation idxJ = p.idxTemplate;
                idxJ.result = ej;
                idxJ.inputs = {a1, j1};
                idxJ.boundsProven = true;
                Operation mm{isMin ? Op::Min : Op::Max, nxt, {bv, ej}, {}, 0, p.idxIterSpan};
                mm.width = narrowW;
                mm.elemUnsigned = narrowUns;
                Operation inc{Op::Binary, jn, {j1, one}, "rangeIncrement", 0, p.hdrSpan};
                inc.overflowProven = true;
                fn.blocks[minBodyBlk].ops.push_back({Op::Load, a1, {}, {}, p.arrSlot, p.idxIterSpan});
                fn.blocks[minBodyBlk].ops.push_back({Op::Load, j1, {}, {}, scanIterSlot, p.idxIterSpan});
                fn.blocks[minBodyBlk].ops.push_back(std::move(idxJ));
                fn.blocks[minBodyBlk].ops.push_back({Op::Load, bv, {}, {}, bestValSlot, p.idxIterSpan});
                fn.blocks[minBodyBlk].ops.push_back(std::move(mm));
                fn.blocks[minBodyBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {nxt}, {}, bestValSlot, p.idxIterSpan});
                fn.blocks[minBodyBlk].ops.push_back({Op::Int, one, {}, "1", 0, p.hdrSpan});
                fn.blocks[minBodyBlk].ops.push_back(std::move(inc));
                fn.blocks[minBodyBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jn}, {}, scanIterSlot, p.hdrSpan});
                fn.blocks[minBodyBlk].exit = Exit::Jump;
                fn.blocks[minBodyBlk].yes = minHdrBlk;
                fn.blocks[minBodyBlk].loopHint = LoopHint::Vectorize;
                fn.blocks[minBodyBlk].noInterleave = true;
            }

            // checkBlk: strict checks if bestVal != initVal else exitBlk; non-strict always scans backward
            if (isStrict) {
                const Value bv{Type::Int, fn.nextValue++};
                const Value iv{Type::Int, fn.nextValue++};
                const Value chg{Type::Bool, fn.nextValue++};
                Operation ne{Op::Binary, chg, {bv, iv}, "!=", 0, p.hdrSpan};
                ne.width = narrowW;
                fn.blocks[checkBlk].ops.push_back({Op::Load, bv, {}, {}, bestValSlot, p.hdrSpan});
                fn.blocks[checkBlk].ops.push_back({Op::Load, iv, {}, {}, initValSlot, p.hdrSpan});
                fn.blocks[checkBlk].ops.push_back(std::move(ne));
                fn.blocks[checkBlk].exit = Exit::Branch;
                fn.blocks[checkBlk].value = chg;
                fn.blocks[checkBlk].yes = findInitBlk;
                fn.blocks[checkBlk].no = p.exitBlk;

                // SIMD 512-bit FindFirst for strict first occurrence of bestVal in [jLower, jUpper)
                const Value af{Type::IntArray, fn.nextValue++};
                const Value bv2{Type::Int, fn.nextValue++};
                const Value jFound{Type::Int, fn.nextValue++};
                Operation ff{Op::FindFirst, jFound, {af, p.jLower, p.jUpper, bv2}, {}, 0, p.idxIterSpan};
                ff.elemBits = narrowW;
                ff.elemUnsigned = narrowUns;
                fn.blocks[findInitBlk].ops.push_back({Op::Load, af, {}, {}, p.arrSlot, p.idxIterSpan});
                fn.blocks[findInitBlk].ops.push_back({Op::Load, bv2, {}, {}, bestValSlot, p.idxIterSpan});
                fn.blocks[findInitBlk].ops.push_back(std::move(ff));
                fn.blocks[findInitBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jFound}, {}, p.bestSlot, p.idxIterSpan});
                fn.blocks[findInitBlk].exit = Exit::Jump;
                fn.blocks[findInitBlk].yes = p.exitBlk;
                continue;
            } else {
                fn.blocks[checkBlk].exit = Exit::Jump;
                fn.blocks[checkBlk].yes = findInitBlk;
            }

            // findInitBlk: non-strict sets scanIterSlot = jUpper - 1 (overflowProven since jLower < jUpper) -> findBodyBlk.
            {
                const Value one{Type::Int, fn.nextValue++};
                const Value jLast{Type::Int, fn.nextValue++};
                Operation sub1{Op::Binary, jLast, {p.jUpper, one}, "-", 0, p.hdrSpan};
                sub1.overflowProven = true;
                fn.blocks[findInitBlk].ops.push_back({Op::Int, one, {}, "1", 0, p.hdrSpan});
                fn.blocks[findInitBlk].ops.push_back(std::move(sub1));
                fn.blocks[findInitBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jLast}, {}, scanIterSlot, p.hdrSpan});
            }
            fn.blocks[findInitBlk].exit = Exit::Jump;
            fn.blocks[findInitBlk].yes = findBodyBlk;

            // findBodyBlk: if a[j] == bestVal -> hitBlk else findStepBlk
            {
                const Value af{Type::IntArray, fn.nextValue++};
                const Value jf{Type::Int, fn.nextValue++};
                const Value ej{Type::Int, fn.nextValue++};
                const Value bv{Type::Int, fn.nextValue++};
                const Value eq{Type::Bool, fn.nextValue++};
                Operation idxF = p.idxTemplate;
                idxF.result = ej;
                idxF.inputs = {af, jf};
                idxF.boundsProven = true;
                Operation eqOp{Op::Binary, eq, {ej, bv}, "==", 0, p.idxIterSpan};
                eqOp.width = narrowW;
                fn.blocks[findBodyBlk].ops.push_back({Op::Load, af, {}, {}, p.arrSlot, p.idxIterSpan});
                fn.blocks[findBodyBlk].ops.push_back({Op::Load, jf, {}, {}, scanIterSlot, p.idxIterSpan});
                fn.blocks[findBodyBlk].ops.push_back(std::move(idxF));
                fn.blocks[findBodyBlk].ops.push_back({Op::Load, bv, {}, {}, bestValSlot, p.idxIterSpan});
                fn.blocks[findBodyBlk].ops.push_back(std::move(eqOp));
                fn.blocks[findBodyBlk].exit = Exit::Branch;
                fn.blocks[findBodyBlk].value = eq;
                fn.blocks[findBodyBlk].yes = hitBlk;
                fn.blocks[findBodyBlk].no = findStepBlk;
            }

            // hitBlk: bestSlot = j -> exitBlk
            {
                const Value jHit{Type::Int, fn.nextValue++};
                fn.blocks[hitBlk].ops.push_back({Op::Load, jHit, {}, {}, scanIterSlot, p.idxIterSpan});
                fn.blocks[hitBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jHit}, {}, p.bestSlot, p.idxIterSpan});
                fn.blocks[hitBlk].exit = Exit::Jump;
                fn.blocks[hitBlk].yes = p.exitBlk;
            }

            // findStepBlk: advance and check loop bound cleanly (guaranteed overflow-free)
            {
                const Value jCur{Type::Int, fn.nextValue++};
                const Value one{Type::Int, fn.nextValue++};
                fn.blocks[findStepBlk].ops.push_back({Op::Load, jCur, {}, {}, scanIterSlot, p.hdrSpan});
                fn.blocks[findStepBlk].ops.push_back({Op::Int, one, {}, "1", 0, p.hdrSpan});
                const unsigned decBlk = static_cast<unsigned>(fn.blocks.size());
                fn.blocks.emplace_back();
                const Value hasPrev{Type::Bool, fn.nextValue++};
                fn.blocks[findStepBlk].ops.push_back({Op::Binary, hasPrev, {jCur, p.jLower}, ">", 0, p.hdrSpan});
                fn.blocks[findStepBlk].exit = Exit::Branch;
                fn.blocks[findStepBlk].value = hasPrev;
                fn.blocks[findStepBlk].yes = decBlk;
                fn.blocks[findStepBlk].no = p.exitBlk;

                const Value jPrev{Type::Int, fn.nextValue++};
                Operation dec{Op::Binary, jPrev, {jCur, one}, "-", 0, p.hdrSpan};
                dec.overflowProven = true;
                fn.blocks[decBlk].ops.push_back(std::move(dec));
                fn.blocks[decBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jPrev}, {}, scanIterSlot, p.hdrSpan});
                fn.blocks[decBlk].exit = Exit::Jump;
                fn.blocks[decBlk].yes = findBodyBlk;
            }
            continue;
        }

        const unsigned bestValSlot = addIntSlot();

        const unsigned loadBlk = static_cast<unsigned>(fn.blocks.size());
        fn.blocks.emplace_back();

        const Value cmp{Type::Bool, fn.nextValue++};
        fn.blocks[p.preBlk].ops.push_back({Op::Binary, cmp, {p.jLower, p.jUpper}, "<", 0, p.hdrSpan});
        fn.blocks[p.preBlk].exit = Exit::Branch;
        fn.blocks[p.preBlk].value = cmp;
        fn.blocks[p.preBlk].yes = loadBlk;
        fn.blocks[p.preBlk].no = p.exitBlk;

        const Value a0{Type::IntArray, fn.nextValue++};
        const Value i0{Type::Int, fn.nextValue++};
        const Value elemV{Type::Int, fn.nextValue++};
        Operation idxOp = p.idxTemplate;
        idxOp.result = elemV;
        idxOp.inputs = {a0, i0};
        idxOp.boundsProven = true;
        fn.blocks[loadBlk].ops.push_back({Op::Load, a0, {}, {}, p.arrSlot, p.idxTemplate.span});
        fn.blocks[loadBlk].ops.push_back({Op::Load, i0, {}, {}, p.bestSlot, p.idxTemplate.span});
        fn.blocks[loadBlk].ops.push_back(std::move(idxOp));
        fn.blocks[loadBlk].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {elemV}, {}, bestValSlot, p.idxTemplate.span});
        fn.blocks[loadBlk].exit = Exit::Jump;
        fn.blocks[loadBlk].yes = p.hdrBlk;

        for (auto& op : fn.blocks[p.cmpBlk].ops) {
            if (op.result.id == p.idxBestId) {
                op = Operation{Op::Load, op.result, {}, {}, bestValSlot, op.span};
                break;
            }
        }
        fn.blocks[p.tbBlk].ops.push_back(
            {Op::Store, {Type::Void, fn.nextValue++}, {p.idxIterVal}, {}, bestValSlot, p.idxIterSpan});
    }
}

/// Merge chains of unconditional single-predecessor jumps (preserving loop hints).
inline void mergeSinglePredJumps(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    for (;;) {
        const std::size_t n = fn.blocks.size();
        std::vector<bool> reach(n, false);
        std::vector<unsigned> q{0};
        reach[0] = true;
        for (std::size_t qi = 0; qi < q.size(); ++qi) {
            const auto& b = fn.blocks[q[qi]];
            auto push = [&](unsigned s) {
                if (s < n && !reach[s]) { reach[s] = true; q.push_back(s); }
            };
            if (b.exit == Exit::Jump) push(b.yes);
            else if (b.exit == Exit::Branch) { push(b.yes); push(b.no); }
            else if (b.exit == Exit::Switch) {
                push(b.yes);
                for (unsigned t : b.targets) push(t);
            }
        }
        std::vector<unsigned> reachPreds(n, 0);
        std::vector<unsigned> onlyPred(n, 0);
        for (unsigned bi = 0; bi < n; ++bi) {
            if (!reach[bi]) continue;
            const auto& b = fn.blocks[bi];
            auto addEdge = [&](unsigned s) {
                if (s < n) { ++reachPreds[s]; onlyPred[s] = bi; }
            };
            if (b.exit == Exit::Jump) addEdge(b.yes);
            else if (b.exit == Exit::Branch) { addEdge(b.yes); addEdge(b.no); }
            else if (b.exit == Exit::Switch) {
                addEdge(b.yes);
                for (unsigned t : b.targets) addEdge(t);
            }
        }
        bool merged = false;
        for (unsigned u = 0; u < n; ++u) {
            if (!reach[u] || fn.blocks[u].exit != Exit::Jump) continue;
            const unsigned v = fn.blocks[u].yes;
            if (v == 0 || v == u || v >= n || reachPreds[v] != 1 || onlyPred[v] != u) continue;
            if (fn.blocks[v].exit == Exit::Open) continue;
            auto& ub = fn.blocks[u];
            auto& vb = fn.blocks[v];
            ub.ops.insert(ub.ops.end(), std::make_move_iterator(vb.ops.begin()), std::make_move_iterator(vb.ops.end()));
            vb.ops.clear();
            ub.exit = vb.exit;
            ub.value = vb.value;
            ub.yes = vb.yes;
            ub.no = vb.no;
            ub.targets = std::move(vb.targets);
            ub.loopHint = vb.loopHint;
            ub.loopAccessGroup = vb.loopAccessGroup;
            ub.noInterleave = ub.noInterleave || vb.noInterleave;
            ub.coldBranchYes = vb.coldBranchYes;
            ub.unrollCount = vb.unrollCount;
            vb.exit = Exit::Unreachable;
            vb.value.reset();
            merged = true;
            break;
        }
        if (!merged) break;
    }
}

inline void ifConvertUnpredictableDiamonds(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> loadSlot;
    auto rebuildMaps = [&]() {
        defOp.clear();
        loadSlot.clear();
        for (const auto& b : fn.blocks) {
            for (const auto& op : b.ops) {
                if (op.result.type != Type::Void) defOp[op.result.id] = &op;
                if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
            }
        }
    };

    auto isImpureOrTrapping = [](const Operation& op) {
        if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin ||
            op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
            op.op == Op::Print || op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice ||
            op.op == Op::FieldSet || op.op == Op::PlaceSet || op.op == Op::RepeatHeap ||
            op.op == Op::RepeatRegion || op.op == Op::Clone || op.op == Op::ClearOwner ||
            op.op == Op::ClearField || op.op == Op::ClearIndex ||
            op.op == Op::TileRepeat || op.op == Op::FindFirst ||
            op.op == Op::GemmF32 || op.op == Op::MatMulMod ||
            op.op == Op::FloydStepU16 || op.op == Op::ArgMinU16 ||
            (op.op == Op::Assert && !op.boundsProven && !op.unchecked)) {
            return true;
        }
        if ((op.op == Op::Negate || op.op == Op::AbsDiff) && op.result.type == Type::Int && !op.overflowProven) {
            return true;
        }
        if (op.op == Op::Binary && op.inputs[0].type == Type::Int) {
            if (op.data == "/" || op.data == "%" || op.data == "<<" || op.data == ">>" || op.data == ">>>") {
                return true;
            }
            if ((op.data == "+" || op.data == "-" || op.data == "*") && !op.overflowProven) {
                return true;
            }
        }
        return false;
    };

    auto isPureScalarOp = [&](const Operation& op) {
        if (isImpureOrTrapping(op)) return false;
        switch (op.op) {
        case Op::Int:
        case Op::Float:
        case Op::Bool:
        case Op::Load:
        case Op::Store:
        case Op::Negate:
        case Op::AbsDiff:
        case Op::Not:
        case Op::BitNot:
        case Op::Min:
        case Op::Max:
        case Op::Select:
        case Op::Assert:
            return true;
        case Op::Binary:
            return op.inputs.size() >= 2 && op.inputs[0].type != Type::Str && op.data != "*";
        default:
            return false;
        }
    };

    auto parseBaseOff = [&](Value iv) -> std::optional<std::pair<unsigned, std::int64_t>> {
        if (auto ls = loadSlot.find(iv.id); ls != loadSlot.end()) {
            return std::make_pair(ls->second, std::int64_t{0});
        }
        auto dit = defOp.find(iv.id);
        if (dit == defOp.end() || dit->second->op != Op::Binary || dit->second->inputs.size() != 2) return std::nullopt;
        const auto* bop = dit->second;
        if (bop->data == "+") {
            if (auto ls = loadSlot.find(bop->inputs[0].id); ls != loadSlot.end()) {
                if (auto cd = defOp.find(bop->inputs[1].id); cd != defOp.end() && cd->second->op == Op::Int)
                    if (auto c = parseConstInt(cd->second->data)) return std::make_pair(ls->second, *c);
            }
            if (auto ls = loadSlot.find(bop->inputs[1].id); ls != loadSlot.end()) {
                if (auto cd = defOp.find(bop->inputs[0].id); cd != defOp.end() && cd->second->op == Op::Int)
                    if (auto c = parseConstInt(cd->second->data)) return std::make_pair(ls->second, *c);
            }
        } else if (bop->data == "-") {
            if (auto ls = loadSlot.find(bop->inputs[0].id); ls != loadSlot.end()) {
                if (auto cd = defOp.find(bop->inputs[1].id); cd != defOp.end() && cd->second->op == Op::Int)
                    if (auto c = parseConstInt(cd->second->data)) return std::make_pair(ls->second, -(*c));
            }
        }
        return std::nullopt;
    };

    auto tryConvertCompareSwap = [&](unsigned bi, unsigned tb) -> bool {
        auto& blk = fn.blocks[bi];
        const auto& tblk = fn.blocks[tb];
        if (tblk.ops.size() < 3 || tblk.ops.size() > 24) return false;
        auto condIt = defOp.find(blk.value->id);
        if (condIt == defOp.end() || condIt->second->op != Op::Binary || condIt->second->inputs.size() != 2)
            return false;
        const std::string cmpStr = condIt->second->data;
        if (cmpStr != ">" && cmpStr != "<" && cmpStr != ">=" && cmpStr != "<=") return false;
        const Value valA = condIt->second->inputs[0];
        const Value valB = condIt->second->inputs[1];
        if (valA.type != Type::Int || valB.type != Type::Int) return false;

        int posA = -1, posB = -1;
        for (int oi = 0; oi < static_cast<int>(blk.ops.size()); ++oi) {
            if (blk.ops[oi].result.id == valA.id) posA = oi;
            if (blk.ops[oi].result.id == valB.id) posB = oi;
        }
        if (posA < 0 || posB < 0 || posA == posB) return false;
        const Operation& opA = blk.ops[posA];
        const Operation& opB = blk.ops[posB];
        if (opA.op != Op::Index || opB.op != Op::Index || !opA.boundsProven || !opB.boundsProven) return false;
        if (opA.elemBits != opB.elemBits || opA.elemUnsigned != opB.elemUnsigned) return false;
        auto arrLsA = loadSlot.find(opA.inputs[0].id);
        auto arrLsB = loadSlot.find(opB.inputs[0].id);
        if (arrLsA == loadSlot.end() || arrLsB == loadSlot.end() || arrLsA->second != arrLsB->second) return false;
        const unsigned arrSlot = arrLsA->second;
        auto boA = parseBaseOff(opA.inputs[1]);
        auto boB = parseBaseOff(opB.inputs[1]);
        if (!boA || !boB || boA->first != boB->first || boA->second == boB->second) return false;
        const unsigned baseSlot = boA->first;
        const std::int64_t offA = boA->second;
        const std::int64_t offB = boB->second;

        for (int oi = std::min(posA, posB) + 1; oi < static_cast<int>(blk.ops.size()); ++oi) {
            const auto& bo = blk.ops[oi];
            if (bo.op == Op::Store && (bo.slot == arrSlot || bo.slot == baseSlot)) return false;
            if (bo.op == Op::IndexSet || isImpureOrTrapping(bo)) return false;
        }

        auto matchIdxTag = [&](Value vArr, Value vIdx) -> int {
            auto al = loadSlot.find(vArr.id);
            if (al == loadSlot.end() || al->second != arrSlot) return -1;
            auto bo = parseBaseOff(vIdx);
            if (!bo || bo->first != baseSlot) return -1;
            if (bo->second == offA) return 0;
            if (bo->second == offB) return 1;
            return -1;
        };

        auto slotUsedOutside = [&](unsigned s) {
            for (unsigned bj = 0; bj < fn.blocks.size(); ++bj) {
                if (bj == tb) continue;
                for (const auto& op : fn.blocks[bj].ops) {
                    if ((op.op == Op::Load || op.op == Op::Store) && op.slot == s) return true;
                }
            }
            return false;
        };

        std::unordered_map<unsigned, int> valTag;
        std::unordered_map<unsigned, int> tbDefPos;
        std::optional<unsigned> tmpSlot;
        int tmpTag = -1;
        bool seenIdxSet = false;
        int setMask = 0;
        std::vector<int> scalarStoreIdx;
        std::unordered_set<unsigned> writtenScalars;

        for (int oi = 0; oi < static_cast<int>(tblk.ops.size()); ++oi) {
            const auto& op = tblk.ops[oi];
            if (op.result.type != Type::Void) tbDefPos[op.result.id] = oi;
            if (isImpureOrTrapping(op)) return false;
            if (op.op == Op::Index) {
                if (seenIdxSet || !op.boundsProven) return false;
                const int t = matchIdxTag(op.inputs[0], op.inputs[1]);
                if (t < 0) return false;
                valTag[op.result.id] = t;
            } else if (op.op == Op::IndexSet) {
                if (!op.boundsProven || op.inputs.size() < 3) return false;
                seenIdxSet = true;
                const int target = matchIdxTag(op.inputs[0], op.inputs[1]);
                auto vt = valTag.find(op.inputs[2].id);
                if (target < 0 || vt == valTag.end() || vt->second != (1 - target)) return false;
                if (setMask & (1 << target)) return false;
                setMask |= (1 << target);
            } else if (op.op == Op::Store) {
                if (op.slot == arrSlot || op.slot == baseSlot || op.inputs.empty()) return false;
                if (auto vt = valTag.find(op.inputs[0].id); vt != valTag.end()) {
                    if (seenIdxSet || tmpSlot.has_value() || writtenScalars.contains(op.slot)) return false;
                    if (slotUsedOutside(op.slot)) return false;
                    tmpSlot = op.slot;
                    tmpTag = vt->second;
                } else {
                    if ((tmpSlot && op.slot == *tmpSlot) || writtenScalars.contains(op.slot)) return false;
                    writtenScalars.insert(op.slot);
                    scalarStoreIdx.push_back(oi);
                }
            } else if (op.op == Op::Load) {
                if (writtenScalars.contains(op.slot)) return false;
                if (tmpSlot && op.slot == *tmpSlot) {
                    valTag[op.result.id] = tmpTag;
                }
            }
        }
        if (setMask != 3) return false;

        std::vector<bool> needed(tblk.ops.size(), false);
        std::vector<int> work;
        for (int si : scalarStoreIdx) {
            auto it = tbDefPos.find(tblk.ops[si].inputs[0].id);
            if (it != tbDefPos.end() && !needed[it->second]) {
                needed[it->second] = true;
                work.push_back(it->second);
            }
        }
        while (!work.empty()) {
            const int wi = work.back();
            work.pop_back();
            const auto& op = tblk.ops[wi];
            if (op.op == Op::Index || op.op == Op::IndexSet || op.op == Op::Store) return false;
            if (op.op == Op::Load && tmpSlot && op.slot == *tmpSlot) return false;
            for (const Value& in : op.inputs) {
                if (auto it = tbDefPos.find(in.id); it != tbDefPos.end() && !needed[it->second]) {
                    needed[it->second] = true;
                    work.push_back(it->second);
                }
            }
        }

        const Value condV = *blk.value;
        const unsigned joinB = tblk.yes;
        const Value arrLoadA = opA.inputs[0], idxValA = opA.inputs[1];
        const Value arrLoadB = opB.inputs[0], idxValB = opB.inputs[1];
        const std::uint8_t ebBits = opA.elemBits;
        const bool ebUnsigned = opA.elemUnsigned;
        const Span casSpan = condIt->second->span;
        const auto tbOps = tblk.ops;

        std::unordered_map<unsigned, Value> remap;
        for (std::size_t oi = 0; oi < tbOps.size(); ++oi) {
            if (!needed[oi]) continue;
            Operation nop = tbOps[oi];
            for (Value& in : nop.inputs) {
                if (auto rit = remap.find(in.id); rit != remap.end()) in = rit->second;
            }
            if (nop.result.type != Type::Void) {
                const unsigned oldId = nop.result.id;
                nop.result.id = fn.nextValue++;
                remap[oldId] = nop.result;
            }
            blk.ops.push_back(std::move(nop));
        }

        const std::uint8_t minMaxW = (ebBits <= 16) ? 32 : ((ebBits == 32 && !ebUnsigned) ? 32 : 64);
        const Value loV{Type::Int, fn.nextValue++};
        const Value hiV{Type::Int, fn.nextValue++};
        Operation minOp{Op::Min, loV, {valA, valB}, {}, 0, casSpan};
        minOp.overflowProven = true;
        minOp.width = minMaxW;
        Operation maxOp{Op::Max, hiV, {valA, valB}, {}, 0, casSpan};
        maxOp.overflowProven = true;
        maxOp.width = minMaxW;
        blk.ops.push_back(std::move(minOp));
        blk.ops.push_back(std::move(maxOp));

        const bool wantAsc = (cmpStr == ">" || cmpStr == ">=");
        const Value newA = wantAsc ? loV : hiV;
        const Value newB = wantAsc ? hiV : loV;
        Operation setA{Op::IndexSet, {Type::Void, fn.nextValue++}, {arrLoadA, idxValA, newA}, {}, 0, casSpan};
        setA.boundsProven = true;
        setA.elemBits = ebBits;
        setA.elemUnsigned = ebUnsigned;
        Operation setB{Op::IndexSet, {Type::Void, fn.nextValue++}, {arrLoadB, idxValB, newB}, {}, 0, casSpan};
        setB.boundsProven = true;
        setB.elemBits = ebBits;
        setB.elemUnsigned = ebUnsigned;
        blk.ops.push_back(std::move(setA));
        blk.ops.push_back(std::move(setB));

        for (int si : scalarStoreIdx) {
            const auto& st = tbOps[si];
            Value rhs = st.inputs[0];
            if (auto rit = remap.find(rhs.id); rit != remap.end()) rhs = rit->second;
            const Value oldV{fn.slots[st.slot], fn.nextValue++};
            const Value selV{fn.slots[st.slot], fn.nextValue++};
            blk.ops.push_back({Op::Load, oldV, {}, {}, st.slot, st.span});
            blk.ops.push_back({Op::Select, selV, {condV, rhs, oldV}, {}, 0, st.span});
            blk.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {selV}, {}, st.slot, st.span});
        }

        fn.blocks[tb].ops.clear();
        blk.exit = Exit::Jump;
        blk.yes = joinB;
        blk.no = 0;
        blk.value.reset();
        return true;
    };

    auto tryOnePass = [&]() -> bool {
        const detail::CFGInfo cfg = detail::buildCFG(fn);
        rebuildMaps();
        bool passChanged = false;

        auto slotUsedOutsideBlock = [&](unsigned slot, unsigned onlyBlk) {
            if (slot < fn.source.params.size()) return true;
            for (unsigned bj = 0; bj < fn.blocks.size(); ++bj) {
                if (bj == onlyBlk) continue;
                for (const auto& op : fn.blocks[bj].ops) {
                    if ((op.op == Op::Load || op.op == Op::Store || op.op == Op::Drop ||
                         op.op == Op::ClearOwner || op.op == Op::ClearField) && op.slot == slot) {
                        return true;
                    }
                }
            }
            return false;
        };

        auto validateScalarStores = [&](const std::vector<Operation>& ops,
                                        unsigned blkIdx,
                                        std::unordered_set<unsigned>& localSlots,
                                        std::vector<unsigned>& storeOrder,
                                        std::unordered_map<unsigned, Value>& storedVal) -> bool {
            std::unordered_set<unsigned> storedHere;
            for (const auto& op : ops) {
                if (!isPureScalarOp(op)) return false;
                if (op.op == Op::Load && storedHere.contains(op.slot)) return false;
                if (op.op == Op::Store && !op.inputs.empty()) {
                    if (!slotUsedOutsideBlock(op.slot, blkIdx)) {
                        localSlots.insert(op.slot);
                    } else {
                        if (storedHere.contains(op.slot)) return false;
                        storedHere.insert(op.slot);
                        storeOrder.push_back(op.slot);
                        storedVal.emplace(op.slot, op.inputs[0]);
                    }
                }
            }
            return true;
        };

        for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
            auto& blk = fn.blocks[bi];
            if (cfg.loopOfBlock[bi] < 0) continue;
            if (blk.exit != Exit::Branch || !blk.value || blk.yes == blk.no) continue;
            const unsigned tb = blk.yes, eb = blk.no;
            if (tb >= fn.blocks.size() || eb >= fn.blocks.size()) continue;
            if (cfg.preds[tb].size() != 1) continue;
            auto& tblk = fn.blocks[tb];
            auto& eblk = fn.blocks[eb];

            // (A) Pure scalar triangle: blk -> tb -> eb
            if (tblk.exit == Exit::Jump && tblk.yes == eb && !tblk.ops.empty() && tblk.ops.size() <= 10) {
                std::unordered_set<unsigned> tbLocal;
                std::vector<unsigned> tbOrder;
                std::unordered_map<unsigned, Value> tbVal;
                if (validateScalarStores(tblk.ops, tb, tbLocal, tbOrder, tbVal) && !tbOrder.empty()) {
                    const Value condV = *blk.value;
                    const auto tbOps = tblk.ops;
                    for (const auto& op : tbOps) {
                        if (op.op == Op::Store && !op.inputs.empty() && !tbLocal.contains(op.slot)) {
                            const Value oldV{fn.slots[op.slot], fn.nextValue++};
                            const Value selV{fn.slots[op.slot], fn.nextValue++};
                            blk.ops.push_back({Op::Load, oldV, {}, {}, op.slot, op.span});
                            blk.ops.push_back({Op::Select, selV, {condV, op.inputs[0], oldV}, {}, 0, op.span});
                            blk.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {selV}, {}, op.slot, op.span});
                        } else {
                            blk.ops.push_back(op);
                        }
                    }
                    tblk.ops.clear();
                    tblk.exit = Exit::Unreachable;
                    blk.exit = Exit::Jump;
                    blk.yes = eb;
                    blk.no = 0;
                    blk.value.reset();
                    passChanged = true;
                    break;
                }
            }

            if (cfg.preds[eb].size() != 1) continue;
            if (tblk.exit != Exit::Jump || eblk.exit != Exit::Jump || tblk.yes != eblk.yes) continue;

            if (eblk.ops.empty() && tryConvertCompareSwap(bi, tb)) {
                tblk.exit = Exit::Unreachable;
                passChanged = true;
                break;
            }

            auto checkDataExpr = [&](auto& self, Value v, int depth) -> std::pair<bool, bool> {
                if (depth > 6) return {false, false};
                auto dit = defOp.find(v.id);
                if (dit == defOp.end()) return {false, false};
                const Operation* dop = dit->second;
                if (dop->op == Op::Index) return {true, true};
                if (dop->op == Op::Int || dop->op == Op::Bool) return {true, false};
                if ((dop->op == Op::Negate || dop->op == Op::BitNot) && !dop->inputs.empty()) {
                    return self(self, dop->inputs[0], depth + 1);
                }
                if (dop->op == Op::Binary && dop->inputs.size() == 2 &&
                    (dop->data == "+" || dop->data == "-" || dop->data == "*" ||
                     dop->data == "%" || dop->data == "/")) {
                    auto l = self(self, dop->inputs[0], depth + 1);
                    auto r = self(self, dop->inputs[1], depth + 1);
                    return {l.first && r.first, l.second || r.second};
                }
                return {false, false};
            };

            if (!tblk.ops.empty() && !eblk.ops.empty()) {
                // (B) Pure scalar 2-sided diamond: !tb.ops.empty() && !eb.ops.empty()
                bool hasSelect = false;
                for (const auto& op : tblk.ops) if (op.op == Op::Select) { hasSelect = true; break; }
                for (const auto& op : eblk.ops) if (op.op == Op::Select) { hasSelect = true; break; }
                if (!hasSelect && tblk.ops.size() + eblk.ops.size() <= 16) {
                    std::unordered_set<unsigned> tbLocal, ebLocal;
                    std::vector<unsigned> tbOrder, ebOrder;
                    std::unordered_map<unsigned, Value> tbVal, ebVal;
                    if (validateScalarStores(tblk.ops, tb, tbLocal, tbOrder, tbVal) &&
                        validateScalarStores(eblk.ops, eb, ebLocal, ebOrder, ebVal) &&
                        (!tbOrder.empty() || !ebOrder.empty())) {
                        const Value condV = *blk.value;
                        const unsigned joinB = tblk.yes;
                        const auto tbOps = tblk.ops;
                        const auto ebOps = eblk.ops;
                        for (const auto& op : tbOps) {
                            if (op.op == Op::Store && !tbLocal.contains(op.slot)) continue;
                            blk.ops.push_back(op);
                        }
                        for (const auto& op : ebOps) {
                            if (op.op == Op::Store && !ebLocal.contains(op.slot)) continue;
                            blk.ops.push_back(op);
                        }
                        std::vector<unsigned> allSlots = tbOrder;
                        std::unordered_set<unsigned> seenSlots(tbOrder.begin(), tbOrder.end());
                        for (unsigned s : ebOrder) {
                            if (seenSlots.insert(s).second) allSlots.push_back(s);
                        }
                        for (unsigned s : allSlots) {
                            auto itT = tbVal.find(s);
                            auto itE = ebVal.find(s);
                            const Value selV{fn.slots[s], fn.nextValue++};
                            if (itT != tbVal.end() && itE != ebVal.end()) {
                                blk.ops.push_back({Op::Select, selV, {condV, itT->second, itE->second}, {}, 0, blk.ops.back().span});
                            } else if (itT != tbVal.end()) {
                                const Value oldV{fn.slots[s], fn.nextValue++};
                                blk.ops.push_back({Op::Load, oldV, {}, {}, s, itT->second.id ? blk.ops.back().span : Span{}});
                                blk.ops.push_back({Op::Select, selV, {condV, itT->second, oldV}, {}, 0, blk.ops.back().span});
                            } else {
                                const Value oldV{fn.slots[s], fn.nextValue++};
                                blk.ops.push_back({Op::Load, oldV, {}, {}, s, blk.ops.back().span});
                                blk.ops.push_back({Op::Select, selV, {condV, oldV, itE->second}, {}, 0, blk.ops.back().span});
                            }
                            blk.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {selV}, {}, s, blk.ops.back().span});
                        }
                        tblk.ops.clear();
                        tblk.exit = Exit::Unreachable;
                        eblk.ops.clear();
                        eblk.exit = Exit::Unreachable;
                        blk.exit = Exit::Jump;
                        blk.yes = joinB;
                        blk.no = 0;
                        blk.value.reset();
                        passChanged = true;
                        break;
                    }
                }

                // (B2) 2-sided Same-Element IndexSet diamond: pure array-data comparison writing same (arr, idx)
                if (tblk.ops.size() + eblk.ops.size() <= 24) {
                    const Value condV = *blk.value;
                    bool condIsArrayDataCmp = false;
                    if (auto cdit = defOp.find(condV.id);
                        cdit != defOp.end() && cdit->second->op == Op::Binary && cdit->second->inputs.size() == 2 &&
                        (cdit->second->data == "==" || cdit->second->data == "!=" ||
                         cdit->second->data == "<" || cdit->second->data == ">" ||
                         cdit->second->data == "<=" || cdit->second->data == ">=")) {
                        auto l = checkDataExpr(checkDataExpr, cdit->second->inputs[0], 0);
                        auto r = checkDataExpr(checkDataExpr, cdit->second->inputs[1], 0);
                        condIsArrayDataCmp = (l.first && l.second && r.first && r.second);
                    }
                    auto findSingleIndexSet = [&](const std::vector<Operation>& ops, int& outPos,
                                                  unsigned& outArr, unsigned& outIdx) -> bool {
                        outPos = -1;
                        bool seenSet = false;
                        for (int oi = 0; oi < static_cast<int>(ops.size()); ++oi) {
                            const auto& op = ops[oi];
                            if (isImpureOrTrapping(op) || op.op == Op::Store || op.op == Op::Drop ||
                                op.op == Op::RegionMark || op.op == Op::RegionRewind) {
                                return false;
                            }
                            if (op.op == Op::Index) {
                                if (!op.boundsProven || seenSet) return false;
                            } else if (op.op == Op::IndexSet) {
                                if (seenSet || !op.boundsProven || op.inputs.size() < 3 ||
                                    op.inputs[2].type != Type::Int) {
                                    return false;
                                }
                                auto als = loadSlot.find(op.inputs[0].id);
                                auto ils = loadSlot.find(op.inputs[1].id);
                                if (als == loadSlot.end() || ils == loadSlot.end()) return false;
                                outPos = oi;
                                outArr = als->second;
                                outIdx = ils->second;
                                seenSet = true;
                            }
                        }
                        return seenSet;
                    };
                    int posT = -1, posE = -1;
                    unsigned arrT = 0, idxT = 0, arrE = 0, idxE = 0;
                    if (condIsArrayDataCmp &&
                        findSingleIndexSet(tblk.ops, posT, arrT, idxT) &&
                        findSingleIndexSet(eblk.ops, posE, arrE, idxE) &&
                        arrT == arrE && idxT == idxE) {
                        const Operation idxOpT = tblk.ops[posT];
                        const Operation idxOpE = eblk.ops[posE];
                        if (idxOpT.elemBits == idxOpE.elemBits && idxOpT.elemUnsigned == idxOpE.elemUnsigned) {
                            const unsigned joinB = tblk.yes;
                            const auto tbOps = tblk.ops;
                            const auto ebOps = eblk.ops;
                            for (int oi = 0; oi < static_cast<int>(tbOps.size()); ++oi) {
                                if (oi != posT) blk.ops.push_back(tbOps[oi]);
                            }
                            for (int oi = 0; oi < static_cast<int>(ebOps.size()); ++oi) {
                                if (oi != posE) blk.ops.push_back(ebOps[oi]);
                            }
                            const std::uint8_t selWidth = (idxOpT.elemBits <= 16) ? 32
                                : ((idxOpT.elemBits == 32 && !idxOpT.elemUnsigned) ? 32 : 64);
                            const Value selV{Type::Int, fn.nextValue++};
                            Operation selOp{Op::Select, selV, {condV, idxOpT.inputs[2], idxOpE.inputs[2]}, {}, 0, idxOpT.span};
                            selOp.width = selWidth;
                            blk.ops.push_back(std::move(selOp));
                            Operation setOp = idxOpT;
                            setOp.inputs[2] = selV;
                            blk.ops.push_back(std::move(setOp));

                            tblk.ops.clear();
                            tblk.exit = Exit::Unreachable;
                            eblk.ops.clear();
                            eblk.exit = Exit::Unreachable;
                            blk.exit = Exit::Jump;
                            blk.yes = joinB;
                            blk.no = 0;
                            blk.value.reset();
                            passChanged = true;
                            break;
                        }
                    }
                }
                continue;
            }

            if (!eblk.ops.empty() || tblk.ops.empty() || tblk.ops.size() > 14) continue;

            // 1-sided diamond (possibly touching proven-bounds array accesses)
            bool branchComparesOnlyArrayValues = false;
            if (auto cdit = defOp.find(blk.value->id);
                cdit != defOp.end() && cdit->second->op == Op::Binary && cdit->second->inputs.size() == 2 &&
                (cdit->second->data == "<" || cdit->second->data == ">" ||
                 cdit->second->data == "<=" || cdit->second->data == ">=" ||
                 cdit->second->data == "==" || cdit->second->data == "!=")) {
                auto l = checkDataExpr(checkDataExpr, cdit->second->inputs[0], 0);
                auto r = checkDataExpr(checkDataExpr, cdit->second->inputs[1], 0);
                branchComparesOnlyArrayValues = (l.first && r.first && (l.second || r.second));
            }
            bool ok = true;
            std::unordered_set<unsigned> writtenScalars;
            std::vector<std::pair<unsigned, unsigned>> accessedArrPairs;
            int idxSetCnt = 0;
            for (const auto& op : tblk.ops) {
                if (isImpureOrTrapping(op)) { ok = false; break; }
                if (op.op == Op::Index) {
                    if (!op.boundsProven) { ok = false; break; }
                    auto als = loadSlot.find(op.inputs[0].id);
                    auto ils = loadSlot.find(op.inputs[1].id);
                    if (als == loadSlot.end() || ils == loadSlot.end() || writtenScalars.contains(ils->second)) {
                        ok = false; break;
                    }
                    accessedArrPairs.emplace_back(als->second, ils->second);
                } else if (op.op == Op::IndexSet && op.inputs.size() >= 3) {
                    if (!op.boundsProven) { ok = false; break; }
                    auto als = loadSlot.find(op.inputs[0].id);
                    auto ils = loadSlot.find(op.inputs[1].id);
                    if (als == loadSlot.end() || ils == loadSlot.end() ||
                        op.inputs[2].type != Type::Int || writtenScalars.contains(ils->second)) {
                        ok = false; break;
                    }
                    accessedArrPairs.emplace_back(als->second, ils->second);
                    ++idxSetCnt;
                }
                if (op.op == Op::Store) {
                    if (writtenScalars.contains(op.slot)) { ok = false; break; }
                    writtenScalars.insert(op.slot);
                }
            }
            if (!ok || (writtenScalars.empty() && idxSetCnt == 0) || idxSetCnt > 1) continue;

            if (!branchComparesOnlyArrayValues) {
                for (const auto& [as, is] : accessedArrPairs) {
                    bool foundPriorUncondAccess = false;
                    for (const auto& op : blk.ops) {
                        if (op.op == Op::Store && (op.slot == as || op.slot == is)) foundPriorUncondAccess = false;
                        if ((op.op == Op::Index || op.op == Op::IndexSet) && op.boundsProven && op.inputs.size() >= 2) {
                            auto a2 = loadSlot.find(op.inputs[0].id);
                            auto i2 = loadSlot.find(op.inputs[1].id);
                            if (a2 != loadSlot.end() && i2 != loadSlot.end() && a2->second == as && i2->second == is)
                                foundPriorUncondAccess = true;
                        }
                    }
                    if (!foundPriorUncondAccess) { ok = false; break; }
                }
            }
            if (!ok) continue;

            // Hoist pure ops of tb into blk using Op::Select for writes (or identity monoid selection for min/max reductions)
            const Value condV = *blk.value;
            const unsigned joinB = tblk.yes;
            const auto tbOps = tblk.ops; // copy before modifying

            std::unordered_map<unsigned, std::size_t> tbDefIdx;
            std::unordered_map<unsigned, int> tbUseCount;
            for (std::size_t oi = 0; oi < tbOps.size(); ++oi) {
                if (tbOps[oi].result.type != Type::Void) tbDefIdx[tbOps[oi].result.id] = oi;
                for (const Value& in : tbOps[oi].inputs) ++tbUseCount[in.id];
            }
            struct MonoidRewrite {
                std::size_t targetIdx = 0;
                int selfSide = 0;
            };
            std::unordered_map<std::size_t, MonoidRewrite> monoidAtMm;
            std::unordered_set<std::size_t> directWriteIdx;
            for (std::size_t oi = 0; oi < tbOps.size(); ++oi) {
                const auto& op = tbOps[oi];
                Value rhs{};
                bool isStore = false;
                bool isIdxSet = false;
                if (op.op == Op::Store && !op.inputs.empty()) {
                    rhs = op.inputs[0];
                    isStore = true;
                } else if (op.op == Op::IndexSet && op.inputs.size() >= 3) {
                    rhs = op.inputs[2];
                    isIdxSet = true;
                } else {
                    continue;
                }
                auto mIt = tbDefIdx.find(rhs.id);
                if (mIt == tbDefIdx.end()) continue;
                const std::size_t mmIdx = mIt->second;
                const auto& mmOp = tbOps[mmIdx];
                if ((mmOp.op != Op::Max && mmOp.op != Op::Min) || mmOp.inputs.size() != 2 ||
                    mmOp.result.type != Type::Int || monoidAtMm.contains(mmIdx))
                    continue;
                if (tbUseCount[mmOp.result.id] != 1) continue;
                int matchedSide = -1;
                for (int side = 0; side < 2; ++side) {
                    auto sIt = tbDefIdx.find(mmOp.inputs[side].id);
                    if (sIt == tbDefIdx.end()) continue;
                    const auto& selfOp = tbOps[sIt->second];
                    if (isStore && selfOp.op == Op::Load && selfOp.slot == op.slot) {
                        matchedSide = side;
                        break;
                    }
                    if (isIdxSet && selfOp.op == Op::Index && selfOp.boundsProven && selfOp.inputs.size() >= 2) {
                        auto a1 = loadSlot.find(op.inputs[0].id);
                        auto i1 = loadSlot.find(op.inputs[1].id);
                        auto a2 = loadSlot.find(selfOp.inputs[0].id);
                        auto i2 = loadSlot.find(selfOp.inputs[1].id);
                        if (a1 != loadSlot.end() && i1 != loadSlot.end() &&
                            a2 != loadSlot.end() && i2 != loadSlot.end() &&
                            a1->second == a2->second && i1->second == i2->second) {
                            matchedSide = side;
                            break;
                        }
                    }
                }
                if (matchedSide >= 0) {
                    monoidAtMm[mmIdx] = {oi, matchedSide};
                    directWriteIdx.insert(oi);
                }
            }

            for (std::size_t oi = 0; oi < tbOps.size(); ++oi) {
                const auto& op = tbOps[oi];
                if (auto mIt = monoidAtMm.find(oi); mIt != monoidAtMm.end()) {
                    const int selfSide = mIt->second.selfSide;
                    const Value cand = op.inputs[1 - selfSide];
                    std::uint8_t w = op.width;
                    if (w != 8 && w != 16 && w != 32 && w != 64) w = 64;
                    const char* idStr = (op.op == Op::Max)
                        ? (w == 8 ? "-128" : (w == 16 ? "-32768" : (w == 32 ? "-2147483648" : "-9223372036854775808")))
                        : (w == 8 ? "127" : (w == 16 ? "32767" : (w == 32 ? "2147483647" : "9223372036854775807")));
                    const Value idV{Type::Int, fn.nextValue++};
                    const Value candSel{Type::Int, fn.nextValue++};
                    blk.ops.push_back({Op::Int, idV, {}, idStr, 0, op.span});
                    Operation selOp{Op::Select, candSel, {condV, cand, idV}, {}, 0, op.span};
                    selOp.width = w;
                    blk.ops.push_back(std::move(selOp));
                    Operation mmCopy = op;
                    mmCopy.inputs[1 - selfSide] = candSel;
                    mmCopy.width = w;
                    mmCopy.overflowProven = true;
                    blk.ops.push_back(std::move(mmCopy));
                } else if (op.op == Op::Store && !op.inputs.empty()) {
                    if (directWriteIdx.contains(oi)) {
                        blk.ops.push_back(op);
                    } else {
                        const Value oldV{fn.slots[op.slot], fn.nextValue++};
                        const Value selV{fn.slots[op.slot], fn.nextValue++};
                        blk.ops.push_back({Op::Load, oldV, {}, {}, op.slot, op.span});
                        blk.ops.push_back({Op::Select, selV, {condV, op.inputs[0], oldV}, {}, 0, op.span});
                        blk.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {selV}, {}, op.slot, op.span});
                    }
                } else if (op.op == Op::IndexSet && op.inputs.size() >= 3) {
                    if (directWriteIdx.contains(oi)) {
                        blk.ops.push_back(op);
                    } else {
                        const Value oldElem{Type::Int, fn.nextValue++};
                        Operation idxOp{Op::Index, oldElem, {op.inputs[0], op.inputs[1]}, {}, 0, op.span};
                        idxOp.boundsProven = true;
                        idxOp.elemBits = op.elemBits;
                        idxOp.elemUnsigned = op.elemUnsigned;
                        const Value selElem{Type::Int, fn.nextValue++};
                        Operation selOp{Op::Select, selElem, {condV, op.inputs[2], oldElem}, {}, 0, op.span};
                        selOp.width = (op.elemBits == 8 || op.elemBits == 16 || op.elemBits == 32) ? op.elemBits : 64;
                        Operation setOp = op;
                        setOp.inputs[2] = selElem;
                        blk.ops.push_back(idxOp);
                        blk.ops.push_back(std::move(selOp));
                        blk.ops.push_back(setOp);
                    }
                } else {
                    blk.ops.push_back(op);
                }
            }
            tblk.ops.clear();
            tblk.exit = Exit::Unreachable;
            blk.exit = Exit::Jump;
            blk.yes = joinB;
            blk.no = 0;
            blk.value.reset();
            passChanged = true;
            break;
        }
        return passChanged;
    };

    for (int pass = 0; pass < 4; ++pass) {
        bool any = false;
        while (tryOnePass()) any = true;
        if (!any) break;
        mergeSinglePredJumps(fn);
    }
}

/// Hint rare equality-guarded heavy blocks (e.g. `if (x == C) { expensiveWork() }`) inside loops
/// as cold so LLVM keeps expensive math/calls out of the hot loop path.
inline void tagColdEqualityBranches(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::unordered_map<unsigned, const Operation*> defOp;
    for (const auto& blk : fn.blocks) {
        for (const auto& op : blk.ops) {
            if (op.result.type != Type::Void) defOp[op.result.id] = &op;
        }
    }
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        if (cfg.loopOfBlock[bi] < 0) continue;
        auto& blk = fn.blocks[bi];
        if (blk.exit != Exit::Branch || !blk.value || blk.yes == blk.no) continue;
        const unsigned tb = blk.yes, eb = blk.no;
        if (tb >= fn.blocks.size() || eb >= fn.blocks.size()) continue;
        if (cfg.preds[tb].size() != 1 || cfg.preds[eb].size() != 1) continue;
        const auto& tblk = fn.blocks[tb];
        const auto& eblk = fn.blocks[eb];
        if (tblk.exit != Exit::Jump || eblk.exit != Exit::Jump || tblk.yes != eblk.yes) continue;
        if (!eblk.ops.empty() || tblk.ops.size() < 10) continue;
        auto cd = defOp.find(blk.value->id);
        if (cd == defOp.end() || cd->second->op != Op::Binary || cd->second->data != "==" ||
            cd->second->inputs.size() != 2 || cd->second->inputs[0].type != Type::Int)
            continue;
        const bool lhsConst = defOp.count(cd->second->inputs[0].id) && defOp.at(cd->second->inputs[0].id)->op == Op::Int;
        const bool rhsConst = defOp.count(cd->second->inputs[1].id) && defOp.at(cd->second->inputs[1].id)->op == Op::Int;
        if (!lhsConst && !rhsConst) continue;
        // Skip 50%-likely parity checks such as `(i % 2) == 1`.
        const unsigned varId = lhsConst ? cd->second->inputs[1].id : cd->second->inputs[0].id;
        if (auto vd = defOp.find(varId); vd != defOp.end() && vd->second->op == Op::Binary &&
            vd->second->data == "%" && vd->second->inputs.size() == 2) {
            auto md = defOp.find(vd->second->inputs[1].id);
            if (md != defOp.end() && md->second->op == Op::Int) {
                if (auto mv = parseConstInt(md->second->data); mv && *mv >= 2 && *mv <= 3) continue;
            }
        }
        bool hasHeavyOp = false;
        for (const auto& op : tblk.ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall || op.op == Op::ClosureCall ||
                (op.op == Op::Binary && (op.data == "*" || op.data == "/" || op.data == "%"))) {
                hasHeavyOp = true;
                break;
            }
        }
        if (hasHeavyOp) blk.coldBranchYes = true;
    }
}

inline void tagVectorizableAndUnrollableLoops(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, unsigned> loadSlot;
    int nextAg = 0;
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        nextAg = std::max(nextAg, fn.blocks[bi].loopAccessGroup + 1);
        for (const auto& op : fn.blocks[bi].ops) {
            nextAg = std::max(nextAg, op.loopAccessGroup + 1);
            if (op.result.type != Type::Void) {
                defOp[op.result.id] = &op;
                defBlock[op.result.id] = bi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
        }
    }

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lp = cfg.loops[li];
        if (lp.preheaders.size() != 1) continue;
        const unsigned hdr = lp.header;
        const unsigned pre = lp.preheaders[0];
        const auto& hblk = fn.blocks[hdr];
        if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
            continue;
        if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != hdr) continue;

        auto cdit = defOp.find(hblk.value->id);
        if (cdit == defOp.end() || cdit->second->op != Op::Binary || cdit->second->data != "<") continue;
        auto jls = loadSlot.find(cdit->second->inputs[0].id);
        if (jls == loadSlot.end()) continue;
        const unsigned iterSlot = jls->second;
        const Value upperVal = cdit->second->inputs[1];
        if (defBlock.count(upperVal.id) && lp.body.contains(defBlock[upperVal.id])) continue;

        std::optional<Value> lowerVal;
        for (const auto& op : fn.blocks[pre].ops)
            if (op.op == Op::Store && op.slot == iterSlot && !op.inputs.empty()) lowerVal = op.inputs[0];
        if (!lowerVal) continue;

        auto loDef = defOp.find(lowerVal->id);
        auto hiDef = defOp.find(upperVal.id);
        std::optional<std::int64_t> cLo = (loDef != defOp.end() && loDef->second->op == Op::Int)
            ? parseConstInt(loDef->second->data) : std::nullopt;
        std::optional<std::int64_t> cHi = (hiDef != defOp.end() && hiDef->second->op == Op::Int)
            ? parseConstInt(hiDef->second->data) : std::nullopt;

        // Constant-bound small loops (<= 8 iterations) with internal branches (no nested loops, single latch) -> FullUnroll
        if (lp.body.size() > 2 && lp.body.size() <= 10 && cLo && cHi && *cHi >= *cLo && *cHi - *cLo <= 8) {
            int latches = 0;
            unsigned latchBlk = 0;
            bool innerSafe = true;
            int incCntAll = 0;
            for (unsigned b : lp.body) {
                if (cfg.loopOfBlock[b] != static_cast<int>(li)) { innerSafe = false; break; }
                const auto& blkB = fn.blocks[b];
                if (blkB.exit == Exit::Jump && blkB.yes == hdr) { ++latches; latchBlk = b; }
                else if (blkB.exit == Exit::Branch && (blkB.yes == hdr || blkB.no == hdr)) { innerSafe = false; break; }
                for (const auto& op : blkB.ops) {
                    if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin ||
                        op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                        op.op == Op::Print || op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice ||
                        op.op == Op::FieldSet || op.op == Op::PlaceSet || op.op == Op::RepeatHeap ||
                        op.op == Op::RepeatRegion || op.op == Op::Clone || op.op == Op::ClearOwner ||
                        op.op == Op::ClearField || op.op == Op::ClearIndex ||
                        op.op == Op::TileRepeat || op.op == Op::FindFirst ||
                        op.op == Op::GemmF32 || op.op == Op::MatMulMod ||
                        op.op == Op::FloydStepU16 || op.op == Op::ArgMinU16 ||
                        (op.op == Op::Assert && !op.boundsProven && !op.unchecked)) { innerSafe = false; break; }
                    if ((op.op == Op::Index || op.op == Op::IndexSet) && !op.boundsProven) { innerSafe = false; break; }
                    if ((op.op == Op::Negate || op.op == Op::AbsDiff) && op.result.type == Type::Int && !op.overflowProven) { innerSafe = false; break; }
                    if (op.op == Op::Binary && op.inputs[0].type == Type::Int) {
                        if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                             op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) { innerSafe = false; break; }
                        if ((op.data == "/" || op.data == "%") && !op.divSafe) { innerSafe = false; break; }
                    }
                    if (op.op == Op::Store && op.slot == iterSlot) {
                        ++incCntAll;
                        auto kd = defOp.find(op.inputs[0].id);
                        if (kd == defOp.end() || kd->second->op != Op::Binary || kd->second->data != "rangeIncrement") {
                            innerSafe = false; break;
                        }
                    }
                }
                if (!innerSafe) break;
            }
            if (innerSafe && latches == 1 && incCntAll == 1 && fn.blocks[latchBlk].loopHint == LoopHint::None) {
                fn.blocks[latchBlk].loopHint = LoopHint::FullUnroll;
            }
            continue;
        }

        if (lp.body.size() != 2) continue;
        const unsigned body = hblk.yes;
        if (fn.blocks[body].exit != Exit::Jump || fn.blocks[body].yes != hdr) continue;
        if (fn.blocks[body].loopHint != LoopHint::None || fn.blocks[body].unrollCount > 1) continue;

        bool safe = true;
        int incCnt = 0;
        std::unordered_set<int> readGroups, writeGroups;
        std::vector<std::size_t> writeIdxPositions, memOpPositions;
        int totalMemOps = 0;
        for (std::size_t oi = 0; oi < fn.blocks[body].ops.size(); ++oi) {
            const auto& op = fn.blocks[body].ops[oi];
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin ||
                op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                op.op == Op::Print || op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice ||
                op.op == Op::FieldSet || op.op == Op::PlaceSet || op.op == Op::RepeatHeap ||
                op.op == Op::RepeatRegion || op.op == Op::Clone || op.op == Op::ClearOwner ||
                op.op == Op::ClearField || op.op == Op::ClearIndex ||
                op.op == Op::TileRepeat || op.op == Op::FindFirst ||
                op.op == Op::GemmF32 || op.op == Op::MatMulMod ||
                op.op == Op::FloydStepU16 || op.op == Op::ArgMinU16 ||
                (op.op == Op::Assert && !op.boundsProven && !op.unchecked)) {
                safe = false; break;
            }
            if (op.op == Op::Index || op.op == Op::IndexSet) {
                if (!op.boundsProven) { safe = false; break; }
                ++totalMemOps;
                memOpPositions.push_back(oi);
                if (op.op == Op::Index) readGroups.insert(op.aliasGroup);
                else {
                    writeGroups.insert(op.aliasGroup);
                    writeIdxPositions.push_back(oi);
                }
            } else if ((op.op == Op::Negate || op.op == Op::AbsDiff) &&
                       op.result.type == Type::Int && !op.overflowProven) {
                safe = false; break;
            } else if (op.op == Op::Binary && op.inputs[0].type == Type::Int) {
                if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                     op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) {
                    safe = false; break;
                }
                if ((op.data == "/" || op.data == "%") && !op.divSafe) { safe = false; break; }
            } else if (op.op == Op::Store && op.slot == iterSlot) {
                ++incCnt;
                auto kd = defOp.find(op.inputs[0].id);
                if (kd == defOp.end() || kd->second->op != Op::Binary || kd->second->data != "rangeIncrement") {
                    safe = false; break;
                }
            }
        }
        if (!safe || incCnt != 1) continue;

        // Note: cLo and cHi already computed above for 2-block loops too
        if (cLo && cHi && *cHi >= *cLo && *cHi - *cLo <= 8) {
            fn.blocks[body].loopHint = LoopHint::FullUnroll;
            continue;
        }
        if (totalMemOps == 0) continue;
        if (writeGroups.empty()) {
            fn.blocks[body].loopHint = LoopHint::Vectorize;
            continue;
        }
        bool disjoint = !writeGroups.contains(-1) && writeIdxPositions.size() == 1;
        if (disjoint) {
            for (int wg : writeGroups) {
                if (readGroups.contains(wg)) { disjoint = false; break; }
            }
        }
        if (disjoint) {
            auto usesIterAffine = [&](auto& self, Value v, int d) -> bool {
                if (d > 12) return false;
                auto dit = defOp.find(v.id);
                if (dit == defOp.end()) return false;
                const Operation* o = dit->second;
                if (o->op == Op::Load) return o->slot == iterSlot;
                if (o->op == Op::Binary && (o->data == "+" || o->data == "-")) {
                    bool lIter = self(self, o->inputs[0], d + 1);
                    bool rIter = self(self, o->inputs[1], d + 1);
                    if (o->data == "+") return lIter != rIter;
                    return lIter && !rIter;
                }
                return false;
            };
            const Value wIdx = fn.blocks[body].ops[writeIdxPositions[0]].inputs[1];
            if (usesIterAffine(usesIterAffine, wIdx, 0)) {
                const int ag = nextAg++;
                for (std::size_t oi : memOpPositions) fn.blocks[body].ops[oi].loopAccessGroup = ag;
                fn.blocks[body].loopHint = LoopHint::VectorizeParallel;
                fn.blocks[body].loopAccessGroup = ag;
                continue;
            }
        }
        fn.blocks[body].loopHint = LoopHint::Vectorize;
    }
}

/// Clear `initialize = true` on `RepeatRegion`/`RepeatHeap` allocations of `IntArray` when
/// every element `0..len-1` is unconditionally overwritten before the array is ever read.
inline void eliminateDeadRepeatInit(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> loadSlot;
    std::vector<int> storeCount(fn.slots.size(), 0);
    std::vector<bool> hasClearOwner(fn.slots.size(), false);
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const auto& op : fn.blocks[bi].ops) {
            if (op.result.type != Type::Void) defOp[op.result.id] = &op;
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
            if (op.op == Op::Store && op.slot < fn.slots.size()) ++storeCount[op.slot];
            if (op.op == Op::ClearOwner && op.slot < fn.slots.size()) hasClearOwner[op.slot] = true;
        }
    }

    auto opTouchesSlot = [&](const Operation& op, unsigned slot) -> bool {
        if ((op.op == Op::Load || op.op == Op::Store || op.op == Op::Drop ||
             op.op == Op::ClearOwner || op.op == Op::ClearField) && op.slot == slot) {
            return true;
        }
        for (unsigned ms : op.mutArgSlots) {
            if (ms == slot) return true;
        }
        return false;
    };

    for (unsigned bAlloc = 0; bAlloc < fn.blocks.size(); ++bAlloc) {
        const auto& allocOps = fn.blocks[bAlloc].ops;
        for (std::size_t opIdx = 0; opIdx < allocOps.size(); ++opIdx) {
            auto& repOp = fn.blocks[bAlloc].ops[opIdx];
            if ((repOp.op != Op::RepeatRegion && repOp.op != Op::RepeatHeap) ||
                !repOp.initialize || repOp.result.type != Type::IntArray || repOp.inputs.size() < 2) {
                continue;
            }
            const Value lenVal = repOp.inputs[1];
            std::optional<unsigned> lenSlot;
            std::optional<std::int64_t> lenConst;
            if (auto ls = loadSlot.find(lenVal.id); ls != loadSlot.end()) lenSlot = ls->second;
            else if (auto ld = defOp.find(lenVal.id); ld != defOp.end() && ld->second->op == Op::Int)
                lenConst = parseConstInt(ld->second->data);

            int storeIdx = -1;
            unsigned s = 0;
            for (std::size_t k = opIdx + 1; k < allocOps.size(); ++k) {
                if (allocOps[k].op == Op::Store && !allocOps[k].inputs.empty() &&
                    allocOps[k].inputs[0].id == repOp.result.id) {
                    storeIdx = static_cast<int>(k);
                    s = allocOps[k].slot;
                    break;
                }
            }
            if (storeIdx < 0 || s >= fn.slots.size() || fn.slots[s] != Type::IntArray) continue;
            if (s < fn.source.params.size() || (s < fn.slotByRef.size() && fn.slotByRef[s])) continue;
            if (storeCount[s] != 1 || hasClearOwner[s]) continue;

            // Verify repOp.result is only used by that store, and lenSlot isn't overwritten in bAlloc after opIdx
            bool badAlloc = false;
            for (std::size_t k = 0; k < allocOps.size(); ++k) {
                const auto& op = allocOps[k];
                if (static_cast<int>(k) != storeIdx) {
                    for (const auto& in : op.inputs) {
                        if (in.id == repOp.result.id) { badAlloc = true; break; }
                    }
                    if (badAlloc || opTouchesSlot(op, s)) { badAlloc = true; break; }
                }
                if (lenSlot && k > opIdx &&
                    ((op.op == Op::Store && op.slot == *lenSlot) ||
                     std::find(op.mutArgSlots.begin(), op.mutArgSlots.end(), *lenSlot) != op.mutArgSlots.end())) {
                    badAlloc = true; break;
                }
            }
            if (!badAlloc && fn.blocks[bAlloc].value && fn.blocks[bAlloc].value->id == repOp.result.id)
                badAlloc = true;
            if (badAlloc) continue;

            // Walk forward along single-edge Jump chain to loop header `hdr`
            if (fn.blocks[bAlloc].exit != Exit::Jump) continue;
            unsigned prevBlk = bAlloc;
            unsigned cur = fn.blocks[bAlloc].yes;
            bool foundHdr = false;
            for (int hop = 0; hop < 16 && cur < fn.blocks.size(); ++hop) {
                if (cfg.headerToLoop.count(cur)) {
                    foundHdr = true;
                    break;
                }
                if (cfg.preds[cur].size() != 1 || fn.blocks[cur].exit != Exit::Jump) break;
                bool dirty = false;
                for (const auto& op : fn.blocks[cur].ops) {
                    if (opTouchesSlot(op, s)) { dirty = true; break; }
                    if (lenSlot && ((op.op == Op::Store && op.slot == *lenSlot) ||
                        std::find(op.mutArgSlots.begin(), op.mutArgSlots.end(), *lenSlot) != op.mutArgSlots.end())) {
                        dirty = true; break;
                    }
                }
                if (dirty) break;
                prevBlk = cur;
                cur = fn.blocks[cur].yes;
            }
            if (!foundHdr) continue;

            const unsigned hdr = cur;
            const auto& lp = cfg.loops[cfg.headerToLoop.at(hdr)];
            if (lp.preheaders.size() != 1 || lp.preheaders[0] != prevBlk || lp.body.size() != 2) continue;
            const auto& hblk = fn.blocks[hdr];
            if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
                continue;
            const unsigned body = hblk.yes;
            if (body >= fn.blocks.size() || fn.blocks[body].exit != Exit::Jump || fn.blocks[body].yes != hdr)
                continue;

            bool okHdr = true;
            for (const auto& op : hblk.ops) {
                if (opTouchesSlot(op, s)) { okHdr = false; break; }
                if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary) { okHdr = false; break; }
            }
            if (!okHdr) continue;
            auto hCmp = defOp.find(hblk.value->id);
            if (hCmp == defOp.end() || hCmp->second->op != Op::Binary || hCmp->second->data != "<" ||
                hCmp->second->inputs.size() != 2) continue;
            auto jls = loadSlot.find(hCmp->second->inputs[0].id);
            if (jls == loadSlot.end()) continue;
            const unsigned iterSlot = jls->second;
            if (iterSlot == s || iterSlot >= fn.slots.size() || fn.slots[iterSlot] != Type::Int) continue;
            if (iterSlot < fn.slotByRef.size() && fn.slotByRef[iterSlot]) continue;

            const Value upperVal = hCmp->second->inputs[1];
            bool sameBound = (upperVal.id == lenVal.id);
            if (!sameBound && lenConst) {
                auto ud = defOp.find(upperVal.id);
                if (ud != defOp.end() && ud->second->op == Op::Int && parseConstInt(ud->second->data) == *lenConst)
                    sameBound = true;
            }
            if (!sameBound && lenSlot) {
                auto uls = loadSlot.find(upperVal.id);
                if (uls != loadSlot.end() && uls->second == *lenSlot) sameBound = true;
            }
            if (!sameBound) continue;

            std::optional<std::int64_t> iterInit;
            for (const auto& op : fn.blocks[prevBlk].ops) {
                if (op.op == Op::Store && op.slot == iterSlot && !op.inputs.empty()) {
                    auto idIt = defOp.find(op.inputs[0].id);
                    if (idIt != defOp.end() && idIt->second->op == Op::Int)
                        iterInit = parseConstInt(idIt->second->data);
                    else iterInit.reset();
                }
            }
            if (iterInit != 0) continue;

            // Validate loop body fills s[iterSlot] before advancing iterSlot and never reads s
            const auto& bodyOps = fn.blocks[body].ops;
            int iterStores = 0;
            int iterStorePos = -1;
            bool okBody = true;
            std::unordered_set<unsigned> loadSIds;
            for (int oi = 0; oi < static_cast<int>(bodyOps.size()); ++oi) {
                const auto& op = bodyOps[oi];
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Builtin ||
                    op.op == Op::DynCall || op.op == Op::ClosureCall || op.op == Op::Clone ||
                    op.op == Op::ClearOwner || op.op == Op::ClearField || op.op == Op::ClearIndex ||
                    op.op == Op::Drop) {
                    okBody = false; break;
                }
                if (lenSlot && op.op == Op::Store && op.slot == *lenSlot) { okBody = false; break; }
                if (op.op == Op::Load && op.slot == s) loadSIds.insert(op.result.id);
                if (op.op == Op::Store && op.slot == iterSlot) {
                    ++iterStores;
                    iterStorePos = oi;
                }
            }
            if (!okBody || iterStores != 1 || iterStorePos < 0 || bodyOps[iterStorePos].inputs.empty()) continue;
            auto incDef = defOp.find(bodyOps[iterStorePos].inputs[0].id);
            if (incDef == defOp.end() || incDef->second->op != Op::Binary ||
                (incDef->second->data != "rangeIncrement" && incDef->second->data != "+") ||
                incDef->second->inputs.size() != 2) continue;
            auto incL = loadSlot.find(incDef->second->inputs[0].id);
            auto incStep = parseConstInt(defOp.count(incDef->second->inputs[1].id)
                                         ? defOp.at(incDef->second->inputs[1].id)->data : "");
            if (incL == loadSlot.end() || incL->second != iterSlot || incStep != 1) continue;

            bool wroteSI = false;
            for (int oi = 0; oi < static_cast<int>(bodyOps.size()); ++oi) {
                const auto& op = bodyOps[oi];
                if (op.op == Op::Index && !op.inputs.empty() && loadSIds.contains(op.inputs[0].id)) {
                    okBody = false; break;
                }
                if (op.op == Op::Load && op.slot == iterSlot && oi > iterStorePos) {
                    okBody = false; break;
                }
                for (std::size_t k = 0; k < op.inputs.size(); ++k) {
                    if (!loadSIds.contains(op.inputs[k].id)) continue;
                    if (op.op != Op::IndexSet || k != 0) { okBody = false; break; }
                }
                if (!okBody) break;
                if (op.op == Op::IndexSet && op.inputs.size() >= 3 && loadSIds.contains(op.inputs[0].id)) {
                    auto ils = loadSlot.find(op.inputs[1].id);
                    if (ils == loadSlot.end() || ils->second != iterSlot || oi >= iterStorePos) {
                        okBody = false; break;
                    }
                    wroteSI = true;
                }
            }
            if (okBody && wroteSI) {
                repOp.initialize = false;
            }
        }
    }
}

/// Rewrite `s = (s + C) % m` inside a loop with `0 <= s < m` and invariant `m >= 1`
/// to use preheader-hoisted `step = C % m` so `%` compiles to a single branchless `sub`/`select`.
inline void strengthReduceLoopModStep(
    FunctionIR& fn,
    const std::unordered_map<unsigned, IntRange>& valRanges,
    const std::unordered_map<std::size_t, std::vector<std::pair<unsigned, unsigned>>>& modLtDiv) {
    if (fn.blocks.empty() || modLtDiv.empty()) return;
    for (;;) {
        const detail::CFGInfo cfg = detail::buildCFG(fn);
        std::unordered_map<unsigned, const Operation*> defOp;
        std::unordered_map<unsigned, unsigned> loadSlot;
        std::vector<std::unordered_set<unsigned>> loopWritten(cfg.loops.size());
        for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
            for (const auto& op : fn.blocks[bi].ops) {
                if (op.result.type != Type::Void) defOp[op.result.id] = &op;
                if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
                if (op.op == Op::Store && cfg.loopOfBlock[bi] >= 0) {
                    const std::size_t li = static_cast<std::size_t>(cfg.loopOfBlock[bi]);
                    for (std::size_t k = 0; k < cfg.loops.size(); ++k) {
                        if (cfg.loops[k].body.contains(bi)) loopWritten[k].insert(op.slot);
                    }
                    (void)li;
                }
            }
        }
        bool changed = false;
        for (const auto& [li, pairs] : modLtDiv) {
            if (li >= cfg.loops.size()) continue;
            const auto& lp = cfg.loops[li];
            if (lp.preheaders.size() != 1) continue;
            const unsigned pre = lp.preheaders[0];
            for (const auto& [s, mSlot] : pairs) {
                if (loopWritten[li].contains(mSlot)) continue;
                int storeCnt = 0;
                unsigned storeBlk = 0;
                std::size_t remPos = 0, addPos = 0;
                const Operation* storeOp = nullptr;
                for (unsigned b : lp.body) {
                    for (const auto& op : fn.blocks[b].ops) {
                        if (op.op == Op::Store && op.slot == s) {
                            ++storeCnt; storeBlk = b; storeOp = &op;
                        }
                    }
                }
                if (storeCnt != 1 || cfg.loopOfBlock[storeBlk] != static_cast<int>(li) ||
                    !storeOp || storeOp->inputs.empty())
                    continue;
                auto& bOps = fn.blocks[storeBlk].ops;
                bool foundRem = false, foundAdd = false;
                for (std::size_t oi = 0; oi < bOps.size(); ++oi) {
                    if (bOps[oi].result.type != Type::Void && bOps[oi].result.id == storeOp->inputs[0].id &&
                        bOps[oi].op == Op::Binary && bOps[oi].data == "%" &&
                        bOps[oi].divSafe && bOps[oi].unsignedDiv && !bOps[oi].modNoOp && bOps[oi].modSubSteps == 0) {
                        remPos = oi; foundRem = true; break;
                    }
                }
                if (!foundRem) continue;
                auto mls = loadSlot.find(bOps[remPos].inputs[1].id);
                if (mls == loadSlot.end() || mls->second != mSlot) continue;
                auto mIt = valRanges.find(bOps[remPos].inputs[1].id);
                if (mIt == valRanges.end() || mIt->second.lower < 1 || mIt->second.upper > kMaxInt / 2) continue;
                for (std::size_t oi = 0; oi < remPos; ++oi) {
                    if (bOps[oi].result.type != Type::Void && bOps[oi].result.id == bOps[remPos].inputs[0].id &&
                        bOps[oi].op == Op::Binary && bOps[oi].data == "+") {
                        addPos = oi; foundAdd = true; break;
                    }
                }
                if (!foundAdd) continue;
                int cSide = -1;
                std::int64_t constVal = 0;
                for (int side = 0; side < 2; ++side) {
                    auto sls = loadSlot.find(bOps[addPos].inputs[side].id);
                    auto cd = defOp.find(bOps[addPos].inputs[1 - side].id);
                    auto cv = (cd != defOp.end() && cd->second->op == Op::Int) ? parseConstInt(cd->second->data) : std::nullopt;
                    if (sls != loadSlot.end() && sls->second == s && cv && *cv >= 1) {
                        cSide = 1 - side;
                        constVal = *cv;
                        break;
                    }
                }
                if (cSide < 0 || constVal < mIt->second.lower) continue;
                bool intervened = false;
                for (std::size_t oi = 0; oi < remPos; ++oi) {
                    if (bOps[oi].op == Op::Store && bOps[oi].slot == s) { intervened = true; break; }
                }
                if (intervened) continue;

                const unsigned stepSlot = static_cast<unsigned>(fn.slots.size());
                fn.slots.push_back(Type::Int);
                if (!fn.slotByRef.empty()) fn.slotByRef.push_back(false);
                if (!fn.slotElemBits.empty()) fn.slotElemBits.push_back(64);
                if (!fn.slotElemUnsigned.empty()) fn.slotElemUnsigned.push_back(false);
                if (!fn.slotAliasGroup.empty()) fn.slotAliasGroup.push_back(-1);

                const Span sp = bOps[remPos].span;
                const Value mPre{Type::Int, fn.nextValue++};
                const Value cPre{Type::Int, fn.nextValue++};
                const Value stepVal{Type::Int, fn.nextValue++};
                fn.blocks[pre].ops.push_back({Op::Load, mPre, {}, {}, mSlot, sp});
                fn.blocks[pre].ops.push_back({Op::Int, cPre, {}, std::to_string(constVal), 0, sp});
                Operation remStep{Op::Binary, stepVal, {cPre, mPre}, "%", 0, sp};
                remStep.divSafe = true;
                remStep.unsignedDiv = true;
                remStep.overflowProven = true;
                remStep.width = (constVal <= 2147483647LL && mIt->second.upper <= 4294967295LL) ? 32 : 64;
                fn.blocks[pre].ops.push_back(remStep);
                fn.blocks[pre].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {stepVal}, {}, stepSlot, sp});

                const Value ldStep{Type::Int, fn.nextValue++};
                const bool fits32 = (mIt->second.upper <= 1073741823LL);
                bOps[addPos].overflowProven = true;
                bOps[addPos].width = fits32 ? 32 : 64;
                bOps[addPos].inputs[cSide] = ldStep;
                bOps[remPos].modSubSteps = 1;
                bOps[remPos].modNoOp = false;
                bOps[remPos].divSafe = true;
                bOps[remPos].unsignedDiv = true;
                bOps[remPos].overflowProven = true;
                bOps[remPos].width = fits32 ? 32 : 64;
                bOps.insert(bOps.begin() + static_cast<std::ptrdiff_t>(addPos),
                            Operation{Op::Load, ldStep, {}, {}, stepSlot, sp});
                changed = true;
                break;
            }
            if (changed) break;
        }
        if (!changed) break;
    }
}

/// Strength-reduce `(a * iter + b) % M` inside non-vectorizable canonical `for` loops into
/// a preheader-evaluated `r = (a * init + b) % M` and latch step `r = (r + (a % M)) % M`
/// using `modSubSteps = 1`, eliminating inner-loop multiply-shifts without inhibiting vectorization.
inline void strengthReduceAffineIterModConst(
    FunctionIR& fn,
    const std::unordered_map<unsigned, IntRange>& valRanges) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, int> defPos;
    std::unordered_map<unsigned, unsigned> loadSlot;
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto& ops = fn.blocks[bi].ops;
        for (int oi = 0; oi < static_cast<int>(ops.size()); ++oi) {
            const auto& op = ops[oi];
            if (op.result.type != Type::Void) {
                defOp[op.result.id] = &op;
                defBlock[op.result.id] = bi;
                defPos[op.result.id] = oi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
        }
    }

    struct AffinePlan {
        unsigned pre = 0;
        unsigned latch = 0;
        unsigned modBlk = 0;
        int modIdx = 0;
        unsigned remSlot = 0;
        std::vector<Operation> preInitOps;
        std::int64_t step = 0;
        std::int64_t modConst = 0;
        Span span{};
        bool emitUpdate = true;
    };
    std::vector<AffinePlan> plans;

    struct AffNode {
        std::int64_t coeffA = 0;
        IntRange preRng{0, 0};
        bool ok = false;
    };

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lp = cfg.loops[li];
        if (lp.preheaders.size() != 1) continue;
        const unsigned pre = lp.preheaders[0];
        const unsigned hdr = lp.header;
        if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != hdr) continue;
        const auto& hblk = fn.blocks[hdr];
        if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
            continue;
        bool okHdr = true;
        for (const auto& op : hblk.ops) {
            if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary) { okHdr = false; break; }
        }
        if (!okHdr) continue;
        auto hCmp = defOp.find(hblk.value->id);
        if (hCmp == defOp.end() || hCmp->second->op != Op::Binary || hCmp->second->data != "<" ||
            hCmp->second->inputs.size() != 2) continue;
        auto jls = loadSlot.find(hCmp->second->inputs[0].id);
        if (jls == loadSlot.end()) continue;
        const unsigned iterSlot = jls->second;
        if (iterSlot >= fn.slots.size() || fn.slots[iterSlot] != Type::Int) continue;
        if (iterSlot < fn.slotByRef.size() && fn.slotByRef[iterSlot]) continue;

        int backPreds = 0;
        unsigned latch = 0;
        for (unsigned p : cfg.preds[hdr]) {
            if (lp.body.contains(p)) { ++backPreds; latch = p; }
        }
        if (backPreds != 1 || fn.blocks[latch].exit != Exit::Jump || fn.blocks[latch].yes != hdr) continue;

        int iterStores = 0;
        int iterStorePos = -1;
        const Operation* iterStoreOp = nullptr;
        std::unordered_set<unsigned> loopWritten;
        for (unsigned b : lp.body) {
            for (int oi = 0; oi < static_cast<int>(fn.blocks[b].ops.size()); ++oi) {
                const auto& op = fn.blocks[b].ops[oi];
                if (op.op == Op::Store) {
                    loopWritten.insert(op.slot);
                    if (op.slot == iterSlot) {
                        ++iterStores;
                        if (b == latch) { iterStorePos = oi; iterStoreOp = &op; }
                    }
                }
                for (unsigned ms : op.mutArgSlots) loopWritten.insert(ms);
            }
        }
        if (iterStores != 1 || iterStorePos < 0 || !iterStoreOp || iterStoreOp->inputs.empty()) continue;
        auto incDef = defOp.find(iterStoreOp->inputs[0].id);
        if (incDef == defOp.end() || incDef->second->op != Op::Binary ||
            (incDef->second->data != "rangeIncrement" && incDef->second->data != "+") ||
            incDef->second->inputs.size() != 2) continue;
        auto incL = loadSlot.find(incDef->second->inputs[0].id);
        auto incStep = parseConstInt(defOp.count(incDef->second->inputs[1].id)
                                     ? defOp.at(incDef->second->inputs[1].id)->data : "");
        if (incL == loadSlot.end() || incL->second != iterSlot || incStep != 1) continue;

        std::optional<std::int64_t> initVal;
        for (const auto& op : fn.blocks[pre].ops) {
            if (op.op == Op::Store && op.slot == iterSlot && !op.inputs.empty()) {
                auto idIt = defOp.find(op.inputs[0].id);
                if (idIt != defOp.end() && idIt->second->op == Op::Int) {
                    initVal = parseConstInt(idIt->second->data);
                } else {
                    initVal.reset();
                }
            }
        }
        if (!initVal || *initVal < 0) continue;

        // Non-vectorizable gate: multi-block loop body, loop-carried scalar recurrence, or scatter IndexSet
        bool nonVectorizable = (lp.body.size() > 2);
        if (!nonVectorizable) {
            std::unordered_set<unsigned> hdrRead;
            for (const auto& op : hblk.ops) {
                if (op.op == Op::Load) hdrRead.insert(op.slot);
            }
            std::unordered_map<unsigned, int> firstLoad, firstStore;
            const auto& lOps = fn.blocks[latch].ops;
            for (int oi = 0; oi < static_cast<int>(lOps.size()); ++oi) {
                const auto& op = lOps[oi];
                if (op.op == Op::Load && !firstLoad.count(op.slot)) firstLoad[op.slot] = oi;
                if (op.op == Op::Store && !firstStore.count(op.slot)) firstStore[op.slot] = oi;
            }
            for (const auto& [sl, stPos] : firstStore) {
                if (sl == iterSlot || sl >= fn.slots.size() || fn.slots[sl] != Type::Int) continue;
                if (hdrRead.contains(sl)) { nonVectorizable = true; break; }
                auto fl = firstLoad.find(sl);
                if (fl != firstLoad.end() && fl->second < stPos) { nonVectorizable = true; break; }
            }
        }
        if (!nonVectorizable) {
            auto isAffineOfIter = [&](auto& self, Value v, int d) -> bool {
                if (d > 8) return false;
                if (auto ls = loadSlot.find(v.id); ls != loadSlot.end()) {
                    return ls->second == iterSlot || !loopWritten.contains(ls->second);
                }
                auto dit = defOp.find(v.id);
                if (dit == defOp.end()) return false;
                if (dit->second->op == Op::Int) return true;
                if (dit->second->op == Op::Binary && dit->second->inputs.size() == 2 &&
                    (dit->second->data == "+" || dit->second->data == "-" || dit->second->data == "*")) {
                    return self(self, dit->second->inputs[0], d + 1) &&
                           self(self, dit->second->inputs[1], d + 1);
                }
                return false;
            };
            for (unsigned b : lp.body) {
                for (const auto& op : fn.blocks[b].ops) {
                    if (op.op == Op::IndexSet && op.inputs.size() >= 2 &&
                        !isAffineOfIter(isAffineOfIter, op.inputs[1], 0)) {
                        nonVectorizable = true;
                        break;
                    }
                }
                if (nonVectorizable) break;
            }
        }
        if (!nonVectorizable) continue;

        // Collect all slots referenced in header or feeding header's upper bound in pre
        std::unordered_set<unsigned> hdrSlots;
        for (const auto& op : hblk.ops) {
            if (op.op == Op::Load) hdrSlots.insert(op.slot);
        }
        {
            std::vector<unsigned> work{hCmp->second->inputs[1].id};
            std::unordered_set<unsigned> visited;
            for (std::size_t wi = 0; wi < work.size() && work.size() < 32; ++wi) {
                const unsigned vid = work[wi];
                if (!visited.insert(vid).second) continue;
                auto dit = defOp.find(vid);
                if (dit == defOp.end()) continue;
                if (dit->second->op == Op::Load) hdrSlots.insert(dit->second->slot);
                for (const Value& in : dit->second->inputs) work.push_back(in.id);
            }
        }

        std::unordered_map<std::string, unsigned> loopRemSlots;
        auto canonExpr = [&](auto& self, Value v, int depth) -> std::string {
            if (depth > 8) return "";
            auto dit = defOp.find(v.id);
            if (dit == defOp.end()) return "";
            const Operation* dop = dit->second;
            if (dop->op == Op::Int) return "#" + dop->data;
            if (dop->op == Op::Load) {
                if (dop->slot == iterSlot) return "I";
                return "S" + std::to_string(dop->slot);
            }
            if (dop->op == Op::Binary && dop->inputs.size() == 2) {
                std::string ls = self(self, dop->inputs[0], depth + 1);
                std::string rs = self(self, dop->inputs[1], depth + 1);
                if ((dop->data == "+" || dop->data == "*") && rs < ls) std::swap(ls, rs);
                return "(" + dop->data + "," + ls + "," + rs + ")";
            }
            return "";
        };

        for (unsigned b : lp.body) {
            if (b == hdr) continue;
            // Ensure no internal Exit::Branch / Switch / Assert between hdr and b refined invSlots
            bool hasInnerBranchDom = false;
            unsigned cur = b;
            while (cur != hdr && cur < cfg.idom.size() && cfg.idom[cur] >= 0) {
                cur = static_cast<unsigned>(cfg.idom[cur]);
                if (cur == hdr) break;
                if (fn.blocks[cur].exit == Exit::Branch || fn.blocks[cur].exit == Exit::Switch) {
                    hasInnerBranchDom = true;
                    break;
                }
                for (const auto& op : fn.blocks[cur].ops) {
                    if (op.op == Op::Assert && !op.boundsProven && !op.unchecked) {
                        hasInnerBranchDom = true;
                        break;
                    }
                }
                if (hasInnerBranchDom) break;
            }
            if (hasInnerBranchDom) continue;

            const auto& ops = fn.blocks[b].ops;
            for (int oi = 0; oi < static_cast<int>(ops.size()); ++oi) {
                if (b == latch && oi >= iterStorePos) break;
                const auto& op = ops[oi];
                if (op.op == Op::Assert && !op.boundsProven && !op.unchecked) break;
                if (op.op != Op::Binary || op.data != "%" || !op.divSafe || !op.unsignedDiv ||
                    op.modNoOp || op.modSubSteps > 0 || op.inputs.size() != 2) continue;

                auto mDef = defOp.find(op.inputs[1].id);
                auto mOpt = (mDef != defOp.end() && mDef->second->op == Op::Int)
                    ? parseConstInt(mDef->second->data) : std::nullopt;
                if (!mOpt || *mOpt < 3 || *mOpt > 1073741823LL || ((*mOpt & (*mOpt - 1)) == 0)) continue;
                const std::int64_t M = *mOpt;

                auto directLs = loadSlot.find(op.inputs[0].id);
                if (directLs != loadSlot.end() && directLs->second == iterSlot) continue;

                std::unordered_map<unsigned, IntRange> nodeRng;
                auto analyzeAffine = [&](auto& self, Value v, int depth) -> AffNode {
                    if (depth > 8) return {};
                    auto dit = defOp.find(v.id);
                    if (dit == defOp.end()) return {};
                    const Operation* dop = dit->second;
                    if (dop->op == Op::Int) {
                        auto c = parseConstInt(dop->data);
                        if (!c) return {};
                        nodeRng[v.id] = {*c, *c};
                        return {0, {*c, *c}, true};
                    }
                    if (dop->op == Op::Load) {
                        const unsigned sl = dop->slot;
                        if (sl == iterSlot) {
                            if (defBlock[v.id] == latch && defPos[v.id] >= iterStorePos) return {};
                            nodeRng[v.id] = {*initVal, *initVal};
                            return {1, {*initVal, *initVal}, true};
                        }
                        if (sl >= fn.slots.size() || fn.slots[sl] != Type::Int) return {};
                        if (sl < fn.slotByRef.size() && fn.slotByRef[sl]) return {};
                        if (loopWritten.contains(sl) || hdrSlots.contains(sl)) return {};
                        auto vrIt = valRanges.find(v.id);
                        if (vrIt == valRanges.end() || vrIt->second.lower < 0) return {};
                        nodeRng[v.id] = vrIt->second;
                        return {0, vrIt->second, true};
                    }
                    if (dop->op == Op::Binary && dop->overflowProven && dop->inputs.size() == 2) {
                        if (dop->data == "+") {
                            AffNode L = self(self, dop->inputs[0], depth + 1);
                            AffNode R = self(self, dop->inputs[1], depth + 1);
                            if (!L.ok || !R.ok) return {};
                            auto cA = checkedAdd64(L.coeffA, R.coeffA);
                            auto rng = ReturnRanges::addRanges(L.preRng, R.preRng);
                            if (!cA || !rng) return {};
                            nodeRng[v.id] = *rng;
                            return {*cA, *rng, true};
                        }
                        if (dop->data == "*") {
                            AffNode L = self(self, dop->inputs[0], depth + 1);
                            AffNode R = self(self, dop->inputs[1], depth + 1);
                            if (!L.ok || !R.ok) return {};
                            std::int64_t cA = 0;
                            if (L.coeffA != 0 && R.coeffA != 0) return {};
                            if (L.coeffA != 0) {
                                if (R.preRng.lower != R.preRng.upper || R.preRng.lower < 0) return {};
                                auto p = checkedMul64(L.coeffA, R.preRng.lower);
                                if (!p) return {};
                                cA = *p;
                            } else if (R.coeffA != 0) {
                                if (L.preRng.lower != L.preRng.upper || L.preRng.lower < 0) return {};
                                auto p = checkedMul64(R.coeffA, L.preRng.lower);
                                if (!p) return {};
                                cA = *p;
                            }
                            auto rng = ReturnRanges::multiplyRanges(L.preRng, R.preRng);
                            if (!rng) return {};
                            nodeRng[v.id] = *rng;
                            return {cA, *rng, true};
                        }
                    }
                    return {};
                };

                AffNode root = analyzeAffine(analyzeAffine, op.inputs[0], 0);
                if (!root.ok || root.coeffA <= 0) continue;
                const std::int64_t step = root.coeffA % M;
                if (step <= 0 || root.preRng.lower < 0 || root.preRng.upper > kMaxInt) continue;

                const std::string exprKey = std::to_string(M) + "|" + canonExpr(canonExpr, op.inputs[0], 0);
                if (auto it = loopRemSlots.find(exprKey); it != loopRemSlots.end()) {
                    plans.push_back({pre, latch, b, oi, it->second, {}, 0, M, op.span, false});
                    continue;
                }

                const unsigned remSlot = static_cast<unsigned>(fn.slots.size());
                fn.slots.push_back(Type::Int);
                if (!fn.slotByRef.empty()) fn.slotByRef.push_back(false);
                if (!fn.slotElemBits.empty()) fn.slotElemBits.push_back(64);
                if (!fn.slotElemUnsigned.empty()) fn.slotElemUnsigned.push_back(false);
                if (!fn.slotAliasGroup.empty()) fn.slotAliasGroup.push_back(-1);
                loopRemSlots[exprKey] = remSlot;

                std::vector<Operation> preInitOps;
                std::unordered_map<unsigned, Value> preClone;
                auto cloneToPre = [&](auto& self, Value origV) -> Value {
                    if (auto it = preClone.find(origV.id); it != preClone.end()) return it->second;
                    const Operation* dop = defOp.at(origV.id);
                    const Value nv{Type::Int, fn.nextValue++};
                    if (dop->op == Op::Load && dop->slot == iterSlot) {
                        preInitOps.push_back({Op::Int, nv, {}, std::to_string(*initVal), 0, op.span});
                    } else if (dop->op == Op::Int) {
                        preInitOps.push_back({Op::Int, nv, {}, dop->data, 0, dop->span});
                    } else if (dop->op == Op::Load) {
                        preInitOps.push_back({Op::Load, nv, {}, {}, dop->slot, dop->span});
                    } else {
                        const Value lV = self(self, dop->inputs[0]);
                        const Value rV = self(self, dop->inputs[1]);
                        Operation bin{Op::Binary, nv, {lV, rV}, dop->data, 0, dop->span};
                        bin.overflowProven = true;
                        const IntRange nr = nodeRng.at(origV.id);
                        bin.width = (nr.lower >= 0 && nr.upper <= 2147483647LL) ? 32 : 64;
                        preInitOps.push_back(std::move(bin));
                    }
                    preClone[origV.id] = nv;
                    return nv;
                };

                const Value lhsPre = cloneToPre(cloneToPre, op.inputs[0]);
                const Value cM{Type::Int, fn.nextValue++};
                const Value rInit{Type::Int, fn.nextValue++};
                preInitOps.push_back({Op::Int, cM, {}, std::to_string(M), 0, op.span});
                Operation remInit{Op::Binary, rInit, {lhsPre, cM}, "%", 0, op.span};
                remInit.divSafe = true;
                remInit.unsignedDiv = true;
                remInit.overflowProven = true;
                remInit.width = (root.preRng.lower >= 0 && root.preRng.upper <= 2147483647LL) ? 32 : 64;
                preInitOps.push_back(std::move(remInit));
                preInitOps.push_back({Op::Store, {Type::Void, fn.nextValue++}, {rInit}, {}, remSlot, op.span});

                plans.push_back({pre, latch, b, oi, remSlot, std::move(preInitOps), step, M, op.span, true});
            }
        }
    }

    for (auto& plan : plans) {
        const Operation orig = fn.blocks[plan.modBlk].ops[plan.modIdx];
        fn.blocks[plan.modBlk].ops[plan.modIdx] = Operation{Op::Load, orig.result, {}, {}, plan.remSlot, orig.span};
        if (!plan.emitUpdate) continue;

        auto& pOps = fn.blocks[plan.pre].ops;
        pOps.insert(pOps.end(), plan.preInitOps.begin(), plan.preInitOps.end());

        auto& lOps = fn.blocks[plan.latch].ops;
        const Value r0{Type::Int, fn.nextValue++};
        const Value cS{Type::Int, fn.nextValue++};
        const Value r1{Type::Int, fn.nextValue++};
        const Value cM{Type::Int, fn.nextValue++};
        const Value r2{Type::Int, fn.nextValue++};
        Operation addOp{Op::Binary, r1, {r0, cS}, "+", 0, plan.span};
        addOp.overflowProven = true;
        addOp.width = 32;
        Operation remOp{Op::Binary, r2, {r1, cM}, "%", 0, plan.span};
        remOp.divSafe = true;
        remOp.unsignedDiv = true;
        remOp.overflowProven = true;
        remOp.modSubSteps = 1;
        remOp.width = 32;
        lOps.push_back({Op::Load, r0, {}, {}, plan.remSlot, plan.span});
        lOps.push_back({Op::Int, cS, {}, std::to_string(plan.step), 0, plan.span});
        lOps.push_back(std::move(addOp));
        lOps.push_back({Op::Int, cM, {}, std::to_string(plan.modConst), 0, plan.span});
        lOps.push_back(std::move(remOp));
        lOps.push_back({Op::Store, {Type::Void, fn.nextValue++}, {r2}, {}, plan.remSlot, plan.span});
    }
}

/// Replace `iterSlot % K` (`3 <= K <= 64`, non-power-of-2) inside canonical `for` loops with
/// explicit IR periodic-`K` unrolling (`3 <= K <= 7`, literal remainders `0..K-1`) or a
/// carried `(r + 1 == K) ? 0 : (r + 1)` counter updated at the latch.
inline void strengthReduceForIterModConst(
    FunctionIR& fn,
    const std::unordered_map<unsigned, IntRange>& valRanges) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, int> defPos;
    std::unordered_map<unsigned, unsigned> loadSlot;
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto& ops = fn.blocks[bi].ops;
        for (int oi = 0; oi < static_cast<int>(ops.size()); ++oi) {
            const auto& op = ops[oi];
            if (op.result.type != Type::Void) {
                defOp[op.result.id] = &op;
                defBlock[op.result.id] = bi;
                defPos[op.result.id] = oi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
        }
    }

    struct ModPlan {
        unsigned pre = 0;
        unsigned latch = 0;
        unsigned iterSlot = 0;
        std::int64_t initVal = 0;
        std::map<std::int64_t, std::vector<std::pair<unsigned, int>>> occsByK;
    };
    struct UnrollPlan {
        unsigned pre = 0;
        unsigned hdr = 0;
        unsigned latch = 0;
        unsigned iterSlot = 0;
        int iterStorePos = -1;
        std::int64_t initVal = 0;
        std::int64_t K = 0;
        Value upperVal{};
        std::optional<Operation> upperHdrOp;
        std::unordered_set<int> modIdxSet;
        Span span{};
    };
    std::vector<ModPlan> plans;
    std::vector<UnrollPlan> unrollPlans;

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lp = cfg.loops[li];
        if (lp.preheaders.size() != 1) continue;
        const unsigned pre = lp.preheaders[0];
        const unsigned hdr = lp.header;
        if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != hdr) continue;
        const auto& hblk = fn.blocks[hdr];
        if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
            continue;
        bool okHdr = true;
        for (const auto& op : hblk.ops) {
            if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary) { okHdr = false; break; }
        }
        if (!okHdr) continue;
        auto hCmp = defOp.find(hblk.value->id);
        if (hCmp == defOp.end() || hCmp->second->op != Op::Binary || hCmp->second->data != "<") continue;
        auto jls = loadSlot.find(hCmp->second->inputs[0].id);
        if (jls == loadSlot.end()) continue;
        const unsigned iterSlot = jls->second;
        if (iterSlot >= fn.slots.size() || fn.slots[iterSlot] != Type::Int) continue;
        if (iterSlot < fn.slotByRef.size() && fn.slotByRef[iterSlot]) continue;

        int backPreds = 0;
        unsigned latch = 0;
        for (unsigned p : cfg.preds[hdr]) {
            if (lp.body.contains(p)) { ++backPreds; latch = p; }
        }
        if (backPreds != 1 || fn.blocks[latch].exit != Exit::Jump || fn.blocks[latch].yes != hdr) continue;

        int iterStores = 0;
        int iterStorePos = -1;
        const Operation* iterStoreOp = nullptr;
        std::unordered_set<unsigned> loopWritten;
        for (unsigned b : lp.body) {
            for (int oi = 0; oi < static_cast<int>(fn.blocks[b].ops.size()); ++oi) {
                const auto& op = fn.blocks[b].ops[oi];
                if (op.op == Op::Store) {
                    loopWritten.insert(op.slot);
                    if (op.slot == iterSlot) {
                        ++iterStores;
                        if (b == latch) { iterStorePos = oi; iterStoreOp = &op; }
                    }
                }
                for (unsigned ms : op.mutArgSlots) loopWritten.insert(ms);
            }
        }
        if (iterStores != 1 || iterStorePos < 0 || !iterStoreOp || iterStoreOp->inputs.empty()) continue;
        auto incDef = defOp.find(iterStoreOp->inputs[0].id);
        if (incDef == defOp.end() || incDef->second->op != Op::Binary ||
            (incDef->second->data != "rangeIncrement" && incDef->second->data != "+") ||
            incDef->second->inputs.size() != 2)
            continue;
        auto incL = loadSlot.find(incDef->second->inputs[0].id);
        auto incStep = parseConstInt(defOp.count(incDef->second->inputs[1].id)
                                     ? defOp.at(incDef->second->inputs[1].id)->data : "");
        if (incL == loadSlot.end() || incL->second != iterSlot || incStep != 1) continue;

        std::optional<std::int64_t> initVal;
        for (const auto& op : fn.blocks[pre].ops) {
            if (op.op == Op::Store && op.slot == iterSlot && !op.inputs.empty()) {
                auto idIt = defOp.find(op.inputs[0].id);
                if (idIt != defOp.end() && idIt->second->op == Op::Int) {
                    initVal = parseConstInt(idIt->second->data);
                } else {
                    initVal.reset();
                }
            }
        }
        if (!initVal || *initVal < 0) continue;

        std::unordered_set<std::int64_t> divKs;
        for (unsigned b : lp.body) {
            if (b == hdr) continue;
            const auto& ops = fn.blocks[b].ops;
            for (int oi = 0; oi < static_cast<int>(ops.size()); ++oi) {
                if (b == latch && oi >= iterStorePos) break;
                const auto& op = ops[oi];
                if (op.op != Op::Binary || op.data != "/" || !op.divSafe || !op.unsignedDiv ||
                    op.inputs.size() != 2)
                    continue;
                auto lhsS = loadSlot.find(op.inputs[0].id);
                if (lhsS == loadSlot.end() || lhsS->second != iterSlot) continue;
                if (b == latch && defBlock.count(op.inputs[0].id) &&
                    defBlock[op.inputs[0].id] == latch && defPos[op.inputs[0].id] >= iterStorePos) {
                    continue;
                }
                auto kd = defOp.find(op.inputs[1].id);
                auto kv = (kd != defOp.end() && kd->second->op == Op::Int)
                    ? parseConstInt(kd->second->data) : std::nullopt;
                if (kv && *kv >= 3 && *kv <= 64) divKs.insert(*kv);
            }
        }

        ModPlan plan{pre, latch, iterSlot, *initVal, {}};
        for (unsigned b : lp.body) {
            if (b == hdr) continue;
            const auto& ops = fn.blocks[b].ops;
            for (int oi = 0; oi < static_cast<int>(ops.size()); ++oi) {
                if (b == latch && oi >= iterStorePos) break;
                const auto& op = ops[oi];
                if (op.op != Op::Binary || op.data != "%" || !op.divSafe || !op.unsignedDiv ||
                    op.modNoOp || op.modSubSteps > 0 || op.inputs.size() != 2)
                    continue;
                auto lhsS = loadSlot.find(op.inputs[0].id);
                if (lhsS == loadSlot.end() || lhsS->second != iterSlot) continue;
                if (b == latch && defBlock.count(op.inputs[0].id) &&
                    defBlock[op.inputs[0].id] == latch && defPos[op.inputs[0].id] >= iterStorePos) {
                    continue;
                }
                auto kd = defOp.find(op.inputs[1].id);
                auto kv = (kd != defOp.end() && kd->second->op == Op::Int)
                    ? parseConstInt(kd->second->data) : std::nullopt;
                if (!kv || *kv < 3 || *kv > 64 || ((*kv & (*kv - 1)) == 0) || divKs.contains(*kv)) continue;
                plan.occsByK[*kv].emplace_back(b, oi);
            }
        }
        if (!plan.occsByK.empty()) {
            bool queuedUnroll = false;
            if (lp.body.size() == 2 && hblk.yes == latch && plan.occsByK.size() == 1) {
                const std::int64_t onlyK = plan.occsByK.begin()->first;
                const auto& lOps = fn.blocks[latch].ops;
                if (onlyK >= 3 && ((onlyK <= 7 && lOps.size() <= 96) || (onlyK <= 13 && lOps.size() <= 24)) &&
                    *initVal <= kMaxInt - onlyK) {
                    bool okUnroll = true;
                    for (int oi = 0; oi < static_cast<int>(lOps.size()); ++oi) {
                        const auto& op = lOps[oi];
                        if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Builtin ||
                            op.op == Op::DynCall || op.op == Op::ClosureCall) {
                            okUnroll = false; break;
                        }
                        if (oi > iterStorePos && op.op == Op::Load && op.slot == iterSlot) {
                            okUnroll = false; break;
                        }
                    }
                    const Value upperVal = hCmp->second->inputs[1];
                    auto ur = valRanges.find(upperVal.id);
                    if (ur == valRanges.end() || ur->second.upper > kMaxInt - onlyK) okUnroll = false;
                    std::optional<Operation> upperHdrOp;
                    if (okUnroll && defBlock.count(upperVal.id) && defBlock[upperVal.id] == hdr) {
                        const Operation* uo = defOp.at(upperVal.id);
                        if (uo->op == Op::Int || (uo->op == Op::Load && !loopWritten.contains(uo->slot))) {
                            upperHdrOp = *uo;
                        } else {
                            okUnroll = false;
                        }
                    } else if (okUnroll && defBlock.count(upperVal.id) && lp.body.contains(defBlock[upperVal.id])) {
                        okUnroll = false;
                    }
                    if (okUnroll && incDef->second->data != "rangeIncrement" && !incDef->second->overflowProven)
                        okUnroll = false;
                    if (okUnroll) {
                        std::unordered_set<int> modIdxSet;
                        for (const auto& [mb, mo] : plan.occsByK.begin()->second) {
                            if (mb == latch) modIdxSet.insert(mo);
                        }
                        const Span sp = lOps[plan.occsByK.begin()->second.front().second].span;
                        unrollPlans.push_back({pre, hdr, latch, iterSlot, iterStorePos, *initVal,
                                               onlyK, upperVal, std::move(upperHdrOp), std::move(modIdxSet), sp});
                        queuedUnroll = true;
                    }
                }
            }
            if (!queuedUnroll) plans.push_back(std::move(plan));
        }
    }

    for (const auto& up : unrollPlans) {
        const unsigned hdrUnr = static_cast<unsigned>(fn.blocks.size());
        fn.blocks.emplace_back();
        const unsigned bodyUnr = static_cast<unsigned>(fn.blocks.size());
        fn.blocks.emplace_back();

        Value upperV = up.upperVal;
        if (up.upperHdrOp) {
            Operation uCopy = *up.upperHdrOp;
            uCopy.result.id = fn.nextValue++;
            upperV = uCopy.result;
            fn.blocks[hdrUnr].ops.push_back(std::move(uCopy));
        }
        const Value j0{Type::Int, fn.nextValue++};
        const Value cK{Type::Int, fn.nextValue++};
        const Value jEnd{Type::Int, fn.nextValue++};
        const Value cmp{Type::Bool, fn.nextValue++};
        Operation addEnd{Op::Binary, jEnd, {j0, cK}, "+", 0, up.span};
        addEnd.overflowProven = true;
        addEnd.width = 64;
        Operation cmpOp{Op::Binary, cmp, {jEnd, upperV}, "<=", 0, up.span};
        cmpOp.overflowProven = true;
        cmpOp.width = 64;
        fn.blocks[hdrUnr].ops.push_back({Op::Load, j0, {}, {}, up.iterSlot, up.span});
        fn.blocks[hdrUnr].ops.push_back({Op::Int, cK, {}, std::to_string(up.K), 0, up.span});
        fn.blocks[hdrUnr].ops.push_back(std::move(addEnd));
        fn.blocks[hdrUnr].ops.push_back(std::move(cmpOp));
        fn.blocks[hdrUnr].exit = Exit::Branch;
        fn.blocks[hdrUnr].value = cmp;
        fn.blocks[hdrUnr].yes = bodyUnr;
        fn.blocks[hdrUnr].no = up.hdr;

        const std::vector<Operation> origOps = fn.blocks[up.latch].ops;
        auto& bOps = fn.blocks[bodyUnr].ops;
        const Value jBase{Type::Int, fn.nextValue++};
        bOps.push_back({Op::Load, jBase, {}, {}, up.iterSlot, up.span});
        for (std::int64_t c = 0; c < up.K; ++c) {
            Value jC = jBase;
            if (c > 0) {
                const Value cOff{Type::Int, fn.nextValue++};
                jC = {Type::Int, fn.nextValue++};
                Operation offAdd{Op::Binary, jC, {jBase, cOff}, "+", 0, up.span};
                offAdd.overflowProven = true;
                offAdd.width = 64;
                bOps.push_back({Op::Int, cOff, {}, std::to_string(c), 0, up.span});
                bOps.push_back(std::move(offAdd));
            }
            std::unordered_map<unsigned, Value> remap;
            for (int oi = 0; oi < static_cast<int>(origOps.size()); ++oi) {
                if (oi == up.iterStorePos) continue;
                const Operation& orig = origOps[oi];
                if (up.modIdxSet.contains(oi)) {
                    const Value litV{Type::Int, fn.nextValue++};
                    bOps.push_back({Op::Int, litV, {}, std::to_string((up.initVal + c) % up.K), 0, orig.span});
                    remap[orig.result.id] = litV;
                } else if (orig.op == Op::Load && orig.slot == up.iterSlot) {
                    remap[orig.result.id] = jC;
                } else {
                    Operation nop = orig;
                    for (Value& in : nop.inputs) {
                        if (auto rit = remap.find(in.id); rit != remap.end()) in = rit->second;
                    }
                    const unsigned oldId = nop.result.id;
                    nop.result.id = fn.nextValue++;
                    if (nop.result.type != Type::Void) {
                        remap[oldId] = nop.result;
                    }
                    bOps.push_back(std::move(nop));
                }
            }
        }
        const Value cStep{Type::Int, fn.nextValue++};
        const Value jNext{Type::Int, fn.nextValue++};
        Operation stepAdd{Op::Binary, jNext, {jBase, cStep}, "+", 0, up.span};
        stepAdd.overflowProven = true;
        stepAdd.width = 64;
        bOps.push_back({Op::Int, cStep, {}, std::to_string(up.K), 0, up.span});
        bOps.push_back(std::move(stepAdd));
        bOps.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jNext}, {}, up.iterSlot, up.span});
        fn.blocks[bodyUnr].exit = Exit::Jump;
        fn.blocks[bodyUnr].yes = hdrUnr;
        fn.blocks[up.pre].yes = hdrUnr;
    }

    for (const auto& plan : plans) {
        for (const auto& [K, occs] : plan.occsByK) {
            const unsigned remSlot = static_cast<unsigned>(fn.slots.size());
            fn.slots.push_back(Type::Int);
            if (!fn.slotByRef.empty()) fn.slotByRef.push_back(false);
            if (!fn.slotElemBits.empty()) fn.slotElemBits.push_back(64);
            if (!fn.slotElemUnsigned.empty()) fn.slotElemUnsigned.push_back(false);
            if (!fn.slotAliasGroup.empty()) fn.slotAliasGroup.push_back(-1);

            const Span sp = fn.blocks[occs.front().first].ops[occs.front().second].span;
            const Value initC{Type::Int, fn.nextValue++};
            fn.blocks[plan.pre].ops.push_back({Op::Int, initC, {}, std::to_string(plan.initVal % K), 0, sp});
            fn.blocks[plan.pre].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {initC}, {}, remSlot, sp});

            for (const auto& [bi, oi] : occs) {
                const Operation orig = fn.blocks[bi].ops[oi];
                fn.blocks[bi].ops[oi] = Operation{Op::Load, orig.result, {}, {}, remSlot, orig.span};
            }

            auto& lOps = fn.blocks[plan.latch].ops;
            int stPos = -1;
            for (int oi = 0; oi < static_cast<int>(lOps.size()); ++oi) {
                if (lOps[oi].op == Op::Store && lOps[oi].slot == plan.iterSlot) { stPos = oi; break; }
            }
            if (stPos >= 0) {
                const Span lsp = lOps[stPos].span;
                const Value r0{Type::Int, fn.nextValue++};
                const Value c1{Type::Int, fn.nextValue++};
                const Value r1{Type::Int, fn.nextValue++};
                const Value cK{Type::Int, fn.nextValue++};
                const Value eq{Type::Bool, fn.nextValue++};
                const Value c0{Type::Int, fn.nextValue++};
                const Value nxt{Type::Int, fn.nextValue++};
                Operation addOp{Op::Binary, r1, {r0, c1}, "+", 0, lsp};
                addOp.overflowProven = true;
                addOp.width = 64;
                Operation eqOp{Op::Binary, eq, {r1, cK}, "==", 0, lsp};
                eqOp.overflowProven = true;
                eqOp.width = 64;
                std::vector<Operation> stepOps;
                stepOps.push_back({Op::Load, r0, {}, {}, remSlot, lsp});
                stepOps.push_back({Op::Int, c1, {}, "1", 0, lsp});
                stepOps.push_back(std::move(addOp));
                stepOps.push_back({Op::Int, cK, {}, std::to_string(K), 0, lsp});
                stepOps.push_back(std::move(eqOp));
                stepOps.push_back({Op::Int, c0, {}, "0", 0, lsp});
                stepOps.push_back({Op::Select, nxt, {eq, c0, r1}, {}, 0, lsp});
                stepOps.push_back({Op::Store, {Type::Void, fn.nextValue++}, {nxt}, {}, remSlot, lsp});
                lOps.insert(lOps.begin() + stPos + 1, stepOps.begin(), stepOps.end());
            }
        }
    }
}

/// Replace pure `while (x > 0) { acc += x % K; x /= K; }` with a radix-`B = K^m` LUT loop
/// plus a dynamic `x <= INT32_MAX` 32-bit fast path.
inline void unrollRadixDigitSumLoops(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    const std::size_t origLoopCount = detail::buildCFG(fn).loops.size();
    for (std::size_t attempt = 0; attempt < origLoopCount; ++attempt) {
        const detail::CFGInfo cfg = detail::buildCFG(fn);
        std::unordered_map<unsigned, const Operation*> defOp;
        std::unordered_map<unsigned, unsigned> loadSlot;
        for (const auto& blk : fn.blocks) {
            for (const auto& op : blk.ops) {
                if (op.result.type != Type::Void) defOp[op.result.id] = &op;
                if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
            }
        }
        bool transformed = false;
        for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
            const auto& lp = cfg.loops[li];
            if (lp.body.size() != 2) continue;
            const unsigned hdr = lp.header;
            const auto& hblk = fn.blocks[hdr];
            if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
                continue;
            const unsigned body = hblk.yes;
            const unsigned exitBlk = hblk.no;
            const auto& bblk = fn.blocks[body];
            if (bblk.exit != Exit::Jump || bblk.yes != hdr || cfg.preds[body].size() != 1) continue;
            auto hCmp = defOp.find(hblk.value->id);
            if (hCmp == defOp.end() || hCmp->second->op != Op::Binary || hCmp->second->data != ">") continue;
            auto xLs = loadSlot.find(hCmp->second->inputs[0].id);
            if (xLs == loadSlot.end() ||
                parseConstInt(defOp.count(hCmp->second->inputs[1].id) ? defOp.at(hCmp->second->inputs[1].id)->data : "") != 0)
                continue;
            bool okHdr = true;
            for (const auto& op : hblk.ops) {
                if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary) { okHdr = false; break; }
            }
            if (!okHdr) continue;
            const unsigned xSlot = xLs->second;
            int storeCnt = 0;
            int xStoreIdx = -1, accStoreIdx = -1;
            unsigned accSlot = 0;
            bool okBody = true;
            for (int oi = 0; oi < static_cast<int>(bblk.ops.size()); ++oi) {
                const auto& op = bblk.ops[oi];
                if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary && op.op != Op::Store) {
                    okBody = false; break;
                }
                if (op.op == Op::Store) {
                    ++storeCnt;
                    if (op.slot == xSlot) xStoreIdx = oi;
                    else { accSlot = op.slot; accStoreIdx = oi; }
                }
            }
            if (!okBody || storeCnt != 2 || xStoreIdx < 0 || accStoreIdx < 0 || accSlot == xSlot) continue;
            if (fn.slots[xSlot] != Type::Int || fn.slots[accSlot] != Type::Int) continue;
            const auto& xSt = bblk.ops[xStoreIdx];
            const auto& aSt = bblk.ops[accStoreIdx];
            auto divDef = defOp.find(xSt.inputs[0].id);
            if (divDef == defOp.end() || divDef->second->op != Op::Binary || divDef->second->data != "/" ||
                !divDef->second->divSafe || !divDef->second->unsignedDiv)
                continue;
            auto dxLs = loadSlot.find(divDef->second->inputs[0].id);
            auto kVal = parseConstInt(defOp.count(divDef->second->inputs[1].id)
                                      ? defOp.at(divDef->second->inputs[1].id)->data : "");
            if (dxLs == loadSlot.end() || dxLs->second != xSlot || !kVal || *kVal < 2 || *kVal > 16) continue;
            const std::int64_t K = *kVal;
            const std::int64_t B = (K <= 10) ? (K * K * K) : (K * K);
            const std::string lutName = "digitSumLut_" + std::to_string(K) + "_" + std::to_string(B);

            auto addDef = defOp.find(aSt.inputs[0].id);
            if (addDef == defOp.end() || addDef->second->op != Op::Binary || addDef->second->data != "+" ||
                !addDef->second->overflowProven)
                continue;
            const Operation* remOpPtr = nullptr;
            for (int side = 0; side < 2; ++side) {
                auto als = loadSlot.find(addDef->second->inputs[side].id);
                auto rd = defOp.find(addDef->second->inputs[1 - side].id);
                if (als != loadSlot.end() && als->second == accSlot && rd != defOp.end() &&
                    rd->second->op == Op::Binary && rd->second->data == "%" &&
                    rd->second->divSafe && rd->second->unsignedDiv) {
                    auto rxs = loadSlot.find(rd->second->inputs[0].id);
                    auto rks = parseConstInt(defOp.count(rd->second->inputs[1].id)
                                             ? defOp.at(rd->second->inputs[1].id)->data : "");
                    if (rxs != loadSlot.end() && rxs->second == xSlot && rks == K) {
                        remOpPtr = rd->second; break;
                    }
                }
            }
            if (!remOpPtr) continue;
            int remPos = -1;
            for (int oi = 0; oi < static_cast<int>(bblk.ops.size()); ++oi)
                if (&bblk.ops[oi] == remOpPtr) remPos = oi;
            if (remPos < 0 || remPos > xStoreIdx) continue;

            const unsigned origBlockCount = static_cast<unsigned>(fn.blocks.size());
            const unsigned entryCheck = origBlockCount;
            const unsigned widthCheck = origBlockCount + 1;
            const unsigned loop32Hdr = origBlockCount + 2;
            const unsigned loop32Body = origBlockCount + 3;
            const unsigned loop64Hdr = origBlockCount + 4;
            const unsigned loop64Body = origBlockCount + 5;
            const unsigned tailBlk = origBlockCount + 6;
            const Span sp = bblk.ops[xStoreIdx].span;

            for (unsigned p = 0; p < origBlockCount; ++p) {
                if (lp.body.contains(p)) continue;
                auto& pb = fn.blocks[p];
                if (pb.exit == Exit::Jump && pb.yes == hdr) pb.yes = entryCheck;
                else if (pb.exit == Exit::Branch) {
                    if (pb.yes == hdr) pb.yes = entryCheck;
                    if (pb.no == hdr) pb.no = entryCheck;
                } else if (pb.exit == Exit::Switch) {
                    if (pb.yes == hdr) pb.yes = entryCheck;
                    for (unsigned& t : pb.targets) if (t == hdr) t = entryCheck;
                }
            }
            fn.blocks[hdr].ops.clear();
            fn.blocks[hdr].exit = Exit::Jump;
            fn.blocks[hdr].yes = exitBlk;
            fn.blocks[hdr].no = 0;
            fn.blocks[hdr].value.reset();
            fn.blocks[body].ops.clear();
            fn.blocks[body].exit = Exit::Jump;
            fn.blocks[body].yes = exitBlk;
            fn.blocks[body].no = 0;
            fn.blocks[body].value.reset();

            auto emitCmpBranch = [&](Block& blk, const std::string& cmpStr, std::int64_t rhsVal,
                                     unsigned yBlk, unsigned nBlk) {
                const Value lx{Type::Int, fn.nextValue++};
                const Value c{Type::Int, fn.nextValue++};
                const Value cmpV{Type::Bool, fn.nextValue++};
                blk.ops.push_back({Op::Load, lx, {}, {}, xSlot, sp});
                blk.ops.push_back({Op::Int, c, {}, std::to_string(rhsVal), 0, sp});
                Operation cmpOp{Op::Binary, cmpV, {lx, c}, cmpStr, 0, sp};
                cmpOp.overflowProven = true;
                blk.ops.push_back(std::move(cmpOp));
                blk.exit = Exit::Branch;
                blk.value = cmpV;
                blk.yes = yBlk;
                blk.no = nBlk;
            };
            auto emitStepBody = [&](Block& blk, std::uint8_t widthBits, unsigned hdrTarget) {
                const Value lx{Type::Int, fn.nextValue++};
                const Value cB{Type::Int, fn.nextValue++};
                const Value q{Type::Int, fn.nextValue++};
                const Value qMul{Type::Int, fn.nextValue++};
                const Value rem{Type::Int, fn.nextValue++};
                const Value ds{Type::Int, fn.nextValue++};
                const Value oldAcc{Type::Int, fn.nextValue++};
                const Value newAcc{Type::Int, fn.nextValue++};
                blk.ops.push_back({Op::Load, lx, {}, {}, xSlot, sp});
                blk.ops.push_back({Op::Int, cB, {}, std::to_string(B), 0, sp});
                Operation divOp{Op::Binary, q, {lx, cB}, "/", 0, sp};
                divOp.divSafe = true; divOp.unsignedDiv = true; divOp.overflowProven = true; divOp.width = widthBits;
                Operation mulOp{Op::Binary, qMul, {q, cB}, "*", 0, sp};
                mulOp.overflowProven = true; mulOp.width = widthBits;
                Operation subOp{Op::Binary, rem, {lx, qMul}, "-", 0, sp};
                subOp.overflowProven = true; subOp.width = widthBits;
                Operation lutOp{Op::Builtin, ds, {rem}, lutName, 0, sp};
                Operation addOp{Op::Binary, newAcc, {oldAcc, ds}, "+", 0, sp};
                addOp.overflowProven = true; addOp.width = 64;
                blk.ops.push_back(std::move(divOp));
                blk.ops.push_back(std::move(mulOp));
                blk.ops.push_back(std::move(subOp));
                blk.ops.push_back(std::move(lutOp));
                blk.ops.push_back({Op::Load, oldAcc, {}, {}, accSlot, sp});
                blk.ops.push_back(std::move(addOp));
                blk.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {newAcc}, {}, accSlot, sp});
                blk.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {q}, {}, xSlot, sp});
                blk.exit = Exit::Jump;
                blk.yes = hdrTarget;
            };

            Block ecBlk, wcBlk, h32Blk, b32Blk, h64Blk, b64Blk, tBlk;
            emitCmpBranch(ecBlk, ">", 0, widthCheck, exitBlk);
            emitCmpBranch(wcBlk, "<=", 2147483647LL, loop32Hdr, loop64Hdr);
            emitCmpBranch(h32Blk, ">=", B, loop32Body, tailBlk);
            emitStepBody(b32Blk, 32, loop32Hdr);
            emitCmpBranch(h64Blk, ">=", B, loop64Body, tailBlk);
            emitStepBody(b64Blk, 64, loop64Hdr);

            const Value xt{Type::Int, fn.nextValue++};
            const Value dst{Type::Int, fn.nextValue++};
            const Value at{Type::Int, fn.nextValue++};
            const Value newAt{Type::Int, fn.nextValue++};
            const Value zt{Type::Int, fn.nextValue++};
            tBlk.ops.push_back({Op::Load, xt, {}, {}, xSlot, sp});
            tBlk.ops.push_back({Op::Builtin, dst, {xt}, lutName, 0, sp});
            tBlk.ops.push_back({Op::Load, at, {}, {}, accSlot, sp});
            Operation tAdd{Op::Binary, newAt, {at, dst}, "+", 0, sp};
            tAdd.overflowProven = true; tAdd.width = 64;
            tBlk.ops.push_back(std::move(tAdd));
            tBlk.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {newAt}, {}, accSlot, sp});
            tBlk.ops.push_back({Op::Int, zt, {}, "0", 0, sp});
            tBlk.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {zt}, {}, xSlot, sp});
            tBlk.exit = Exit::Jump;
            tBlk.yes = exitBlk;

            fn.blocks.push_back(std::move(ecBlk));
            fn.blocks.push_back(std::move(wcBlk));
            fn.blocks.push_back(std::move(h32Blk));
            fn.blocks.push_back(std::move(b32Blk));
            fn.blocks.push_back(std::move(h64Blk));
            fn.blocks.push_back(std::move(b64Blk));
            fn.blocks.push_back(std::move(tBlk));
            transformed = true;
            break;
        }
        if (!transformed) break;
    }
}

/// Memoize pure, trap-free inner loops whose net effect is an additive reduction depending on
/// a single bounded scalar key in `[lo, hi]` (`1 <= hi - lo <= 63`).
inline void memoizePureSmallDomainLoops(FunctionIR& fn,
                                        const std::unordered_map<unsigned, IntRange>& valRanges) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> defBlock;
    std::unordered_map<unsigned, unsigned> loadSlot;
    for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const auto& op : fn.blocks[bi].ops) {
            if (op.result.type != Type::Void) {
                defOp[op.result.id] = &op;
                defBlock[op.result.id] = bi;
            }
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
        }
    }

    for (std::size_t inLi = 0; inLi < cfg.loops.size(); ++inLi) {
        const auto& lpIn = cfg.loops[inLi];
        if (lpIn.preheaders.size() != 1) continue;
        const unsigned pre = lpIn.preheaders[0];
        const unsigned inHdr = lpIn.header;
        if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != inHdr) continue;
        if (cfg.loopOfBlock[pre] < 0) continue;
        const std::size_t outLi = static_cast<std::size_t>(cfg.loopOfBlock[pre]);
        const auto& lpOut = cfg.loops[outLi];
        if (lpOut.preheaders.size() != 1) continue;
        const unsigned outerPre = lpOut.preheaders[0];
        if (lpOut.body.contains(outerPre) || fn.blocks[outerPre].exit != Exit::Jump) continue;

        const auto& ihb = fn.blocks[inHdr];
        if (ihb.exit != Exit::Branch || lpIn.body.contains(ihb.no) || !lpIn.body.contains(ihb.yes)) continue;
        const unsigned inExit = ihb.no;
        bool singleExit = true;
        for (unsigned b : lpIn.body) {
            const auto& blk = fn.blocks[b];
            for (unsigned su : cfg.succs[b]) {
                if (!lpIn.body.contains(su) && (b != inHdr || su != inExit)) { singleExit = false; break; }
            }
            if (!singleExit) break;
            if (blk.exit == Exit::Return || blk.exit == Exit::Unreachable || blk.exit == Exit::Switch) {
                singleExit = false; break;
            }
        }
        if (!singleExit) continue;

        bool pureIn = true;
        std::unordered_set<unsigned> inWritten, inRead;
        for (unsigned b : lpIn.body) {
            if (cfg.loopOfBlock[b] != static_cast<int>(inLi)) { pureIn = false; break; }
            for (const auto& op : fn.blocks[b].ops) {
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin ||
                    op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                    op.op == Op::Print || op.op == Op::Index || op.op == Op::IndexSet || op.op == Op::Length ||
                    op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice || op.op == Op::Construct ||
                    op.op == Op::Field || op.op == Op::FieldSet || op.op == Op::PlaceSet || op.op == Op::Tag ||
                    op.op == Op::RepeatHeap || op.op == Op::RepeatRegion || op.op == Op::Clone ||
                    op.op == Op::Drop || op.op == Op::ClearOwner || op.op == Op::ClearField || op.op == Op::ClearIndex ||
                    op.op == Op::ReadByte || (op.op == Op::Assert && !op.boundsProven && !op.unchecked)) {
                    pureIn = false; break;
                }
                if ((op.op == Op::Negate || op.op == Op::AbsDiff) && op.result.type == Type::Int && !op.overflowProven) {
                    pureIn = false; break;
                }
                if (op.op == Op::Binary && op.inputs[0].type == Type::Int) {
                    if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                         op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) {
                        pureIn = false; break;
                    }
                    if ((op.data == "/" || op.data == "%") && !op.divSafe) { pureIn = false; break; }
                }
                if (op.op == Op::Store) inWritten.insert(op.slot);
                if (op.op == Op::Load) inRead.insert(op.slot);
            }
            if (!pureIn) break;
        }
        if (!pureIn || inWritten.empty()) continue;

        // Find live-out slots among inWritten reachable from inExit without first being overwritten
        std::unordered_set<unsigned> liveOutSlots;
        for (unsigned s : inWritten) {
            std::vector<unsigned> stack{inExit};
            std::unordered_set<unsigned> vis;
            bool isLive = false;
            while (!stack.empty() && !isLive) {
                const unsigned curB = stack.back(); stack.pop_back();
                if (!vis.insert(curB).second) continue;
                bool overwritten = false;
                for (const auto& op : fn.blocks[curB].ops) {
                    if (op.op == Op::Load && op.slot == s) { isLive = true; break; }
                    if (op.op == Op::Store && op.slot == s) { overwritten = true; break; }
                }
                if (!isLive && !overwritten) {
                    for (unsigned su : cfg.succs[curB]) {
                        if (!lpIn.body.contains(su)) stack.push_back(su);
                    }
                }
            }
            if (isLive) liveOutSlots.insert(s);
        }
        if (liveOutSlots.size() != 1) continue;
        const unsigned accSlot = *liveOutSlots.begin();
        if (accSlot >= fn.slots.size() || fn.slots[accSlot] != Type::Int) continue;

        // Verify all stores to accSlot in lpIn.body are additive increments whose step does not depend on accSlot
        bool okAcc = true;
        int accWrites = 0;
        for (unsigned b : lpIn.body) {
            for (const auto& op : fn.blocks[b].ops) {
                if (op.op != Op::Store || op.slot != accSlot) continue;
                ++accWrites;
                if (op.inputs.empty()) { okAcc = false; break; }
                auto d = defOp.find(op.inputs[0].id);
                if (d == defOp.end() || d->second->op != Op::Binary || d->second->data != "+" ||
                    !d->second->overflowProven) {
                    okAcc = false; break;
                }
                auto ls = loadSlot.find(d->second->inputs[0].id);
                if (ls == loadSlot.end() || ls->second != accSlot) { okAcc = false; break; }
                std::unordered_set<unsigned> visV;
                auto readsAcc = [&](auto& self, unsigned vid, int dep) -> bool {
                    if (dep > 24 || !visV.insert(vid).second) return true;
                    auto it = defOp.find(vid);
                    if (it == defOp.end()) return false;
                    if (it->second->op == Op::Load) return it->second->slot == accSlot;
                    for (const auto& in : it->second->inputs) if (self(self, in.id, dep + 1)) return true;
                    return false;
                };
                if (readsAcc(readsAcc, d->second->inputs[1].id, 0)) { okAcc = false; break; }
            }
            if (!okAcc) break;
        }
        if (!okAcc || accWrites == 0) continue;

        // Collect all external Int dependencies needed by lpIn.body (via upward-exposed slots & cross-block inputs)
        std::unordered_set<unsigned> depExternalSlots;
        bool okDeps = true;
        std::unordered_set<unsigned> visDeps;
        std::function<void(Value, int)> traceVal;
        traceVal = [&](Value v, int depth) {
            if (!okDeps || depth > 24 || !visDeps.insert(v.id).second) return;
            auto it = defOp.find(v.id);
            if (it == defOp.end()) { okDeps = false; return; }
            const Operation* op = it->second;
            if (op->op == Op::Int || op->op == Op::Bool) return;
            if (op->op == Op::Load) {
                const unsigned s = op->slot;
                if (s == accSlot) return;
                if (inWritten.contains(s)) {
                    // Must be initialized in `pre` cleanly (no dependency outside constants/key)
                    int preSt = 0;
                    const Operation* lastPreSt = nullptr;
                    for (const auto& pop : fn.blocks[pre].ops) {
                        if (pop.op == Op::Store && pop.slot == s) { ++preSt; lastPreSt = &pop; }
                    }
                    if (preSt == 0 || !lastPreSt || lastPreSt->inputs.empty()) { okDeps = false; return; }
                    traceVal(lastPreSt->inputs[0], depth + 1);
                } else {
                    depExternalSlots.insert(s);
                }
                return;
            }
            if (op->op != Op::Binary && op->op != Op::Negate && op->op != Op::AbsDiff &&
                op->op != Op::BitNot && op->op != Op::Not &&
                op->op != Op::Min && op->op != Op::Max && op->op != Op::Select) {
                okDeps = false; return;
            }
            for (const auto& in : op->inputs) traceVal(in, depth + 1);
        };
        for (unsigned b : lpIn.body) {
            for (const auto& op : fn.blocks[b].ops) {
                if (op.op == Op::Load) {
                    if (op.slot != accSlot && !inWritten.contains(op.slot)) depExternalSlots.insert(op.slot);
                }
                for (const auto& in : op.inputs) {
                    if (defBlock.count(in.id) && !lpIn.body.contains(defBlock[in.id])) {
                        traceVal(in, 0);
                    }
                }
            }
            if (fn.blocks[b].exit == Exit::Branch && fn.blocks[b].value &&
                defBlock.count(fn.blocks[b].value->id) && !lpIn.body.contains(defBlock[fn.blocks[b].value->id])) {
                traceVal(*fn.blocks[b].value, 0);
            }
        }
        // Check initial values in `pre` for all inWritten slots != accSlot
        for (unsigned s : inWritten) {
            if (s == accSlot) continue;
            bool inInit = false;
            for (const auto& pop : fn.blocks[pre].ops) {
                if (pop.op == Op::Store && pop.slot == s && !pop.inputs.empty()) {
                    inInit = true;
                    traceVal(pop.inputs[0], 0);
                }
            }
            (void)inInit;
        }
        if (!okDeps || depExternalSlots.size() != 1) continue;
        const unsigned keySlot = *depExternalSlots.begin();
        if (keySlot >= fn.slots.size() || fn.slots[keySlot] != Type::Int || inWritten.contains(keySlot)) continue;

        std::optional<IntRange> keyRange;
        for (const auto& [vid, slot] : loadSlot) {
            if (slot != keySlot) continue;
            if (auto bIt = defBlock.find(vid); bIt != defBlock.end() &&
                (bIt->second == pre || lpIn.body.contains(bIt->second))) {
                auto vr = valRanges.find(vid);
                if (vr == valRanges.end()) { keyRange.reset(); break; }
                keyRange = keyRange ? ReturnRanges::envelope(*keyRange, vr->second) : vr->second;
            }
        }
        if (!keyRange || keyRange->lower < 0 || keyRange->upper > 65536 ||
            keyRange->upper <= keyRange->lower || keyRange->upper - keyRange->lower > 63)
            continue;

        const std::int64_t kLo = keyRange->lower;
        const std::int64_t kSpan = keyRange->upper - keyRange->lower + 1;
        const Span sp = ihb.ops.empty() ? Span{} : ihb.ops.front().span;

        if (!fn.usesRegion) {
            const Value markV{Type::Void, fn.nextValue++};
            fn.blocks[0].ops.insert(fn.blocks[0].ops.begin(), {Op::RegionMark, markV, {}, {}, 0, sp});
            for (auto& blk : fn.blocks) {
                if (blk.exit == Exit::Return) {
                    blk.ops.push_back({Op::RegionRewind, {Type::Void, fn.nextValue++}, {markV}, {}, 0, sp});
                }
            }
            fn.usesRegion = true;
        }

        auto addSlot = [&](Type t, std::uint8_t bits, bool uns) -> unsigned {
            const unsigned sid = static_cast<unsigned>(fn.slots.size());
            fn.slots.push_back(t);
            if (!fn.slotByRef.empty()) fn.slotByRef.push_back(false);
            if (!fn.slotElemBits.empty()) fn.slotElemBits.push_back(bits);
            if (!fn.slotElemUnsigned.empty()) fn.slotElemUnsigned.push_back(uns);
            if (!fn.slotAliasGroup.empty()) fn.slotAliasGroup.push_back(-1);
            return sid;
        };
        const unsigned lutSlot = addSlot(Type::IntArray, 64, false);
        const unsigned validSlot = addSlot(Type::IntArray, 8, true);
        const unsigned beforeSlot = addSlot(Type::Int, 64, false);

        // Allocate lut and valid in outerPre (after RegionMark if outerPre == 0)
        {
            const Value z1{Type::Int, fn.nextValue++};
            const Value n1{Type::Int, fn.nextValue++};
            const Value lArr{Type::IntArray, fn.nextValue++};
            Operation repLut{Op::RepeatRegion, lArr, {z1, n1}, {}, 0, sp};
            repLut.initialize = true;
            repLut.elemBits = 64;
            repLut.elemUnsigned = false;
            const Value z2{Type::Int, fn.nextValue++};
            const Value n2{Type::Int, fn.nextValue++};
            const Value vArr{Type::IntArray, fn.nextValue++};
            Operation repVal{Op::RepeatRegion, vArr, {z2, n2}, {}, 0, sp};
            repVal.initialize = true;
            repVal.elemBits = 8;
            repVal.elemUnsigned = true;
            auto& pOps = fn.blocks[outerPre].ops;
            pOps.push_back({Op::Int, z1, {}, "0", 0, sp});
            pOps.push_back({Op::Int, n1, {}, std::to_string(kSpan), 0, sp});
            pOps.push_back(repLut);
            pOps.push_back({Op::Store, {Type::Void, fn.nextValue++}, {lArr}, {}, lutSlot, sp});
            pOps.push_back({Op::Int, z2, {}, "0", 0, sp});
            pOps.push_back({Op::Int, n2, {}, std::to_string(kSpan), 0, sp});
            pOps.push_back(repVal);
            pOps.push_back({Op::Store, {Type::Void, fn.nextValue++}, {vArr}, {}, validSlot, sp});
        }

        const unsigned hitBlk = static_cast<unsigned>(fn.blocks.size());
        const unsigned missPreBlk = hitBlk + 1;
        const unsigned afterExitBlk = hitBlk + 2;

        // Preheader check
        {
            auto& preOps = fn.blocks[pre].ops;
            const Value kv{Type::Int, fn.nextValue++};
            const Value loC{Type::Int, fn.nextValue++};
            const Value idxV{Type::Int, fn.nextValue++};
            const Value va{Type::IntArray, fn.nextValue++};
            const Value flg{Type::Int, fn.nextValue++};
            const Value zV{Type::Int, fn.nextValue++};
            const Value isHit{Type::Bool, fn.nextValue++};
            preOps.push_back({Op::Load, kv, {}, {}, keySlot, sp});
            preOps.push_back({Op::Int, loC, {}, std::to_string(kLo), 0, sp});
            Operation subIdx{Op::Binary, idxV, {kv, loC}, "-", 0, sp};
            subIdx.overflowProven = true;
            subIdx.width = 32;
            preOps.push_back(subIdx);
            preOps.push_back({Op::Load, va, {}, {}, validSlot, sp});
            Operation ldFlag{Op::Index, flg, {va, idxV}, {}, 0, sp};
            ldFlag.boundsProven = true;
            ldFlag.elemBits = 8;
            ldFlag.elemUnsigned = true;
            preOps.push_back(ldFlag);
            preOps.push_back({Op::Int, zV, {}, "0", 0, sp});
            Operation cmpHit{Op::Binary, isHit, {flg, zV}, "!=", 0, sp};
            cmpHit.width = 16;
            preOps.push_back(cmpHit);
            fn.blocks[pre].exit = Exit::Branch;
            fn.blocks[pre].value = isHit;
            fn.blocks[pre].yes = hitBlk;
            fn.blocks[pre].no = missPreBlk;
        }

        Block hb, mb, ab;
        // hitBlk: acc += lut[key - lo]; Jump -> inExit
        {
            const Value kv{Type::Int, fn.nextValue++};
            const Value loC{Type::Int, fn.nextValue++};
            const Value idxV{Type::Int, fn.nextValue++};
            const Value la{Type::IntArray, fn.nextValue++};
            const Value dv{Type::Int, fn.nextValue++};
            const Value oa{Type::Int, fn.nextValue++};
            const Value na{Type::Int, fn.nextValue++};
            hb.ops.push_back({Op::Load, kv, {}, {}, keySlot, sp});
            hb.ops.push_back({Op::Int, loC, {}, std::to_string(kLo), 0, sp});
            Operation subIdx{Op::Binary, idxV, {kv, loC}, "-", 0, sp};
            subIdx.overflowProven = true;
            subIdx.width = 32;
            hb.ops.push_back(subIdx);
            hb.ops.push_back({Op::Load, la, {}, {}, lutSlot, sp});
            Operation ldLut{Op::Index, dv, {la, idxV}, {}, 0, sp};
            ldLut.boundsProven = true;
            hb.ops.push_back(ldLut);
            hb.ops.push_back({Op::Load, oa, {}, {}, accSlot, sp});
            Operation addAcc{Op::Binary, na, {oa, dv}, "+", 0, sp};
            addAcc.overflowProven = true;
            hb.ops.push_back(addAcc);
            hb.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {na}, {}, accSlot, sp});
            hb.exit = Exit::Jump;
            hb.yes = inExit;
        }
        // missPreBlk: before = acc; Jump -> inHdr
        {
            const Value oa{Type::Int, fn.nextValue++};
            mb.ops.push_back({Op::Load, oa, {}, {}, accSlot, sp});
            mb.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {oa}, {}, beforeSlot, sp});
            mb.exit = Exit::Jump;
            mb.yes = inHdr;
        }
        // afterExitBlk: lut[key - lo] = acc - before; valid[key - lo] = 1; Jump -> inExit
        {
            const Value kv{Type::Int, fn.nextValue++};
            const Value loC{Type::Int, fn.nextValue++};
            const Value idxV{Type::Int, fn.nextValue++};
            const Value na{Type::Int, fn.nextValue++};
            const Value ba{Type::Int, fn.nextValue++};
            const Value dV{Type::Int, fn.nextValue++};
            const Value la{Type::IntArray, fn.nextValue++};
            const Value va{Type::IntArray, fn.nextValue++};
            const Value oneC{Type::Int, fn.nextValue++};
            ab.ops.push_back({Op::Load, kv, {}, {}, keySlot, sp});
            ab.ops.push_back({Op::Int, loC, {}, std::to_string(kLo), 0, sp});
            Operation subIdx{Op::Binary, idxV, {kv, loC}, "-", 0, sp};
            subIdx.overflowProven = true;
            subIdx.width = 32;
            ab.ops.push_back(subIdx);
            ab.ops.push_back({Op::Load, na, {}, {}, accSlot, sp});
            ab.ops.push_back({Op::Load, ba, {}, {}, beforeSlot, sp});
            Operation subD{Op::Binary, dV, {na, ba}, "-", 0, sp};
            subD.overflowProven = true;
            ab.ops.push_back(subD);
            ab.ops.push_back({Op::Load, la, {}, {}, lutSlot, sp});
            Operation stLut{Op::IndexSet, {Type::Void, fn.nextValue++}, {la, idxV, dV}, {}, 0, sp};
            stLut.boundsProven = true;
            ab.ops.push_back(stLut);
            ab.ops.push_back({Op::Load, va, {}, {}, validSlot, sp});
            ab.ops.push_back({Op::Int, oneC, {}, "1", 0, sp});
            Operation stVal{Op::IndexSet, {Type::Void, fn.nextValue++}, {va, idxV, oneC}, {}, 0, sp};
            stVal.boundsProven = true;
            stVal.elemBits = 8;
            stVal.elemUnsigned = true;
            ab.ops.push_back(stVal);
            ab.exit = Exit::Jump;
            ab.yes = inExit;
        }
        fn.blocks[inHdr].no = afterExitBlk;
        fn.blocks.push_back(std::move(hb));
        fn.blocks.push_back(std::move(mb));
        fn.blocks.push_back(std::move(ab));
        break; // Recompute CFG cleanly on multiple candidates if ever needed
    }
}

/// Elide redundant Repeat initialize=true on dense prefix DP initialization and promote
/// loop-invariant `arr[i]` read/write accumulators when all other reads use `arr[i - c]` (`c >= 1`).
inline void promoteDisjointIndexedAccum(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    // 1. Dead-Init Elision
    {
        const detail::CFGInfo cfg = detail::buildCFG(fn);
        std::unordered_map<unsigned, const Operation*> defOp;
        std::unordered_map<unsigned, unsigned> loadSlot;
        for (const auto& blk : fn.blocks) {
            for (const auto& op : blk.ops) {
                if (op.result.type != Type::Void) defOp[op.result.id] = &op;
                if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
            }
        }
        for (std::size_t s = 0; s < fn.slots.size(); ++s) {
            if (fn.slots[s] != Type::IntArray) continue;
            int repStores = 0;
            int repBlk = -1;
            std::size_t repIdx = 0;
            Operation* repPtr = nullptr;
            bool okArr = true;
            for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
                for (std::size_t oi = 0; oi < fn.blocks[bi].ops.size(); ++oi) {
                    auto& op = fn.blocks[bi].ops[oi];
                    if (op.op == Op::Store && op.slot == s && !op.inputs.empty()) {
                        auto d = defOp.find(op.inputs[0].id);
                        if (d != defOp.end() && (d->second->op == Op::RepeatRegion || d->second->op == Op::RepeatHeap) &&
                            d->second->initialize) {
                            ++repStores;
                            if (repBlk < 0) {
                                repBlk = static_cast<int>(bi);
                                repIdx = oi;
                                // locate actual op pointer for Repeat
                                for (auto& rop : fn.blocks[bi].ops)
                                    if (rop.result.id == op.inputs[0].id) repPtr = &rop;
                            }
                        } else {
                            okArr = false;
                        }
                    }
                }
            }
            if (!okArr || repStores != 1 || repBlk != 0 || !repPtr || fn.blocks[0].exit != Exit::Jump) continue;
            const unsigned hdr = fn.blocks[0].yes;
            auto lit = cfg.headerToLoop.find(hdr);
            if (lit == cfg.headerToLoop.end()) continue;
            const auto& hblk = fn.blocks[hdr];
            if (hblk.exit != Exit::Branch || !hblk.value || !cfg.loops[lit->second].body.contains(hblk.yes)) continue;
            auto cmp = defOp.find(hblk.value->id);
            if (cmp == defOp.end() || cmp->second->op != Op::Binary || cmp->second->data != "<") continue;
            auto iterLs = loadSlot.find(cmp->second->inputs[0].id);
            if (iterLs == loadSlot.end()) continue;
            const unsigned iterS = iterLs->second;
            const Value repCnt = repPtr->inputs[1];
            const Value loopUpper = cmp->second->inputs[1];
            auto sameLenExpr = [&](Value a, Value b) -> bool {
                auto ad = defOp.find(a.id), bd = defOp.find(b.id);
                if (ad == defOp.end() || bd == defOp.end()) return false;
                if (ad->second->op == Op::Load && bd->second->op == Op::Load) return ad->second->slot == bd->second->slot;
                if (ad->second->op == Op::Binary && bd->second->op == Op::Binary && ad->second->data == bd->second->data) {
                    auto l0 = loadSlot.find(ad->second->inputs[0].id), l1 = loadSlot.find(bd->second->inputs[0].id);
                    auto r0 = parseConstInt(defOp.count(ad->second->inputs[1].id) ? defOp.at(ad->second->inputs[1].id)->data : "");
                    auto r1 = parseConstInt(defOp.count(bd->second->inputs[1].id) ? defOp.at(bd->second->inputs[1].id)->data : "");
                    return l0 != loadSlot.end() && l1 != loadSlot.end() && l0->second == l1->second && r0 && r1 && *r0 == *r1;
                }
                return false;
            };
            if (!sameLenExpr(repCnt, loopUpper)) continue;
            std::optional<std::int64_t> loInit;
            bool arr0Written = false, readBeforeLoop = false;
            for (std::size_t oi = repIdx + 1; oi < fn.blocks[0].ops.size(); ++oi) {
                const auto& op = fn.blocks[0].ops[oi];
                if (op.op == Op::Store && op.slot == iterS && !op.inputs.empty()) {
                    loInit = parseConstInt(defOp.count(op.inputs[0].id) ? defOp.at(op.inputs[0].id)->data : "");
                } else if (op.op == Op::IndexSet && !op.inputs.empty()) {
                    auto als = loadSlot.find(op.inputs[0].id);
                    auto c0 = parseConstInt(defOp.count(op.inputs[1].id) ? defOp.at(op.inputs[1].id)->data : "");
                    if (als != loadSlot.end() && als->second == s) {
                        if (c0 == 0 && !arr0Written) arr0Written = true;
                        else readBeforeLoop = true;
                    }
                } else if (op.op == Op::Index && !op.inputs.empty()) {
                    auto als = loadSlot.find(op.inputs[0].id);
                    if (als != loadSlot.end() && als->second == s) readBeforeLoop = true;
                }
            }
            if (!arr0Written || readBeforeLoop || loInit != 1) continue;
            // Verify hblk.yes unconditionally writes arr[iterS] BEFORE any read other than arr[iterS - c] (c >= 1)
            const unsigned bodyB = hblk.yes;
            bool firstSetAtIter = false, okLoopBody = true;
            for (const auto& op : fn.blocks[bodyB].ops) {
                if (op.op == Op::Index && !op.inputs.empty()) {
                    auto als = loadSlot.find(op.inputs[0].id);
                    if (als != loadSlot.end() && als->second == s && !firstSetAtIter) {
                        auto idDef = defOp.find(op.inputs[1].id);
                        if (idDef == defOp.end() || idDef->second->op != Op::Binary || idDef->second->data != "-") {
                            okLoopBody = false; break;
                        }
                        auto ls0 = loadSlot.find(idDef->second->inputs[0].id);
                        auto rc = parseConstInt(defOp.count(idDef->second->inputs[1].id)
                                                ? defOp.at(idDef->second->inputs[1].id)->data : "");
                        if (ls0 == loadSlot.end() || ls0->second != iterS || !rc || *rc < 1) {
                            okLoopBody = false; break;
                        }
                    }
                } else if (op.op == Op::IndexSet && !op.inputs.empty()) {
                    auto als = loadSlot.find(op.inputs[0].id);
                    if (als != loadSlot.end() && als->second == s && !firstSetAtIter) {
                        auto ils = loadSlot.find(op.inputs[1].id);
                        if (ils != loadSlot.end() && ils->second == iterS && op.boundsProven) {
                            firstSetAtIter = true;
                        } else { okLoopBody = false; break; }
                    }
                }
            }
            if (okLoopBody && firstSetAtIter) repPtr->initialize = false;
        }
    }

    // 2. Inner-Loop Scalar Promotion of Disjoint `arr[i]`
    for (;;) {
        const detail::CFGInfo cfg = detail::buildCFG(fn);
        std::unordered_map<unsigned, const Operation*> defOp;
        std::unordered_map<unsigned, unsigned> loadSlot;
        std::vector<std::unordered_set<unsigned>> loopWritten(cfg.loops.size());
        for (unsigned bi = 0; bi < fn.blocks.size(); ++bi) {
            for (const auto& op : fn.blocks[bi].ops) {
                if (op.result.type != Type::Void) defOp[op.result.id] = &op;
                if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
                if (op.op == Op::Store && cfg.loopOfBlock[bi] >= 0) {
                    for (std::size_t k = 0; k < cfg.loops.size(); ++k)
                        if (cfg.loops[k].body.contains(bi)) loopWritten[k].insert(op.slot);
                }
            }
        }
        bool promoted = false;
        for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
            const auto& lp = cfg.loops[li];
            if (lp.preheaders.size() != 1) continue;
            const unsigned hdr = lp.header;
            const unsigned pre = lp.preheaders[0];
            if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != hdr) continue;
            const auto& hblk = fn.blocks[hdr];
            if (hblk.exit != Exit::Branch || lp.body.contains(hblk.no) || !lp.body.contains(hblk.yes)) continue;
            const unsigned exitB = hblk.no;
            bool innerOnly = true;
            for (unsigned b : lp.body) {
                if (cfg.loopOfBlock[b] != static_cast<int>(li)) { innerOnly = false; break; }
                for (unsigned su : cfg.succs[b])
                    if (!lp.body.contains(su) && (b != hdr || su != exitB)) innerOnly = false;
            }
            if (!innerOnly) continue;

            // Find candidate (arrSlot, idxSlot)
            std::unordered_map<unsigned, std::unordered_set<unsigned>> setIdxs;
            bool safeOps = true;
            for (unsigned b : lp.body) {
                for (const auto& op : fn.blocks[b].ops) {
                    if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall ||
                        op.op == Op::ClosureCall || op.op == Op::ClearIndex || op.op == Op::ClearOwner) {
                        safeOps = false; break;
                    }
                    if (op.op == Op::IndexSet && op.inputs.size() >= 3) {
                        if (!op.boundsProven) { safeOps = false; break; }
                        auto als = loadSlot.find(op.inputs[0].id);
                        auto ils = loadSlot.find(op.inputs[1].id);
                        if (als == loadSlot.end() || ils == loadSlot.end()) { safeOps = false; break; }
                        setIdxs[als->second].insert(ils->second);
                    }
                }
                if (!safeOps) break;
            }
            if (!safeOps || setIdxs.empty()) continue;

            for (const auto& [arrSlot, idxs] : setIdxs) {
                if (idxs.size() != 1 || loopWritten[li].contains(arrSlot)) continue;
                const unsigned iSlot = *idxs.begin();
                if (loopWritten[li].contains(iSlot)) continue;

                bool allDisjoint = true;
                int writeCount = 0;
                std::uint8_t elemBits = 64;
                bool elemUnsigned = false;
                for (unsigned b : lp.body) {
                    for (const auto& op : fn.blocks[b].ops) {
                        if ((op.op == Op::Index || op.op == Op::IndexSet) && !op.inputs.empty()) {
                            auto als = loadSlot.find(op.inputs[0].id);
                            if (als == loadSlot.end() || als->second != arrSlot) continue;
                            if (!op.boundsProven) { allDisjoint = false; break; }
                            elemBits = op.elemBits;
                            elemUnsigned = op.elemUnsigned;
                            if (op.op == Op::IndexSet) {
                                ++writeCount;
                                continue;
                            }
                            auto ils = loadSlot.find(op.inputs[1].id);
                            if (ils != loadSlot.end() && ils->second == iSlot) continue;
                            // Must be iSlot - positive offset (> 0)
                            auto idDef = defOp.find(op.inputs[1].id);
                            if (idDef == defOp.end() || idDef->second->op != Op::Binary || idDef->second->data != "-") {
                                allDisjoint = false; break;
                            }
                            auto lhs = loadSlot.find(idDef->second->inputs[0].id);
                            if (lhs == loadSlot.end() || lhs->second != iSlot) { allDisjoint = false; break; }
                            auto checkPos = [&](auto& self, Value v, int d) -> bool {
                                if (d > 6) return false;
                                auto dv = defOp.find(v.id);
                                if (dv == defOp.end()) return false;
                                if (dv->second->op == Op::Int) {
                                    auto c = parseConstInt(dv->second->data);
                                    return c && *c >= 1;
                                }
                                if (dv->second->op == Op::Binary && dv->second->data == "+") {
                                    auto r = parseConstInt(defOp.count(dv->second->inputs[1].id)
                                                           ? defOp.at(dv->second->inputs[1].id)->data : "");
                                    auto l = parseConstInt(defOp.count(dv->second->inputs[0].id)
                                                           ? defOp.at(dv->second->inputs[0].id)->data : "");
                                    if (r && *r >= 1) return true;
                                    if (l && *l >= 1) return true;
                                }
                                if (dv->second->op == Op::Load) {
                                    for (unsigned b2 : lp.body) {
                                        for (const auto& op2 : fn.blocks[b2].ops) {
                                            if (op2.op == Op::Store && op2.slot == dv->second->slot && !op2.inputs.empty()) {
                                                if (!self(self, op2.inputs[0], d + 1)) return false;
                                            }
                                        }
                                    }
                                    return true;
                                }
                                return false;
                            };
                            if (!checkPos(checkPos, idDef->second->inputs[1], 0)) { allDisjoint = false; break; }
                        }
                    }
                    if (!allDisjoint) break;
                }
                if (!allDisjoint || writeCount == 0) continue;

                const unsigned scSlot = static_cast<unsigned>(fn.slots.size());
                fn.slots.push_back(Type::Int);
                if (!fn.slotByRef.empty()) fn.slotByRef.push_back(false);
                if (!fn.slotElemBits.empty()) fn.slotElemBits.push_back(64);
                if (!fn.slotElemUnsigned.empty()) fn.slotElemUnsigned.push_back(false);
                if (!fn.slotAliasGroup.empty()) fn.slotAliasGroup.push_back(-1);

                const Span sp = fn.blocks[hdr].ops.empty() ? Span{} : fn.blocks[hdr].ops.front().span;
                // Load arr[iSlot] into scSlot in preheader
                {
                    const Value av{Type::IntArray, fn.nextValue++};
                    const Value iv{Type::Int, fn.nextValue++};
                    const Value initV{Type::Int, fn.nextValue++};
                    fn.blocks[pre].ops.push_back({Op::Load, av, {}, {}, arrSlot, sp});
                    fn.blocks[pre].ops.push_back({Op::Load, iv, {}, {}, iSlot, sp});
                    Operation initIdx{Op::Index, initV, {av, iv}, {}, 0, sp};
                    initIdx.boundsProven = true;
                    initIdx.elemBits = elemBits;
                    initIdx.elemUnsigned = elemUnsigned;
                    fn.blocks[pre].ops.push_back(initIdx);
                    fn.blocks[pre].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {initV}, {}, scSlot, sp});
                }
                // Replace Index / IndexSet on arr[iSlot] inside lp.body with Load / Store scSlot
                for (unsigned b : lp.body) {
                    for (auto& op : fn.blocks[b].ops) {
                        if (op.op == Op::Index && op.inputs.size() >= 2) {
                            auto als = loadSlot.find(op.inputs[0].id);
                            auto ils = loadSlot.find(op.inputs[1].id);
                            if (als != loadSlot.end() && als->second == arrSlot &&
                                ils != loadSlot.end() && ils->second == iSlot) {
                                op.op = Op::Load;
                                op.inputs.clear();
                                op.data.clear();
                                op.slot = scSlot;
                            }
                        } else if (op.op == Op::IndexSet && op.inputs.size() >= 3) {
                            auto als = loadSlot.find(op.inputs[0].id);
                            auto ils = loadSlot.find(op.inputs[1].id);
                            if (als != loadSlot.end() && als->second == arrSlot &&
                                ils != loadSlot.end() && ils->second == iSlot) {
                                const Value rhsV = op.inputs[2];
                                op = Operation{Op::Store, {Type::Void, fn.nextValue++}, {rhsV}, {}, scSlot, op.span};
                            }
                        }
                    }
                }
                // Write back arr[iSlot] = scSlot on exit
                unsigned writeBlk = exitB;
                if (cfg.preds[exitB].size() > 1) {
                    const unsigned tramp = static_cast<unsigned>(fn.blocks.size());
                    Block tb;
                    tb.exit = Exit::Jump;
                    tb.yes = exitB;
                    fn.blocks[hdr].no = tramp;
                    fn.blocks.push_back(std::move(tb));
                    writeBlk = tramp;
                }
                {
                    auto& eOps = fn.blocks[writeBlk].ops;
                    const Value av{Type::IntArray, fn.nextValue++};
                    const Value iv{Type::Int, fn.nextValue++};
                    const Value sv{Type::Int, fn.nextValue++};
                    std::vector<Operation> wb;
                    wb.push_back({Op::Load, av, {}, {}, arrSlot, sp});
                    wb.push_back({Op::Load, iv, {}, {}, iSlot, sp});
                    wb.push_back({Op::Load, sv, {}, {}, scSlot, sp});
                    Operation stIdx{Op::IndexSet, {Type::Void, fn.nextValue++}, {av, iv, sv}, {}, 0, sp};
                    stIdx.boundsProven = true;
                    stIdx.elemBits = elemBits;
                    stIdx.elemUnsigned = elemUnsigned;
                    wb.push_back(stIdx);
                    eOps.insert(eOps.begin(), wb.begin(), wb.end());
                }
                promoted = true;
                break;
            }
            if (promoted) break;
        }
        if (!promoted) break;
    }
}

/// Prepend an 8-wide exact block-shift fast path before pure bounds-proven insertion-sort
/// inner loops: `while (j > 0 && a[j - 1] OP v) { a[j] = a[j - 1]; j -= 1; }`.
inline void vectorizeBackwardShiftLoops(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    const std::size_t origLoops = detail::buildCFG(fn).loops.size();
    for (std::size_t attempt = 0; attempt < origLoops; ++attempt) {
        const detail::CFGInfo cfg = detail::buildCFG(fn);
        std::unordered_map<unsigned, const Operation*> defOp;
        std::unordered_map<unsigned, unsigned> loadSlot;
        for (const auto& blk : fn.blocks) {
            for (const auto& op : blk.ops) {
                if (op.result.type != Type::Void) defOp[op.result.id] = &op;
                if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
            }
        }
        bool transformed = false;
        for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
            const auto& lp = cfg.loops[li];
            if (lp.body.size() != 4) continue;
            const unsigned hdr = lp.header;
            const auto& hblk = fn.blocks[hdr];
            if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) ||
                !lp.body.contains(hblk.no) || hblk.yes == hblk.no)
                continue;
            const unsigned rhsBlk = hblk.yes, endBlk = hblk.no;
            const auto& rblk = fn.blocks[rhsBlk];
            const auto& eblk = fn.blocks[endBlk];
            if (rblk.exit != Exit::Jump || rblk.yes != endBlk || cfg.preds[rhsBlk].size() != 1) continue;
            if (eblk.exit != Exit::Branch || !eblk.value || !lp.body.contains(eblk.yes) ||
                lp.body.contains(eblk.no))
                continue;
            const unsigned bodyBlk = eblk.yes;
            const auto& bblk = fn.blocks[bodyBlk];
            if (bblk.exit != Exit::Jump || bblk.yes != hdr || cfg.preds[bodyBlk].size() != 1) continue;

            // Skip if already preceded by `j >= 8` fast block
            bool alreadyPeeled = false;
            for (unsigned p : cfg.preds[hdr]) {
                if (lp.body.contains(p)) continue;
                const auto& pb = fn.blocks[p];
                if (pb.exit == Exit::Branch && pb.value) {
                    auto cd = defOp.find(pb.value->id);
                    if (cd != defOp.end() && cd->second->op == Op::Binary && cd->second->data == ">=") {
                        if (parseConstInt(defOp.count(cd->second->inputs[1].id)
                                          ? defOp.at(cd->second->inputs[1].id)->data : "") == 8)
                            alreadyPeeled = true;
                    }
                }
            }
            if (alreadyPeeled) continue;

            auto c1 = defOp.find(hblk.value->id);
            if (c1 == defOp.end() || c1->second->op != Op::Binary || c1->second->data != ">") continue;
            auto jLs = loadSlot.find(c1->second->inputs[0].id);
            if (jLs == loadSlot.end() ||
                parseConstInt(defOp.count(c1->second->inputs[1].id) ? defOp.at(c1->second->inputs[1].id)->data : "") != 0)
                continue;
            const unsigned jSlot = jLs->second;
            if (hblk.ops.empty() || hblk.ops.back().op != Op::Store) continue;
            const unsigned bSlot = hblk.ops.back().slot;
            if (eblk.ops.size() != 1 || eblk.ops[0].op != Op::Load || eblk.ops[0].slot != bSlot) continue;

            const Operation* cmp2 = nullptr;
            bool okRhs = true;
            for (const auto& op : rblk.ops) {
                if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::DynCall || op.op == Op::IndexSet) {
                    okRhs = false; break;
                }
                if (op.op == Op::Store) {
                    if (op.slot != bSlot || op.inputs.empty()) { okRhs = false; break; }
                    auto d = defOp.find(op.inputs[0].id);
                    if (d != defOp.end()) cmp2 = d->second;
                }
            }
            if (!okRhs || !cmp2 || cmp2->op != Op::Binary ||
                (cmp2->data != ">" && cmp2->data != "<" && cmp2->data != ">=" && cmp2->data != "<="))
                continue;
            auto idxDef = defOp.find(cmp2->inputs[0].id);
            auto vLs = loadSlot.find(cmp2->inputs[1].id);
            if (idxDef == defOp.end() || idxDef->second->op != Op::Index || !idxDef->second->boundsProven ||
                vLs == loadSlot.end())
                continue;
            auto aLs = loadSlot.find(idxDef->second->inputs[0].id);
            auto jm1 = defOp.find(idxDef->second->inputs[1].id);
            if (aLs == loadSlot.end() || jm1 == defOp.end() || jm1->second->op != Op::Binary || jm1->second->data != "-")
                continue;
            auto jmLs = loadSlot.find(jm1->second->inputs[0].id);
            if (jmLs == loadSlot.end() || jmLs->second != jSlot ||
                parseConstInt(defOp.count(jm1->second->inputs[1].id) ? defOp.at(jm1->second->inputs[1].id)->data : "") != 1)
                continue;

            const unsigned aSlot = aLs->second;
            const unsigned vSlot = vLs->second;
            if (aSlot == jSlot || aSlot == vSlot || jSlot == vSlot) continue;

            int bodyIdxSetCnt = 0, bodyJStoreCnt = 0;
            bool okBody = true;
            std::uint8_t elemBits = idxDef->second->elemBits;
            bool elemUnsigned = idxDef->second->elemUnsigned;
            for (const auto& op : bblk.ops) {
                if (op.op == Op::IndexSet && op.inputs.size() >= 3) {
                    ++bodyIdxSetCnt;
                    auto als2 = loadSlot.find(op.inputs[0].id);
                    auto ils2 = loadSlot.find(op.inputs[1].id);
                    auto rhsIdx = defOp.find(op.inputs[2].id);
                    if (!op.boundsProven || als2 == loadSlot.end() || als2->second != aSlot ||
                        ils2 == loadSlot.end() || ils2->second != jSlot || rhsIdx == defOp.end() ||
                        rhsIdx->second->op != Op::Index || !rhsIdx->second->boundsProven) {
                        okBody = false; break;
                    }
                    auto as3 = loadSlot.find(rhsIdx->second->inputs[0].id);
                    auto jm2 = defOp.find(rhsIdx->second->inputs[1].id);
                    if (as3 == loadSlot.end() || as3->second != aSlot || jm2 == defOp.end() ||
                        jm2->second->op != Op::Binary || jm2->second->data != "-") {
                        okBody = false; break;
                    }
                    auto jl2 = loadSlot.find(jm2->second->inputs[0].id);
                    if (jl2 == loadSlot.end() || jl2->second != jSlot ||
                        parseConstInt(defOp.count(jm2->second->inputs[1].id)
                                      ? defOp.at(jm2->second->inputs[1].id)->data : "") != 1) {
                        okBody = false; break;
                    }
                } else if (op.op == Op::Store) {
                    if (op.slot != jSlot || op.inputs.empty()) { okBody = false; break; }
                    ++bodyJStoreCnt;
                    auto jd = defOp.find(op.inputs[0].id);
                    if (jd == defOp.end() || jd->second->op != Op::Binary || jd->second->data != "-" ||
                        !jd->second->overflowProven) {
                        okBody = false; break;
                    }
                    auto jl3 = loadSlot.find(jd->second->inputs[0].id);
                    if (jl3 == loadSlot.end() || jl3->second != jSlot ||
                        parseConstInt(defOp.count(jd->second->inputs[1].id)
                                      ? defOp.at(jd->second->inputs[1].id)->data : "") != 1) {
                        okBody = false; break;
                    }
                } else if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Index && op.op != Op::Binary) {
                    okBody = false; break;
                }
            }
            if (!okBody || bodyIdxSetCnt != 1 || bodyJStoreCnt != 1) continue;

            const std::string cmpStr = cmp2->data;
            const std::uint8_t cmpWidth = cmp2->width;
            const Span sp = cmp2->span;
            const unsigned fastHdr = static_cast<unsigned>(fn.blocks.size());
            const unsigned fastBody = fastHdr + 1;

            for (unsigned p = 0; p < fastHdr; ++p) {
                if (lp.body.contains(p)) continue;
                auto& pb = fn.blocks[p];
                if (pb.exit == Exit::Jump && pb.yes == hdr) pb.yes = fastHdr;
                else if (pb.exit == Exit::Branch) {
                    if (pb.yes == hdr) pb.yes = fastHdr;
                    if (pb.no == hdr) pb.no = fastHdr;
                } else if (pb.exit == Exit::Switch) {
                    if (pb.yes == hdr) pb.yes = fastHdr;
                    for (unsigned& t : pb.targets) if (t == hdr) t = fastHdr;
                }
            }

            Block fh, fb;
            {
                const Value lj{Type::Int, fn.nextValue++};
                const Value c8{Type::Int, fn.nextValue++};
                const Value cond{Type::Bool, fn.nextValue++};
                fh.ops.push_back({Op::Load, lj, {}, {}, jSlot, sp});
                fh.ops.push_back({Op::Int, c8, {}, "8", 0, sp});
                Operation cmpOp{Op::Binary, cond, {lj, c8}, ">=", 0, sp};
                cmpOp.overflowProven = true;
                fh.ops.push_back(cmpOp);
                fh.exit = Exit::Branch;
                fh.value = cond;
                fh.yes = fastBody;
                fh.no = hdr;
            }
            {
                // fastBody: check 8 elements a[j-1..j-8]; wait: jump to fastShift on true, hdr on false
                const unsigned fastShift = fastBody + 1;
                const Value arrV{Type::IntArray, fn.nextValue++};
                const Value jV{Type::Int, fn.nextValue++};
                const Value vV{Type::Int, fn.nextValue++};
                fb.ops.push_back({Op::Load, arrV, {}, {}, aSlot, sp});
                fb.ops.push_back({Op::Load, jV, {}, {}, jSlot, sp});
                fb.ops.push_back({Op::Load, vV, {}, {}, vSlot, sp});
                Value tVals[8];
                Value cVals[8];
                for (int k = 0; k < 8; ++k) {
                    const Value ck{Type::Int, fn.nextValue++};
                    const Value idxK{Type::Int, fn.nextValue++};
                    tVals[k] = Value{Type::Int, fn.nextValue++};
                    cVals[k] = Value{Type::Bool, fn.nextValue++};
                    fb.ops.push_back({Op::Int, ck, {}, std::to_string(k + 1), 0, sp});
                    Operation subK{Op::Binary, idxK, {jV, ck}, "-", 0, sp};
                    subK.overflowProven = true;
                    fb.ops.push_back(subK);
                    Operation ldK{Op::Index, tVals[k], {arrV, idxK}, {}, 0, sp};
                    ldK.boundsProven = true;
                    ldK.elemBits = elemBits;
                    ldK.elemUnsigned = elemUnsigned;
                    fb.ops.push_back(ldK);
                    Operation cmK{Op::Binary, cVals[k], {tVals[k], vV}, cmpStr, 0, sp};
                    cmK.width = cmpWidth;
                    fb.ops.push_back(cmK);
                }
                auto joinAnd = [&](Value a, Value b) {
                    Value r{Type::Bool, fn.nextValue++};
                    fb.ops.push_back({Op::Binary, r, {a, b}, "&", 0, sp});
                    return r;
                };
                const Value a01 = joinAnd(cVals[0], cVals[1]);
                const Value a23 = joinAnd(cVals[2], cVals[3]);
                const Value a45 = joinAnd(cVals[4], cVals[5]);
                const Value a67 = joinAnd(cVals[6], cVals[7]);
                const Value a03 = joinAnd(a01, a23);
                const Value a47 = joinAnd(a45, a67);
                const Value all8 = joinAnd(a03, a47);
                fb.exit = Exit::Branch;
                fb.value = all8;
                fb.yes = fastShift;
                fb.no = hdr;

                Block fs;
                for (int k = 0; k < 8; ++k) {
                    const Value ck{Type::Int, fn.nextValue++};
                    const Value dstIdx{Type::Int, fn.nextValue++};
                    fs.ops.push_back({Op::Int, ck, {}, std::to_string(k), 0, sp});
                    Operation subD{Op::Binary, dstIdx, {jV, ck}, "-", 0, sp};
                    subD.overflowProven = true;
                    fs.ops.push_back(subD);
                    Operation stK{Op::IndexSet, {Type::Void, fn.nextValue++}, {arrV, dstIdx, tVals[k]}, {}, 0, sp};
                    stK.boundsProven = true;
                    stK.elemBits = elemBits;
                    stK.elemUnsigned = elemUnsigned;
                    fs.ops.push_back(stK);
                }
                const Value c8{Type::Int, fn.nextValue++};
                const Value newJ{Type::Int, fn.nextValue++};
                fs.ops.push_back({Op::Int, c8, {}, "8", 0, sp});
                Operation subJ{Op::Binary, newJ, {jV, c8}, "-", 0, sp};
                subJ.overflowProven = true;
                fs.ops.push_back(subJ);
                fs.ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {newJ}, {}, jSlot, sp});
                fs.exit = Exit::Jump;
                fs.yes = fastHdr;

                fn.blocks.push_back(std::move(fh));
                fn.blocks.push_back(std::move(fb));
                fn.blocks.push_back(std::move(fs));
            }
            transformed = true;
            break;
        }
        if (!transformed) break;
    }
}

#include "native_opt_phase9.hpp"

inline void optimizeIR(std::vector<FunctionIR>& functions,
                       const std::vector<bool>& safeDepths,
                       const std::string& entryMain,
                       bool allowCompaction = true,
                       std::vector<FastClone>* fastClones = nullptr,
                       const Program* programPtr = nullptr,
                       std::deque<Function>* closureSpecStorage = nullptr) {
    if (allowCompaction && programPtr && closureSpecStorage)
        devirtualizeNonEscapingClosures(*programPtr, functions, *closureSpecStorage);
    if (programPtr) {
        for (auto& fn : functions) {
            if (fn.source.isTest) continue;
            elideLastUseMoves(fn, *programPtr);
            analyzeMonotonicPools(fn, *programPtr);
            fusePerceusReuse(fn, *programPtr);
        }
    }
    inlineSmallFunctions(functions, safeDepths, entryMain);

    std::unordered_map<std::string, IntRange> fnReturnRanges;
    std::vector<std::unordered_map<unsigned, std::optional<IntRange>>> fnElemRanges(functions.size());
    std::vector<std::unordered_map<unsigned, IntRange>> fnValRanges(functions.size());
    std::vector<std::unordered_map<std::size_t, std::vector<std::pair<unsigned, unsigned>>>> fnModLtDiv(functions.size());
    for (int iter = 0; iter < 3; ++iter) {
        bool changed = false;
        for (std::size_t fi = 0; fi < functions.size(); ++fi) {
            auto& fn = functions[fi];
            if (fn.source.isTest) continue;
            detail::FunctionProver prover(fn, fnReturnRanges, {}, programPtr);
            prover.run();
            if (iter == 2) {
                prover.applyCompaction(allowCompaction);
                fnElemRanges[fi] = prover.elemRanges();
                fnValRanges[fi] = prover.valRanges();
                fnModLtDiv[fi] = prover.modLtDiv();
            }
            if (fn.source.result == Type::Int) {
                if (auto rr = prover.computeReturnRange()) {
                    auto it = fnReturnRanges.find(fn.source.name);
                    if (it == fnReturnRanges.end() ||
                        it->second.lower != rr->lower || it->second.upper != rr->upper) {
                        fnReturnRanges[fn.source.name] = *rr;
                        changed = true;
                    }
                }
            }
        }
        if (!changed && iter >= 1) {
            // Ensure compaction runs even on early convergence
            if (iter < 2) {
                for (std::size_t fi = 0; fi < functions.size(); ++fi) {
                    auto& fn = functions[fi];
                    if (fn.source.isTest) continue;
                    detail::FunctionProver prover(fn, fnReturnRanges, {}, programPtr);
                    prover.run();
                    prover.applyCompaction(allowCompaction);
                    fnElemRanges[fi] = prover.elemRanges();
                    fnValRanges[fi] = prover.valRanges();
                    fnModLtDiv[fi] = prover.modLtDiv();
                }
            }
            break;
        }
    }

    auto runLoopTransforms = [&](FunctionIR& fn,
                                 const std::unordered_map<unsigned, std::optional<IntRange>>& er,
                                 const std::unordered_map<unsigned, IntRange>& vr,
                                 const std::unordered_map<std::size_t, std::vector<std::pair<unsigned, unsigned>>>& mld) {
        if (!allowCompaction || fn.source.isTest) return;
        auto erCopy = er;
        eliminateDeadRepeatInit(fn);
        tileRepeatPeriodicArrayFill(fn);
        strengthReduceLoopModStep(fn, vr, mld);
        unrollRadixDigitSumLoops(fn);
        memoizePureSmallDomainLoops(fn, vr);
        optimizeMinPlusInnerLoops(fn, erCopy);
        unrollMinPlusOuterRows4(fn);
        promoteDisjointIndexedAccum(fn);
        promoteInvariantArrayElement(fn);
        if (!interchangeRowStripMatMul(fn, erCopy)) {
            transposeStridedMatMul(fn, erCopy);
            unrollAndJamMatMul(fn, erCopy);
        }
        tileAndUnrollFloatMatMul(fn, programPtr);
        chunkNarrowReduction(fn, vr);
        vectorizeBackwardShiftLoops(fn);
        cacheArgExtremeValue(fn, erCopy);
        ifConvertUnpredictableDiamonds(fn);
        mergeSinglePredJumps(fn);
        strengthReduceAffineIterModConst(fn, vr);
        strengthReduceForIterModConst(fn, vr);
        promoteInvariantArrayElement(fn);
        tagColdEqualityBranches(fn);
    };

    if (allowCompaction && fastClones) {
        auto scoreQuality = [](const FunctionIR& fn) {
            struct { int unchecked = 0; int wideOps = 0; int wideArrays = 0; int signedDiv = 0; int modDiv = 0; } s;
            for (auto bits : fn.slotElemBits) s.wideArrays += static_cast<int>(bits);
            for (const auto& blk : fn.blocks) {
                for (const auto& op : blk.ops) {
                    if ((op.op == Op::Index || op.op == Op::IndexSet) && !op.boundsProven) ++s.unchecked;
                    if (op.op == Op::PlaceSet) {
                        for (const auto& st : op.placeSteps)
                            if (st.isIndex) { ++s.unchecked; break; }
                    }
                    if (op.op == Op::Negate && op.result.type == Type::Int && !op.overflowProven) ++s.unchecked;
                    if (op.op == Op::AbsDiff && !op.overflowProven) ++s.unchecked;
                    if (op.op == Op::Binary && op.inputs[0].type == Type::Int) {
                        if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                             op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) ++s.unchecked;
                        if ((op.data == "/" || op.data == "%")) {
                            if (!op.divSafe) ++s.unchecked;
                            if (!op.unsignedDiv) ++s.signedDiv;
                            if (op.data == "%" && !op.modNoOp && op.modSubSteps == 0) ++s.modDiv;
                        }
                    }
                    if (op.width == 64 && op.result.type == Type::Int &&
                        (op.op == Op::Binary || op.op == Op::Min || op.op == Op::Max ||
                         op.op == Op::Negate || op.op == Op::AbsDiff)) {
                        ++s.wideOps;
                    }
                }
            }
            return s;
        };

        for (std::size_t i = 0; i < functions.size(); ++i) {
            const FunctionIR& orig = functions[i];
            if (orig.source.isTest || orig.source.isExternC || orig.source.name == entryMain) continue;
            if (orig.source.params.empty() || orig.source.params.size() > 3) continue;
            bool allIntParams = true;
            for (const auto& p : orig.source.params) {
                if (p.byRef || p.type != Type::Int) { allIntParams = false; break; }
            }
            if (!allIntParams) continue;
            bool hasCall = false;
            for (const auto& blk : orig.blocks) {
                for (const auto& op : blk.ops) {
                    if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin || op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall) { hasCall = true; break; }
                }
                if (hasCall) break;
            }
            if (hasCall) continue;

            FunctionIR fast = orig;
            std::vector<std::optional<IntRange>> initParams(orig.source.params.size(), IntRange{0, 1048575});
            detail::FunctionProver fastProver(fast, fnReturnRanges, std::move(initParams), programPtr);
            fastProver.run();
            fastProver.applyCompaction(true);

            const auto qOrig = scoreQuality(orig);
            const auto qFast = scoreQuality(fast);
            if (qFast.unchecked < qOrig.unchecked || qFast.wideArrays < qOrig.wideArrays ||
                qFast.signedDiv < qOrig.signedDiv || qFast.modDiv < qOrig.modDiv || qFast.wideOps < qOrig.wideOps) {
                const bool dSafe = i < safeDepths.size() ? safeDepths[i] : true;
                runLoopTransforms(fast, fastProver.elemRanges(), fastProver.valRanges(), fastProver.modLtDiv());
                const bool looped = !detail::buildCFG(fast).loops.empty();
                assignAliasGroupAndStackArena(fast, dSafe, programPtr, false);
                if (allowCompaction) tagVectorizableAndUnrollableLoops(fast);
                fastClones->push_back({orig.source.name, orig.source.name + ".fast", dSafe, looped, std::move(fast)});
            }
        }
    }

    for (std::size_t i = 0; i < functions.size(); ++i) {
        runLoopTransforms(functions[i], fnElemRanges[i], fnValRanges[i], fnModLtDiv[i]);
    }
    assignAliasGroupsAndStackArena(functions, safeDepths, programPtr);
    if (allowCompaction) {
        for (auto& fn : functions) {
            if (!fn.source.isTest) tagVectorizableAndUnrollableLoops(fn);
        }
    }
}

} // namespace veloz::native
