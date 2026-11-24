#pragma once

namespace detail {
inline std::uint64_t gcd64(std::uint64_t a, std::uint64_t b) {
    while (b != 0) {
        std::uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

inline std::optional<std::uint64_t> lcmBounded64(std::uint64_t a, std::uint64_t b, std::uint64_t limit) {
    if (a == 0 || b == 0) return std::nullopt;
    const std::uint64_t g = gcd64(a, b);
    const std::uint64_t q = a / g;
    if (q > limit / b) return std::nullopt;
    return q * b;
}

inline unsigned allocOptSlot(FunctionIR& fn, Type t) {
    const unsigned s = static_cast<unsigned>(fn.slots.size());
    fn.slots.push_back(t);
    if (fn.slotByRef.size() == s) fn.slotByRef.push_back(false);
    if (fn.slotElemBits.size() == s) fn.slotElemBits.push_back(64);
    if (fn.slotElemUnsigned.size() == s) fn.slotElemUnsigned.push_back(false);
    if (fn.slotAliasGroup.size() == s) fn.slotAliasGroup.push_back(-1);
    if (fn.slotOwned.size() == s) fn.slotOwned.push_back(false);
    return s;
}
} // namespace detail

/// For 1D loops filling an `IntArray` with a proven-periodic expression `f(i)` of period `P <= 2048`
/// (`i` enters solely through `unsigned` safe `% d_k` over polynomial expressions of `i`),
/// run the loop only for `0 .. min(N, P)` and double-copy the tile with `@tz.tile_repeat`.
inline void tileRepeatPeriodicArrayFill(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    const std::size_t nSlots = fn.slots.size();
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

    struct Plan {
        unsigned pre = 0, hdr = 0, body = 0, exitBlk = 0;
        unsigned arrSlot = 0, iSlot = 0;
        std::size_t cmpIdx = 0;
        Value nVal{};
        std::uint64_t period = 0;
        std::uint8_t elemBits = 64;
        Span span{};
        bool cloneBoundLoad = false;
        unsigned boundLoadSlot = 0;
    };
    std::vector<Plan> plans;
    std::unordered_set<unsigned> usedHeaders;

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lp = cfg.loops[li];
        if (lp.body.size() != 2 || lp.preheaders.size() != 1) continue;
        const unsigned hdr = lp.header;
        const unsigned pre = lp.preheaders[0];
        if (usedHeaders.contains(hdr)) continue;
        const auto& hblk = fn.blocks[hdr];
        if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
            continue;
        if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != hdr) continue;
        const unsigned body = hblk.yes;
        const unsigned exitBlk = hblk.no;
        if (fn.blocks[body].exit != Exit::Jump || fn.blocks[body].yes != hdr) continue;

        std::optional<std::size_t> cmpPos;
        for (std::size_t oi = 0; oi < hblk.ops.size(); ++oi) {
            if (hblk.ops[oi].result.id == hblk.value->id) { cmpPos = oi; break; }
        }
        if (!cmpPos) continue;
        const Operation& cmpOp = hblk.ops[*cmpPos];
        if (cmpOp.op != Op::Binary || cmpOp.data != "<" || cmpOp.inputs.size() != 2) continue;
        auto ils = loadSlot.find(cmpOp.inputs[0].id);
        if (ils == loadSlot.end()) continue;
        const unsigned iSlot = ils->second;
        const Value nVal = cmpOp.inputs[1];
        if (nVal.type != Type::Int) continue;

        bool cloneBoundLoad = false;
        unsigned boundLoadSlot = 0;
        if (auto ndb = defBlock.find(nVal.id); ndb != defBlock.end() && lp.body.contains(ndb->second)) {
            auto nd = defOp.find(nVal.id);
            if (nd == defOp.end() || nd->second->op != Op::Load || nd->second->slot == iSlot) continue;
            cloneBoundLoad = true;
            boundLoadSlot = nd->second->slot;
        }

        // pre must initialize iSlot = 0
        std::optional<std::int64_t> initI;
        for (const auto& op : fn.blocks[pre].ops) {
            if (op.op == Op::Store && op.slot == iSlot && !op.inputs.empty()) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && d->second->op == Op::Int) initI = parseConstInt(d->second->data);
                else initI.reset();
            }
        }
        if (initI != 0) continue;

        // Inspect written slots in hdr + body; allow iSlot (+1 once) and body-local single-store scalar temps
        std::unordered_set<unsigned> writtenInLoop;
        for (unsigned lb : lp.body) {
            for (const auto& op : fn.blocks[lb].ops) {
                if (op.op == Op::Store && op.slot < nSlots) writtenInLoop.insert(op.slot);
                for (unsigned ms : op.mutArgSlots) {
                    if (ms < nSlots) writtenInLoop.insert(ms);
                }
            }
        }
        if (cloneBoundLoad && writtenInLoop.contains(boundLoadSlot)) continue;

        bool okBody = true;
        int incCount = 0;
        std::optional<Operation> setOp;
        std::unordered_map<unsigned, Value> tempSlotExpr;
        std::unordered_set<unsigned> tempSlotsBeforeLoad;
        for (const auto& op : fn.blocks[body].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin ||
                op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                op.op == Op::Print || op.op == Op::Index || op.op == Op::TileRepeat || op.op == Op::FindFirst ||
                op.op == Op::GemmF32 || op.op == Op::MatMulMod ||
                op.op == Op::FloydStepU16 || op.op == Op::ArgMinU16 ||
                op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice ||
                op.op == Op::FieldSet || op.op == Op::PlaceSet || op.op == Op::RepeatHeap ||
                op.op == Op::RepeatRegion || op.op == Op::Clone || op.op == Op::ClearOwner ||
                op.op == Op::ClearField || op.op == Op::ClearIndex ||
                (op.op == Op::Assert && !op.boundsProven && !op.unchecked)) {
                okBody = false; break;
            }
            if ((op.op == Op::Negate || op.op == Op::AbsDiff) && op.result.type == Type::Int && !op.overflowProven) {
                okBody = false; break;
            }
            if (op.op == Op::Binary && !op.inputs.empty() && op.inputs[0].type == Type::Int) {
                if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                     op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) {
                    okBody = false; break;
                }
                if ((op.data == "/" || op.data == "%") && !op.divSafe) {
                    okBody = false; break;
                }
            }
            if (op.op == Op::IndexSet) {
                if (setOp || !op.boundsProven || op.inputs.size() < 3 || op.inputs[0].type != Type::IntArray) {
                    okBody = false; break;
                }
                auto jls = loadSlot.find(op.inputs[1].id);
                if (jls == loadSlot.end() || jls->second != iSlot) { okBody = false; break; }
                setOp = op;
            } else if (op.op == Op::Load) {
                if (op.slot != iSlot && writtenInLoop.contains(op.slot) && !tempSlotsBeforeLoad.contains(op.slot)) {
                    okBody = false; break; // loop-carried scalar other than iSlot!
                }
            } else if (op.op == Op::Store) {
                if (op.slot == iSlot) {
                    ++incCount;
                    auto kd = defOp.find(op.inputs[0].id);
                    if (kd == defOp.end() || kd->second->op != Op::Binary ||
                        (kd->second->data != "rangeIncrement" && kd->second->data != "+") ||
                        !kd->second->overflowProven) {
                        okBody = false; break;
                    }
                    auto kls = loadSlot.find(kd->second->inputs[0].id);
                    auto kc = defOp.find(kd->second->inputs[1].id);
                    auto c1 = (kc != defOp.end() && kc->second->op == Op::Int) ? parseConstInt(kc->second->data) : std::nullopt;
                    if (kls == loadSlot.end() || kls->second != iSlot || c1 != 1) { okBody = false; break; }
                } else {
                    if (fn.slots[op.slot] != Type::Int || tempSlotExpr.contains(op.slot) || op.inputs.empty()) {
                        okBody = false; break;
                    }
                    tempSlotExpr[op.slot] = op.inputs[0];
                    tempSlotsBeforeLoad.insert(op.slot);
                }
            }
        }
        if (!okBody || !setOp || incCount != 1) continue;
        // Also ensure hdr does not write or call anything unsafe
        bool okHdr = true;
        for (const auto& op : hblk.ops) {
            if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary) { okHdr = false; break; }
            if (op.op == Op::Load && writtenInLoop.contains(op.slot) && op.slot != iSlot) { okHdr = false; break; }
        }
        if (!okHdr) continue;

        auto als = loadSlot.find(setOp->inputs[0].id);
        if (als == loadSlot.end()) continue;
        const unsigned arrSlot = als->second;
        if (writtenInLoop.contains(arrSlot) || arrSlot >= nSlots || fn.slots[arrSlot] != Type::IntArray) continue;

        // Check tempSlots are not loaded outside body
        bool tempEscapes = false;
        for (unsigned bi = 0; bi < fn.blocks.size() && !tempEscapes; ++bi) {
            if (bi == body) continue;
            for (const auto& op : fn.blocks[bi].ops) {
                if ((op.op == Op::Load || op.op == Op::Store) && tempSlotExpr.contains(op.slot)) {
                    tempEscapes = true; break;
                }
            }
        }
        if (tempEscapes) continue;

        // Check whether an expression inside `%` depends only polynomially (+,-,*,Negate) on iSlot & invariants
        std::function<bool(Value, int)> isPolyOfI = [&](Value v, int depth) -> bool {
            if (depth > 32) return false;
            auto db = defBlock.find(v.id);
            if (db == defBlock.end() || !lp.body.contains(db->second)) return true;
            auto dit = defOp.find(v.id);
            if (dit == defOp.end()) return false;
            const Operation* o = dit->second;
            if (o->op == Op::Int || o->op == Op::Bool) return true;
            if (o->op == Op::Load) {
                if (o->slot == iSlot) return true;
                if (auto it = tempSlotExpr.find(o->slot); it != tempSlotExpr.end())
                    return isPolyOfI(it->second, depth + 1);
                return !writtenInLoop.contains(o->slot);
            }
            if (o->op == Op::Negate) return o->overflowProven && isPolyOfI(o->inputs[0], depth + 1);
            if (o->op == Op::Binary && (o->data == "+" || o->data == "-" || o->data == "*")) {
                return o->overflowProven && isPolyOfI(o->inputs[0], depth + 1) && isPolyOfI(o->inputs[1], depth + 1);
            }
            return false;
        };

        std::function<std::optional<std::uint64_t>(Value, int)> exprPeriod =
            [&](Value v, int depth) -> std::optional<std::uint64_t> {
            if (depth > 32) return std::nullopt;
            auto db = defBlock.find(v.id);
            if (db == defBlock.end() || !lp.body.contains(db->second)) return 1ULL;
            auto dit = defOp.find(v.id);
            if (dit == defOp.end()) return std::nullopt;
            const Operation* o = dit->second;
            if (o->op == Op::Int || o->op == Op::Bool) return 1ULL;
            if (o->op == Op::Load) {
                if (o->slot == iSlot) return std::nullopt; // non-periodic raw i
                if (auto it = tempSlotExpr.find(o->slot); it != tempSlotExpr.end())
                    return exprPeriod(it->second, depth + 1);
                if (writtenInLoop.contains(o->slot)) return std::nullopt;
                return 1ULL;
            }
            if (o->op == Op::Binary && o->data == "%") {
                if (!o->divSafe || !o->unsignedDiv || o->inputs.size() < 2) return std::nullopt;
                auto dDef = defOp.find(o->inputs[1].id);
                if (dDef == defOp.end() || dDef->second->op != Op::Int) return std::nullopt;
                auto dc = parseConstInt(dDef->second->data);
                if (!dc || *dc < 1 || *dc > 2048) return std::nullopt;
                if (auto pLhs = exprPeriod(o->inputs[0], depth + 1)) return pLhs;
                if (isPolyOfI(o->inputs[0], 0)) return static_cast<std::uint64_t>(*dc);
                return std::nullopt;
            }
            if (o->op == Op::Negate || o->op == Op::Min || o->op == Op::Max || o->op == Op::AbsDiff ||
                o->op == Op::Select ||
                (o->op == Op::Binary && (o->data == "+" || o->data == "-" || o->data == "*" || o->data == "/" ||
                                         o->data == "<<" || o->data == ">>" || o->data == ">>>"))) {
                std::uint64_t curP = 1;
                for (const auto& in : o->inputs) {
                    auto ip = exprPeriod(in, depth + 1);
                    if (!ip) return std::nullopt;
                    auto l = detail::lcmBounded64(curP, *ip, 2048);
                    if (!l) return std::nullopt;
                    curP = *l;
                }
                return curP;
            }
            return std::nullopt;
        };

        auto per = exprPeriod(setOp->inputs[2], 0);
        if (!per || *per <= 1 || *per > 2048) continue;

        usedHeaders.insert(hdr);
        plans.push_back({pre, hdr, body, exitBlk, arrSlot, iSlot, *cmpPos, nVal, *per,
                         setOp->elemBits, setOp->span, cloneBoundLoad, boundLoadSlot});
    }

    for (const auto& p : plans) {
        if (fn.blocks[p.pre].exit != Exit::Jump || fn.blocks[p.pre].yes != p.hdr) continue;
        const unsigned limSlot = detail::allocOptSlot(fn, Type::Int);
        const unsigned tileBlk = static_cast<unsigned>(fn.blocks.size());
        fn.blocks.emplace_back();

        Value nInPre = p.nVal;
        if (p.cloneBoundLoad) {
            nInPre = Value{Type::Int, fn.nextValue++};
            fn.blocks[p.pre].ops.push_back({Op::Load, nInPre, {}, {}, p.boundLoadSlot, p.span});
        }
        const Value pPre{Type::Int, fn.nextValue++};
        const Value limV{Type::Int, fn.nextValue++};
        Operation minLim{Op::Min, limV, {nInPre, pPre}, {}, 0, p.span};
        minLim.width = 64;
        fn.blocks[p.pre].ops.push_back({Op::Int, pPre, {}, std::to_string(p.period), 0, p.span});
        fn.blocks[p.pre].ops.push_back(std::move(minLim));
        fn.blocks[p.pre].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {limV}, {}, limSlot, p.span});

        const Value limHdr{Type::Int, fn.nextValue++};
        fn.blocks[p.hdr].ops.insert(fn.blocks[p.hdr].ops.begin() + static_cast<std::ptrdiff_t>(p.cmpIdx),
                                    {Op::Load, limHdr, {}, {}, limSlot, p.span});
        fn.blocks[p.hdr].ops[p.cmpIdx + 1].inputs[1] = limHdr;
        fn.blocks[p.hdr].no = tileBlk;

        const Value arrV{Type::IntArray, fn.nextValue++};
        Value nInTile = p.nVal;
        if (p.cloneBoundLoad) {
            nInTile = Value{Type::Int, fn.nextValue++};
            fn.blocks[tileBlk].ops.push_back({Op::Load, nInTile, {}, {}, p.boundLoadSlot, p.span});
        }
        const Value pTile{Type::Int, fn.nextValue++};
        Operation tr{Op::TileRepeat, {Type::Void, fn.nextValue++}, {arrV, nInTile, pTile}, {}, 0, p.span};
        tr.elemBits = p.elemBits;
        fn.blocks[tileBlk].ops.push_back({Op::Load, arrV, {}, {}, p.arrSlot, p.span});
        fn.blocks[tileBlk].ops.push_back({Op::Int, pTile, {}, std::to_string(p.period), 0, p.span});
        fn.blocks[tileBlk].ops.push_back(std::move(tr));
        fn.blocks[tileBlk].exit = Exit::Jump;
        fn.blocks[tileBlk].yes = p.exitBlk;
    }
}

/// Strip-mine `i64` additive reduction loops `acc += term` with bounded `|term| <= 2097151`
/// into `1024`-iteration chunks accumulated in `i32`, unlocking AVX-512 `vpmaddwd`/`vpaddd`.
inline void chunkNarrowReduction(FunctionIR& fn,
                                 const std::unordered_map<unsigned, IntRange>& valRanges) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    const std::size_t nSlots = fn.slots.size();
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

    struct Plan {
        unsigned pre = 0, hdr = 0, body = 0, exitBlk = 0;
        unsigned jSlot = 0, accSlot = 0;
        Value jLower{}, jUpper{};
        std::size_t accAddIdx = 0;
        std::size_t accStoreIdx = 0;
        Value termVal{};
        Span span{};
        bool cloneUpperLoad = false;
        unsigned upperLoadSlot = 0;
    };
    std::vector<Plan> plans;
    std::unordered_set<unsigned> usedHeaders;

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lp = cfg.loops[li];
        if (lp.body.size() != 2 || lp.preheaders.size() != 1) continue;
        const unsigned hdr = lp.header;
        const unsigned pre = lp.preheaders[0];
        if (usedHeaders.contains(hdr)) continue;
        const auto& hblk = fn.blocks[hdr];
        if (hblk.exit != Exit::Branch || !hblk.value || !lp.body.contains(hblk.yes) || lp.body.contains(hblk.no))
            continue;
        if (fn.blocks[pre].exit != Exit::Jump || fn.blocks[pre].yes != hdr) continue;
        const unsigned body = hblk.yes;
        const unsigned exitBlk = hblk.no;
        if (fn.blocks[body].exit != Exit::Jump || fn.blocks[body].yes != hdr) continue;

        auto cmpIt = defOp.find(hblk.value->id);
        if (cmpIt == defOp.end() || cmpIt->second->op != Op::Binary || cmpIt->second->data != "<") continue;
        auto jls = loadSlot.find(cmpIt->second->inputs[0].id);
        if (jls == loadSlot.end()) continue;
        const unsigned jSlot = jls->second;
        const Value jUpper = cmpIt->second->inputs[1];
        if (jUpper.type != Type::Int) continue;

        bool cloneUpperLoad = false;
        unsigned upperLoadSlot = 0;
        if (auto udb = defBlock.find(jUpper.id); udb != defBlock.end() && lp.body.contains(udb->second)) {
            auto ud = defOp.find(jUpper.id);
            if (ud == defOp.end() || ud->second->op != Op::Load || ud->second->slot == jSlot) continue;
            cloneUpperLoad = true;
            upperLoadSlot = ud->second->slot;
        }

        std::optional<Value> jLower;
        for (const auto& op : fn.blocks[pre].ops) {
            if (op.op == Op::Store && op.slot == jSlot && !op.inputs.empty()) jLower = op.inputs[0];
        }
        if (!jLower) continue;

        auto loR = valRanges.find(jLower->id);
        auto hiR = valRanges.find(jUpper.id);
        auto loC = (defOp.count(jLower->id) && defOp.at(jLower->id)->op == Op::Int)
            ? parseConstInt(defOp.at(jLower->id)->data) : std::nullopt;
        if (loC) {
            if (*loC < -1000000000LL || *loC > 1000000000LL) continue;
        } else if (loR == valRanges.end() || loR->second.lower < -1000000000LL || loR->second.upper > 1000000000LL) {
            continue;
        }
        if (hiR == valRanges.end() || hiR->second.lower < -1000000000LL || hiR->second.upper > 1000000000LL) {
            continue;
        }

        std::unordered_set<unsigned> writtenInLoop;
        for (unsigned lb : lp.body) {
            for (const auto& op : fn.blocks[lb].ops) {
                if (op.op == Op::Store && op.slot < nSlots) writtenInLoop.insert(op.slot);
                for (unsigned ms : op.mutArgSlots) {
                    if (ms < nSlots) writtenInLoop.insert(ms);
                }
            }
        }
        if (cloneUpperLoad && writtenInLoop.contains(upperLoadSlot)) continue;

        // Check body safety + identify single loop-carried accSlot (+ local temp stores allowed)
        bool okBody = true;
        int jIncs = 0;
        std::unordered_set<unsigned> localTempInitialized;
        std::optional<unsigned> accSlot;
        std::optional<std::size_t> accStoreIdx, accAddIdx;
        Value termVal{};
        const auto& bops = fn.blocks[body].ops;
        for (std::size_t oi = 0; oi < bops.size(); ++oi) {
            const auto& op = bops[oi];
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::FnPtr || op.op == Op::Builtin ||
                op.op == Op::BoxDyn || op.op == Op::DynCall || op.op == Op::MakeClosure || op.op == Op::ClosureCall ||
                op.op == Op::Print || op.op == Op::IndexSet || op.op == Op::TileRepeat || op.op == Op::FindFirst ||
                op.op == Op::GemmF32 || op.op == Op::MatMulMod ||
                op.op == Op::FloydStepU16 || op.op == Op::ArgMinU16 ||
                op.op == Op::ArrayLit || op.op == Op::Interp || op.op == Op::Slice ||
                op.op == Op::FieldSet || op.op == Op::PlaceSet || op.op == Op::RepeatHeap ||
                op.op == Op::RepeatRegion || op.op == Op::Clone || op.op == Op::ClearOwner ||
                op.op == Op::ClearField || op.op == Op::ClearIndex ||
                (op.op == Op::Assert && !op.boundsProven && !op.unchecked)) {
                okBody = false; break;
            }
            if (op.op == Op::Index && !op.boundsProven) { okBody = false; break; }
            if ((op.op == Op::Negate || op.op == Op::AbsDiff) && op.result.type == Type::Int && !op.overflowProven) {
                okBody = false; break;
            }
            if (op.op == Op::Binary && !op.inputs.empty() && op.inputs[0].type == Type::Int) {
                if ((op.data == "+" || op.data == "-" || op.data == "*" ||
                     op.data == "<<" || op.data == ">>" || op.data == ">>>") && !op.overflowProven) {
                    okBody = false; break;
                }
                if ((op.data == "/" || op.data == "%") && !op.divSafe) { okBody = false; break; }
            }
            if (op.op == Op::Store) {
                if (op.slot == jSlot) {
                    ++jIncs;
                    auto kd = defOp.find(op.inputs[0].id);
                    if (kd == defOp.end() || kd->second->op != Op::Binary ||
                        (kd->second->data != "rangeIncrement" && kd->second->data != "+") ||
                        !kd->second->overflowProven) {
                        okBody = false; break;
                    }
                    auto kls = loadSlot.find(kd->second->inputs[0].id);
                    auto kc = defOp.find(kd->second->inputs[1].id);
                    auto c1 = (kc != defOp.end() && kc->second->op == Op::Int) ? parseConstInt(kc->second->data) : std::nullopt;
                    if (kls == loadSlot.end() || kls->second != jSlot || c1 != 1) { okBody = false; break; }
                } else if (fn.slots[op.slot] == Type::Int && !op.inputs.empty()) {
                    auto ad = defOp.find(op.inputs[0].id);
                    bool matchedAcc = false;
                    if (ad != defOp.end() && ad->second->op == Op::Binary && ad->second->data == "+" &&
                        ad->second->overflowProven && ad->second->width == 64 && ad->second->inputs.size() == 2) {
                        for (int side = 0; side < 2; ++side) {
                            auto als = loadSlot.find(ad->second->inputs[side].id);
                            if (als != loadSlot.end() && als->second == op.slot) {
                                matchedAcc = true;
                                termVal = ad->second->inputs[1 - side];
                                break;
                            }
                        }
                    }
                    if (matchedAcc && !localTempInitialized.contains(op.slot)) {
                        if (accSlot) { okBody = false; break; }
                        accSlot = op.slot;
                        accStoreIdx = oi;
                        for (std::size_t ai = 0; ai < oi; ++ai) {
                            if (bops[ai].result.id == ad->second->result.id) { accAddIdx = ai; break; }
                        }
                        if (!accAddIdx) { okBody = false; break; }
                    } else {
                        localTempInitialized.insert(op.slot);
                    }
                } else {
                    okBody = false; break;
                }
            }
        }
        if (!okBody || jIncs != 1 || !accSlot || !accAddIdx || !accStoreIdx) continue;
        // Ensure accSlot only read once (in accAdd op), accAdd result used only once (in accStore), and not read in hdr
        int accReads = 0;
        const unsigned accAddResId = bops[*accAddIdx].result.id;
        int accAddUses = 0;
        for (const auto& blk : fn.blocks) {
            for (const auto& op : blk.ops) {
                for (const auto& in : op.inputs) {
                    if (in.id == accAddResId) ++accAddUses;
                }
            }
            if (blk.value && blk.value->id == accAddResId) ++accAddUses;
        }
        if (accAddUses != 1) okBody = false;
        for (const auto& op : bops) {
            if (op.op == Op::Load && op.slot == *accSlot) {
                ++accReads;
                if (op.result.id != bops[*accAddIdx].inputs[0].id &&
                    op.result.id != bops[*accAddIdx].inputs[1].id) {
                    okBody = false; break;
                }
            }
        }
        for (const auto& op : hblk.ops) {
            if (op.op == Op::Load && (op.slot == *accSlot || (writtenInLoop.contains(op.slot) && op.slot != jSlot))) {
                okBody = false; break;
            }
        }
        if (!okBody || accReads != 1) continue;

        auto tr = valRanges.find(termVal.id);
        if (tr == valRanges.end()) continue;
        if (tr->second.lower < -2097151LL || tr->second.upper > 2097151LL) continue;

        usedHeaders.insert(hdr);
        plans.push_back({pre, hdr, body, exitBlk, jSlot, *accSlot, *jLower, jUpper,
                         *accAddIdx, *accStoreIdx, termVal, bops[*accAddIdx].span,
                         cloneUpperLoad, upperLoadSlot});
    }

    for (const auto& p : plans) {
        if (fn.blocks[p.pre].exit != Exit::Jump || fn.blocks[p.pre].yes != p.hdr) continue;
        const unsigned chunkStartSlot = detail::allocOptSlot(fn, Type::Int);
        const unsigned chunkEndSlot = detail::allocOptSlot(fn, Type::Int);
        const unsigned chunkAccSlot = detail::allocOptSlot(fn, Type::I32);

        const unsigned outerHdr = static_cast<unsigned>(fn.blocks.size());
        const unsigned outerBody = outerHdr + 1;
        const unsigned outerLatch = outerHdr + 2;
        fn.blocks.resize(outerLatch + 1);

        // pre: jump -> outerHdr, initialize chunkStartSlot = jLower
        fn.blocks[p.pre].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {p.jLower}, {}, chunkStartSlot, p.span});
        fn.blocks[p.pre].exit = Exit::Jump;
        fn.blocks[p.pre].yes = outerHdr;

        // outerHdr: chunkStart < jUpper -> outerBody else exitBlk
        {
            const Value cs{Type::Int, fn.nextValue++};
            Value hiV = p.jUpper;
            if (p.cloneUpperLoad) {
                hiV = Value{Type::Int, fn.nextValue++};
                fn.blocks[outerHdr].ops.push_back({Op::Load, hiV, {}, {}, p.upperLoadSlot, p.span});
            }
            const Value cmp{Type::Bool, fn.nextValue++};
            fn.blocks[outerHdr].ops.push_back({Op::Load, cs, {}, {}, chunkStartSlot, p.span});
            fn.blocks[outerHdr].ops.push_back({Op::Binary, cmp, {cs, hiV}, "<", 0, p.span});
            fn.blocks[outerHdr].exit = Exit::Branch;
            fn.blocks[outerHdr].value = cmp;
            fn.blocks[outerHdr].yes = outerBody;
            fn.blocks[outerHdr].no = p.exitBlk;
        }

        // outerBody: c1024 = 1024; cand = cs + 1024; ce = min(hi, cand); jSlot = cs; chunkAccSlot = 0 (i32) -> hdr
        {
            const Value cs{Type::Int, fn.nextValue++};
            const Value c1024{Type::Int, fn.nextValue++};
            const Value cand{Type::Int, fn.nextValue++};
            Value hiV = p.jUpper;
            if (p.cloneUpperLoad) {
                hiV = Value{Type::Int, fn.nextValue++};
                fn.blocks[outerBody].ops.push_back({Op::Load, hiV, {}, {}, p.upperLoadSlot, p.span});
            }
            const Value ce{Type::Int, fn.nextValue++};
            const Value z0{Type::I32, fn.nextValue++};
            Operation add1024{Op::Binary, cand, {cs, c1024}, "+", 0, p.span};
            add1024.overflowProven = true;
            Operation minEnd{Op::Min, ce, {hiV, cand}, {}, 0, p.span};
            minEnd.width = 64;
            fn.blocks[outerBody].ops.push_back({Op::Load, cs, {}, {}, chunkStartSlot, p.span});
            fn.blocks[outerBody].ops.push_back({Op::Int, c1024, {}, "1024", 0, p.span});
            fn.blocks[outerBody].ops.push_back(std::move(add1024));
            fn.blocks[outerBody].ops.push_back(std::move(minEnd));
            fn.blocks[outerBody].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {ce}, {}, chunkEndSlot, p.span});
            fn.blocks[outerBody].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {cs}, {}, p.jSlot, p.span});
            fn.blocks[outerBody].ops.push_back({Op::Int, z0, {}, "0", 0, p.span});
            fn.blocks[outerBody].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z0}, {}, chunkAccSlot, p.span});
            fn.blocks[outerBody].exit = Exit::Jump;
            fn.blocks[outerBody].yes = p.hdr;
        }

        // hdr: load chunkEndSlot instead of jUpper, branch no -> outerLatch
        {
            const Value ceH{Type::Int, fn.nextValue++};
            auto& hops = fn.blocks[p.hdr].ops;
            for (auto& op : hops) {
                if (op.op == Op::Binary && op.data == "<" && op.inputs.size() >= 2 &&
                    op.inputs[1].id == p.jUpper.id) {
                    op.inputs[1] = ceH;
                    break;
                }
            }
            hops.insert(hops.begin(), {Op::Load, ceH, {}, {}, chunkEndSlot, p.span});
            fn.blocks[p.hdr].no = outerLatch;
        }

        // body: switch accSlot -> chunkAccSlot (Type::I32), narrow eligible term ops to width = 16 or 32
        {
            auto& bops = fn.blocks[p.body].ops;
            std::unordered_set<unsigned> narrowTermIds{p.termVal.id};
            std::unordered_set<unsigned> tempNarrowSlots;
            bool chg = true;
            while (chg) {
                chg = false;
                for (std::size_t oi = bops.size(); oi-- > 0;) {
                    auto& op = bops[oi];
                    if (op.op == Op::Store && op.slot != p.jSlot && op.slot != p.accSlot &&
                        tempNarrowSlots.contains(op.slot) && !op.inputs.empty()) {
                        if (narrowTermIds.insert(op.inputs[0].id).second) chg = true;
                    }
                    if (op.result.type == Type::Int && narrowTermIds.contains(op.result.id)) {
                        if (op.op == Op::Load && op.slot != p.jSlot && op.slot != p.accSlot) {
                            tempNarrowSlots.insert(op.slot);
                        } else if ((op.width == 64 || op.width == 32) &&
                                   (op.op == Op::Negate || op.op == Op::AbsDiff || op.op == Op::Min || op.op == Op::Max ||
                                    (op.op == Op::Binary &&
                                     (op.data == "+" || op.data == "-" || op.data == "*")))) {
                            auto rOut = valRanges.find(op.result.id);
                            bool allFit16 = (rOut != valRanges.end() && fitsSigned16(rOut->second));
                            bool allFit32 = (rOut != valRanges.end() && fitsSigned32(rOut->second));
                            for (const auto& in : op.inputs) {
                                auto rIn = valRanges.find(in.id);
                                if (rIn == valRanges.end()) {
                                    allFit16 = false;
                                    allFit32 = false;
                                    break;
                                }
                                if (!fitsSigned16(rIn->second)) allFit16 = false;
                                if (!fitsSigned32(rIn->second)) allFit32 = false;
                            }
                            if (allFit16) {
                                op.width = 16;
                                for (const auto& in : op.inputs) {
                                    if (narrowTermIds.insert(in.id).second) chg = true;
                                }
                            } else if (allFit32 && op.width == 64) {
                                op.width = 32;
                                for (const auto& in : op.inputs) {
                                    if (narrowTermIds.insert(in.id).second) chg = true;
                                }
                            }
                        }
                    }
                }
            }
            std::optional<Value> accLoad32;
            std::optional<Value> accAdd32;
            std::vector<Operation> rebuilt;
            rebuilt.reserve(bops.size() + 1);
            for (std::size_t oi = 0; oi < bops.size(); ++oi) {
                Operation op = std::move(bops[oi]);
                if (op.op == Op::Load && op.slot == p.accSlot) {
                    op.slot = chunkAccSlot;
                    op.result.type = Type::I32;
                    accLoad32 = op.result;
                    rebuilt.push_back(std::move(op));
                    continue;
                }
                if (oi == p.accAddIdx && accLoad32) {
                    const Value term32{Type::I32, fn.nextValue++};
                    rebuilt.push_back({Op::TruncI32, term32, {p.termVal}, {}, 0, p.span});
                    op.result.type = Type::I32;
                    op.inputs = {*accLoad32, term32};
                    op.overflowProven = true;
                    op.width = 32;
                    accAdd32 = op.result;
                    rebuilt.push_back(std::move(op));
                    continue;
                }
                if (oi == p.accStoreIdx && accAdd32) {
                    op.slot = chunkAccSlot;
                    op.inputs[0] = *accAdd32;
                    rebuilt.push_back(std::move(op));
                    continue;
                }
                if (op.op == Op::Store && op.slot == p.accSlot) {
                    op.slot = chunkAccSlot;
                }
                rebuilt.push_back(std::move(op));
            }
            bops = std::move(rebuilt);
            fn.blocks[p.body].loopHint = LoopHint::Vectorize;
        }

        // outerLatch: accSlot += sext(chunkAccSlot); chunkStartSlot = chunkEndSlot -> outerHdr
        {
            const Value acc{Type::Int, fn.nextValue++};
            const Value cAcc{Type::I32, fn.nextValue++};
            const Value cAcc64{Type::Int, fn.nextValue++};
            const Value nextAcc{Type::Int, fn.nextValue++};
            const Value ce{Type::Int, fn.nextValue++};
            Operation addAcc{Op::Binary, nextAcc, {acc, cAcc64}, "+", 0, p.span};
            addAcc.overflowProven = true;
            addAcc.width = 64;
            fn.blocks[outerLatch].ops.push_back({Op::Load, acc, {}, {}, p.accSlot, p.span});
            fn.blocks[outerLatch].ops.push_back({Op::Load, cAcc, {}, {}, chunkAccSlot, p.span});
            fn.blocks[outerLatch].ops.push_back({Op::SextI64, cAcc64, {cAcc}, {}, 0, p.span});
            fn.blocks[outerLatch].ops.push_back(std::move(addAcc));
            fn.blocks[outerLatch].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {nextAcc}, {}, p.accSlot, p.span});
            fn.blocks[outerLatch].ops.push_back({Op::Load, ce, {}, {}, chunkEndSlot, p.span});
            fn.blocks[outerLatch].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {ce}, {}, chunkStartSlot, p.span});
            fn.blocks[outerLatch].exit = Exit::Jump;
            fn.blocks[outerLatch].yes = outerHdr;
        }
    }
}

/// Replace 3-nested `i -> j -> k` integer matmul-mod (`vMatrixMultiply`) for bounded `N <= min(512, safeMaxK32)`
/// with a 4-row `i-k-j` strip algorithm or `@tz.matmul_mod.u16`. Returns true when any loop nest was rewritten.
inline bool interchangeRowStripMatMul(
    FunctionIR& fn,
    const std::unordered_map<unsigned, std::optional<IntRange>>& elemRanges) {
    if (fn.blocks.empty() || !fn.usesRegion) return false;
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
                }
            }
        }
    }

    struct Plan {
        unsigned preI = 0, hdrI = 0, hdrJ = 0, jInit = 0, hdrK = 0, bodyK = 0, jTail = 0, iExit = 0;
        unsigned iSlot = 0, jSlot = 0, kSlot = 0, nSlot = 0, aSlot = 0, bSlot = 0, accSlot = 0, totalSlot = 0;
        Value modVal{};
        std::int64_t modConst = 0;
        std::int64_t safeMaxN = 0;
        std::int64_t maxProd = 1;
        std::int64_t maxA = 0, maxB = 0;
        Operation aIdxTemplate{}, bIdxTemplate{};
        Span sp{};
    };
    std::vector<Plan> plans;
    std::unordered_set<unsigned> usedPre;

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lpI = cfg.loops[li];
        if (lpI.preheaders.size() != 1) continue;
        const unsigned preI = lpI.preheaders[0];
        const unsigned hdrI = lpI.header;
        if (usedPre.contains(preI) || fn.blocks[preI].exit != Exit::Jump || fn.blocks[preI].yes != hdrI) continue;
        const auto& hIBlk = fn.blocks[hdrI];
        if (hIBlk.exit != Exit::Branch || !hIBlk.value || !lpI.body.contains(hIBlk.yes) || lpI.body.contains(hIBlk.no))
            continue;
        const unsigned iExit = hIBlk.no;
        auto cI = defOp.find(hIBlk.value->id);
        if (cI == defOp.end() || cI->second->op != Op::Binary || cI->second->data != "<") continue;
        auto ils = loadSlot.find(cI->second->inputs[0].id);
        auto nls = loadSlot.find(cI->second->inputs[1].id);
        if (ils == loadSlot.end() || nls == loadSlot.end()) continue;
        const unsigned iSlot = ils->second;
        const unsigned nSlot = nls->second;
        std::optional<std::int64_t> iLowC;
        for (const auto& op : fn.blocks[preI].ops) {
            if (op.op == Op::Store && op.slot == iSlot && !op.inputs.empty()) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && d->second->op == Op::Int) iLowC = parseConstInt(d->second->data);
            }
        }
        if (iLowC != 0) continue;

        bool hasIBodyPre = false;
        unsigned iBodyPre = hdrI;
        unsigned hdrJ = hIBlk.yes;
        if (!cfg.headerToLoop.contains(hdrJ) && fn.blocks[hdrJ].exit == Exit::Jump && lpI.body.contains(hdrJ)) {
            hasIBodyPre = true;
            iBodyPre = hdrJ;
            hdrJ = fn.blocks[iBodyPre].yes;
        }
        auto litJ = cfg.headerToLoop.find(hdrJ);
        if (litJ == cfg.headerToLoop.end() || litJ->second == li) continue;
        const auto& lpJ = cfg.loops[litJ->second];
        const auto& hJBlk = fn.blocks[hdrJ];
        if (hJBlk.exit != Exit::Branch || !hJBlk.value || !lpJ.body.contains(hJBlk.yes) || lpJ.body.contains(hJBlk.no))
            continue;
        auto cJ = defOp.find(hJBlk.value->id);
        if (cJ == defOp.end() || cJ->second->op != Op::Binary || cJ->second->data != "<") continue;
        auto jls = loadSlot.find(cJ->second->inputs[0].id);
        auto jnls = loadSlot.find(cJ->second->inputs[1].id);
        if (jls == loadSlot.end() || jnls == loadSlot.end() || jnls->second != nSlot || jls->second == iSlot) continue;
        const unsigned jSlot = jls->second;
        const unsigned jInit = hJBlk.yes;
        const unsigned iLatch = hJBlk.no;
        if (fn.blocks[jInit].exit != Exit::Jump) continue;
        const unsigned hdrK = fn.blocks[jInit].yes;
        auto litK = cfg.headerToLoop.find(hdrK);
        if (litK == cfg.headerToLoop.end() || litK->second == litJ->second) continue;
        const auto& lpK = cfg.loops[litK->second];
        if (lpK.body.size() != 2 || lpK.preheaders.size() != 1 || lpK.preheaders[0] != jInit) continue;
        const auto& hKBlk = fn.blocks[hdrK];
        if (hKBlk.exit != Exit::Branch || !hKBlk.value || !lpK.body.contains(hKBlk.yes) || lpK.body.contains(hKBlk.no))
            continue;
        const unsigned bodyK = hKBlk.yes;
        const unsigned jTail = hKBlk.no;
        if (fn.blocks[bodyK].exit != Exit::Jump || fn.blocks[bodyK].yes != hdrK) continue;
        if (fn.blocks[jTail].exit != Exit::Jump || fn.blocks[jTail].yes != hdrJ) continue;
        if (fn.blocks[iLatch].exit != Exit::Jump || fn.blocks[iLatch].yes != hdrI) continue;
        if (lpI.body.size() != (hasIBodyPre ? 8u : 7u) ||
            !lpI.body.contains(iLatch) || !lpI.body.contains(jTail) || !lpI.body.contains(bodyK))
            continue;

        bool okHI = true;
        std::optional<std::int64_t> jLowC;
        auto scanJInitOps = [&](const std::vector<Operation>& ops, bool allowNoStore) {
            for (const auto& op : ops) {
                if (op.op == Op::Store) {
                    if (op.slot == jSlot && !op.inputs.empty()) {
                        auto d = defOp.find(op.inputs[0].id);
                        if (d != defOp.end() && d->second->op == Op::Int) jLowC = parseConstInt(d->second->data);
                    } else okHI = false;
                } else if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary) {
                    okHI = false;
                }
            }
            (void)allowNoStore;
        };
        if (hasIBodyPre) {
            for (const auto& op : hIBlk.ops) {
                if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary) { okHI = false; break; }
            }
            if (okHI) scanJInitOps(fn.blocks[iBodyPre].ops, false);
        } else {
            scanJInitOps(hIBlk.ops, false);
        }
        if (!okHI || jLowC != 0) continue;

        auto cK = defOp.find(hKBlk.value->id);
        if (cK == defOp.end() || cK->second->op != Op::Binary || cK->second->data != "<") continue;
        auto kls = loadSlot.find(cK->second->inputs[0].id);
        auto knls = loadSlot.find(cK->second->inputs[1].id);
        if (kls == loadSlot.end() || knls == loadSlot.end() || knls->second != nSlot || kls->second == iSlot || kls->second == jSlot)
            continue;
        const unsigned kSlot = kls->second;

        // Inspect jInit: kSlot = 0, accSlot = 0 (allow loading loop-invariant nSlot for hdrK)
        std::optional<unsigned> accSlot;
        std::optional<std::int64_t> kLowC, accZeroC;
        bool okInit = true;
        for (const auto& op : fn.blocks[jInit].ops) {
            if (op.op == Op::Store && !op.inputs.empty()) {
                auto d = defOp.find(op.inputs[0].id);
                auto c = (d != defOp.end() && d->second->op == Op::Int) ? parseConstInt(d->second->data) : std::nullopt;
                if (op.slot == kSlot) kLowC = c;
                else if (!accSlot && fn.slots[op.slot] == Type::Int) { accSlot = op.slot; accZeroC = c; }
                else { okInit = false; break; }
            } else if (op.op != Op::Int &&
                       !(op.op == Op::Load && (op.slot == nSlot || op.slot == iSlot || op.slot == jSlot))) {
                okInit = false; break;
            }
        }
        if (!okInit || !accSlot || kLowC != 0 || accZeroC != 0) continue;

        // Inspect iLatch: only iSlot += 1
        int iIncs = 0;
        bool okILatch = true;
        for (const auto& op : fn.blocks[iLatch].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Index || op.op == Op::IndexSet ||
                op.op == Op::Print || op.op == Op::Assert) { okILatch = false; break; }
            if (op.op == Op::Store) {
                if (op.slot == iSlot) ++iIncs;
                else { okILatch = false; break; }
            }
        }
        if (!okILatch || iIncs != 1) continue;

        // Inspect bodyK: accSlot = accSlot + a[i*n + k] * b[k*n + j], kSlot += 1
        auto resolveRowCol = [&](Value idxV, unsigned rowSlot, unsigned colSlot) -> bool {
            auto addIt = defOp.find(idxV.id);
            if (addIt == defOp.end() || addIt->second->op != Op::Binary || addIt->second->data != "+") return false;
            auto mulIt = defOp.find(addIt->second->inputs[0].id);
            auto cIt = loadSlot.find(addIt->second->inputs[1].id);
            if (mulIt == defOp.end() || mulIt->second->op != Op::Binary || mulIt->second->data != "*" ||
                cIt == loadSlot.end() || cIt->second != colSlot) return false;
            auto r0 = loadSlot.find(mulIt->second->inputs[0].id);
            auto n0 = loadSlot.find(mulIt->second->inputs[1].id);
            if (r0 != loadSlot.end() && n0 != loadSlot.end() &&
                ((r0->second == rowSlot && n0->second == nSlot) || (r0->second == nSlot && n0->second == rowSlot)))
                return true;
            return false;
        };

        std::optional<unsigned> aSlot, bSlot;
        Operation aTmpl{}, bTmpl{};
        bool matchedAcc = false;
        int kIncs = 0;
        bool okBodyK = true;
        for (const auto& op : fn.blocks[bodyK].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::IndexSet || op.op == Op::Builtin ||
                op.op == Op::Print || op.op == Op::Assert) { okBodyK = false; break; }
            if (op.op == Op::Index && !op.boundsProven) { okBodyK = false; break; }
            if (op.op == Op::Binary && !op.overflowProven) { okBodyK = false; break; }
            if (op.op == Op::Store) {
                if (op.slot == kSlot) { ++kIncs; }
                else if (op.slot == *accSlot && !matchedAcc && !op.inputs.empty()) {
                    auto addOp = defOp.find(op.inputs[0].id);
                    if (addOp == defOp.end() || addOp->second->op != Op::Binary || addOp->second->data != "+") {
                        okBodyK = false; break;
                    }
                    for (int side = 0; side < 2; ++side) {
                        auto als = loadSlot.find(addOp->second->inputs[side].id);
                        if (als != loadSlot.end() && als->second == *accSlot) {
                            auto mulOp = defOp.find(addOp->second->inputs[1 - side].id);
                            if (mulOp != defOp.end() && mulOp->second->op == Op::Binary && mulOp->second->data == "*") {
                                auto d0 = defOp.find(mulOp->second->inputs[0].id);
                                auto d1 = defOp.find(mulOp->second->inputs[1].id);
                                if (d0 != defOp.end() && d0->second->op == Op::Index &&
                                    d1 != defOp.end() && d1->second->op == Op::Index) {
                                    auto s0 = loadSlot.find(d0->second->inputs[0].id);
                                    auto s1 = loadSlot.find(d1->second->inputs[0].id);
                                    if (s0 != loadSlot.end() && s1 != loadSlot.end()) {
                                        if (resolveRowCol(d0->second->inputs[1], iSlot, kSlot) &&
                                            resolveRowCol(d1->second->inputs[1], kSlot, jSlot)) {
                                            aSlot = s0->second; bSlot = s1->second;
                                            aTmpl = *d0->second; bTmpl = *d1->second;
                                            matchedAcc = true;
                                        } else if (resolveRowCol(d1->second->inputs[1], iSlot, kSlot) &&
                                                   resolveRowCol(d0->second->inputs[1], kSlot, jSlot)) {
                                            aSlot = s1->second; bSlot = s0->second;
                                            aTmpl = *d1->second; bTmpl = *d0->second;
                                            matchedAcc = true;
                                        }
                                    }
                                }
                            }
                            break;
                        }
                    }
                    if (!matchedAcc) { okBodyK = false; break; }
                } else { okBodyK = false; break; }
            }
        }
        if (!okBodyK || !matchedAcc || kIncs != 1 || !aSlot || !bSlot) continue;
        if (*aSlot >= nSlots || *bSlot >= nSlots || originCount[*aSlot] != 1 || originCount[*bSlot] != 1 ||
            badArr[*aSlot] || badArr[*bSlot] || *aSlot == *bSlot) continue;

        // Inspect jTail: totalSlot = totalSlot + (accSlot % M), jSlot += 1
        std::optional<unsigned> totalSlot;
        Value modVal{};
        std::int64_t modConst = 0;
        int jIncs = 0;
        bool okTail = true;
        for (const auto& op : fn.blocks[jTail].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Index || op.op == Op::IndexSet ||
                op.op == Op::Print || op.op == Op::Assert) { okTail = false; break; }
            if (op.op == Op::Store) {
                if (op.slot == jSlot) { ++jIncs; }
                else if (!totalSlot && fn.slots[op.slot] == Type::Int && op.slot != nSlot &&
                         op.slot != iSlot && op.slot != kSlot && op.slot != *accSlot && !op.inputs.empty()) {
                    auto addT = defOp.find(op.inputs[0].id);
                    if (addT != defOp.end() && addT->second->op == Op::Binary && addT->second->data == "+" &&
                        addT->second->overflowProven) {
                        for (int side = 0; side < 2; ++side) {
                            auto tls = loadSlot.find(addT->second->inputs[side].id);
                            if (tls != loadSlot.end() && tls->second == op.slot) {
                                auto remOp = defOp.find(addT->second->inputs[1 - side].id);
                                if (remOp != defOp.end() && remOp->second->op == Op::Binary && remOp->second->data == "%" &&
                                    remOp->second->divSafe && remOp->second->inputs.size() >= 2) {
                                    auto als = loadSlot.find(remOp->second->inputs[0].id);
                                    if (als != loadSlot.end() && als->second == *accSlot) {
                                        auto mc = defOp.find(remOp->second->inputs[1].id);
                                        if (mc != defOp.end() && mc->second->op == Op::Int) {
                                            if (auto mv = parseConstInt(mc->second->data); mv && *mv >= 2) {
                                                modVal = remOp->second->inputs[1];
                                                modConst = *mv;
                                                totalSlot = op.slot;
                                            }
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                    if (!totalSlot) { okTail = false; break; }
                } else { okTail = false; break; }
            }
        }
        if (!okTail || !totalSlot || jIncs != 1) continue;

        auto erA = elemRanges.find(*aSlot);
        auto erB = elemRanges.find(*bSlot);
        if (erA == elemRanges.end() || !erA->second || erA->second->lower < 0 ||
            erB == elemRanges.end() || !erB->second || erB->second->lower < 0) continue;
        const std::int64_t maxA = erA->second->upper;
        const std::int64_t maxB = erB->second->upper;
        const std::int64_t maxProd = maxA * maxB;
        if (maxProd <= 0 || maxA > 46340 || maxB > 46340) continue;
        const std::int64_t safeMaxN = std::min<std::int64_t>(512, 2147483647LL / maxProd);
        if (safeMaxN < 16) continue;

        usedPre.insert(preI);
        plans.push_back({preI, hdrI, hdrJ, jInit, hdrK, bodyK, jTail, iExit,
                         iSlot, jSlot, kSlot, nSlot, *aSlot, *bSlot, *accSlot, *totalSlot,
                         modVal, modConst, safeMaxN, maxProd, maxA, maxB,
                         aTmpl, bTmpl, hIBlk.ops.empty() ? Span{} : hIBlk.ops.front().span});
    }

    bool anyRewritten = false;
    for (const auto& p : plans) {
        if (fn.blocks[p.preI].exit != Exit::Jump || fn.blocks[p.preI].yes != p.hdrI) continue;
        anyRewritten = true;

        const std::int64_t lhsUpperBound = std::min<std::int64_t>(2147483647LL, p.safeMaxN * p.maxProd);
        if (p.aIdxTemplate.elemBits == 16 && p.bIdxTemplate.elemBits == 16 &&
            p.maxA <= 32767 && p.maxB <= 32767 &&
            p.modConst >= 2 && p.modConst <= 2147483647LL) {
            const unsigned fastBlk = static_cast<unsigned>(fn.blocks.size());
            fn.blocks.emplace_back();

            const Value nv{Type::Int, fn.nextValue++};
            const Value c1{Type::Int, fn.nextValue++};
            const Value cMax{Type::Int, fn.nextValue++};
            const Value g1{Type::Bool, fn.nextValue++};
            const Value g2{Type::Bool, fn.nextValue++};
            const Value gOk{Type::Bool, fn.nextValue++};
            fn.blocks[p.preI].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, c1, {}, "1", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, cMax, {}, std::to_string(p.safeMaxN), 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, g1, {nv, c1}, ">=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, g2, {nv, cMax}, "<=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, gOk, {g1, g2}, "&", 0, p.sp});
            fn.blocks[p.preI].exit = Exit::Branch;
            fn.blocks[p.preI].value = gOk;
            fn.blocks[p.preI].yes = fastBlk;
            fn.blocks[p.preI].no = p.hdrI;

            const Value av{Type::IntArray, fn.nextValue++};
            const Value bv{Type::IntArray, fn.nextValue++};
            const Value n2{Type::Int, fn.nextValue++};
            const Value modC{Type::Int, fn.nextValue++};
            const Value mSum{Type::Int, fn.nextValue++};
            const Value tOld{Type::Int, fn.nextValue++};
            const Value tNew{Type::Int, fn.nextValue++};
            Operation mmOp{Op::MatMulMod, mSum, {av, bv, n2, modC}, {}, 0, p.sp};
            mmOp.lhsUpper = lhsUpperBound;
            mmOp.elemBits = 16;
            mmOp.elemUnsigned = true;
            Operation addTot{Op::Binary, tNew, {tOld, mSum}, "+", 0, p.sp};
            addTot.overflowProven = true;
            auto& fops = fn.blocks[fastBlk].ops;
            fops.push_back({Op::Load, av, {}, {}, p.aSlot, p.sp});
            fops.push_back({Op::Load, bv, {}, {}, p.bSlot, p.sp});
            fops.push_back({Op::Load, n2, {}, {}, p.nSlot, p.sp});
            fops.push_back({Op::Int, modC, {}, std::to_string(p.modConst), 0, p.sp});
            fops.push_back(std::move(mmOp));
            fops.push_back({Op::Load, tOld, {}, {}, p.totalSlot, p.sp});
            fops.push_back(std::move(addTot));
            fops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {tNew}, {}, p.totalSlot, p.sp});
            fn.blocks[fastBlk].exit = Exit::Jump;
            fn.blocks[fastBlk].yes = p.iExit;
            continue;
        }

        // Allocate 4 single-origin region i32 row arrays OUTSIDE loops -> distinct aliasGroups + stack arena!
        std::array<unsigned, 4> cSlots{};
        for (int r = 0; r < 4; ++r) {
            cSlots[r] = detail::allocOptSlot(fn, Type::IntArray);
            fn.slotElemBits[cSlots[r]] = 32;
            fn.slotElemUnsigned[cSlots[r]] = false;
        }
        const unsigned r0Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned r1Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned r2Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned r3Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned b0Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned b1Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned b2Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned b3Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned bkSlot = detail::allocOptSlot(fn, Type::Int);
        const unsigned a0Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned a1Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned a2Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned a3Slot = detail::allocOptSlot(fn, Type::Int);
        const unsigned redAccSlot = detail::allocOptSlot(fn, Type::Int);

        const unsigned fastAlloc = static_cast<unsigned>(fn.blocks.size());
        const unsigned i4Hdr = fastAlloc + 1;
        const unsigned k0Init = fastAlloc + 2;
        const unsigned j0Hdr = fastAlloc + 3;
        const unsigned j0Body = fastAlloc + 4;
        const unsigned kLoopInit = fastAlloc + 5;
        const unsigned kLoopHdr = fastAlloc + 6;
        const unsigned kLoopPreJ = fastAlloc + 7;
        const unsigned kLoopJHdr = fastAlloc + 8;
        const unsigned kLoopJBody = fastAlloc + 9;
        const unsigned kLoopStep = fastAlloc + 10;
        const unsigned redInit = fastAlloc + 11;
        const unsigned redHdr = fastAlloc + 12;
        const unsigned redBody = fastAlloc + 13;
        const unsigned i4Step = fastAlloc + 14;
        fn.blocks.resize(i4Step + 1);

        auto makeMul = [&](std::vector<Operation>& ops, Value a, Value b, std::uint8_t w = 64) {
            Value res{Type::Int, fn.nextValue++};
            Operation m{Op::Binary, res, {a, b}, "*", 0, p.sp};
            m.overflowProven = true;
            m.width = w;
            ops.push_back(std::move(m));
            return res;
        };
        auto makeAdd = [&](std::vector<Operation>& ops, Value a, Value b, std::uint8_t w = 64, bool rangeInc = false) {
            Value res{Type::Int, fn.nextValue++};
            Operation m{Op::Binary, res, {a, b}, rangeInc ? "rangeIncrement" : "+", 0, p.sp};
            m.overflowProven = true;
            m.width = w;
            ops.push_back(std::move(m));
            return res;
        };
        auto emitRowIdx = [&](std::vector<Operation>& ops, unsigned arrS, Value idx, bool isRowC, std::uint8_t bits = 32) {
            const Value av{Type::IntArray, fn.nextValue++};
            const Value rv{Type::Int, fn.nextValue++};
            Operation ix = isRowC ? Operation{Op::Index, rv, {av, idx}, {}, 0, p.sp}
                                  : (arrS == p.bSlot ? p.bIdxTemplate : p.aIdxTemplate);
            ix.result = rv;
            ix.inputs = {av, idx};
            ix.boundsProven = true;
            if (isRowC) {
                ix.elemBits = bits;
                ix.elemUnsigned = false;
            }
            ops.push_back({Op::Load, av, {}, {}, arrS, p.sp});
            ops.push_back(std::move(ix));
            return rv;
        };
        auto emitRowSet = [&](std::vector<Operation>& ops, unsigned arrS, Value idx, Value val, std::uint8_t bits = 32) {
            const Value av{Type::IntArray, fn.nextValue++};
            Operation sx{Op::IndexSet, {Type::Void, fn.nextValue++}, {av, idx, val}, {}, 0, p.sp};
            sx.boundsProven = true;
            sx.elemBits = bits;
            sx.elemUnsigned = false;
            ops.push_back({Op::Load, av, {}, {}, arrS, p.sp});
            ops.push_back(std::move(sx));
        };

        // Guard in preI: 1 <= N <= safeMaxN -> fastAlloc else hdrI
        {
            const Value nv{Type::Int, fn.nextValue++};
            const Value c1{Type::Int, fn.nextValue++};
            const Value cMax{Type::Int, fn.nextValue++};
            const Value g1{Type::Bool, fn.nextValue++};
            const Value g2{Type::Bool, fn.nextValue++};
            const Value gOk{Type::Bool, fn.nextValue++};
            fn.blocks[p.preI].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, c1, {}, "1", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, cMax, {}, std::to_string(p.safeMaxN), 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, g1, {nv, c1}, ">=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, g2, {nv, cMax}, "<=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, gOk, {g1, g2}, "&", 0, p.sp});
            fn.blocks[p.preI].exit = Exit::Branch;
            fn.blocks[p.preI].value = gOk;
            fn.blocks[p.preI].yes = fastAlloc;
            fn.blocks[p.preI].no = p.hdrI;
        }

        // fastAlloc: allocate c0..c3 (i32, initialize=false) once outside loops -> i4Hdr
        {
            const Value nv{Type::Int, fn.nextValue++};
            const Value z0{Type::Int, fn.nextValue++};
            fn.blocks[fastAlloc].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[fastAlloc].ops.push_back({Op::Int, z0, {}, "0", 0, p.sp});
            for (int r = 0; r < 4; ++r) {
                const Value ca{Type::IntArray, fn.nextValue++};
                Operation rep{Op::RepeatRegion, ca, {z0, nv}, {}, 0, p.sp};
                rep.initialize = false;
                rep.elemBits = 32;
                rep.elemUnsigned = false;
                fn.blocks[fastAlloc].ops.push_back(std::move(rep));
                fn.blocks[fastAlloc].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {ca}, {}, cSlots[r], p.sp});
            }
            fn.blocks[fastAlloc].exit = Exit::Jump;
            fn.blocks[fastAlloc].yes = i4Hdr;
        }

        // i4Hdr: (i + 3) < N -> k0Init else hdrI (falls seamlessly to remainder 1-row loop!)
        {
            const Value iv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value c3{Type::Int, fn.nextValue++};
            const Value i3 = (fn.blocks[i4Hdr].ops.push_back({Op::Load, iv, {}, {}, p.iSlot, p.sp}),
                              fn.blocks[i4Hdr].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp}),
                              fn.blocks[i4Hdr].ops.push_back({Op::Int, c3, {}, "3", 0, p.sp}),
                              makeAdd(fn.blocks[i4Hdr].ops, iv, c3));
            const Value cond{Type::Bool, fn.nextValue++};
            fn.blocks[i4Hdr].ops.push_back({Op::Binary, cond, {i3, nv}, "<", 0, p.sp});
            fn.blocks[i4Hdr].exit = Exit::Branch;
            fn.blocks[i4Hdr].value = cond;
            fn.blocks[i4Hdr].yes = k0Init;
            fn.blocks[i4Hdr].no = p.hdrI;
        }

        // k0Init: r0..3 = i+0..3; b0..3 = r_r * N; a0..3 = a[b_r]; jSlot = 0 -> j0Hdr
        {
            auto& ops = fn.blocks[k0Init].ops;
            const Value iv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            ops.push_back({Op::Load, iv, {}, {}, p.iSlot, p.sp});
            ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            const std::array<unsigned, 4> rSlots{r0Slot, r1Slot, r2Slot, r3Slot};
            const std::array<unsigned, 4> bSlots{b0Slot, b1Slot, b2Slot, b3Slot};
            const std::array<unsigned, 4> a0s{a0Slot, a1Slot, a2Slot, a3Slot};
            for (int r = 0; r < 4; ++r) {
                Value rr = iv;
                if (r > 0) {
                    const Value cr{Type::Int, fn.nextValue++};
                    ops.push_back({Op::Int, cr, {}, std::to_string(r), 0, p.sp});
                    rr = makeAdd(ops, iv, cr);
                }
                ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {rr}, {}, rSlots[r], p.sp});
                Value br = makeMul(ops, rr, nv);
                ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {br}, {}, bSlots[r], p.sp});
                Value av = emitRowIdx(ops, p.aSlot, br, false);
                ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {av}, {}, a0s[r], p.sp});
            }
            const Value z0{Type::Int, fn.nextValue++};
            ops.push_back({Op::Int, z0, {}, "0", 0, p.sp});
            ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z0}, {}, p.jSlot, p.sp});
            fn.blocks[k0Init].exit = Exit::Jump;
            fn.blocks[k0Init].yes = j0Hdr;
        }

        // j0Hdr / j0Body: c_r[j] = a_r * b[j] (k = 0 avoids memset/zeroing pass!)
        {
            const Value jv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value cond{Type::Bool, fn.nextValue++};
            fn.blocks[j0Hdr].ops.push_back({Op::Load, jv, {}, {}, p.jSlot, p.sp});
            fn.blocks[j0Hdr].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[j0Hdr].ops.push_back({Op::Binary, cond, {jv, nv}, "<", 0, p.sp});
            fn.blocks[j0Hdr].exit = Exit::Branch;
            fn.blocks[j0Hdr].value = cond;
            fn.blocks[j0Hdr].yes = j0Body;
            fn.blocks[j0Hdr].no = kLoopInit;

            auto& bops = fn.blocks[j0Body].ops;
            const Value j1{Type::Int, fn.nextValue++};
            bops.push_back({Op::Load, j1, {}, {}, p.jSlot, p.sp});
            const Value bv = emitRowIdx(bops, p.bSlot, j1, false);
            const std::array<unsigned, 4> a0s{a0Slot, a1Slot, a2Slot, a3Slot};
            for (int r = 0; r < 4; ++r) {
                const Value ar{Type::Int, fn.nextValue++};
                bops.push_back({Op::Load, ar, {}, {}, a0s[r], p.sp});
                Value pr = makeMul(bops, ar, bv, 32);
                emitRowSet(bops, cSlots[r], j1, pr, 32);
            }
            const Value one{Type::Int, fn.nextValue++};
            bops.push_back({Op::Int, one, {}, "1", 0, p.sp});
            Value jNext = makeAdd(bops, j1, one, 64, true);
            bops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jNext}, {}, p.jSlot, p.sp});
            fn.blocks[j0Body].exit = Exit::Jump;
            fn.blocks[j0Body].yes = j0Hdr;
            fn.blocks[j0Body].loopHint = LoopHint::VectorizeParallel;
            fn.blocks[j0Body].noInterleave = true;
        }

        // kLoop: for k in 1 .. N
        {
            const Value c1{Type::Int, fn.nextValue++};
            fn.blocks[kLoopInit].ops.push_back({Op::Int, c1, {}, "1", 0, p.sp});
            fn.blocks[kLoopInit].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {c1}, {}, p.kSlot, p.sp});
            fn.blocks[kLoopInit].exit = Exit::Jump;
            fn.blocks[kLoopInit].yes = kLoopHdr;

            const Value kv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value cond{Type::Bool, fn.nextValue++};
            fn.blocks[kLoopHdr].ops.push_back({Op::Load, kv, {}, {}, p.kSlot, p.sp});
            fn.blocks[kLoopHdr].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[kLoopHdr].ops.push_back({Op::Binary, cond, {kv, nv}, "<", 0, p.sp});
            fn.blocks[kLoopHdr].exit = Exit::Branch;
            fn.blocks[kLoopHdr].value = cond;
            fn.blocks[kLoopHdr].yes = kLoopPreJ;
            fn.blocks[kLoopHdr].no = redInit;

            auto& pops = fn.blocks[kLoopPreJ].ops;
            const Value k2{Type::Int, fn.nextValue++};
            const Value n2{Type::Int, fn.nextValue++};
            pops.push_back({Op::Load, k2, {}, {}, p.kSlot, p.sp});
            pops.push_back({Op::Load, n2, {}, {}, p.nSlot, p.sp});
            Value bk = makeMul(pops, k2, n2);
            pops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {bk}, {}, bkSlot, p.sp});
            const std::array<unsigned, 4> bSlots{b0Slot, b1Slot, b2Slot, b3Slot};
            const std::array<unsigned, 4> a0s{a0Slot, a1Slot, a2Slot, a3Slot};
            for (int r = 0; r < 4; ++r) {
                const Value br{Type::Int, fn.nextValue++};
                pops.push_back({Op::Load, br, {}, {}, bSlots[r], p.sp});
                Value idx = makeAdd(pops, br, k2);
                Value av = emitRowIdx(pops, p.aSlot, idx, false);
                pops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {av}, {}, a0s[r], p.sp});
            }
            const Value z0{Type::Int, fn.nextValue++};
            pops.push_back({Op::Int, z0, {}, "0", 0, p.sp});
            pops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z0}, {}, p.jSlot, p.sp});
            fn.blocks[kLoopPreJ].exit = Exit::Jump;
            fn.blocks[kLoopPreJ].yes = kLoopJHdr;

            const Value jv{Type::Int, fn.nextValue++};
            const Value n3{Type::Int, fn.nextValue++};
            const Value condJ{Type::Bool, fn.nextValue++};
            fn.blocks[kLoopJHdr].ops.push_back({Op::Load, jv, {}, {}, p.jSlot, p.sp});
            fn.blocks[kLoopJHdr].ops.push_back({Op::Load, n3, {}, {}, p.nSlot, p.sp});
            fn.blocks[kLoopJHdr].ops.push_back({Op::Binary, condJ, {jv, n3}, "<", 0, p.sp});
            fn.blocks[kLoopJHdr].exit = Exit::Branch;
            fn.blocks[kLoopJHdr].value = condJ;
            fn.blocks[kLoopJHdr].yes = kLoopJBody;
            fn.blocks[kLoopJHdr].no = kLoopStep;

            auto& jops = fn.blocks[kLoopJBody].ops;
            const Value j1{Type::Int, fn.nextValue++};
            const Value bk1{Type::Int, fn.nextValue++};
            jops.push_back({Op::Load, j1, {}, {}, p.jSlot, p.sp});
            jops.push_back({Op::Load, bk1, {}, {}, bkSlot, p.sp});
            Value bIdx = makeAdd(jops, bk1, j1);
            Value bv = emitRowIdx(jops, p.bSlot, bIdx, false);
            for (int r = 0; r < 4; ++r) {
                const Value ar{Type::Int, fn.nextValue++};
                jops.push_back({Op::Load, ar, {}, {}, a0s[r], p.sp});
                Value pr = makeMul(jops, ar, bv, 32);
                Value cv = emitRowIdx(jops, cSlots[r], j1, true, 32);
                Value nv4 = makeAdd(jops, cv, pr, 32);
                emitRowSet(jops, cSlots[r], j1, nv4, 32);
            }
            const Value one{Type::Int, fn.nextValue++};
            jops.push_back({Op::Int, one, {}, "1", 0, p.sp});
            Value jNext = makeAdd(jops, j1, one, 64, true);
            jops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jNext}, {}, p.jSlot, p.sp});
            fn.blocks[kLoopJBody].exit = Exit::Jump;
            fn.blocks[kLoopJBody].yes = kLoopJHdr;
            fn.blocks[kLoopJBody].loopHint = LoopHint::VectorizeParallel;
            fn.blocks[kLoopJBody].noInterleave = true;

            const Value k3{Type::Int, fn.nextValue++};
            const Value oneK{Type::Int, fn.nextValue++};
            fn.blocks[kLoopStep].ops.push_back({Op::Load, k3, {}, {}, p.kSlot, p.sp});
            fn.blocks[kLoopStep].ops.push_back({Op::Int, oneK, {}, "1", 0, p.sp});
            Value kNext = makeAdd(fn.blocks[kLoopStep].ops, k3, oneK, 64, true);
            fn.blocks[kLoopStep].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {kNext}, {}, p.kSlot, p.sp});
            fn.blocks[kLoopStep].exit = Exit::Jump;
            fn.blocks[kLoopStep].yes = kLoopHdr;
        }

        // redInit / redHdr / redBody: redAcc = 0; for j in 0..N: redAcc += sum_{r=0..3}(c_r[j] % M); total += redAcc
        {
            const Value z0{Type::Int, fn.nextValue++};
            fn.blocks[redInit].ops.push_back({Op::Int, z0, {}, "0", 0, p.sp});
            fn.blocks[redInit].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z0}, {}, p.jSlot, p.sp});
            fn.blocks[redInit].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z0}, {}, redAccSlot, p.sp});
            fn.blocks[redInit].exit = Exit::Jump;
            fn.blocks[redInit].yes = redHdr;

            const Value jv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value condR{Type::Bool, fn.nextValue++};
            fn.blocks[redHdr].ops.push_back({Op::Load, jv, {}, {}, p.jSlot, p.sp});
            fn.blocks[redHdr].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[redHdr].ops.push_back({Op::Binary, condR, {jv, nv}, "<", 0, p.sp});
            fn.blocks[redHdr].exit = Exit::Branch;
            fn.blocks[redHdr].value = condR;
            fn.blocks[redHdr].yes = redBody;
            fn.blocks[redHdr].no = i4Step;

            auto& rops = fn.blocks[redBody].ops;
            const Value j1{Type::Int, fn.nextValue++};
            const Value mv{Type::Int, fn.nextValue++};
            const Value acc{Type::Int, fn.nextValue++};
            rops.push_back({Op::Load, j1, {}, {}, p.jSlot, p.sp});
            rops.push_back({Op::Int, mv, {}, std::to_string(p.modConst), 0, p.sp});
            std::array<Value, 4> mods{};
            for (int r = 0; r < 4; ++r) {
                Value cv = emitRowIdx(rops, cSlots[r], j1, true, 32);
                Value remV{Type::Int, fn.nextValue++};
                Operation remOp{Op::Binary, remV, {cv, mv}, "%", 0, p.sp};
                remOp.divSafe = true;
                remOp.unsignedDiv = true;
                remOp.width = 32;
                remOp.lhsUpper = lhsUpperBound;
                rops.push_back(std::move(remOp));
                mods[r] = remV;
            }
            const std::uint8_t sW = (p.modConst <= 500000000LL) ? 32 : 64;
            Value s01 = makeAdd(rops, mods[0], mods[1], sW);
            Value s23 = makeAdd(rops, mods[2], mods[3], sW);
            Value sAll = makeAdd(rops, s01, s23, sW);
            rops.push_back({Op::Load, acc, {}, {}, redAccSlot, p.sp});
            Value nextAcc = makeAdd(rops, acc, sAll, (lhsUpperBound <= 2147483647LL && 4 * p.modConst * p.safeMaxN <= 2147483647LL) ? 32 : 64);
            rops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {nextAcc}, {}, redAccSlot, p.sp});
            const Value oneR{Type::Int, fn.nextValue++};
            rops.push_back({Op::Int, oneR, {}, "1", 0, p.sp});
            Value jNext = makeAdd(rops, j1, oneR, 64, true);
            rops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jNext}, {}, p.jSlot, p.sp});
            fn.blocks[redBody].exit = Exit::Jump;
            fn.blocks[redBody].yes = redHdr;
            fn.blocks[redBody].loopHint = LoopHint::Vectorize;
            fn.blocks[redBody].noInterleave = true;
        }

        // i4Step: totalSlot += redAccSlot; iSlot += 4 -> i4Hdr
        {
            const Value tot{Type::Int, fn.nextValue++};
            const Value ra{Type::Int, fn.nextValue++};
            const Value iv{Type::Int, fn.nextValue++};
            const Value c4{Type::Int, fn.nextValue++};
            fn.blocks[i4Step].ops.push_back({Op::Load, tot, {}, {}, p.totalSlot, p.sp});
            fn.blocks[i4Step].ops.push_back({Op::Load, ra, {}, {}, redAccSlot, p.sp});
            Value nextTot = makeAdd(fn.blocks[i4Step].ops, tot, ra);
            fn.blocks[i4Step].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {nextTot}, {}, p.totalSlot, p.sp});
            fn.blocks[i4Step].ops.push_back({Op::Load, iv, {}, {}, p.iSlot, p.sp});
            fn.blocks[i4Step].ops.push_back({Op::Int, c4, {}, "4", 0, p.sp});
            Value nextI = makeAdd(fn.blocks[i4Step].ops, iv, c4);
            fn.blocks[i4Step].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {nextI}, {}, p.iSlot, p.sp});
            fn.blocks[i4Step].exit = Exit::Jump;
            fn.blocks[i4Step].yes = i4Hdr;
        }
    }
    return anyRewritten;
}

/// Tile `j` (`JB=128`) + 4-row `i` unroll-and-jam on `i -> k -> j` `[f32]`/`[f64]` matmul
/// while strictly preserving exact `k = 0..N-1` summation order (bit-exact IEEE-754 output).
inline void tileAndUnrollFloatMatMul(FunctionIR& fn, const Program* programPtr) {
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
            if (op.op == Op::Store && !op.inputs.empty() && op.slot < nSlots &&
                detail::isAnyArrayType(programPtr, fn.slots[op.slot])) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && (d->second->op == Op::RepeatRegion || d->second->op == Op::RepeatHeap)) {
                    ++originCount[op.slot];
                } else {
                    badArr[op.slot] = true;
                }
            }
        }
    }

    struct Plan {
        unsigned preI = 0, hdrI = 0, hdrK = 0, kPreJ = 0, hdrJ = 0, bodyJ = 0, kStep = 0, iStep = 0, iExit = 0;
        unsigned iSlot = 0, kSlot = 0, jSlot = 0, nSlot = 0;
        unsigned aSlot = 0, bSlot = 0, cSlot = 0;
        Type arrType = Type::Void;
        Type elemType = Type::Void;
        Span sp{};
    };
    std::vector<Plan> plans;
    std::unordered_set<unsigned> usedPre;

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lpI = cfg.loops[li];
        if (lpI.preheaders.size() != 1) continue;
        const unsigned preI = lpI.preheaders[0];
        const unsigned hdrI = lpI.header;
        if (usedPre.contains(preI) || fn.blocks[preI].exit != Exit::Jump || fn.blocks[preI].yes != hdrI) continue;
        const auto& hIBlk = fn.blocks[hdrI];
        if (hIBlk.exit != Exit::Branch || !hIBlk.value || !lpI.body.contains(hIBlk.yes) || lpI.body.contains(hIBlk.no))
            continue;
        const unsigned iExit = hIBlk.no;
        auto cI = defOp.find(hIBlk.value->id);
        if (cI == defOp.end() || cI->second->op != Op::Binary || cI->second->data != "<") continue;
        auto ils = loadSlot.find(cI->second->inputs[0].id);
        auto nls = loadSlot.find(cI->second->inputs[1].id);
        if (ils == loadSlot.end() || nls == loadSlot.end()) continue;
        const unsigned iSlot = ils->second;
        const unsigned nSlot = nls->second;

        std::optional<std::int64_t> iLowC;
        for (const auto& op : fn.blocks[preI].ops) {
            if (op.op == Op::Store && op.slot == iSlot && !op.inputs.empty()) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && d->second->op == Op::Int) iLowC = parseConstInt(d->second->data);
            }
        }
        if (iLowC != 0) continue;

        bool hasIBodyPre = false;
        unsigned iBodyPre = hdrI;
        unsigned hdrK = hIBlk.yes;
        if (!cfg.headerToLoop.contains(hdrK) && fn.blocks[hdrK].exit == Exit::Jump && lpI.body.contains(hdrK)) {
            hasIBodyPre = true;
            iBodyPre = hdrK;
            hdrK = fn.blocks[iBodyPre].yes;
        }
        auto litK = cfg.headerToLoop.find(hdrK);
        if (litK == cfg.headerToLoop.end() || litK->second == li) continue;
        const auto& lpK = cfg.loops[litK->second];
        const auto& hKBlk = fn.blocks[hdrK];
        if (hKBlk.exit != Exit::Branch || !hKBlk.value || !lpK.body.contains(hKBlk.yes) || lpK.body.contains(hKBlk.no))
            continue;
        auto cK = defOp.find(hKBlk.value->id);
        if (cK == defOp.end() || cK->second->op != Op::Binary || cK->second->data != "<") continue;
        auto kls = loadSlot.find(cK->second->inputs[0].id);
        auto knls = loadSlot.find(cK->second->inputs[1].id);
        if (kls == loadSlot.end() || knls == loadSlot.end() || knls->second != nSlot || kls->second == iSlot) continue;
        const unsigned kSlot = kls->second;

        const unsigned kPreJ = hKBlk.yes;
        const unsigned iStep = hKBlk.no;
        if (fn.blocks[kPreJ].exit != Exit::Jump) continue;
        const unsigned hdrJ = fn.blocks[kPreJ].yes;
        auto litJ = cfg.headerToLoop.find(hdrJ);
        if (litJ == cfg.headerToLoop.end() || litJ->second == litK->second) continue;
        const auto& lpJ = cfg.loops[litJ->second];
        if (lpJ.body.size() != 2 || lpJ.preheaders.size() != 1 || lpJ.preheaders[0] != kPreJ) continue;
        const auto& hJBlk = fn.blocks[hdrJ];
        if (hJBlk.exit != Exit::Branch || !hJBlk.value || !lpJ.body.contains(hJBlk.yes) || lpJ.body.contains(hJBlk.no))
            continue;
        const unsigned bodyJ = hJBlk.yes;
        const unsigned kStep = hJBlk.no;
        if (fn.blocks[bodyJ].exit != Exit::Jump || fn.blocks[bodyJ].yes != hdrJ) continue;
        if (fn.blocks[kStep].exit != Exit::Jump || fn.blocks[kStep].yes != hdrK) continue;
        if (fn.blocks[iStep].exit != Exit::Jump || fn.blocks[iStep].yes != hdrI) continue;
        if (lpI.body.size() != (hasIBodyPre ? 8u : 7u) ||
            !lpI.body.contains(iStep) || !lpI.body.contains(kStep) || !lpI.body.contains(bodyJ))
            continue;

        auto cJ = defOp.find(hJBlk.value->id);
        if (cJ == defOp.end() || cJ->second->op != Op::Binary || cJ->second->data != "<") continue;
        auto jls = loadSlot.find(cJ->second->inputs[0].id);
        auto jnls = loadSlot.find(cJ->second->inputs[1].id);
        if (jls == loadSlot.end() || jnls == loadSlot.end() || jnls->second != nSlot ||
            jls->second == iSlot || jls->second == kSlot) continue;
        const unsigned jSlot = jls->second;

        // Ensure hdrI (or iBodyPre) sets k=0 cleanly and hKBlk (or kPreJ) sets j=0 cleanly
        std::optional<std::int64_t> kLowC, jLowC;
        bool okHI = true;
        if (hasIBodyPre) {
            for (const auto& op : hIBlk.ops) {
                if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary) { okHI = false; break; }
            }
            for (const auto& op : fn.blocks[iBodyPre].ops) {
                if (op.op == Op::Store) {
                    if (op.slot == kSlot && !op.inputs.empty()) {
                        auto d = defOp.find(op.inputs[0].id);
                        if (d != defOp.end() && d->second->op == Op::Int) kLowC = parseConstInt(d->second->data);
                    } else okHI = false;
                } else if (op.op != Op::Load && op.op != Op::Int && op.op != Op::Binary) okHI = false;
            }
        } else {
            for (const auto& op : hIBlk.ops) {
                if (op.op == Op::Store && op.slot == kSlot && !op.inputs.empty()) {
                    auto d = defOp.find(op.inputs[0].id);
                    if (d != defOp.end() && d->second->op == Op::Int) kLowC = parseConstInt(d->second->data);
                }
            }
        }
        if (!okHI) continue;
        for (const auto& op : hKBlk.ops) {
            if (op.op == Op::Store && op.slot == jSlot && !op.inputs.empty()) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && d->second->op == Op::Int) jLowC = parseConstInt(d->second->data);
            }
        }

        auto resolveRowCol = [&](Value idxV, unsigned rowSlot, unsigned colSlot) -> bool {
            auto addIt = defOp.find(idxV.id);
            if (addIt == defOp.end() || addIt->second->op != Op::Binary || addIt->second->data != "+") return false;
            auto mulIt = defOp.find(addIt->second->inputs[0].id);
            auto cIt = loadSlot.find(addIt->second->inputs[1].id);
            if (mulIt == defOp.end() || mulIt->second->op != Op::Binary || mulIt->second->data != "*" ||
                cIt == loadSlot.end() || cIt->second != colSlot) return false;
            auto r0 = loadSlot.find(mulIt->second->inputs[0].id);
            auto n0 = loadSlot.find(mulIt->second->inputs[1].id);
            return (r0 != loadSlot.end() && n0 != loadSlot.end() &&
                    ((r0->second == rowSlot && n0->second == nSlot) ||
                     (r0->second == nSlot && n0->second == rowSlot)));
        };

        // Check kPreJ computes aik = a[i*n + k] (stored into single temp aikSlot) and optionally j=0
        std::optional<unsigned> aSlot, aikSlot;
        bool okPreJ = true;
        for (const auto& op : fn.blocks[kPreJ].ops) {
            if (op.op == Op::Store && op.slot == jSlot && !op.inputs.empty()) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && d->second->op == Op::Int) jLowC = parseConstInt(d->second->data);
                else { okPreJ = false; break; }
            } else if (op.op == Op::Store && !aikSlot && !op.inputs.empty() &&
                       (fn.slots[op.slot] == Type::F32 || fn.slots[op.slot] == Type::Float)) {
                auto idxD = defOp.find(op.inputs[0].id);
                if (idxD != defOp.end() && idxD->second->op == Op::Index && idxD->second->boundsProven &&
                    resolveRowCol(idxD->second->inputs[1], iSlot, kSlot)) {
                    if (auto als = loadSlot.find(idxD->second->inputs[0].id); als != loadSlot.end()) {
                        aSlot = als->second;
                        aikSlot = op.slot;
                    }
                }
            } else if (op.op == Op::Store || op.op == Op::IndexSet || op.op == Op::Call || op.op == Op::ExternCall) {
                okPreJ = false; break;
            }
        }
        if (kLowC != 0 || jLowC != 0 || !okPreJ || !aSlot || !aikSlot) continue;

        // Check bodyJ: c[i*n + j] = c[i*n + j] + aik * b[k*n + j], j += 1
        std::optional<unsigned> bSlot, cSlot;
        bool matchedFma = false;
        int jIncs = 0;
        bool okBodyJ = true;
        for (const auto& op : fn.blocks[bodyJ].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Builtin ||
                op.op == Op::Print || op.op == Op::Assert) { okBodyJ = false; break; }
            if ((op.op == Op::Index || op.op == Op::IndexSet) && !op.boundsProven) { okBodyJ = false; break; }
            if (op.op == Op::Store) {
                if (op.slot == jSlot) ++jIncs;
                else { okBodyJ = false; break; }
            }
            if (op.op == Op::IndexSet && op.inputs.size() >= 3) {
                auto cls = loadSlot.find(op.inputs[0].id);
                if (cls == loadSlot.end() || !resolveRowCol(op.inputs[1], iSlot, jSlot)) {
                    okBodyJ = false; break;
                }
                cSlot = cls->second;
                auto addOp = defOp.find(op.inputs[2].id);
                if (addOp == defOp.end() || addOp->second->op != Op::Binary || addOp->second->data != "+") {
                    okBodyJ = false; break;
                }
                for (int side = 0; side < 2; ++side) {
                    auto cRd = defOp.find(addOp->second->inputs[side].id);
                    auto mulOp = defOp.find(addOp->second->inputs[1 - side].id);
                    if (cRd != defOp.end() && cRd->second->op == Op::Index &&
                        loadSlot.count(cRd->second->inputs[0].id) &&
                        loadSlot.at(cRd->second->inputs[0].id) == *cSlot &&
                        resolveRowCol(cRd->second->inputs[1], iSlot, jSlot) &&
                        mulOp != defOp.end() && mulOp->second->op == Op::Binary && mulOp->second->data == "*") {
                        for (int ms = 0; ms < 2; ++ms) {
                            auto als = loadSlot.find(mulOp->second->inputs[ms].id);
                            auto bRd = defOp.find(mulOp->second->inputs[1 - ms].id);
                            if (als != loadSlot.end() && als->second == *aikSlot &&
                                bRd != defOp.end() && bRd->second->op == Op::Index &&
                                loadSlot.count(bRd->second->inputs[0].id) &&
                                resolveRowCol(bRd->second->inputs[1], kSlot, jSlot)) {
                                bSlot = loadSlot.at(bRd->second->inputs[0].id);
                                matchedFma = true;
                                break;
                            }
                        }
                        if (matchedFma) break;
                    }
                }
                if (!matchedFma) { okBodyJ = false; break; }
            }
        }
        if (!okBodyJ || !matchedFma || jIncs != 1 || !bSlot || !cSlot) continue;
        if (*cSlot == *aSlot || *cSlot == *bSlot ||
            originCount[*aSlot] != 1 || originCount[*bSlot] != 1 || originCount[*cSlot] != 1 ||
            badArr[*aSlot] || badArr[*bSlot] || badArr[*cSlot]) continue;

        const Type arrT = fn.slots[*cSlot];
        const Type elemT = fn.slots[*aikSlot];
        usedPre.insert(preI);
        plans.push_back({preI, hdrI, hdrK, kPreJ, hdrJ, bodyJ, kStep, iStep, iExit,
                         iSlot, kSlot, jSlot, nSlot, *aSlot, *bSlot, *cSlot, arrT, elemT,
                         hIBlk.ops.empty() ? Span{} : hIBlk.ops.front().span});
    }

    int nextAg = 0;
    for (const auto& blk : fn.blocks) {
        nextAg = std::max(nextAg, blk.loopAccessGroup + 1);
        for (const auto& op : blk.ops) nextAg = std::max(nextAg, op.loopAccessGroup + 1);
    }

    for (const auto& p : plans) {
        if (fn.blocks[p.preI].exit != Exit::Jump || fn.blocks[p.preI].yes != p.hdrI) continue;

        if (p.elemType == Type::F32) {
            const unsigned gemmBlk = static_cast<unsigned>(fn.blocks.size());
            fn.blocks.emplace_back();

            const Value nv{Type::Int, fn.nextValue++};
            const Value cLim{Type::Int, fn.nextValue++};
            const Value cZero{Type::Int, fn.nextValue++};
            const Value okHi{Type::Bool, fn.nextValue++};
            const Value okLo{Type::Bool, fn.nextValue++};
            const Value ok{Type::Bool, fn.nextValue++};
            fn.blocks[p.preI].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, cLim, {}, "4096", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, cZero, {}, "0", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, okHi, {nv, cLim}, "<=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, okLo, {nv, cZero}, ">=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, ok, {okHi, okLo}, "&", 0, p.sp});
            fn.blocks[p.preI].exit = Exit::Branch;
            fn.blocks[p.preI].value = ok;
            fn.blocks[p.preI].yes = gemmBlk;
            fn.blocks[p.preI].no = p.hdrI;

            const Value av{p.arrType, fn.nextValue++};
            const Value bv{p.arrType, fn.nextValue++};
            const Value cv{p.arrType, fn.nextValue++};
            const Value n2{Type::Int, fn.nextValue++};
            auto& gops = fn.blocks[gemmBlk].ops;
            gops.push_back({Op::Load, av, {}, {}, p.aSlot, p.sp});
            gops.push_back({Op::Load, bv, {}, {}, p.bSlot, p.sp});
            gops.push_back({Op::Load, cv, {}, {}, p.cSlot, p.sp});
            gops.push_back({Op::Load, n2, {}, {}, p.nSlot, p.sp});
            gops.push_back({Op::GemmF32, {Type::Void, fn.nextValue++}, {av, bv, cv, n2}, {}, 0, p.sp});
            fn.blocks[gemmBlk].exit = Exit::Jump;
            fn.blocks[gemmBlk].yes = p.iExit;
            continue;
        }

        const unsigned jbSlot = detail::allocOptSlot(fn, Type::Int);
        const unsigned jHiSlot = detail::allocOptSlot(fn, Type::Int);
        const std::array<unsigned, 4> baseISlots{
            detail::allocOptSlot(fn, Type::Int), detail::allocOptSlot(fn, Type::Int),
            detail::allocOptSlot(fn, Type::Int), detail::allocOptSlot(fn, Type::Int)
        };
        const std::array<unsigned, 4> aScalSlots{
            detail::allocOptSlot(fn, p.elemType), detail::allocOptSlot(fn, p.elemType),
            detail::allocOptSlot(fn, p.elemType), detail::allocOptSlot(fn, p.elemType)
        };
        const unsigned baseKSlot = detail::allocOptSlot(fn, Type::Int);
        const int ag4 = nextAg++;
        const int ag1 = nextAg++;

        const unsigned jbInit = static_cast<unsigned>(fn.blocks.size());
        const unsigned jbHdr = jbInit + 1;
        const unsigned jbBody = jbInit + 2;
        const unsigned i4Hdr = jbInit + 3;
        const unsigned i4Pre = jbInit + 4;
        const unsigned k4Hdr = jbInit + 5;
        const unsigned k4Pre = jbInit + 6;
        const unsigned j4Hdr = jbInit + 7;
        const unsigned j4Body = jbInit + 8;
        const unsigned k4Step = jbInit + 9;
        const unsigned i4Step = jbInit + 10;
        const unsigned i1Hdr = jbInit + 11;
        const unsigned i1Pre = jbInit + 12;
        const unsigned k1Hdr = jbInit + 13;
        const unsigned k1Pre = jbInit + 14;
        const unsigned j1Hdr = jbInit + 15;
        const unsigned j1Body = jbInit + 16;
        const unsigned k1Step = jbInit + 17;
        const unsigned i1Step = jbInit + 18;
        const unsigned jbStep = jbInit + 19;
        fn.blocks.resize(jbStep + 1);

        auto mkAdd = [&](std::vector<Operation>& ops, Value a, Value b, bool rangeInc = false) {
            Value res{Type::Int, fn.nextValue++};
            Operation o{Op::Binary, res, {a, b}, rangeInc ? "rangeIncrement" : "+", 0, p.sp};
            o.overflowProven = true;
            ops.push_back(std::move(o));
            return res;
        };
        auto mkMul = [&](std::vector<Operation>& ops, Value a, Value b) {
            Value res{Type::Int, fn.nextValue++};
            Operation o{Op::Binary, res, {a, b}, "*", 0, p.sp};
            o.overflowProven = true;
            ops.push_back(std::move(o));
            return res;
        };
        auto emitLoadArr = [&](std::vector<Operation>& ops, unsigned s, Value idx, int ag = -1) {
            Value av{p.arrType, fn.nextValue++};
            Value rv{p.elemType, fn.nextValue++};
            Operation ix{Op::Index, rv, {av, idx}, {}, 0, p.sp};
            ix.boundsProven = true;
            ix.loopAccessGroup = ag;
            ops.push_back({Op::Load, av, {}, {}, s, p.sp});
            ops.push_back(std::move(ix));
            return rv;
        };
        auto emitStoreArr = [&](std::vector<Operation>& ops, unsigned s, Value idx, Value v, int ag = -1) {
            Value av{p.arrType, fn.nextValue++};
            Operation sx{Op::IndexSet, {Type::Void, fn.nextValue++}, {av, idx, v}, {}, 0, p.sp};
            sx.boundsProven = true;
            sx.loopAccessGroup = ag;
            ops.push_back({Op::Load, av, {}, {}, s, p.sp});
            ops.push_back(std::move(sx));
        };

        // Guard n <= 4096 in preI -> jbInit (safe fallback -> p.hdrI)
        {
            const Value nv{Type::Int, fn.nextValue++};
            const Value cLim{Type::Int, fn.nextValue++};
            const Value cZero{Type::Int, fn.nextValue++};
            const Value okHi{Type::Bool, fn.nextValue++};
            const Value okLo{Type::Bool, fn.nextValue++};
            const Value ok{Type::Bool, fn.nextValue++};
            fn.blocks[p.preI].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, cLim, {}, "4096", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, cZero, {}, "0", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, okHi, {nv, cLim}, "<=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, okLo, {nv, cZero}, ">=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, ok, {okHi, okLo}, "&", 0, p.sp});
            fn.blocks[p.preI].exit = Exit::Branch;
            fn.blocks[p.preI].value = ok;
            fn.blocks[p.preI].yes = jbInit;
            fn.blocks[p.preI].no = p.hdrI;
        }

        // jbInit / jbHdr / jbBody
        {
            const Value z0{Type::Int, fn.nextValue++};
            fn.blocks[jbInit].ops.push_back({Op::Int, z0, {}, "0", 0, p.sp});
            fn.blocks[jbInit].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z0}, {}, jbSlot, p.sp});
            fn.blocks[jbInit].exit = Exit::Jump;
            fn.blocks[jbInit].yes = jbHdr;

            const Value jbv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value cmp{Type::Bool, fn.nextValue++};
            fn.blocks[jbHdr].ops.push_back({Op::Load, jbv, {}, {}, jbSlot, p.sp});
            fn.blocks[jbHdr].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[jbHdr].ops.push_back({Op::Binary, cmp, {jbv, nv}, "<", 0, p.sp});
            fn.blocks[jbHdr].exit = Exit::Branch;
            fn.blocks[jbHdr].value = cmp;
            fn.blocks[jbHdr].yes = jbBody;
            fn.blocks[jbHdr].no = p.iExit;

            const Value jb2{Type::Int, fn.nextValue++};
            const Value n2{Type::Int, fn.nextValue++};
            const Value c128{Type::Int, fn.nextValue++};
            const Value jhi{Type::Int, fn.nextValue++};
            const Value z1{Type::Int, fn.nextValue++};
            fn.blocks[jbBody].ops.push_back({Op::Load, jb2, {}, {}, jbSlot, p.sp});
            fn.blocks[jbBody].ops.push_back({Op::Load, n2, {}, {}, p.nSlot, p.sp});
            fn.blocks[jbBody].ops.push_back({Op::Int, c128, {}, "128", 0, p.sp});
            Value cand = mkAdd(fn.blocks[jbBody].ops, jb2, c128);
            Operation mn{Op::Min, jhi, {n2, cand}, {}, 0, p.sp};
            mn.width = 64;
            fn.blocks[jbBody].ops.push_back(std::move(mn));
            fn.blocks[jbBody].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jhi}, {}, jHiSlot, p.sp});
            fn.blocks[jbBody].ops.push_back({Op::Int, z1, {}, "0", 0, p.sp});
            fn.blocks[jbBody].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z1}, {}, p.iSlot, p.sp});
            fn.blocks[jbBody].exit = Exit::Jump;
            fn.blocks[jbBody].yes = i4Hdr;
        }

        // i4Hdr: (i+3) < n -> i4Pre else i1Hdr
        {
            const Value iv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value c3{Type::Int, fn.nextValue++};
            const Value cond{Type::Bool, fn.nextValue++};
            fn.blocks[i4Hdr].ops.push_back({Op::Load, iv, {}, {}, p.iSlot, p.sp});
            fn.blocks[i4Hdr].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[i4Hdr].ops.push_back({Op::Int, c3, {}, "3", 0, p.sp});
            Value i3 = mkAdd(fn.blocks[i4Hdr].ops, iv, c3);
            fn.blocks[i4Hdr].ops.push_back({Op::Binary, cond, {i3, nv}, "<", 0, p.sp});
            fn.blocks[i4Hdr].exit = Exit::Branch;
            fn.blocks[i4Hdr].value = cond;
            fn.blocks[i4Hdr].yes = i4Pre;
            fn.blocks[i4Hdr].no = i1Hdr;
        }
        // i4Pre: baseISlots[r] = (i+r)*n, k = 0
        {
            auto& ops = fn.blocks[i4Pre].ops;
            const Value iv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value z0{Type::Int, fn.nextValue++};
            ops.push_back({Op::Load, iv, {}, {}, p.iSlot, p.sp});
            ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            for (int r = 0; r < 4; ++r) {
                Value row = iv;
                if (r > 0) {
                    const Value cr{Type::Int, fn.nextValue++};
                    ops.push_back({Op::Int, cr, {}, std::to_string(r), 0, p.sp});
                    row = mkAdd(ops, iv, cr);
                }
                Value baseR = mkMul(ops, row, nv);
                ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {baseR}, {}, baseISlots[r], p.sp});
            }
            ops.push_back({Op::Int, z0, {}, "0", 0, p.sp});
            ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z0}, {}, p.kSlot, p.sp});
            fn.blocks[i4Pre].exit = Exit::Jump;
            fn.blocks[i4Pre].yes = k4Hdr;
        }
        // k4Hdr -> k4Pre -> j4Hdr -> j4Body -> k4Step -> k4Hdr -> i4Step -> i4Hdr
        {
            const Value kv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value cond{Type::Bool, fn.nextValue++};
            fn.blocks[k4Hdr].ops.push_back({Op::Load, kv, {}, {}, p.kSlot, p.sp});
            fn.blocks[k4Hdr].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[k4Hdr].ops.push_back({Op::Binary, cond, {kv, nv}, "<", 0, p.sp});
            fn.blocks[k4Hdr].exit = Exit::Branch;
            fn.blocks[k4Hdr].value = cond;
            fn.blocks[k4Hdr].yes = k4Pre;
            fn.blocks[k4Hdr].no = i4Step;

            auto& kops = fn.blocks[k4Pre].ops;
            const Value k2{Type::Int, fn.nextValue++};
            const Value n2{Type::Int, fn.nextValue++};
            const Value jbv{Type::Int, fn.nextValue++};
            kops.push_back({Op::Load, k2, {}, {}, p.kSlot, p.sp});
            kops.push_back({Op::Load, n2, {}, {}, p.nSlot, p.sp});
            Value bk = mkMul(kops, k2, n2);
            kops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {bk}, {}, baseKSlot, p.sp});
            for (int r = 0; r < 4; ++r) {
                const Value br{Type::Int, fn.nextValue++};
                kops.push_back({Op::Load, br, {}, {}, baseISlots[r], p.sp});
                Value aiIdx = mkAdd(kops, br, k2);
                Value av = emitLoadArr(kops, p.aSlot, aiIdx);
                kops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {av}, {}, aScalSlots[r], p.sp});
            }
            kops.push_back({Op::Load, jbv, {}, {}, jbSlot, p.sp});
            kops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jbv}, {}, p.jSlot, p.sp});
            fn.blocks[k4Pre].exit = Exit::Jump;
            fn.blocks[k4Pre].yes = j4Hdr;

            const Value jv{Type::Int, fn.nextValue++};
            const Value jhi{Type::Int, fn.nextValue++};
            const Value condJ{Type::Bool, fn.nextValue++};
            fn.blocks[j4Hdr].ops.push_back({Op::Load, jv, {}, {}, p.jSlot, p.sp});
            fn.blocks[j4Hdr].ops.push_back({Op::Load, jhi, {}, {}, jHiSlot, p.sp});
            fn.blocks[j4Hdr].ops.push_back({Op::Binary, condJ, {jv, jhi}, "<", 0, p.sp});
            fn.blocks[j4Hdr].exit = Exit::Branch;
            fn.blocks[j4Hdr].value = condJ;
            fn.blocks[j4Hdr].yes = j4Body;
            fn.blocks[j4Hdr].no = k4Step;

            auto& jops = fn.blocks[j4Body].ops;
            const Value j1{Type::Int, fn.nextValue++};
            const Value bkv{Type::Int, fn.nextValue++};
            jops.push_back({Op::Load, j1, {}, {}, p.jSlot, p.sp});
            jops.push_back({Op::Load, bkv, {}, {}, baseKSlot, p.sp});
            Value bIdx = mkAdd(jops, bkv, j1);
            Value bv = emitLoadArr(jops, p.bSlot, bIdx, ag4);
            for (int r = 0; r < 4; ++r) {
                const Value aScal{p.elemType, fn.nextValue++};
                const Value baseR{Type::Int, fn.nextValue++};
                jops.push_back({Op::Load, aScal, {}, {}, aScalSlots[r], p.sp});
                jops.push_back({Op::Load, baseR, {}, {}, baseISlots[r], p.sp});
                Value cIdx = mkAdd(jops, baseR, j1);
                Value cv = emitLoadArr(jops, p.cSlot, cIdx, ag4);
                Value prod{p.elemType, fn.nextValue++};
                jops.push_back({Op::Binary, prod, {aScal, bv}, "*", 0, p.sp});
                Value sum{p.elemType, fn.nextValue++};
                jops.push_back({Op::Binary, sum, {cv, prod}, "+", 0, p.sp});
                emitStoreArr(jops, p.cSlot, cIdx, sum, ag4);
            }
            const Value oneJ{Type::Int, fn.nextValue++};
            jops.push_back({Op::Int, oneJ, {}, "1", 0, p.sp});
            Value jNext = mkAdd(jops, j1, oneJ, true);
            jops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jNext}, {}, p.jSlot, p.sp});
            fn.blocks[j4Body].exit = Exit::Jump;
            fn.blocks[j4Body].yes = j4Hdr;
            fn.blocks[j4Body].loopHint = LoopHint::VectorizeParallel;
            fn.blocks[j4Body].loopAccessGroup = ag4;

            const Value k3{Type::Int, fn.nextValue++};
            const Value oneK{Type::Int, fn.nextValue++};
            fn.blocks[k4Step].ops.push_back({Op::Load, k3, {}, {}, p.kSlot, p.sp});
            fn.blocks[k4Step].ops.push_back({Op::Int, oneK, {}, "1", 0, p.sp});
            Value kNext = mkAdd(fn.blocks[k4Step].ops, k3, oneK, true);
            fn.blocks[k4Step].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {kNext}, {}, p.kSlot, p.sp});
            fn.blocks[k4Step].exit = Exit::Jump;
            fn.blocks[k4Step].yes = k4Hdr;

            const Value i3{Type::Int, fn.nextValue++};
            const Value c4{Type::Int, fn.nextValue++};
            fn.blocks[i4Step].ops.push_back({Op::Load, i3, {}, {}, p.iSlot, p.sp});
            fn.blocks[i4Step].ops.push_back({Op::Int, c4, {}, "4", 0, p.sp});
            Value iNext = mkAdd(fn.blocks[i4Step].ops, i3, c4);
            fn.blocks[i4Step].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {iNext}, {}, p.iSlot, p.sp});
            fn.blocks[i4Step].exit = Exit::Jump;
            fn.blocks[i4Step].yes = i4Hdr;
        }

        // i1Hdr remainder (1 row): i < n -> i1Pre -> k1Hdr -> k1Pre -> j1Hdr -> j1Body -> k1Step -> i1Step -> jbStep
        {
            const Value iv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value cond{Type::Bool, fn.nextValue++};
            fn.blocks[i1Hdr].ops.push_back({Op::Load, iv, {}, {}, p.iSlot, p.sp});
            fn.blocks[i1Hdr].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[i1Hdr].ops.push_back({Op::Binary, cond, {iv, nv}, "<", 0, p.sp});
            fn.blocks[i1Hdr].exit = Exit::Branch;
            fn.blocks[i1Hdr].value = cond;
            fn.blocks[i1Hdr].yes = i1Pre;
            fn.blocks[i1Hdr].no = jbStep;

            const Value iv2{Type::Int, fn.nextValue++};
            const Value nv2{Type::Int, fn.nextValue++};
            const Value z0{Type::Int, fn.nextValue++};
            fn.blocks[i1Pre].ops.push_back({Op::Load, iv2, {}, {}, p.iSlot, p.sp});
            fn.blocks[i1Pre].ops.push_back({Op::Load, nv2, {}, {}, p.nSlot, p.sp});
            Value baseR = mkMul(fn.blocks[i1Pre].ops, iv2, nv2);
            fn.blocks[i1Pre].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {baseR}, {}, baseISlots[0], p.sp});
            fn.blocks[i1Pre].ops.push_back({Op::Int, z0, {}, "0", 0, p.sp});
            fn.blocks[i1Pre].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z0}, {}, p.kSlot, p.sp});
            fn.blocks[i1Pre].exit = Exit::Jump;
            fn.blocks[i1Pre].yes = k1Hdr;

            const Value kv{Type::Int, fn.nextValue++};
            const Value nv3{Type::Int, fn.nextValue++};
            const Value condK{Type::Bool, fn.nextValue++};
            fn.blocks[k1Hdr].ops.push_back({Op::Load, kv, {}, {}, p.kSlot, p.sp});
            fn.blocks[k1Hdr].ops.push_back({Op::Load, nv3, {}, {}, p.nSlot, p.sp});
            fn.blocks[k1Hdr].ops.push_back({Op::Binary, condK, {kv, nv3}, "<", 0, p.sp});
            fn.blocks[k1Hdr].exit = Exit::Branch;
            fn.blocks[k1Hdr].value = condK;
            fn.blocks[k1Hdr].yes = k1Pre;
            fn.blocks[k1Hdr].no = i1Step;

            auto& k1ops = fn.blocks[k1Pre].ops;
            const Value k2{Type::Int, fn.nextValue++};
            const Value n3{Type::Int, fn.nextValue++};
            const Value br0{Type::Int, fn.nextValue++};
            const Value jbv{Type::Int, fn.nextValue++};
            k1ops.push_back({Op::Load, k2, {}, {}, p.kSlot, p.sp});
            k1ops.push_back({Op::Load, n3, {}, {}, p.nSlot, p.sp});
            Value bk = mkMul(k1ops, k2, n3);
            k1ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {bk}, {}, baseKSlot, p.sp});
            k1ops.push_back({Op::Load, br0, {}, {}, baseISlots[0], p.sp});
            Value aiIdx = mkAdd(k1ops, br0, k2);
            Value av = emitLoadArr(k1ops, p.aSlot, aiIdx);
            k1ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {av}, {}, aScalSlots[0], p.sp});
            k1ops.push_back({Op::Load, jbv, {}, {}, jbSlot, p.sp});
            k1ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jbv}, {}, p.jSlot, p.sp});
            fn.blocks[k1Pre].exit = Exit::Jump;
            fn.blocks[k1Pre].yes = j1Hdr;

            const Value jv{Type::Int, fn.nextValue++};
            const Value jhi{Type::Int, fn.nextValue++};
            const Value condJ{Type::Bool, fn.nextValue++};
            fn.blocks[j1Hdr].ops.push_back({Op::Load, jv, {}, {}, p.jSlot, p.sp});
            fn.blocks[j1Hdr].ops.push_back({Op::Load, jhi, {}, {}, jHiSlot, p.sp});
            fn.blocks[j1Hdr].ops.push_back({Op::Binary, condJ, {jv, jhi}, "<", 0, p.sp});
            fn.blocks[j1Hdr].exit = Exit::Branch;
            fn.blocks[j1Hdr].value = condJ;
            fn.blocks[j1Hdr].yes = j1Body;
            fn.blocks[j1Hdr].no = k1Step;

            auto& j1ops = fn.blocks[j1Body].ops;
            const Value j1{Type::Int, fn.nextValue++};
            const Value bkv{Type::Int, fn.nextValue++};
            const Value aScal{p.elemType, fn.nextValue++};
            const Value baseR1{Type::Int, fn.nextValue++};
            j1ops.push_back({Op::Load, j1, {}, {}, p.jSlot, p.sp});
            j1ops.push_back({Op::Load, bkv, {}, {}, baseKSlot, p.sp});
            Value bIdx = mkAdd(j1ops, bkv, j1);
            Value bv = emitLoadArr(j1ops, p.bSlot, bIdx, ag1);
            j1ops.push_back({Op::Load, aScal, {}, {}, aScalSlots[0], p.sp});
            j1ops.push_back({Op::Load, baseR1, {}, {}, baseISlots[0], p.sp});
            Value cIdx = mkAdd(j1ops, baseR1, j1);
            Value cv = emitLoadArr(j1ops, p.cSlot, cIdx, ag1);
            Value prod{p.elemType, fn.nextValue++};
            j1ops.push_back({Op::Binary, prod, {aScal, bv}, "*", 0, p.sp});
            Value sum{p.elemType, fn.nextValue++};
            j1ops.push_back({Op::Binary, sum, {cv, prod}, "+", 0, p.sp});
            emitStoreArr(j1ops, p.cSlot, cIdx, sum, ag1);
            const Value oneJ{Type::Int, fn.nextValue++};
            j1ops.push_back({Op::Int, oneJ, {}, "1", 0, p.sp});
            Value jNext = mkAdd(j1ops, j1, oneJ, true);
            j1ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jNext}, {}, p.jSlot, p.sp});
            fn.blocks[j1Body].exit = Exit::Jump;
            fn.blocks[j1Body].yes = j1Hdr;
            fn.blocks[j1Body].loopHint = LoopHint::VectorizeParallel;
            fn.blocks[j1Body].loopAccessGroup = ag1;

            const Value k3{Type::Int, fn.nextValue++};
            const Value oneK{Type::Int, fn.nextValue++};
            fn.blocks[k1Step].ops.push_back({Op::Load, k3, {}, {}, p.kSlot, p.sp});
            fn.blocks[k1Step].ops.push_back({Op::Int, oneK, {}, "1", 0, p.sp});
            Value kNext = mkAdd(fn.blocks[k1Step].ops, k3, oneK, true);
            fn.blocks[k1Step].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {kNext}, {}, p.kSlot, p.sp});
            fn.blocks[k1Step].exit = Exit::Jump;
            fn.blocks[k1Step].yes = k1Hdr;

            const Value i3{Type::Int, fn.nextValue++};
            const Value oneI{Type::Int, fn.nextValue++};
            fn.blocks[i1Step].ops.push_back({Op::Load, i3, {}, {}, p.iSlot, p.sp});
            fn.blocks[i1Step].ops.push_back({Op::Int, oneI, {}, "1", 0, p.sp});
            Value iNext = mkAdd(fn.blocks[i1Step].ops, i3, oneI, true);
            fn.blocks[i1Step].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {iNext}, {}, p.iSlot, p.sp});
            fn.blocks[i1Step].exit = Exit::Jump;
            fn.blocks[i1Step].yes = i1Hdr;

            const Value jb3{Type::Int, fn.nextValue++};
            fn.blocks[jbStep].ops.push_back({Op::Load, jb3, {}, {}, jHiSlot, p.sp});
            fn.blocks[jbStep].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jb3}, {}, jbSlot, p.sp});
            fn.blocks[jbStep].exit = Exit::Jump;
            fn.blocks[jbStep].yes = jbHdr;
        }
    }
}

/// After `optimizeMinPlusInnerLoops`, unroll the enclosing `i` loop `4x` when the entire `i` loop
/// body is a single `VectorizeParallel` min-plus row update so `d[k*N + j]` loads once for 4 rows.
inline void unrollMinPlusOuterRows4(FunctionIR& fn) {
    if (fn.blocks.empty()) return;
    const detail::CFGInfo cfg = detail::buildCFG(fn);
    std::unordered_map<unsigned, const Operation*> defOp;
    std::unordered_map<unsigned, unsigned> loadSlot;
    for (const auto& blk : fn.blocks) {
        for (const auto& op : blk.ops) {
            if (op.result.type != Type::Void) defOp[op.result.id] = &op;
            if (op.op == Op::Load) loadSlot[op.result.id] = op.slot;
        }
    }

    struct Plan {
        unsigned preI = 0, hdrI = 0, preJ = 0, hoistBlk = 0, hdrJ = 0, bodyJ = 0, exitJ = 0, exitI = 0;
        unsigned iSlot = 0, jSlot = 0, kSlot = 0, nSlot = 0, arrSlot = 0, dikSlot = 0;
        Operation minTmpl{}, addTmpl{}, dIdxTmpl{}, dSetTmpl{};
        Span sp{};
    };
    std::vector<Plan> plans;
    std::unordered_set<unsigned> usedPre;

    for (std::size_t li = 0; li < cfg.loops.size(); ++li) {
        const auto& lpI = cfg.loops[li];
        if (lpI.preheaders.size() != 1 || (lpI.body.size() != 6 && lpI.body.size() != 5)) continue;
        const unsigned preI = lpI.preheaders[0];
        const unsigned hdrI = lpI.header;
        if (usedPre.contains(preI) || fn.blocks[preI].exit != Exit::Jump || fn.blocks[preI].yes != hdrI) continue;
        const auto& hIBlk = fn.blocks[hdrI];
        if (hIBlk.exit != Exit::Branch || !hIBlk.value || !lpI.body.contains(hIBlk.yes) || lpI.body.contains(hIBlk.no))
            continue;
        const unsigned exitI = hIBlk.no;
        auto cI = defOp.find(hIBlk.value->id);
        if (cI == defOp.end() || cI->second->op != Op::Binary || cI->second->data != "<") continue;
        auto ils = loadSlot.find(cI->second->inputs[0].id);
        auto nls = loadSlot.find(cI->second->inputs[1].id);
        if (ils == loadSlot.end() || nls == loadSlot.end()) continue;
        const unsigned iSlot = ils->second;
        const unsigned nSlot = nls->second;
        std::optional<std::int64_t> iLowC;
        for (const auto& op : fn.blocks[preI].ops) {
            if (op.op == Op::Store && op.slot == iSlot && !op.inputs.empty()) {
                auto d = defOp.find(op.inputs[0].id);
                if (d != defOp.end() && d->second->op == Op::Int) iLowC = parseConstInt(d->second->data);
            }
        }
        if (iLowC != 0) continue;

        const unsigned preJ = hIBlk.yes;
        if (fn.blocks[preJ].exit != Exit::Branch || fn.blocks[preJ].yes == preJ || !lpI.body.contains(preJ)) continue;
        const unsigned hoistBlk = fn.blocks[preJ].yes;
        const unsigned exitJ = fn.blocks[preJ].no;
        if (!lpI.body.contains(hoistBlk) || !lpI.body.contains(exitJ)) continue;
        if (fn.blocks[hoistBlk].exit != Exit::Jump) continue;
        const unsigned hdrJ = fn.blocks[hoistBlk].yes;
        if (!lpI.body.contains(hdrJ)) continue;
        const auto& hJBlk = fn.blocks[hdrJ];
        if (hJBlk.exit != Exit::Branch || hJBlk.no != exitJ) continue;
        const unsigned bodyJ = hJBlk.yes;
        if (!lpI.body.contains(bodyJ) || fn.blocks[bodyJ].loopHint != LoopHint::VectorizeParallel ||
            fn.blocks[bodyJ].exit != Exit::Jump || fn.blocks[bodyJ].yes != hdrJ) continue;
        if (fn.blocks[exitJ].exit != Exit::Jump || fn.blocks[exitJ].yes != hdrI) continue;

        auto cJ = defOp.find(hJBlk.value->id);
        if (cJ == defOp.end() || cJ->second->op != Op::Binary || cJ->second->data != "<") continue;
        auto jls = loadSlot.find(cJ->second->inputs[0].id);
        auto jnls = loadSlot.find(cJ->second->inputs[1].id);
        if (jls == loadSlot.end() || jnls == loadSlot.end() || jnls->second != nSlot) continue;
        const unsigned jSlot = jls->second;

        // Inspect hoistBlk: loads d[i*n + k] -> stores dikSlot
        std::optional<unsigned> arrSlot, dikSlot, kSlot;
        for (const auto& op : fn.blocks[hoistBlk].ops) {
            if (op.op == Op::Store && !op.inputs.empty()) {
                auto ix = defOp.find(op.inputs[0].id);
                if (ix != defOp.end() && ix->second->op == Op::Index && ix->second->boundsProven) {
                    auto als = loadSlot.find(ix->second->inputs[0].id);
                    auto addD = defOp.find(ix->second->inputs[1].id);
                    if (als != loadSlot.end() && addD != defOp.end() && addD->second->op == Op::Binary &&
                        addD->second->data == "+") {
                        auto kl = loadSlot.find(addD->second->inputs[1].id);
                        if (kl != loadSlot.end() && kl->second != iSlot && kl->second != nSlot && kl->second != jSlot) {
                            arrSlot = als->second;
                            dikSlot = op.slot;
                            kSlot = kl->second;
                        }
                    }
                }
            }
        }
        if (!arrSlot || !dikSlot || !kSlot) continue;

        // Inspect bodyJ for exact minT, its addT operand, and dIdxTmpl/dSetTmpl
        std::optional<Operation> minT, addT, idxT, setT;
        for (const auto& op : fn.blocks[bodyJ].ops) {
            if (op.op == Op::Index) idxT = op;
            else if (op.op == Op::IndexSet) setT = op;
            else if (op.op == Op::Min && op.inputs.size() == 2) {
                minT = op;
                for (int side = 0; side < 2; ++side) {
                    auto ad = defOp.find(op.inputs[side].id);
                    if (ad != defOp.end() && ad->second->op == Op::Binary && ad->second->data == "+") {
                        addT = *ad->second;
                        break;
                    }
                }
            }
        }
        if (!minT || !addT || !idxT || !setT || !minT->elemUnsigned) continue;

        // Inspect exitJ: must only increment iSlot by 1 and jump to hdrI
        int iIncs = 0;
        bool okExitJ = true;
        for (const auto& op : fn.blocks[exitJ].ops) {
            if (op.op == Op::Call || op.op == Op::ExternCall || op.op == Op::Index || op.op == Op::IndexSet) {
                okExitJ = false; break;
            }
            if (op.op == Op::Store) {
                if (op.slot == iSlot) ++iIncs;
                else { okExitJ = false; break; }
            }
        }
        if (!okExitJ || iIncs != 1) continue;

        usedPre.insert(preI);
        plans.push_back({preI, hdrI, preJ, hoistBlk, hdrJ, bodyJ, exitJ, exitI,
                         iSlot, jSlot, *kSlot, nSlot, *arrSlot, *dikSlot,
                         *minT, *addT, *idxT, *setT, hIBlk.ops.empty() ? Span{} : hIBlk.ops.front().span});
    }

    int nextAg = 0;
    for (const auto& blk : fn.blocks) {
        nextAg = std::max(nextAg, blk.loopAccessGroup + 1);
        for (const auto& op : blk.ops) nextAg = std::max(nextAg, op.loopAccessGroup + 1);
    }

    for (const auto& p : plans) {
        if (fn.blocks[p.preI].exit != Exit::Jump || fn.blocks[p.preI].yes != p.hdrI) continue;

        if (p.dIdxTmpl.elemBits == 16 &&
            p.dSetTmpl.elemBits == 16 && p.addTmpl.overflowProven && p.addTmpl.width == 16 &&
            p.minTmpl.op == Op::Min && p.minTmpl.width == 16 && p.minTmpl.elemUnsigned) {
            const unsigned floydBlk = static_cast<unsigned>(fn.blocks.size());
            fn.blocks.emplace_back();

            const Value nv{Type::Int, fn.nextValue++};
            const Value c1{Type::Int, fn.nextValue++};
            const Value cLim{Type::Int, fn.nextValue++};
            const Value gLo{Type::Bool, fn.nextValue++};
            const Value gHi{Type::Bool, fn.nextValue++};
            const Value gOk{Type::Bool, fn.nextValue++};
            fn.blocks[p.preI].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, c1, {}, "1", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, cLim, {}, "4096", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, gLo, {nv, c1}, ">=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, gHi, {nv, cLim}, "<=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, gOk, {gLo, gHi}, "&", 0, p.sp});
            fn.blocks[p.preI].exit = Exit::Branch;
            fn.blocks[p.preI].value = gOk;
            fn.blocks[p.preI].yes = floydBlk;
            fn.blocks[p.preI].no = p.hdrI;

            const Value av{Type::IntArray, fn.nextValue++};
            const Value n2{Type::Int, fn.nextValue++};
            const Value kv{Type::Int, fn.nextValue++};
            auto& fops = fn.blocks[floydBlk].ops;
            fops.push_back({Op::Load, av, {}, {}, p.arrSlot, p.sp});
            fops.push_back({Op::Load, n2, {}, {}, p.nSlot, p.sp});
            fops.push_back({Op::Load, kv, {}, {}, p.kSlot, p.sp});
            fops.push_back({Op::FloydStepU16, {Type::Void, fn.nextValue++}, {av, n2, kv}, {}, 0, p.sp});
            fn.blocks[floydBlk].exit = Exit::Jump;
            fn.blocks[floydBlk].yes = p.exitI;
            continue;
        }

        std::array<unsigned, 4> baseISlots{}, dikSlots{};
        for (int r = 0; r < 4; ++r) {
            baseISlots[r] = detail::allocOptSlot(fn, Type::Int);
            dikSlots[r] = detail::allocOptSlot(fn, Type::Int);
        }
        const unsigned baseKSlot = detail::allocOptSlot(fn, Type::Int);
        const int ag4 = nextAg++;

        const unsigned hdrI4 = static_cast<unsigned>(fn.blocks.size());
        const unsigned hoist4 = hdrI4 + 1;
        const unsigned hdrJ4 = hdrI4 + 2;
        const unsigned bodyJ4 = hdrI4 + 3;
        const unsigned stepI4 = hdrI4 + 4;
        fn.blocks.resize(stepI4 + 1);

        auto mkAdd = [&](std::vector<Operation>& ops, Value a, Value b, std::uint8_t w = 64, bool rangeInc = false) {
            Value res{Type::Int, fn.nextValue++};
            Operation o{Op::Binary, res, {a, b}, rangeInc ? "rangeIncrement" : "+", 0, p.sp};
            o.overflowProven = true;
            o.width = w;
            ops.push_back(std::move(o));
            return res;
        };
        auto mkMul = [&](std::vector<Operation>& ops, Value a, Value b) {
            Value res{Type::Int, fn.nextValue++};
            Operation o{Op::Binary, res, {a, b}, "*", 0, p.sp};
            o.overflowProven = true;
            ops.push_back(std::move(o));
            return res;
        };
        auto emitReadD = [&](std::vector<Operation>& ops, Value idx, int ag = -1) {
            Value av{Type::IntArray, fn.nextValue++};
            Operation ix = p.dIdxTmpl;
            ix.result = Value{Type::Int, fn.nextValue++};
            ix.inputs = {av, idx};
            ix.boundsProven = true;
            ix.loopAccessGroup = ag;
            ops.push_back({Op::Load, av, {}, {}, p.arrSlot, p.sp});
            ops.push_back(ix);
            return ix.result;
        };
        auto emitWriteD = [&](std::vector<Operation>& ops, Value idx, Value v, int ag = -1) {
            Value av{Type::IntArray, fn.nextValue++};
            Operation sx = p.dSetTmpl;
            sx.result = Value{Type::Void, fn.nextValue++};
            sx.inputs = {av, idx, v};
            sx.boundsProven = true;
            sx.loopAccessGroup = ag;
            ops.push_back({Op::Load, av, {}, {}, p.arrSlot, p.sp});
            ops.push_back(sx);
        };

        {
            const Value nv{Type::Int, fn.nextValue++};
            const Value c0{Type::Int, fn.nextValue++};
            const Value cLim{Type::Int, fn.nextValue++};
            const Value gLo{Type::Bool, fn.nextValue++};
            const Value gHi{Type::Bool, fn.nextValue++};
            const Value gOk{Type::Bool, fn.nextValue++};
            fn.blocks[p.preI].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, c0, {}, "0", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Int, cLim, {}, "4096", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, gLo, {nv, c0}, ">=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, gHi, {nv, cLim}, "<=", 0, p.sp});
            fn.blocks[p.preI].ops.push_back({Op::Binary, gOk, {gLo, gHi}, "&", 0, p.sp});
            fn.blocks[p.preI].exit = Exit::Branch;
            fn.blocks[p.preI].value = gOk;
            fn.blocks[p.preI].yes = hdrI4;
            fn.blocks[p.preI].no = p.hdrI;
        }

        // hdrI4: (i + 3) < N -> hoist4 else hdrI
        {
            const Value iv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value c3{Type::Int, fn.nextValue++};
            const Value cond{Type::Bool, fn.nextValue++};
            fn.blocks[hdrI4].ops.push_back({Op::Load, iv, {}, {}, p.iSlot, p.sp});
            fn.blocks[hdrI4].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[hdrI4].ops.push_back({Op::Int, c3, {}, "3", 0, p.sp});
            Value i3 = mkAdd(fn.blocks[hdrI4].ops, iv, c3);
            fn.blocks[hdrI4].ops.push_back({Op::Binary, cond, {i3, nv}, "<", 0, p.sp});
            fn.blocks[hdrI4].exit = Exit::Branch;
            fn.blocks[hdrI4].value = cond;
            fn.blocks[hdrI4].yes = hoist4;
            fn.blocks[hdrI4].no = p.hdrI;
        }

        // hoist4: baseI_r = (i+r)*n; baseK = k*n; dik_r = d[baseI_r + k]; j = 0 -> hdrJ4
        {
            auto& ops = fn.blocks[hoist4].ops;
            const Value iv{Type::Int, fn.nextValue++};
            const Value kv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value z0{Type::Int, fn.nextValue++};
            ops.push_back({Op::Load, iv, {}, {}, p.iSlot, p.sp});
            ops.push_back({Op::Load, kv, {}, {}, p.kSlot, p.sp});
            ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            Value bk = mkMul(ops, kv, nv);
            ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {bk}, {}, baseKSlot, p.sp});
            for (int r = 0; r < 4; ++r) {
                Value rr = iv;
                if (r > 0) {
                    const Value cr{Type::Int, fn.nextValue++};
                    ops.push_back({Op::Int, cr, {}, std::to_string(r), 0, p.sp});
                    rr = mkAdd(ops, iv, cr);
                }
                Value bi = mkMul(ops, rr, nv);
                ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {bi}, {}, baseISlots[r], p.sp});
                Value ik = mkAdd(ops, bi, kv);
                Value dik = emitReadD(ops, ik);
                ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {dik}, {}, dikSlots[r], p.sp});
            }
            ops.push_back({Op::Int, z0, {}, "0", 0, p.sp});
            ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {z0}, {}, p.jSlot, p.sp});
            fn.blocks[hoist4].exit = Exit::Jump;
            fn.blocks[hoist4].yes = hdrJ4;
        }

        // hdrJ4 / bodyJ4
        {
            const Value jv{Type::Int, fn.nextValue++};
            const Value nv{Type::Int, fn.nextValue++};
            const Value cond{Type::Bool, fn.nextValue++};
            fn.blocks[hdrJ4].ops.push_back({Op::Load, jv, {}, {}, p.jSlot, p.sp});
            fn.blocks[hdrJ4].ops.push_back({Op::Load, nv, {}, {}, p.nSlot, p.sp});
            fn.blocks[hdrJ4].ops.push_back({Op::Binary, cond, {jv, nv}, "<", 0, p.sp});
            fn.blocks[hdrJ4].exit = Exit::Branch;
            fn.blocks[hdrJ4].value = cond;
            fn.blocks[hdrJ4].yes = bodyJ4;
            fn.blocks[hdrJ4].no = stepI4;

            auto& bops = fn.blocks[bodyJ4].ops;
            const Value j1{Type::Int, fn.nextValue++};
            const Value bk{Type::Int, fn.nextValue++};
            bops.push_back({Op::Load, j1, {}, {}, p.jSlot, p.sp});
            bops.push_back({Op::Load, bk, {}, {}, baseKSlot, p.sp});
            Value kj = mkAdd(bops, bk, j1);
            Value dkj = emitReadD(bops, kj, ag4);
            for (int r = 0; r < 4; ++r) {
                const Value bi{Type::Int, fn.nextValue++};
                const Value dik{Type::Int, fn.nextValue++};
                bops.push_back({Op::Load, bi, {}, {}, baseISlots[r], p.sp});
                Value ij = mkAdd(bops, bi, j1);
                Value dij = emitReadD(bops, ij, ag4);
                bops.push_back({Op::Load, dik, {}, {}, dikSlots[r], p.sp});
                Operation addOp = p.addTmpl;
                addOp.result = Value{Type::Int, fn.nextValue++};
                addOp.inputs = {dik, dkj};
                addOp.overflowProven = true;
                bops.push_back(addOp);
                Operation minOp = p.minTmpl;
                minOp.result = Value{Type::Int, fn.nextValue++};
                minOp.inputs = {dij, addOp.result};
                bops.push_back(minOp);
                emitWriteD(bops, ij, minOp.result, ag4);
            }
            const Value one{Type::Int, fn.nextValue++};
            bops.push_back({Op::Int, one, {}, "1", 0, p.sp});
            Value jNext = mkAdd(bops, j1, one, 64, true);
            bops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {jNext}, {}, p.jSlot, p.sp});
            fn.blocks[bodyJ4].exit = Exit::Jump;
            fn.blocks[bodyJ4].yes = hdrJ4;
            fn.blocks[bodyJ4].loopHint = LoopHint::VectorizeParallel;
            fn.blocks[bodyJ4].loopAccessGroup = ag4;
            fn.blocks[bodyJ4].noInterleave = true;
        }

        // stepI4: i += 4 -> hdrI4
        {
            const Value iv{Type::Int, fn.nextValue++};
            const Value c4{Type::Int, fn.nextValue++};
            fn.blocks[stepI4].ops.push_back({Op::Load, iv, {}, {}, p.iSlot, p.sp});
            fn.blocks[stepI4].ops.push_back({Op::Int, c4, {}, "4", 0, p.sp});
            Value iNext = mkAdd(fn.blocks[stepI4].ops, iv, c4);
            fn.blocks[stepI4].ops.push_back({Op::Store, {Type::Void, fn.nextValue++}, {iNext}, {}, p.iSlot, p.sp});
            fn.blocks[stepI4].exit = Exit::Jump;
            fn.blocks[stepI4].yes = hdrI4;
        }
    }
}
