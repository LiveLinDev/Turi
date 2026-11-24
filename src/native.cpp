#include "native_ir.hpp"
#include "native_opt.hpp"
#include "layout.hpp"
#include "native_runtime.hpp"
#include "native_arrays.hpp"
#include "native_strings.hpp"

#include <algorithm>
#include <charconv>
#include <deque>
#include <map>
#include <queue>
#include <set>
#include <sstream>

namespace veloz::native {
namespace {

std::string reg(Value v) { return "%v" + std::to_string(v.id); }

std::string symbol(std::string_view name) {
    static constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out = "@\"turi.";
    for (const unsigned char c : name) {
        if (c >= 32 && c < 127 && c != '"' && c != '\\') out += static_cast<char>(c);
        else {
            out += '\\';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out + '"';
}

// The emitted module links against the 64-bit host CRT. All Turi call conventions are internal.
class ModuleWriter {
    const Program& program;
    std::map<std::string, unsigned> pool;
    std::map<std::string, unsigned> diagnosticFilePool;
    std::vector<std::string> diagnosticFiles;
    std::ostringstream bodies;
    std::ostringstream coldOutlines;
    std::vector<std::string> metadata;
    unsigned nextMetaId = 1;
    unsigned coldBranchProfId = 0;
    std::vector<unsigned> curScopeMeta, curNoAliasMeta;
    std::unordered_map<int, unsigned> curAccessGroupMeta;
    bool emitsRegion = false;
    bool emitsStackArena = false;
    bool curPureStackArena = false;
    bool checkDepth = true;
    bool auditCopies = false;
    bool usesTileRepeat = false;
    bool usesGemmF32 = false;
    bool usesMatMulModU16 = false;
    bool usesFloydStepU16 = false;
    bool usesArgMinU16 = false;
    std::set<std::uint8_t> usesFindFirstBits;
    std::unordered_map<unsigned, std::int64_t> curConstInts;
    bool usesSysArgs = false;
    bool usesDropFile = false;
    bool usesDropListener = false;
    bool usesDropSocket = false;
    bool usesDropMutex = false;
    bool usesDropLock = false;
    bool usesDropScope = false;
    bool usesDropTask = false;
    bool usesDropRegion = false;
    bool usesDropAtomic = false;
    bool usesCloneAtomic = false;
    bool usesDropRing = false;
    bool usesCloneRing = false;
    enum class SysOpKind {
        FileOpen, FileRead, FileReadAll, FileReadLine, FileWrite, FileFlush,
        TryReadFile, TryWriteFile,
        TcpListen, TcpAccept, TcpConnect, SockRead, SockWrite
    };
    struct SysHelperKey {
        SysOpKind kind;
        Type resType;
        bool hasHost = false;
        bool operator<(const SysHelperKey& o) const {
            if (kind != o.kind) return static_cast<int>(kind) < static_cast<int>(o.kind);
            if (resType != o.resType) return resType < o.resType;
            return hasHost < o.hasHost;
        }
    };
    std::set<Type> emitResOks;
    std::set<Type> doneResOks;
    std::set<Type> emitResErrs;
    std::set<Type> doneResErrs;
    std::set<SysHelperKey> emitSysHelpers;
    std::set<SysHelperKey> doneSysHelpers;
    std::set<std::string> usedDecls;
    std::set<std::string> usedRawDecls;
    std::set<Type> usedEnumValTables;
    std::set<std::pair<std::int64_t, std::int64_t>> usedDigitSumLuts;
    std::vector<Type> curSlotTypes;
    std::vector<bool> curSlotByRef;
    std::set<Type> emitDrops, emitDropUnaligned, emitClones, emitRepeats, emitSlices, emitAts, emitPushes, emitPops, emitClears;
    std::set<Type> emitVClones, emitMapSets, emitMapHas, emitMapDels, emitMapKeys, emitMapVals;
    std::set<Type> emitSetAdds, emitSetHas, emitSetDels, emitSetItems;
    std::set<Type> emitTblClears, emitTblReserves, emitTblWithCaps;
    std::map<Type, Type> emitMapGets;
    bool emitTblSecure = false;
    std::set<Type> emitPoolWithCaps, emitPoolReserves, emitPoolClears, emitPoolAdds, emitPoolHas, emitPoolHasMono, emitPoolSets, emitPoolHandles;
    std::map<Type, Type> emitPoolGets, emitPoolGetsMono, emitPoolRemoves;
    std::set<Type> emitRcNews, emitRcGets, emitRcSets, emitRcReuses, emitRcUpdates;
    std::set<Type> emitFzNews, emitFzGets, emitPmaps, emitRingPushes;
    std::map<Type, Type> emitRingPops;
    std::set<std::pair<Type, Type>> emitFzReads;
    std::set<std::pair<Type, Type>> emitTaskSpawns;
    std::set<std::pair<Type, Type>> usedDynBoxes;
    bool usesDynRuntime = false;
    std::set<std::size_t> usedClosures;
    bool usesFnRuntime = false;

    std::string llvmType(Type type) const {
        if (type == Type::IntArray || program.isArray(type)) return "%array";
        if (program.isMap(type) || program.isSet(type)) return "%table";
        if (program.isPool(type)) return "%pool";
        if (program.isHandle(type)) return "%handle";
        if (program.isAnyPtr(type) || program.isShared(type) || program.isTask(type) ||
            program.isFrozen(type) || program.isAtomic(type) || program.isRingQueue(type)) return "ptr";
        if (program.isSimd(type)) {
            return "<" + std::to_string(program.simdLanes(type)) + " x " + llvmType(program.simdElem(type)) + ">";
        }
        if (program.isDyn(type)) return "%dyn";
        if (program.isFn(type)) return "%fn";
        if (type >= Type::NamedBase) return "%t" + std::to_string(static_cast<int>(type));
        switch (type) {
        case Type::I8:
        case Type::U8: return "i8";
        case Type::I16:
        case Type::U16: return "i16";
        case Type::I32:
        case Type::U32: return "i32";
        case Type::Int:
        case Type::U64: return "i64";
        case Type::F32: return "float";
        case Type::Float: return "double";
        case Type::Bool: return "i1";
        case Type::Str: return "%text";
        case Type::StringBuilder: return "%array";
        case Type::File:
        case Type::TcpListener:
        case Type::Socket:
        case Type::Mutex:
        case Type::Lock:
        case Type::Scope:
        case Type::Region:
            return "ptr";
        case Type::Void: return "void";
        default: throw std::logic_error("unresolved type during LLVM emission");
        }
    }
    std::string operand(Value v) const { return llvmType(v.type) + " " + reg(v); }
    Type elemOf(Type t) const { return t == Type::IntArray ? Type::Int : program.elemOf(t); }
    bool needsDropImpl(Type t, std::set<Type>& seen) const {
        if (t == Type::IntArray || t == Type::Str || t == Type::StringBuilder ||
            Program::isPrimitiveResource(t) ||
            program.isArray(t) || program.isMap(t) || program.isSet(t) ||
            program.isPool(t) || program.isShared(t) || program.isTask(t) ||
            program.isFrozen(t) || program.isAtomic(t) || program.isRingQueue(t) ||
            program.isDyn(t) || program.isFn(t)) return true;
        if (t < Type::NamedBase || program.isHandle(t) || program.isAnyPtr(t) || program.isSimd(t)) return false;
        if (!seen.insert(t).second) return false;
        const auto* decl = program.declaration(t);
        if (!decl) return false;
        for (const auto& v : decl->variants) {
            for (const auto& f : v.fields) {
                if (needsDropImpl(f.type, seen)) return true;
            }
        }
        return false;
    }
    bool needsDrop(Type t) const {
        std::set<Type> seen;
        return needsDropImpl(t, seen);
    }
    std::string strideExpr(Type elemType) const {
        return "ptrtoint (ptr getelementptr (" + llvmType(elemType) + ", ptr null, i32 1) to i64)";
    }
    int keyKindOf(Type k) const {
        if (k == Type::Int || k == Type::U64 || program.isHandle(k)) return 0;
        if (k == Type::Bool || k == Type::I8 || k == Type::U8) return 1;
        if (k == Type::I16 || k == Type::U16) return 4;
        if (k == Type::I32 || k == Type::U32) return 2;
        if (k == Type::Str) return 3;
        return layoutOf(k).tagTy == "i32" ? 2 : 1;
    }
    std::string keyStrideExpr(Type k) const {
        if (k == Type::Int || k == Type::U64 || program.isHandle(k)) return "8";
        if (k == Type::Bool || k == Type::I8 || k == Type::U8) return "1";
        if (k == Type::I16 || k == Type::U16) return "2";
        if (k == Type::I32 || k == Type::U32) return "4";
        if (k == Type::Str) return strideExpr(Type::Str);
        return layoutOf(k).tagTy == "i32" ? "4" : "1";
    }
    std::string keyStorageLlvm(Type k) const {
        if (k == Type::Int || k == Type::U64 || program.isHandle(k)) return "i64";
        if (k == Type::Bool || k == Type::I8 || k == Type::U8) return "i8";
        if (k == Type::I16 || k == Type::U16) return "i16";
        if (k == Type::I32 || k == Type::U32) return "i32";
        if (k == Type::Str) return "%text";
        return layoutOf(k).tagTy;
    }
    bool isZeroSized(Type t) const {
        return t >= Type::NamedBase && !program.isArray(t) && !program.isMap(t) &&
               !program.isSet(t) && !program.isPool(t) && !program.isHandle(t) &&
               !program.isShared(t) && !program.isTask(t) && !program.isFrozen(t) &&
               !program.isAtomic(t) && !program.isRingQueue(t) &&
               !program.isDyn(t) && !program.isFn(t) && !program.isAnyPtr(t) && !program.isSimd(t) && layoutOf(t).size == 0;
    }
    std::string requestDrop(Type t) {
        if (t == Type::IntArray || t == Type::StringBuilder || (program.isArray(t) && !needsDrop(elemOf(t)))) return "@tz.drop";
        if (t == Type::Str) return "@tz.dropText";
        if (t == Type::File) { usesDropFile = true; return "@tz.dropFile"; }
        if (t == Type::TcpListener) { usesDropListener = true; return "@tz.dropListener"; }
        if (t == Type::Socket) { usesDropSocket = true; return "@tz.dropSocket"; }
        if (t == Type::Mutex) { usesDropMutex = true; return "@tz.dropMutex"; }
        if (t == Type::Lock) { usesDropLock = true; return "@tz.dropLock"; }
        if (t == Type::Scope) { usesDropScope = true; return "@tz.dropScope"; }
        if (t == Type::Region) { usesDropRegion = true; return "@tz.dropRegion"; }
        if (program.isTask(t)) { usesDropTask = true; return "@tz.dropTask"; }
        if (program.isAtomic(t)) { usesDropAtomic = true; return "@tz.dropAtomic"; }
        if (program.isRingQueue(t)) { usesDropRing = true; return "@tz.dropRing"; }
        if (program.isDyn(t)) { usesDynRuntime = true; return "@tz.dropDyn"; }
        if (program.isFn(t)) { usesFnRuntime = true; return "@tz.dropFn"; }
        emitDrops.insert(t);
        return "@tz.drop.t" + std::to_string(static_cast<int>(t));
    }
    std::string requestDropUnaligned(Type t) {
        if (!needsDrop(t)) return "null";
        emitDropUnaligned.insert(t);
        return "@tz.dropUnaligned.t" + std::to_string(static_cast<int>(t));
    }
    std::string requestClone(Type t) {
        if (t == Type::IntArray) return "@tz.clone";
        if (t == Type::Str) return "@tz.cloneText";
        if (t == Type::StringBuilder) return "@tz.sbClone";
        if (program.isAtomic(t)) { usesCloneAtomic = true; return "@tz.cloneAtomic"; }
        if (program.isRingQueue(t)) { usesCloneRing = true; return "@tz.cloneRing"; }
        if (program.isDyn(t)) { usesDynRuntime = true; return "@tz.cloneDyn"; }
        if (program.isFn(t)) { usesFnRuntime = true; return "@tz.cloneFn"; }
        emitClones.insert(t);
        return "@tz.clone.t" + std::to_string(static_cast<int>(t));
    }
    std::string requestVClone(Type t) {
        emitVClones.insert(t);
        return "@tz.vclone.t" + std::to_string(static_cast<int>(t));
    }
    std::string cloneCallArgs(Type t, const std::string& valExpr, const std::string& msgExpr) const {
        if (t == Type::IntArray)
            return valExpr + ", ptr null, " + msgExpr + (auditCopies ? ", i64 0, i64 0" : "");
        return valExpr + ", " + msgExpr;
    }
    std::string requestRepeat(Type arrT) {
        emitRepeats.insert(arrT);
        return "@tz.repeat.t" + std::to_string(static_cast<int>(arrT));
    }
    std::string requestSlice(Type arrT) {
        emitSlices.insert(arrT);
        return "@tz.slice.t" + std::to_string(static_cast<int>(arrT));
    }
    std::string requestAt(Type arrT) {
        emitAts.insert(arrT);
        return "@tz.at.t" + std::to_string(static_cast<int>(arrT));
    }
    std::string requestPush(Type arrT) {
        emitPushes.insert(arrT);
        return "@tz.push.t" + std::to_string(static_cast<int>(arrT));
    }
    std::string requestPop(Type arrT) {
        emitPops.insert(arrT);
        return "@tz.pop.t" + std::to_string(static_cast<int>(arrT));
    }
    std::string requestClear(Type arrT) {
        emitClears.insert(arrT);
        return "@tz.clear.t" + std::to_string(static_cast<int>(arrT));
    }
    std::string requestTblWithCap(Type tblT) {
        emitTblWithCaps.insert(tblT);
        return "@tz.tblWithCap.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestTblSecure() {
        emitTblSecure = true;
        return "@tz.tblSecure";
    }
    std::string requestTblClear(Type tblT) {
        emitTblClears.insert(tblT);
        return "@tz.tblClear.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestTblReserve(Type tblT) {
        emitTblReserves.insert(tblT);
        return "@tz.tblReserve.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestMapSet(Type tblT) {
        emitMapSets.insert(tblT);
        return "@tz.mapSet.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestMapGet(Type tblT, Type optT) {
        emitMapGets[tblT] = optT;
        return "@tz.mapGet.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestMapHas(Type tblT) {
        emitMapHas.insert(tblT);
        return "@tz.mapHas.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestMapDel(Type tblT) {
        emitMapDels.insert(tblT);
        return "@tz.mapDel.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestMapKeys(Type tblT) {
        emitMapKeys.insert(tblT);
        return "@tz.mapKeys.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestMapVals(Type tblT) {
        emitMapVals.insert(tblT);
        return "@tz.mapValues.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestSetAdd(Type tblT) {
        emitSetAdds.insert(tblT);
        return "@tz.setAdd.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestSetHas(Type tblT) {
        emitSetHas.insert(tblT);
        return "@tz.setHas.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestSetDel(Type tblT) {
        emitSetDels.insert(tblT);
        return "@tz.setDel.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestSetItems(Type tblT) {
        emitSetItems.insert(tblT);
        return "@tz.setItems.t" + std::to_string(static_cast<int>(tblT));
    }
    std::string requestPoolWithCap(Type poolT) {
        emitPoolWithCaps.insert(poolT);
        return "@tz.poolWithCap.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolReserve(Type poolT) {
        emitPoolReserves.insert(poolT);
        return "@tz.poolReserve.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolClear(Type poolT) {
        emitPoolClears.insert(poolT);
        return "@tz.poolClear.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolAdd(Type poolT) {
        emitPoolAdds.insert(poolT);
        return "@tz.poolAdd.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolGet(Type poolT, Type optT) {
        emitPoolGets[poolT] = optT;
        return "@tz.poolGet.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolGetMono(Type poolT, Type optT) {
        emitPoolGetsMono[poolT] = optT;
        return "@tz.poolGetMono.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolHas(Type poolT) {
        emitPoolHas.insert(poolT);
        return "@tz.poolHas.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolHasMono(Type poolT) {
        emitPoolHasMono.insert(poolT);
        return "@tz.poolHasMono.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolSet(Type poolT) {
        emitPoolSets.insert(poolT);
        return "@tz.poolSet.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolRemove(Type poolT, Type optT) {
        emitPoolRemoves[poolT] = optT;
        return "@tz.poolRemove.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestPoolHandles(Type poolT) {
        emitPoolHandles.insert(poolT);
        return "@tz.poolHandles.t" + std::to_string(static_cast<int>(poolT));
    }
    std::string requestRcNew(Type shT) {
        emitRcNews.insert(shT);
        return "@tz.rcNew.t" + std::to_string(static_cast<int>(shT));
    }
    std::string requestRcGet(Type shT) {
        emitRcGets.insert(shT);
        return "@tz.rcGet.t" + std::to_string(static_cast<int>(shT));
    }
    std::string requestRcSet(Type shT) {
        emitRcSets.insert(shT);
        return "@tz.rcSet.t" + std::to_string(static_cast<int>(shT));
    }
    std::string requestRcReuse(Type shT) {
        emitRcReuses.insert(shT);
        return "@tz.rcReuse.t" + std::to_string(static_cast<int>(shT));
    }
    std::string requestRcUpdate(Type shT) {
        emitRcUpdates.insert(shT);
        return "@tz.rcUpdate.t" + std::to_string(static_cast<int>(shT));
    }
    std::string requestFzNew(Type fzT) {
        emitFzNews.insert(fzT);
        return "@tz.fzNew.t" + std::to_string(static_cast<int>(fzT));
    }
    std::string requestFzGet(Type fzT) {
        emitFzGets.insert(fzT);
        return "@tz.fzGet.t" + std::to_string(static_cast<int>(fzT));
    }
    std::string requestFzRead(Type fzT, Type retT) {
        emitFzReads.emplace(fzT, retT);
        return "@tz.fzRead.t" + std::to_string(static_cast<int>(fzT)) +
               ".r" + std::to_string(static_cast<int>(retT));
    }
    std::string requestTaskRun(Type valT, Type retT) {
        emitTaskSpawns.emplace(valT, retT);
        return "@tz.task.run.v" + std::to_string(static_cast<int>(valT)) +
               ".r" + std::to_string(static_cast<int>(retT));
    }
    std::string requestPmap(Type arrT) {
        emitPmaps.insert(arrT);
        return "@tz.pmap.t" + std::to_string(static_cast<int>(arrT));
    }
    std::string requestRingPush(Type rqT) {
        emitRingPushes.insert(rqT);
        return "@tz.ringPush.t" + std::to_string(static_cast<int>(rqT));
    }
    std::string requestRingPop(Type rqT, Type optT) {
        emitRingPops[rqT] = optT;
        return "@tz.ringPop.t" + std::to_string(static_cast<int>(rqT));
    }
    std::string requestResOk(Type resT) {
        emitResOks.insert(resT);
        return "@tz.resOk.t" + std::to_string(static_cast<int>(resT));
    }
    std::string requestResErr(Type resT) {
        emitResErrs.insert(resT);
        return "@tz.resErr.t" + std::to_string(static_cast<int>(resT));
    }
    std::string requestSysHelper(SysOpKind kind, Type resT) {
        emitSysHelpers.insert(SysHelperKey{kind, resT, false});
        return "@tz.sysH." + std::to_string(static_cast<int>(kind)) + ".t" + std::to_string(static_cast<int>(resT));
    }
    unsigned variantIndexByName(Type t, std::string_view vname) const {
        const auto* decl = program.declaration(t);
        if (!decl) throw std::logic_error("expected enum declaration");
        for (std::size_t i = 0; i < decl->variants.size(); ++i) {
            if (decl->variants[i].name == vname) return static_cast<unsigned>(i);
        }
        throw std::logic_error("missing variant " + std::string(vname));
    }

    std::string slotPtr(unsigned s) const {
        if (s < curSlotByRef.size() && curSlotByRef[s]) return "%arg" + std::to_string(s);
        return "%slot" + std::to_string(s);
    }

    LayoutCalculator layoutCalc{program};
    std::pair<std::size_t, std::size_t> sizeAlignOf(Type type) const {
        return layoutCalc.sizeAlignOf(type);
    }
    const TypeLayout& layoutOf(Type type) const {
        return layoutCalc.layoutOf(type);
    }
    std::string allocaAlignSuffix(Type type) const {
        const std::size_t a = sizeAlignOf(type).second;
        if (a > 8) return ", align " + std::to_string(a);
        return {};
    }
    std::string heapAlignSuffix(Type type) const {
        return layoutCalc.llvmNaturalAlignOf(type) > 16 ? ", align 1" : "";
    }
    std::string simdLlvmSuffix(Type elem, unsigned lanes) const {
        if (elem == Type::F32) return "v" + std::to_string(lanes) + "f32";
        if (elem == Type::Float) return "v" + std::to_string(lanes) + "f64";
        return "v" + std::to_string(lanes) + "i" + std::to_string(Program::intBitWidth(elem));
    }
    std::string simdLlvmSplatConst(Type elem, unsigned lanes, std::string_view lit) const {
        const std::string ety = llvmType(elem);
        std::string out = "<";
        for (unsigned k = 0; k < lanes; ++k) {
            if (k > 0) out += ", ";
            out += ety + " " + std::string(lit);
        }
        return out + ">";
    }

    void fieldList(std::ostream& out, const Variant& variant, const VariantLayout& vl,
                   bool packed, std::size_t trailingPad) const {
        out << (packed ? "<{ " : "{ ");
        bool firstElem = true;
        auto sep = [&]() {
            if (!firstElem) out << ", ";
            firstElem = false;
        };
        for (std::size_t phys = 0; phys < vl.physOrder.size(); ++phys) {
            if (phys < vl.llvmPrePad.size() && vl.llvmPrePad[phys] > 0) {
                sep();
                out << "[" << vl.llvmPrePad[phys] << " x i8]";
            }
            sep();
            out << llvmType(variant.fields[vl.physOrder[phys]].type);
        }
        if (trailingPad > 0) {
            sep();
            out << "[" << trailingPad << " x i8]";
        }
        out << (packed ? " }>" : " }");
    }
    void writeTypes(std::ostream& out) const {
        for (const auto& decl : program.data) {
            const auto& lay = layoutOf(decl.type);
            if (!lay.isEnum) {
                out << llvmType(decl.type) << " = type ";
                fieldList(out, decl.variants.at(0), lay.variants.at(0), lay.packed, lay.trailingPad);
                out << "\n";
            } else if (!lay.multiPayload) {
                out << llvmType(decl.type) << " = type { " << lay.tagTy;
                if (lay.payloadVariants == 1) {
                    const auto& pv = decl.variants.at(lay.singlePayloadVariant);
                    const auto& pvl = lay.variants.at(lay.singlePayloadVariant);
                    for (std::size_t pi = 0; pi < pvl.physOrder.size(); ++pi) {
                        if (pi < lay.singlePayPrePad.size() && lay.singlePayPrePad[pi] > 0) {
                            out << ", [" << lay.singlePayPrePad[pi] << " x i8]";
                        }
                        out << ", " << llvmType(pv.fields[pvl.physOrder[pi]].type);
                    }
                }
                if (lay.trailingPad > 0) out << ", [" << lay.trailingPad << " x i8]";
                out << " }\n";
            } else {
                for (std::size_t vi = 0; vi < decl.variants.size(); ++vi) {
                    if (decl.variants[vi].fields.empty()) continue;
                    out << llvmType(decl.type) << ".p" << vi << " = type ";
                    fieldList(out, decl.variants[vi], lay.variants[vi], false, 0);
                    out << "\n";
                }
                out << llvmType(decl.type) << " = type { " << lay.tagTy;
                if (lay.payPrePad > 0) out << ", [" << lay.payPrePad << " x i8]";
                out << ", [" << lay.numWords << " x " << lay.wordTy << "]";
                if (lay.trailingPad > 0) out << ", [" << lay.trailingPad << " x i8]";
                out << " }\n";
            }
        }
    }
    void construct(const Operation& op) {
        const auto& lay = layoutOf(op.result.type);
        const auto result = reg(op.result);
        const auto type = llvmType(op.result.type);
        if (lay.multiPayload) {
            if (op.inputs.empty()) {
                bodies << "  " << result << " = insertvalue " << type << " zeroinitializer, "
                       << lay.tagTy << " " << op.slot << ", 0\n";
                return;
            }
            const std::string scratch = "%enum.tmp." + std::to_string(static_cast<int>(op.result.type));
            const std::string pType = type + ".p" + std::to_string(op.slot);
            bodies << "  store " << type << " zeroinitializer, ptr " << scratch << "\n"
                   << "  " << result << ".tagp = getelementptr inbounds " << type << ", ptr " << scratch << ", i32 0, i32 0\n"
                   << "  store " << lay.tagTy << " " << op.slot << ", ptr " << result << ".tagp\n"
                   << "  " << result << ".pay = getelementptr inbounds " << type << ", ptr " << scratch
                   << ", i32 0, i32 " << lay.payFieldIdx << "\n";
            for (std::size_t i = 0; i < op.inputs.size(); ++i) {
                const unsigned phys = lay.variants.at(op.slot).logicalToLlvm.at(op.fields.at(i));
                bodies << "  " << result << ".f" << i << " = getelementptr inbounds " << pType
                       << ", ptr " << result << ".pay, i32 0, i32 " << phys << "\n"
                       << "  store " << operand(op.inputs[i]) << ", ptr " << result << ".f" << i << "\n";
            }
            bodies << "  " << result << " = load " << type << ", ptr " << scratch << "\n";
            return;
        }
        std::string aggregate = "zeroinitializer";
        if (lay.isEnum) {
            const auto tagged = op.inputs.empty() ? result : result + ".tag";
            bodies << "  " << tagged << " = insertvalue " << type << " " << aggregate
                   << ", " << lay.tagTy << " " << op.slot << ", 0\n";
            aggregate = tagged;
        } else if (op.inputs.empty()) {
            bodies << "  " << result << " = freeze " << type << " zeroinitializer\n";
        }
        for (std::size_t i = 0; i < op.inputs.size(); ++i) {
            const auto name = i + 1 == op.inputs.size() ? result : result + ".field" + std::to_string(i);
            const unsigned elemIdx = lay.isEnum
                ? lay.singlePayLogicalToLlvm.at(op.fields.at(i))
                : lay.variants.at(0).logicalToLlvm.at(op.fields.at(i));
            bodies << "  " << name << " = insertvalue " << type << " " << aggregate
                   << ", " << operand(op.inputs[i]) << ", " << elemIdx << "\n";
            aggregate = name;
        }
    }
    std::string literal(const std::string& text) {
        if (text.empty()) return "%text zeroinitializer";
        std::uint32_t pref = 0;
        for (std::size_t i = 0; i < 4 && i < text.size(); ++i) {
            pref |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[i])) << (i * 8);
        }
        if (text.size() <= 12) {
            std::uint64_t tail = 0;
            for (std::size_t i = 4; i < text.size(); ++i) {
                tail |= static_cast<std::uint64_t>(static_cast<unsigned char>(text[i])) << ((i - 4) * 8);
            }
            const std::string tailStr = tail == 0
                ? "ptr null"
                : ("ptr inttoptr (i64 " + std::to_string(tail) + " to ptr)");
            return "%text { i32 " + std::to_string(static_cast<std::int32_t>(text.size())) +
                   ", i32 " + std::to_string(static_cast<std::int32_t>(pref)) +
                   ", " + tailStr + " }";
        }
        const auto [it, inserted] = pool.emplace(text, static_cast<unsigned>(pool.size()));
        return "%text { i32 " + std::to_string(static_cast<std::int32_t>(text.size())) +
               ", i32 " + std::to_string(static_cast<std::int32_t>(pref)) +
               ", ptr @tz.s" + std::to_string(it->second) + " }";
    }
    static std::string quoted(std::string_view text) {
        constexpr std::string_view hex = "0123456789abcdef";
        std::string result = "\"";
        for (unsigned char c : text) {
            if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
            else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
            else result += static_cast<char>(c);
        }
        return result + '"';
    }
    static constexpr std::string_view diagnosticIds[] = {
        "R_OVERFLOW", "R_DIV_ZERO", "R_BOUNDS", "R_ARRAY_SIZE", "R_ALLOC", "R_DEPTH", "R_ASSERT", "R_PARSE", "R_NULL_PTR", "R_ALIGN", "R_CONTRACT"
    };
    static unsigned diagnosticCode(std::string_view id) {
        unsigned code = 0;
        for (const auto candidate : diagnosticIds) {
            if (candidate == id) return code;
            ++code;
        }
        throw std::logic_error("unknown native diagnostic");
    }
    std::string diagnostic(std::string_view id, Span span) {
        const auto code = diagnosticCode(id);
        auto file = program.fileOf(span);
        const auto [it, inserted] = diagnosticFilePool.emplace(
            file, static_cast<unsigned>(diagnosticFiles.size()));
        if (inserted) diagnosticFiles.push_back(std::move(file));
        return "%diag { i32 " + std::to_string(code) + ", i32 " + std::to_string(it->second)
            + ", i64 " + std::to_string(span.start) + ", i64 " + std::to_string(span.end) + " }";
    }
    void writeStrings(std::ostream& out) const {
        constexpr std::string_view hex = "0123456789ABCDEF";
        for (const auto& [text, id] : pool) {
            // A trailing byte gives even empty strings a valid address for memcmp/fwrite.
            out << "@tz.s" << id << " = private unnamed_addr constant [" << text.size() + 1 << " x i8] c\"";
            for (unsigned char c : text) {
                out << '\\' << hex[c >> 4] << hex[c & 15];
            }
            out << "\\00\"\n";
        }
    }
    static std::optional<std::pair<std::uint64_t, unsigned>> findExactBarrett(
        std::uint64_t d, std::uint64_t xMax, unsigned width) {
        if (d < 2 || (d & (d - 1)) == 0) return std::nullopt;
        const std::uint64_t maxMul = (width >= 64) ? ~0ULL : ((1ULL << width) - 1ULL);
        if (xMax > maxMul) return std::nullopt;
        const std::uint64_t qMax = xMax / d;
        const unsigned maxS = std::min<unsigned>(width * 2 - 1, 62);
        for (unsigned s = width; s <= maxS; ++s) {
            const std::uint64_t pow2s = 1ULL << s;
            const std::uint64_t m = (pow2s + d - 1ULL) / d;
            if (m > maxMul) continue;
            const std::uint64_t rem = m * d - pow2s;
            if (rem == 0 || (qMax + 1ULL) <= (m - 1ULL) / rem) {
                return std::pair{m, s};
            }
        }
        return std::nullopt;
    }
    void binary(const Operation& op) {
        const auto left = op.inputs[0];
        const auto right = op.inputs[1];
        const auto result = reg(op.result);
        const auto& token = op.data;
        if (token == "rangeIncrement") {
            bodies << "  " << result << " = add nsw " << operand(left) << ", " << reg(right) << "\n";
            return;
        }
        if (Program::isFloatType(left.type)) {
            static const std::map<std::string, std::string> fOps{
                {"+", "fadd"}, {"-", "fsub"}, {"*", "fmul"}, {"/", "fdiv"}};
            if (auto it = fOps.find(token); it != fOps.end()) {
                bodies << "  " << result << " = " << it->second << " " << operand(left) << ", " << reg(right) << "\n";
                return;
            }
            static const std::map<std::string, std::string> fPreds{
                {"==", "oeq"}, {"!=", "une"}, {"<", "olt"}, {"<=", "ole"}, {">", "ogt"}, {">=", "oge"}};
            bodies << "  " << result << " = fcmp " << fPreds.at(token) << " " << operand(left)
                   << ", " << reg(right) << "\n";
            return;
        }
        if (left.type == Type::Str) {
            if (token == "+") {
                bodies << "  " << result << " = call %text @tz.concatText(" << operand(left) << ", "
                       << operand(right) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
                return;
            }
            if (token == "==" || token == "!=") {
                const auto same = token == "==" ? result : result + ".eq";
                bodies << "  " << same << " = call i1 @tz.eq(" << operand(left) << ", " << operand(right) << ")\n";
                if (token == "!=") bodies << "  " << result << " = xor i1 " << same << ", true\n";
                return;
            }
            bodies << "  " << result << ".cmp = call i64 @tz.cmpText(" << operand(left) << ", " << operand(right) << ")\n";
            static const std::map<std::string, std::string> sPreds{
                {"<", "slt"}, {"<=", "sle"}, {">", "sgt"}, {">=", "sge"}};
            bodies << "  " << result << " = icmp " << sPreds.at(token) << " i64 " << result << ".cmp, 0\n";
            return;
        }
        if (program.isHandle(left.type)) {
            bodies << "  " << result << ".li = extractvalue " << operand(left) << ", 0\n"
                   << "  " << result << ".lg = extractvalue " << operand(left) << ", 1\n"
                   << "  " << result << ".ri = extractvalue " << operand(right) << ", 0\n"
                   << "  " << result << ".rg = extractvalue " << operand(right) << ", 1\n"
                   << "  " << result << ".ei = icmp eq i32 " << result << ".li, " << result << ".ri\n"
                   << "  " << result << ".eg = icmp eq i32 " << result << ".lg, " << result << ".rg\n";
            if (token == "==") {
                bodies << "  " << result << " = and i1 " << result << ".ei, " << result << ".eg\n";
            } else {
                bodies << "  " << result << ".eq = and i1 " << result << ".ei, " << result << ".eg\n"
                       << "  " << result << " = xor i1 " << result << ".eq, true\n";
            }
            return;
        }
        if (program.isAnyPtr(left.type)) {
            const char* pred = (token == "==") ? "eq" : "ne";
            bodies << "  " << result << " = icmp " << pred << " ptr " << reg(left) << ", " << reg(right) << "\n";
            return;
        }
        if (program.isSimd(left.type)) {
            const Type elemT = program.simdElem(left.type);
            const unsigned lanes = program.simdLanes(left.type);
            const std::string vty = llvmType(left.type);
            if (token == "==" || token == "!=") {
                const std::string redIntr = "llvm.vector.reduce.and.v" + std::to_string(lanes) + "i1";
                usedRawDecls.insert("declare i1 @" + redIntr + "(<" + std::to_string(lanes) + " x i1>)");
                const std::string cmpReg = result + ".cmp";
                if (Program::isFloatType(elemT)) {
                    bodies << "  " << cmpReg << " = fcmp oeq " << operand(left) << ", " << reg(right) << "\n";
                } else {
                    bodies << "  " << cmpReg << " = icmp eq " << operand(left) << ", " << reg(right) << "\n";
                }
                const std::string redReg = (token == "==") ? result : (result + ".eq");
                bodies << "  " << redReg << " = call i1 @" << redIntr << "(<" << std::to_string(lanes) << " x i1> " << cmpReg << ")\n";
                if (token == "!=") bodies << "  " << result << " = xor i1 " << redReg << ", true\n";
                return;
            }
            if (Program::isFloatType(elemT)) {
                const char* fop = (token == "+") ? "fadd" : (token == "-") ? "fsub" : (token == "*") ? "fmul" : "fdiv";
                bodies << "  " << result << " = " << fop << " " << operand(left) << ", " << reg(right) << "\n";
                return;
            }
            if (token == "<<" || token == ">>" || token == ">>>") {
                const unsigned w = Program::intBitWidth(elemT);
                const std::string mvec = simdLlvmSplatConst(elemT, lanes, std::to_string(w - 1u));
                const std::string shReg = result + ".sh";
                const char* shiftOp = (token == "<<") ? "shl"
                    : ((token == ">>>" || Program::isUnsignedInt(elemT)) ? "lshr" : "ashr");
                bodies << "  " << shReg << " = and " << operand(right) << ", " << mvec << "\n"
                       << "  " << result << " = " << shiftOp << " " << vty << " " << reg(left) << ", " << shReg << "\n";
                return;
            }
            const char* iop = (token == "+") ? "add"
                : (token == "-") ? "sub"
                : (token == "*") ? "mul"
                : (token == "&") ? "and"
                : (token == "|") ? "or" : "xor";
            bodies << "  " << result << " = " << iop << " " << operand(left) << ", " << reg(right) << "\n";
            return;
        }
        if (token == "&" || token == "|" || token == "^") {
            const char* llvmBit = token == "&" ? "and" : (token == "|" ? "or" : "xor");
            bodies << "  " << result << " = " << llvmBit << " " << operand(left) << ", " << reg(right) << "\n";
            return;
        }
        if (token == "<<" || token == ">>" || token == ">>>") {
            const unsigned w = Program::intBitWidth(left.type);
            const std::string ity = llvmType(left.type);
            if (!(left.type == Type::Int && op.overflowProven)) {
                bodies << "  " << result << ".bad = icmp uge " << ity << " " << reg(right) << ", " << w << "\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_OVERFLOW", op.span) << ")\n";
            }
            const char* llvmShift = token == "<<" ? "shl"
                : ((token == ">>>" || Program::isUnsignedInt(left.type)) ? "lshr" : "ashr");
            bodies << "  " << result << " = " << llvmShift << " " << operand(left) << ", " << reg(right) << "\n";
            return;
        }
        if (token == "+" || token == "-" || token == "*" || token == "/" || token == "%") {
            if (left.type != Type::Int) {
                const unsigned w = Program::intBitWidth(left.type);
                const std::string ity = llvmType(left.type);
                const bool isSigned = Program::isSignedInt(left.type);
                if (token == "+" || token == "-" || token == "*") {
                    if (op.overflowProven) {
                        const char* llvmArith = token == "+" ? "add" : (token == "-" ? "sub" : "mul");
                        bodies << "  " << result << " = " << llvmArith << (isSigned ? " nsw " : " nuw ")
                               << operand(left) << ", " << reg(right) << "\n";
                        return;
                    }
                    const char* intrOp = token == "+" ? "add" : (token == "-" ? "sub" : "mul");
                    const std::string intr = std::string("llvm.") + (isSigned ? "s" : "u") + intrOp + ".with.overflow." + ity;
                    usedDecls.insert(intr);
                    bodies << "  " << result << ".st = call {" << ity << ", i1} @" << intr << "("
                           << operand(left) << ", " << operand(right) << ")\n"
                           << "  " << result << " = extractvalue {" << ity << ", i1} " << result << ".st, 0\n"
                           << "  " << result << ".ov = extractvalue {" << ity << ", i1} " << result << ".st, 1\n"
                           << "  call void @tz.guard(i1 " << result << ".ov, " << diagnostic("R_OVERFLOW", op.span) << ")\n";
                    return;
                }
                bodies << "  " << result << ".z = icmp eq " << ity << " " << reg(right) << ", 0\n"
                       << "  call void @tz.guard(i1 " << result << ".z, " << diagnostic("R_DIV_ZERO", op.span) << ")\n";
                if (isSigned) {
                    bodies << "  " << result << ".mn = icmp eq " << ity << " " << reg(left) << ", " << minSignedLit(w) << "\n"
                           << "  " << result << ".n1 = icmp eq " << ity << " " << reg(right) << ", -1\n"
                           << "  " << result << ".ov = and i1 " << result << ".mn, " << result << ".n1\n"
                           << "  call void @tz.guard(i1 " << result << ".ov, " << diagnostic("R_OVERFLOW", op.span) << ")\n";
                }
                const char* divOp = (token == "/") ? (isSigned ? "sdiv" : "udiv") : (isSigned ? "srem" : "urem");
                bodies << "  " << result << " = " << divOp << " " << operand(left) << ", " << reg(right) << "\n";
                return;
            }
            static const std::map<std::string, std::string> functions{
                {"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"}, {"%", "rem"}};
            if (op.overflowProven && (token == "+" || token == "-" || token == "*")) {
                if (op.width < 64) {
                    const std::string wty = "i" + std::to_string(static_cast<unsigned>(op.width));
                    bodies << "  " << result << ".l = trunc i64 " << reg(left) << " to " << wty << "\n"
                           << "  " << result << ".r = trunc i64 " << reg(right) << " to " << wty << "\n"
                           << "  " << result << ".w = " << functions.at(token) << " nsw " << wty << " "
                           << result << ".l, " << result << ".r\n"
                           << "  " << result << " = sext " << wty << " " << result << ".w to i64\n";
                } else {
                    bodies << "  " << result << " = " << functions.at(token) << " nsw "
                           << operand(left) << ", " << reg(right) << "\n";
                }
                return;
            }
            if (op.divSafe && (token == "/" || token == "%")) {
                if (token == "%" && op.modNoOp) {
                    bodies << "  " << result << " = add i64 " << reg(left) << ", 0\n";
                    return;
                }
                if (token == "%" && op.modSubSteps == 1) {
                    if (op.width < 64) {
                        const std::string wty = "i" + std::to_string(static_cast<unsigned>(op.width));
                        bodies << "  " << result << ".l = trunc i64 " << reg(left) << " to " << wty << "\n"
                               << "  " << result << ".r = trunc i64 " << reg(right) << " to " << wty << "\n"
                               << "  " << result << ".diff = sub " << wty << " " << result << ".l, " << result << ".r\n"
                               << "  " << result << ".ge = icmp uge " << wty << " " << result << ".l, " << result << ".r\n"
                               << "  " << result << ".w = select i1 " << result << ".ge, " << wty << " "
                               << result << ".diff, " << wty << " " << result << ".l\n"
                               << "  " << result << " = zext " << wty << " " << result << ".w to i64\n";
                    } else {
                        bodies << "  " << result << ".diff = sub i64 " << reg(left) << ", " << reg(right) << "\n"
                               << "  " << result << ".ge = icmp uge i64 " << reg(left) << ", " << reg(right) << "\n"
                               << "  " << result << " = select i1 " << result << ".ge, i64 "
                               << result << ".diff, i64 " << reg(left) << "\n";
                    }
                    return;
                }
                if (token == "%" && op.modSubSteps >= 2) {
                    // Branchless arithmetic reduction prevents LLVM SelectOptimize from splitting chained selects into branches.
                    auto emitStep = [&](const std::string& wty, unsigned bits, const std::string& l,
                                        const std::string& r, std::uint8_t steps) {
                        std::string cur = l;
                        for (std::uint8_t si = 0; si < steps; ++si) {
                            const std::string pfx = result + ".s" + std::to_string(si);
                            bodies << "  " << pfx << ".diff = sub " << wty << " " << cur << ", " << r << "\n"
                                   << "  " << pfx << ".mask = ashr " << wty << " " << pfx << ".diff, " << (bits - 1) << "\n"
                                   << "  " << pfx << ".corr = and " << wty << " " << pfx << ".mask, " << r << "\n"
                                   << "  " << pfx << ".next = add " << wty << " " << pfx << ".diff, " << pfx << ".corr\n";
                            cur = pfx + ".next";
                        }
                        return cur;
                    };
                    if (op.width < 64) {
                        const unsigned bits = static_cast<unsigned>(op.width);
                        const std::string wty = "i" + std::to_string(bits);
                        bodies << "  " << result << ".l = trunc i64 " << reg(left) << " to " << wty << "\n"
                               << "  " << result << ".r = trunc i64 " << reg(right) << " to " << wty << "\n";
                        const std::string finalVal = emitStep(wty, bits, result + ".l", result + ".r", op.modSubSteps);
                        bodies << "  " << result << " = zext " << wty << " " << finalVal << " to i64\n";
                    } else {
                        const std::string finalVal = emitStep("i64", 64, reg(left), reg(right), op.modSubSteps);
                        bodies << "  " << result << " = add i64 " << finalVal << ", 0\n";
                    }
                    return;
                }
                if (op.divSafe && op.unsignedDiv && op.width < 64 && op.lhsUpper >= 0) {
                    if (auto rcIt = curConstInts.find(right.id); rcIt != curConstInts.end() && rcIt->second >= 2) {
                        const std::uint64_t d = static_cast<std::uint64_t>(rcIt->second);
                        const std::uint64_t xMax = static_cast<std::uint64_t>(op.lhsUpper);
                        unsigned barWidth = static_cast<unsigned>(op.width);
                        auto bar = findExactBarrett(d, xMax, barWidth);
                        if (!bar && barWidth < 32 && xMax <= 0xFFFFFFFFULL && d <= 0xFFFFFFFFULL) {
                            barWidth = 32;
                            bar = findExactBarrett(d, xMax, 32);
                        }
                        if (bar) {
                            const unsigned wideBits = barWidth * 2;
                            const std::string wty = "i" + std::to_string(barWidth);
                            const std::string wideTy = "i" + std::to_string(wideBits);
                            bodies << "  " << result << ".bl = trunc i64 " << reg(left) << " to " << wty << "\n"
                                   << "  " << result << ".lz = zext " << wty << " " << result << ".bl to " << wideTy << "\n"
                                   << "  " << result << ".bp = mul nuw " << wideTy << " " << result << ".lz, " << bar->first << "\n"
                                   << "  " << result << ".bs = lshr " << wideTy << " " << result << ".bp, " << bar->second << "\n"
                                   << "  " << result << ".q = trunc " << wideTy << " " << result << ".bs to " << wty << "\n";
                            if (token == "/") {
                                bodies << "  " << result << " = zext " << wty << " " << result << ".q to i64\n";
                            } else {
                                bodies << "  " << result << ".qd = mul " << wty << " " << result << ".q, " << d << "\n"
                                       << "  " << result << ".w = sub " << wty << " " << result << ".bl, " << result << ".qd\n"
                                       << "  " << result << " = zext " << wty << " " << result << ".w to i64\n";
                            }
                            return;
                        }
                    }
                }
                const char* llvmOp = (token == "/")
                    ? (op.unsignedDiv ? "udiv" : "sdiv")
                    : (op.unsignedDiv ? "urem" : "srem");
                if (op.width < 64) {
                    const std::string wty = "i" + std::to_string(static_cast<unsigned>(op.width));
                    const char* extOp = op.unsignedDiv ? "zext" : "sext";
                    bodies << "  " << result << ".l = trunc i64 " << reg(left) << " to " << wty << "\n"
                           << "  " << result << ".r = trunc i64 " << reg(right) << " to " << wty << "\n"
                           << "  " << result << ".w = " << llvmOp << " " << wty << " "
                           << result << ".l, " << result << ".r\n"
                           << "  " << result << " = " << extOp << " " << wty << " " << result << ".w to i64\n";
                } else {
                    bodies << "  " << result << " = " << llvmOp << " "
                           << operand(left) << ", " << reg(right) << "\n";
                }
                return;
            }
            bodies << "  " << result << " = call i64 @tz." << functions.at(token) << "("
                   << operand(left) << ", " << operand(right) << ", ";
            if (token == "/" || token == "%") bodies << diagnostic("R_DIV_ZERO", op.span) << ", ";
            bodies << diagnostic("R_OVERFLOW", op.span) << ")\n";
            return;
        }
        const bool isUnsigned = Program::isUnsignedInt(left.type);
        static const std::map<std::string, std::string> sPreds{
            {"==", "eq"}, {"!=", "ne"}, {"<", "slt"}, {"<=", "sle"}, {">", "sgt"}, {">=", "sge"}};
        static const std::map<std::string, std::string> uPreds{
            {"==", "eq"}, {"!=", "ne"}, {"<", "ult"}, {"<=", "ule"}, {">", "ugt"}, {">=", "uge"}};
        const auto& pred = (isUnsigned ? uPreds : sPreds).at(token);
        if (left.type == Type::Int && op.width < 64) {
            const std::string wty = "i" + std::to_string(static_cast<unsigned>(op.width));
            bodies << "  " << result << ".l = trunc i64 " << reg(left) << " to " << wty << "\n"
                   << "  " << result << ".r = trunc i64 " << reg(right) << " to " << wty << "\n"
                   << "  " << result << " = icmp " << pred << " " << wty << " "
                   << result << ".l, " << result << ".r\n";
        } else {
            bodies << "  " << result << " = icmp " << pred << " " << operand(left)
                   << ", " << reg(right) << "\n";
        }
    }
    std::string mutArgPtr(const Operation& op, std::size_t argIdx, const std::string& pfx) {
        const unsigned s = op.mutArgSlots.at(argIdx);
        std::string ptr = slotPtr(s);
        if (argIdx < op.mutArgPaths.size() && !op.mutArgPaths[argIdx].empty()) {
            Type curT = curSlotTypes.at(s);
            for (std::size_t si = 0; si < op.mutArgPaths[argIdx].size(); ++si) {
                const auto& step = op.mutArgPaths[argIdx][si];
                const std::string stepPfx = pfx + ".ms" + std::to_string(si);
                if (step.isIndex) {
                    bodies << "  " << stepPfx << ".arr = load %array, ptr " << ptr << "\n"
                           << "  " << stepPfx << ".idx = load i64, ptr " << slotPtr(step.idxSlot) << "\n";
                    const std::string atFn = (curT == Type::IntArray) ? "@tz.at" : requestAt(curT);
                    bodies << "  " << stepPfx << ".ep = call ptr " << atFn << "(%array " << stepPfx << ".arr, i64 "
                           << stepPfx << ".idx, " << diagnostic("R_BOUNDS", op.span) << ")\n";
                    ptr = stepPfx + ".ep";
                    curT = elemOf(curT);
                } else {
                    const auto& lay = layoutOf(curT);
                    const unsigned phys = lay.variants.at(0).logicalToLlvm.at(step.fieldIdx);
                    const std::string nextPtr = stepPfx + ".mf";
                    bodies << "  " << nextPtr << " = getelementptr inbounds " << llvmType(curT)
                           << ", ptr " << ptr << ", i32 0, i32 " << phys << "\n";
                    ptr = nextPtr;
                    curT = program.declaration(curT)->variants.at(0).fields.at(step.fieldIdx).type;
                }
            }
        }
        return ptr;
    }

    static std::string minSignedLit(unsigned w) {
        switch (w) {
        case 8: return "-128";
        case 16: return "-32768";
        case 32: return "-2147483648";
        default: return "-9223372036854775808";
        }
    }
    static std::string maxSignedLit(unsigned w) {
        switch (w) {
        case 8: return "127";
        case 16: return "32767";
        case 32: return "2147483647";
        default: return "9223372036854775807";
        }
    }
    static std::string doubleHex(double d) {
        const auto bits = std::bit_cast<std::uint64_t>(d);
        static constexpr std::string_view hex = "0123456789ABCDEF";
        std::string out = "0x";
        out.reserve(18);
        for (int shift = 60; shift >= 0; shift -= 4) {
            out.push_back(hex[(bits >> shift) & 15u]);
        }
        return out;
    }
    void emitPackValueToTmp(Type t, const std::string& valReg, const std::string& basePtr,
                            std::size_t off, std::size_t& seq, const std::string& pfx) {
        if (Program::isNumericType(t) || program.isAnyPtr(t) || program.isSimd(t)) {
            std::string dstPtr = basePtr;
            if (off > 0) {
                dstPtr = pfx + ".dp" + std::to_string(seq++);
                bodies << "  " << dstPtr << " = getelementptr inbounds i8, ptr " << basePtr << ", i64 " << off << "\n";
            }
            bodies << "  store " << llvmType(t) << " " << valReg << ", ptr " << dstPtr << ", align 1\n";
            return;
        }
        const auto* decl = program.declaration(t);
        const auto& lay = layoutOf(t);
        const auto& fields = decl->variants.at(0).fields;
        const auto& vl = lay.variants.at(0);
        const std::string tLlvm = llvmType(t);
        for (std::size_t phys = 0; phys < vl.physOrder.size(); ++phys) {
            const unsigned logIdx = vl.physOrder[phys];
            const Type ft = fields[logIdx].type;
            const std::string fReg = pfx + ".pk" + std::to_string(seq++);
            bodies << "  " << fReg << " = extractvalue " << tLlvm << " " << valReg << ", " << vl.logicalToLlvm[logIdx] << "\n";
            emitPackValueToTmp(ft, fReg, basePtr, off + vl.physOffsets[phys], seq, pfx);
        }
    }
    void emitUnpackValueFromTmp(Type t, const std::string& outReg, const std::string& basePtr,
                                std::size_t off, std::size_t& seq, const std::string& pfx) {
        if (Program::isNumericType(t) || program.isAnyPtr(t) || program.isSimd(t)) {
            std::string srcPtr = basePtr;
            if (off > 0) {
                srcPtr = pfx + ".sp" + std::to_string(seq++);
                bodies << "  " << srcPtr << " = getelementptr inbounds i8, ptr " << basePtr << ", i64 " << off << "\n";
            }
            bodies << "  " << outReg << " = load " << llvmType(t) << ", ptr " << srcPtr << ", align 1\n";
            return;
        }
        const auto* decl = program.declaration(t);
        const auto& lay = layoutOf(t);
        const auto& fields = decl->variants.at(0).fields;
        const auto& vl = lay.variants.at(0);
        const std::string tLlvm = llvmType(t);
        if (fields.empty()) {
            bodies << "  " << outReg << " = freeze " << tLlvm << " zeroinitializer\n";
            return;
        }
        std::string acc = "zeroinitializer";
        for (std::size_t phys = 0; phys < vl.physOrder.size(); ++phys) {
            const unsigned logIdx = vl.physOrder[phys];
            const Type ft = fields[logIdx].type;
            const std::string fReg = pfx + ".uv" + std::to_string(seq++);
            emitUnpackValueFromTmp(ft, fReg, basePtr, off + vl.physOffsets[phys], seq, pfx);
            const std::string nextAcc = (phys + 1 == vl.physOrder.size()) ? outReg : (pfx + ".ua" + std::to_string(seq++));
            bodies << "  " << nextAcc << " = insertvalue " << tLlvm << " " << acc << ", "
                   << llvmType(ft) << " " << fReg << ", " << vl.logicalToLlvm[logIdx] << "\n";
            acc = nextAcc;
        }
    }

    void builtin(const Operation& op) {
        const auto result = reg(op.result);
        const auto& name = op.data;
        if (name.starts_with("digitSumLut_")) {
            const auto pos1 = std::string_view("digitSumLut_").size();
            const auto sep = name.find('_', pos1);
            const std::int64_t k = std::stoll(name.substr(pos1, sep - pos1));
            const std::int64_t b = std::stoll(name.substr(sep + 1));
            usedDigitSumLuts.emplace(k, b);
            const std::string sym = "@tz.dsum." + std::to_string(k) + "." + std::to_string(b);
            const std::string arrTy = "[" + std::to_string(b) + " x i8]";
            bodies << "  " << result << ".ptr = getelementptr inbounds " << arrTy << ", ptr " << sym
                   << ", i64 0, i64 " << reg(op.inputs.at(0)) << "\n"
                   << "  " << result << ".raw = load i8, ptr " << result << ".ptr\n"
                   << "  " << result << " = zext i8 " << result << ".raw to i64\n";
            return;
        }
        if (name == "castNum") {
            const Type fromT = op.inputs.at(0).type;
            const Type toT = op.result.type;
            const std::string fromLlvm = llvmType(fromT);
            const std::string toLlvm = llvmType(toT);
            const std::string x = reg(op.inputs.at(0));
            if (Program::isFloatType(fromT)) {
                if (Program::isFloatType(toT)) {
                    if (fromT == toT) {
                        bodies << "  " << result << " = fadd " << toLlvm << " " << x << ", 0.0\n";
                    } else if (toT == Type::Float) {
                        bodies << "  " << result << " = fpext float " << x << " to double\n";
                    } else {
                        bodies << "  " << result << " = fptrunc double " << x << " to float\n";
                    }
                    return;
                }
                std::string dReg = x;
                if (fromT == Type::F32) {
                    dReg = result + ".d";
                    bodies << "  " << dReg << " = fpext float " << x << " to double\n";
                }
                const unsigned w = Program::intBitWidth(toT);
                if (Program::isSignedInt(toT)) {
                    double hi = 0.0, lo = 0.0;
                    const char* loPred = "ult";
                    if (w == 64) {
                        hi = 9223372036854775808.0;
                        lo = -9223372036854775808.0;
                        loPred = "ult";
                    } else {
                        const std::int64_t mn = -(1LL << (w - 1));
                        const std::int64_t mx = static_cast<std::int64_t>((1ULL << (w - 1)) - 1ULL);
                        hi = static_cast<double>(mx) + 1.0;
                        lo = static_cast<double>(mn) - 1.0;
                        loPred = "ule";
                    }
                    bodies << "  " << result << ".hi = fcmp uge double " << dReg << ", " << doubleHex(hi) << "\n"
                           << "  " << result << ".lo = fcmp " << loPred << " double " << dReg << ", " << doubleHex(lo) << "\n"
                           << "  " << result << ".bad = or i1 " << result << ".hi, " << result << ".lo\n"
                           << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_OVERFLOW", op.span) << ")\n"
                           << "  " << result << " = fptosi double " << dReg << " to " << toLlvm << "\n";
                } else {
                    const double hi = (w == 64)
                        ? 18446744073709551616.0
                        : (static_cast<double>((1ULL << w) - 1ULL) + 1.0);
                    bodies << "  " << result << ".lo = fcmp ult double " << dReg << ", 0x0000000000000000\n"
                           << "  " << result << ".hi = fcmp uge double " << dReg << ", " << doubleHex(hi) << "\n"
                           << "  " << result << ".bad = or i1 " << result << ".hi, " << result << ".lo\n"
                           << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_OVERFLOW", op.span) << ")\n"
                           << "  " << result << " = fptoui double " << dReg << " to " << toLlvm << "\n";
                }
                return;
            }
            if (Program::isFloatType(toT)) {
                const char* conv = Program::isSignedInt(fromT) ? "sitofp" : "uitofp";
                bodies << "  " << result << " = " << conv << " " << fromLlvm << " " << x << " to " << toLlvm << "\n";
                return;
            }
            const unsigned fromW = Program::intBitWidth(fromT);
            const unsigned toW = Program::intBitWidth(toT);
            if (toW == fromW) {
                bodies << "  " << result << " = add " << toLlvm << " " << x << ", 0\n";
            } else if (toW < fromW) {
                bodies << "  " << result << " = trunc " << fromLlvm << " " << x << " to " << toLlvm << "\n";
            } else {
                const char* extOp = Program::isSignedInt(fromT) ? "sext" : "zext";
                bodies << "  " << result << " = " << extOp << " " << fromLlvm << " " << x << " to " << toLlvm << "\n";
            }
            return;
        }
        if (name == "bitcast") {
            const Type fromT = op.inputs.at(0).type;
            const Type toT = op.result.type;
            const std::string fromLlvm = llvmType(fromT);
            const std::string toLlvm = llvmType(toT);
            const std::size_t sz = sizeAlignOf(toT).first;
            if (sz == 0) {
                bodies << "  " << result << " = freeze " << toLlvm << " zeroinitializer\n";
                return;
            }
            if (Program::isNumericType(fromT) && Program::isNumericType(toT)) {
                if (fromLlvm == toLlvm) {
                    bodies << "  " << result << " = add " << toLlvm << " " << reg(op.inputs.at(0)) << ", 0\n";
                } else {
                    bodies << "  " << result << " = bitcast " << operand(op.inputs.at(0)) << " to " << toLlvm << "\n";
                }
                return;
            }
            const std::string tmp = "%bc.tmp." + std::to_string(op.result.id);
            bodies << "  call void @llvm.memset.p0.i64(ptr " << tmp << ", i8 0, i64 " << sz << ", i1 false)\n";
            std::size_t seq = 0;
            emitPackValueToTmp(fromT, reg(op.inputs.at(0)), tmp, 0, seq, result);
            emitUnpackValueFromTmp(toT, result, tmp, 0, seq, result);
            return;
        }
        if (name == "intFromFloat") {
            const auto x = reg(op.inputs.at(0));
            bodies << "  " << result << ".hi = fcmp uge double " << x << ", 0x43E0000000000000\n"
                   << "  " << result << ".lo = fcmp ult double " << x << ", 0xC3E0000000000000\n"
                   << "  " << result << ".bad = or i1 " << result << ".hi, " << result << ".lo\n"
                   << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_OVERFLOW", op.span) << ")\n"
                   << "  " << result << " = fptosi double " << x << " to i64\n";
            return;
        }
        if (name == "floatFromInt") {
            bodies << "  " << result << " = sitofp " << operand(op.inputs.at(0)) << " to double\n";
            return;
        }
        if (name == "enumInt") {
            const Type et = static_cast<Type>(op.slot);
            usedEnumValTables.insert(et);
            const auto n = program.declaration(et)->variants.size();
            bodies << "  " << result << ".ptr = getelementptr inbounds [" << n << " x i64], ptr @tz.enumval."
                   << static_cast<int>(et) << ", i64 0, i64 " << reg(op.inputs.at(0)) << "\n"
                   << "  " << result << " = load i64, ptr " << result << ".ptr\n";
            return;
        }
        if (name == "wrappingAdd" || name == "wrappingSub" || name == "wrappingMul") {
            const char* llvmOp = name == "wrappingAdd" ? "add" : (name == "wrappingSub" ? "sub" : "mul");
            bodies << "  " << result << " = " << llvmOp << " " << operand(op.inputs.at(0)) << ", "
                   << reg(op.inputs.at(1)) << "\n";
            return;
        }
        if (name == "saturatingAdd" || name == "saturatingSub") {
            const std::string ity = llvmType(op.result.type);
            const bool isSigned = Program::isSignedInt(op.result.type);
            const std::string intr = std::string("llvm.") + (isSigned ? "s" : "u") +
                (name == "saturatingAdd" ? "add.sat." : "sub.sat.") + ity;
            usedDecls.insert(intr);
            bodies << "  " << result << " = call " << ity << " @" << intr << "(" << operand(op.inputs.at(0)) << ", "
                   << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "saturatingMul") {
            const Type ty = op.result.type;
            const std::string ity = llvmType(ty);
            const auto a = reg(op.inputs.at(0));
            const auto b = reg(op.inputs.at(1));
            if (Program::isSignedInt(ty)) {
                const unsigned w = Program::intBitWidth(ty);
                const std::string intr = "llvm.smul.with.overflow." + ity;
                if (w < 64) usedDecls.insert(intr);
                bodies << "  " << result << ".st = call {" << ity << ", i1} @" << intr << "(" << ity << " " << a << ", " << ity << " " << b << ")\n"
                       << "  " << result << ".val = extractvalue {" << ity << ", i1} " << result << ".st, 0\n"
                       << "  " << result << ".ov = extractvalue {" << ity << ", i1} " << result << ".st, 1\n"
                       << "  " << result << ".x = xor " << ity << " " << a << ", " << b << "\n"
                       << "  " << result << ".neg = icmp slt " << ity << " " << result << ".x, 0\n"
                       << "  " << result << ".sat = select i1 " << result << ".neg, " << ity << " " << minSignedLit(w) << ", " << ity << " " << maxSignedLit(w) << "\n"
                       << "  " << result << " = select i1 " << result << ".ov, " << ity << " " << result << ".sat, " << ity << " " << result << ".val\n";
            } else {
                const std::string intr = "llvm.umul.with.overflow." + ity;
                usedDecls.insert(intr);
                bodies << "  " << result << ".st = call {" << ity << ", i1} @" << intr << "(" << ity << " " << a << ", " << ity << " " << b << ")\n"
                       << "  " << result << ".val = extractvalue {" << ity << ", i1} " << result << ".st, 0\n"
                       << "  " << result << ".ov = extractvalue {" << ity << ", i1} " << result << ".st, 1\n"
                       << "  " << result << " = select i1 " << result << ".ov, " << ity << " -1, " << ity << " " << result << ".val\n";
            }
            return;
        }
        if (name == "abs") {
            const Type ty = op.result.type;
            if (Program::isFloatType(ty)) {
                const std::string fty = llvmType(ty);
                const std::string intr = std::string("llvm.fabs.") + (ty == Type::F32 ? "f32" : "f64");
                usedDecls.insert(intr);
                bodies << "  " << result << " = call " << fty << " @" << intr << "(" << operand(op.inputs.at(0)) << ")\n";
            } else if (!Program::isSignedInt(ty)) {
                bodies << "  " << result << " = add " << operand(op.inputs.at(0)) << ", 0\n";
            } else {
                const unsigned w = Program::intBitWidth(ty);
                const std::string ity = llvmType(ty);
                const std::string intr = "llvm.abs." + ity;
                usedDecls.insert(intr);
                const auto x = reg(op.inputs.at(0));
                bodies << "  " << result << ".bad = icmp eq " << ity << " " << x << ", " << minSignedLit(w) << "\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_OVERFLOW", op.span) << ")\n"
                       << "  " << result << " = call " << ity << " @" << intr << "(" << ity << " " << x << ", i1 true)\n";
            }
            return;
        }
        if (name == "min" || name == "max") {
            const Type ty = op.result.type;
            if (Program::isFloatType(ty)) {
                const std::string fty = llvmType(ty);
                const std::string sfx = (ty == Type::F32) ? ".f32" : ".f64";
                const std::string intr = std::string("llvm.") + (name == "min" ? "minnum" : "maxnum") + sfx;
                usedDecls.insert(intr);
                bodies << "  " << result << " = call " << fty << " @" << intr << "(" << operand(op.inputs.at(0))
                       << ", " << operand(op.inputs.at(1)) << ")\n";
            } else {
                const bool isSigned = Program::isSignedInt(ty);
                const std::string ity = llvmType(ty);
                const std::string intr = std::string("llvm.") + (isSigned ? "s" : "u") +
                    (name == "min" ? "min." : "max.") + ity;
                if (!isSigned || ity == "i8") usedDecls.insert(intr);
                bodies << "  " << result << " = call " << ity << " @" << intr << "(" << operand(op.inputs.at(0))
                       << ", " << operand(op.inputs.at(1)) << ")\n";
            }
            return;
        }
        if (name == "sqrt" || name == "floor" || name == "ceil" || name == "round" || name == "sin" || name == "cos") {
            const std::string fty = llvmType(op.result.type);
            const std::string intr = "llvm." + name + (op.result.type == Type::F32 ? ".f32" : ".f64");
            usedDecls.insert(intr);
            bodies << "  " << result << " = call " << fty << " @" << intr << "(" << operand(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "pow") {
            const std::string fty = llvmType(op.result.type);
            const std::string intr = op.result.type == Type::F32 ? "llvm.pow.f32" : "llvm.pow.f64";
            usedDecls.insert(intr);
            bodies << "  " << result << " = call " << fty << " @" << intr << "(" << operand(op.inputs.at(0))
                   << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "isNaN") {
            const auto x = reg(op.inputs.at(0));
            const std::string fty = llvmType(op.inputs.at(0).type);
            bodies << "  " << result << " = fcmp uno " << fty << " " << x << ", " << x << "\n";
            return;
        }
        if (name == "isInf") {
            usedDecls.insert("llvm.fabs.f64");
            std::string dArg = operand(op.inputs.at(0));
            if (op.inputs.at(0).type == Type::F32) {
                bodies << "  " << result << ".d = fpext float " << reg(op.inputs.at(0)) << " to double\n";
                dArg = "double " + result + ".d";
            }
            bodies << "  " << result << ".abs = call double @llvm.fabs.f64(" << dArg << ")\n"
                   << "  " << result << " = fcmp oeq double " << result << ".abs, 0x7FF0000000000000\n";
            return;
        }
        if (name == "writeByte") {
            bodies << "  " << result << ".m = and i64 " << reg(op.inputs.at(0)) << ", 255\n"
                   << "  " << result << ".c = trunc i64 " << result << ".m to i32\n"
                   << "  call i32 @putchar(i32 " << result << ".c)\n";
            return;
        }
        if (name == "write" || name == "ewrite" || name == "eprint") {
            const char* errFlag = (name == "write") ? "false" : "true";
            bodies << "  call void @tz.write(" << operand(op.inputs.at(0)) << ", i1 " << errFlag << ")\n";
            if (name == "eprint")
                bodies << "  call void @tz.write(" << literal("\n") << ", i1 true)\n";
            return;
        }
        if (name == "flush") {
            usedDecls.insert("fflush");
            bodies << "  " << result << ".st = call ptr @tz.stream(i1 false)\n"
                   << "  call i32 @fflush(ptr " << result << ".st)\n";
            return;
        }
        if (name == "argsCount") {
            usesSysArgs = true;
            usedDecls.insert("tz_sys_args_count");
            bodies << "  " << result << " = call i64 @tz_sys_args_count()\n";
            return;
        }
        auto extractText = [&](const Value& tv, const std::string& pfx, const std::string& tmpSlot) {
            bodies << "  store %text " << reg(tv) << ", ptr " << tmpSlot << "\n"
                   << "  " << pfx << ".p = call ptr @tz.textPtr(ptr " << tmpSlot << ")\n"
                   << "  " << pfx << ".hdr = extractvalue %text " << reg(tv) << ", 0\n"
                   << "  " << pfx << ".l32 = and i32 " << pfx << ".hdr, 2147483647\n"
                   << "  " << pfx << ".n = zext i32 " << pfx << ".l32 to i64\n";
        };
        if (name == "argAt") {
            usesSysArgs = true;
            usedDecls.insert("tz_sys_arg_at");
            bodies << "  call void @tz_sys_arg_at(" << operand(op.inputs.at(0)) << ", ptr %sys.slice.tmp)\n"
                   << "  " << result << ".pp = getelementptr inbounds { ptr, i64 }, ptr %sys.slice.tmp, i32 0, i32 0\n"
                   << "  " << result << ".np = getelementptr inbounds { ptr, i64 }, ptr %sys.slice.tmp, i32 0, i32 1\n"
                   << "  " << result << ".p = load ptr, ptr " << result << ".pp\n"
                   << "  " << result << ".n = load i64, ptr " << result << ".np\n"
                   << "  " << result << " = call %text @tz.textFromSlice(ptr " << result << ".p, i64 "
                   << result << ".n, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "clockNs") {
            usedDecls.insert("tz_sys_clock_ns");
            bodies << "  " << result << " = call i64 @tz_sys_clock_ns()\n";
            return;
        }
        if (name == "sleepMs") {
            usedDecls.insert("tz_sys_sleep_ms");
            bodies << "  call void @tz_sys_sleep_ms(" << operand(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "readFile" || name == "envGet" || name == "execCapture") {
            const std::string cfn = name == "readFile" ? "tz_sys_read_file"
                                  : (name == "envGet" ? "tz_sys_env_get" : "tz_sys_exec_capture");
            usedDecls.insert(cfn);
            extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
            bodies << "  call void @" << cfn << "(ptr " << result << ".a0.p, i64 " << result
                   << ".a0.n, ptr %sys.slice.tmp)\n"
                   << "  " << result << ".pp = getelementptr inbounds { ptr, i64 }, ptr %sys.slice.tmp, i32 0, i32 0\n"
                   << "  " << result << ".np = getelementptr inbounds { ptr, i64 }, ptr %sys.slice.tmp, i32 0, i32 1\n"
                   << "  " << result << ".p = load ptr, ptr " << result << ".pp\n"
                   << "  " << result << ".n = load i64, ptr " << result << ".np\n"
                   << "  " << result << " = call %text @tz.textFromOwned(ptr " << result << ".p, i64 "
                   << result << ".n, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "execStatus") {
            usedDecls.insert("tz_sys_exec_status");
            extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
            bodies << "  " << result << " = call i64 @tz_sys_exec_status(ptr " << result << ".a0.p, i64 "
                   << result << ".a0.n)\n";
            return;
        }
        if (name == "fileExists" || name == "removeFile" || name == "makeDir") {
            const std::string cfn = name == "fileExists" ? "tz_sys_file_exists"
                                  : (name == "removeFile" ? "tz_sys_remove_file" : "tz_sys_make_dir");
            usedDecls.insert(cfn);
            extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
            bodies << "  " << result << ".r = call i8 @" << cfn << "(ptr " << result << ".a0.p, i64 "
                   << result << ".a0.n)\n"
                   << "  " << result << " = icmp ne i8 " << result << ".r, 0\n";
            return;
        }
        if (name == "writeFile" || name == "appendFile") {
            const std::string cfn = name == "writeFile" ? "tz_sys_write_file" : "tz_sys_append_file";
            usedDecls.insert(cfn);
            extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
            extractText(op.inputs.at(1), result + ".a1", "%text.tmp1");
            bodies << "  " << result << ".r = call i8 @" << cfn << "(ptr " << result << ".a0.p, i64 "
                   << result << ".a0.n, ptr " << result << ".a1.p, i64 " << result << ".a1.n)\n"
                   << "  " << result << " = icmp ne i8 " << result << ".r, 0\n";
            return;
        }
        if (name == "cap") {
            const Type cT = op.inputs.at(0).type;
            if (program.isMap(cT) || program.isSet(cT)) {
                bodies << "  " << result << ".b32 = extractvalue " << operand(op.inputs.at(0)) << ", 2\n"
                       << "  " << result << ".b64 = zext i32 " << result << ".b32 to i64\n"
                       << "  " << result << ".mul = mul i64 " << result << ".b64, 7\n"
                       << "  " << result << " = lshr i64 " << result << ".mul, 3\n";
                return;
            }
            bodies << "  " << result << ".c32 = extractvalue " << operand(op.inputs.at(0)) << ", 2\n"
                   << "  " << result << " = zext i32 " << result << ".c32 to i64\n";
            return;
        }
        if (name == "push") {
            const Type arrT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  call void " << requestPush(arrT) << "(ptr " << sp
                   << ", " << operand(op.inputs.at(1)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "pop") {
            const Type arrT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << requestPop(arrT)
                   << "(ptr " << sp << ", " << diagnostic("R_BOUNDS", op.span) << ")\n";
            return;
        }
        if (name == "clear") {
            const Type colT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            if (program.isMap(colT) || program.isSet(colT)) {
                bodies << "  call void " << requestTblClear(colT) << "(ptr " << sp << ")\n";
                return;
            }
            if (program.isPool(colT)) {
                bodies << "  call void " << requestPoolClear(colT) << "(ptr " << sp << ")\n";
                return;
            }
            const Type elemT = elemOf(colT);
            if (needsDrop(elemT)) {
                bodies << "  call void " << requestClear(colT) << "(ptr " << sp << ")\n";
            } else {
                bodies << "  " << result << ".lp = getelementptr inbounds %array, ptr " << sp << ", i32 0, i32 1\n"
                       << "  store i32 0, ptr " << result << ".lp\n";
            }
            return;
        }
        if (name == "reserve") {
            const Type colT = op.inputs.at(0).type;
            const std::string minCap = reg(op.inputs.at(1));
            const std::string sp = mutArgPtr(op, 0, result);
            if (program.isMap(colT) || program.isSet(colT)) {
                bodies << "  call void " << requestTblReserve(colT) << "(ptr " << sp
                       << ", i64 " << minCap << ", " << diagnostic("R_ARRAY_SIZE", op.span) << ", "
                       << diagnostic("R_ALLOC", op.span) << ")\n";
                return;
            }
            if (program.isPool(colT)) {
                bodies << "  call void " << requestPoolReserve(colT) << "(ptr " << sp
                       << ", i64 " << minCap << ", " << diagnostic("R_ARRAY_SIZE", op.span) << ", "
                       << diagnostic("R_ALLOC", op.span) << ")\n";
                return;
            }
            const Type elemT = elemOf(colT);
            bodies << "  " << result << ".neg = icmp slt i64 " << minCap << ", 0\n"
                   << "  call void @tz.guard(i1 " << result << ".neg, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n"
                   << "  call void @tz.growBuf(ptr " << sp
                   << ", i64 " << strideExpr(elemT) << ", i64 " << minCap << ", "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "poolNew") {
            bodies << "  " << result << " = insertvalue %pool zeroinitializer, ptr null, 0\n";
            return;
        }
        if (name == "poolWithCap") {
            bodies << "  " << result << " = call %pool " << requestPoolWithCap(op.result.type) << "("
                   << operand(op.inputs.at(0)) << ", " << diagnostic("R_ARRAY_SIZE", op.span) << ", "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "poolAdd") {
            const Type poolT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << " = call %handle " << requestPoolAdd(poolT) << "(ptr " << sp
                   << ", " << operand(op.inputs.at(1)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "poolGet") {
            const Type poolT = op.inputs.at(0).type;
            const std::string fn = op.boundsProven
                ? requestPoolGetMono(poolT, op.result.type)
                : requestPoolGet(poolT, op.result.type);
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << fn
                   << "(" << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ", "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "poolHas") {
            const Type poolT = op.inputs.at(0).type;
            const std::string fn = op.boundsProven
                ? requestPoolHasMono(poolT)
                : requestPoolHas(poolT);
            bodies << "  " << result << " = call i1 " << fn << "("
                   << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "poolSet") {
            const Type poolT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << " = call i1 " << requestPoolSet(poolT) << "(ptr " << sp
                   << ", " << operand(op.inputs.at(1)) << ", " << operand(op.inputs.at(2)) << ")\n";
            return;
        }
        if (name == "poolRemove") {
            const Type poolT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " "
                   << requestPoolRemove(poolT, op.result.type) << "(ptr " << sp << ", "
                   << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "poolHandles") {
            const Type poolT = op.inputs.at(0).type;
            bodies << "  " << result << " = call %array " << requestPoolHandles(poolT) << "("
                   << operand(op.inputs.at(0)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sharedNew") {
            bodies << "  " << result << " = call ptr " << requestRcNew(op.result.type) << "("
                   << operand(op.inputs.at(0)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sharedReuse") {
            bodies << "  " << result << " = call ptr " << requestRcReuse(op.result.type) << "("
                   << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ", "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sharedGet") {
            const Type shT = op.inputs.at(0).type;
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << requestRcGet(shT)
                   << "(" << operand(op.inputs.at(0)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sharedSet") {
            const Type shT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  call void " << requestRcSet(shT) << "(ptr " << sp << ", "
                   << operand(op.inputs.at(1)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sharedUpdate") {
            const Type shT = op.inputs.at(0).type;
            if (checkDepth)
                bodies << "  call void @tz.guard(i1 %atLimit, " << diagnostic("R_DEPTH", op.span) << ")\n";
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  call void " << requestRcUpdate(shT) << "(i32 %childDepth, ptr " << sp << ", "
                   << operand(op.inputs.at(1)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "mapNew" || name == "setNew") {
            bodies << "  " << result << " = insertvalue %table zeroinitializer, ptr null, 0\n";
            return;
        }
        if (name == "mapWithCap" || name == "setWithCap") {
            bodies << "  " << result << " = call %table " << requestTblWithCap(op.result.type) << "("
                   << operand(op.inputs.at(0)) << ", " << diagnostic("R_ARRAY_SIZE", op.span) << ", "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "mapSecure" || name == "setSecure") {
            bodies << "  " << result << " = call %table " << requestTblSecure() << "()\n";
            return;
        }
        if (name == "mapSet") {
            const Type tblT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  call void " << requestMapSet(tblT) << "(ptr " << sp
                   << ", " << operand(op.inputs.at(1)) << ", " << operand(op.inputs.at(2))
                   << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "mapGet") {
            const Type tblT = op.inputs.at(0).type;
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " "
                   << requestMapGet(tblT, op.result.type) << "(" << operand(op.inputs.at(0)) << ", "
                   << operand(op.inputs.at(1)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "mapHas") {
            const Type tblT = op.inputs.at(0).type;
            bodies << "  " << result << " = call i1 " << requestMapHas(tblT) << "("
                   << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "mapDel") {
            const Type tblT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << " = call i1 " << requestMapDel(tblT) << "(ptr "
                   << sp << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "mapKeys") {
            const Type tblT = op.inputs.at(0).type;
            bodies << "  " << result << " = call %array " << requestMapKeys(tblT) << "("
                   << operand(op.inputs.at(0)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "mapValues") {
            const Type tblT = op.inputs.at(0).type;
            bodies << "  " << result << " = call %array " << requestMapVals(tblT) << "("
                   << operand(op.inputs.at(0)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "setAdd") {
            const Type tblT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << " = call i1 " << requestSetAdd(tblT) << "(ptr "
                   << sp << ", " << operand(op.inputs.at(1))
                   << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "setHas") {
            const Type tblT = op.inputs.at(0).type;
            bodies << "  " << result << " = call i1 " << requestSetHas(tblT) << "("
                   << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "setDel") {
            const Type tblT = op.inputs.at(0).type;
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << " = call i1 " << requestSetDel(tblT) << "(ptr "
                   << sp << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "setItems") {
            const Type tblT = op.inputs.at(0).type;
            bodies << "  " << result << " = call %array " << requestSetItems(tblT) << "("
                   << operand(op.inputs.at(0)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sort" || name == "sortStable") {
            const Type elemT = elemOf(op.inputs.at(0).type);
            const std::string sp = mutArgPtr(op, 0, result);
            const char* sfx = [&]() -> const char* {
                switch (elemT) {
                case Type::Int: return "i64";
                case Type::I8: return "i8";
                case Type::I16: return "i16";
                case Type::I32: return "i32";
                case Type::U8: return "u8";
                case Type::U16: return "u16";
                case Type::U32: return "u32";
                case Type::U64: return "u64";
                case Type::F32: return "f32";
                case Type::Float: return "f64";
                case Type::Str: return "str";
                default: return "bool";
                }
            }();
            const std::string cfn = std::string(name == "sort" ? "tz_col_sort_" : "tz_col_sort_stable_") + sfx;
            usedDecls.insert(cfn);
            bodies << "  " << result << ".arr = load %array, ptr " << sp << "\n"
                   << "  " << result << ".p = extractvalue %array " << result << ".arr, 0\n"
                   << "  " << result << ".n32 = extractvalue %array " << result << ".arr, 1\n"
                   << "  " << result << ".n = zext i32 " << result << ".n32 to i64\n";
            if (name == "sort") {
                bodies << "  call void @" << cfn << "(ptr " << result << ".p, i64 " << result << ".n)\n";
            } else {
                bodies << "  " << result << ".ok = call i8 @" << cfn << "(ptr " << result << ".p, i64 " << result << ".n)\n"
                       << "  " << result << ".bad = icmp eq i8 " << result << ".ok, 0\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_ALLOC", op.span) << ")\n";
            }
            return;
        }
        if (name == "strFromInt") {
            const Type inT = op.inputs.at(0).type;
            if (inT == Type::U64) {
                usedDecls.insert("tz_sys_format_u64");
                bodies << "  " << result << ".un = call i64 @tz_sys_format_u64(" << operand(op.inputs.at(0))
                       << ", ptr %float.buf, i64 64)\n"
                       << "  " << result << " = call %text @tz.textCopySlice(ptr %float.buf, i64 " << result << ".un, "
                       << diagnostic("R_ALLOC", op.span) << ")\n";
            } else if (inT == Type::Int) {
                bodies << "  " << result << " = call %text @tz.intToText(" << operand(op.inputs.at(0))
                       << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            } else {
                const char* extOp = Program::isSignedInt(inT) ? "sext" : "zext";
                bodies << "  " << result << ".ext = " << extOp << " " << operand(op.inputs.at(0)) << " to i64\n"
                       << "  " << result << " = call %text @tz.intToText(i64 " << result << ".ext, "
                       << diagnostic("R_ALLOC", op.span) << ")\n";
            }
            return;
        }
        if (name == "strFromFloat") {
            usedDecls.insert("tz_sys_format_float");
            std::string dArg = operand(op.inputs.at(0));
            if (op.inputs.at(0).type == Type::F32) {
                bodies << "  " << result << ".fd = fpext float " << reg(op.inputs.at(0)) << " to double\n";
                dArg = "double " + result + ".fd";
            }
            bodies << "  " << result << ".fn = call i64 @tz_sys_format_float(" << dArg
                   << ", ptr %float.buf, i64 64)\n"
                   << "  " << result << " = call %text @tz.textCopySlice(ptr %float.buf, i64 " << result << ".fn, "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "strFromBool") {
            bodies << "  " << result << " = select " << operand(op.inputs.at(0))
                   << ", " << literal("true") << ", " << literal("false") << "\n";
            return;
        }
        if (name == "strFromByte") {
            bodies << "  " << result << " = call %text @tz.byteToText(" << operand(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "parseInt") {
            bodies << "  " << result << " = call i64 @tz.parseInt(" << operand(op.inputs.at(0))
                   << ", " << diagnostic("R_PARSE", op.span) << ")\n";
            return;
        }
        if (name == "tryParseInt") {
            bodies << "  " << result << " = call i64 @tz.tryParseInt(" << operand(op.inputs.at(0))
                   << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "parseFloat" || name == "tryParseFloat") {
            usedDecls.insert("tz_sys_parse_float");
            extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
            bodies << "  store double 0.0, ptr %float.out\n"
                   << "  " << result << ".ok8 = call i8 @tz_sys_parse_float(ptr " << result << ".a0.p, i64 "
                   << result << ".a0.n, ptr %float.out)\n";
            if (name == "parseFloat") {
                bodies << "  " << result << ".bad = icmp eq i8 " << result << ".ok8, 0\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_PARSE", op.span) << ")\n"
                       << "  " << result << " = load double, ptr %float.out\n";
            } else {
                bodies << "  " << result << ".ok = icmp ne i8 " << result << ".ok8, 0\n"
                       << "  " << result << ".val = load double, ptr %float.out\n"
                       << "  " << result << " = select i1 " << result << ".ok, double " << result
                       << ".val, double " << reg(op.inputs.at(1)) << "\n";
            }
            return;
        }
        if (name == "strContains" || name == "strStartsWith" || name == "strEndsWith" || name == "strIndexOf") {
            const std::string cfn = name == "strContains" ? "@tz.strContains"
                                  : (name == "strStartsWith" ? "@tz.strStartsWith"
                                  : (name == "strEndsWith" ? "@tz.strEndsWith" : "@tz.strIndexOf"));
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << cfn << "("
                   << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "sbNew") {
            bodies << "  " << result << " = insertvalue %array zeroinitializer, ptr null, 0\n";
            return;
        }
        if (name == "sbWithCap") {
            bodies << "  " << result << " = call %array @tz.sbWithCap(" << operand(op.inputs.at(0)) << ", "
                   << diagnostic("R_ARRAY_SIZE", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sbPush") {
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  call void @tz.sbPush(ptr " << sp << ", "
                   << operand(op.inputs.at(1)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sbPushByte") {
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  call void @tz.sbPushByte(ptr " << sp << ", "
                   << operand(op.inputs.at(1)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sbPushInt") {
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  call void @tz.sbPushInt(ptr " << sp << ", "
                   << operand(op.inputs.at(1)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sbPushFloat") {
            usedDecls.insert("tz_sys_format_float");
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << ".fn = call i64 @tz_sys_format_float(" << operand(op.inputs.at(1))
                   << ", ptr %float.buf, i64 64)\n"
                   << "  call void @tz.sbPushRaw(ptr " << sp
                   << ", ptr %float.buf, i64 " << result << ".fn, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sbClear") {
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << ".lp = getelementptr inbounds %array, ptr " << sp
                   << ", i32 0, i32 1\n"
                   << "  store i32 0, ptr " << result << ".lp\n";
            return;
        }
        if (name == "sbStr") {
            bodies << "  " << result << " = call %text @tz.sbStr(" << operand(op.inputs.at(0))
                   << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "sbTake") {
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << " = call %text @tz.sbTake(ptr " << sp << ")\n";
            return;
        }
        if (name == "fileOpen") {
            extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
            extractText(op.inputs.at(1), result + ".a1", "%text.tmp1");
            const std::string hfn = requestSysHelper(SysOpKind::FileOpen, op.result.type);
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".a0.p, i64 " << result << ".a0.n, ptr "
                   << result << ".a1.p, i64 " << result << ".a1.n)\n";
            return;
        }
        if (name == "tryReadFile") {
            extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
            const std::string hfn = requestSysHelper(SysOpKind::TryReadFile, op.result.type);
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".a0.p, i64 " << result << ".a0.n, "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "tryWriteFile") {
            extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
            extractText(op.inputs.at(1), result + ".a1", "%text.tmp1");
            const std::string hfn = requestSysHelper(SysOpKind::TryWriteFile, op.result.type);
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".a0.p, i64 " << result << ".a0.n, ptr "
                   << result << ".a1.p, i64 " << result << ".a1.n)\n";
            return;
        }
        if (name == "fileRead") {
            const std::string sp = mutArgPtr(op, 0, result);
            const std::string hfn = requestSysHelper(SysOpKind::FileRead, op.result.type);
            bodies << "  " << result << ".fp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".fp, " << operand(op.inputs.at(1)) << ", "
                   << diagnostic("R_ARRAY_SIZE", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "fileReadAll") {
            const std::string sp = mutArgPtr(op, 0, result);
            const std::string hfn = requestSysHelper(SysOpKind::FileReadAll, op.result.type);
            bodies << "  " << result << ".fp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".fp, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "fileReadLine") {
            const std::string sp = mutArgPtr(op, 0, result);
            const std::string hfn = requestSysHelper(SysOpKind::FileReadLine, op.result.type);
            bodies << "  " << result << ".fp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".fp, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "fileWrite") {
            const std::string sp = mutArgPtr(op, 0, result);
            extractText(op.inputs.at(1), result + ".a1", "%text.tmp0");
            const std::string hfn = requestSysHelper(SysOpKind::FileWrite, op.result.type);
            bodies << "  " << result << ".fp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".fp, ptr " << result << ".a1.p, i64 " << result << ".a1.n)\n";
            return;
        }
        if (name == "fileFlush") {
            const std::string sp = mutArgPtr(op, 0, result);
            const std::string hfn = requestSysHelper(SysOpKind::FileFlush, op.result.type);
            bodies << "  " << result << ".fp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".fp)\n";
            return;
        }
        if (name == "fileClose" || name == "listenerClose" || name == "socketClose" || name == "unlock") {
            const char* cfn = name == "fileClose" ? "tz_sys_file_close"
                            : (name == "listenerClose" ? "tz_sys_tcp_close"
                            : (name == "socketClose" ? "tz_sys_sock_close" : "tz_sys_lock_unlock"));
            usedDecls.insert(cfn);
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  call void @" << cfn << "(ptr " << sp << ")\n";
            return;
        }
        if (name == "tcpListen") {
            const std::string hfn = requestSysHelper(SysOpKind::TcpListen, op.result.type);
            if (op.inputs.size() >= 2) {
                extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
                bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                       << "(ptr " << result << ".a0.p, i64 " << result << ".a0.n, "
                       << operand(op.inputs.at(1)) << ")\n";
            } else {
                bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                       << "(ptr null, i64 0, " << operand(op.inputs.at(0)) << ")\n";
            }
            return;
        }
        if (name == "tcpLocalPort") {
            usedDecls.insert("tz_sys_tcp_local_port");
            bodies << "  " << result << " = call i64 @tz_sys_tcp_local_port("
                   << operand(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "tcpAccept") {
            const std::string sp = mutArgPtr(op, 0, result);
            const std::string hfn = requestSysHelper(SysOpKind::TcpAccept, op.result.type);
            bodies << "  " << result << ".lp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".lp)\n";
            return;
        }
        if (name == "tcpConnect") {
            extractText(op.inputs.at(0), result + ".a0", "%text.tmp0");
            const std::string hfn = requestSysHelper(SysOpKind::TcpConnect, op.result.type);
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".a0.p, i64 " << result << ".a0.n, "
                   << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "socketRead") {
            const std::string sp = mutArgPtr(op, 0, result);
            const std::string hfn = requestSysHelper(SysOpKind::SockRead, op.result.type);
            bodies << "  " << result << ".sp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".sp, " << operand(op.inputs.at(1)) << ", "
                   << diagnostic("R_ARRAY_SIZE", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "socketWrite") {
            const std::string sp = mutArgPtr(op, 0, result);
            extractText(op.inputs.at(1), result + ".a1", "%text.tmp0");
            const std::string hfn = requestSysHelper(SysOpKind::SockWrite, op.result.type);
            bodies << "  " << result << ".sp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call " << llvmType(op.result.type) << " " << hfn
                   << "(ptr " << result << ".sp, ptr " << result << ".a1.p, i64 " << result << ".a1.n)\n";
            return;
        }
        if (name == "mutexNew") {
            usedDecls.insert("tz_sys_mutex_new");
            bodies << "  " << result << " = call ptr @tz_sys_mutex_new()\n"
                   << "  " << result << ".bad = icmp eq ptr " << result << ", null\n"
                   << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "mutexLock") {
            usedDecls.insert("tz_sys_mutex_lock");
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << ".mp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call ptr @tz_sys_mutex_lock(ptr " << result << ".mp)\n"
                   << "  " << result << ".bad = icmp eq ptr " << result << ", null\n"
                   << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "mutexTryLock") {
            usedDecls.insert("tz_sys_mutex_try_lock");
            const Type optT = op.result.type;
            const std::string optLlvm = llvmType(optT);
            const auto& lay = layoutOf(optT);
            const unsigned noneIdx = variantIndexByName(optT, "None");
            const unsigned someIdx = variantIndexByName(optT, "Some");
            const unsigned phys = lay.singlePayLogicalToLlvm.at(0);
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << ".mp = load ptr, ptr " << sp << "\n"
                   << "  " << result << ".lk = call ptr @tz_sys_mutex_try_lock(ptr " << result << ".mp)\n"
                   << "  " << result << ".isNull = icmp eq ptr " << result << ".lk, null\n"
                   << "  " << result << ".none = insertvalue " << optLlvm << " zeroinitializer, " << lay.tagTy << " " << noneIdx << ", 0\n"
                   << "  " << result << ".s0 = insertvalue " << optLlvm << " zeroinitializer, " << lay.tagTy << " " << someIdx << ", 0\n"
                   << "  " << result << ".some = insertvalue " << optLlvm << " " << result << ".s0, ptr " << result << ".lk, " << phys << "\n"
                   << "  " << result << " = select i1 " << result << ".isNull, " << optLlvm << " " << result << ".none, " << optLlvm << " " << result << ".some\n";
            return;
        }
        if (name == "fileIsOpen" || name == "socketIsOpen" || name == "listenerIsOpen" || name == "lockIsHeld") {
            bodies << "  " << result << " = icmp ne ptr " << reg(op.inputs.at(0)) << ", null\n";
            return;
        }
        if (name == "taskScope") {
            usedDecls.insert("tz_conc_scope_new");
            bodies << "  " << result << " = call ptr @tz_conc_scope_new()\n"
                   << "  " << result << ".bad = icmp eq ptr " << result << ", null\n"
                   << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "taskSpawn" || name == "taskSpawnMove") {
            const bool isMove = (name == "taskSpawnMove");
            const Type taskT = op.result.type;
            const Type retT = program.taskElem(taskT);
            const Type valT = isMove ? op.inputs.at(1).type : Type::Void;
            const Value& fnVal = isMove ? op.inputs.at(2) : op.inputs.at(1);
            const bool hasRes = (retT != Type::Void && !isZeroSized(retT));
            const bool hasVal = (valT != Type::Void && !isZeroSized(valT));
            std::string payTy = "{ ";
            if (hasRes) payTy += llvmType(retT) + ", ";
            payTy += "%fn";
            if (hasVal) payTy += ", " + llvmType(valT);
            payTy += " }";
            const unsigned fnIdx = hasRes ? 1u : 0u;
            const unsigned valIdx = hasRes ? 2u : 1u;
            const std::string sp = mutArgPtr(op, 0, result);
            const std::string runFn = requestTaskRun(valT, retT);
            const std::string dropResFn = (hasRes && needsDrop(retT)) ? requestDrop(retT) : "null";
            usedDecls.insert("tz_conc_task_alloc");
            usedDecls.insert("tz_conc_task_payload");
            usedDecls.insert("tz_conc_task_submit");
            bodies << "  " << result << ".sc = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call ptr @tz_conc_task_alloc(ptr " << result << ".sc, "
                   << "i64 ptrtoint (ptr getelementptr (" << payTy << ", ptr null, i32 1) to i64), "
                   << "ptr " << runFn << ", ptr " << dropResFn << ")\n"
                   << "  " << result << ".bad = icmp eq ptr " << result << ", null\n"
                   << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_ALLOC", op.span) << ")\n"
                   << "  " << result << ".pay = call ptr @tz_conc_task_payload(ptr " << result << ")\n"
                   << "  " << result << ".fnp = getelementptr inbounds " << payTy << ", ptr " << result << ".pay, i32 0, i32 " << fnIdx << "\n"
                   << "  store %fn " << reg(fnVal) << ", ptr " << result << ".fnp\n";
            if (hasVal) {
                bodies << "  " << result << ".valp = getelementptr inbounds " << payTy << ", ptr " << result << ".pay, i32 0, i32 " << valIdx << "\n"
                       << "  store " << operand(op.inputs.at(1)) << ", ptr " << result << ".valp" << heapAlignSuffix(valT) << "\n";
            }
            bodies << "  call void @tz_conc_task_submit(ptr " << result << ")\n";
            return;
        }
        if (name == "taskJoin") {
            usedDecls.insert("tz_conc_task_wait");
            usedDecls.insert("tz_conc_task_join_done");
            const Type retT = op.result.type;
            const bool hasRes = (retT != Type::Void && !isZeroSized(retT));
            const std::string tReg = reg(op.inputs.at(0));
            bodies << "  call void @tz_conc_task_wait(ptr " << tReg << ")\n";
            if (hasRes) {
                usedDecls.insert("tz_conc_task_payload");
                bodies << "  " << result << ".pay = call ptr @tz_conc_task_payload(ptr " << tReg << ")\n"
                       << "  " << result << " = load " << llvmType(retT) << ", ptr " << result << ".pay" << heapAlignSuffix(retT) << "\n";
            } else if (retT != Type::Void) {
                bodies << "  " << result << " = freeze " << llvmType(retT) << " zeroinitializer\n";
            }
            bodies << "  call void @tz_conc_task_join_done(ptr " << tReg << ")\n";
            return;
        }
        if (name == "parallelMap") {
            if (checkDepth) {
                bodies << "  call void @tz.guard(i1 %atLimit, " << diagnostic("R_DEPTH", op.span) << ")\n";
            }
            const std::string pfn = requestPmap(op.result.type);
            bodies << "  " << result << " = call %array " << pfn << "(i32 %childDepth, "
                   << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ", "
                   << diagnostic("R_ARRAY_SIZE", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "freeze") {
            const std::string ffn = requestFzNew(op.result.type);
            bodies << "  " << result << " = call ptr " << ffn << "(" << operand(op.inputs.at(0))
                   << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "frozenGet") {
            const Type fzT = op.inputs.at(0).type;
            const std::string gfn = requestFzGet(fzT);
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << gfn
                   << "(" << operand(op.inputs.at(0)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "frozenRead") {
            if (checkDepth) {
                bodies << "  call void @tz.guard(i1 %atLimit, " << diagnostic("R_DEPTH", op.span) << ")\n";
            }
            const Type fzT = op.inputs.at(0).type;
            const Type retT = op.result.type;
            const std::string rfn = requestFzRead(fzT, retT);
            bodies << "  ";
            if (retT != Type::Void) bodies << result << " = ";
            bodies << "call " << llvmType(retT) << " " << rfn << "(i32 %childDepth, "
                   << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "ptrNull" || name == "mutPtrNull") {
            bodies << "  " << result << " = getelementptr i8, ptr null, i64 0\n";
            return;
        }
        if (name == "ptrIsNull") {
            bodies << "  " << result << " = icmp eq ptr " << reg(op.inputs.at(0)) << ", null\n";
            return;
        }
        if (name == "ptrAddr") {
            bodies << "  " << result << " = ptrtoint ptr " << reg(op.inputs.at(0)) << " to i64\n";
            return;
        }
        if (name == "ptrConst" || name == "ptrMut" || name == "ptrCast" || name == "mutPtrCast") {
            bodies << "  " << result << " = getelementptr i8, ptr " << reg(op.inputs.at(0)) << ", i64 0\n";
            return;
        }
        if (name == "ptrFromAddr" || name == "mutPtrFromAddr") {
            bodies << "  " << result << " = inttoptr i64 " << reg(op.inputs.at(0)) << " to ptr\n";
            return;
        }
        if (name == "ptrOf" || name == "mutPtrOf") {
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << " = getelementptr i8, ptr " << sp << ", i64 0\n";
            return;
        }
        if (name == "slicePtr") {
            bodies << "  " << result << " = extractvalue %array " << reg(op.inputs.at(0)) << ", 0\n";
            return;
        }
        if (name == "mutSlicePtr") {
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << ".arr = load %array, ptr " << sp << "\n"
                   << "  " << result << " = extractvalue %array " << result << ".arr, 0\n";
            return;
        }
        if (name == "ptrAdd") {
            const Type elemT = program.ptrElem(op.inputs.at(0).type);
            const std::size_t elemSz = sizeAlignOf(elemT).first;
            const std::string p = reg(op.inputs.at(0));
            const std::string off = reg(op.inputs.at(1));
            if (!op.unchecked) {
                bodies << "  " << result << ".bad = icmp eq ptr " << p << ", null\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_NULL_PTR", op.span) << ")\n";
            }
            if (elemSz == 1) {
                bodies << "  " << result << " = getelementptr i8, ptr " << p << ", i64 " << off << "\n";
            } else {
                bodies << "  " << result << ".bytes = mul i64 " << off << ", " << elemSz << "\n"
                       << "  " << result << " = getelementptr i8, ptr " << p << ", i64 " << result << ".bytes\n";
            }
            return;
        }
        if (name == "ptrRead") {
            const Type elemT = op.result.type;
            const std::string p = reg(op.inputs.at(0));
            if (!op.unchecked) {
                bodies << "  " << result << ".bad = icmp eq ptr " << p << ", null\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_NULL_PTR", op.span) << ")\n";
            }
            if (elemT == Type::Bool) {
                bodies << "  " << result << ".b8 = load i8, ptr " << p << ", align 1\n"
                       << "  " << result << " = icmp ne i8 " << result << ".b8, 0\n";
            } else {
                bodies << "  " << result << " = load " << llvmType(elemT) << ", ptr " << p << ", align 1\n";
            }
            return;
        }
        if (name == "ptrWrite") {
            const Type elemT = program.ptrElem(op.inputs.at(0).type);
            const std::string p = reg(op.inputs.at(0));
            const std::string v = reg(op.inputs.at(1));
            if (!op.unchecked) {
                bodies << "  " << result << ".bad = icmp eq ptr " << p << ", null\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_NULL_PTR", op.span) << ")\n";
            }
            if (elemT == Type::Bool) {
                bodies << "  " << result << ".b8 = zext i1 " << v << " to i8\n"
                       << "  store i8 " << result << ".b8, ptr " << p << ", align 1\n";
            } else {
                bodies << "  store " << llvmType(elemT) << " " << v << ", ptr " << p << ", align 1\n";
            }
            return;
        }
        if (name == "ptrAlloc" || name == "ptrAllocAligned") {
            usedDecls.insert("tz_sys_aligned_alloc");
            usedDecls.insert("llvm.umul.with.overflow.i64");
            const Type elemT = program.ptrElem(op.result.type);
            const auto [elemSz, elemAl] = sizeAlignOf(elemT);
            const std::string cnt = reg(op.inputs.at(0));
            bodies << "  " << result << ".neg = icmp slt i64 " << cnt << ", 0\n"
                   << "  call void @tz.guard(i1 " << result << ".neg, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n";
            std::string alArg;
            if (name == "ptrAllocAligned") {
                alArg = reg(op.inputs.at(1));
                bodies << "  " << result << ".alneg = icmp sle i64 " << alArg << ", 0\n"
                       << "  " << result << ".alm1 = sub i64 " << alArg << ", 1\n"
                       << "  " << result << ".aland = and i64 " << alArg << ", " << result << ".alm1\n"
                       << "  " << result << ".alnp = icmp ne i64 " << result << ".aland, 0\n"
                       << "  " << result << ".albad = or i1 " << result << ".alneg, " << result << ".alnp\n"
                       << "  call void @tz.guard(i1 " << result << ".albad, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n";
            } else {
                alArg = std::to_string(std::max<std::size_t>(1, elemAl));
            }
            bodies << "  " << result << ".st = call {i64, i1} @llvm.umul.with.overflow.i64(i64 " << cnt << ", i64 " << elemSz << ")\n"
                   << "  " << result << ".bytes = extractvalue {i64, i1} " << result << ".st, 0\n"
                   << "  " << result << ".ov = extractvalue {i64, i1} " << result << ".st, 1\n"
                   << "  call void @tz.guard(i1 " << result << ".ov, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n"
                   << "  " << result << " = call ptr @tz_sys_aligned_alloc(i64 " << result << ".bytes, i64 " << alArg << ")\n"
                   << "  " << result << ".null = icmp eq ptr " << result << ", null\n"
                   << "  call void @tz.guard(i1 " << result << ".null, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "ptrFree") {
            usedDecls.insert("tz_sys_aligned_free");
            bodies << "  call void @tz_sys_aligned_free(ptr " << reg(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "ptrCopy") {
            usedDecls.insert("llvm.umul.with.overflow.i64");
            const Type elemT = program.ptrElem(op.inputs.at(0).type);
            const std::size_t elemSz = sizeAlignOf(elemT).first;
            const std::string dst = reg(op.inputs.at(0));
            const std::string src = reg(op.inputs.at(1));
            const std::string cnt = reg(op.inputs.at(2));
            bodies << "  " << result << ".neg = icmp slt i64 " << cnt << ", 0\n"
                   << "  call void @tz.guard(i1 " << result << ".neg, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n"
                   << "  " << result << ".st = call {i64, i1} @llvm.umul.with.overflow.i64(i64 " << cnt << ", i64 " << elemSz << ")\n"
                   << "  " << result << ".bytes = extractvalue {i64, i1} " << result << ".st, 0\n"
                   << "  " << result << ".ov = extractvalue {i64, i1} " << result << ".st, 1\n"
                   << "  call void @tz.guard(i1 " << result << ".ov, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n";
            if (!op.unchecked) {
                bodies << "  " << result << ".hasc = icmp sgt i64 " << cnt << ", 0\n"
                       << "  " << result << ".dn = icmp eq ptr " << dst << ", null\n"
                       << "  " << result << ".sn = icmp eq ptr " << src << ", null\n"
                       << "  " << result << ".pn = or i1 " << result << ".dn, " << result << ".sn\n"
                       << "  " << result << ".nbad = and i1 " << result << ".hasc, " << result << ".pn\n"
                       << "  call void @tz.guard(i1 " << result << ".nbad, " << diagnostic("R_NULL_PTR", op.span) << ")\n";
            }
            bodies << "  call void @tz.ptrCopy(ptr " << dst << ", ptr " << src << ", i64 " << result << ".bytes)\n";
            return;
        }
        if (name == "ptrZero") {
            usedDecls.insert("llvm.umul.with.overflow.i64");
            const Type elemT = program.ptrElem(op.inputs.at(0).type);
            const std::size_t elemSz = sizeAlignOf(elemT).first;
            const std::string dst = reg(op.inputs.at(0));
            const std::string cnt = reg(op.inputs.at(1));
            bodies << "  " << result << ".neg = icmp slt i64 " << cnt << ", 0\n"
                   << "  call void @tz.guard(i1 " << result << ".neg, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n"
                   << "  " << result << ".st = call {i64, i1} @llvm.umul.with.overflow.i64(i64 " << cnt << ", i64 " << elemSz << ")\n"
                   << "  " << result << ".bytes = extractvalue {i64, i1} " << result << ".st, 0\n"
                   << "  " << result << ".ov = extractvalue {i64, i1} " << result << ".st, 1\n"
                   << "  call void @tz.guard(i1 " << result << ".ov, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n";
            if (!op.unchecked) {
                bodies << "  " << result << ".hasc = icmp sgt i64 " << cnt << ", 0\n"
                       << "  " << result << ".dn = icmp eq ptr " << dst << ", null\n"
                       << "  " << result << ".nbad = and i1 " << result << ".hasc, " << result << ".dn\n"
                       << "  call void @tz.guard(i1 " << result << ".nbad, " << diagnostic("R_NULL_PTR", op.span) << ")\n";
            }
            bodies << "  call void @tz.ptrZero(ptr " << dst << ", i64 " << result << ".bytes)\n";
            return;
        }
        if (name == "cstr") {
            extractText(op.inputs.at(0), result + ".s", "%text.tmp0");
            bodies << "  " << result << ".n1 = add i64 " << result << ".s.n, 1\n"
                   << "  " << result << ".ov = icmp ugt i64 " << result << ".n1, " << maxArrayLength << "\n"
                   << "  call void @tz.guard(i1 " << result << ".ov, " << diagnostic("R_ALLOC", op.span) << ")\n"
                   << "  " << result << ".buf = call ptr @tz.malloc(i64 " << result << ".n1, " << diagnostic("R_ALLOC", op.span) << ")\n"
                   << "  call void @tz.ptrCopy(ptr " << result << ".buf, ptr " << result << ".s.p, i64 " << result << ".s.n)\n"
                   << "  " << result << ".zp = getelementptr inbounds i8, ptr " << result << ".buf, i64 " << result << ".s.n\n"
                   << "  store i8 0, ptr " << result << ".zp\n"
                   << "  " << result << ".n32 = trunc i64 " << result << ".n1 to i32\n"
                   << "  " << result << ".a0 = insertvalue %array zeroinitializer, ptr " << result << ".buf, 0\n"
                   << "  " << result << ".a1 = insertvalue %array " << result << ".a0, i32 " << result << ".n32, 1\n"
                   << "  " << result << " = insertvalue %array " << result << ".a1, i32 " << result << ".n32, 2\n";
            return;
        }
        if (name == "strFromCStr") {
            usedDecls.insert("tz_sys_cstr_len");
            const std::string p = reg(op.inputs.at(0));
            if (!op.unchecked) {
                bodies << "  " << result << ".bad = icmp eq ptr " << p << ", null\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_NULL_PTR", op.span) << ")\n";
            }
            bodies << "  " << result << ".n = call i64 @tz_sys_cstr_len(ptr " << p << ")\n"
                   << "  " << result << " = call %text @tz.textCopySlice(ptr " << p << ", i64 " << result << ".n, "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "strFromPtr") {
            const std::string p = reg(op.inputs.at(0));
            const std::string n = reg(op.inputs.at(1));
            bodies << "  " << result << ".neg = icmp slt i64 " << n << ", 0\n"
                   << "  " << result << ".big = icmp ugt i64 " << n << ", " << maxStringLength << "\n"
                   << "  " << result << ".szbad = or i1 " << result << ".neg, " << result << ".big\n"
                   << "  call void @tz.guard(i1 " << result << ".szbad, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n";
            if (!op.unchecked) {
                bodies << "  " << result << ".pos = icmp sgt i64 " << n << ", 0\n"
                       << "  " << result << ".isn = icmp eq ptr " << p << ", null\n"
                       << "  " << result << ".nbad = and i1 " << result << ".pos, " << result << ".isn\n"
                       << "  call void @tz.guard(i1 " << result << ".nbad, " << diagnostic("R_NULL_PTR", op.span) << ")\n";
            }
            bodies << "  " << result << " = call %text @tz.textCopySlice(ptr " << p << ", i64 " << n << ", "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "simdSplat") {
            const Type vt = op.result.type;
            const std::string vty = llvmType(vt);
            const unsigned lanes = program.simdLanes(vt);
            const std::string maskTy = "<" + std::to_string(lanes) + " x i32>";
            bodies << "  " << result << ".0 = insertelement " << vty << " poison, " << operand(op.inputs.at(0)) << ", i32 0\n"
                   << "  " << result << " = shufflevector " << vty << " " << result << ".0, " << vty
                   << " poison, " << maskTy << " zeroinitializer\n";
            return;
        }
        if (name == "simdGet") {
            const Type vt = op.inputs.at(0).type;
            const unsigned lanes = program.simdLanes(vt);
            const std::string vty = llvmType(vt);
            const std::string idx = reg(op.inputs.at(1));
            if (!op.unchecked) {
                bodies << "  " << result << ".bad = icmp uge i64 " << idx << ", " << lanes << "\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_BOUNDS", op.span) << ")\n";
            }
            bodies << "  " << result << " = extractelement " << vty << " " << reg(op.inputs.at(0)) << ", i64 " << idx << "\n";
            return;
        }
        if (name == "simdSet") {
            const Type vt = op.result.type;
            const unsigned lanes = program.simdLanes(vt);
            const std::string vty = llvmType(vt);
            const std::string idx = reg(op.inputs.at(1));
            if (!op.unchecked) {
                bodies << "  " << result << ".bad = icmp uge i64 " << idx << ", " << lanes << "\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_BOUNDS", op.span) << ")\n";
            }
            bodies << "  " << result << " = insertelement " << vty << " " << reg(op.inputs.at(0)) << ", "
                   << operand(op.inputs.at(2)) << ", i64 " << idx << "\n";
            return;
        }
        if (name == "simdFromArray" || name == "simdToArray") {
            const bool isLoad = (name == "simdFromArray");
            const Type vt = isLoad ? op.result.type : op.inputs.at(0).type;
            const Type elemT = program.simdElem(vt);
            const unsigned lanes = program.simdLanes(vt);
            const std::string vty = llvmType(vt);
            const std::string elemLlvm = llvmType(elemT);
            std::string arrReg;
            if (isLoad) {
                arrReg = reg(op.inputs.at(0));
            } else {
                const std::string sp = mutArgPtr(op, 1, result);
                arrReg = result + ".arr";
                bodies << "  " << arrReg << " = load %array, ptr " << sp << "\n";
            }
            const std::string off = reg(op.inputs.at(isLoad ? 1 : 2));
            bodies << "  " << result << ".len32 = extractvalue %array " << arrReg << ", 1\n"
                   << "  " << result << ".len = zext i32 " << result << ".len32 to i64\n";
            if (!op.unchecked) {
                bodies << "  " << result << ".in = icmp ule i64 " << off << ", " << result << ".len\n"
                       << "  " << result << ".rem = sub i64 " << result << ".len, " << off << "\n"
                       << "  " << result << ".en = icmp uge i64 " << result << ".rem, " << lanes << "\n"
                       << "  " << result << ".ok = and i1 " << result << ".in, " << result << ".en\n"
                       << "  " << result << ".bad = xor i1 " << result << ".ok, true\n"
                       << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_BOUNDS", op.span) << ")\n";
            }
            bodies << "  " << result << ".buf = extractvalue %array " << arrReg << ", 0\n"
                   << "  " << result << ".ep = getelementptr inbounds " << elemLlvm << ", ptr " << result << ".buf, i64 " << off << "\n";
            if (isLoad) {
                bodies << "  " << result << " = load " << vty << ", ptr " << result << ".ep, align 1\n";
            } else {
                bodies << "  store " << vty << " " << reg(op.inputs.at(0)) << ", ptr " << result << ".ep, align 1\n";
            }
            return;
        }
        if (name == "simdLoad" || name == "simdLoadAligned" ||
            name == "simdStore" || name == "simdStoreAligned") {
            const bool isLoad = (name == "simdLoad" || name == "simdLoadAligned");
            const bool aligned = (name == "simdLoadAligned" || name == "simdStoreAligned");
            const Type vt = isLoad ? op.result.type : op.inputs.at(1).type;
            const std::string vty = llvmType(vt);
            const std::size_t align = sizeAlignOf(vt).second;
            const std::string p = reg(op.inputs.at(0));
            if (!op.unchecked) {
                bodies << "  " << result << ".nbad = icmp eq ptr " << p << ", null\n"
                       << "  call void @tz.guard(i1 " << result << ".nbad, " << diagnostic("R_NULL_PTR", op.span) << ")\n";
                if (aligned) {
                    bodies << "  " << result << ".pi = ptrtoint ptr " << p << " to i64\n"
                           << "  " << result << ".am = and i64 " << result << ".pi, " << (align - 1u) << "\n"
                           << "  " << result << ".abad = icmp ne i64 " << result << ".am, 0\n"
                           << "  call void @tz.guard(i1 " << result << ".abad, " << diagnostic("R_ALIGN", op.span) << ")\n";
                }
            }
            const std::size_t emitAlign = aligned ? align : 1u;
            if (isLoad) {
                bodies << "  " << result << " = load " << vty << ", ptr " << p << ", align " << emitAlign << "\n";
            } else {
                bodies << "  store " << vty << " " << reg(op.inputs.at(1)) << ", ptr " << p << ", align " << emitAlign << "\n";
            }
            return;
        }
        if (name == "simdFma") {
            const Type vt = op.result.type;
            const std::string vty = llvmType(vt);
            const std::string intr = "llvm.fma." + simdLlvmSuffix(program.simdElem(vt), program.simdLanes(vt));
            usedRawDecls.insert("declare " + vty + " @" + intr + "(" + vty + ", " + vty + ", " + vty + ")");
            bodies << "  " << result << " = call " << vty << " @" << intr << "("
                   << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ", " << operand(op.inputs.at(2)) << ")\n";
            return;
        }
        if (name == "simdMin" || name == "simdMax") {
            const bool isMin = (name == "simdMin");
            const Type vt = op.result.type;
            const Type elemT = program.simdElem(vt);
            const unsigned lanes = program.simdLanes(vt);
            const std::string vty = llvmType(vt);
            if (Program::isFloatType(elemT)) {
                const char* pred = isMin ? "olt" : "ogt";
                bodies << "  " << result << ".cmp = fcmp " << pred << " " << operand(op.inputs.at(0)) << ", " << reg(op.inputs.at(1)) << "\n"
                       << "  " << result << " = select <" << lanes << " x i1> " << result << ".cmp, "
                       << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << "\n";
            } else {
                const char* opKind = Program::isSignedInt(elemT) ? (isMin ? "smin" : "smax") : (isMin ? "umin" : "umax");
                const std::string intr = std::string("llvm.") + opKind + "." + simdLlvmSuffix(elemT, lanes);
                usedRawDecls.insert("declare " + vty + " @" + intr + "(" + vty + ", " + vty + ")");
                bodies << "  " << result << " = call " << vty << " @" << intr << "("
                       << operand(op.inputs.at(0)) << ", " << operand(op.inputs.at(1)) << ")\n";
            }
            return;
        }
        if (name == "simdAbs") {
            const Type vt = op.result.type;
            const Type elemT = program.simdElem(vt);
            const unsigned lanes = program.simdLanes(vt);
            const std::string vty = llvmType(vt);
            const std::string sfx = simdLlvmSuffix(elemT, lanes);
            if (Program::isFloatType(elemT)) {
                const std::string intr = "llvm.fabs." + sfx;
                usedRawDecls.insert("declare " + vty + " @" + intr + "(" + vty + ")");
                bodies << "  " << result << " = call " << vty << " @" << intr << "(" << operand(op.inputs.at(0)) << ")\n";
            } else {
                const std::string intr = "llvm.abs." + sfx;
                usedRawDecls.insert("declare " + vty + " @" + intr + "(" + vty + ", i1 immarg)");
                bodies << "  " << result << " = call " << vty << " @" << intr << "(" << operand(op.inputs.at(0)) << ", i1 false)\n";
            }
            return;
        }
        if (name == "simdSqrt") {
            const Type vt = op.result.type;
            const std::string vty = llvmType(vt);
            const std::string intr = "llvm.sqrt." + simdLlvmSuffix(program.simdElem(vt), program.simdLanes(vt));
            usedRawDecls.insert("declare " + vty + " @" + intr + "(" + vty + ")");
            bodies << "  " << result << " = call " << vty << " @" << intr << "(" << operand(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "simdSum" || name == "simdReduceMin" || name == "simdReduceMax") {
            const Type vt = op.inputs.at(0).type;
            const Type elemT = program.simdElem(vt);
            const unsigned lanes = program.simdLanes(vt);
            const std::string vty = llvmType(vt);
            const std::string ety = llvmType(elemT);
            const std::string sfx = simdLlvmSuffix(elemT, lanes);
            if (name == "simdSum" && Program::isFloatType(elemT)) {
                const std::string intr = "llvm.vector.reduce.fadd." + sfx;
                usedRawDecls.insert("declare " + ety + " @" + intr + "(" + ety + ", " + vty + ")");
                bodies << "  " << result << " = call " << ety << " @" << intr << "(" << ety << " -0.0, "
                       << operand(op.inputs.at(0)) << ")\n";
            } else {
                const char* rkind = (name == "simdSum") ? "add"
                    : Program::isFloatType(elemT) ? (name == "simdReduceMin" ? "fmin" : "fmax")
                    : Program::isSignedInt(elemT) ? (name == "simdReduceMin" ? "smin" : "smax")
                    : (name == "simdReduceMin" ? "umin" : "umax");
                const std::string intr = std::string("llvm.vector.reduce.") + rkind + "." + sfx;
                usedRawDecls.insert("declare " + ety + " @" + intr + "(" + vty + ")");
                bodies << "  " << result << " = call " << ety << " @" << intr << "(" << operand(op.inputs.at(0)) << ")\n";
            }
            return;
        }
        if (name == "simdCmpEq" || name == "simdCmpLt" || name == "simdCmpLe" ||
            name == "simdCmpGt" || name == "simdCmpGe") {
            const Type vt = op.inputs.at(0).type;
            const Type elemT = program.simdElem(vt);
            const unsigned lanes = program.simdLanes(vt);
            const std::string outVty = llvmType(op.result.type);
            const char* pred = nullptr;
            if (Program::isFloatType(elemT)) {
                pred = (name == "simdCmpEq") ? "oeq" : (name == "simdCmpLt") ? "olt"
                    : (name == "simdCmpLe") ? "ole" : (name == "simdCmpGt") ? "ogt" : "oge";
                bodies << "  " << result << ".c = fcmp " << pred << " " << operand(op.inputs.at(0))
                       << ", " << reg(op.inputs.at(1)) << "\n";
            } else {
                const bool s = Program::isSignedInt(elemT);
                pred = (name == "simdCmpEq") ? "eq"
                    : (name == "simdCmpLt") ? (s ? "slt" : "ult")
                    : (name == "simdCmpLe") ? (s ? "sle" : "ule")
                    : (name == "simdCmpGt") ? (s ? "sgt" : "ugt")
                    : (s ? "sge" : "uge");
                bodies << "  " << result << ".c = icmp " << pred << " " << operand(op.inputs.at(0))
                       << ", " << reg(op.inputs.at(1)) << "\n";
            }
            bodies << "  " << result << " = sext <" << lanes << " x i1> " << result << ".c to " << outVty << "\n";
            return;
        }
        if (name == "simdSelect") {
            const Type mt = op.inputs.at(0).type;
            const unsigned lanes = program.simdLanes(mt);
            bodies << "  " << result << ".c = icmp ne " << operand(op.inputs.at(0)) << ", zeroinitializer\n"
                   << "  " << result << " = select <" << lanes << " x i1> " << result << ".c, "
                   << operand(op.inputs.at(1)) << ", " << operand(op.inputs.at(2)) << "\n";
            return;
        }
        if (name == "clz" || name == "ctz" || name == "popcnt" || name == "bswap" ||
            name == "rotl" || name == "rotr") {
            const Type vt = op.result.type;
            const unsigned w = Program::intBitWidth(vt);
            const std::string ity = llvmType(vt);
            if (name == "bswap" && w == 8u) {
                bodies << "  " << result << " = add i8 " << reg(op.inputs.at(0)) << ", 0\n";
                return;
            }
            if (name == "clz" || name == "ctz") {
                const std::string intr = std::string("llvm.") + (name == "clz" ? "ctlz" : "cttz") + "." + ity;
                usedRawDecls.insert("declare " + ity + " @" + intr + "(" + ity + ", i1 immarg)");
                bodies << "  " << result << " = call " << ity << " @" << intr << "(" << operand(op.inputs.at(0)) << ", i1 false)\n";
                return;
            }
            if (name == "popcnt" || name == "bswap") {
                const std::string intr = std::string("llvm.") + (name == "popcnt" ? "ctpop" : "bswap") + "." + ity;
                if (!(name == "bswap" && w == 32u)) {
                    usedRawDecls.insert("declare " + ity + " @" + intr + "(" + ity + ")");
                }
                bodies << "  " << result << " = call " << ity << " @" << intr << "(" << operand(op.inputs.at(0)) << ")\n";
                return;
            }
            const std::string intr = std::string("llvm.") + (name == "rotl" ? "fshl" : "fshr") + "." + ity;
            usedRawDecls.insert("declare " + ity + " @" + intr + "(" + ity + ", " + ity + ", " + ity + ")");
            const std::string xv = reg(op.inputs.at(0));
            const std::string sv = reg(op.inputs.at(1));
            bodies << "  " << result << " = call " << ity << " @" << intr << "(" << ity << " " << xv << ", "
                   << ity << " " << xv << ", " << ity << " " << sv << ")\n";
            return;
        }
        if (name == "prefetchRead" || name == "prefetchWrite") {
            usedRawDecls.insert("declare void @llvm.prefetch.p0(ptr, i32 immarg, i32 immarg, i32 immarg)");
            const int rw = (name == "prefetchWrite") ? 1 : 0;
            bodies << "  call void @llvm.prefetch.p0(ptr " << reg(op.inputs.at(0)) << ", i32 " << rw << ", i32 3, i32 1)\n";
            return;
        }
        if (name == "spinHint") {
            usedDecls.insert("tz_sys_spin_hint");
            bodies << "  call void @tz_sys_spin_hint()\n";
            return;
        }
        auto atomicStorage = [&](Type et) -> std::pair<std::string, unsigned> {
            if (et == Type::Bool || et == Type::I8 || et == Type::U8) return {"i8", 1u};
            if (et == Type::I16 || et == Type::U16) return {"i16", 2u};
            if (et == Type::I32 || et == Type::U32) return {"i32", 4u};
            return {"i64", 8u};
        };
        auto llvmOrd = [](unsigned code) -> const char* {
            switch (code) {
                case 0u: return "monotonic";
                case 1u: return "acquire";
                case 2u: return "release";
                case 3u: return "acq_rel";
                default: return "seq_cst";
            }
        };
        auto casFailOrd = [](unsigned code) -> const char* {
            switch (code) {
                case 0u: return "monotonic";
                case 1u: return "acquire";
                case 2u: return "monotonic";
                case 3u: return "acquire";
                default: return "seq_cst";
            }
        };
        if (name == "atomicNew") {
            const Type et = program.atomicElem(op.result.type);
            const auto [sTy, al] = atomicStorage(et);
            const std::string vReg = reg(op.inputs.at(0));
            bodies << "  " << result << " = call noalias ptr @tz.malloc(i64 72, " << diagnostic("R_ALLOC", op.span) << ")\n"
                   << "  store i64 1, ptr " << result << ", align 8\n"
                   << "  " << result << ".vp = getelementptr inbounds i8, ptr " << result << ", i64 64\n";
            if (et == Type::Bool) {
                bodies << "  " << result << ".b8 = zext i1 " << vReg << " to i8\n"
                       << "  store i8 " << result << ".b8, ptr " << result << ".vp, align 1\n";
            } else {
                bodies << "  store " << sTy << " " << vReg << ", ptr " << result << ".vp, align " << al << "\n";
            }
            return;
        }
        if (name == "atomicLoad" || name == "atomicLoadOrd") {
            const Type et = program.atomicElem(op.inputs.at(0).type);
            const auto [sTy, al] = atomicStorage(et);
            const unsigned code = op.fields.empty() ? 4u : op.fields[0];
            bodies << "  " << result << ".vp = getelementptr inbounds i8, ptr " << reg(op.inputs.at(0)) << ", i64 64\n";
            if (et == Type::Bool) {
                bodies << "  " << result << ".b8 = load atomic i8, ptr " << result << ".vp " << llvmOrd(code) << ", align 1\n"
                       << "  " << result << " = trunc i8 " << result << ".b8 to i1\n";
            } else {
                bodies << "  " << result << " = load atomic " << sTy << ", ptr " << result << ".vp " << llvmOrd(code)
                       << ", align " << al << "\n";
            }
            return;
        }
        if (name == "atomicStore" || name == "atomicStoreOrd") {
            const Type et = program.atomicElem(op.inputs.at(0).type);
            const auto [sTy, al] = atomicStorage(et);
            const unsigned code = op.fields.empty() ? 4u : op.fields[0];
            const std::string vReg = reg(op.inputs.at(1));
            bodies << "  " << result << ".vp = getelementptr inbounds i8, ptr " << reg(op.inputs.at(0)) << ", i64 64\n";
            if (et == Type::Bool) {
                bodies << "  " << result << ".b8 = zext i1 " << vReg << " to i8\n"
                       << "  store atomic i8 " << result << ".b8, ptr " << result << ".vp " << llvmOrd(code) << ", align 1\n";
            } else {
                bodies << "  store atomic " << sTy << " " << vReg << ", ptr " << result << ".vp " << llvmOrd(code)
                       << ", align " << al << "\n";
            }
            return;
        }
        if (name == "atomicSwap" || name == "atomicSwapOrd") {
            const Type et = program.atomicElem(op.inputs.at(0).type);
            const auto [sTy, al] = atomicStorage(et);
            const unsigned code = op.fields.empty() ? 4u : op.fields[0];
            const std::string vReg = reg(op.inputs.at(1));
            bodies << "  " << result << ".vp = getelementptr inbounds i8, ptr " << reg(op.inputs.at(0)) << ", i64 64\n";
            if (et == Type::Bool) {
                bodies << "  " << result << ".b8 = zext i1 " << vReg << " to i8\n"
                       << "  " << result << ".old = atomicrmw xchg ptr " << result << ".vp, i8 " << result << ".b8 "
                       << llvmOrd(code) << ", align 1\n"
                       << "  " << result << " = trunc i8 " << result << ".old to i1\n";
            } else {
                bodies << "  " << result << " = atomicrmw xchg ptr " << result << ".vp, " << sTy << " " << vReg
                       << " " << llvmOrd(code) << ", align " << al << "\n";
            }
            return;
        }
        if (name == "atomicCas" || name == "atomicCasOrd") {
            const Type et = program.atomicElem(op.inputs.at(0).type);
            const auto [sTy, al] = atomicStorage(et);
            const unsigned code = op.fields.empty() ? 4u : op.fields[0];
            std::string expReg = reg(op.inputs.at(1));
            std::string desReg = reg(op.inputs.at(2));
            bodies << "  " << result << ".vp = getelementptr inbounds i8, ptr " << reg(op.inputs.at(0)) << ", i64 64\n";
            if (et == Type::Bool) {
                bodies << "  " << result << ".e8 = zext i1 " << expReg << " to i8\n"
                       << "  " << result << ".d8 = zext i1 " << desReg << " to i8\n";
                expReg = result + ".e8";
                desReg = result + ".d8";
            }
            bodies << "  " << result << ".cx = cmpxchg ptr " << result << ".vp, " << sTy << " " << expReg
                   << ", " << sTy << " " << desReg << " " << llvmOrd(code) << " " << casFailOrd(code)
                   << ", align " << al << "\n"
                   << "  " << result << " = extractvalue { " << sTy << ", i1 } " << result << ".cx, 1\n";
            return;
        }
        if (name.starts_with("atomicAdd") || name.starts_with("atomicSub") ||
            name.starts_with("atomicAnd") || name.starts_with("atomicOr") ||
            name.starts_with("atomicXor")) {
            const Type et = program.atomicElem(op.inputs.at(0).type);
            const auto [sTy, al] = atomicStorage(et);
            const unsigned code = op.fields.empty() ? 4u : op.fields[0];
            const char* rmwOp = "add";
            if (name.starts_with("atomicSub")) rmwOp = "sub";
            else if (name.starts_with("atomicAnd")) rmwOp = "and";
            else if (name.starts_with("atomicOr")) rmwOp = "or";
            else if (name.starts_with("atomicXor")) rmwOp = "xor";
            bodies << "  " << result << ".vp = getelementptr inbounds i8, ptr " << reg(op.inputs.at(0)) << ", i64 64\n"
                   << "  " << result << " = atomicrmw " << rmwOp << " ptr " << result << ".vp, " << sTy << " "
                   << reg(op.inputs.at(1)) << " " << llvmOrd(code) << ", align " << al << "\n";
            return;
        }
        if (name == "atomicFence") {
            const unsigned code = op.fields.empty() ? 4u : op.fields[0];
            bodies << "  fence " << llvmOrd(code) << "\n";
            return;
        }
        if (name == "ringNew") {
            usedDecls.insert("tz_conc_ring_new");
            const Type et = program.ringQueueElem(op.result.type);
            const bool zeroE = isZeroSized(et);
            const std::string es = zeroE ? "0" : strideExpr(et);
            const std::string dropFn = (!zeroE && needsDrop(et)) ? requestDropUnaligned(et) : "null";
            const std::string capReg = reg(op.inputs.at(0));
            bodies << "  " << result << ".le0 = icmp sle i64 " << capReg << ", 0\n"
                   << "  " << result << ".big = icmp sgt i64 " << capReg << ", " << maxArrayLength << "\n"
                   << "  " << result << ".badcap = or i1 " << result << ".le0, " << result << ".big\n"
                   << "  call void @tz.guard(i1 " << result << ".badcap, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n"
                   << "  " << result << " = call ptr @tz_conc_ring_new(i64 " << capReg << ", i64 " << es
                   << ", ptr " << dropFn << ")\n"
                   << "  " << result << ".bad = icmp eq ptr " << result << ", null\n"
                   << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "ringPush") {
            const std::string pfn = requestRingPush(op.inputs.at(0).type);
            bodies << "  " << result << " = call i1 " << pfn << "(ptr " << reg(op.inputs.at(0))
                   << ", " << operand(op.inputs.at(1)) << ")\n";
            return;
        }
        if (name == "ringPop") {
            const std::string pfn = requestRingPop(op.inputs.at(0).type, op.result.type);
            bodies << "  " << result << " = call " << llvmType(op.result.type) << " " << pfn
                   << "(ptr " << reg(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "ringLen") {
            usedDecls.insert("tz_conc_ring_len");
            bodies << "  " << result << " = call i64 @tz_conc_ring_len(ptr " << reg(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "ringCap") {
            usedDecls.insert("tz_conc_ring_cap");
            bodies << "  " << result << " = call i64 @tz_conc_ring_cap(ptr " << reg(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "regionNew") {
            usedDecls.insert("tz_sys_region_new");
            const std::string capReg = reg(op.inputs.at(0));
            bodies << "  " << result << ".neg = icmp slt i64 " << capReg << ", 0\n"
                   << "  " << result << ".big = icmp sgt i64 " << capReg << ", " << (1LL << 30) << "\n"
                   << "  " << result << ".badcap = or i1 " << result << ".neg, " << result << ".big\n"
                   << "  call void @tz.guard(i1 " << result << ".badcap, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n"
                   << "  " << result << " = call ptr @tz_sys_region_new(i64 " << capReg << ")\n"
                   << "  " << result << ".bad = icmp eq ptr " << result << ", null\n"
                   << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "regionAlloc" || name == "regionAllocAligned") {
            usedDecls.insert("tz_sys_region_alloc");
            usedDecls.insert("llvm.umul.with.overflow.i64");
            const Type elemT = program.ptrElem(op.result.type);
            const auto [elemSz, elemAl] = sizeAlignOf(elemT);
            const std::string cnt = reg(op.inputs.at(1));
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << ".neg = icmp slt i64 " << cnt << ", 0\n"
                   << "  call void @tz.guard(i1 " << result << ".neg, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n";
            std::string alArg;
            if (name == "regionAllocAligned") {
                alArg = reg(op.inputs.at(2));
                bodies << "  " << result << ".alneg = icmp sle i64 " << alArg << ", 0\n"
                       << "  " << result << ".alm1 = sub i64 " << alArg << ", 1\n"
                       << "  " << result << ".aland = and i64 " << alArg << ", " << result << ".alm1\n"
                       << "  " << result << ".alnp = icmp ne i64 " << result << ".aland, 0\n"
                       << "  " << result << ".albad = or i1 " << result << ".alneg, " << result << ".alnp\n"
                       << "  call void @tz.guard(i1 " << result << ".albad, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n";
            } else {
                alArg = std::to_string(std::max<std::size_t>(1, elemAl));
            }
            bodies << "  " << result << ".st = call {i64, i1} @llvm.umul.with.overflow.i64(i64 " << cnt << ", i64 " << elemSz << ")\n"
                   << "  " << result << ".bytes = extractvalue {i64, i1} " << result << ".st, 0\n"
                   << "  " << result << ".ov = extractvalue {i64, i1} " << result << ".st, 1\n"
                   << "  call void @tz.guard(i1 " << result << ".ov, " << diagnostic("R_ARRAY_SIZE", op.span) << ")\n"
                   << "  " << result << ".rp = load ptr, ptr " << sp << "\n"
                   << "  " << result << " = call ptr @tz_sys_region_alloc(ptr " << result << ".rp, i64 "
                   << result << ".bytes, i64 " << alArg << ")\n"
                   << "  " << result << ".null = icmp eq ptr " << result << ", null\n"
                   << "  call void @tz.guard(i1 " << result << ".null, " << diagnostic("R_ALLOC", op.span) << ")\n";
            return;
        }
        if (name == "regionReset") {
            usedDecls.insert("tz_sys_region_reset");
            const std::string sp = mutArgPtr(op, 0, result);
            bodies << "  " << result << ".rp = load ptr, ptr " << sp << "\n"
                   << "  call void @tz_sys_region_reset(ptr " << result << ".rp)\n";
            return;
        }
        if (name == "regionUsed") {
            usedDecls.insert("tz_sys_region_used");
            bodies << "  " << result << " = call i64 @tz_sys_region_used(ptr " << reg(op.inputs.at(0)) << ")\n";
            return;
        }
        if (name == "regionCapacity") {
            usedDecls.insert("tz_sys_region_cap");
            bodies << "  " << result << " = call i64 @tz_sys_region_cap(ptr " << reg(op.inputs.at(0)) << ")\n";
            return;
        }
        throw std::logic_error("unknown native builtin: " + name);
    }
    void operation(const Operation& op) {
        const auto result = reg(op.result);
        const auto type = llvmType(op.result.type);
        const auto first = [&] { return operand(op.inputs.at(0)); };
        switch (op.op) {
        case Op::Int: {
            if (op.result.type == Type::U64) {
                std::uint64_t u{};
                const char* beg = op.data.data();
                const char* end = beg + op.data.size();
                int base = 10;
                if (op.data.starts_with("0x") || op.data.starts_with("0X")) { beg += 2; base = 16; }
                else if (op.data.starts_with("0b") || op.data.starts_with("0B")) { beg += 2; base = 2; }
                const auto parsed = std::from_chars(beg, end, u, base);
                if (parsed.ec != std::errc{} || parsed.ptr != end) {
                    throw std::logic_error("invalid checked u64 literal");
                }
                bodies << "  " << result << " = add i64 0, " << u << "\n";
                break;
            }
            std::int64_t n{};
            const auto parsed = std::from_chars(op.data.data(), op.data.data() + op.data.size(), n);
            if (parsed.ec != std::errc{} || parsed.ptr != op.data.data() + op.data.size()) {
                throw std::logic_error("invalid checked integer literal");
            }
            bodies << "  " << result << " = add " << type << " 0, " << n << "\n";
            break;
        }
        case Op::Float:
            if (op.result.type == Type::F32)
                bodies << "  " << result << " = bitcast i32 " << op.data << " to float\n";
            else
                bodies << "  " << result << " = bitcast i64 " << op.data << " to double\n";
            break;
        case Op::Bool: bodies << "  " << result << " = or i1 false, " << op.data << "\n"; break;
        case Op::Text: bodies << "  " << result << " = freeze " << literal(op.data) << "\n"; break;
        case Op::Interp: {
            bodies << "  store %array zeroinitializer, ptr %interp.sb\n";
            const std::string allocDiag = diagnostic("R_ALLOC", op.span);
            for (std::size_t i = 0; i < op.inputs.size(); ++i) {
                const Value& pv = op.inputs[i];
                const std::string step = result + ".p" + std::to_string(i);
                if (pv.type == Type::Str) {
                    bodies << "  call void @tz.sbPush(ptr %interp.sb, " << operand(pv) << ", " << allocDiag << ")\n";
                } else if (pv.type == Type::Int) {
                    bodies << "  call void @tz.sbPushInt(ptr %interp.sb, " << operand(pv) << ", " << allocDiag << ")\n";
                } else if (pv.type == Type::U64) {
                    usedDecls.insert("tz_sys_format_u64");
                    bodies << "  " << step << ".n = call i64 @tz_sys_format_u64(" << operand(pv)
                           << ", ptr %float.buf, i64 64)\n"
                           << "  call void @tz.sbPushRaw(ptr %interp.sb, ptr %float.buf, i64 " << step << ".n, "
                           << allocDiag << ")\n";
                } else if (Program::isIntType(pv.type)) {
                    const char* extOp = Program::isSignedInt(pv.type) ? "sext" : "zext";
                    bodies << "  " << step << ".ext = " << extOp << " " << operand(pv) << " to i64\n"
                           << "  call void @tz.sbPushInt(ptr %interp.sb, i64 " << step << ".ext, " << allocDiag << ")\n";
                } else if (Program::isFloatType(pv.type)) {
                    usedDecls.insert("tz_sys_format_float");
                    std::string dArg = operand(pv);
                    if (pv.type == Type::F32) {
                        bodies << "  " << step << ".fd = fpext float " << reg(pv) << " to double\n";
                        dArg = "double " + step + ".fd";
                    }
                    bodies << "  " << step << ".n = call i64 @tz_sys_format_float(" << dArg
                           << ", ptr %float.buf, i64 64)\n"
                           << "  call void @tz.sbPushRaw(ptr %interp.sb, ptr %float.buf, i64 " << step << ".n, "
                           << allocDiag << ")\n";
                } else if (pv.type == Type::Bool) {
                    bodies << "  " << step << ".t = select " << operand(pv) << ", " << literal("true")
                           << ", " << literal("false") << "\n"
                           << "  call void @tz.sbPush(ptr %interp.sb, %text " << step << ".t, " << allocDiag << ")\n";
                } else {
                    throw std::logic_error("unsupported interpolation part type in native backend");
                }
            }
            bodies << "  " << result << " = call %text @tz.sbTake(ptr %interp.sb)\n";
            break;
        }
        case Op::ArrayLit: {
            if (op.inputs.empty()) {
                bodies << "  " << result << " = insertvalue %array zeroinitializer, ptr null, 0\n";
                break;
            }
            const Type elemT = elemOf(op.result.type);
            const std::string elemLlvm = llvmType(elemT);
            const std::size_t n = op.inputs.size();
            bodies << "  " << result << ".p = call ptr @tz.allocBuf(i64 " << n << ", i64 " << strideExpr(elemT)
                   << ", " << diagnostic("R_ARRAY_SIZE", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            for (std::size_t i = 0; i < n; ++i) {
                bodies << "  " << result << ".e" << i << " = getelementptr inbounds " << elemLlvm
                       << ", ptr " << result << ".p, i64 " << i << "\n"
                       << "  store " << operand(op.inputs[i]) << ", ptr " << result << ".e" << i << heapAlignSuffix(elemT) << "\n";
            }
            bodies << "  " << result << ".a0 = insertvalue %array zeroinitializer, ptr " << result << ".p, 0\n"
                   << "  " << result << ".a1 = insertvalue %array " << result << ".a0, i32 " << n << ", 1\n"
                   << "  " << result << " = insertvalue %array " << result << ".a1, i32 " << n << ", 2\n";
            break;
        }
        case Op::Slice: {
            const auto base = first();
            const auto lo = reg(op.inputs.at(1));
            const auto hi = reg(op.inputs.at(2));
            if (op.result.type == Type::Str) {
                bodies << "  " << result << " = call %text @tz.sliceText(" << base
                       << ", i64 " << lo << ", i64 " << hi << ", "
                       << diagnostic("R_BOUNDS", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            } else {
                bodies << "  " << result << " = call %array " << requestSlice(op.result.type) << "(" << base
                       << ", i64 " << lo << ", i64 " << hi << ", "
                       << diagnostic("R_BOUNDS", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            }
            break;
        }
        case Op::Load:
            bodies << "  " << result << " = load " << type << ", ptr " << slotPtr(op.slot) << "\n";
            break;
        case Op::Store: bodies << "  store " << first() << ", ptr " << slotPtr(op.slot) << "\n"; break;
        case Op::Call: {
            std::vector<std::string> ptrArgs(op.inputs.size());
            for (std::size_t i = 0; i < op.inputs.size(); ++i) {
                if (i < op.mutArgSlots.size() && op.mutArgSlots[i] != ~0u)
                    ptrArgs[i] = mutArgPtr(op, i, result + ".a" + std::to_string(i));
            }
            if (checkDepth)
                bodies << "  call void @tz.guard(i1 %atLimit, " << diagnostic("R_DEPTH", op.span) << ")\n";
            bodies << "  ";
            if (op.result.type != Type::Void) bodies << result << " = ";
            bodies << "call " << type << " " << symbol(op.data) << "(i32 %childDepth";
            for (std::size_t i = 0; i < op.inputs.size(); ++i) {
                if (!ptrArgs[i].empty())
                    bodies << ", ptr " << ptrArgs[i];
                else
                    bodies << ", " << operand(op.inputs[i]);
            }
            bodies << ")\n";
            break;
        }
        case Op::ExternCall: {
            const Function* callee = nullptr;
            for (const auto& fn : program.functions) {
                if (!fn.isTest && fn.name == op.data) { callee = &fn; break; }
            }
            if (!callee) throw std::logic_error("missing extern callee in native emission");
            bodies << "  ";
            if (op.result.type != Type::Void) bodies << result << " = ";
            bodies << "call " << ffiAttr(callee->result) << type << " @" << callee->cSymbol << "(";
            for (std::size_t i = 0; i < op.inputs.size(); ++i) {
                if (i > 0) bodies << ", ";
                bodies << llvmType(op.inputs[i].type) << ffiParamAttr(callee->params[i].type) << " " << reg(op.inputs[i]);
            }
            bodies << ")\n";
            break;
        }
        case Op::FnPtr:
            bodies << "  " << result << " = getelementptr i8, ptr @" << op.data << ", i64 0\n";
            break;
        case Op::BoxDyn: {
            const Type iface = static_cast<Type>(op.fields.at(0));
            const Type concreteT = op.inputs.at(0).type;
            usesDynRuntime = true;
            usedDynBoxes.emplace(iface, concreteT);
            const std::string suffix = std::to_string(static_cast<int>(iface)) + "." + std::to_string(static_cast<int>(concreteT));
            bodies << "  " << result << " = call %dyn @tz.box." << suffix << "("
                   << operand(op.inputs.at(0)) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            break;
        }
        case Op::DynCall: {
            usesDynRuntime = true;
            const unsigned mi = op.fields.at(1);
            std::vector<std::string> ptrArgs(op.inputs.size());
            for (std::size_t i = 0; i < op.inputs.size(); ++i) {
                if (i < op.mutArgSlots.size() && op.mutArgSlots[i] != ~0u)
                    ptrArgs[i] = mutArgPtr(op, i, result + ".a" + std::to_string(i));
            }
            if (checkDepth)
                bodies << "  call void @tz.guard(i1 %atLimit, " << diagnostic("R_DEPTH", op.span) << ")\n";
            std::string dynReg;
            if (!ptrArgs[0].empty()) {
                dynReg = result + ".dyn";
                bodies << "  " << dynReg << " = load %dyn, ptr " << ptrArgs[0] << "\n";
            } else {
                dynReg = reg(op.inputs.at(0));
            }
            bodies << "  " << result << ".data = extractvalue %dyn " << dynReg << ", 0\n"
                   << "  " << result << ".vt = extractvalue %dyn " << dynReg << ", 1\n"
                   << "  " << result << ".fp = getelementptr inbounds ptr, ptr " << result << ".vt, i64 " << (2u + mi) << "\n"
                   << "  " << result << ".fn = load ptr, ptr " << result << ".fp\n"
                   << "  ";
            if (op.result.type != Type::Void) bodies << result << " = ";
            bodies << "call " << type << " " << result << ".fn(i32 %childDepth, ptr " << result << ".data";
            for (std::size_t i = 1; i < op.inputs.size(); ++i) {
                if (!ptrArgs[i].empty())
                    bodies << ", ptr " << ptrArgs[i];
                else
                    bodies << ", " << operand(op.inputs[i]);
            }
            bodies << ")\n";
            break;
        }
        case Op::MakeClosure: {
            const std::size_t cid = op.fields.at(0);
            usesFnRuntime = true;
            usedClosures.insert(cid);
            const auto& cl = program.closures.at(cid);
            bodies << "  " << result << " = call %fn @tz.mkclosure." << cid << "(";
            if (cl.envStruct != Type::Void && !isZeroSized(cl.envStruct)) {
                bodies << operand(op.inputs.at(0)) << ", ";
            }
            bodies << diagnostic("R_ALLOC", op.span) << ")\n";
            break;
        }
        case Op::ClosureCall: {
            usesFnRuntime = true;
            if (checkDepth)
                bodies << "  call void @tz.guard(i1 %atLimit, " << diagnostic("R_DEPTH", op.span) << ")\n";
            const std::string fnReg = reg(op.inputs.at(0));
            bodies << "  " << result << ".env = extractvalue %fn " << fnReg << ", 0\n"
                   << "  " << result << ".vt = extractvalue %fn " << fnReg << ", 1\n"
                   << "  " << result << ".fp = getelementptr inbounds ptr, ptr " << result << ".vt, i64 2\n"
                   << "  " << result << ".fn = load ptr, ptr " << result << ".fp\n"
                   << "  ";
            if (op.result.type != Type::Void) bodies << result << " = ";
            bodies << "call " << type << " " << result << ".fn(i32 %childDepth, ptr " << result << ".env";
            for (std::size_t i = 1; i < op.inputs.size(); ++i) {
                bodies << ", " << operand(op.inputs[i]);
            }
            bodies << ")\n";
            break;
        }
        case Op::Builtin: builtin(op); break;
        case Op::Length:
            if (op.inputs.at(0).type == Type::Str) {
                bodies << "  " << result << ".hdr = extractvalue " << first() << ", 0\n"
                       << "  " << result << ".l32 = and i32 " << result << ".hdr, 2147483647\n"
                       << "  " << result << " = zext i32 " << result << ".l32 to i64\n";
            } else {
                bodies << "  " << result << ".l32 = extractvalue " << first() << ", 1\n"
                       << "  " << result << " = zext i32 " << result << ".l32 to i64\n";
            }
            break;
        case Op::ReadByte:
            bodies << "  " << result << ".raw = call i32 @getchar()\n"
                   << "  " << result << " = sext i32 " << result << ".raw to i64\n";
            break;
        case Op::RepeatHeap:
        case Op::RepeatRegion: {
            if (op.result.type == Type::IntArray) {
                const unsigned bits = static_cast<unsigned>(op.elemBits);
                const std::string suffix = bits < 64 ? (".i" + std::to_string(bits)) : "";
                bodies << "  " << result << " = call %array @tz.repeat" << suffix << "(" << first() << ", " << operand(op.inputs[1])
                       << ", ptr " << (op.op == Op::RepeatRegion ? "%arena" : "null") << ", i1 "
                       << (op.initialize ? "true" : "false") << ", "
                       << diagnostic("R_ARRAY_SIZE", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            } else {
                bodies << "  " << result << " = call %array " << requestRepeat(op.result.type) << "(" << first()
                       << ", " << operand(op.inputs[1]) << ", i1 " << (op.initialize ? "true" : "false") << ", "
                       << diagnostic("R_ARRAY_SIZE", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            }
            break;
        }
        case Op::Clone:
            if (op.result.type == Type::IntArray) {
                const unsigned bits = static_cast<unsigned>(op.elemBits);
                const std::string suffix = bits < 64 ? (".i" + std::to_string(bits)) : "";
                bodies << "  " << result << " = call %array @tz.clone" << suffix << "(" << first() << ", ptr null, "
                       << diagnostic("R_ALLOC", op.span);
                if (auditCopies)
                    bodies << ", i64 " << op.span.start << ", i64 " << op.span.end;
                bodies << ")\n";
            } else {
                bodies << "  " << result << " = call " << type << " " << requestClone(op.result.type)
                       << "(" << first() << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            }
            break;
        case Op::Drop: {
            const Type slotT = curSlotTypes.at(op.slot);
            bodies << "  call void " << requestDrop(slotT) << "(ptr " << slotPtr(op.slot) << ")\n";
            break;
        }
        case Op::ClearOwner: {
            const Type slotT = curSlotTypes.at(op.slot);
            bodies << "  store " << llvmType(slotT) << " zeroinitializer, ptr " << slotPtr(op.slot) << "\n";
            break;
        }
        case Op::ClearField: {
            Type curT = curSlotTypes.at(op.slot);
            std::string ptr = slotPtr(op.slot);
            bool packedField = false;
            for (std::size_t fi = 0; fi < op.fields.size(); ++fi) {
                const auto& lay = layoutOf(curT);
                if (lay.packed) packedField = true;
                const unsigned logIdx = op.fields[fi];
                const unsigned phys = lay.variants.at(0).logicalToLlvm.at(logIdx);
                const std::string nextPtr = result + ".cf" + std::to_string(fi);
                bodies << "  " << nextPtr << " = getelementptr inbounds " << llvmType(curT)
                       << ", ptr " << ptr << ", i32 0, i32 " << phys << "\n";
                ptr = nextPtr;
                curT = program.declaration(curT)->variants.at(0).fields.at(logIdx).type;
            }
            bodies << "  store " << llvmType(curT) << " zeroinitializer, ptr " << ptr
                   << (packedField ? ", align 1" : "") << "\n";
            break;
        }
        case Op::ClearIndex: {
            const Type arrT = op.inputs.at(0).type;
            const Type elemT = elemOf(arrT);
            const std::string elemLlvm = llvmType(elemT);
            bodies << "  " << result << ".base = extractvalue " << first() << ", 0\n"
                   << "  " << result << ".ep = getelementptr inbounds " << elemLlvm << ", ptr " << result << ".base, "
                   << operand(op.inputs.at(1)) << "\n"
                   << "  store " << elemLlvm << " zeroinitializer, ptr " << result << ".ep\n";
            break;
        }
        case Op::RegionMark:
            if (!curPureStackArena) bodies << "  " << result << " = call %mark @tz.mark(ptr %arena)\n";
            break;
        case Op::RegionRewind:
            if (!curPureStackArena) bodies << "  call void @tz.rewind(ptr %arena, %mark " << reg(op.inputs.at(0)) << ")\n";
            break;
        case Op::Index:
        case Op::IndexSet: {
            const Type arrT = op.inputs.at(0).type;
            std::string aliasSuffix;
            if (op.aliasGroup >= 0 && static_cast<std::size_t>(op.aliasGroup) < curScopeMeta.size()) {
                aliasSuffix = ", !alias.scope !" + std::to_string(curScopeMeta[op.aliasGroup]) +
                              ", !noalias !" + std::to_string(curNoAliasMeta[op.aliasGroup]);
            }
            if (op.loopAccessGroup >= 0) {
                if (auto agIt = curAccessGroupMeta.find(op.loopAccessGroup); agIt != curAccessGroupMeta.end()) {
                    aliasSuffix += ", !llvm.access.group !" + std::to_string(agIt->second);
                }
            }
            if (arrT == Type::Str) {
                bodies << "  " << result << " = call i64 @tz.byteAt(" << first() << ", "
                       << operand(op.inputs.at(1)) << ", " << diagnostic("R_BOUNDS", op.span) << ")\n";
                break;
            }
            if (arrT == Type::IntArray) {
                const unsigned bits = static_cast<unsigned>(op.elemBits);
                const std::string elemTy = (bits < 64) ? ("i" + std::to_string(bits)) : "i64";
                if (op.boundsProven) {
                    bodies << "  " << result << ".base = extractvalue " << first() << ", 0\n"
                           << "  " << result << ".ptr = getelementptr inbounds " << elemTy << ", ptr " << result << ".base, "
                           << operand(op.inputs.at(1)) << "\n";
                } else {
                    const std::string atName = (bits < 64) ? ("@tz.at.i" + std::to_string(bits)) : "@tz.at";
                    bodies << "  " << result << ".ptr = call ptr " << atName << "(" << first() << ", " << operand(op.inputs.at(1))
                           << ", " << diagnostic("R_BOUNDS", op.span) << ")\n";
                }
                if (op.op == Op::Index) {
                    if (bits < 64) {
                        bodies << "  " << result << ".raw = load " << elemTy << ", ptr " << result << ".ptr" << aliasSuffix << "\n"
                               << "  " << result << " = " << (op.elemUnsigned ? "zext " : "sext ")
                               << elemTy << " " << result << ".raw to i64\n";
                    } else {
                        bodies << "  " << result << " = load i64, ptr " << result << ".ptr" << aliasSuffix << "\n";
                    }
                } else {
                    if (bits < 64) {
                        bodies << "  " << result << ".val = trunc i64 " << reg(op.inputs.at(2)) << " to " << elemTy << "\n"
                               << "  store " << elemTy << " " << result << ".val, ptr " << result << ".ptr" << aliasSuffix << "\n";
                    } else {
                        bodies << "  store " << operand(op.inputs.at(2)) << ", ptr " << result << ".ptr" << aliasSuffix << "\n";
                    }
                }
            } else {
                const Type elemT = elemOf(arrT);
                const std::string elemLlvm = llvmType(elemT);
                if (op.boundsProven) {
                    bodies << "  " << result << ".base = extractvalue " << first() << ", 0\n"
                           << "  " << result << ".ptr = getelementptr inbounds " << elemLlvm << ", ptr " << result << ".base, "
                           << operand(op.inputs.at(1)) << "\n";
                } else {
                    bodies << "  " << result << ".ptr = call ptr " << requestAt(arrT) << "(" << first() << ", "
                           << operand(op.inputs.at(1)) << ", " << diagnostic("R_BOUNDS", op.span) << ")\n";
                }
                const std::string hAlign = heapAlignSuffix(elemT);
                if (op.op == Op::Index) {
                    bodies << "  " << result << " = load " << elemLlvm << ", ptr " << result << ".ptr" << hAlign << aliasSuffix << "\n";
                } else {
                    if (needsDrop(elemT)) {
                        bodies << "  call void " << (hAlign.empty() ? requestDrop(elemT) : requestDropUnaligned(elemT))
                               << "(ptr " << result << ".ptr)\n";
                    }
                    bodies << "  store " << operand(op.inputs.at(2)) << ", ptr " << result << ".ptr" << hAlign << aliasSuffix << "\n";
                }
            }
            break;
        }
        case Op::FieldSet: {
            Type curT = curSlotTypes.at(op.slot);
            std::string ptr = slotPtr(op.slot);
            bool packedField = false;
            for (std::size_t fi = 0; fi < op.fields.size(); ++fi) {
                const auto& lay = layoutOf(curT);
                if (lay.packed) packedField = true;
                const unsigned logIdx = op.fields[fi];
                const unsigned phys = lay.variants.at(0).logicalToLlvm.at(logIdx);
                const std::string nextPtr = result + ".f" + std::to_string(fi);
                bodies << "  " << nextPtr << " = getelementptr inbounds " << llvmType(curT)
                       << ", ptr " << ptr << ", i32 0, i32 " << phys << "\n";
                ptr = nextPtr;
                curT = program.declaration(curT)->variants.at(0).fields.at(logIdx).type;
            }
            if (op.inputs.size() == 1) {
                if (needsDrop(curT)) {
                    bodies << "  call void " << (packedField ? requestDropUnaligned(curT) : requestDrop(curT))
                           << "(ptr " << ptr << ")\n";
                }
                bodies << "  store " << operand(op.inputs[0]) << ", ptr " << ptr
                       << (packedField ? ", align 1" : "") << "\n";
            } else {
                bodies << "  " << result << ".arr = load %array, ptr " << ptr
                       << (packedField ? ", align 1" : "") << "\n";
                if (curT == Type::IntArray) {
                    bodies << "  " << result << ".ep = call ptr @tz.at(%array " << result << ".arr, "
                           << operand(op.inputs[0]) << ", " << diagnostic("R_BOUNDS", op.span) << ")\n"
                           << "  store " << operand(op.inputs[1]) << ", ptr " << result << ".ep\n";
                } else {
                    const Type elemT = elemOf(curT);
                    bodies << "  " << result << ".ep = call ptr " << requestAt(curT) << "(%array " << result << ".arr, "
                           << operand(op.inputs[0]) << ", " << diagnostic("R_BOUNDS", op.span) << ")\n";
                    if (needsDrop(elemT)) {
                        bodies << "  call void " << requestDrop(elemT) << "(ptr " << result << ".ep)\n";
                    }
                    bodies << "  store " << operand(op.inputs[1]) << ", ptr " << result << ".ep\n";
                }
            }
            break;
        }
        case Op::PlaceSet: {
            Type curT = curSlotTypes.at(op.slot);
            std::string ptr = slotPtr(op.slot);
            bool packedField = false;
            std::size_t idxPos = 0;
            for (std::size_t si = 0; si < op.placeSteps.size(); ++si) {
                const auto& step = op.placeSteps[si];
                const std::string stepPfx = result + ".p" + std::to_string(si);
                if (step.isIndex) {
                    bodies << "  " << stepPfx << ".arr = load %array, ptr " << ptr
                           << (packedField ? ", align 1" : "") << "\n";
                    const std::string atFn = (curT == Type::IntArray) ? "@tz.at" : requestAt(curT);
                    bodies << "  " << stepPfx << ".ep = call ptr " << atFn << "(%array " << stepPfx << ".arr, "
                           << operand(op.inputs.at(idxPos++)) << ", " << diagnostic("R_BOUNDS", op.span) << ")\n";
                    ptr = stepPfx + ".ep";
                    curT = elemOf(curT);
                    packedField = false;
                } else {
                    const auto& lay = layoutOf(curT);
                    if (lay.packed) packedField = true;
                    const unsigned phys = lay.variants.at(0).logicalToLlvm.at(step.fieldIdx);
                    const std::string nextPtr = stepPfx + ".f";
                    bodies << "  " << nextPtr << " = getelementptr inbounds " << llvmType(curT)
                           << ", ptr " << ptr << ", i32 0, i32 " << phys << "\n";
                    ptr = nextPtr;
                    curT = program.declaration(curT)->variants.at(0).fields.at(step.fieldIdx).type;
                }
            }
            if (needsDrop(curT)) {
                bodies << "  call void " << (packedField ? requestDropUnaligned(curT) : requestDrop(curT))
                       << "(ptr " << ptr << ")\n";
            }
            bodies << "  store " << operand(op.inputs.back()) << ", ptr " << ptr
                   << (packedField ? ", align 1" : "") << "\n";
            break;
        }
        case Op::Construct: construct(op); break;
        case Op::Field: {
            const Type baseType = op.inputs.at(0).type;
            const auto& lay = layoutOf(baseType);
            if (lay.multiPayload) {
                const unsigned v = op.fields.at(0) - 1;
                const unsigned phys = lay.variants.at(v).logicalToLlvm.at(op.fields.at(1));
                const std::string scratch = "%enum.tmp." + std::to_string(static_cast<int>(baseType));
                const std::string baseLlvm = llvmType(baseType);
                const std::string pType = baseLlvm + ".p" + std::to_string(v);
                bodies << "  store " << first() << ", ptr " << scratch << "\n"
                       << "  " << result << ".pay = getelementptr inbounds " << baseLlvm << ", ptr " << scratch
                       << ", i32 0, i32 " << lay.payFieldIdx << "\n"
                       << "  " << result << ".fp = getelementptr inbounds " << pType << ", ptr " << result << ".pay, i32 0, i32 " << phys << "\n"
                       << "  " << result << " = load " << type << ", ptr " << result << ".fp\n";
            } else if (lay.isEnum) {
                const unsigned phys = lay.singlePayLogicalToLlvm.at(op.fields.at(1));
                bodies << "  " << result << " = extractvalue " << first() << ", " << phys << "\n";
            } else {
                const unsigned phys = lay.variants.at(0).logicalToLlvm.at(op.fields.at(0));
                bodies << "  " << result << " = extractvalue " << first() << ", " << phys << "\n";
            }
            break;
        }
        case Op::Tag: {
            const auto& lay = layoutOf(op.inputs.at(0).type);
            bodies << "  " << result << ".tag = extractvalue " << first() << ", 0\n"
                   << "  " << result << " = zext " << lay.tagTy << " " << result << ".tag to i64\n";
            break;
        }
        case Op::Negate:
            if (program.isSimd(op.result.type)) {
                const Type elemT = program.simdElem(op.result.type);
                if (Program::isFloatType(elemT)) {
                    bodies << "  " << result << " = fneg " << first() << "\n";
                } else {
                    bodies << "  " << result << " = sub " << type << " zeroinitializer, " << reg(op.inputs.at(0)) << "\n";
                }
                break;
            }
            if (Program::isFloatType(op.result.type)) {
                bodies << "  " << result << " = fneg " << first() << "\n";
                break;
            }
            if (op.result.type != Type::Int) {
                const std::string ity = llvmType(op.result.type);
                const std::string intr = "llvm.ssub.with.overflow." + ity;
                usedDecls.insert(intr);
                bodies << "  " << result << ".st = call {" << ity << ", i1} @" << intr << "(" << ity << " 0, " << first() << ")\n"
                       << "  " << result << " = extractvalue {" << ity << ", i1} " << result << ".st, 0\n"
                       << "  " << result << ".ov = extractvalue {" << ity << ", i1} " << result << ".st, 1\n"
                       << "  call void @tz.guard(i1 " << result << ".ov, " << diagnostic("R_OVERFLOW", op.span) << ")\n";
                break;
            }
            if (op.overflowProven) {
                if (op.width < 64) {
                    const std::string wty = "i" + std::to_string(static_cast<unsigned>(op.width));
                    bodies << "  " << result << ".l = trunc i64 " << reg(op.inputs.at(0)) << " to " << wty << "\n"
                           << "  " << result << ".w = sub nsw " << wty << " 0, " << result << ".l\n"
                           << "  " << result << " = sext " << wty << " " << result << ".w to i64\n";
                } else {
                    bodies << "  " << result << " = sub nsw i64 0, " << reg(op.inputs.at(0)) << "\n";
                }
            } else {
                bodies << "  " << result << " = call i64 @tz.sub(i64 0, " << first() << ", "
                       << diagnostic("R_OVERFLOW", op.span) << ")\n";
            }
            break;
        case Op::Min:
        case Op::Max: {
            const bool isUnsigned = op.width < 64 && op.elemUnsigned;
            const std::string intrStem = (op.op == Op::Min)
                ? (isUnsigned ? "llvm.umin" : "llvm.smin")
                : (isUnsigned ? "llvm.umax" : "llvm.smax");
            if (op.width < 64) {
                const std::string wty = "i" + std::to_string(static_cast<unsigned>(op.width));
                if (isUnsigned || op.width == 8) {
                    usedDecls.insert(intrStem + "." + wty);
                }
                bodies << "  " << result << ".l = trunc i64 " << reg(op.inputs.at(0)) << " to " << wty << "\n"
                       << "  " << result << ".r = trunc i64 " << reg(op.inputs.at(1)) << " to " << wty << "\n"
                       << "  " << result << ".w = call " << wty << " @" << intrStem << "." << wty
                       << "(" << wty << " " << result << ".l, " << wty << " " << result << ".r)\n"
                       << "  " << result << " = " << (isUnsigned ? "zext " : "sext ") << wty << " " << result << ".w to i64\n";
            } else {
                bodies << "  " << result << " = call i64 @" << intrStem << ".i64(" << first() << ", "
                       << operand(op.inputs.at(1)) << ")\n";
            }
            break;
        }
        case Op::AbsDiff: {
            const auto a = reg(op.inputs.at(0));
            const auto b = reg(op.inputs.at(1));
            if (op.overflowProven && op.width < 64) {
                const std::string wty = "i" + std::to_string(static_cast<unsigned>(op.width));
                usedDecls.insert("llvm.abs." + wty);
                bodies << "  " << result << ".l = trunc i64 " << a << " to " << wty << "\n"
                       << "  " << result << ".r = trunc i64 " << b << " to " << wty << "\n"
                       << "  " << result << ".d = sub nsw " << wty << " " << result << ".l, " << result << ".r\n"
                       << "  " << result << ".w = call " << wty << " @llvm.abs." << wty << "("
                       << wty << " " << result << ".d, i1 true)\n"
                       << "  " << result << " = sext " << wty << " " << result << ".w to i64\n";
            } else {
                bodies << "  " << result << ".cmp = icmp sge i64 " << a << ", " << b << "\n"
                       << "  " << result << ".hi = select i1 " << result << ".cmp, i64 " << a << ", i64 " << b << "\n"
                       << "  " << result << ".lo = select i1 " << result << ".cmp, i64 " << b << ", i64 " << a << "\n";
                if (op.overflowProven) {
                    bodies << "  " << result << " = sub nsw i64 " << result << ".hi, " << result << ".lo\n";
                } else {
                    bodies << "  " << result << " = sub i64 " << result << ".hi, " << result << ".lo\n"
                           << "  " << result << ".bad = icmp slt i64 " << result << ", 0\n"
                           << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_OVERFLOW", op.span) << ")\n";
                }
            }
            break;
        }
        case Op::Select:
            if (op.result.type == Type::Int && op.width < 64) {
                const std::string wty = "i" + std::to_string(static_cast<unsigned>(op.width));
                bodies << "  " << result << ".t = trunc i64 " << reg(op.inputs.at(1)) << " to " << wty << "\n"
                       << "  " << result << ".f = trunc i64 " << reg(op.inputs.at(2)) << " to " << wty << "\n"
                       << "  " << result << ".w = select i1 " << reg(op.inputs.at(0))
                       << ", " << wty << " " << result << ".t, " << wty << " " << result << ".f\n"
                       << "  " << result << " = sext " << wty << " " << result << ".w to i64\n";
            } else {
                bodies << "  " << result << " = select i1 " << reg(op.inputs.at(0))
                       << ", " << operand(op.inputs.at(1))
                       << ", " << operand(op.inputs.at(2)) << "\n";
            }
            break;
        case Op::Not: bodies << "  " << result << " = xor " << first() << ", true\n"; break;
        case Op::BitNot:
            if (program.isSimd(op.result.type)) {
                const Type elemT = program.simdElem(op.result.type);
                const unsigned lanes = program.simdLanes(op.result.type);
                bodies << "  " << result << " = xor " << first() << ", " << simdLlvmSplatConst(elemT, lanes, "-1") << "\n";
            } else {
                bodies << "  " << result << " = xor " << first() << ", -1\n";
            }
            break;
        case Op::Binary: binary(op); break;
        case Op::TileRepeat: {
            usesTileRepeat = true;
            const unsigned elemBytes = op.elemBits > 0 ? std::max(1u, static_cast<unsigned>(op.elemBits) / 8u) : 8u;
            bodies << "  " << result << ".base = extractvalue " << first() << ", 0\n"
                   << "  call void @tz.tile_repeat(ptr " << result << ".base, "
                   << operand(op.inputs.at(1)) << ", " << operand(op.inputs.at(2)) << ", i64 " << elemBytes << ")\n";
            break;
        }
        case Op::FindFirst: {
            const std::uint8_t bits = (op.elemBits == 8 || op.elemBits == 16 || op.elemBits == 32) ? op.elemBits : 64;
            usesFindFirstBits.insert(bits);
            const std::string ety = "i" + std::to_string(static_cast<unsigned>(bits));
            bodies << "  " << result << ".base = extractvalue " << first() << ", 0\n";
            if (bits < 64) {
                bodies << "  " << result << ".tv = trunc i64 " << reg(op.inputs.at(3)) << " to " << ety << "\n"
                       << "  " << result << " = call i64 @tz.find_first.i" << static_cast<unsigned>(bits)
                       << "(ptr " << result << ".base, " << operand(op.inputs.at(1)) << ", "
                       << operand(op.inputs.at(2)) << ", " << ety << " " << result << ".tv)\n";
            } else {
                bodies << "  " << result << " = call i64 @tz.find_first.i64(ptr " << result << ".base, "
                       << operand(op.inputs.at(1)) << ", " << operand(op.inputs.at(2)) << ", "
                       << operand(op.inputs.at(3)) << ")\n";
            }
            break;
        }
        case Op::TruncI32:
            bodies << "  " << result << " = trunc i64 " << reg(op.inputs.at(0)) << " to i32\n";
            break;
        case Op::SextI64:
            bodies << "  " << result << " = sext i32 " << reg(op.inputs.at(0)) << " to i64\n";
            break;
        case Op::GemmF32: {
            usesGemmF32 = true;
            bodies << "  " << result << ".ap = extractvalue " << operand(op.inputs.at(0)) << ", 0\n"
                   << "  " << result << ".bp = extractvalue " << operand(op.inputs.at(1)) << ", 0\n"
                   << "  " << result << ".cp = extractvalue " << operand(op.inputs.at(2)) << ", 0\n"
                   << "  call void @tz.gemm_f32_panel(ptr " << result << ".ap, ptr " << result << ".bp, ptr "
                   << result << ".cp, " << operand(op.inputs.at(3)) << ")\n";
            break;
        }
        case Op::MatMulMod: {
            usesMatMulModU16 = true;
            std::uint64_t bmul = 0;
            unsigned bshift = 0;
            const auto modId = op.inputs.at(3).id;
            if (auto mIt = curConstInts.find(modId); mIt != curConstInts.end() && mIt->second >= 2) {
                const std::uint64_t xMax = op.lhsUpper > 0
                    ? static_cast<std::uint64_t>(op.lhsUpper)
                    : 0x7FFFFFFFULL;
                if (auto bar = findExactBarrett(static_cast<std::uint64_t>(mIt->second), xMax, 32)) {
                    bmul = bar->first;
                    bshift = bar->second;
                }
            }
            bodies << "  " << result << ".ap = extractvalue " << operand(op.inputs.at(0)) << ", 0\n"
                   << "  " << result << ".bp = extractvalue " << operand(op.inputs.at(1)) << ", 0\n"
                   << "  " << result << ".m32 = trunc i64 " << reg(op.inputs.at(3)) << " to i32\n"
                   << "  " << result << " = call i64 @tz.matmul_mod.u16(ptr " << result << ".ap, ptr "
                   << result << ".bp, " << operand(op.inputs.at(2)) << ", i32 " << result << ".m32, i64 "
                   << bmul << ", i64 " << bshift << ")\n";
            break;
        }
        case Op::FloydStepU16: {
            usesFloydStepU16 = true;
            bodies << "  " << result << ".dp = extractvalue " << operand(op.inputs.at(0)) << ", 0\n"
                   << "  call void @tz.floyd_step_u16(ptr " << result << ".dp, "
                   << operand(op.inputs.at(1)) << ", " << operand(op.inputs.at(2)) << ")\n";
            break;
        }
        case Op::ArgMinU16: {
            usesArgMinU16 = true;
            bodies << "  " << result << ".ap = extractvalue " << operand(op.inputs.at(0)) << ", 0\n"
                   << "  " << result << " = call i64 @tz.argmin.u16(ptr " << result << ".ap, "
                   << operand(op.inputs.at(1)) << ", " << operand(op.inputs.at(2)) << ", "
                   << operand(op.inputs.at(3)) << ")\n";
            break;
        }
        case Op::Assert:
            if (op.boundsProven || op.unchecked) break;
            bodies << "  " << result << ".bad = xor " << first() << ", true\n"
                   << "  call void @tz.guard(i1 " << result << ".bad, "
                   << diagnostic((op.data == "requires" || op.data == "ensures") ? "R_CONTRACT" : "R_ASSERT", op.span) << ")\n";
            break;
        case Op::Print:
            switch (op.inputs[0].type) {
            case Type::Int: bodies << "  call void @tz.printInt(" << first() << ")\n"; break;
            case Type::I8:
            case Type::I16:
            case Type::I32:
                bodies << "  " << result << ".ext = sext " << first() << " to i64\n"
                       << "  call void @tz.printInt(i64 " << result << ".ext)\n";
                break;
            case Type::U8:
            case Type::U16:
            case Type::U32:
                bodies << "  " << result << ".ext = zext " << first() << " to i64\n"
                       << "  call void @tz.printInt(i64 " << result << ".ext)\n";
                break;
            case Type::U64:
                usedDecls.insert("tz_sys_print_u64");
                bodies << "  call void @tz_sys_print_u64(" << first() << ")\n";
                return;
            case Type::F32:
                usedDecls.insert("tz_sys_print_float");
                bodies << "  " << result << ".d = fpext " << first() << " to double\n"
                       << "  call void @tz_sys_print_float(double " << result << ".d)\n";
                return;
            case Type::Float:
                usedDecls.insert("tz_sys_print_float");
                bodies << "  call void @tz_sys_print_float(" << first() << ")\n";
                return;
            case Type::Bool:
                bodies << "  " << result << ".text = select " << first() << ", " << literal("true")
                       << ", " << literal("false") << "\n"
                       << "  call void @tz.write(%text " << result << ".text, i1 false)\n";
                break;
            case Type::Str: bodies << "  call void @tz.write(" << first() << ", i1 false)\n"; break;
            default: throw std::logic_error("unprintable checked type");
            }
            bodies << "  call i32 @putchar(i32 10)\n";
            break;
        }
    }
    void writeBlock(unsigned i, const Block& block) {
        bodies << "b" << i << ":\n";
        for (const auto& op : block.ops) operation(op);
        auto emitLoopMeta = [&]() -> std::string {
            if (block.loopHint == LoopHint::None && block.unrollCount <= 1) return {};
            std::vector<unsigned> props;
            if (block.loopHint == LoopHint::FullUnroll) {
                const unsigned pUnroll = nextMetaId++;
                metadata.push_back("!" + std::to_string(pUnroll) + " = !{!\"llvm.loop.unroll.full\"}");
                props.push_back(pUnroll);
            } else if (block.loopHint != LoopHint::None) {
                if (block.loopHint == LoopHint::VectorizeParallel && block.loopAccessGroup >= 0) {
                    if (auto agIt = curAccessGroupMeta.find(block.loopAccessGroup); agIt != curAccessGroupMeta.end()) {
                        const unsigned pPar = nextMetaId++;
                        metadata.push_back("!" + std::to_string(pPar) +
                                           " = !{!\"llvm.loop.parallel_accesses\", !" + std::to_string(agIt->second) + "}");
                        props.push_back(pPar);
                    }
                }
                const unsigned pVec = nextMetaId++;
                metadata.push_back("!" + std::to_string(pVec) + " = !{!\"llvm.loop.vectorize.enable\", i1 true}");
                props.push_back(pVec);
                const unsigned pIlv = nextMetaId++;
                if (block.noInterleave) {
                    metadata.push_back("!" + std::to_string(pIlv) + " = !{!\"llvm.loop.interleave.count\", i32 1}");
                } else {
                    metadata.push_back("!" + std::to_string(pIlv) + " = !{!\"llvm.loop.interleave.enable\", i1 true}");
                }
                props.push_back(pIlv);
            } else if (block.unrollCount > 1) {
                const unsigned pCnt = nextMetaId++;
                metadata.push_back("!" + std::to_string(pCnt) +
                                   " = !{!\"llvm.loop.unroll.count\", i32 " +
                                   std::to_string(static_cast<unsigned>(block.unrollCount)) + "}");
                props.push_back(pCnt);
            }
            const unsigned loopId = nextMetaId++;
            std::string decl = "!" + std::to_string(loopId) + " = distinct !{!" + std::to_string(loopId);
            for (unsigned p : props) decl += ", !" + std::to_string(p);
            decl += "}";
            metadata.push_back(std::move(decl));
            return ", !llvm.loop !" + std::to_string(loopId);
        };
        switch (block.exit) {
        case Exit::Return:
            if (emitsRegion) {
                if (!curPureStackArena) bodies << "  call void @tz.finish(ptr %arena)\n";
                if (emitsStackArena)
#if defined(VELOZ_CLANG_MAJOR) && VELOZ_CLANG_MAJOR < 22
                    bodies << "  call void @llvm.lifetime.end.p0(i64 -1, ptr %arenaStack)\n";
#else
                    bodies << "  call void @llvm.lifetime.end.p0(ptr %arenaStack)\n";
#endif
            }
            bodies << "  ret " << (block.value ? operand(*block.value) : "void") << "\n";
            break;
        case Exit::Jump: bodies << "  br label %b" << block.yes << emitLoopMeta() << "\n"; break;
        case Exit::Branch: {
            std::string profSuffix;
            if (block.coldBranchYes) {
                if (coldBranchProfId == 0) {
                    coldBranchProfId = nextMetaId++;
                    metadata.push_back("!" + std::to_string(coldBranchProfId) +
                                       " = !{!\"branch_weights\", i32 1, i32 64}");
                }
                profSuffix = ", !prof !" + std::to_string(coldBranchProfId);
            }
            bodies << "  br " << operand(*block.value) << ", label %b" << block.yes << ", label %b" << block.no
                   << emitLoopMeta() << profSuffix << "\n";
            break;
        }
        case Exit::Switch:
            bodies << "  switch " << operand(*block.value) << ", label %b" << block.yes << " [\n";
            for (unsigned tag = 0; tag < block.targets.size(); ++tag) {
                bodies << "    i64 " << tag << ", label %b" << block.targets[tag] << "\n";
            }
            bodies << "  ]\n";
            break;
        case Exit::Unreachable: bodies << "  unreachable\n"; break;
        case Exit::Open: throw std::logic_error("unterminated Turi IR block");
        }
    }
public:
    explicit ModuleWriter(const Program& program, bool audit) : program(program), auditCopies(audit) {}
    static std::string ffiAttr(Type t) {
        if (t == Type::I8 || t == Type::I16) return "signext ";
        if (t == Type::U8 || t == Type::U16 || t == Type::Bool) return "zeroext ";
        return {};
    }
    static std::string ffiParamAttr(Type t) {
        if (t == Type::I8 || t == Type::I16) return " signext";
        if (t == Type::U8 || t == Type::U16 || t == Type::Bool) return " zeroext";
        return {};
    }
    void trampoline(const FunctionIR& ir, const std::string& fastCallee, const std::string& slowCallee) {
        const auto& f = ir.source;
        bodies << "define internal " << llvmType(f.result) << " " << symbol(f.name) << "(i32 %depth";
        for (unsigned i = 0; i < f.params.size(); ++i) bodies << ", " << llvmType(f.params[i].type) << " %arg" << i;
        bodies << ") sanitize_address {\nentry:\n";
        std::string accReg = "%arg0";
        for (unsigned i = 1; i < f.params.size(); ++i) {
            std::string nextReg = "%fast.or" + std::to_string(i);
            bodies << "  " << nextReg << " = or i64 " << accReg << ", %arg" << i << "\n";
            accReg = nextReg;
        }
        bodies << "  %fast.ok = icmp ule i64 " << accReg << ", 1048575\n"
               << "  br i1 %fast.ok, label %fast.call, label %slow.call\n"
               << "fast.call:\n  ";
        if (f.result != Type::Void) bodies << "%fast.res = ";
        bodies << "tail call " << llvmType(f.result) << " " << symbol(fastCallee) << "(i32 %depth";
        for (unsigned i = 0; i < f.params.size(); ++i)
            bodies << ", " << llvmType(f.params[i].type) << " %arg" << i;
        bodies << ")\n";
        if (f.result != Type::Void) bodies << "  ret " << llvmType(f.result) << " %fast.res\n";
        else bodies << "  ret void\n";
        bodies << "slow.call:\n  ";
        if (f.result != Type::Void) bodies << "%slow.res = ";
        bodies << "tail call " << llvmType(f.result) << " " << symbol(slowCallee) << "(i32 %depth";
        for (unsigned i = 0; i < f.params.size(); ++i)
            bodies << ", " << llvmType(f.params[i].type) << " %arg" << i;
        bodies << ")\n";
        if (f.result != Type::Void) bodies << "  ret " << llvmType(f.result) << " %slow.res\n";
        else bodies << "  ret void\n";
        bodies << "}\n";
    }
    void function(const FunctionIR& ir, bool depthKnownSafe, const std::string& overrideName = "",
                  bool coldNoInline = false, bool noInline = false, bool fastParamRange = false) {
        const auto& f = ir.source;
        const std::string& fnName = overrideName.empty() ? f.name : overrideName;
        checkDepth = !depthKnownSafe;
        emitsRegion = ir.usesRegion;
        emitsStackArena = ir.usesRegion && ir.canStackArena;
        curPureStackArena = false;
        curSlotByRef = ir.slotByRef;
        curScopeMeta.clear();
        curNoAliasMeta.clear();
        curAccessGroupMeta.clear();
        for (const auto& blk : ir.blocks) {
            auto addAg = [&](int ag) {
                if (ag >= 0 && !curAccessGroupMeta.contains(ag)) {
                    const unsigned id = nextMetaId++;
                    curAccessGroupMeta[ag] = id;
                    metadata.push_back("!" + std::to_string(id) + " = distinct !{}");
                }
            };
            addAg(blk.loopAccessGroup);
            for (const auto& op : blk.ops) addAg(op.loopAccessGroup);
        }
        int maxGroup = -1;
        for (int g : ir.slotAliasGroup) maxGroup = std::max(maxGroup, g);
        if (maxGroup >= 1) {
            const int count = maxGroup + 1;
            const unsigned domain = nextMetaId++;
            metadata.push_back("!" + std::to_string(domain) + " = distinct !{!" + std::to_string(domain) + ", !\"turi.domain\"}");
            std::vector<unsigned> scopes(count);
            for (int g = 0; g < count; ++g) {
                scopes[g] = nextMetaId++;
                metadata.push_back("!" + std::to_string(scopes[g]) + " = distinct !{!" + std::to_string(scopes[g]) +
                                   ", !" + std::to_string(domain) + ", !\"turi.arr." + std::to_string(g) + "\"}");
            }
            for (int g = 0; g < count; ++g) {
                const unsigned sList = nextMetaId++;
                metadata.push_back("!" + std::to_string(sList) + " = !{!" + std::to_string(scopes[g]) + "}");
                curScopeMeta.push_back(sList);
                const unsigned nList = nextMetaId++;
                std::string list = "!" + std::to_string(nList) + " = !{";
                bool firstItem = true;
                for (int h = 0; h < count; ++h) {
                    if (h == g) continue;
                    if (!firstItem) list += ", ";
                    firstItem = false;
                    list += "!" + std::to_string(scopes[h]);
                }
                list += "}";
                metadata.push_back(std::move(list));
                curNoAliasMeta.push_back(nList);
            }
        }
        if (f.isExternC && overrideName.empty()) {
            bodies << "define " << ffiAttr(f.result) << llvmType(f.result) << " @" << f.cSymbol << "(";
            for (unsigned i = 0; i < f.params.size(); ++i) {
                if (i > 0) bodies << ", ";
                bodies << llvmType(f.params[i].type) << ffiParamAttr(f.params[i].type) << " %arg" << i;
            }
            bodies << ") sanitize_address {\nentry:\n  %atLimit = icmp uge i32 0, 256\n  %childDepth = add i32 0, 1\n";
        } else {
            bodies << "define internal " << llvmType(f.result) << " " << symbol(fnName) << "(i32 %depth";
            for (unsigned i = 0; i < f.params.size(); ++i) {
                if (f.params[i].byRef) {
                    bodies << ", ptr noalias %arg" << i;
                } else {
                    bodies << ", " << llvmType(f.params[i].type);
                    if (fastParamRange && f.params[i].type == Type::Int) bodies << " range(i64 0, 1048576)";
                    bodies << " %arg" << i;
                }
            }
            bodies << ")";
            if (coldNoInline) bodies << " cold noinline";
            else if (noInline) bodies << " noinline";
            bodies << " sanitize_address {\nentry:\n  %atLimit = icmp uge i32 %depth, 256\n"
                   << "  %childDepth = add i32 %depth, 1\n";
        }
        curSlotTypes = ir.slots;
        curSlotByRef = ir.slotByRef;
        curConstInts.clear();
        for (const auto& blk : ir.blocks) {
            for (const auto& op : blk.ops) {
                if (op.op == Op::Int && op.result.type == Type::Int) {
                    std::int64_t cv = 0;
                    const char* beg = op.data.data();
                    const char* end = beg + op.data.size();
                    const auto res = std::from_chars(beg, end, cv);
                    if (res.ec == std::errc{} && res.ptr == end) curConstInts[op.result.id] = cv;
                }
            }
        }
        std::size_t stackArenaBytes = 32768;
        if (ir.usesRegion && ir.canStackArena) {
            std::unordered_map<unsigned, std::int64_t> entryInts;
            for (const auto& op : ir.blocks.front().ops) {
                if (op.op == Op::Int && op.result.type != Type::Void) {
                    try { entryInts[op.result.id] = std::stoll(op.data); } catch (...) {}
                }
            }
            bool exactSmall = true;
            std::uint64_t totalBytes = 0;
            for (std::size_t bi = 0; bi < ir.blocks.size() && exactSmall; ++bi) {
                for (const auto& op : ir.blocks[bi].ops) {
                    if (op.op == Op::Clone || op.op == Op::Slice || op.op == Op::Interp || op.op == Op::Builtin) {
                        exactSmall = false;
                        break;
                    }
                    if (op.op == Op::ArrayLit) {
                        if (bi != 0) { exactSmall = false; break; }
                        std::size_t elemSz = 8;
                        if (op.result.type == Type::IntArray) elemSz = op.elemBits ? static_cast<std::size_t>(op.elemBits) / 8 : 8;
                        else if (program.isArray(op.result.type)) elemSz = sizeAlignOf(elemOf(op.result.type)).first;
                        totalBytes += static_cast<std::uint64_t>(op.inputs.size()) * elemSz + 32;
                    } else if (op.op == Op::RepeatRegion) {
                        if (bi != 0 || op.inputs.size() < 2) { exactSmall = false; break; }
                        auto it = entryInts.find(op.inputs[1].id);
                        if (it == entryInts.end() || it->second < 0 || it->second > 2048) {
                            exactSmall = false;
                            break;
                        }
                        std::size_t elemSz = 8;
                        if (op.result.type == Type::IntArray) elemSz = op.elemBits ? static_cast<std::size_t>(op.elemBits) / 8 : 8;
                        else if (program.isArray(op.result.type)) elemSz = sizeAlignOf(elemOf(op.result.type)).first;
                        totalBytes += static_cast<std::uint64_t>(it->second) * elemSz + 32;
                    }
                }
            }
            if (exactSmall && totalBytes <= 2048) {
                stackArenaBytes = std::max<std::size_t>(128, ((static_cast<std::size_t>(totalBytes) + 63) / 64) * 64);
                bool b0HasPred = false;
                for (const auto& blk : ir.blocks) {
                    if ((blk.exit == Exit::Jump && blk.yes == 0) ||
                        (blk.exit == Exit::Branch && (blk.yes == 0 || blk.no == 0)) ||
                        (blk.exit == Exit::Switch &&
                         (blk.yes == 0 || std::find(blk.targets.begin(), blk.targets.end(), 0u) != blk.targets.end()))) {
                        b0HasPred = true;
                        break;
                    }
                }
                std::unordered_set<unsigned> markIds;
                for (const auto& blk : ir.blocks) {
                    for (const auto& op : blk.ops) {
                        if (op.op == Op::RegionMark && op.result.type != Type::Void)
                            markIds.insert(op.result.id);
                    }
                }
                bool pureMarks = !b0HasPred;
                if (pureMarks && !markIds.empty()) {
                    for (const auto& blk : ir.blocks) {
                        if (blk.value && markIds.contains(blk.value->id)) { pureMarks = false; break; }
                        for (const auto& op : blk.ops) {
                            if (op.op == Op::RegionRewind) continue;
                            for (const auto& in : op.inputs) {
                                if (markIds.contains(in.id)) { pureMarks = false; break; }
                            }
                            if (!pureMarks) break;
                        }
                        if (!pureMarks) break;
                    }
                }
                if (pureMarks) curPureStackArena = true;
            }
            bodies << "  %arenaStack = alloca { %chunk, [" << stackArenaBytes << " x i8] }, align 32\n";
        }
        if (ir.usesRegion) {
            bodies << "  %arena = alloca ptr\n";
        }
        for (unsigned i = 0; i < ir.slots.size(); ++i) {
            if (i < ir.slotByRef.size() && ir.slotByRef[i]) continue;
            bodies << "  %slot" << i << " = alloca " << llvmType(ir.slots[i]) << allocaAlignSuffix(ir.slots[i]) << "\n";
        }
        bool needsSysSliceTmp = false;
        bool needsTextTmp0 = false;
        bool needsTextTmp1 = false;
        bool needsFloatBuf = false;
        bool needsFloatOut = false;
        bool needsInterpSb = false;
        std::set<Type> usedMultiEnums;
        std::vector<std::tuple<unsigned, std::size_t, std::size_t>> bitcastTmps;
        for (const auto& blk : ir.blocks) {
            for (const auto& op : blk.ops) {
                if (op.op == Op::Construct && !op.inputs.empty() &&
                    !program.isArray(op.result.type) && op.result.type >= Type::NamedBase &&
                    layoutOf(op.result.type).multiPayload) {
                    usedMultiEnums.insert(op.result.type);
                } else if (op.op == Op::Field && !op.inputs.empty() &&
                           !program.isArray(op.inputs[0].type) && op.inputs[0].type >= Type::NamedBase &&
                           layoutOf(op.inputs[0].type).multiPayload) {
                    usedMultiEnums.insert(op.inputs[0].type);
                } else if (op.op == Op::Interp) {
                    needsInterpSb = true;
                    for (const auto& pv : op.inputs) {
                        if (Program::isFloatType(pv.type) || pv.type == Type::U64) needsFloatBuf = true;
                    }
                } else if (op.op == Op::Builtin) {
                    if (op.data == "argAt") {
                        needsSysSliceTmp = true;
                    } else if (op.data == "readFile" || op.data == "envGet" || op.data == "execCapture") {
                        needsSysSliceTmp = true;
                        needsTextTmp0 = true;
                    } else if (op.data == "tryReadFile" ||
                               op.data == "execStatus" || op.data == "fileExists" ||
                               op.data == "removeFile" || op.data == "makeDir" ||
                               op.data == "fileWrite" || op.data == "tcpListen" ||
                               op.data == "tcpConnect" || op.data == "socketWrite" ||
                               op.data == "cstr") {
                        needsTextTmp0 = true;
                    } else if (op.data == "writeFile" || op.data == "appendFile" ||
                               op.data == "tryWriteFile" || op.data == "fileOpen") {
                        needsTextTmp0 = true;
                        needsTextTmp1 = true;
                    } else if (op.data == "strFromFloat" || op.data == "sbPushFloat" ||
                               (op.data == "strFromInt" && !op.inputs.empty() && op.inputs[0].type == Type::U64)) {
                        needsFloatBuf = true;
                    } else if (op.data == "parseFloat" || op.data == "tryParseFloat") {
                        needsTextTmp0 = true;
                        needsFloatOut = true;
                    } else if (op.data == "bitcast" && !op.inputs.empty()) {
                        const Type ft = op.inputs[0].type, tt = op.result.type;
                        const auto [sz, alF] = sizeAlignOf(ft);
                        const auto alT = sizeAlignOf(tt).second;
                        if (sz > 0 && (!Program::isNumericType(ft) || !Program::isNumericType(tt))) {
                            bitcastTmps.emplace_back(op.result.id, sz, std::max(alF, alT));
                        }
                    }
                }
            }
        }
        if (needsSysSliceTmp) bodies << "  %sys.slice.tmp = alloca { ptr, i64 }\n";
        if (needsTextTmp0) bodies << "  %text.tmp0 = alloca %text\n";
        if (needsTextTmp1) bodies << "  %text.tmp1 = alloca %text\n";
        if (needsFloatBuf) bodies << "  %float.buf = alloca [64 x i8]\n";
        if (needsFloatOut) bodies << "  %float.out = alloca double\n";
        if (needsInterpSb) bodies << "  %interp.sb = alloca %array\n";
        for (Type et : usedMultiEnums) {
            bodies << "  %enum.tmp." << static_cast<int>(et) << " = alloca " << llvmType(et)
                   << allocaAlignSuffix(et) << "\n";
        }
        for (const auto& [rid, sz, al] : bitcastTmps) {
            bodies << "  %bc.tmp." << rid << " = alloca [" << sz << " x i8], align " << al << "\n";
        }
        if (ir.usesRegion) {
            if (ir.canStackArena) {
#if defined(VELOZ_CLANG_MAJOR) && VELOZ_CLANG_MAJOR < 22
                bodies << "  call void @llvm.lifetime.start.p0(i64 -1, ptr %arenaStack)\n"
#else
                bodies << "  call void @llvm.lifetime.start.p0(ptr %arenaStack)\n"
#endif
                       << "  store %chunk { ptr null, i64 " << stackArenaBytes << ", i64 0, i64 0 }, ptr %arenaStack\n"
                       << "  store ptr %arenaStack, ptr %arena\n";
            } else {
                bodies << "  store ptr null, ptr %arena\n";
            }
        }
        for (unsigned i = 0; i < ir.slots.size(); ++i) {
            if (i < ir.slotByRef.size() && ir.slotByRef[i]) continue;
            if (needsDrop(ir.slots[i]))
                bodies << "  store " << llvmType(ir.slots[i]) << " zeroinitializer, ptr %slot" << i << "\n";
        }
        for (unsigned i = 0; i < f.params.size(); ++i) {
            if (f.params[i].byRef) continue;
            bodies << "  store " << llvmType(f.params[i].type) << " %arg" << i << ", ptr %slot" << i << "\n";
        }
        struct OutlinedColdBlock {
            std::string helperName;
            std::vector<unsigned> readOnly;
            std::vector<bool> roPassPtr;
            std::vector<unsigned> mutOutside;
            unsigned yesTarget = 0;
        };
        std::unordered_map<unsigned, OutlinedColdBlock> outlinedCold;
        {
            std::vector<unsigned> predCount(ir.blocks.size(), 0);
            for (const auto& b : ir.blocks) {
                if (b.exit == Exit::Jump && b.yes < ir.blocks.size()) ++predCount[b.yes];
                else if (b.exit == Exit::Branch) {
                    if (b.yes < ir.blocks.size()) ++predCount[b.yes];
                    if (b.no < ir.blocks.size()) ++predCount[b.no];
                } else if (b.exit == Exit::Switch) {
                    if (b.yes < ir.blocks.size()) ++predCount[b.yes];
                    for (unsigned t : b.targets)
                        if (t < ir.blocks.size()) ++predCount[t];
                }
            }
            for (unsigned bi = 0; bi < ir.blocks.size(); ++bi) {
                if (!ir.blocks[bi].coldBranchYes) continue;
                const unsigned tb = ir.blocks[bi].yes;
                if (tb == 0 || tb >= ir.blocks.size() || outlinedCold.contains(tb) || predCount[tb] != 1) continue;
                const auto& tblk = ir.blocks[tb];
                if (tblk.exit != Exit::Jump || tblk.ops.size() < 40) continue;

                std::unordered_set<unsigned> defInTb;
                bool ok = true;
                for (const auto& op : tblk.ops) {
                    switch (op.op) {
                    case Op::Load:
                    case Op::Store:
                    case Op::Index:
                    case Op::IndexSet:
                    case Op::Int:
                    case Op::Bool:
                    case Op::Float:
                    case Op::Binary:
                    case Op::Min:
                    case Op::Max:
                    case Op::Select:
                    case Op::Negate:
                    case Op::AbsDiff:
                    case Op::Not:
                    case Op::BitNot:
                    case Op::TruncI32:
                    case Op::SextI64:
                    case Op::Assert:
                    case Op::Length:
                        break;
                    default:
                        ok = false;
                        break;
                    }
                    if (!ok) break;
                    for (const Value& in : op.inputs) {
                        if (!defInTb.contains(in.id)) { ok = false; break; }
                    }
                    if (!ok) break;
                    if (op.result.type != Type::Void) defInTb.insert(op.result.id);
                }
                if (!ok) continue;

                std::set<unsigned> usedInTb, storedInTb, usedOutsideTb;
                for (const auto& op : tblk.ops) {
                    if (op.op == Op::Load) usedInTb.insert(op.slot);
                    else if (op.op == Op::Store) {
                        usedInTb.insert(op.slot);
                        storedInTb.insert(op.slot);
                    }
                }
                for (unsigned bj = 0; bj < ir.blocks.size(); ++bj) {
                    if (bj == tb) continue;
                    for (const auto& op : ir.blocks[bj].ops) {
                        if (op.op == Op::Load || op.op == Op::Store || op.op == Op::Drop ||
                            op.op == Op::ClearOwner || op.op == Op::ClearField) {
                            usedOutsideTb.insert(op.slot);
                        }
                    }
                }
                for (std::size_t pi = 0; pi < f.params.size(); ++pi) usedOutsideTb.insert(static_cast<unsigned>(pi));

                std::vector<unsigned> localOnly, readOnly, mutOutside;
                for (unsigned s : usedInTb) {
                    if (s >= ir.slots.size() || (s < ir.slotByRef.size() && ir.slotByRef[s])) {
                        ok = false;
                        break;
                    }
                    const Type st = ir.slots[s];
                    if (!Program::isNumericType(st) && st != Type::Bool && !program.isArray(st)) {
                        ok = false;
                        break;
                    }
                    if (!usedOutsideTb.contains(s)) localOnly.push_back(s);
                    else if (!storedInTb.contains(s)) readOnly.push_back(s);
                    else mutOutside.push_back(s);
                }
                if (!ok || readOnly.size() + mutOutside.size() > 8) continue;

                std::vector<bool> roPassPtr(readOnly.size(), false);
                for (std::size_t k = 0; k < readOnly.size(); ++k) {
                    const unsigned s = readOnly[k];
                    if (!program.isArray(ir.slots[s])) continue;
                    std::unordered_set<unsigned> loadIds;
                    for (const auto& op : tblk.ops) {
                        if (op.op == Op::Load && op.slot == s && op.result.type != Type::Void) {
                            loadIds.insert(op.result.id);
                        }
                    }
                    bool ptrOnly = !loadIds.empty();
                    for (const auto& op : tblk.ops) {
                        if ((op.op == Op::Index || op.op == Op::IndexSet) && op.boundsProven && op.inputs.size() >= 2) {
                            for (std::size_t ii = 1; ii < op.inputs.size(); ++ii) {
                                if (loadIds.contains(op.inputs[ii].id)) ptrOnly = false;
                            }
                        } else {
                            for (const Value& in : op.inputs) {
                                if (loadIds.contains(in.id)) ptrOnly = false;
                            }
                        }
                    }
                    roPassPtr[k] = ptrOnly;
                }

                const std::string helperName = fnName + ".cold.b" + std::to_string(tb);
                std::ostringstream helperBuf;
                helperBuf << "define internal void " << symbol(helperName) << "(";
                bool firstParam = true;
                for (unsigned s : mutOutside) {
                    if (!firstParam) helperBuf << ", ";
                    firstParam = false;
                    helperBuf << "ptr %slot" << s;
                }
                for (std::size_t k = 0; k < readOnly.size(); ++k) {
                    if (!firstParam) helperBuf << ", ";
                    firstParam = false;
                    const unsigned s = readOnly[k];
                    helperBuf << (roPassPtr[k] ? "ptr" : llvmType(ir.slots[s])) << " %carg" << k;
                }
                helperBuf << ") cold noinline sanitize_address {\nentry:\n";
                for (std::size_t k = 0; k < readOnly.size(); ++k) {
                    const unsigned s = readOnly[k];
                    helperBuf << "  %slot" << s << " = alloca " << llvmType(ir.slots[s])
                              << allocaAlignSuffix(ir.slots[s]) << "\n";
                    if (roPassPtr[k]) {
                        helperBuf << "  %carg" << k << ".arr = insertvalue %array zeroinitializer, ptr %carg" << k << ", 0\n"
                                  << "  store %array %carg" << k << ".arr, ptr %slot" << s << "\n";
                    } else {
                        helperBuf << "  store " << llvmType(ir.slots[s]) << " %carg" << k << ", ptr %slot" << s << "\n";
                    }
                }
                for (unsigned s : localOnly) {
                    helperBuf << "  %slot" << s << " = alloca " << llvmType(ir.slots[s])
                              << allocaAlignSuffix(ir.slots[s]) << "\n";
                }
                helperBuf << "  br label %b" << tb << "\nb" << tb << ":\n";

                const auto savedScopeMeta = std::move(curScopeMeta);
                const auto savedNoAliasMeta = std::move(curNoAliasMeta);
                const auto savedAgMeta = std::move(curAccessGroupMeta);
                const bool savedEmitsRegion = emitsRegion;
                const bool savedEmitsStack = emitsStackArena;
                const bool savedPureStack = curPureStackArena;
                const bool savedCheckDepth = checkDepth;
                curScopeMeta.clear();
                curNoAliasMeta.clear();
                curAccessGroupMeta.clear();
                emitsRegion = false;
                emitsStackArena = false;
                curPureStackArena = false;
                checkDepth = false;

                std::swap(bodies, helperBuf);
                for (const auto& op : tblk.ops) operation(op);
                std::swap(bodies, helperBuf);

                curScopeMeta = std::move(savedScopeMeta);
                curNoAliasMeta = std::move(savedNoAliasMeta);
                curAccessGroupMeta = std::move(savedAgMeta);
                emitsRegion = savedEmitsRegion;
                emitsStackArena = savedEmitsStack;
                curPureStackArena = savedPureStack;
                checkDepth = savedCheckDepth;

                helperBuf << "  ret void\n}\n";
                coldOutlines << helperBuf.str();
                outlinedCold.emplace(tb, OutlinedColdBlock{
                    helperName, std::move(readOnly), std::move(roPassPtr), std::move(mutOutside), tblk.yes});
            }
        }
        bodies << "  br label %b0\n";
        for (unsigned i = 0; i < ir.blocks.size(); ++i) {
            if (auto oit = outlinedCold.find(i); oit != outlinedCold.end()) {
                const auto& oc = oit->second;
                bodies << "b" << i << ":\n";
                std::vector<std::string> roRegs;
                roRegs.reserve(oc.readOnly.size());
                for (std::size_t k = 0; k < oc.readOnly.size(); ++k) {
                    const unsigned s = oc.readOnly[k];
                    const std::string r = "%cold.b" + std::to_string(i) + ".r" + std::to_string(k);
                    if (oc.roPassPtr[k]) {
                        const std::string arrR = "%cold.b" + std::to_string(i) + ".a" + std::to_string(k);
                        bodies << "  " << arrR << " = load %array, ptr " << slotPtr(s) << "\n"
                               << "  " << r << " = extractvalue %array " << arrR << ", 0\n";
                    } else {
                        bodies << "  " << r << " = load " << llvmType(ir.slots[s]) << ", ptr " << slotPtr(s) << "\n";
                    }
                    roRegs.push_back(r);
                }
                bodies << "  call void " << symbol(oc.helperName) << "(";
                bool firstArg = true;
                for (unsigned s : oc.mutOutside) {
                    if (!firstArg) bodies << ", ";
                    firstArg = false;
                    bodies << "ptr " << slotPtr(s);
                }
                for (std::size_t k = 0; k < oc.readOnly.size(); ++k) {
                    if (!firstArg) bodies << ", ";
                    firstArg = false;
                    const unsigned s = oc.readOnly[k];
                    bodies << (oc.roPassPtr[k] ? "ptr" : llvmType(ir.slots[s])) << " " << roRegs[k];
                }
                bodies << ")\n"
                       << "  br label %b" << oc.yesTarget << "\n";
            } else {
                writeBlock(i, ir.blocks[i]);
            }
        }
        bodies << "}\n";
    }
    void writeDecls(std::ostream& out) const {
        for (const auto& d : usedDecls) {
            if (d == "tz_sys_file_open") out << "declare ptr @tz_sys_file_open(ptr, i64, ptr, i64, ptr)\n";
            else if (d == "tz_sys_file_read" || d == "tz_sys_sock_read")
                out << "declare i8 @" << d << "(ptr, i64, ptr, ptr)\n";
            else if (d == "tz_sys_file_read_all") out << "declare i8 @tz_sys_file_read_all(ptr, ptr, ptr)\n";
            else if (d == "tz_sys_file_read_line") out << "declare i32 @tz_sys_file_read_line(ptr, ptr, ptr)\n";
            else if (d == "tz_sys_file_write" || d == "tz_sys_sock_write")
                out << "declare i64 @" << d << "(ptr, ptr, i64, ptr)\n";
            else if (d == "tz_sys_file_flush") out << "declare i8 @tz_sys_file_flush(ptr, ptr)\n";
            else if (d == "tz_sys_file_close" || d == "tz_sys_tcp_close" || d == "tz_sys_sock_close" ||
                     d == "tz_sys_lock_unlock" || d == "tz_sys_mutex_drop")
                out << "declare void @" << d << "(ptr)\n";
            else if (d == "tz_sys_mutex_new") out << "declare ptr @tz_sys_mutex_new()\n";
            else if (d == "tz_sys_mutex_lock" || d == "tz_sys_mutex_try_lock")
                out << "declare ptr @" << d << "(ptr)\n";
            else if (d == "tz_sys_tcp_listen" || d == "tz_sys_tcp_connect")
                out << "declare ptr @" << d << "(ptr, i64, i64, ptr)\n";
            else if (d == "tz_sys_tcp_local_port") out << "declare i64 @tz_sys_tcp_local_port(ptr)\n";
            else if (d == "tz_sys_tcp_accept") out << "declare ptr @tz_sys_tcp_accept(ptr, ptr)\n";
            else if (d == "fflush") out << "declare i32 @fflush(ptr)\n";
            else if (d.starts_with("llvm.sadd.with.overflow.") || d.starts_with("llvm.ssub.with.overflow.") ||
                     d.starts_with("llvm.smul.with.overflow.") || d.starts_with("llvm.uadd.with.overflow.") ||
                     d.starts_with("llvm.usub.with.overflow.") || d.starts_with("llvm.umul.with.overflow.")) {
                const std::string ty = d.substr(d.rfind('.') + 1);
                out << "declare {" << ty << ", i1} @" << d << "(" << ty << ", " << ty << ")\n";
            } else if (d.starts_with("llvm.sadd.sat.") || d.starts_with("llvm.ssub.sat.") ||
                       d.starts_with("llvm.uadd.sat.") || d.starts_with("llvm.usub.sat.") ||
                       d == "llvm.smin.i8" || d == "llvm.smax.i8" ||
                       d.starts_with("llvm.umin.") || d.starts_with("llvm.umax.")) {
                const std::string ty = d.substr(d.rfind('.') + 1);
                out << "declare " << ty << " @" << d << "(" << ty << ", " << ty << ")\n";
            } else if (d.starts_with("llvm.abs.")) {
                const std::string ty = d.substr(d.rfind('.') + 1);
                out << "declare " << ty << " @" << d << "(" << ty << ", i1 immarg)\n";
            } else if (d == "llvm.fabs.f32" || d == "llvm.sqrt.f32" || d == "llvm.floor.f32" ||
                       d == "llvm.ceil.f32" || d == "llvm.round.f32" || d == "llvm.sin.f32" ||
                       d == "llvm.cos.f32")
                out << "declare float @" << d << "(float)\n";
            else if (d == "llvm.pow.f32" || d == "llvm.minnum.f32" || d == "llvm.maxnum.f32")
                out << "declare float @" << d << "(float, float)\n";
            else if (d == "llvm.fabs.f64" || d == "llvm.sqrt.f64" || d == "llvm.floor.f64" ||
                     d == "llvm.ceil.f64" || d == "llvm.round.f64" || d == "llvm.sin.f64" ||
                     d == "llvm.cos.f64")
                out << "declare double @" << d << "(double)\n";
            else if (d == "llvm.pow.f64" || d == "llvm.minnum.f64" || d == "llvm.maxnum.f64")
                out << "declare double @" << d << "(double, double)\n";
            else if (d == "tz_sys_print_float") out << "declare void @tz_sys_print_float(double)\n";
            else if (d == "tz_sys_format_float") out << "declare i64 @tz_sys_format_float(double, ptr, i64)\n";
            else if (d == "tz_sys_print_u64") out << "declare void @tz_sys_print_u64(i64)\n";
            else if (d == "tz_sys_format_u64") out << "declare i64 @tz_sys_format_u64(i64, ptr, i64)\n";
            else if (d == "tz_sys_parse_float") out << "declare i8 @tz_sys_parse_float(ptr, i64, ptr)\n";
            else if (d == "tz_sys_set_args") out << "declare void @tz_sys_set_args(i32, ptr)\n";
            else if (d == "tz_sys_args_count" || d == "tz_sys_clock_ns") out << "declare i64 @" << d << "()\n";
            else if (d == "tz_sys_sleep_ms") out << "declare void @tz_sys_sleep_ms(i64)\n";
            else if (d == "tz_sys_arg_at") out << "declare void @tz_sys_arg_at(i64, ptr)\n";
            else if (d == "tz_sys_read_file" || d == "tz_sys_env_get" || d == "tz_sys_exec_capture")
                out << "declare void @" << d << "(ptr, i64, ptr)\n";
            else if (d == "tz_sys_exec_status") out << "declare i64 @tz_sys_exec_status(ptr, i64)\n";
            else if (d == "tz_sys_file_exists" || d == "tz_sys_remove_file" || d == "tz_sys_make_dir")
                out << "declare i8 @" << d << "(ptr, i64)\n";
            else if (d == "tz_sys_write_file" || d == "tz_sys_append_file")
                out << "declare i8 @" << d << "(ptr, i64, ptr, i64)\n";
            else if (d == "tz_sys_try_read_file")
                out << "declare i8 @tz_sys_try_read_file(ptr, i64, ptr, ptr)\n";
            else if (d == "tz_sys_try_write_file")
                out << "declare i8 @tz_sys_try_write_file(ptr, i64, ptr, i64, ptr)\n";
            else if (d == "tz_col_table_init_secure")
                out << "declare void @tz_col_table_init_secure(ptr)\n";
            else if (d == "tz_col_table_reserve")
                out << "declare i8 @tz_col_table_reserve(ptr, i64, i64, i64, i32)\n";
            else if (d == "tz_col_table_drop" || d == "tz_col_table_clear")
                out << "declare void @" << d << "(ptr, i32, i64, i64, ptr)\n";
            else if (d == "tz_col_table_clone")
                out << "declare i8 @tz_col_table_clone(ptr, ptr, i32, i64, i64, ptr, ptr)\n";
            else if (d == "tz_col_map_find")
                out << "declare ptr @tz_col_map_find(ptr, ptr, i32, i64, i64)\n";
            else if (d == "tz_col_map_set")
                out << "declare i8 @tz_col_map_set(ptr, ptr, ptr, i32, i64, i64, ptr)\n";
            else if (d == "tz_col_map_del")
                out << "declare i8 @tz_col_map_del(ptr, ptr, i32, i64, i64, ptr)\n";
            else if (d == "tz_col_set_add")
                out << "declare i8 @tz_col_set_add(ptr, ptr, i32, i64)\n";
            else if (d.starts_with("tz_col_sort_stable_"))
                out << "declare i8 @" << d << "(ptr, i64)\n";
            else if (d.starts_with("tz_col_sort_"))
                out << "declare void @" << d << "(ptr, i64)\n";
            else if (d == "tz_col_pool_reserve")
                out << "declare i8 @tz_col_pool_reserve(ptr, i64, i64)\n";
            else if (d == "tz_col_pool_add")
                out << "declare i8 @tz_col_pool_add(ptr, ptr, i64, ptr)\n";
            else if (d == "tz_col_pool_get_ptr")
                out << "declare ptr @tz_col_pool_get_ptr(ptr, i32, i32, i64)\n";
            else if (d == "tz_col_pool_remove")
                out << "declare i8 @tz_col_pool_remove(ptr, i32, i32, ptr, i64)\n";
            else if (d == "tz_col_pool_clear" || d == "tz_col_pool_drop")
                out << "declare void @" << d << "(ptr, i64, ptr)\n";
            else if (d == "tz_col_pool_clone")
                out << "declare i8 @tz_col_pool_clone(ptr, ptr, i64, ptr, ptr)\n";
            else if (d == "tz_col_pool_handles")
                out << "declare i32 @tz_col_pool_handles(ptr, ptr)\n";
            else if (d == "tz_conc_scope_new")
                out << "declare ptr @tz_conc_scope_new()\n";
            else if (d == "tz_conc_scope_drop" || d == "tz_conc_task_submit" ||
                     d == "tz_conc_task_wait" || d == "tz_conc_task_join_done" ||
                     d == "tz_conc_task_drop")
                out << "declare void @" << d << "(ptr)\n";
            else if (d == "tz_conc_task_alloc")
                out << "declare ptr @tz_conc_task_alloc(ptr, i64, ptr, ptr)\n";
            else if (d == "tz_conc_task_payload")
                out << "declare ptr @tz_conc_task_payload(ptr)\n";
            else if (d == "tz_conc_parallel_for")
                out << "declare void @tz_conc_parallel_for(i64, ptr, ptr)\n";
            else if (d == "tz_sys_aligned_alloc")
                out << "declare ptr @tz_sys_aligned_alloc(i64, i64)\n";
            else if (d == "tz_sys_aligned_free")
                out << "declare void @tz_sys_aligned_free(ptr)\n";
            else if (d == "tz_sys_cstr_len")
                out << "declare i64 @tz_sys_cstr_len(ptr)\n";
            else if (d == "tz_sys_spin_hint")
                out << "declare void @tz_sys_spin_hint()\n";
            else if (d == "tz_sys_region_new")
                out << "declare ptr @tz_sys_region_new(i64)\n";
            else if (d == "tz_sys_region_alloc")
                out << "declare ptr @tz_sys_region_alloc(ptr, i64, i64)\n";
            else if (d == "tz_sys_region_reset" || d == "tz_sys_region_drop")
                out << "declare void @" << d << "(ptr)\n";
            else if (d == "tz_sys_region_used" || d == "tz_sys_region_cap")
                out << "declare i64 @" << d << "(ptr)\n";
            else if (d == "tz_conc_ring_new")
                out << "declare ptr @tz_conc_ring_new(i64, i64, ptr)\n";
            else if (d == "tz_conc_ring_retain")
                out << "declare ptr @tz_conc_ring_retain(ptr)\n";
            else if (d == "tz_conc_ring_release")
                out << "declare void @tz_conc_ring_release(ptr)\n";
            else if (d == "tz_conc_ring_try_push" || d == "tz_conc_ring_try_pop")
                out << "declare i8 @" << d << "(ptr, ptr)\n";
            else if (d == "tz_conc_ring_len" || d == "tz_conc_ring_cap")
                out << "declare i64 @" << d << "(ptr)\n";
        }
        for (const auto& decl : usedRawDecls) {
            out << decl << "\n";
        }
    }
    void writeExternDecls(std::ostream& out) const {
        std::set<std::string> emitted(usedDecls.begin(), usedDecls.end());
        constexpr const char* predeclared[] = {
            "fwrite", "putchar", "getchar", "memcmp", "exit", "_setmode", "__acrt_iob_func", "malloc", "free"
        };
        for (const char* s : predeclared) emitted.insert(s);
        for (const auto& f : program.functions) {
            if (!f.isTest && f.isExternC && !f.isExternDecl && !f.cSymbol.empty())
                emitted.insert(f.cSymbol);
        }
        for (const auto& f : program.functions) {
            if (f.isTest || !f.isExternDecl || f.cSymbol.empty()) continue;
            if (!emitted.insert(f.cSymbol).second) continue;
            out << "declare " << ffiAttr(f.result) << llvmType(f.result) << " @" << f.cSymbol << "(";
            for (std::size_t i = 0; i < f.params.size(); ++i) {
                if (i > 0) out << ", ";
                out << llvmType(f.params[i].type) << ffiParamAttr(f.params[i].type);
            }
            out << ")\n";
        }
    }
    void writeTypedHelpers(std::ostream& out) {
        std::set<Type> doneDrops, doneDropUnaligned, doneClones, doneVClones, doneRepeats, doneSlices, doneAts, donePushes, donePops, doneClears;
        std::set<Type> doneTblWithCaps, doneTblClears, doneTblReserves;
        std::set<Type> doneMapSets, doneMapGets, doneMapHas, doneMapDels, doneMapKeys, emitDoneMapVals;
        std::set<Type> doneSetAdds, doneSetHas, doneSetDels, doneSetItems;
        std::set<Type> donePoolWithCaps, donePoolReserves, donePoolClears, donePoolAdds, donePoolGets, donePoolGetsMono;
        std::set<Type> donePoolHas, donePoolHasMono, donePoolSets, donePoolRemoves, donePoolHandles;
        std::set<Type> doneRcNews, doneRcGets, doneRcSets, doneRcReuses, doneRcUpdates;
        std::set<Type> doneFzNews, doneFzGets, donePmaps, doneRingPushes;
        std::map<Type, Type> doneRingPops;
        std::set<std::pair<Type, Type>> doneFzReads, doneTaskSpawns;
        std::set<Type> doneDynTypes;
        std::set<std::pair<Type, Type>> doneDynBoxes;
        bool doneDynRuntime = false;
        std::set<std::size_t> doneClosures;
        bool doneFnRuntime = false;
        bool doneTblSecure = false;
        bool doneDropFile = false, doneDropListener = false, doneDropSocket = false,
             doneDropMutex = false, doneDropLock = false,
             doneDropScope = false, doneDropTask = false,
             doneDropRegion = false, doneDropAtomic = false, doneCloneAtomic = false,
             doneDropRing = false, doneCloneRing = false;
        auto emitStoreKey = [&](std::ostream& s, Type k, const std::string& valReg, const std::string& ptrReg, const std::string& pfx) {
            if (k == Type::Bool) {
                s << "  " << pfx << ".b8 = zext i1 " << valReg << " to i8\n"
                  << "  store i8 " << pfx << ".b8, ptr " << ptrReg << "\n";
            } else if (program.isHandle(k)) {
                s << "  " << pfx << ".idx = extractvalue %handle " << valReg << ", 0\n"
                  << "  " << pfx << ".gen = extractvalue %handle " << valReg << ", 1\n"
                  << "  " << pfx << ".i64 = zext i32 " << pfx << ".idx to i64\n"
                  << "  " << pfx << ".g64 = zext i32 " << pfx << ".gen to i64\n"
                  << "  " << pfx << ".hi = shl i64 " << pfx << ".g64, 32\n"
                  << "  " << pfx << ".pk = or i64 " << pfx << ".hi, " << pfx << ".i64\n"
                  << "  store i64 " << pfx << ".pk, ptr " << ptrReg << "\n";
            } else if (Program::isIntType(k) || k == Type::Str) {
                s << "  store " << llvmType(k) << " " << valReg << ", ptr " << ptrReg << "\n";
            } else {
                const auto& klay = layoutOf(k);
                s << "  " << pfx << ".tag = extractvalue " << llvmType(k) << " " << valReg << ", 0\n"
                  << "  store " << klay.tagTy << " " << pfx << ".tag, ptr " << ptrReg << "\n";
            }
        };
        auto emitLoadKey = [&](std::ostream& s, Type k, const std::string& ptrReg, const std::string& outReg, const std::string& pfx) {
            if (k == Type::Bool) {
                s << "  " << pfx << ".b8 = load i8, ptr " << ptrReg << "\n"
                  << "  " << outReg << " = trunc i8 " << pfx << ".b8 to i1\n";
            } else if (program.isHandle(k)) {
                s << "  " << pfx << ".pk = load i64, ptr " << ptrReg << "\n"
                  << "  " << pfx << ".idx = trunc i64 " << pfx << ".pk to i32\n"
                  << "  " << pfx << ".hi = lshr i64 " << pfx << ".pk, 32\n"
                  << "  " << pfx << ".gen = trunc i64 " << pfx << ".hi to i32\n"
                  << "  " << pfx << ".h0 = insertvalue %handle zeroinitializer, i32 " << pfx << ".idx, 0\n"
                  << "  " << outReg << " = insertvalue %handle " << pfx << ".h0, i32 " << pfx << ".gen, 1\n";
            } else if (Program::isIntType(k) || k == Type::Str) {
                s << "  " << outReg << " = load " << llvmType(k) << ", ptr " << ptrReg << "\n";
            } else {
                const auto& klay = layoutOf(k);
                s << "  " << pfx << ".tag = load " << klay.tagTy << ", ptr " << ptrReg << "\n"
                  << "  " << outReg << " = insertvalue " << llvmType(k) << " zeroinitializer, " << klay.tagTy << " " << pfx << ".tag, 0\n";
            }
        };
        bool progress = true;
        while (progress) {
            progress = false;
            if (usesDynRuntime && !doneDynRuntime) {
                doneDynRuntime = true;
                progress = true;
                out << "define internal void @tz.dropDyn(ptr %slot) sanitize_address {\nentry:\n"
                    << "  %d = load %dyn, ptr %slot\n"
                    << "  %vt = extractvalue %dyn %d, 1\n"
                    << "  %isnull = icmp eq ptr %vt, null\n"
                    << "  br i1 %isnull, label %done, label %doDrop\ndoDrop:\n"
                    << "  %data = extractvalue %dyn %d, 0\n"
                    << "  %fn = load ptr, ptr %vt\n"
                    << "  call void %fn(ptr %data)\n"
                    << "  store %dyn zeroinitializer, ptr %slot\n"
                    << "  br label %done\ndone:\n  ret void\n}\n"
                    << "define internal %dyn @tz.cloneDyn(%dyn %src, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %vt = extractvalue %dyn %src, 1\n"
                    << "  %isnull = icmp eq ptr %vt, null\n"
                    << "  br i1 %isnull, label %empty, label %doClone\nempty:\n"
                    << "  ret %dyn zeroinitializer\ndoClone:\n"
                    << "  %data = extractvalue %dyn %src, 0\n"
                    << "  %fp = getelementptr inbounds ptr, ptr %vt, i64 1\n"
                    << "  %fn = load ptr, ptr %fp\n"
                    << "  %ndata = call ptr %fn(ptr %data, %diag %allocMsg)\n"
                    << "  %res = insertvalue %dyn %src, ptr %ndata, 0\n"
                    << "  ret %dyn %res\n}\n";
            }
            const auto dynBoxesCopy = usedDynBoxes;
            for (const auto& [iface, concreteT] : dynBoxesCopy) {
                if (!doneDynBoxes.emplace(iface, concreteT).second) continue;
                progress = true;
                const std::string tLlvm = llvmType(concreteT);
                const bool zeroT = isZeroSized(concreteT);
                const std::string typeTag = std::to_string(static_cast<int>(concreteT));
                const std::string suffix = std::to_string(static_cast<int>(iface)) + "." + typeTag;
                if (doneDynTypes.insert(concreteT).second) {
                    out << "define internal void @tz.dyndrop." << typeTag << "(ptr %data) sanitize_address {\nentry:\n";
                    if (!zeroT) {
                        out << "  %isnull = icmp eq ptr %data, null\n"
                            << "  br i1 %isnull, label %done, label %freeIt\nfreeIt:\n";
                        if (needsDrop(concreteT)) {
                            out << "  call void " << requestDrop(concreteT) << "(ptr %data)\n";
                        }
                        out << "  call void @free(ptr %data)\n  br label %done\ndone:\n";
                    }
                    out << "  ret void\n}\n"
                        << "define internal ptr @tz.dynclone." << typeTag << "(ptr %data, %diag %allocMsg) sanitize_address {\nentry:\n";
                    if (zeroT) {
                        out << "  ret ptr null\n}\n";
                    } else {
                        out << "  %p = call noalias ptr @tz.malloc(i64 " << strideExpr(concreteT) << ", %diag %allocMsg)\n"
                            << "  %v = load " << tLlvm << ", ptr %data\n";
                        std::string stored = "%v";
                        if (needsDrop(concreteT)) {
                            out << "  %cv = call " << tLlvm << " " << requestClone(concreteT) << "("
                                << cloneCallArgs(concreteT, tLlvm + " %v", "%diag %allocMsg") << ")\n";
                            stored = "%cv";
                        }
                        out << "  store " << tLlvm << " " << stored << ", ptr %p\n"
                            << "  ret ptr %p\n}\n";
                    }
                }
                const InterfaceDecl* ifaceDecl = nullptr;
                for (const auto& id : program.interfaces) {
                    if (id.type == iface) { ifaceDecl = &id; break; }
                }
                const Program::DynVtable* dvPtr = nullptr;
                for (const auto& dv : program.dynVtables) {
                    if (dv.iface == iface && dv.target == concreteT) { dvPtr = &dv; break; }
                }
                const std::size_t nm = ifaceDecl->methods.size();
                for (std::size_t mi = 0; mi < nm; ++mi) {
                    const Function& sig = ifaceDecl->methods[mi];
                    const std::string& implFn = dvPtr->methods[mi];
                    const std::string retLlvm = llvmType(sig.result);
                    out << "define internal " << retLlvm << " @tz.dynm." << suffix << "." << mi
                        << "(i32 %depth, ptr %data";
                    for (std::size_t pi = 1; pi < sig.params.size(); ++pi) {
                        out << ", " << (sig.params[pi].byRef ? "ptr" : llvmType(sig.params[pi].type))
                            << " %p" << pi;
                    }
                    out << ") sanitize_address {\nentry:\n";
                    std::string firstArg;
                    if (sig.params[0].byRef) {
                        if (zeroT) {
                            out << "  %zt = alloca {}\n";
                            firstArg = "ptr %zt";
                        } else {
                            firstArg = "ptr %data";
                        }
                    } else {
                        if (zeroT) {
                            firstArg = tLlvm + " zeroinitializer";
                        } else {
                            out << "  %selfVal = load " << tLlvm << ", ptr %data\n";
                            firstArg = tLlvm + " %selfVal";
                        }
                    }
                    out << "  ";
                    if (sig.result != Type::Void) out << "%res = ";
                    out << "call " << retLlvm << " " << symbol(implFn) << "(i32 %depth, " << firstArg;
                    for (std::size_t pi = 1; pi < sig.params.size(); ++pi) {
                        out << ", " << (sig.params[pi].byRef ? "ptr" : llvmType(sig.params[pi].type))
                            << " %p" << pi;
                    }
                    out << ")\n";
                    if (sig.result == Type::Void) out << "  ret void\n}\n";
                    else out << "  ret " << retLlvm << " %res\n}\n";
                }
                out << "@tz.vtable." << suffix << " = private unnamed_addr constant [" << (2 + nm) << " x ptr] [\n"
                    << "  ptr @tz.dyndrop." << typeTag << ",\n  ptr @tz.dynclone." << typeTag;
                for (std::size_t mi = 0; mi < nm; ++mi) {
                    out << ",\n  ptr @tz.dynm." << suffix << "." << mi;
                }
                out << "\n]\n"
                    << "define internal %dyn @tz.box." << suffix << "(" << tLlvm << " %val, %diag %allocMsg) sanitize_address {\nentry:\n";
                if (zeroT) {
                    out << "  %d0 = insertvalue %dyn zeroinitializer, ptr null, 0\n"
                        << "  %d1 = insertvalue %dyn %d0, ptr @tz.vtable." << suffix << ", 1\n"
                        << "  ret %dyn %d1\n}\n";
                } else {
                    out << "  %p = call noalias ptr @tz.malloc(i64 " << strideExpr(concreteT) << ", %diag %allocMsg)\n"
                        << "  store " << tLlvm << " %val, ptr %p\n"
                        << "  %d0 = insertvalue %dyn zeroinitializer, ptr %p, 0\n"
                        << "  %d1 = insertvalue %dyn %d0, ptr @tz.vtable." << suffix << ", 1\n"
                        << "  ret %dyn %d1\n}\n";
                }
            }
            if (usesFnRuntime && !doneFnRuntime) {
                doneFnRuntime = true;
                progress = true;
                out << "define internal void @tz.dropFn(ptr %slot) sanitize_address {\nentry:\n"
                    << "  %f = load %fn, ptr %slot\n"
                    << "  %vt = extractvalue %fn %f, 1\n"
                    << "  %isnull = icmp eq ptr %vt, null\n"
                    << "  br i1 %isnull, label %done, label %doDrop\ndoDrop:\n"
                    << "  %env = extractvalue %fn %f, 0\n"
                    << "  %fnptr = load ptr, ptr %vt\n"
                    << "  call void %fnptr(ptr %env)\n"
                    << "  store %fn zeroinitializer, ptr %slot\n"
                    << "  br label %done\ndone:\n  ret void\n}\n"
                    << "define internal %fn @tz.cloneFn(%fn %src, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %vt = extractvalue %fn %src, 1\n"
                    << "  %isnull = icmp eq ptr %vt, null\n"
                    << "  br i1 %isnull, label %empty, label %doClone\nempty:\n"
                    << "  ret %fn zeroinitializer\ndoClone:\n"
                    << "  %env = extractvalue %fn %src, 0\n"
                    << "  %fp = getelementptr inbounds ptr, ptr %vt, i64 1\n"
                    << "  %fnptr = load ptr, ptr %fp\n"
                    << "  %nenv = call ptr %fnptr(ptr %env, %diag %allocMsg)\n"
                    << "  %res = insertvalue %fn %src, ptr %nenv, 0\n"
                    << "  ret %fn %res\n}\n";
            }
            const auto closuresCopy = usedClosures;
            for (std::size_t cid : closuresCopy) {
                if (!doneClosures.insert(cid).second) continue;
                progress = true;
                const auto& cl = program.closures.at(cid);
                const bool zeroE = (cl.envStruct == Type::Void || isZeroSized(cl.envStruct));
                const std::string cidStr = std::to_string(cid);
                out << "define internal void @tz.fn.drop." << cidStr << "(ptr %env) sanitize_address {\nentry:\n";
                if (!zeroE) {
                    out << "  %isnull = icmp eq ptr %env, null\n"
                        << "  br i1 %isnull, label %done, label %freeIt\nfreeIt:\n";
                    if (needsDrop(cl.envStruct)) {
                        out << "  call void " << requestDrop(cl.envStruct) << "(ptr %env)\n";
                    }
                    out << "  call void @free(ptr %env)\n  br label %done\ndone:\n";
                }
                out << "  ret void\n}\n"
                    << "define internal ptr @tz.fn.clone." << cidStr << "(ptr %env, %diag %allocMsg) sanitize_address {\nentry:\n";
                if (zeroE) {
                    out << "  ret ptr null\n}\n";
                } else {
                    const std::string eLlvm = llvmType(cl.envStruct);
                    const std::string hAlign = heapAlignSuffix(cl.envStruct);
                    out << "  %p = call noalias ptr @tz.malloc(i64 " << strideExpr(cl.envStruct) << ", %diag %allocMsg)\n"
                        << "  %v = load " << eLlvm << ", ptr %env" << hAlign << "\n";
                    std::string stored = "%v";
                    if (needsDrop(cl.envStruct)) {
                        out << "  %cv = call " << eLlvm << " " << requestClone(cl.envStruct) << "("
                            << cloneCallArgs(cl.envStruct, eLlvm + " %v", "%diag %allocMsg") << ")\n";
                        stored = "%cv";
                    }
                    out << "  store " << eLlvm << " " << stored << ", ptr %p" << hAlign << "\n"
                        << "  ret ptr %p\n}\n";
                }
                const std::string retLlvm = llvmType(cl.result);
                out << "define internal " << retLlvm << " @tz.fn.call." << cidStr << "(i32 %depth, ptr %env";
                for (std::size_t pi = 0; pi < cl.paramTypes.size(); ++pi) {
                    out << ", " << llvmType(cl.paramTypes[pi]) << " %p" << pi;
                }
                out << ") sanitize_address {\nentry:\n";
                if (!zeroE) {
                    const std::string eLlvm = llvmType(cl.envStruct);
                    out << "  %envVal = load " << eLlvm << ", ptr %env" << heapAlignSuffix(cl.envStruct) << "\n";
                }
                out << "  ";
                if (cl.result != Type::Void) out << "%res = ";
                out << "call " << retLlvm << " " << symbol(cl.fnName) << "(i32 %depth";
                if (cl.envStruct != Type::Void) {
                    const std::string eLlvm = llvmType(cl.envStruct);
                    out << ", " << eLlvm << " " << (zeroE ? "zeroinitializer" : "%envVal");
                }
                for (std::size_t pi = 0; pi < cl.paramTypes.size(); ++pi) {
                    out << ", " << llvmType(cl.paramTypes[pi]) << " %p" << pi;
                }
                out << ")\n";
                if (cl.result == Type::Void) out << "  ret void\n}\n";
                else out << "  ret " << retLlvm << " %res\n}\n";
                out << "@tz.fnvt." << cidStr << " = private unnamed_addr constant [3 x ptr] [\n"
                    << "  ptr @tz.fn.drop." << cidStr << ",\n"
                    << "  ptr @tz.fn.clone." << cidStr << ",\n"
                    << "  ptr @tz.fn.call." << cidStr << "\n]\n";
                if (zeroE) {
                    out << "define internal %fn @tz.mkclosure." << cidStr << "(%diag %allocMsg) sanitize_address {\nentry:\n"
                        << "  %f0 = insertvalue %fn zeroinitializer, ptr null, 0\n"
                        << "  %f1 = insertvalue %fn %f0, ptr @tz.fnvt." << cidStr << ", 1\n"
                        << "  ret %fn %f1\n}\n";
                } else {
                    const std::string eLlvm = llvmType(cl.envStruct);
                    out << "define internal %fn @tz.mkclosure." << cidStr << "(" << eLlvm
                        << " %val, %diag %allocMsg) sanitize_address {\nentry:\n"
                        << "  %p = call noalias ptr @tz.malloc(i64 " << strideExpr(cl.envStruct) << ", %diag %allocMsg)\n"
                        << "  store " << eLlvm << " %val, ptr %p" << heapAlignSuffix(cl.envStruct) << "\n"
                        << "  %f0 = insertvalue %fn zeroinitializer, ptr %p, 0\n"
                        << "  %f1 = insertvalue %fn %f0, ptr @tz.fnvt." << cidStr << ", 1\n"
                        << "  ret %fn %f1\n}\n";
                }
            }
            const auto dropsCopy = emitDrops;
            for (Type t : dropsCopy) {
                if (!doneDrops.insert(t).second) continue;
                progress = true;
                const std::string tLlvm = llvmType(t);
                const std::string fnName = "@tz.drop.t" + std::to_string(static_cast<int>(t));
                out << "define internal void " << fnName << "(ptr %slot) sanitize_address {\nentry:\n";
                if (program.isArray(t)) {
                    const Type elemT = elemOf(t);
                    const std::string elemLlvm = llvmType(elemT);
                    const std::string elemDrop = heapAlignSuffix(elemT).empty() ? requestDrop(elemT) : requestDropUnaligned(elemT);
                    out << "  %arr = load %array, ptr %slot\n"
                        << "  %len32 = extractvalue %array %arr, 1\n"
                        << "  %len = zext i32 %len32 to i64\n"
                        << "  %p = extractvalue %array %arr, 0\n"
                        << "  %empty = icmp eq i64 %len, 0\n"
                        << "  br i1 %empty, label %done, label %loop\n"
                        << "loop:\n"
                        << "  %rem = phi i64 [ %len, %entry ], [ %idx, %loop ]\n"
                        << "  %idx = sub i64 %rem, 1\n"
                        << "  %ep = getelementptr inbounds " << elemLlvm << ", ptr %p, i64 %idx\n"
                        << "  call void " << elemDrop << "(ptr %ep)\n"
                        << "  %more = icmp ne i64 %idx, 0\n"
                        << "  br i1 %more, label %loop, label %done\n"
                        << "done:\n"
                        << "  call void @free(ptr %p)\n"
                        << "  store %array zeroinitializer, ptr %slot\n"
                        << "  ret void\n}\n";
                } else if (program.isMap(t) || program.isSet(t)) {
                    const Type kt = program.isMap(t) ? program.mapKey(t) : program.setElem(t);
                    const int kk = keyKindOf(kt);
                    const std::string ks = keyStrideExpr(kt);
                    const Type vt = program.isMap(t) ? program.mapVal(t) : Type::Void;
                    const std::string vs = (program.isMap(t) && !isZeroSized(vt)) ? strideExpr(vt) : "0";
                    const std::string vdrop = (program.isMap(t) && needsDrop(vt)) ? requestDrop(vt) : "null";
                    usedDecls.insert("tz_col_table_drop");
                    out << "  call void @tz_col_table_drop(ptr %slot, i32 " << kk << ", i64 " << ks
                        << ", i64 " << vs << ", ptr " << vdrop << ")\n"
                        << "  ret void\n}\n";
                } else if (program.isPool(t)) {
                    const Type et = program.poolElem(t);
                    const std::string es = isZeroSized(et) ? "0" : strideExpr(et);
                    const std::string edrop = needsDrop(et) ? requestDrop(et) : "null";
                    usedDecls.insert("tz_col_pool_drop");
                    out << "  call void @tz_col_pool_drop(ptr %slot, i64 " << es << ", ptr " << edrop << ")\n"
                        << "  ret void\n}\n";
                } else if (program.isShared(t)) {
                    const Type et = program.sharedElem(t);
                    const bool zeroE = isZeroSized(et);
                    const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + llvmType(et) + " }");
                    out << "  %p = load ptr, ptr %slot\n"
                        << "  %isnull = icmp eq ptr %p, null\n"
                        << "  br i1 %isnull, label %done, label %dec\n"
                        << "dec:\n"
                        << "  store ptr null, ptr %slot\n"
                        << "  %rc = load i64, ptr %p\n"
                        << "  %next = sub i64 %rc, 1\n"
                        << "  %zero = icmp eq i64 %next, 0\n"
                        << "  br i1 %zero, label %freeIt, label %keep\n"
                        << "keep:\n"
                        << "  store i64 %next, ptr %p\n"
                        << "  br label %done\n"
                        << "freeIt:\n";
                    if (!zeroE && needsDrop(et)) {
                        const std::string edrop = requestDrop(et);
                        out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %p, i32 0, i32 1\n"
                            << "  call void " << edrop << "(ptr %vp)\n";
                    }
                    out << "  call void @free(ptr %p)\n"
                        << "  br label %done\n"
                        << "done:\n  ret void\n}\n";
                } else if (program.isFrozen(t)) {
                    const Type et = program.frozenElem(t);
                    const bool zeroE = isZeroSized(et);
                    const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + llvmType(et) + " }");
                    out << "  %p = load ptr, ptr %slot\n"
                        << "  %isnull = icmp eq ptr %p, null\n"
                        << "  br i1 %isnull, label %done, label %dec\n"
                        << "dec:\n"
                        << "  store ptr null, ptr %slot\n"
                        << "  %old = atomicrmw sub ptr %p, i64 1 acq_rel, align 8\n"
                        << "  %last = icmp eq i64 %old, 1\n"
                        << "  br i1 %last, label %freeIt, label %done\n"
                        << "freeIt:\n";
                    if (!zeroE && needsDrop(et)) {
                        const std::string edrop = requestDrop(et);
                        out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %p, i32 0, i32 1\n"
                            << "  call void " << edrop << "(ptr %vp)\n";
                    }
                    out << "  call void @free(ptr %p)\n"
                        << "  br label %done\n"
                        << "done:\n  ret void\n}\n";
                } else {
                    const auto* decl = program.declaration(t);
                    const auto& lay = layoutOf(t);
                    if (!lay.isEnum) {
                        const auto& fields = decl->variants.at(0).fields;
                        for (std::size_t logIdx = fields.size(); logIdx-- > 0; ) {
                            const Type ft = fields[logIdx].type;
                            if (!needsDrop(ft)) continue;
                            const unsigned phys = lay.variants.at(0).logicalToLlvm.at(logIdx);
                            const std::string dfn = lay.packed ? requestDropUnaligned(ft) : requestDrop(ft);
                            out << "  %fp." << logIdx << " = getelementptr inbounds " << tLlvm
                                << ", ptr %slot, i32 0, i32 " << phys << "\n"
                                << "  call void " << dfn << "(ptr %fp." << logIdx << ")\n";
                        }
                        out << "  store " << tLlvm << " zeroinitializer, ptr %slot"
                            << (lay.packed ? ", align 1" : "") << "\n  ret void\n}\n";
                    } else if (!lay.multiPayload) {
                        if (lay.payloadVariants == 1) {
                            const unsigned v = lay.singlePayloadVariant;
                            const auto& fields = decl->variants.at(v).fields;
                            out << "  %tagp = getelementptr inbounds " << tLlvm << ", ptr %slot, i32 0, i32 0\n"
                                << "  %tag = load " << lay.tagTy << ", ptr %tagp\n"
                                << "  %match = icmp eq " << lay.tagTy << " %tag, " << v << "\n"
                                << "  br i1 %match, label %dropPay, label %done\n"
                                << "dropPay:\n";
                            for (std::size_t logIdx = fields.size(); logIdx-- > 0; ) {
                                const Type ft = fields[logIdx].type;
                                if (!needsDrop(ft)) continue;
                                const unsigned phys = lay.singlePayLogicalToLlvm.at(logIdx);
                                out << "  %fp." << logIdx << " = getelementptr inbounds " << tLlvm
                                    << ", ptr %slot, i32 0, i32 " << phys << "\n"
                                    << "  call void " << requestDrop(ft) << "(ptr %fp." << logIdx << ")\n";
                            }
                            out << "  br label %done\ndone:\n";
                        }
                        out << "  store " << tLlvm << " zeroinitializer, ptr %slot\n  ret void\n}\n";
                    } else {
                        out << "  %tagp = getelementptr inbounds " << tLlvm << ", ptr %slot, i32 0, i32 0\n"
                            << "  %tag = load " << lay.tagTy << ", ptr %tagp\n"
                            << "  %pay = getelementptr inbounds " << tLlvm << ", ptr %slot, i32 0, i32 " << lay.payFieldIdx << "\n"
                            << "  switch " << lay.tagTy << " %tag, label %done [\n";
                        std::vector<std::size_t> dropVars;
                        for (std::size_t vi = 0; vi < decl->variants.size(); ++vi) {
                            bool anyDrop = false;
                            for (const auto& f : decl->variants[vi].fields) {
                                if (needsDrop(f.type)) { anyDrop = true; break; }
                            }
                            if (anyDrop) {
                                dropVars.push_back(vi);
                                out << "    " << lay.tagTy << " " << vi << ", label %var." << vi << "\n";
                            }
                        }
                        out << "  ]\n";
                        for (std::size_t vi : dropVars) {
                            const auto& fields = decl->variants[vi].fields;
                            out << "var." << vi << ":\n";
                            for (std::size_t logIdx = fields.size(); logIdx-- > 0; ) {
                                const Type ft = fields[logIdx].type;
                                if (!needsDrop(ft)) continue;
                                const unsigned phys = lay.variants.at(vi).logicalToLlvm.at(logIdx);
                                out << "  %fp." << vi << "." << logIdx << " = getelementptr inbounds " << tLlvm
                                    << ".p" << vi << ", ptr %pay, i32 0, i32 " << phys << "\n"
                                    << "  call void " << requestDrop(ft) << "(ptr %fp." << vi << "." << logIdx << ")\n";
                            }
                            out << "  br label %done\n";
                        }
                        out << "done:\n  store " << tLlvm << " zeroinitializer, ptr %slot\n  ret void\n}\n";
                    }
                }
            }
            const auto dropUnalignedCopy = emitDropUnaligned;
            for (Type t : dropUnalignedCopy) {
                if (!doneDropUnaligned.insert(t).second) continue;
                progress = true;
                const std::string tLlvm = llvmType(t);
                const std::string innerDrop = requestDrop(t);
                out << "define internal void @tz.dropUnaligned.t" << static_cast<int>(t)
                    << "(ptr %slot) sanitize_address {\nentry:\n"
                    << "  %tmp = alloca " << tLlvm << allocaAlignSuffix(t) << "\n"
                    << "  %val = load " << tLlvm << ", ptr %slot, align 1\n"
                    << "  store " << tLlvm << " %val, ptr %tmp" << allocaAlignSuffix(t) << "\n"
                    << "  call void " << innerDrop << "(ptr %tmp)\n"
                    << "  store " << tLlvm << " zeroinitializer, ptr %slot, align 1\n"
                    << "  ret void\n}\n";
            }
            const auto clonesCopy = emitClones;
            for (Type t : clonesCopy) {
                if (!doneClones.insert(t).second) continue;
                progress = true;
                const std::string tLlvm = llvmType(t);
                const std::string fnName = "@tz.clone.t" + std::to_string(static_cast<int>(t));
                out << "define internal " << tLlvm << " " << fnName << "(" << tLlvm << " %src, %diag %msg) sanitize_address {\nentry:\n";
                if (program.isArray(t)) {
                    const Type elemT = elemOf(t);
                    const std::string elemLlvm = llvmType(elemT);
                    const std::string stride = strideExpr(elemT);
                    out << "  %n32 = extractvalue %array %src, 1\n"
                        << "  %n = zext i32 %n32 to i64\n"
                        << "  %empty = icmp eq i64 %n, 0\n"
                        << "  br i1 %empty, label %zero, label %copy\n"
                        << "zero:\n"
                        << "  ret %array zeroinitializer\n"
                        << "copy:\n"
                        << "  %p = extractvalue %array %src, 0\n"
                        << "  %dst = call ptr @tz.allocBuf(i64 %n, i64 " << stride << ", %diag %msg, %diag %msg)\n";
                    if (!needsDrop(elemT)) {
                        out << "  %bytes = mul i64 %n, " << stride << "\n"
                            << "  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %p, i64 %bytes, i1 false)\n"
                            << "  br label %finish\n";
                    } else {
                        const std::string elemClone = requestClone(elemT);
                        const std::string hAlign = heapAlignSuffix(elemT);
                        out << "  br label %loop\n"
                            << "loop:\n"
                            << "  %i = phi i64 [ 0, %copy ], [ %next, %loop ]\n"
                            << "  %sp = getelementptr inbounds " << elemLlvm << ", ptr %p, i64 %i\n"
                            << "  %el = load " << elemLlvm << ", ptr %sp" << hAlign << "\n"
                            << "  %cl = call " << elemLlvm << " " << elemClone << "("
                            << cloneCallArgs(elemT, elemLlvm + " %el", "%diag %msg") << ")\n"
                            << "  %dp = getelementptr inbounds " << elemLlvm << ", ptr %dst, i64 %i\n"
                            << "  store " << elemLlvm << " %cl, ptr %dp" << hAlign << "\n"
                            << "  %next = add i64 %i, 1\n"
                            << "  %more = icmp ult i64 %next, %n\n"
                            << "  br i1 %more, label %loop, label %finish\n";
                    }
                    out << "finish:\n"
                        << "  %a0 = insertvalue %array zeroinitializer, ptr %dst, 0\n"
                        << "  %a1 = insertvalue %array %a0, i32 %n32, 1\n"
                        << "  %a = insertvalue %array %a1, i32 %n32, 2\n"
                        << "  ret %array %a\n}\n";
                } else if (program.isMap(t) || program.isSet(t)) {
                    const Type kt = program.isMap(t) ? program.mapKey(t) : program.setElem(t);
                    const int kk = keyKindOf(kt);
                    const std::string ks = keyStrideExpr(kt);
                    const Type vt = program.isMap(t) ? program.mapVal(t) : Type::Void;
                    const std::string vs = (program.isMap(t) && !isZeroSized(vt)) ? strideExpr(vt) : "0";
                    const std::string vclone = (program.isMap(t) && needsDrop(vt)) ? requestVClone(vt) : "null";
                    usedDecls.insert("tz_col_table_clone");
                    out << "  %srcSlot = alloca %table\n"
                        << "  %dstSlot = alloca %table\n"
                        << "  %diagSlot = alloca %diag\n"
                        << "  store %table %src, ptr %srcSlot\n"
                        << "  store %diag %msg, ptr %diagSlot\n"
                        << "  %ok = call i8 @tz_col_table_clone(ptr %dstSlot, ptr %srcSlot, i32 " << kk
                        << ", i64 " << ks << ", i64 " << vs << ", ptr " << vclone << ", ptr %diagSlot)\n"
                        << "  %bad = icmp eq i8 %ok, 0\n"
                        << "  call void @tz.guard(i1 %bad, %diag %msg)\n"
                        << "  %res = load %table, ptr %dstSlot\n"
                        << "  ret %table %res\n}\n";
                } else if (program.isPool(t)) {
                    const Type et = program.poolElem(t);
                    const std::string es = isZeroSized(et) ? "0" : strideExpr(et);
                    const std::string eclone = needsDrop(et) ? requestVClone(et) : "null";
                    usedDecls.insert("tz_col_pool_clone");
                    out << "  %srcSlot = alloca %pool\n"
                        << "  %dstSlot = alloca %pool\n"
                        << "  %diagSlot = alloca %diag\n"
                        << "  store %pool %src, ptr %srcSlot\n"
                        << "  store %diag %msg, ptr %diagSlot\n"
                        << "  %ok = call i8 @tz_col_pool_clone(ptr %dstSlot, ptr %srcSlot, i64 " << es
                        << ", ptr " << eclone << ", ptr %diagSlot)\n"
                        << "  %bad = icmp eq i8 %ok, 0\n"
                        << "  call void @tz.guard(i1 %bad, %diag %msg)\n"
                        << "  %res = load %pool, ptr %dstSlot\n"
                        << "  ret %pool %res\n}\n";
                } else if (program.isShared(t)) {
                    out << "  %isnull = icmp eq ptr %src, null\n"
                        << "  br i1 %isnull, label %done, label %inc\n"
                        << "inc:\n"
                        << "  %rc = load i64, ptr %src\n"
                        << "  %next = add i64 %rc, 1\n"
                        << "  store i64 %next, ptr %src\n"
                        << "  br label %done\n"
                        << "done:\n  ret ptr %src\n}\n";
                } else if (program.isFrozen(t)) {
                    out << "  %isnull = icmp eq ptr %src, null\n"
                        << "  br i1 %isnull, label %done, label %inc\n"
                        << "inc:\n"
                        << "  %old = atomicrmw add ptr %src, i64 1 monotonic, align 8\n"
                        << "  br label %done\n"
                        << "done:\n  ret ptr %src\n}\n";
                } else {
                    const auto* decl = program.declaration(t);
                    const auto& lay = layoutOf(t);
                    if (!lay.isEnum) {
                        std::string acc = "%src";
                        unsigned step = 0;
                        const auto& fields = decl->variants.at(0).fields;
                        for (std::size_t logIdx = 0; logIdx < fields.size(); ++logIdx) {
                            const Type ft = fields[logIdx].type;
                            if (!needsDrop(ft)) continue;
                            const unsigned phys = lay.variants.at(0).logicalToLlvm.at(logIdx);
                            const std::string fLlvm = llvmType(ft);
                            const std::string fClone = requestClone(ft);
                            const std::string oldR = "%old." + std::to_string(step);
                            const std::string newR = "%new." + std::to_string(step);
                            const std::string nextAcc = "%acc." + std::to_string(step + 1);
                            out << "  " << oldR << " = extractvalue " << tLlvm << " " << acc << ", " << phys << "\n"
                                << "  " << newR << " = call " << fLlvm << " " << fClone << "("
                                << cloneCallArgs(ft, fLlvm + " " + oldR, "%diag %msg") << ")\n"
                                << "  " << nextAcc << " = insertvalue " << tLlvm << " " << acc << ", "
                                << fLlvm << " " << newR << ", " << phys << "\n";
                            acc = nextAcc;
                            ++step;
                        }
                        out << "  ret " << tLlvm << " " << acc << "\n}\n";
                    } else if (!lay.multiPayload) {
                        if (lay.payloadVariants != 1) {
                            out << "  ret " << tLlvm << " %src\n}\n";
                        } else {
                            const unsigned v = lay.singlePayloadVariant;
                            const auto& fields = decl->variants.at(v).fields;
                            out << "  %tag = extractvalue " << tLlvm << " %src, 0\n"
                                << "  %match = icmp eq " << lay.tagTy << " %tag, " << v << "\n"
                                << "  br i1 %match, label %clonePay, label %trivial\n"
                                << "trivial:\n  ret " << tLlvm << " %src\nclonePay:\n";
                            std::string acc = "%src";
                            unsigned step = 0;
                            for (std::size_t logIdx = 0; logIdx < fields.size(); ++logIdx) {
                                const Type ft = fields[logIdx].type;
                                if (!needsDrop(ft)) continue;
                                const unsigned phys = lay.singlePayLogicalToLlvm.at(logIdx);
                                const std::string fLlvm = llvmType(ft);
                                const std::string fClone = requestClone(ft);
                                const std::string oldR = "%old." + std::to_string(step);
                                const std::string newR = "%new." + std::to_string(step);
                                const std::string nextAcc = "%acc." + std::to_string(step + 1);
                                out << "  " << oldR << " = extractvalue " << tLlvm << " " << acc << ", " << phys << "\n"
                                    << "  " << newR << " = call " << fLlvm << " " << fClone << "("
                                    << cloneCallArgs(ft, fLlvm + " " + oldR, "%diag %msg") << ")\n"
                                    << "  " << nextAcc << " = insertvalue " << tLlvm << " " << acc << ", "
                                    << fLlvm << " " << newR << ", " << phys << "\n";
                                acc = nextAcc;
                                ++step;
                            }
                            out << "  ret " << tLlvm << " " << acc << "\n}\n";
                        }
                    } else {
                        out << "  %tmp = alloca " << tLlvm << allocaAlignSuffix(t) << "\n"
                            << "  store " << tLlvm << " %src, ptr %tmp\n"
                            << "  %tagp = getelementptr inbounds " << tLlvm << ", ptr %tmp, i32 0, i32 0\n"
                            << "  %tag = load " << lay.tagTy << ", ptr %tagp\n"
                            << "  %pay = getelementptr inbounds " << tLlvm << ", ptr %tmp, i32 0, i32 " << lay.payFieldIdx << "\n"
                            << "  switch " << lay.tagTy << " %tag, label %done [\n";
                        std::vector<std::size_t> cloneVars;
                        for (std::size_t vi = 0; vi < decl->variants.size(); ++vi) {
                            bool anyDrop = false;
                            for (const auto& f : decl->variants[vi].fields) {
                                if (needsDrop(f.type)) { anyDrop = true; break; }
                            }
                            if (anyDrop) {
                                cloneVars.push_back(vi);
                                out << "    " << lay.tagTy << " " << vi << ", label %var." << vi << "\n";
                            }
                        }
                        out << "  ]\n";
                        for (std::size_t vi : cloneVars) {
                            const auto& fields = decl->variants[vi].fields;
                            out << "var." << vi << ":\n";
                            for (std::size_t logIdx = 0; logIdx < fields.size(); ++logIdx) {
                                const Type ft = fields[logIdx].type;
                                if (!needsDrop(ft)) continue;
                                const unsigned phys = lay.variants.at(vi).logicalToLlvm.at(logIdx);
                                const std::string fLlvm = llvmType(ft);
                                const std::string fClone = requestClone(ft);
                                const std::string sfx = "." + std::to_string(vi) + "." + std::to_string(logIdx);
                                out << "  %fp" << sfx << " = getelementptr inbounds " << tLlvm << ".p" << vi
                                    << ", ptr %pay, i32 0, i32 " << phys << "\n"
                                    << "  %ov" << sfx << " = load " << fLlvm << ", ptr %fp" << sfx << "\n"
                                    << "  %nv" << sfx << " = call " << fLlvm << " " << fClone << "("
                                    << cloneCallArgs(ft, fLlvm + " %ov" + sfx, "%diag %msg") << ")\n"
                                    << "  store " << fLlvm << " %nv" << sfx << ", ptr %fp" << sfx << "\n";
                            }
                            out << "  br label %done\n";
                        }
                        out << "done:\n  %res = load " << tLlvm << ", ptr %tmp\n  ret " << tLlvm << " %res\n}\n";
                    }
                }
            }
            const auto repeatsCopy = emitRepeats;
            for (Type t : repeatsCopy) {
                if (!doneRepeats.insert(t).second) continue;
                progress = true;
                const Type elemT = elemOf(t);
                const std::string elemLlvm = llvmType(elemT);
                const std::string stride = strideExpr(elemT);
                const std::string fnName = "@tz.repeat.t" + std::to_string(static_cast<int>(t));
                out << "define internal %array " << fnName << "(" << elemLlvm
                    << " %val, i64 %count, i1 %init, %diag %sizeMsg, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %p = call ptr @tz.allocBuf(i64 %count, i64 " << stride << ", %diag %sizeMsg, %diag %allocMsg)\n"
                    << "  %empty = icmp eq i64 %count, 0\n"
                    << "  br i1 %empty, label %zero, label %checkInit\n"
                    << "zero:\n  ret %array zeroinitializer\n"
                    << "checkInit:\n  br i1 %init, label %fill, label %finish\n"
                    << "fill:\n"
                    << "  %i = phi i64 [ 0, %checkInit ], [ %next, %fill ]\n"
                    << "  %dst = getelementptr inbounds " << elemLlvm << ", ptr %p, i64 %i\n";
                const std::string hAlign = heapAlignSuffix(elemT);
                if (needsDrop(elemT)) {
                    const std::string elemClone = requestClone(elemT);
                    out << "  %cv = call " << elemLlvm << " " << elemClone << "("
                        << cloneCallArgs(elemT, elemLlvm + " %val", "%diag %allocMsg") << ")\n"
                        << "  store " << elemLlvm << " %cv, ptr %dst" << hAlign << "\n";
                } else {
                    out << "  store " << elemLlvm << " %val, ptr %dst" << hAlign << "\n";
                }
                out << "  %next = add i64 %i, 1\n"
                    << "  %more = icmp ult i64 %next, %count\n"
                    << "  br i1 %more, label %fill, label %finish\n"
                    << "finish:\n"
                    << "  %cnt32 = trunc i64 %count to i32\n"
                    << "  %a0 = insertvalue %array zeroinitializer, ptr %p, 0\n"
                    << "  %a1 = insertvalue %array %a0, i32 %cnt32, 1\n"
                    << "  %a = insertvalue %array %a1, i32 %cnt32, 2\n"
                    << "  ret %array %a\n}\n";
            }
            const auto slicesCopy = emitSlices;
            for (Type t : slicesCopy) {
                if (!doneSlices.insert(t).second) continue;
                progress = true;
                const Type elemT = elemOf(t);
                const std::string elemLlvm = llvmType(elemT);
                const std::string stride = strideExpr(elemT);
                const std::string fnName = "@tz.slice.t" + std::to_string(static_cast<int>(t));
                out << "define internal %array " << fnName
                    << "(%array %src, i64 %lo, i64 %hi, %diag %boundsMsg, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %len32 = extractvalue %array %src, 1\n"
                    << "  %len = zext i32 %len32 to i64\n"
                    << "  call void @tz.sliceBounds(i64 %len, i64 %lo, i64 %hi, %diag %boundsMsg)\n"
                    << "  %cnt = sub i64 %hi, %lo\n"
                    << "  %empty = icmp eq i64 %cnt, 0\n"
                    << "  br i1 %empty, label %zero, label %copy\n"
                    << "zero:\n  ret %array zeroinitializer\n"
                    << "copy:\n"
                    << "  %srcP = extractvalue %array %src, 0\n"
                    << "  %srcStart = getelementptr inbounds " << elemLlvm << ", ptr %srcP, i64 %lo\n"
                    << "  %dstP = call ptr @tz.allocBuf(i64 %cnt, i64 " << stride << ", %diag %boundsMsg, %diag %allocMsg)\n";
                if (!needsDrop(elemT)) {
                    out << "  %bytes = mul i64 %cnt, " << stride << "\n"
                        << "  call void @llvm.memcpy.p0.p0.i64(ptr %dstP, ptr %srcStart, i64 %bytes, i1 false)\n"
                        << "  br label %finish\n";
                } else {
                    const std::string elemClone = requestClone(elemT);
                    const std::string hAlign = heapAlignSuffix(elemT);
                    out << "  br label %loop\n"
                        << "loop:\n"
                        << "  %i = phi i64 [ 0, %copy ], [ %next, %loop ]\n"
                        << "  %sp = getelementptr inbounds " << elemLlvm << ", ptr %srcStart, i64 %i\n"
                        << "  %el = load " << elemLlvm << ", ptr %sp" << hAlign << "\n"
                        << "  %cl = call " << elemLlvm << " " << elemClone << "("
                        << cloneCallArgs(elemT, elemLlvm + " %el", "%diag %allocMsg") << ")\n"
                        << "  %dp = getelementptr inbounds " << elemLlvm << ", ptr %dstP, i64 %i\n"
                        << "  store " << elemLlvm << " %cl, ptr %dp" << hAlign << "\n"
                        << "  %next = add i64 %i, 1\n"
                        << "  %more = icmp ult i64 %next, %cnt\n"
                        << "  br i1 %more, label %loop, label %finish\n";
                }
                out << "finish:\n"
                    << "  %cnt32 = trunc i64 %cnt to i32\n"
                    << "  %a0 = insertvalue %array zeroinitializer, ptr %dstP, 0\n"
                    << "  %a1 = insertvalue %array %a0, i32 %cnt32, 1\n"
                    << "  %a = insertvalue %array %a1, i32 %cnt32, 2\n"
                    << "  ret %array %a\n}\n";
            }
            const auto clearsCopy = emitClears;
            for (Type t : clearsCopy) {
                if (!doneClears.insert(t).second) continue;
                progress = true;
                const Type elemT = elemOf(t);
                const std::string elemLlvm = llvmType(elemT);
                const std::string elemDrop = requestDrop(elemT);
                const std::string fnName = "@tz.clear.t" + std::to_string(static_cast<int>(t));
                out << "define internal void " << fnName << "(ptr %slot) sanitize_address {\nentry:\n"
                    << "  %arr = load %array, ptr %slot\n"
                    << "  %len32 = extractvalue %array %arr, 1\n"
                    << "  %len = zext i32 %len32 to i64\n"
                    << "  %buf = extractvalue %array %arr, 0\n"
                    << "  %empty = icmp eq i64 %len, 0\n"
                    << "  br i1 %empty, label %done, label %loop\n"
                    << "loop:\n"
                    << "  %rem = phi i64 [ %len, %entry ], [ %idx, %loop ]\n"
                    << "  %idx = sub i64 %rem, 1\n"
                    << "  %ep = getelementptr inbounds " << elemLlvm << ", ptr %buf, i64 %idx\n"
                    << "  call void " << elemDrop << "(ptr %ep)\n"
                    << "  %more = icmp ne i64 %idx, 0\n"
                    << "  br i1 %more, label %loop, label %done\n"
                    << "done:\n"
                    << "  %lp = getelementptr inbounds %array, ptr %slot, i32 0, i32 1\n"
                    << "  store i32 0, ptr %lp\n"
                    << "  ret void\n}\n";
            }
            const auto atsCopy = emitAts;
            for (Type t : atsCopy) {
                if (!doneAts.insert(t).second) continue;
                progress = true;
                const Type elemT = elemOf(t);
                const std::string elemLlvm = llvmType(elemT);
                const std::string fnName = "@tz.at.t" + std::to_string(static_cast<int>(t));
                out << "define internal ptr " << fnName << "(%array %a, i64 %index, %diag %msg) sanitize_address {\n"
                    << "  %n32 = extractvalue %array %a, 1\n"
                    << "  %n = zext i32 %n32 to i64\n"
                    << "  %bad = icmp uge i64 %index, %n\n"
                    << "  call void @tz.guard(i1 %bad, %diag %msg)\n"
                    << "  %p = extractvalue %array %a, 0\n"
                    << "  %ptr = getelementptr inbounds " << elemLlvm << ", ptr %p, i64 %index\n"
                    << "  ret ptr %ptr\n}\n";
            }
            const auto pushesCopy = emitPushes;
            for (Type t : pushesCopy) {
                if (!donePushes.insert(t).second) continue;
                progress = true;
                const Type elemT = elemOf(t);
                const std::string elemLlvm = llvmType(elemT);
                const std::string fnName = "@tz.push.t" + std::to_string(static_cast<int>(t));
                out << "define internal void " << fnName << "(ptr %slot, " << elemLlvm
                    << " %val, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %arr0 = load %array, ptr %slot\n"
                    << "  %len32 = extractvalue %array %arr0, 1\n"
                    << "  %len = zext i32 %len32 to i64\n"
                    << "  %need = add i64 %len, 1\n"
                    << "  call void @tz.growBuf(ptr %slot, i64 " << strideExpr(elemT) << ", i64 %need, %diag %allocMsg)\n"
                    << "  %pf = getelementptr inbounds %array, ptr %slot, i32 0, i32 0\n"
                    << "  %buf = load ptr, ptr %pf\n"
                    << "  %dst = getelementptr inbounds " << elemLlvm << ", ptr %buf, i64 %len\n"
                    << "  store " << elemLlvm << " %val, ptr %dst" << heapAlignSuffix(elemT) << "\n"
                    << "  %lf = getelementptr inbounds %array, ptr %slot, i32 0, i32 1\n"
                    << "  %newLen32 = add i32 %len32, 1\n"
                    << "  store i32 %newLen32, ptr %lf\n"
                    << "  ret void\n}\n";
            }
            const auto popsCopy = emitPops;
            for (Type t : popsCopy) {
                if (!donePops.insert(t).second) continue;
                progress = true;
                const Type elemT = elemOf(t);
                const std::string elemLlvm = llvmType(elemT);
                const std::string fnName = "@tz.pop.t" + std::to_string(static_cast<int>(t));
                const std::string hAlign = heapAlignSuffix(elemT);
                out << "define internal " << elemLlvm << " " << fnName << "(ptr %slot, %diag %boundsMsg) sanitize_address {\nentry:\n"
                    << "  %arr = load %array, ptr %slot\n"
                    << "  %len32 = extractvalue %array %arr, 1\n"
                    << "  %empty = icmp eq i32 %len32, 0\n"
                    << "  call void @tz.guard(i1 %empty, %diag %boundsMsg)\n"
                    << "  %newLen32 = sub i32 %len32, 1\n"
                    << "  %idx = zext i32 %newLen32 to i64\n"
                    << "  %buf = extractvalue %array %arr, 0\n"
                    << "  %ep = getelementptr inbounds " << elemLlvm << ", ptr %buf, i64 %idx\n"
                    << "  %res = load " << elemLlvm << ", ptr %ep" << hAlign << "\n";
                if (needsDrop(elemT)) {
                    out << "  store " << elemLlvm << " zeroinitializer, ptr %ep" << hAlign << "\n";
                }
                out << "  %lf = getelementptr inbounds %array, ptr %slot, i32 0, i32 1\n"
                    << "  store i32 %newLen32, ptr %lf\n"
                    << "  ret " << elemLlvm << " %res\n}\n";
            }
            const auto vClonesCopy = emitVClones;
            for (Type t : vClonesCopy) {
                if (!doneVClones.insert(t).second) continue;
                progress = true;
                const std::string tLlvm = llvmType(t);
                const std::string vcName = "@tz.vclone.t" + std::to_string(static_cast<int>(t));
                const std::string fnClone = requestClone(t);
                out << "define internal void " << vcName << "(ptr %dst, ptr %src, ptr %diagPtr) sanitize_address {\nentry:\n"
                    << "  %d = load %diag, ptr %diagPtr\n"
                    << "  %v = load " << tLlvm << ", ptr %src\n"
                    << "  %cv = call " << tLlvm << " " << fnClone << "("
                    << cloneCallArgs(t, tLlvm + " %v", "%diag %d") << ")\n"
                    << "  store " << tLlvm << " %cv, ptr %dst\n"
                    << "  ret void\n}\n";
            }
            if (emitTblSecure && !doneTblSecure) {
                doneTblSecure = true;
                progress = true;
                usedDecls.insert("tz_col_table_init_secure");
                out << "define internal %table @tz.tblSecure() sanitize_address {\nentry:\n"
                    << "  %t = alloca %table\n"
                    << "  call void @tz_col_table_init_secure(ptr %t)\n"
                    << "  %res = load %table, ptr %t\n"
                    << "  ret %table %res\n}\n";
            }
            const auto withCapsCopy = emitTblWithCaps;
            for (Type t : withCapsCopy) {
                if (!doneTblWithCaps.insert(t).second) continue;
                progress = true;
                const Type kt = program.isMap(t) ? program.mapKey(t) : program.setElem(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                const Type vt = program.isMap(t) ? program.mapVal(t) : Type::Void;
                const std::string vs = (program.isMap(t) && !isZeroSized(vt)) ? strideExpr(vt) : "0";
                usedDecls.insert("tz_col_table_reserve");
                out << "define internal %table @tz.tblWithCap.t" << static_cast<int>(t)
                    << "(i64 %n, %diag %sizeMsg, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %neg = icmp slt i64 %n, 0\n"
                    << "  call void @tz.guard(i1 %neg, %diag %sizeMsg)\n"
                    << "  %t = alloca %table\n"
                    << "  store %table zeroinitializer, ptr %t\n"
                    << "  %ok = call i8 @tz_col_table_reserve(ptr %t, i64 %n, i64 " << ks
                    << ", i64 " << vs << ", i32 " << kk << ")\n"
                    << "  %bad = icmp eq i8 %ok, 0\n"
                    << "  call void @tz.guard(i1 %bad, %diag %allocMsg)\n"
                    << "  %res = load %table, ptr %t\n"
                    << "  ret %table %res\n}\n";
            }
            const auto tblClearsCopy = emitTblClears;
            for (Type t : tblClearsCopy) {
                if (!doneTblClears.insert(t).second) continue;
                progress = true;
                const Type kt = program.isMap(t) ? program.mapKey(t) : program.setElem(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                const Type vt = program.isMap(t) ? program.mapVal(t) : Type::Void;
                const std::string vs = (program.isMap(t) && !isZeroSized(vt)) ? strideExpr(vt) : "0";
                const std::string vdrop = (program.isMap(t) && needsDrop(vt)) ? requestDrop(vt) : "null";
                usedDecls.insert("tz_col_table_clear");
                out << "define internal void @tz.tblClear.t" << static_cast<int>(t)
                    << "(ptr %slot) sanitize_address {\nentry:\n"
                    << "  call void @tz_col_table_clear(ptr %slot, i32 " << kk << ", i64 " << ks
                    << ", i64 " << vs << ", ptr " << vdrop << ")\n"
                    << "  ret void\n}\n";
            }
            const auto tblReservesCopy = emitTblReserves;
            for (Type t : tblReservesCopy) {
                if (!doneTblReserves.insert(t).second) continue;
                progress = true;
                const Type kt = program.isMap(t) ? program.mapKey(t) : program.setElem(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                const Type vt = program.isMap(t) ? program.mapVal(t) : Type::Void;
                const std::string vs = (program.isMap(t) && !isZeroSized(vt)) ? strideExpr(vt) : "0";
                usedDecls.insert("tz_col_table_reserve");
                out << "define internal void @tz.tblReserve.t" << static_cast<int>(t)
                    << "(ptr %slot, i64 %n, %diag %sizeMsg, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %neg = icmp slt i64 %n, 0\n"
                    << "  call void @tz.guard(i1 %neg, %diag %sizeMsg)\n"
                    << "  %ok = call i8 @tz_col_table_reserve(ptr %slot, i64 %n, i64 " << ks
                    << ", i64 " << vs << ", i32 " << kk << ")\n"
                    << "  %bad = icmp eq i8 %ok, 0\n"
                    << "  call void @tz.guard(i1 %bad, %diag %allocMsg)\n"
                    << "  ret void\n}\n";
            }
            const auto mapSetsCopy = emitMapSets;
            for (Type t : mapSetsCopy) {
                if (!doneMapSets.insert(t).second) continue;
                progress = true;
                const Type kt = program.mapKey(t), vt = program.mapVal(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                const bool zeroV = isZeroSized(vt);
                const std::string vs = zeroV ? "0" : strideExpr(vt);
                const std::string vdrop = needsDrop(vt) ? requestDrop(vt) : "null";
                usedDecls.insert("tz_col_map_set");
                out << "define internal void @tz.mapSet.t" << static_cast<int>(t)
                    << "(ptr %slot, " << llvmType(kt) << " %k, " << llvmType(vt)
                    << " %v, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %k.tmp = alloca " << keyStorageLlvm(kt) << "\n";
                emitStoreKey(out, kt, "%k", "%k.tmp", "%ks");
                std::string vPtrExpr = "null";
                if (!zeroV) {
                    out << "  %v.tmp = alloca " << llvmType(vt) << "\n"
                        << "  store " << llvmType(vt) << " %v, ptr %v.tmp\n";
                    vPtrExpr = "%v.tmp";
                }
                out << "  %ok = call i8 @tz_col_map_set(ptr %slot, ptr %k.tmp, ptr " << vPtrExpr
                    << ", i32 " << kk << ", i64 " << ks << ", i64 " << vs << ", ptr " << vdrop << ")\n"
                    << "  %bad = icmp eq i8 %ok, 0\n"
                    << "  call void @tz.guard(i1 %bad, %diag %allocMsg)\n"
                    << "  ret void\n}\n";
            }
            const auto mapGetsCopy = emitMapGets;
            for (const auto& [t, optT] : mapGetsCopy) {
                if (!doneMapGets.insert(t).second) continue;
                progress = true;
                const Type kt = program.mapKey(t), vt = program.mapVal(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                const bool zeroV = isZeroSized(vt);
                const std::string vs = zeroV ? "0" : strideExpr(vt);
                const std::string optLlvm = llvmType(optT), vLlvm = llvmType(vt);
                usedDecls.insert("tz_col_map_find");
                out << "define internal " << optLlvm << " @tz.mapGet.t" << static_cast<int>(t)
                    << "(%table %t, " << llvmType(kt) << " %k, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %m.tmp = alloca %table\n"
                    << "  store %table %t, ptr %m.tmp\n"
                    << "  %k.tmp = alloca " << keyStorageLlvm(kt) << "\n";
                emitStoreKey(out, kt, "%k", "%k.tmp", "%kg");
                out << "  %vp = call ptr @tz_col_map_find(ptr %m.tmp, ptr %k.tmp, i32 " << kk
                    << ", i64 " << ks << ", i64 " << vs << ")\n"
                    << "  %miss = icmp eq ptr %vp, null\n"
                    << "  br i1 %miss, label %none, label %some\n"
                    << "none:\n  ret " << optLlvm << " zeroinitializer\n"
                    << "some:\n";
                if (zeroV) {
                    out << "  %r0 = insertvalue " << optLlvm << " zeroinitializer, i8 1, 0\n"
                        << "  ret " << optLlvm << " %r0\n}\n";
                } else {
                    out << "  %vraw = load " << vLlvm << ", ptr %vp\n";
                    std::string packedVal = "%vraw";
                    if (needsDrop(vt)) {
                        const std::string vclone = requestClone(vt);
                        out << "  %vc = call " << vLlvm << " " << vclone << "("
                            << cloneCallArgs(vt, vLlvm + " %vraw", "%diag %allocMsg") << ")\n";
                        packedVal = "%vc";
                    }
                    out << "  %r0 = insertvalue " << optLlvm << " zeroinitializer, i8 1, 0\n"
                        << "  %r1 = insertvalue " << optLlvm << " %r0, " << vLlvm << " " << packedVal << ", 1\n"
                        << "  ret " << optLlvm << " %r1\n}\n";
                }
            }
            const auto mapHasCopy = emitMapHas;
            for (Type t : mapHasCopy) {
                if (!doneMapHas.insert(t).second) continue;
                progress = true;
                const Type kt = program.mapKey(t), vt = program.mapVal(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                const std::string vs = isZeroSized(vt) ? "0" : strideExpr(vt);
                usedDecls.insert("tz_col_map_find");
                out << "define internal i1 @tz.mapHas.t" << static_cast<int>(t)
                    << "(%table %t, " << llvmType(kt) << " %k) sanitize_address {\nentry:\n"
                    << "  %m.tmp = alloca %table\n"
                    << "  store %table %t, ptr %m.tmp\n"
                    << "  %k.tmp = alloca " << keyStorageLlvm(kt) << "\n";
                emitStoreKey(out, kt, "%k", "%k.tmp", "%kh");
                out << "  %vp = call ptr @tz_col_map_find(ptr %m.tmp, ptr %k.tmp, i32 " << kk
                    << ", i64 " << ks << ", i64 " << vs << ")\n"
                    << "  %ok = icmp ne ptr %vp, null\n"
                    << "  ret i1 %ok\n}\n";
            }
            const auto mapDelsCopy = emitMapDels;
            for (Type t : mapDelsCopy) {
                if (!doneMapDels.insert(t).second) continue;
                progress = true;
                const Type kt = program.mapKey(t), vt = program.mapVal(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                const std::string vs = isZeroSized(vt) ? "0" : strideExpr(vt);
                const std::string vdrop = needsDrop(vt) ? requestDrop(vt) : "null";
                usedDecls.insert("tz_col_map_del");
                out << "define internal i1 @tz.mapDel.t" << static_cast<int>(t)
                    << "(ptr %slot, " << llvmType(kt) << " %k) sanitize_address {\nentry:\n"
                    << "  %k.tmp = alloca " << keyStorageLlvm(kt) << "\n";
                emitStoreKey(out, kt, "%k", "%k.tmp", "%kd");
                out << "  %rc = call i8 @tz_col_map_del(ptr %slot, ptr %k.tmp, i32 " << kk
                    << ", i64 " << ks << ", i64 " << vs << ", ptr " << vdrop << ")\n"
                    << "  %ok = icmp ne i8 %rc, 0\n"
                    << "  ret i1 %ok\n}\n";
            }
            const auto setAddsCopy = emitSetAdds;
            for (Type t : setAddsCopy) {
                if (!doneSetAdds.insert(t).second) continue;
                progress = true;
                const Type kt = program.setElem(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                usedDecls.insert("tz_col_set_add");
                out << "define internal i1 @tz.setAdd.t" << static_cast<int>(t)
                    << "(ptr %slot, " << llvmType(kt) << " %k, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %k.tmp = alloca " << keyStorageLlvm(kt) << "\n";
                emitStoreKey(out, kt, "%k", "%k.tmp", "%sa");
                out << "  %rc = call i8 @tz_col_set_add(ptr %slot, ptr %k.tmp, i32 " << kk
                    << ", i64 " << ks << ")\n"
                    << "  %bad = icmp eq i8 %rc, 0\n"
                    << "  call void @tz.guard(i1 %bad, %diag %allocMsg)\n"
                    << "  %added = icmp eq i8 %rc, 2\n"
                    << "  ret i1 %added\n}\n";
            }
            const auto setHasCopy = emitSetHas;
            for (Type t : setHasCopy) {
                if (!doneSetHas.insert(t).second) continue;
                progress = true;
                const Type kt = program.setElem(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                usedDecls.insert("tz_col_map_find");
                out << "define internal i1 @tz.setHas.t" << static_cast<int>(t)
                    << "(%table %t, " << llvmType(kt) << " %k) sanitize_address {\nentry:\n"
                    << "  %m.tmp = alloca %table\n"
                    << "  store %table %t, ptr %m.tmp\n"
                    << "  %k.tmp = alloca " << keyStorageLlvm(kt) << "\n";
                emitStoreKey(out, kt, "%k", "%k.tmp", "%sh");
                out << "  %vp = call ptr @tz_col_map_find(ptr %m.tmp, ptr %k.tmp, i32 " << kk
                    << ", i64 " << ks << ", i64 0)\n"
                    << "  %ok = icmp ne ptr %vp, null\n"
                    << "  ret i1 %ok\n}\n";
            }
            const auto setDelsCopy = emitSetDels;
            for (Type t : setDelsCopy) {
                if (!doneSetDels.insert(t).second) continue;
                progress = true;
                const Type kt = program.setElem(t);
                const int kk = keyKindOf(kt);
                const std::string ks = keyStrideExpr(kt);
                usedDecls.insert("tz_col_map_del");
                out << "define internal i1 @tz.setDel.t" << static_cast<int>(t)
                    << "(ptr %slot, " << llvmType(kt) << " %k) sanitize_address {\nentry:\n"
                    << "  %k.tmp = alloca " << keyStorageLlvm(kt) << "\n";
                emitStoreKey(out, kt, "%k", "%k.tmp", "%sd");
                out << "  %rc = call i8 @tz_col_map_del(ptr %slot, ptr %k.tmp, i32 " << kk
                    << ", i64 " << ks << ", i64 0, ptr null)\n"
                    << "  %ok = icmp ne i8 %rc, 0\n"
                    << "  ret i1 %ok\n}\n";
            }
            auto emitCollectKeys = [&](Type t, const std::string& fnPfx, Type kt) {
                const std::string ks = keyStrideExpr(kt);
                const std::string kStorage = keyStorageLlvm(kt);
                const std::string kLlvm = llvmType(kt);
                out << "define internal %array " << fnPfx << static_cast<int>(t)
                    << "(%table %t, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %len32 = extractvalue %table %t, 1\n"
                    << "  %len = zext i32 %len32 to i64\n"
                    << "  %empty = icmp eq i64 %len, 0\n"
                    << "  br i1 %empty, label %zero, label %alloc\n"
                    << "zero:\n  ret %array zeroinitializer\n"
                    << "alloc:\n"
                    << "  %dstP = call ptr @tz.allocBuf(i64 %len, i64 " << strideExpr(kt)
                    << ", %diag %allocMsg, %diag %allocMsg)\n"
                    << "  %data = extractvalue %table %t, 0\n"
                    << "  %b32 = extractvalue %table %t, 2\n"
                    << "  %b = zext i32 %b32 to i64\n"
                    << "  %ctrlEnd = add i64 %b, 16\n"
                    << "  %keysBase = getelementptr inbounds i8, ptr %data, i64 %ctrlEnd\n"
                    << "  br label %loop\n"
                    << "loop:\n"
                    << "  %i = phi i64 [ 0, %alloc ], [ %nextI, %step ]\n"
                    << "  %outIdx = phi i64 [ 0, %alloc ], [ %nextOut, %step ]\n"
                    << "  %cp = getelementptr inbounds i8, ptr %data, i64 %i\n"
                    << "  %cb = load i8, ptr %cp\n"
                    << "  %occ = icmp sge i8 %cb, 0\n"
                    << "  br i1 %occ, label %emit, label %skip\n"
                    << "emit:\n"
                    << "  %kp = getelementptr inbounds " << kStorage << ", ptr %keysBase, i64 %i\n";
                emitLoadKey(out, kt, "%kp", "%kraw", "%kl");
                std::string storeVal = "%kraw";
                if (kt == Type::Str) {
                    const std::string kclone = requestClone(kt);
                    out << "  %kc = call %text " << kclone << "(%text %kraw, %diag %allocMsg)\n";
                    storeVal = "%kc";
                }
                (void)ks;
                out << "  %dp = getelementptr inbounds " << kLlvm << ", ptr %dstP, i64 %outIdx\n"
                    << "  store " << kLlvm << " " << storeVal << ", ptr %dp\n"
                    << "  %outInc = add i64 %outIdx, 1\n"
                    << "  br label %step\n"
                    << "skip:\n  br label %step\n"
                    << "step:\n"
                    << "  %nextOut = phi i64 [ %outInc, %emit ], [ %outIdx, %skip ]\n"
                    << "  %nextI = add i64 %i, 1\n"
                    << "  %more = icmp ult i64 %nextI, %b\n"
                    << "  br i1 %more, label %loop, label %finish\n"
                    << "finish:\n"
                    << "  %a0 = insertvalue %array zeroinitializer, ptr %dstP, 0\n"
                    << "  %a1 = insertvalue %array %a0, i32 %len32, 1\n"
                    << "  %a = insertvalue %array %a1, i32 %len32, 2\n"
                    << "  ret %array %a\n}\n";
            };
            const auto mapKeysCopy = emitMapKeys;
            for (Type t : mapKeysCopy) {
                if (!doneMapKeys.insert(t).second) continue;
                progress = true;
                emitCollectKeys(t, "@tz.mapKeys.t", program.mapKey(t));
            }
            const auto setItemsCopy = emitSetItems;
            for (Type t : setItemsCopy) {
                if (!doneSetItems.insert(t).second) continue;
                progress = true;
                emitCollectKeys(t, "@tz.setItems.t", program.setElem(t));
            }
            const auto mapValsCopy = emitMapVals;
            for (Type t : mapValsCopy) {
                if (!emitDoneMapVals.insert(t).second) continue;
                progress = true;
                const Type kt = program.mapKey(t), vt = program.mapVal(t);
                const std::string ks = keyStrideExpr(kt);
                const std::string vLlvm = llvmType(vt);
                const bool zeroV = isZeroSized(vt);
                out << "define internal %array @tz.mapValues.t" << static_cast<int>(t)
                    << "(%table %t, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %len32 = extractvalue %table %t, 1\n"
                    << "  %len = zext i32 %len32 to i64\n"
                    << "  %empty = icmp eq i64 %len, 0\n"
                    << "  br i1 %empty, label %zero, label %alloc\n"
                    << "zero:\n  ret %array zeroinitializer\n"
                    << "alloc:\n"
                    << "  %dstP = call ptr @tz.allocBuf(i64 %len, i64 " << (zeroV ? "0" : strideExpr(vt))
                    << ", %diag %allocMsg, %diag %allocMsg)\n"
                    << "  %data = extractvalue %table %t, 0\n"
                    << "  %b32 = extractvalue %table %t, 2\n"
                    << "  %b = zext i32 %b32 to i64\n"
                    << "  %ctrlEnd = add i64 %b, 16\n"
                    << "  %keyBytes = mul i64 %b, " << ks << "\n"
                    << "  %valsOff = add i64 %ctrlEnd, %keyBytes\n"
                    << "  %valsBase = getelementptr inbounds i8, ptr %data, i64 %valsOff\n"
                    << "  br label %loop\n"
                    << "loop:\n"
                    << "  %i = phi i64 [ 0, %alloc ], [ %nextI, %step ]\n"
                    << "  %outIdx = phi i64 [ 0, %alloc ], [ %nextOut, %step ]\n"
                    << "  %cp = getelementptr inbounds i8, ptr %data, i64 %i\n"
                    << "  %cb = load i8, ptr %cp\n"
                    << "  %occ = icmp sge i8 %cb, 0\n"
                    << "  br i1 %occ, label %emit, label %skip\n"
                    << "emit:\n";
                if (!zeroV) {
                    out << "  %vp = getelementptr inbounds " << vLlvm << ", ptr %valsBase, i64 %i\n"
                        << "  %vraw = load " << vLlvm << ", ptr %vp\n";
                    std::string storeVal = "%vraw";
                    if (needsDrop(vt)) {
                        const std::string vclone = requestClone(vt);
                        out << "  %vc = call " << vLlvm << " " << vclone << "("
                            << cloneCallArgs(vt, vLlvm + " %vraw", "%diag %allocMsg") << ")\n";
                        storeVal = "%vc";
                    }
                    out << "  %dp = getelementptr inbounds " << vLlvm << ", ptr %dstP, i64 %outIdx\n"
                        << "  store " << vLlvm << " " << storeVal << ", ptr %dp\n";
                }
                out << "  %outInc = add i64 %outIdx, 1\n"
                    << "  br label %step\n"
                    << "skip:\n  br label %step\n"
                    << "step:\n"
                    << "  %nextOut = phi i64 [ %outInc, %emit ], [ %outIdx, %skip ]\n"
                    << "  %nextI = add i64 %i, 1\n"
                    << "  %more = icmp ult i64 %nextI, %b\n"
                    << "  br i1 %more, label %loop, label %finish\n"
                    << "finish:\n"
                    << "  %a0 = insertvalue %array zeroinitializer, ptr %dstP, 0\n"
                    << "  %a1 = insertvalue %array %a0, i32 %len32, 1\n"
                    << "  %a = insertvalue %array %a1, i32 %len32, 2\n"
                    << "  ret %array %a\n}\n";
            }
            const auto poolWithCapsCopy = emitPoolWithCaps;
            for (Type t : poolWithCapsCopy) {
                if (!donePoolWithCaps.insert(t).second) continue;
                progress = true;
                const Type et = program.poolElem(t);
                const std::string es = isZeroSized(et) ? "0" : strideExpr(et);
                usedDecls.insert("tz_col_pool_reserve");
                out << "define internal %pool @tz.poolWithCap.t" << static_cast<int>(t)
                    << "(i64 %n, %diag %sizeMsg, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %neg = icmp slt i64 %n, 0\n"
                    << "  call void @tz.guard(i1 %neg, %diag %sizeMsg)\n"
                    << "  %slot = alloca %pool\n"
                    << "  store %pool zeroinitializer, ptr %slot\n"
                    << "  %pos = icmp sgt i64 %n, 0\n"
                    << "  br i1 %pos, label %alloc, label %done\n"
                    << "alloc:\n"
                    << "  %ok = call i8 @tz_col_pool_reserve(ptr %slot, i64 %n, i64 " << es << ")\n"
                    << "  %bad = icmp eq i8 %ok, 0\n"
                    << "  call void @tz.guard(i1 %bad, %diag %allocMsg)\n"
                    << "  br label %done\n"
                    << "done:\n"
                    << "  %res = load %pool, ptr %slot\n"
                    << "  ret %pool %res\n}\n";
            }
            const auto poolReservesCopy = emitPoolReserves;
            for (Type t : poolReservesCopy) {
                if (!donePoolReserves.insert(t).second) continue;
                progress = true;
                const Type et = program.poolElem(t);
                const std::string es = isZeroSized(et) ? "0" : strideExpr(et);
                usedDecls.insert("tz_col_pool_reserve");
                out << "define internal void @tz.poolReserve.t" << static_cast<int>(t)
                    << "(ptr %slot, i64 %n, %diag %sizeMsg, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %neg = icmp slt i64 %n, 0\n"
                    << "  call void @tz.guard(i1 %neg, %diag %sizeMsg)\n"
                    << "  %pos = icmp sgt i64 %n, 0\n"
                    << "  br i1 %pos, label %alloc, label %done\n"
                    << "alloc:\n"
                    << "  %ok = call i8 @tz_col_pool_reserve(ptr %slot, i64 %n, i64 " << es << ")\n"
                    << "  %bad = icmp eq i8 %ok, 0\n"
                    << "  call void @tz.guard(i1 %bad, %diag %allocMsg)\n"
                    << "  br label %done\n"
                    << "done:\n  ret void\n}\n";
            }
            const auto poolClearsCopy = emitPoolClears;
            for (Type t : poolClearsCopy) {
                if (!donePoolClears.insert(t).second) continue;
                progress = true;
                const Type et = program.poolElem(t);
                const std::string es = isZeroSized(et) ? "0" : strideExpr(et);
                const std::string edrop = needsDrop(et) ? requestDrop(et) : "null";
                usedDecls.insert("tz_col_pool_clear");
                out << "define internal void @tz.poolClear.t" << static_cast<int>(t)
                    << "(ptr %slot) sanitize_address {\nentry:\n"
                    << "  call void @tz_col_pool_clear(ptr %slot, i64 " << es << ", ptr " << edrop << ")\n"
                    << "  ret void\n}\n";
            }
            const auto poolAddsCopy = emitPoolAdds;
            for (Type t : poolAddsCopy) {
                if (!donePoolAdds.insert(t).second) continue;
                progress = true;
                const Type et = program.poolElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string es = zeroE ? "0" : strideExpr(et);
                usedDecls.insert("tz_col_pool_add");
                out << "define internal %handle @tz.poolAdd.t" << static_cast<int>(t)
                    << "(ptr %slot, " << eLlvm << " %v, %diag %allocMsg) sanitize_address {\nentry:\n";
                std::string vPtrExpr = "null";
                if (!zeroE) {
                    out << "  %v.tmp = alloca " << eLlvm << "\n"
                        << "  store " << eLlvm << " %v, ptr %v.tmp\n";
                    vPtrExpr = "%v.tmp";
                }
                out << "  %h.tmp = alloca %handle\n"
                    << "  %ok = call i8 @tz_col_pool_add(ptr %slot, ptr " << vPtrExpr << ", i64 " << es
                    << ", ptr %h.tmp)\n"
                    << "  %bad = icmp eq i8 %ok, 0\n"
                    << "  call void @tz.guard(i1 %bad, %diag %allocMsg)\n"
                    << "  %res = load %handle, ptr %h.tmp\n"
                    << "  ret %handle %res\n}\n";
            }
            const auto poolGetsCopy = emitPoolGets;
            for (const auto& [t, optT] : poolGetsCopy) {
                if (!donePoolGets.insert(t).second) continue;
                progress = true;
                const Type et = program.poolElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string es = zeroE ? "0" : strideExpr(et);
                const std::string optLlvm = llvmType(optT), eLlvm = llvmType(et);
                usedDecls.insert("tz_col_pool_get_ptr");
                out << "define internal " << optLlvm << " @tz.poolGet.t" << static_cast<int>(t)
                    << "(%pool %p, %handle %h, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %p.tmp = alloca %pool\n"
                    << "  store %pool %p, ptr %p.tmp\n"
                    << "  %idx = extractvalue %handle %h, 0\n"
                    << "  %gen = extractvalue %handle %h, 1\n"
                    << "  %vp = call ptr @tz_col_pool_get_ptr(ptr %p.tmp, i32 %idx, i32 %gen, i64 " << es << ")\n"
                    << "  %miss = icmp eq ptr %vp, null\n"
                    << "  br i1 %miss, label %none, label %some\n"
                    << "none:\n  ret " << optLlvm << " zeroinitializer\n"
                    << "some:\n";
                if (zeroE) {
                    out << "  %r0 = insertvalue " << optLlvm << " zeroinitializer, i8 1, 0\n"
                        << "  ret " << optLlvm << " %r0\n}\n";
                } else {
                    out << "  %vraw = load " << eLlvm << ", ptr %vp\n";
                    std::string packed = "%vraw";
                    if (needsDrop(et)) {
                        const std::string eclone = requestClone(et);
                        out << "  %vc = call " << eLlvm << " " << eclone << "("
                            << cloneCallArgs(et, eLlvm + " %vraw", "%diag %allocMsg") << ")\n";
                        packed = "%vc";
                    }
                    out << "  %r0 = insertvalue " << optLlvm << " zeroinitializer, i8 1, 0\n"
                        << "  %r1 = insertvalue " << optLlvm << " %r0, " << eLlvm << " " << packed << ", 1\n"
                        << "  ret " << optLlvm << " %r1\n}\n";
                }
            }
            const auto poolGetsMonoCopy = emitPoolGetsMono;
            for (const auto& [t, optT] : poolGetsMonoCopy) {
                if (!donePoolGetsMono.insert(t).second) continue;
                progress = true;
                const Type et = program.poolElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string optLlvm = llvmType(optT), eLlvm = llvmType(et);
                out << "define internal " << optLlvm << " @tz.poolGetMono.t" << static_cast<int>(t)
                    << "(%pool %p, %handle %h, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %idx = extractvalue %handle %h, 0\n"
                    << "  %gen = extractvalue %handle %h, 1\n"
                    << "  %used = extractvalue %pool %p, 3\n"
                    << "  %gok = icmp eq i32 %gen, 1\n"
                    << "  %iok = icmp ult i32 %idx, %used\n"
                    << "  %hit = and i1 %gok, %iok\n"
                    << "  br i1 %hit, label %some, label %none\n"
                    << "none:\n  ret " << optLlvm << " zeroinitializer\n"
                    << "some:\n";
                if (zeroE) {
                    out << "  %r0 = insertvalue " << optLlvm << " zeroinitializer, i8 1, 0\n"
                        << "  ret " << optLlvm << " %r0\n}\n";
                } else {
                    out << "  %data = extractvalue %pool %p, 0\n"
                        << "  %cap32 = extractvalue %pool %p, 2\n"
                        << "  %cap64 = zext i32 %cap32 to i64\n"
                        << "  %mbytes = mul i64 %cap64, 8\n"
                        << "  %mplus = add i64 %mbytes, 15\n"
                        << "  %voff = and i64 %mplus, -16\n"
                        << "  %vbase = getelementptr inbounds i8, ptr %data, i64 %voff\n"
                        << "  %idx64 = zext i32 %idx to i64\n"
                        << "  %vp = getelementptr inbounds " << eLlvm << ", ptr %vbase, i64 %idx64\n"
                        << "  %vraw = load " << eLlvm << ", ptr %vp\n";
                    std::string packed = "%vraw";
                    if (needsDrop(et)) {
                        const std::string eclone = requestClone(et);
                        out << "  %vc = call " << eLlvm << " " << eclone << "("
                            << cloneCallArgs(et, eLlvm + " %vraw", "%diag %allocMsg") << ")\n";
                        packed = "%vc";
                    }
                    out << "  %r0 = insertvalue " << optLlvm << " zeroinitializer, i8 1, 0\n"
                        << "  %r1 = insertvalue " << optLlvm << " %r0, " << eLlvm << " " << packed << ", 1\n"
                        << "  ret " << optLlvm << " %r1\n}\n";
                }
            }
            const auto poolHasCopy = emitPoolHas;
            for (Type t : poolHasCopy) {
                if (!donePoolHas.insert(t).second) continue;
                progress = true;
                const Type et = program.poolElem(t);
                const std::string es = isZeroSized(et) ? "0" : strideExpr(et);
                usedDecls.insert("tz_col_pool_get_ptr");
                out << "define internal i1 @tz.poolHas.t" << static_cast<int>(t)
                    << "(%pool %p, %handle %h) sanitize_address {\nentry:\n"
                    << "  %p.tmp = alloca %pool\n"
                    << "  store %pool %p, ptr %p.tmp\n"
                    << "  %idx = extractvalue %handle %h, 0\n"
                    << "  %gen = extractvalue %handle %h, 1\n"
                    << "  %vp = call ptr @tz_col_pool_get_ptr(ptr %p.tmp, i32 %idx, i32 %gen, i64 " << es << ")\n"
                    << "  %ok = icmp ne ptr %vp, null\n"
                    << "  ret i1 %ok\n}\n";
            }
            const auto poolHasMonoCopy = emitPoolHasMono;
            for (Type t : poolHasMonoCopy) {
                if (!donePoolHasMono.insert(t).second) continue;
                progress = true;
                out << "define internal i1 @tz.poolHasMono.t" << static_cast<int>(t)
                    << "(%pool %p, %handle %h) sanitize_address {\nentry:\n"
                    << "  %idx = extractvalue %handle %h, 0\n"
                    << "  %gen = extractvalue %handle %h, 1\n"
                    << "  %used = extractvalue %pool %p, 3\n"
                    << "  %gok = icmp eq i32 %gen, 1\n"
                    << "  %iok = icmp ult i32 %idx, %used\n"
                    << "  %ok = and i1 %gok, %iok\n"
                    << "  ret i1 %ok\n}\n";
            }
            const auto poolSetsCopy = emitPoolSets;
            for (Type t : poolSetsCopy) {
                if (!donePoolSets.insert(t).second) continue;
                progress = true;
                const Type et = program.poolElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string es = zeroE ? "0" : strideExpr(et);
                usedDecls.insert("tz_col_pool_get_ptr");
                out << "define internal i1 @tz.poolSet.t" << static_cast<int>(t)
                    << "(ptr %slot, %handle %h, " << eLlvm << " %v) sanitize_address {\nentry:\n"
                    << "  %idx = extractvalue %handle %h, 0\n"
                    << "  %gen = extractvalue %handle %h, 1\n"
                    << "  %vp = call ptr @tz_col_pool_get_ptr(ptr %slot, i32 %idx, i32 %gen, i64 " << es << ")\n"
                    << "  %hit = icmp ne ptr %vp, null\n"
                    << "  br i1 %hit, label %found, label %miss\n"
                    << "found:\n";
                if (!zeroE) {
                    if (needsDrop(et))
                        out << "  call void " << requestDrop(et) << "(ptr %vp)\n";
                    out << "  store " << eLlvm << " %v, ptr %vp\n";
                }
                out << "  ret i1 true\n"
                    << "miss:\n";
                if (!zeroE && needsDrop(et)) {
                    out << "  %v.tmp = alloca " << eLlvm << "\n"
                        << "  store " << eLlvm << " %v, ptr %v.tmp\n"
                        << "  call void " << requestDrop(et) << "(ptr %v.tmp)\n";
                }
                out << "  ret i1 false\n}\n";
            }
            const auto poolRemovesCopy = emitPoolRemoves;
            for (const auto& [t, optT] : poolRemovesCopy) {
                if (!donePoolRemoves.insert(t).second) continue;
                progress = true;
                const Type et = program.poolElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string optLlvm = llvmType(optT), eLlvm = llvmType(et);
                const std::string es = zeroE ? "0" : strideExpr(et);
                usedDecls.insert("tz_col_pool_remove");
                out << "define internal " << optLlvm << " @tz.poolRemove.t" << static_cast<int>(t)
                    << "(ptr %slot, %handle %h) sanitize_address {\nentry:\n"
                    << "  %idx = extractvalue %handle %h, 0\n"
                    << "  %gen = extractvalue %handle %h, 1\n";
                if (zeroE) {
                    out << "  %ok = call i8 @tz_col_pool_remove(ptr %slot, i32 %idx, i32 %gen, ptr null, i64 0)\n"
                        << "  %hit = icmp ne i8 %ok, 0\n"
                        << "  br i1 %hit, label %some, label %none\n"
                        << "none:\n  ret " << optLlvm << " zeroinitializer\n"
                        << "some:\n  %r0 = insertvalue " << optLlvm << " zeroinitializer, i8 1, 0\n"
                        << "  ret " << optLlvm << " %r0\n}\n";
                } else {
                    out << "  %v.tmp = alloca " << eLlvm << "\n"
                        << "  %ok = call i8 @tz_col_pool_remove(ptr %slot, i32 %idx, i32 %gen, ptr %v.tmp, i64 " << es << ")\n"
                        << "  %hit = icmp ne i8 %ok, 0\n"
                        << "  br i1 %hit, label %some, label %none\n"
                        << "none:\n  ret " << optLlvm << " zeroinitializer\n"
                        << "some:\n"
                        << "  %v = load " << eLlvm << ", ptr %v.tmp\n"
                        << "  %r0 = insertvalue " << optLlvm << " zeroinitializer, i8 1, 0\n"
                        << "  %r1 = insertvalue " << optLlvm << " %r0, " << eLlvm << " %v, 1\n"
                        << "  ret " << optLlvm << " %r1\n}\n";
                }
            }
            const auto poolHandlesCopy = emitPoolHandles;
            for (Type t : poolHandlesCopy) {
                if (!donePoolHandles.insert(t).second) continue;
                progress = true;
                usedDecls.insert("tz_col_pool_handles");
                out << "define internal %array @tz.poolHandles.t" << static_cast<int>(t)
                    << "(%pool %p, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %len32 = extractvalue %pool %p, 1\n"
                    << "  %empty = icmp eq i32 %len32, 0\n"
                    << "  br i1 %empty, label %emptyRet, label %alloc\n"
                    << "emptyRet:\n  ret %array zeroinitializer\n"
                    << "alloc:\n"
                    << "  %len64 = zext i32 %len32 to i64\n"
                    << "  %bytes = mul i64 %len64, 8\n"
                    << "  %buf = call noalias ptr @tz.malloc(i64 %bytes, %diag %allocMsg)\n"
                    << "  %p.tmp = alloca %pool\n"
                    << "  store %pool %p, ptr %p.tmp\n"
                    << "  %n = call i32 @tz_col_pool_handles(ptr %p.tmp, ptr %buf)\n"
                    << "  %a0 = insertvalue %array zeroinitializer, ptr %buf, 0\n"
                    << "  %a1 = insertvalue %array %a0, i32 %n, 1\n"
                    << "  %a2 = insertvalue %array %a1, i32 %len32, 2\n"
                    << "  ret %array %a2\n}\n";
            }
            const auto rcNewsCopy = emitRcNews;
            for (Type t : rcNewsCopy) {
                if (!doneRcNews.insert(t).second) continue;
                progress = true;
                const Type et = program.sharedElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + eLlvm + " }");
                out << "define internal ptr @tz.rcNew.t" << static_cast<int>(t)
                    << "(" << eLlvm << " %v, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %p = call noalias ptr @tz.malloc(i64 ptrtoint (ptr getelementptr (" << cellTy
                    << ", ptr null, i32 1) to i64), %diag %allocMsg)\n"
                    << "  store i64 1, ptr %p\n";
                if (!zeroE) {
                    out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %p, i32 0, i32 1\n"
                        << "  store " << eLlvm << " %v, ptr %vp\n";
                }
                out << "  ret ptr %p\n}\n";
            }
            const auto rcGetsCopy = emitRcGets;
            for (Type t : rcGetsCopy) {
                if (!doneRcGets.insert(t).second) continue;
                progress = true;
                const Type et = program.sharedElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + eLlvm + " }");
                out << "define internal " << eLlvm << " @tz.rcGet.t" << static_cast<int>(t)
                    << "(ptr %p, %diag %allocMsg) sanitize_address {\nentry:\n";
                if (zeroE) {
                    out << "  ret " << eLlvm << " zeroinitializer\n}\n";
                } else {
                    out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %p, i32 0, i32 1\n"
                        << "  %raw = load " << eLlvm << ", ptr %vp\n";
                    if (needsDrop(et)) {
                        const std::string eclone = requestClone(et);
                        out << "  %vc = call " << eLlvm << " " << eclone << "("
                            << cloneCallArgs(et, eLlvm + " %raw", "%diag %allocMsg") << ")\n"
                            << "  ret " << eLlvm << " %vc\n}\n";
                    } else {
                        out << "  ret " << eLlvm << " %raw\n}\n";
                    }
                }
            }
            const auto rcSetsCopy = emitRcSets;
            for (Type t : rcSetsCopy) {
                if (!doneRcSets.insert(t).second) continue;
                progress = true;
                const Type et = program.sharedElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + eLlvm + " }");
                const std::string rcNewFn = requestRcNew(t);
                out << "define internal void @tz.rcSet.t" << static_cast<int>(t)
                    << "(ptr %slot, " << eLlvm << " %v, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %p = load ptr, ptr %slot\n"
                    << "  %rc = load i64, ptr %p\n"
                    << "  %uniq = icmp eq i64 %rc, 1\n"
                    << "  br i1 %uniq, label %reuse, label %cow\n"
                    << "reuse:\n";
                if (!zeroE) {
                    out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %p, i32 0, i32 1\n";
                    if (needsDrop(et))
                        out << "  call void " << requestDrop(et) << "(ptr %vp)\n";
                    out << "  store " << eLlvm << " %v, ptr %vp\n";
                }
                out << "  ret void\n"
                    << "cow:\n"
                    << "  %np = call ptr " << rcNewFn << "(" << eLlvm << " %v, %diag %allocMsg)\n"
                    << "  %next = sub i64 %rc, 1\n"
                    << "  store i64 %next, ptr %p\n"
                    << "  store ptr %np, ptr %slot\n"
                    << "  ret void\n}\n";
            }
            const auto rcReusesCopy = emitRcReuses;
            for (Type t : rcReusesCopy) {
                if (!doneRcReuses.insert(t).second) continue;
                progress = true;
                const Type et = program.sharedElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + eLlvm + " }");
                const std::string rcNewFn = requestRcNew(t);
                out << "define internal ptr @tz.rcReuse.t" << static_cast<int>(t)
                    << "(ptr %old, " << eLlvm << " %v, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %isnull = icmp eq ptr %old, null\n"
                    << "  br i1 %isnull, label %allocNew, label %checkRc\n"
                    << "checkRc:\n"
                    << "  %rc = load i64, ptr %old\n"
                    << "  %uniq = icmp eq i64 %rc, 1\n"
                    << "  br i1 %uniq, label %reuseIt, label %decOld\n"
                    << "reuseIt:\n";
                if (!zeroE) {
                    out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %old, i32 0, i32 1\n";
                    if (needsDrop(et))
                        out << "  call void " << requestDrop(et) << "(ptr %vp)\n";
                    out << "  store " << eLlvm << " %v, ptr %vp\n";
                }
                out << "  ret ptr %old\n"
                    << "decOld:\n"
                    << "  %next = sub i64 %rc, 1\n"
                    << "  store i64 %next, ptr %old\n"
                    << "  br label %allocNew\n"
                    << "allocNew:\n"
                    << "  %np = call ptr " << rcNewFn << "(" << eLlvm << " %v, %diag %allocMsg)\n"
                    << "  ret ptr %np\n}\n";
            }
            const auto rcUpdatesCopy = emitRcUpdates;
            for (Type t : rcUpdatesCopy) {
                if (!doneRcUpdates.insert(t).second) continue;
                progress = true;
                const Type et = program.sharedElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + eLlvm + " }");
                const std::string rcNewFn = requestRcNew(t);
                out << "define internal void @tz.rcUpdate.t" << static_cast<int>(t)
                    << "(i32 %depth, ptr %slot, %fn %f, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %p = load ptr, ptr %slot\n";
                if (!zeroE) {
                    out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %p, i32 0, i32 1\n"
                        << "  %cur = load " << eLlvm << ", ptr %vp" << heapAlignSuffix(et) << "\n";
                }
                out << "  %fenv = extractvalue %fn %f, 0\n"
                    << "  %fvt = extractvalue %fn %f, 1\n"
                    << "  %ffp = getelementptr inbounds ptr, ptr %fvt, i64 2\n"
                    << "  %ffn = load ptr, ptr %ffp\n"
                    << "  %newv = call " << eLlvm << " %ffn(i32 %depth, ptr %fenv, " << eLlvm << " "
                    << (zeroE ? "zeroinitializer" : "%cur") << ")\n"
                    << "  %rc = load i64, ptr %p\n"
                    << "  %uniq = icmp eq i64 %rc, 1\n"
                    << "  br i1 %uniq, label %reuse, label %cow\n"
                    << "reuse:\n";
                if (!zeroE) {
                    if (needsDrop(et))
                        out << "  call void " << requestDrop(et) << "(ptr %vp)\n";
                    out << "  store " << eLlvm << " %newv, ptr %vp\n";
                }
                out << "  ret void\n"
                    << "cow:\n"
                    << "  %np = call ptr " << rcNewFn << "(" << eLlvm << " %newv, %diag %allocMsg)\n"
                    << "  %next = sub i64 %rc, 1\n"
                    << "  store i64 %next, ptr %p\n"
                    << "  store ptr %np, ptr %slot\n"
                    << "  ret void\n}\n";
            }
            auto emitSlotDrop = [&](bool flag, bool& done, const char* fnName, const char* sysClose) {
                if (!flag || done) return;
                done = true;
                progress = true;
                usedDecls.insert(sysClose);
                out << "define internal void " << fnName << "(ptr %slot) sanitize_address {\nentry:\n"
                    << "  call void @" << sysClose << "(ptr %slot)\n"
                    << "  ret void\n}\n";
            };
            emitSlotDrop(usesDropFile, doneDropFile, "@tz.dropFile", "tz_sys_file_close");
            emitSlotDrop(usesDropListener, doneDropListener, "@tz.dropListener", "tz_sys_tcp_close");
            emitSlotDrop(usesDropSocket, doneDropSocket, "@tz.dropSocket", "tz_sys_sock_close");
            emitSlotDrop(usesDropMutex, doneDropMutex, "@tz.dropMutex", "tz_sys_mutex_drop");
            emitSlotDrop(usesDropLock, doneDropLock, "@tz.dropLock", "tz_sys_lock_unlock");
            emitSlotDrop(usesDropScope, doneDropScope, "@tz.dropScope", "tz_conc_scope_drop");
            emitSlotDrop(usesDropTask, doneDropTask, "@tz.dropTask", "tz_conc_task_drop");
            emitSlotDrop(usesDropRegion, doneDropRegion, "@tz.dropRegion", "tz_sys_region_drop");
            emitSlotDrop(usesDropRing, doneDropRing, "@tz.dropRing", "tz_conc_ring_release");

            if (usesDropAtomic && !doneDropAtomic) {
                doneDropAtomic = true;
                progress = true;
                out << "define internal void @tz.dropAtomic(ptr %slot) sanitize_address {\nentry:\n"
                    << "  %p = load ptr, ptr %slot\n"
                    << "  %isnull = icmp eq ptr %p, null\n"
                    << "  br i1 %isnull, label %done, label %dec\n"
                    << "dec:\n"
                    << "  store ptr null, ptr %slot\n"
                    << "  %old = atomicrmw sub ptr %p, i64 1 acq_rel, align 8\n"
                    << "  %last = icmp eq i64 %old, 1\n"
                    << "  br i1 %last, label %freeIt, label %done\n"
                    << "freeIt:\n"
                    << "  call void @free(ptr %p)\n"
                    << "  br label %done\n"
                    << "done:\n  ret void\n}\n";
            }
            if (usesCloneAtomic && !doneCloneAtomic) {
                doneCloneAtomic = true;
                progress = true;
                out << "define internal ptr @tz.cloneAtomic(ptr %src, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %isnull = icmp eq ptr %src, null\n"
                    << "  br i1 %isnull, label %done, label %inc\n"
                    << "inc:\n"
                    << "  %old = atomicrmw add ptr %src, i64 1 monotonic, align 8\n"
                    << "  br label %done\n"
                    << "done:\n  ret ptr %src\n}\n";
            }
            if (usesCloneRing && !doneCloneRing) {
                doneCloneRing = true;
                progress = true;
                usedDecls.insert("tz_conc_ring_retain");
                out << "define internal ptr @tz.cloneRing(ptr %src, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %res = call ptr @tz_conc_ring_retain(ptr %src)\n"
                    << "  ret ptr %res\n}\n";
            }
            const auto ringPushesCopy = emitRingPushes;
            for (Type rqT : ringPushesCopy) {
                if (!doneRingPushes.insert(rqT).second) continue;
                progress = true;
                usedDecls.insert("tz_conc_ring_try_push");
                const Type et = program.ringQueueElem(rqT);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                out << "define internal i1 @tz.ringPush.t" << static_cast<int>(rqT)
                    << "(ptr %q, " << eLlvm << " %val) sanitize_address {\nentry:\n";
                if (zeroE) {
                    out << "  %ok8 = call i8 @tz_conc_ring_try_push(ptr %q, ptr null)\n"
                        << "  %ok = icmp ne i8 %ok8, 0\n"
                        << "  ret i1 %ok\n}\n";
                } else {
                    const std::string aAlign = allocaAlignSuffix(et);
                    out << "  %tmp = alloca " << eLlvm << aAlign << "\n"
                        << "  store " << eLlvm << " %val, ptr %tmp" << aAlign << "\n"
                        << "  %ok8 = call i8 @tz_conc_ring_try_push(ptr %q, ptr %tmp)\n"
                        << "  %ok = icmp ne i8 %ok8, 0\n";
                    if (needsDrop(et)) {
                        const std::string edrop = requestDrop(et);
                        out << "  br i1 %ok, label %done, label %failDrop\n"
                            << "failDrop:\n"
                            << "  call void " << edrop << "(ptr %tmp)\n"
                            << "  br label %done\n"
                            << "done:\n"
                            << "  ret i1 %ok\n}\n";
                    } else {
                        out << "  ret i1 %ok\n}\n";
                    }
                }
            }
            const auto ringPopsCopy = emitRingPops;
            for (const auto& [rqT, optT] : ringPopsCopy) {
                if (!doneRingPops.emplace(rqT, optT).second) continue;
                progress = true;
                usedDecls.insert("tz_conc_ring_try_pop");
                const Type et = program.ringQueueElem(rqT);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string optLlvm = llvmType(optT);
                const auto& lay = layoutOf(optT);
                const unsigned noneIdx = variantIndexByName(optT, "None");
                const unsigned someIdx = variantIndexByName(optT, "Some");
                const unsigned phys = lay.singlePayLogicalToLlvm.at(0);
                out << "define internal " << optLlvm << " @tz.ringPop.t" << static_cast<int>(rqT)
                    << "(ptr %q) sanitize_address {\nentry:\n";
                if (zeroE) {
                    out << "  %ok8 = call i8 @tz_conc_ring_try_pop(ptr %q, ptr null)\n"
                        << "  %ok = icmp ne i8 %ok8, 0\n"
                        << "  %none = insertvalue " << optLlvm << " zeroinitializer, " << lay.tagTy << " " << noneIdx << ", 0\n"
                        << "  %some = insertvalue " << optLlvm << " zeroinitializer, " << lay.tagTy << " " << someIdx << ", 0\n"
                        << "  %res = select i1 %ok, " << optLlvm << " %some, " << optLlvm << " %none\n"
                        << "  ret " << optLlvm << " %res\n}\n";
                } else {
                    const std::string aAlign = allocaAlignSuffix(et);
                    out << "  %tmp = alloca " << eLlvm << aAlign << "\n"
                        << "  %ok8 = call i8 @tz_conc_ring_try_pop(ptr %q, ptr %tmp)\n"
                        << "  %ok = icmp ne i8 %ok8, 0\n"
                        << "  br i1 %ok, label %hit, label %miss\n"
                        << "hit:\n"
                        << "  %val = load " << eLlvm << ", ptr %tmp" << aAlign << "\n"
                        << "  %s0 = insertvalue " << optLlvm << " zeroinitializer, " << lay.tagTy << " " << someIdx << ", 0\n"
                        << "  %s1 = insertvalue " << optLlvm << " %s0, " << eLlvm << " %val, " << phys << "\n"
                        << "  ret " << optLlvm << " %s1\n"
                        << "miss:\n"
                        << "  %none = insertvalue " << optLlvm << " zeroinitializer, " << lay.tagTy << " " << noneIdx << ", 0\n"
                        << "  ret " << optLlvm << " %none\n}\n";
                }
            }

            const auto fzNewsCopy = emitFzNews;
            for (Type t : fzNewsCopy) {
                if (!doneFzNews.insert(t).second) continue;
                progress = true;
                const Type et = program.frozenElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + eLlvm + " }");
                out << "define internal ptr @tz.fzNew.t" << static_cast<int>(t)
                    << "(" << eLlvm << " %v, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %p = call noalias ptr @tz.malloc(i64 ptrtoint (ptr getelementptr (" << cellTy
                    << ", ptr null, i32 1) to i64), %diag %allocMsg)\n"
                    << "  store i64 1, ptr %p\n";
                if (!zeroE) {
                    out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %p, i32 0, i32 1\n"
                        << "  store " << eLlvm << " %v, ptr %vp" << heapAlignSuffix(et) << "\n";
                }
                out << "  ret ptr %p\n}\n";
            }

            const auto fzGetsCopy = emitFzGets;
            for (Type t : fzGetsCopy) {
                if (!doneFzGets.insert(t).second) continue;
                progress = true;
                const Type et = program.frozenElem(t);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + eLlvm + " }");
                out << "define internal " << eLlvm << " @tz.fzGet.t" << static_cast<int>(t)
                    << "(ptr %p, %diag %allocMsg) sanitize_address {\nentry:\n";
                if (zeroE) {
                    out << "  ret " << eLlvm << " zeroinitializer\n}\n";
                } else {
                    out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %p, i32 0, i32 1\n"
                        << "  %raw = load " << eLlvm << ", ptr %vp" << heapAlignSuffix(et) << "\n";
                    if (needsDrop(et)) {
                        const std::string eclone = requestClone(et);
                        out << "  %vc = call " << eLlvm << " " << eclone << "("
                            << cloneCallArgs(et, eLlvm + " %raw", "%diag %allocMsg") << ")\n"
                            << "  ret " << eLlvm << " %vc\n}\n";
                    } else {
                        out << "  ret " << eLlvm << " %raw\n}\n";
                    }
                }
            }

            const auto fzReadsCopy = emitFzReads;
            for (const auto& [fzT, retT] : fzReadsCopy) {
                if (!doneFzReads.emplace(fzT, retT).second) continue;
                progress = true;
                const Type et = program.frozenElem(fzT);
                const bool zeroE = isZeroSized(et);
                const std::string eLlvm = llvmType(et);
                const std::string retLlvm = llvmType(retT);
                const std::string cellTy = zeroE ? "{ i64 }" : ("{ i64, " + eLlvm + " }");
                out << "define internal " << retLlvm << " @tz.fzRead.t" << static_cast<int>(fzT)
                    << ".r" << static_cast<int>(retT) << "(i32 %depth, ptr %p, %fn %f) sanitize_address {\nentry:\n";
                if (!zeroE) {
                    out << "  %vp = getelementptr inbounds " << cellTy << ", ptr %p, i32 0, i32 1\n"
                        << "  %cur = load " << eLlvm << ", ptr %vp" << heapAlignSuffix(et) << "\n";
                }
                out << "  %fenv = extractvalue %fn %f, 0\n"
                    << "  %fvt = extractvalue %fn %f, 1\n"
                    << "  %ffp = getelementptr inbounds ptr, ptr %fvt, i64 2\n"
                    << "  %ffn = load ptr, ptr %ffp\n"
                    << "  ";
                if (retT != Type::Void) out << "%res = ";
                out << "call " << retLlvm << " %ffn(i32 %depth, ptr %fenv, " << eLlvm << " "
                    << (zeroE ? "zeroinitializer" : "%cur") << ")\n";
                if (retT == Type::Void) out << "  ret void\n}\n";
                else out << "  ret " << retLlvm << " %res\n}\n";
            }

            const auto taskSpawnsCopy = emitTaskSpawns;
            for (const auto& [valT, retT] : taskSpawnsCopy) {
                if (!doneTaskSpawns.emplace(valT, retT).second) continue;
                progress = true;
                usesFnRuntime = true;
                const bool hasRes = (retT != Type::Void && !isZeroSized(retT));
                const bool hasVal = (valT != Type::Void && !isZeroSized(valT));
                std::string payTy = "{ ";
                if (hasRes) payTy += llvmType(retT) + ", ";
                payTy += "%fn";
                if (hasVal) payTy += ", " + llvmType(valT);
                payTy += " }";
                const unsigned fnIdx = hasRes ? 1u : 0u;
                const unsigned valIdx = hasRes ? 2u : 1u;
                const std::string retLlvm = llvmType(retT);
                out << "define internal void @tz.task.run.v" << static_cast<int>(valT)
                    << ".r" << static_cast<int>(retT) << "(ptr %pay) sanitize_address {\nentry:\n"
                    << "  %fnp = getelementptr inbounds " << payTy << ", ptr %pay, i32 0, i32 " << fnIdx << "\n"
                    << "  %f = load %fn, ptr %fnp\n";
                if (hasVal) {
                    const std::string vLlvm = llvmType(valT);
                    const std::string hAlign = heapAlignSuffix(valT);
                    out << "  %valp = getelementptr inbounds " << payTy << ", ptr %pay, i32 0, i32 " << valIdx << "\n"
                        << "  %val = load " << vLlvm << ", ptr %valp" << hAlign << "\n"
                        << "  store " << vLlvm << " zeroinitializer, ptr %valp" << hAlign << "\n";
                }
                out << "  %fenv = extractvalue %fn %f, 0\n"
                    << "  %fvt = extractvalue %fn %f, 1\n"
                    << "  %ffp = getelementptr inbounds ptr, ptr %fvt, i64 2\n"
                    << "  %ffn = load ptr, ptr %ffp\n"
                    << "  ";
                if (retT != Type::Void) out << "%res = ";
                out << "call " << retLlvm << " %ffn(i32 1, ptr %fenv";
                if (valT != Type::Void) {
                    out << ", " << llvmType(valT) << " " << (hasVal ? "%val" : "zeroinitializer");
                }
                out << ")\n";
                if (hasRes) {
                    out << "  store " << retLlvm << " %res, ptr %pay" << heapAlignSuffix(retT) << "\n";
                }
                out << "  call void @tz.dropFn(ptr %fnp)\n"
                    << "  ret void\n}\n";
            }

            const auto pmapsCopy = emitPmaps;
            for (Type arrT : pmapsCopy) {
                if (!donePmaps.insert(arrT).second) continue;
                progress = true;
                usedDecls.insert("tz_conc_parallel_for");
                const Type elemT = elemOf(arrT);
                const bool zeroE = isZeroSized(elemT);
                const std::string eLlvm = llvmType(elemT);
                const std::string es = zeroE ? "0" : strideExpr(elemT);
                const std::string tid = std::to_string(static_cast<int>(arrT));
                const std::string ctxTy = "{ ptr, %fn, i32 }";
                out << "define internal void @tz.pmap.chunk.t" << tid << "(ptr %ctx, i64 %lo, i64 %hi) sanitize_address {\nentry:\n"
                    << "  %bufp = getelementptr inbounds " << ctxTy << ", ptr %ctx, i32 0, i32 0\n"
                    << "  %buf = load ptr, ptr %bufp\n"
                    << "  %cbp = getelementptr inbounds " << ctxTy << ", ptr %ctx, i32 0, i32 1\n"
                    << "  %cb = load %fn, ptr %cbp\n"
                    << "  %dp = getelementptr inbounds " << ctxTy << ", ptr %ctx, i32 0, i32 2\n"
                    << "  %depth = load i32, ptr %dp\n"
                    << "  %fenv = extractvalue %fn %cb, 0\n"
                    << "  %fvt = extractvalue %fn %cb, 1\n"
                    << "  %ffp = getelementptr inbounds ptr, ptr %fvt, i64 2\n"
                    << "  %ffn = load ptr, ptr %ffp\n"
                    << "  %ok = icmp slt i64 %lo, %hi\n"
                    << "  br i1 %ok, label %loop, label %done\n"
                    << "loop:\n"
                    << "  %i = phi i64 [ %lo, %entry ], [ %next, %loop ]\n"
                    << "  %val = call " << eLlvm << " %ffn(i32 %depth, ptr %fenv, i64 %i)\n";
                if (!zeroE) {
                    out << "  %ep = getelementptr inbounds " << eLlvm << ", ptr %buf, i64 %i\n"
                        << "  store " << eLlvm << " %val, ptr %ep\n";
                }
                out << "  %next = add i64 %i, 1\n"
                    << "  %more = icmp slt i64 %next, %hi\n"
                    << "  br i1 %more, label %loop, label %done\n"
                    << "done:\n"
                    << "  ret void\n}\n"
                    << "define internal %array @tz.pmap.t" << tid
                    << "(i32 %depth, i64 %n, %fn %cb, %diag %sizeMsg, %diag %allocMsg) sanitize_address {\nentry:\n"
                    << "  %buf = call ptr @tz.allocBuf(i64 %n, i64 " << es << ", %diag %sizeMsg, %diag %allocMsg)\n"
                    << "  %empty = icmp eq i64 %n, 0\n"
                    << "  br i1 %empty, label %zero, label %run\n"
                    << "zero:\n"
                    << "  ret %array zeroinitializer\n"
                    << "run:\n"
                    << "  %ctx = alloca " << ctxTy << "\n"
                    << "  %p0 = getelementptr inbounds " << ctxTy << ", ptr %ctx, i32 0, i32 0\n"
                    << "  store ptr %buf, ptr %p0\n"
                    << "  %p1 = getelementptr inbounds " << ctxTy << ", ptr %ctx, i32 0, i32 1\n"
                    << "  store %fn %cb, ptr %p1\n"
                    << "  %p2 = getelementptr inbounds " << ctxTy << ", ptr %ctx, i32 0, i32 2\n"
                    << "  store i32 %depth, ptr %p2\n"
                    << "  call void @tz_conc_parallel_for(i64 %n, ptr %ctx, ptr @tz.pmap.chunk.t" << tid << ")\n"
                    << "  %n32 = trunc i64 %n to i32\n"
                    << "  %a0 = insertvalue %array zeroinitializer, ptr %buf, 0\n"
                    << "  %a1 = insertvalue %array %a0, i32 %n32, 1\n"
                    << "  %a2 = insertvalue %array %a1, i32 %n32, 2\n"
                    << "  ret %array %a2\n}\n";
            }

            const auto sysCopy = emitSysHelpers;
            for (const auto& k : sysCopy) {
                if (!doneSysHelpers.insert(k).second) continue;
                progress = true;
                const std::string resLlvm = llvmType(k.resType);
                const std::string hfn = "@tz.sysH." + std::to_string(static_cast<int>(k.kind)) +
                                        ".t" + std::to_string(static_cast<int>(k.resType));
                const std::string okFn = requestResOk(k.resType);
                const std::string errFn = requestResErr(k.resType);
                auto emitFail = [&]() {
                    out << "fail:\n"
                        << "  %ev = load { ptr, i64 }, ptr %err\n"
                        << "  %ep = extractvalue { ptr, i64 } %ev, 0\n"
                        << "  %en = extractvalue { ptr, i64 } %ev, 1\n"
                        << "  %eres = call " << resLlvm << " " << errFn << "(ptr %ep, i64 %en)\n"
                        << "  ret " << resLlvm << " %eres\n";
                };
                auto emitOwnedTextOk = [&]() {
                    out << "ok:\n"
                        << "  %ov = load { ptr, i64 }, ptr %out\n"
                        << "  %op = extractvalue { ptr, i64 } %ov, 0\n"
                        << "  %on = extractvalue { ptr, i64 } %ov, 1\n"
                        << "  %hasP = icmp ne ptr %op, null\n"
                        << "  %hasN = icmp sgt i64 %on, 0\n"
                        << "  %nonEmpty = and i1 %hasP, %hasN\n"
                        << "  br i1 %nonEmpty, label %ownedStr, label %emptyStr\n"
                        << "emptyStr:\n"
                        << "  %eok = call " << resLlvm << " " << okFn << "(%text zeroinitializer)\n"
                        << "  ret " << resLlvm << " %eok\n"
                        << "ownedStr:\n"
                        << "  %txt = call %text @tz.textFromOwned(ptr %op, i64 %on, %diag %allocMsg)\n"
                        << "  %sok = call " << resLlvm << " " << okFn << "(%text %txt)\n"
                        << "  ret " << resLlvm << " %sok\n";
                };
                switch (k.kind) {
                case SysOpKind::TryReadFile:
                    usedDecls.insert("tz_sys_try_read_file");
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %pp, i64 %pn, %diag %allocMsg) sanitize_address {\nentry:\n"
                        << "  %out = alloca { ptr, i64 }\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %out\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %ok8 = call i8 @tz_sys_try_read_file(ptr %pp, i64 %pn, ptr %out, ptr %err)\n"
                        << "  %bad = icmp eq i8 %ok8, 0\n"
                        << "  br i1 %bad, label %fail, label %ok\n";
                    emitFail();
                    emitOwnedTextOk();
                    out << "}\n";
                    break;
                case SysOpKind::TryWriteFile:
                    usedDecls.insert("tz_sys_try_write_file");
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %pp, i64 %pn, ptr %cp, i64 %cn) sanitize_address {\nentry:\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %ok8 = call i8 @tz_sys_try_write_file(ptr %pp, i64 %pn, ptr %cp, i64 %cn, ptr %err)\n"
                        << "  %bad = icmp eq i8 %ok8, 0\n"
                        << "  br i1 %bad, label %fail, label %ok\n";
                    emitFail();
                    out << "ok:\n"
                        << "  %res = call " << resLlvm << " " << okFn << "(i1 1)\n"
                        << "  ret " << resLlvm << " %res\n}\n";
                    break;
                case SysOpKind::FileOpen:
                    usedDecls.insert("tz_sys_file_open");
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %pp, i64 %pn, ptr %mp, i64 %mn) sanitize_address {\nentry:\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %h = call ptr @tz_sys_file_open(ptr %pp, i64 %pn, ptr %mp, i64 %mn, ptr %err)\n"
                        << "  %bad = icmp eq ptr %h, null\n"
                        << "  br i1 %bad, label %fail, label %ok\n";
                    emitFail();
                    out << "ok:\n"
                        << "  %res = call " << resLlvm << " " << okFn << "(ptr %h)\n"
                        << "  ret " << resLlvm << " %res\n}\n";
                    break;
                case SysOpKind::FileRead:
                case SysOpKind::SockRead: {
                    const char* cfn = (k.kind == SysOpKind::FileRead) ? "tz_sys_file_read" : "tz_sys_sock_read";
                    usedDecls.insert(cfn);
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %hp, i64 %maxBytes, %diag %sizeMsg, %diag %allocMsg) sanitize_address {\nentry:\n"
                        << "  %neg = icmp slt i64 %maxBytes, 0\n"
                        << "  call void @tz.guard(i1 %neg, %diag %sizeMsg)\n"
                        << "  %tooBig = icmp sgt i64 %maxBytes, 1073741823\n"
                        << "  call void @tz.guard(i1 %tooBig, %diag %allocMsg)\n"
                        << "  %out = alloca { ptr, i64 }\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %out\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %ok8 = call i8 @" << cfn << "(ptr %hp, i64 %maxBytes, ptr %out, ptr %err)\n"
                        << "  %bad = icmp eq i8 %ok8, 0\n"
                        << "  br i1 %bad, label %fail, label %ok\n";
                    emitFail();
                    emitOwnedTextOk();
                    out << "}\n";
                    break;
                }
                case SysOpKind::FileReadAll:
                    usedDecls.insert("tz_sys_file_read_all");
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %fp, %diag %allocMsg) sanitize_address {\nentry:\n"
                        << "  %out = alloca { ptr, i64 }\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %out\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %ok8 = call i8 @tz_sys_file_read_all(ptr %fp, ptr %out, ptr %err)\n"
                        << "  %bad = icmp eq i8 %ok8, 0\n"
                        << "  br i1 %bad, label %fail, label %ok\n";
                    emitFail();
                    emitOwnedTextOk();
                    out << "}\n";
                    break;
                case SysOpKind::FileReadLine: {
                    usedDecls.insert("tz_sys_file_read_line");
                    const unsigned okIdx = variantIndexByName(k.resType, "Ok");
                    const Type optT = program.declaration(k.resType)->variants.at(okIdx).fields.at(0).type;
                    const std::string optLlvm = llvmType(optT);
                    const unsigned noneIdx = variantIndexByName(optT, "None");
                    const unsigned someIdx = variantIndexByName(optT, "Some");
                    const auto& optLay = layoutOf(optT);
                    const unsigned somePhys = optLay.singlePayLogicalToLlvm.at(0);
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %fp, %diag %allocMsg) sanitize_address {\nentry:\n"
                        << "  %out = alloca { ptr, i64 }\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %out\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %code = call i32 @tz_sys_file_read_line(ptr %fp, ptr %out, ptr %err)\n"
                        << "  %isErr = icmp slt i32 %code, 0\n"
                        << "  br i1 %isErr, label %fail, label %okCheck\n";
                    emitFail();
                    out << "okCheck:\n"
                        << "  %isEof = icmp eq i32 %code, 0\n"
                        << "  br i1 %isEof, label %eof, label %line\n"
                        << "eof:\n"
                        << "  %optNone = insertvalue " << optLlvm << " zeroinitializer, " << optLay.tagTy << " " << noneIdx << ", 0\n"
                        << "  %resEof = call " << resLlvm << " " << okFn << "(" << optLlvm << " %optNone)\n"
                        << "  ret " << resLlvm << " %resEof\n"
                        << "line:\n"
                        << "  %ov = load { ptr, i64 }, ptr %out\n"
                        << "  %op = extractvalue { ptr, i64 } %ov, 0\n"
                        << "  %on = extractvalue { ptr, i64 } %ov, 1\n"
                        << "  %hasP = icmp ne ptr %op, null\n"
                        << "  %hasN = icmp sgt i64 %on, 0\n"
                        << "  %nonEmpty = and i1 %hasP, %hasN\n"
                        << "  br i1 %nonEmpty, label %ownedStr, label %emptyStr\n"
                        << "emptyStr:\n"
                        << "  %s0e = insertvalue " << optLlvm << " zeroinitializer, " << optLay.tagTy << " " << someIdx << ", 0\n"
                        << "  %resEmpty = call " << resLlvm << " " << okFn << "(" << optLlvm << " %s0e)\n"
                        << "  ret " << resLlvm << " %resEmpty\n"
                        << "ownedStr:\n"
                        << "  %txt = call %text @tz.textFromOwned(ptr %op, i64 %on, %diag %allocMsg)\n"
                        << "  %s0 = insertvalue " << optLlvm << " zeroinitializer, " << optLay.tagTy << " " << someIdx << ", 0\n"
                        << "  %s1 = insertvalue " << optLlvm << " %s0, %text %txt, " << somePhys << "\n"
                        << "  %resLine = call " << resLlvm << " " << okFn << "(" << optLlvm << " %s1)\n"
                        << "  ret " << resLlvm << " %resLine\n}\n";
                    break;
                }
                case SysOpKind::FileWrite:
                case SysOpKind::SockWrite: {
                    const char* cfn = (k.kind == SysOpKind::FileWrite) ? "tz_sys_file_write" : "tz_sys_sock_write";
                    usedDecls.insert(cfn);
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %hp, ptr %dp, i64 %dn) sanitize_address {\nentry:\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %n = call i64 @" << cfn << "(ptr %hp, ptr %dp, i64 %dn, ptr %err)\n"
                        << "  %isErr = icmp slt i64 %n, 0\n"
                        << "  br i1 %isErr, label %fail, label %ok\n";
                    emitFail();
                    out << "ok:\n"
                        << "  %res = call " << resLlvm << " " << okFn << "(i64 %n)\n"
                        << "  ret " << resLlvm << " %res\n}\n";
                    break;
                }
                case SysOpKind::FileFlush: {
                    usedDecls.insert("tz_sys_file_flush");
                    const unsigned okIdx = variantIndexByName(k.resType, "Ok");
                    const auto& okFields = program.declaration(k.resType)->variants.at(okIdx).fields;
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %fp) sanitize_address {\nentry:\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %ok8 = call i8 @tz_sys_file_flush(ptr %fp, ptr %err)\n"
                        << "  %bad = icmp eq i8 %ok8, 0\n"
                        << "  br i1 %bad, label %fail, label %ok\n";
                    emitFail();
                    out << "ok:\n  %res = call " << resLlvm << " " << okFn << "(";
                    if (!okFields.empty()) {
                        if (okFields[0].type == Type::Bool) out << "i1 true";
                        else out << llvmType(okFields[0].type) << " 0";
                    }
                    out << ")\n  ret " << resLlvm << " %res\n}\n";
                    break;
                }
                case SysOpKind::TcpListen:
                case SysOpKind::TcpConnect: {
                    const char* cfn = (k.kind == SysOpKind::TcpListen) ? "tz_sys_tcp_listen" : "tz_sys_tcp_connect";
                    usedDecls.insert(cfn);
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %hp, i64 %hn, i64 %port) sanitize_address {\nentry:\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %h = call ptr @" << cfn << "(ptr %hp, i64 %hn, i64 %port, ptr %err)\n"
                        << "  %bad = icmp eq ptr %h, null\n"
                        << "  br i1 %bad, label %fail, label %ok\n";
                    emitFail();
                    out << "ok:\n"
                        << "  %res = call " << resLlvm << " " << okFn << "(ptr %h)\n"
                        << "  ret " << resLlvm << " %res\n}\n";
                    break;
                }
                case SysOpKind::TcpAccept:
                    usedDecls.insert("tz_sys_tcp_accept");
                    out << "define internal " << resLlvm << " " << hfn
                        << "(ptr %lp) sanitize_address {\nentry:\n"
                        << "  %err = alloca { ptr, i64 }\n"
                        << "  store { ptr, i64 } zeroinitializer, ptr %err\n"
                        << "  %h = call ptr @tz_sys_tcp_accept(ptr %lp, ptr %err)\n"
                        << "  %bad = icmp eq ptr %h, null\n"
                        << "  br i1 %bad, label %fail, label %ok\n";
                    emitFail();
                    out << "ok:\n"
                        << "  %res = call " << resLlvm << " " << okFn << "(ptr %h)\n"
                        << "  ret " << resLlvm << " %res\n}\n";
                    break;
                }
            }

            const auto resOksCopy = emitResOks;
            for (Type t : resOksCopy) {
                if (!doneResOks.insert(t).second) continue;
                progress = true;
                const auto& decl = *program.declaration(t);
                const auto& lay = layoutOf(t);
                const std::string tLlvm = llvmType(t);
                const unsigned okIdx = variantIndexByName(t, "Ok");
                const auto& okFields = decl.variants.at(okIdx).fields;
                const std::string fnName = "@tz.resOk.t" + std::to_string(static_cast<int>(t));
                if (okFields.empty()) {
                    out << "define internal " << tLlvm << " " << fnName << "() sanitize_address {\nentry:\n"
                        << "  %r = insertvalue " << tLlvm << " zeroinitializer, " << lay.tagTy << " " << okIdx << ", 0\n"
                        << "  ret " << tLlvm << " %r\n}\n";
                } else {
                    const Type okT = okFields[0].type;
                    const std::string okLlvm = llvmType(okT);
                    out << "define internal " << tLlvm << " " << fnName << "(" << okLlvm << " %val) sanitize_address {\nentry:\n";
                    if (!lay.multiPayload) {
                        const unsigned phys = lay.singlePayLogicalToLlvm.at(0);
                        out << "  %r0 = insertvalue " << tLlvm << " zeroinitializer, " << lay.tagTy << " " << okIdx << ", 0\n"
                            << "  %r1 = insertvalue " << tLlvm << " %r0, " << okLlvm << " %val, " << phys << "\n"
                            << "  ret " << tLlvm << " %r1\n}\n";
                    } else {
                        const unsigned phys = lay.variants.at(okIdx).logicalToLlvm.at(0);
                        out << "  %s = alloca " << tLlvm << allocaAlignSuffix(t) << "\n"
                            << "  store " << tLlvm << " zeroinitializer, ptr %s\n"
                            << "  %tagp = getelementptr inbounds " << tLlvm << ", ptr %s, i32 0, i32 0\n"
                            << "  store " << lay.tagTy << " " << okIdx << ", ptr %tagp\n"
                            << "  %pay = getelementptr inbounds " << tLlvm << ", ptr %s, i32 0, i32 " << lay.payFieldIdx << "\n"
                            << "  %fp = getelementptr inbounds " << tLlvm << ".p" << okIdx << ", ptr %pay, i32 0, i32 " << phys << "\n"
                            << "  store " << okLlvm << " %val, ptr %fp\n"
                            << "  %r = load " << tLlvm << ", ptr %s\n"
                            << "  ret " << tLlvm << " %r\n}\n";
                    }
                }
            }

            const auto resErrsCopy = emitResErrs;
            for (Type t : resErrsCopy) {
                if (!doneResErrs.insert(t).second) continue;
                progress = true;
                const auto& lay = layoutOf(t);
                const std::string tLlvm = llvmType(t);
                const unsigned errIdx = variantIndexByName(t, "Err");
                const std::string fnName = "@tz.resErr.t" + std::to_string(static_cast<int>(t));
                out << "define internal " << tLlvm << " " << fnName << "(ptr %ep, i64 %en) sanitize_address {\nentry:\n"
                    << "  %hasP = icmp ne ptr %ep, null\n"
                    << "  %hasN = icmp sgt i64 %en, 0\n"
                    << "  %okP = and i1 %hasP, %hasN\n"
                    << "  br i1 %okP, label %pack, label %packDone\n"
                    << "pack:\n"
                    << "  %pk = call %text @tz.packStatic(ptr %ep, i64 %en)\n"
                    << "  br label %packDone\n"
                    << "packDone:\n"
                    << "  %msg = phi %text [ zeroinitializer, %entry ], [ %pk, %pack ]\n";
                if (!lay.multiPayload) {
                    const unsigned phys = lay.singlePayLogicalToLlvm.at(0);
                    out << "  %r0 = insertvalue " << tLlvm << " zeroinitializer, " << lay.tagTy << " " << errIdx << ", 0\n"
                        << "  %r1 = insertvalue " << tLlvm << " %r0, %text %msg, " << phys << "\n"
                        << "  ret " << tLlvm << " %r1\n}\n";
                } else {
                    const unsigned phys = lay.variants.at(errIdx).logicalToLlvm.at(0);
                    out << "  %s = alloca " << tLlvm << allocaAlignSuffix(t) << "\n"
                        << "  store " << tLlvm << " zeroinitializer, ptr %s\n"
                        << "  %tagp = getelementptr inbounds " << tLlvm << ", ptr %s, i32 0, i32 0\n"
                        << "  store " << lay.tagTy << " " << errIdx << ", ptr %tagp\n"
                        << "  %pay = getelementptr inbounds " << tLlvm << ", ptr %s, i32 0, i32 " << lay.payFieldIdx << "\n"
                        << "  %fp = getelementptr inbounds " << tLlvm << ".p" << errIdx << ", ptr %pay, i32 0, i32 " << phys << "\n"
                        << "  store %text %msg, ptr %fp\n"
                        << "  %r = load " << tLlvm << ", ptr %s\n"
                        << "  ret " << tLlvm << " %r\n}\n";
                }
            }
        }
        if (usesTileRepeat) {
            out << "define internal void @tz.tile_repeat(ptr %base, i64 %n, i64 %period, i64 %elemBytes) sanitize_address {\n"
                << "entry:\n"
                << "  %badP = icmp sle i64 %period, 0\n"
                << "  %doneEarly = icmp sle i64 %n, %period\n"
                << "  %skip = or i1 %badP, %doneEarly\n"
                << "  br i1 %skip, label %done, label %prep\n"
                << "prep:\n"
                << "  %totalBytes = mul i64 %n, %elemBytes\n"
                << "  %pBytes0 = mul i64 %period, %elemBytes\n"
                << "  br label %loop\n"
                << "loop:\n"
                << "  %cur = phi i64 [ %pBytes0, %prep ], [ %next, %loop ]\n"
                << "  %rem = sub i64 %totalBytes, %cur\n"
                << "  %take = call i64 @llvm.smin.i64(i64 %cur, i64 %rem)\n"
                << "  %dst = getelementptr inbounds i8, ptr %base, i64 %cur\n"
                << "  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %base, i64 %take, i1 false)\n"
                << "  %next = add i64 %cur, %take\n"
                << "  %more = icmp slt i64 %next, %totalBytes\n"
                << "  br i1 %more, label %loop, label %done\n"
                << "done:\n"
                << "  ret void\n}\n";
        }
        for (std::uint8_t bits : usesFindFirstBits) {
            const unsigned lanes = 512u / static_cast<unsigned>(bits);
            const unsigned elemBytes = std::max(1u, static_cast<unsigned>(bits) / 8u);
            const std::string ety = "i" + std::to_string(static_cast<unsigned>(bits));
            const std::string vty = "<" + std::to_string(lanes) + " x " + ety + ">";
            const std::string bty = "<" + std::to_string(lanes) + " x i1>";
            const std::string mty = "i" + std::to_string(lanes);
            usedRawDecls.insert("declare " + mty + " @llvm.cttz." + mty + "(" + mty + ", i1 immarg)");
            out << "define internal i64 @tz.find_first." << ety << "(ptr %base, i64 %lo, i64 %hi, " << ety << " %target) sanitize_address {\n"
                << "entry:\n"
                << "  %splat0 = insertelement " << vty << " poison, " << ety << " %target, i32 0\n"
                << "  %splat = shufflevector " << vty << " %splat0, " << vty << " poison, <" << lanes << " x i32> zeroinitializer\n"
                << "  %lim = sub i64 %hi, " << lanes << "\n"
                << "  %hasVec = icmp sle i64 %lo, %lim\n"
                << "  br i1 %hasVec, label %vecLoop, label %tailCheck\n"
                << "vecLoop:\n"
                << "  %i = phi i64 [ %lo, %entry ], [ %iNext, %vecCont ]\n"
                << "  %p = getelementptr inbounds " << ety << ", ptr %base, i64 %i\n"
                << "  %v = load " << vty << ", ptr %p, align " << elemBytes << "\n"
                << "  %cmp = icmp eq " << vty << " %v, %splat\n"
                << "  %mask = bitcast " << bty << " %cmp to " << mty << "\n"
                << "  %hit = icmp ne " << mty << " %mask, 0\n"
                << "  br i1 %hit, label %vecHit, label %vecCont\n"
                << "vecHit:\n"
                << "  %tz = call " << mty << " @llvm.cttz." << mty << "(" << mty << " %mask, i1 true)\n";
            if (lanes < 64) {
                out << "  %tz64 = zext " << mty << " %tz to i64\n"
                    << "  %ansV = add i64 %i, %tz64\n";
            } else {
                out << "  %ansV = add i64 %i, %tz\n";
            }
            out << "  ret i64 %ansV\n"
                << "vecCont:\n"
                << "  %iNext = add i64 %i, " << lanes << "\n"
                << "  %moreVec = icmp sle i64 %iNext, %lim\n"
                << "  br i1 %moreVec, label %vecLoop, label %tailCheck\n"
                << "tailCheck:\n"
                << "  %iStart = phi i64 [ %lo, %entry ], [ %iNext, %vecCont ]\n"
                << "  %hasTail = icmp slt i64 %iStart, %hi\n"
                << "  br i1 %hasTail, label %tailLoop, label %miss\n"
                << "tailLoop:\n"
                << "  %j = phi i64 [ %iStart, %tailCheck ], [ %jNext, %tailCont ]\n"
                << "  %tp = getelementptr inbounds " << ety << ", ptr %base, i64 %j\n"
                << "  %tv = load " << ety << ", ptr %tp\n"
                << "  %eq = icmp eq " << ety << " %tv, %target\n"
                << "  br i1 %eq, label %tailHit, label %tailCont\n"
                << "tailHit:\n"
                << "  ret i64 %j\n"
                << "tailCont:\n"
                << "  %jNext = add i64 %j, 1\n"
                << "  %moreTail = icmp slt i64 %jNext, %hi\n"
                << "  br i1 %moreTail, label %tailLoop, label %miss\n"
                << "miss:\n"
                << "  ret i64 %hi\n}\n";
        }
        auto vecConstIota = [](unsigned lanes, unsigned bits) {
            std::string s = "<";
            const std::string ty = "i" + std::to_string(bits);
            for (unsigned i = 0; i < lanes; ++i) {
                if (i > 0) s += ", ";
                s += ty + " " + std::to_string(i);
            }
            return s + ">";
        };
        auto vecConstSplat = [](unsigned lanes, const std::string& ty, const std::string& val) {
            std::string s = "<";
            for (unsigned i = 0; i < lanes; ++i) {
                if (i > 0) s += ", ";
                s += ty + " " + val;
            }
            return s + ">";
        };
        if (usesGemmF32) {
            usedRawDecls.insert("declare <16 x float> @llvm.masked.load.v16f32.p0(ptr, i32 immarg, <16 x i1>, <16 x float>)");
            usedRawDecls.insert("declare void @llvm.masked.store.v16f32.p0(<16 x float>, ptr, i32 immarg, <16 x i1>)");
            const std::string iota16 = vecConstIota(16, 32);
            const std::string z16f = vecConstSplat(16, "float", "0.0");
            out << "define internal void @tz.gemm_f32_panel(ptr readonly %a, ptr readonly %b, ptr noalias %c, i64 %n) noinline sanitize_address {\n"
                << "entry:\n"
                << "  %bpack = alloca [2048 x float], align 64\n"
                << "  %apack = alloca [256 x float], align 64\n"
                << "  %badN = icmp sle i64 %n, 0\n"
                << "  br i1 %badN, label %done, label %jb32.hdr\n"
                << "jb32.hdr:\n"
                << "  %jb = phi i64 [ 0, %entry ], [ %jbNext, %jb32.latch ]\n"
                << "  %jb32 = add i64 %jb, 32\n"
                << "  %has32 = icmp sle i64 %jb32, %n\n"
                << "  br i1 %has32, label %kb32.hdr, label %jb16.check\n"
                << "kb32.hdr:\n"
                << "  %kb = phi i64 [ 0, %jb32.hdr ], [ %kbNext, %kb32.latch ]\n"
                << "  %kbCand = add i64 %kb, 64\n"
                << "  %kEnd = call i64 @llvm.smin.i64(i64 %kbCand, i64 %n)\n"
                << "  %kLen = sub i64 %kEnd, %kb\n"
                << "  br label %pack32.loop\n"
                << "pack32.loop:\n"
                << "  %pk = phi i64 [ 0, %kb32.hdr ], [ %pkNext, %pack32.loop ]\n"
                << "  %srcRow = add i64 %kb, %pk\n"
                << "  %srcMul = mul i64 %srcRow, %n\n"
                << "  %srcOff = add i64 %srcMul, %jb\n"
                << "  %sp0 = getelementptr inbounds float, ptr %b, i64 %srcOff\n"
                << "  %sp1 = getelementptr inbounds float, ptr %sp0, i64 16\n"
                << "  %pv0 = load <16 x float>, ptr %sp0, align 4\n"
                << "  %pv1 = load <16 x float>, ptr %sp1, align 4\n"
                << "  %dstOff = mul i64 %pk, 32\n"
                << "  %dp0 = getelementptr inbounds float, ptr %bpack, i64 %dstOff\n"
                << "  %dp1 = getelementptr inbounds float, ptr %dp0, i64 16\n"
                << "  store <16 x float> %pv0, ptr %dp0, align 64\n"
                << "  store <16 x float> %pv1, ptr %dp1, align 64\n"
                << "  %pkNext = add i64 %pk, 1\n"
                << "  %pkMore = icmp slt i64 %pkNext, %kLen\n"
                << "  br i1 %pkMore, label %pack32.loop, label %i4_32.hdr\n"
                << "i4_32.hdr:\n"
                << "  %i4 = phi i64 [ 0, %pack32.loop ], [ %i4Next, %i4_32.store ]\n"
                << "  %i4Lim = add i64 %i4, 4\n"
                << "  %hasI4_32 = icmp sle i64 %i4Lim, %n\n"
                << "  br i1 %hasI4_32, label %i4_32.prep, label %i1_32.hdr\n"
                << "i4_32.prep:\n"
                << "  %r0 = mul i64 %i4, %n\n"
                << "  %r1 = add i64 %r0, %n\n"
                << "  %r2 = add i64 %r1, %n\n"
                << "  %r3 = add i64 %r2, %n\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %ab" << r << " = add i64 %r" << r << ", %kb\n";
            }
            out << "  br label %packA4_32.loop\n"
                << "packA4_32.loop:\n"
                << "  %pa = phi i64 [ 0, %i4_32.prep ], [ %paNext, %packA4_32.loop ]\n"
                << "  %paBase = mul i64 %pa, 4\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %pao" << r << " = add i64 %ab" << r << ", %pa\n"
                    << "  %pap" << r << " = getelementptr inbounds float, ptr %a, i64 %pao" << r << "\n"
                    << "  %pav" << r << " = load float, ptr %pap" << r << ", align 4\n"
                    << "  %pado" << r << " = add i64 %paBase, " << r << "\n"
                    << "  %padp" << r << " = getelementptr inbounds float, ptr %apack, i64 %pado" << r << "\n"
                    << "  store float %pav" << r << ", ptr %padp" << r << ", align 4\n";
            }
            out << "  %paNext = add i64 %pa, 1\n"
                << "  %paMore = icmp slt i64 %paNext, %kLen\n"
                << "  br i1 %paMore, label %packA4_32.loop, label %i4_32.load\n"
                << "i4_32.load:\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %co" << r << " = add i64 %r" << r << ", %jb\n"
                    << "  %cp" << r << "0 = getelementptr inbounds float, ptr %c, i64 %co" << r << "\n"
                    << "  %cp" << r << "1 = getelementptr inbounds float, ptr %cp" << r << "0, i64 16\n"
                    << "  %ci" << r << "0 = load <16 x float>, ptr %cp" << r << "0, align 4\n"
                    << "  %ci" << r << "1 = load <16 x float>, ptr %cp" << r << "1, align 4\n";
            }
            out << "  br label %k4_32.loop\n"
                << "k4_32.loop:\n"
                << "  %kp = phi i64 [ 0, %i4_32.load ], [ %kpNext, %k4_32.loop ]\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %c" << r << "0 = phi <16 x float> [ %ci" << r << "0, %i4_32.load ], [ %c" << r << "0n, %k4_32.loop ]\n"
                    << "  %c" << r << "1 = phi <16 x float> [ %ci" << r << "1, %i4_32.load ], [ %c" << r << "1n, %k4_32.loop ]\n";
            }
            out << "  %bpoff = mul i64 %kp, 32\n"
                << "  %bp40 = getelementptr inbounds float, ptr %bpack, i64 %bpoff\n"
                << "  %bp41 = getelementptr inbounds float, ptr %bp40, i64 16\n"
                << "  %bv40 = load <16 x float>, ptr %bp40, align 64\n"
                << "  %bv41 = load <16 x float>, ptr %bp41, align 64\n"
                << "  %apoff = mul i64 %kp, 4\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %ao" << r << " = add i64 %apoff, " << r << "\n"
                    << "  %ap" << r << " = getelementptr inbounds float, ptr %apack, i64 %ao" << r << "\n"
                    << "  %as" << r << " = load float, ptr %ap" << r << ", align 4\n"
                    << "  %av" << r << "i = insertelement <16 x float> poison, float %as" << r << ", i32 0\n"
                    << "  %av" << r << " = shufflevector <16 x float> %av" << r << "i, <16 x float> poison, <16 x i32> zeroinitializer\n"
                    << "  %p" << r << "0 = fmul <16 x float> %av" << r << ", %bv40\n"
                    << "  %c" << r << "0n = fadd <16 x float> %c" << r << "0, %p" << r << "0\n"
                    << "  %p" << r << "1 = fmul <16 x float> %av" << r << ", %bv41\n"
                    << "  %c" << r << "1n = fadd <16 x float> %c" << r << "1, %p" << r << "1\n";
            }
            out << "  %kpNext = add i64 %kp, 1\n"
                << "  %kpMore = icmp slt i64 %kpNext, %kLen\n"
                << "  br i1 %kpMore, label %k4_32.loop, label %i4_32.store\n"
                << "i4_32.store:\n";
            for (int r = 0; r < 4; ++r) {
                out << "  store <16 x float> %c" << r << "0n, ptr %cp" << r << "0, align 4\n"
                    << "  store <16 x float> %c" << r << "1n, ptr %cp" << r << "1, align 4\n";
            }
            out << "  %i4Next = add i64 %i4, 4\n"
                << "  br label %i4_32.hdr\n"
                << "i1_32.hdr:\n"
                << "  %i1 = phi i64 [ %i4, %i4_32.hdr ], [ %i1Next, %i1_32.store ]\n"
                << "  %hasI1 = icmp slt i64 %i1, %n\n"
                << "  br i1 %hasI1, label %i1_32.load, label %kb32.latch\n"
                << "i1_32.load:\n"
                << "  %ir0 = mul i64 %i1, %n\n"
                << "  %ico = add i64 %ir0, %jb\n"
                << "  %icp0 = getelementptr inbounds float, ptr %c, i64 %ico\n"
                << "  %icp1 = getelementptr inbounds float, ptr %icp0, i64 16\n"
                << "  %ici0 = load <16 x float>, ptr %icp0, align 4\n"
                << "  %ici1 = load <16 x float>, ptr %icp1, align 4\n"
                << "  %iab = add i64 %ir0, %kb\n"
                << "  br label %k1_32.loop\n"
                << "k1_32.loop:\n"
                << "  %k1p = phi i64 [ 0, %i1_32.load ], [ %k1pNext, %k1_32.loop ]\n"
                << "  %sc0 = phi <16 x float> [ %ici0, %i1_32.load ], [ %sc0n, %k1_32.loop ]\n"
                << "  %sc1 = phi <16 x float> [ %ici1, %i1_32.load ], [ %sc1n, %k1_32.loop ]\n"
                << "  %ibpoff = mul i64 %k1p, 32\n"
                << "  %ibp0 = getelementptr inbounds float, ptr %bpack, i64 %ibpoff\n"
                << "  %ibp1 = getelementptr inbounds float, ptr %ibp0, i64 16\n"
                << "  %ibv0 = load <16 x float>, ptr %ibp0, align 64\n"
                << "  %ibv1 = load <16 x float>, ptr %ibp1, align 64\n"
                << "  %iao = add i64 %iab, %k1p\n"
                << "  %iap = getelementptr inbounds float, ptr %a, i64 %iao\n"
                << "  %ias = load float, ptr %iap, align 4\n"
                << "  %iavi = insertelement <16 x float> poison, float %ias, i32 0\n"
                << "  %iav = shufflevector <16 x float> %iavi, <16 x float> poison, <16 x i32> zeroinitializer\n"
                << "  %ip0 = fmul <16 x float> %iav, %ibv0\n"
                << "  %sc0n = fadd <16 x float> %sc0, %ip0\n"
                << "  %ip1 = fmul <16 x float> %iav, %ibv1\n"
                << "  %sc1n = fadd <16 x float> %sc1, %ip1\n"
                << "  %k1pNext = add i64 %k1p, 1\n"
                << "  %k1pMore = icmp slt i64 %k1pNext, %kLen\n"
                << "  br i1 %k1pMore, label %k1_32.loop, label %i1_32.store\n"
                << "i1_32.store:\n"
                << "  store <16 x float> %sc0n, ptr %icp0, align 4\n"
                << "  store <16 x float> %sc1n, ptr %icp1, align 4\n"
                << "  %i1Next = add i64 %i1, 1\n"
                << "  br label %i1_32.hdr\n"
                << "kb32.latch:\n"
                << "  %kbNext = add i64 %kb, 64\n"
                << "  %kbMore = icmp slt i64 %kbNext, %n\n"
                << "  br i1 %kbMore, label %kb32.hdr, label %jb32.latch\n"
                << "jb32.latch:\n"
                << "  %jbNext = add i64 %jb, 32\n"
                << "  br label %jb32.hdr\n"
                << "jb16.check:\n"
                << "  %jb16Lim = add i64 %jb, 16\n"
                << "  %has16 = icmp sle i64 %jb16Lim, %n\n"
                << "  br i1 %has16, label %kb16.hdr, label %jbTail.check\n"
                << "kb16.hdr:\n"
                << "  %kb16 = phi i64 [ 0, %jb16.check ], [ %kb16Next, %kb16.latch ]\n"
                << "  %kb16Cand = add i64 %kb16, 64\n"
                << "  %kEnd16 = call i64 @llvm.smin.i64(i64 %kb16Cand, i64 %n)\n"
                << "  %kLen16 = sub i64 %kEnd16, %kb16\n"
                << "  br label %pack16.loop\n"
                << "pack16.loop:\n"
                << "  %pk16 = phi i64 [ 0, %kb16.hdr ], [ %pk16Next, %pack16.loop ]\n"
                << "  %srcR16 = add i64 %kb16, %pk16\n"
                << "  %srcM16 = mul i64 %srcR16, %n\n"
                << "  %srcO16 = add i64 %srcM16, %jb\n"
                << "  %sp16 = getelementptr inbounds float, ptr %b, i64 %srcO16\n"
                << "  %pv16 = load <16 x float>, ptr %sp16, align 4\n"
                << "  %dstO16 = mul i64 %pk16, 16\n"
                << "  %dp16 = getelementptr inbounds float, ptr %bpack, i64 %dstO16\n"
                << "  store <16 x float> %pv16, ptr %dp16, align 64\n"
                << "  %pk16Next = add i64 %pk16, 1\n"
                << "  %pk16More = icmp slt i64 %pk16Next, %kLen16\n"
                << "  br i1 %pk16More, label %pack16.loop, label %i4_16.hdr\n"
                << "i4_16.hdr:\n"
                << "  %i4_16 = phi i64 [ 0, %pack16.loop ], [ %i4_16Next, %i4_16.store ]\n"
                << "  %i4_16Lim = add i64 %i4_16, 4\n"
                << "  %hasI4_16 = icmp sle i64 %i4_16Lim, %n\n"
                << "  br i1 %hasI4_16, label %i4_16.prep, label %i1_16.hdr\n"
                << "i4_16.prep:\n"
                << "  %q0 = mul i64 %i4_16, %n\n"
                << "  %q1 = add i64 %q0, %n\n"
                << "  %q2 = add i64 %q1, %n\n"
                << "  %q3 = add i64 %q2, %n\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %qab" << r << " = add i64 %q" << r << ", %kb16\n";
            }
            out << "  br label %packA4_16.loop\n"
                << "packA4_16.loop:\n"
                << "  %pa16 = phi i64 [ 0, %i4_16.prep ], [ %pa16Next, %packA4_16.loop ]\n"
                << "  %pa16Base = mul i64 %pa16, 4\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %pa16o" << r << " = add i64 %qab" << r << ", %pa16\n"
                    << "  %pa16p" << r << " = getelementptr inbounds float, ptr %a, i64 %pa16o" << r << "\n"
                    << "  %pa16v" << r << " = load float, ptr %pa16p" << r << ", align 4\n"
                    << "  %pa16do" << r << " = add i64 %pa16Base, " << r << "\n"
                    << "  %pa16dp" << r << " = getelementptr inbounds float, ptr %apack, i64 %pa16do" << r << "\n"
                    << "  store float %pa16v" << r << ", ptr %pa16dp" << r << ", align 4\n";
            }
            out << "  %pa16Next = add i64 %pa16, 1\n"
                << "  %pa16More = icmp slt i64 %pa16Next, %kLen16\n"
                << "  br i1 %pa16More, label %packA4_16.loop, label %i4_16.load\n"
                << "i4_16.load:\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %qo" << r << " = add i64 %q" << r << ", %jb\n"
                    << "  %qp" << r << " = getelementptr inbounds float, ptr %c, i64 %qo" << r << "\n"
                    << "  %qi" << r << " = load <16 x float>, ptr %qp" << r << ", align 4\n";
            }
            out << "  br label %k4_16.loop\n"
                << "k4_16.loop:\n"
                << "  %kp16 = phi i64 [ 0, %i4_16.load ], [ %kp16Next, %k4_16.loop ]\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %qc" << r << " = phi <16 x float> [ %qi" << r << ", %i4_16.load ], [ %qc" << r << "n, %k4_16.loop ]\n";
            }
            out << "  %bpo16 = mul i64 %kp16, 16\n"
                << "  %bp16 = getelementptr inbounds float, ptr %bpack, i64 %bpo16\n"
                << "  %bv16 = load <16 x float>, ptr %bp16, align 64\n"
                << "  %apoff16 = mul i64 %kp16, 4\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %qao" << r << " = add i64 %apoff16, " << r << "\n"
                    << "  %qap" << r << " = getelementptr inbounds float, ptr %apack, i64 %qao" << r << "\n"
                    << "  %qas" << r << " = load float, ptr %qap" << r << ", align 4\n"
                    << "  %qavi" << r << " = insertelement <16 x float> poison, float %qas" << r << ", i32 0\n"
                    << "  %qav" << r << " = shufflevector <16 x float> %qavi" << r << ", <16 x float> poison, <16 x i32> zeroinitializer\n"
                    << "  %qp" << r << "m = fmul <16 x float> %qav" << r << ", %bv16\n"
                    << "  %qc" << r << "n = fadd <16 x float> %qc" << r << ", %qp" << r << "m\n";
            }
            out << "  %kp16Next = add i64 %kp16, 1\n"
                << "  %kp16More = icmp slt i64 %kp16Next, %kLen16\n"
                << "  br i1 %kp16More, label %k4_16.loop, label %i4_16.store\n"
                << "i4_16.store:\n";
            for (int r = 0; r < 4; ++r) {
                out << "  store <16 x float> %qc" << r << "n, ptr %qp" << r << ", align 4\n";
            }
            out << "  %i4_16Next = add i64 %i4_16, 4\n"
                << "  br label %i4_16.hdr\n"
                << "i1_16.hdr:\n"
                << "  %i1_16 = phi i64 [ %i4_16, %i4_16.hdr ], [ %i1_16Next, %i1_16.store ]\n"
                << "  %hasI1_16 = icmp slt i64 %i1_16, %n\n"
                << "  br i1 %hasI1_16, label %i1_16.load, label %kb16.latch\n"
                << "i1_16.load:\n"
                << "  %qr0 = mul i64 %i1_16, %n\n"
                << "  %qco = add i64 %qr0, %jb\n"
                << "  %qcp = getelementptr inbounds float, ptr %c, i64 %qco\n"
                << "  %qci = load <16 x float>, ptr %qcp, align 4\n"
                << "  %q1ab = add i64 %qr0, %kb16\n"
                << "  br label %k1_16.loop\n"
                << "k1_16.loop:\n"
                << "  %k1p16 = phi i64 [ 0, %i1_16.load ], [ %k1p16Next, %k1_16.loop ]\n"
                << "  %qsc = phi <16 x float> [ %qci, %i1_16.load ], [ %qscn, %k1_16.loop ]\n"
                << "  %qbpo1 = mul i64 %k1p16, 16\n"
                << "  %qbp1 = getelementptr inbounds float, ptr %bpack, i64 %qbpo1\n"
                << "  %qbv1 = load <16 x float>, ptr %qbp1, align 64\n"
                << "  %qao = add i64 %q1ab, %k1p16\n"
                << "  %qap = getelementptr inbounds float, ptr %a, i64 %qao\n"
                << "  %qas = load float, ptr %qap, align 4\n"
                << "  %qavi = insertelement <16 x float> poison, float %qas, i32 0\n"
                << "  %qav = shufflevector <16 x float> %qavi, <16 x float> poison, <16 x i32> zeroinitializer\n"
                << "  %qm1 = fmul <16 x float> %qav, %qbv1\n"
                << "  %qscn = fadd <16 x float> %qsc, %qm1\n"
                << "  %k1p16Next = add i64 %k1p16, 1\n"
                << "  %k1p16More = icmp slt i64 %k1p16Next, %kLen16\n"
                << "  br i1 %k1p16More, label %k1_16.loop, label %i1_16.store\n"
                << "i1_16.store:\n"
                << "  store <16 x float> %qscn, ptr %qcp, align 4\n"
                << "  %i1_16Next = add i64 %i1_16, 1\n"
                << "  br label %i1_16.hdr\n"
                << "kb16.latch:\n"
                << "  %kb16Next = add i64 %kb16, 64\n"
                << "  %kb16More = icmp slt i64 %kb16Next, %n\n"
                << "  br i1 %kb16More, label %kb16.hdr, label %jbTail.check\n"
                << "jbTail.check:\n"
                << "  %jbTail = phi i64 [ %jb, %jb16.check ], [ %jb16Lim, %kb16.latch ]\n"
                << "  %remTail = sub i64 %n, %jbTail\n"
                << "  %hasTail = icmp sgt i64 %remTail, 0\n"
                << "  br i1 %hasTail, label %kbT.prep, label %done\n"
                << "kbT.prep:\n"
                << "  %rem32 = trunc i64 %remTail to i32\n"
                << "  %remI = insertelement <16 x i32> poison, i32 %rem32, i32 0\n"
                << "  %remV = shufflevector <16 x i32> %remI, <16 x i32> poison, <16 x i32> zeroinitializer\n"
                << "  %tmask = icmp ult <16 x i32> " << iota16 << ", %remV\n"
                << "  br label %kbT.hdr\n"
                << "kbT.hdr:\n"
                << "  %kbT = phi i64 [ 0, %kbT.prep ], [ %kbTNext, %kbT.latch ]\n"
                << "  %kbTCand = add i64 %kbT, 64\n"
                << "  %kEndT = call i64 @llvm.smin.i64(i64 %kbTCand, i64 %n)\n"
                << "  %kLenT = sub i64 %kEndT, %kbT\n"
                << "  br label %packT.loop\n"
                << "packT.loop:\n"
                << "  %pkT = phi i64 [ 0, %kbT.hdr ], [ %pkTNext, %packT.loop ]\n"
                << "  %srcRT = add i64 %kbT, %pkT\n"
                << "  %srcMT = mul i64 %srcRT, %n\n"
                << "  %srcOT = add i64 %srcMT, %jbTail\n"
                << "  %spT = getelementptr inbounds float, ptr %b, i64 %srcOT\n"
                << "  %pvT = call <16 x float> @llvm.masked.load.v16f32.p0(ptr %spT, i32 4, <16 x i1> %tmask, <16 x float> " << z16f << ")\n"
                << "  %dstOT = mul i64 %pkT, 16\n"
                << "  %dpT = getelementptr inbounds float, ptr %bpack, i64 %dstOT\n"
                << "  store <16 x float> %pvT, ptr %dpT, align 64\n"
                << "  %pkTNext = add i64 %pkT, 1\n"
                << "  %pkTMore = icmp slt i64 %pkTNext, %kLenT\n"
                << "  br i1 %pkTMore, label %packT.loop, label %iT.hdr\n"
                << "iT.hdr:\n"
                << "  %iT = phi i64 [ 0, %packT.loop ], [ %iTNext, %iT.store ]\n"
                << "  %hasIT = icmp slt i64 %iT, %n\n"
                << "  br i1 %hasIT, label %iT.load, label %kbT.latch\n"
                << "iT.load:\n"
                << "  %tr0 = mul i64 %iT, %n\n"
                << "  %tco = add i64 %tr0, %jbTail\n"
                << "  %tcp = getelementptr inbounds float, ptr %c, i64 %tco\n"
                << "  %tci = call <16 x float> @llvm.masked.load.v16f32.p0(ptr %tcp, i32 4, <16 x i1> %tmask, <16 x float> " << z16f << ")\n"
                << "  %tab = add i64 %tr0, %kbT\n"
                << "  br label %kT.loop\n"
                << "kT.loop:\n"
                << "  %kpT = phi i64 [ 0, %iT.load ], [ %kpTNext, %kT.loop ]\n"
                << "  %tc = phi <16 x float> [ %tci, %iT.load ], [ %tcn, %kT.loop ]\n"
                << "  %tbpo = mul i64 %kpT, 16\n"
                << "  %tbp = getelementptr inbounds float, ptr %bpack, i64 %tbpo\n"
                << "  %tbv = load <16 x float>, ptr %tbp, align 64\n"
                << "  %tao = add i64 %tab, %kpT\n"
                << "  %tap = getelementptr inbounds float, ptr %a, i64 %tao\n"
                << "  %tas = load float, ptr %tap, align 4\n"
                << "  %tavi = insertelement <16 x float> poison, float %tas, i32 0\n"
                << "  %tav = shufflevector <16 x float> %tavi, <16 x float> poison, <16 x i32> zeroinitializer\n"
                << "  %tm = fmul <16 x float> %tav, %tbv\n"
                << "  %tcn = fadd <16 x float> %tc, %tm\n"
                << "  %kpTNext = add i64 %kpT, 1\n"
                << "  %kpTMore = icmp slt i64 %kpTNext, %kLenT\n"
                << "  br i1 %kpTMore, label %kT.loop, label %iT.store\n"
                << "iT.store:\n"
                << "  call void @llvm.masked.store.v16f32.p0(<16 x float> %tcn, ptr %tcp, i32 4, <16 x i1> %tmask)\n"
                << "  %iTNext = add i64 %iT, 1\n"
                << "  br label %iT.hdr\n"
                << "kbT.latch:\n"
                << "  %kbTNext = add i64 %kbT, 64\n"
                << "  %kbTMore = icmp slt i64 %kbTNext, %n\n"
                << "  br i1 %kbTMore, label %kbT.hdr, label %done\n"
                << "done:\n"
                << "  ret void\n}\n";
        }
        if (usesMatMulModU16) {
            usedRawDecls.insert("declare <16 x i16> @llvm.masked.load.v16i16.p0(ptr, i32 immarg, <16 x i1>, <16 x i16>)");
            usedRawDecls.insert("declare i64 @llvm.vector.reduce.add.v16i64(<16 x i64>)");
            const std::string iota16 = vecConstIota(16, 32);
            const std::string z16_16 = vecConstSplat(16, "i16", "0");
            const std::string z16_32 = vecConstSplat(16, "i32", "0");
            const std::string z16_64 = vecConstSplat(16, "i64", "0");
            std::string interleave32 = "<", even16 = "<", odd16 = "<";
            for (int i = 0; i < 16; ++i) {
                if (i > 0) { interleave32 += ", "; even16 += ", "; odd16 += ", "; }
                interleave32 += "i32 " + std::to_string(i) + ", i32 " + std::to_string(16 + i);
                even16 += "i32 " + std::to_string(i * 2);
                odd16 += "i32 " + std::to_string(i * 2 + 1);
            }
            interleave32 += ">";
            even16 += ">";
            odd16 += ">";
            out << "define internal <16 x i32> @tz.barrett_rem16(<16 x i32> %c, "
                << "<16 x i32> %vmod, <16 x i64> %vbmul, <16 x i64> %vbshift, i1 %useBar) alwaysinline sanitize_address {\n"
                << "entry:\n"
                << "  br i1 %useBar, label %bar, label %gen\n"
                << "bar:\n"
                << "  %cz = zext <16 x i32> %c to <16 x i64>\n"
                << "  %p = mul <16 x i64> %cz, %vbmul\n"
                << "  %s = lshr <16 x i64> %p, %vbshift\n"
                << "  %q = trunc <16 x i64> %s to <16 x i32>\n"
                << "  %qm = mul <16 x i32> %q, %vmod\n"
                << "  %rb = sub <16 x i32> %c, %qm\n"
                << "  ret <16 x i32> %rb\n"
                << "gen:\n"
                << "  %rg = urem <16 x i32> %c, %vmod\n"
                << "  ret <16 x i32> %rg\n}\n";
            out << "define internal <16 x i64> @tz.barrett_accum16(<16 x i64> %acc, <16 x i32> %c, "
                << "<16 x i32> %vmod, <16 x i64> %vbmul, <16 x i64> %vbshift, i1 %useBar) alwaysinline sanitize_address {\n"
                << "entry:\n"
                << "  %rem = call <16 x i32> @tz.barrett_rem16(<16 x i32> %c, <16 x i32> %vmod, <16 x i64> %vbmul, <16 x i64> %vbshift, i1 %useBar)\n"
                << "  %rem64 = zext <16 x i32> %rem to <16 x i64>\n"
                << "  %res = add <16 x i64> %acc, %rem64\n"
                << "  ret <16 x i64> %res\n}\n";
            out << "define internal i64 @tz.matmul_mod.u16(ptr readonly %a, ptr readonly %b, i64 %n, i32 %mod32, i64 %bmul, i64 %bshift) sanitize_address {\n"
                << "entry:\n"
                << "  %badN = icmp sle i64 %n, 0\n"
                << "  br i1 %badN, label %retZero, label %prep\n"
                << "retZero:\n"
                << "  ret i64 0\n"
                << "prep:\n"
                << "  %vsum.slot = alloca <16 x i64>, align 64\n"
                << "  store <16 x i64> " << z16_64 << ", ptr %vsum.slot, align 64\n"
                << "  %useBar = icmp ne i64 %bmul, 0\n"
                << "  %canSum32 = icmp ule i32 %mod32, 268435455\n"
                << "  %kEndEven = and i64 %n, -2\n"
                << "  %hasEven = icmp sgt i64 %kEndEven, 0\n"
                << "  %hasOdd = icmp ne i64 %kEndEven, %n\n"
                << "  %m0 = insertelement <16 x i32> poison, i32 %mod32, i32 0\n"
                << "  %vmod = shufflevector <16 x i32> %m0, <16 x i32> poison, <16 x i32> zeroinitializer\n"
                << "  %bm0 = insertelement <16 x i64> poison, i64 %bmul, i32 0\n"
                << "  %vbmul = shufflevector <16 x i64> %bm0, <16 x i64> poison, <16 x i32> zeroinitializer\n"
                << "  %bs0 = insertelement <16 x i64> poison, i64 %bshift, i32 0\n"
                << "  %vbshift = shufflevector <16 x i64> %bs0, <16 x i64> poison, <16 x i32> zeroinitializer\n"
                << "  br label %j32.hdr\n";

            auto emitTileLoop = [&](const std::string& pfx, int rows, int cols, bool masked,
                                    const std::string& colVar, const std::string& entryFrom, const std::string& exitLabel) {
                const std::string tag = pfx + std::to_string(rows);
                const std::string nextHdr = (rows == 4) ? (pfx + "1.hdr") : exitLabel;
                out << tag << ".hdr:\n"
                    << "  %" << tag << "_i = phi i64 [ " << (rows == 4 ? "0" : "%" + pfx + "4_i")
                    << ", %" << entryFrom << " ], [ %" << tag << "_iNext, %" << tag << ".nextI ]\n";
                if (rows == 4) {
                    out << "  %" << tag << "_lim = add i64 %" << tag << "_i, 4\n"
                        << "  %" << tag << "_has = icmp sle i64 %" << tag << "_lim, %n\n"
                        << "  br i1 %" << tag << "_has, label %" << tag << ".prep, label %" << nextHdr << "\n";
                } else {
                    out << "  %" << tag << "_has = icmp slt i64 %" << tag << "_i, %n\n"
                        << "  br i1 %" << tag << "_has, label %" << tag << ".prep, label %" << nextHdr << "\n";
                }
                out << tag << ".prep:\n"
                    << "  %" << tag << "_r0 = mul i64 %" << tag << "_i, %n\n";
                for (int r = 1; r < rows; ++r) {
                    out << "  %" << tag << "_r" << r << " = add i64 %" << tag << "_r" << (r - 1) << ", %n\n";
                }
                out << "  br i1 %hasEven, label %" << tag << ".kEven, label %" << tag << ".kOddCheck\n"
                    << tag << ".kEven:\n"
                    << "  %" << tag << "_k = phi i64 [ 0, %" << tag << ".prep ], [ %" << tag << "_kNext, %" << tag << ".kEven ]\n";
                for (int r = 0; r < rows; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        out << "  %" << tag << "_e" << r << "_" << c << " = phi <16 x i32> [ " << z16_32
                            << ", %" << tag << ".prep ], [ %" << tag << "_e" << r << "_" << c << "n, %" << tag << ".kEven ]\n";
                    }
                }
                out << "  %" << tag << "_bk0 = mul i64 %" << tag << "_k, %n\n"
                    << "  %" << tag << "_bo0 = add i64 %" << tag << "_bk0, " << colVar << "\n"
                    << "  %" << tag << "_bo1 = add i64 %" << tag << "_bo0, %n\n";
                for (int c = 0; c < cols; ++c) {
                    out << "  %" << tag << "_bp0_" << c << " = getelementptr inbounds i16, ptr %b, i64 %" << tag << "_bo0\n";
                    if (c > 0) {
                        out << "  %" << tag << "_bp0c_" << c << " = getelementptr inbounds i16, ptr %" << tag << "_bp0_" << c << ", i64 " << (c * 16) << "\n";
                    }
                    const std::string p0 = (c == 0) ? ("%" + tag + "_bp0_" + std::to_string(c)) : ("%" + tag + "_bp0c_" + std::to_string(c));
                    out << "  %" << tag << "_bp1_" << c << " = getelementptr inbounds i16, ptr %b, i64 %" << tag << "_bo1\n";
                    if (c > 0) {
                        out << "  %" << tag << "_bp1c_" << c << " = getelementptr inbounds i16, ptr %" << tag << "_bp1_" << c << ", i64 " << (c * 16) << "\n";
                    }
                    const std::string p1 = (c == 0) ? ("%" + tag + "_bp1_" + std::to_string(c)) : ("%" + tag + "_bp1c_" + std::to_string(c));
                    if (masked) {
                        out << "  %" << tag << "_b0_" << c << " = call <16 x i16> @llvm.masked.load.v16i16.p0(ptr " << p0 << ", i32 2, <16 x i1> %tmask, <16 x i16> " << z16_16 << ")\n"
                            << "  %" << tag << "_b1_" << c << " = call <16 x i16> @llvm.masked.load.v16i16.p0(ptr " << p1 << ", i32 2, <16 x i1> %tmask, <16 x i16> " << z16_16 << ")\n";
                    } else {
                        out << "  %" << tag << "_b0_" << c << " = load <16 x i16>, ptr " << p0 << ", align 2\n"
                            << "  %" << tag << "_b1_" << c << " = load <16 x i16>, ptr " << p1 << ", align 2\n";
                    }
                    out << "  %" << tag << "_bp" << c << " = shufflevector <16 x i16> %" << tag << "_b0_" << c
                        << ", <16 x i16> %" << tag << "_b1_" << c << ", <32 x i32> " << interleave32 << "\n"
                        << "  %" << tag << "_b32_" << c << " = sext <32 x i16> %" << tag << "_bp" << c << " to <32 x i32>\n";
                }
                for (int r = 0; r < rows; ++r) {
                    out << "  %" << tag << "_ao" << r << " = add i64 %" << tag << "_r" << r << ", %" << tag << "_k\n"
                        << "  %" << tag << "_ap" << r << " = getelementptr inbounds i16, ptr %a, i64 %" << tag << "_ao" << r << "\n"
                        << "  %" << tag << "_a32s" << r << " = load i32, ptr %" << tag << "_ap" << r << ", align 2\n"
                        << "  %" << tag << "_ai" << r << " = insertelement <16 x i32> poison, i32 %" << tag << "_a32s" << r << ", i32 0\n"
                        << "  %" << tag << "_as" << r << " = shufflevector <16 x i32> %" << tag << "_ai" << r << ", <16 x i32> poison, <16 x i32> zeroinitializer\n"
                        << "  %" << tag << "_ap16_" << r << " = bitcast <16 x i32> %" << tag << "_as" << r << " to <32 x i16>\n"
                        << "  %" << tag << "_a32_" << r << " = sext <32 x i16> %" << tag << "_ap16_" << r << " to <32 x i32>\n";
                    for (int c = 0; c < cols; ++c) {
                        out << "  %" << tag << "_m" << r << "_" << c << " = mul nsw <32 x i32> %" << tag << "_a32_" << r << ", %" << tag << "_b32_" << c << "\n"
                            << "  %" << tag << "_ev" << r << "_" << c << " = shufflevector <32 x i32> %" << tag << "_m" << r << "_" << c << ", <32 x i32> poison, <16 x i32> " << even16 << "\n"
                            << "  %" << tag << "_od" << r << "_" << c << " = shufflevector <32 x i32> %" << tag << "_m" << r << "_" << c << ", <32 x i32> poison, <16 x i32> " << odd16 << "\n"
                            << "  %" << tag << "_dp" << r << "_" << c << " = add <16 x i32> %" << tag << "_ev" << r << "_" << c << ", %" << tag << "_od" << r << "_" << c << "\n"
                            << "  %" << tag << "_e" << r << "_" << c << "n = add <16 x i32> %" << tag << "_e" << r << "_" << c << ", %" << tag << "_dp" << r << "_" << c << "\n";
                    }
                }
                out << "  %" << tag << "_kNext = add i64 %" << tag << "_k, 2\n"
                    << "  %" << tag << "_kMore = icmp slt i64 %" << tag << "_kNext, %kEndEven\n"
                    << "  br i1 %" << tag << "_kMore, label %" << tag << ".kEven, label %" << tag << ".kOddCheck\n"
                    << tag << ".kOddCheck:\n";
                for (int r = 0; r < rows; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        out << "  %" << tag << "_mf" << r << "_" << c << " = phi <16 x i32> [ " << z16_32
                            << ", %" << tag << ".prep ], [ %" << tag << "_e" << r << "_" << c << "n, %" << tag << ".kEven ]\n";
                    }
                }
                out << "  br i1 %hasOdd, label %" << tag << ".kOdd, label %" << tag << ".done\n"
                    << tag << ".kOdd:\n"
                    << "  %" << tag << "_obk = mul i64 %kEndEven, %n\n"
                    << "  %" << tag << "_obo = add i64 %" << tag << "_obk, " << colVar << "\n";
                for (int c = 0; c < cols; ++c) {
                    out << "  %" << tag << "_obp_" << c << " = getelementptr inbounds i16, ptr %b, i64 %" << tag << "_obo\n";
                    if (c > 0) {
                        out << "  %" << tag << "_obpc_" << c << " = getelementptr inbounds i16, ptr %" << tag << "_obp_" << c << ", i64 " << (c * 16) << "\n";
                    }
                    const std::string op = (c == 0) ? ("%" + tag + "_obp_" + std::to_string(c)) : ("%" + tag + "_obpc_" + std::to_string(c));
                    if (masked) {
                        out << "  %" << tag << "_obw" << c << " = call <16 x i16> @llvm.masked.load.v16i16.p0(ptr " << op << ", i32 2, <16 x i1> %tmask, <16 x i16> " << z16_16 << ")\n";
                    } else {
                        out << "  %" << tag << "_obw" << c << " = load <16 x i16>, ptr " << op << ", align 2\n";
                    }
                    out << "  %" << tag << "_obv" << c << " = zext <16 x i16> %" << tag << "_obw" << c << " to <16 x i32>\n";
                }
                for (int r = 0; r < rows; ++r) {
                    out << "  %" << tag << "_oao" << r << " = add i64 %" << tag << "_r" << r << ", %kEndEven\n"
                        << "  %" << tag << "_oap" << r << " = getelementptr inbounds i16, ptr %a, i64 %" << tag << "_oao" << r << "\n"
                        << "  %" << tag << "_oaw" << r << " = load i16, ptr %" << tag << "_oap" << r << ", align 2\n"
                        << "  %" << tag << "_oas" << r << " = zext i16 %" << tag << "_oaw" << r << " to i32\n"
                        << "  %" << tag << "_oavi" << r << " = insertelement <16 x i32> poison, i32 %" << tag << "_oas" << r << ", i32 0\n"
                        << "  %" << tag << "_oav" << r << " = shufflevector <16 x i32> %" << tag << "_oavi" << r << ", <16 x i32> poison, <16 x i32> zeroinitializer\n";
                    for (int c = 0; c < cols; ++c) {
                        out << "  %" << tag << "_omp" << r << "_" << c << " = mul <16 x i32> %" << tag << "_oav" << r << ", %" << tag << "_obv" << c << "\n"
                            << "  %" << tag << "_of" << r << "_" << c << " = add <16 x i32> %" << tag << "_mf" << r << "_" << c << ", %" << tag << "_omp" << r << "_" << c << "\n";
                    }
                }
                out << "  br label %" << tag << ".done\n"
                    << tag << ".done:\n";
                for (int r = 0; r < rows; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        out << "  %" << tag << "_fin" << r << "_" << c << " = phi <16 x i32> [ %" << tag << "_mf" << r << "_" << c
                            << ", %" << tag << ".kOddCheck ], [ %" << tag << "_of" << r << "_" << c << ", %" << tag << ".kOdd ]\n";
                    }
                }
                out << "  br i1 %canSum32, label %" << tag << ".sum32, label %" << tag << ".sum64\n"
                    << tag << ".sum32:\n";
                std::vector<std::string> rems;
                for (int r = 0; r < rows; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        const std::string rn = "%" + tag + "_rem" + std::to_string(r) + "_" + std::to_string(c);
                        out << "  " << rn << " = call <16 x i32> @tz.barrett_rem16(<16 x i32> %" << tag << "_fin" << r << "_" << c
                            << ", <16 x i32> %vmod, <16 x i64> %vbmul, <16 x i64> %vbshift, i1 %useBar)\n";
                        rems.push_back(rn);
                    }
                }
                std::vector<std::string> tree = rems;
                int tidx = 0;
                while (tree.size() > 1) {
                    std::vector<std::string> nxt;
                    for (std::size_t i = 0; i + 1 < tree.size(); i += 2) {
                        const std::string sn = "%" + tag + "_st" + std::to_string(tidx++);
                        out << "  " << sn << " = add <16 x i32> " << tree[i] << ", " << tree[i + 1] << "\n";
                        nxt.push_back(sn);
                    }
                    if (tree.size() & 1) nxt.push_back(tree.back());
                    tree = std::move(nxt);
                }
                out << "  %" << tag << "_s64 = zext <16 x i32> " << tree[0] << " to <16 x i64>\n"
                    << "  %" << tag << "_vs32 = load <16 x i64>, ptr %vsum.slot, align 64\n"
                    << "  %" << tag << "_vs32n = add <16 x i64> %" << tag << "_vs32, %" << tag << "_s64\n"
                    << "  store <16 x i64> %" << tag << "_vs32n, ptr %vsum.slot, align 64\n"
                    << "  br label %" << tag << ".nextI\n"
                    << tag << ".sum64:\n"
                    << "  %" << tag << "_vs64 = load <16 x i64>, ptr %vsum.slot, align 64\n";
                std::string curS = "%" + tag + "_vs64";
                for (int r = 0; r < rows; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        const std::string nxt = "%" + tag + "_va" + std::to_string(r * cols + c);
                        out << "  " << nxt << " = call <16 x i64> @tz.barrett_accum16(<16 x i64> " << curS
                            << ", <16 x i32> %" << tag << "_fin" << r << "_" << c << ", <16 x i32> %vmod, <16 x i64> %vbmul, <16 x i64> %vbshift, i1 %useBar)\n";
                        curS = nxt;
                    }
                }
                out << "  store <16 x i64> " << curS << ", ptr %vsum.slot, align 64\n"
                    << "  br label %" << tag << ".nextI\n"
                    << tag << ".nextI:\n"
                    << "  %" << tag << "_iNext = add i64 %" << tag << "_i, " << rows << "\n"
                    << "  br label %" << tag << ".hdr\n";
            };

            out << "j32.hdr:\n"
                << "  %j = phi i64 [ 0, %prep ], [ %jNext, %j32.latch ]\n"
                << "  %j32 = add i64 %j, 32\n"
                << "  %has32 = icmp sle i64 %j32, %n\n"
                << "  br i1 %has32, label %j32_4.hdr, label %j16.check\n";
            emitTileLoop("j32_", 4, 2, false, "%j", "j32.hdr", "j32.latch");
            emitTileLoop("j32_", 1, 2, false, "%j", "j32_4.hdr", "j32.latch");
            out << "j32.latch:\n"
                << "  %jNext = add i64 %j, 32\n"
                << "  br label %j32.hdr\n"
                << "j16.check:\n"
                << "  %j16Lim = add i64 %j, 16\n"
                << "  %has16 = icmp sle i64 %j16Lim, %n\n"
                << "  br i1 %has16, label %j16_4.hdr, label %jTail.check\n";
            emitTileLoop("j16_", 4, 1, false, "%j", "j16.check", "jTail.check");
            emitTileLoop("j16_", 1, 1, false, "%j", "j16_4.hdr", "jTail.check");
            out << "jTail.check:\n"
                << "  %jTail = phi i64 [ %j, %j16.check ], [ %j16Lim, %j16_1.hdr ]\n"
                << "  %remTail = sub i64 %n, %jTail\n"
                << "  %hasTail = icmp sgt i64 %remTail, 0\n"
                << "  br i1 %hasTail, label %jTail.prep, label %finish\n"
                << "jTail.prep:\n"
                << "  %rem32 = trunc i64 %remTail to i32\n"
                << "  %remI = insertelement <16 x i32> poison, i32 %rem32, i32 0\n"
                << "  %remV = shufflevector <16 x i32> %remI, <16 x i32> poison, <16 x i32> zeroinitializer\n"
                << "  %tmask = icmp ult <16 x i32> " << iota16 << ", %remV\n"
                << "  br label %jT_4.hdr\n";
            emitTileLoop("jT_", 4, 1, true, "%jTail", "jTail.prep", "finish");
            emitTileLoop("jT_", 1, 1, true, "%jTail", "jT_4.hdr", "finish");
            out << "finish:\n"
                << "  %vsFin = load <16 x i64>, ptr %vsum.slot, align 64\n"
                << "  %ans = call i64 @llvm.vector.reduce.add.v16i64(<16 x i64> %vsFin)\n"
                << "  ret i64 %ans\n}\n";
        }
        if (usesFloydStepU16) {
            usedRawDecls.insert("declare <32 x i16> @llvm.umin.v32i16(<32 x i16>, <32 x i16>)");
            usedRawDecls.insert("declare <32 x i16> @llvm.masked.load.v32i16.p0(ptr, i32 immarg, <32 x i1>, <32 x i16>)");
            usedRawDecls.insert("declare void @llvm.masked.store.v32i16.p0(<32 x i16>, ptr, i32 immarg, <32 x i1>)");
            const std::string iota32 = vecConstIota(32, 16);
            const std::string z32_16 = vecConstSplat(32, "i16", "0");
            out << "define internal void @tz.floyd_step_u16(ptr %d, i64 %n, i64 %k) alwaysinline sanitize_address {\n"
                << "entry:\n"
                << "  %is32 = icmp eq i64 %n, 32\n"
                << "  br i1 %is32, label %n32.prep, label %chkLt\n"
                << "n32.prep:\n"
                << "  %k32Base = mul i64 %k, 32\n"
                << "  %k32Ptr = getelementptr inbounds i16, ptr %d, i64 %k32Base\n"
                << "  %vk32 = load <32 x i16>, ptr %k32Ptr, align 2\n"
                << "  br label %n32.loop\n"
                << "n32.loop:\n"
                << "  %i32 = phi i64 [ 0, %n32.prep ], [ %i32Next, %n32.loop ]\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %ir32_" << r << " = add i64 %i32, " << r << "\n"
                    << "  %base32_" << r << " = mul i64 %ir32_" << r << ", 32\n"
                    << "  %ik32_" << r << " = add i64 %base32_" << r << ", %k\n"
                    << "  %ikp32_" << r << " = getelementptr inbounds i16, ptr %d, i64 %ik32_" << r << "\n"
                    << "  %dik32_" << r << " = load i16, ptr %ikp32_" << r << ", align 2\n"
                    << "  %vdikI32_" << r << " = insertelement <32 x i16> poison, i16 %dik32_" << r << ", i32 0\n"
                    << "  %vdik32_" << r << " = shufflevector <32 x i16> %vdikI32_" << r << ", <32 x i16> poison, <32 x i32> zeroinitializer\n"
                    << "  %rowp32_" << r << " = getelementptr inbounds i16, ptr %d, i64 %base32_" << r << "\n"
                    << "  %cur32_" << r << " = load <32 x i16>, ptr %rowp32_" << r << ", align 2\n"
                    << "  %sum32_" << r << " = add <32 x i16> %vdik32_" << r << ", %vk32\n"
                    << "  %res32_" << r << " = call <32 x i16> @llvm.umin.v32i16(<32 x i16> %cur32_" << r << ", <32 x i16> %sum32_" << r << ")\n"
                    << "  store <32 x i16> %res32_" << r << ", ptr %rowp32_" << r << ", align 2\n";
            }
            out << "  %i32Next = add i64 %i32, 4\n"
                << "  %i32More = icmp slt i64 %i32Next, 32\n"
                << "  br i1 %i32More, label %n32.loop, label %done\n"
                << "chkLt:\n"
                << "  %lt32 = icmp slt i64 %n, 32\n"
                << "  br i1 %lt32, label %lt.prep, label %gt.jHdr\n"
                << "lt.prep:\n"
                << "  %n16 = trunc i64 %n to i16\n"
                << "  %nI = insertelement <32 x i16> poison, i16 %n16, i32 0\n"
                << "  %nV = shufflevector <32 x i16> %nI, <32 x i16> poison, <32 x i32> zeroinitializer\n"
                << "  %ltMask = icmp ult <32 x i16> " << iota32 << ", %nV\n"
                << "  %ltKBase = mul i64 %k, %n\n"
                << "  %ltKPtr = getelementptr inbounds i16, ptr %d, i64 %ltKBase\n"
                << "  %ltVk = call <32 x i16> @llvm.masked.load.v32i16.p0(ptr %ltKPtr, i32 2, <32 x i1> %ltMask, <32 x i16> " << z32_16 << ")\n"
                << "  br label %lt.loop\n"
                << "lt.loop:\n"
                << "  %iLt = phi i64 [ 0, %lt.prep ], [ %iLtNext, %lt.loop ]\n"
                << "  %ltBase = mul i64 %iLt, %n\n"
                << "  %ltIk = add i64 %ltBase, %k\n"
                << "  %ltIkp = getelementptr inbounds i16, ptr %d, i64 %ltIk\n"
                << "  %ltDik = load i16, ptr %ltIkp, align 2\n"
                << "  %ltVdikI = insertelement <32 x i16> poison, i16 %ltDik, i32 0\n"
                << "  %ltVdik = shufflevector <32 x i16> %ltVdikI, <32 x i16> poison, <32 x i32> zeroinitializer\n"
                << "  %ltRp = getelementptr inbounds i16, ptr %d, i64 %ltBase\n"
                << "  %ltCur = call <32 x i16> @llvm.masked.load.v32i16.p0(ptr %ltRp, i32 2, <32 x i1> %ltMask, <32 x i16> " << z32_16 << ")\n"
                << "  %ltSum = add <32 x i16> %ltVdik, %ltVk\n"
                << "  %ltRes = call <32 x i16> @llvm.umin.v32i16(<32 x i16> %ltCur, <32 x i16> %ltSum)\n"
                << "  call void @llvm.masked.store.v32i16.p0(<32 x i16> %ltRes, ptr %ltRp, i32 2, <32 x i1> %ltMask)\n"
                << "  %iLtNext = add i64 %iLt, 1\n"
                << "  %ltMore = icmp slt i64 %iLtNext, %n\n"
                << "  br i1 %ltMore, label %lt.loop, label %done\n"
                << "gt.jHdr:\n"
                << "  %jg = phi i64 [ 0, %chkLt ], [ %jgNext, %gt.jLatch ]\n"
                << "  %jg32 = add i64 %jg, 32\n"
                << "  %hasJ32 = icmp sle i64 %jg32, %n\n"
                << "  br i1 %hasJ32, label %gt.jPrep, label %gt.tailChk\n"
                << "gt.jPrep:\n"
                << "  %gtkBase = mul i64 %k, %n\n"
                << "  %gtkOff = add i64 %gtkBase, %jg\n"
                << "  %gtkPtr = getelementptr inbounds i16, ptr %d, i64 %gtkOff\n"
                << "  %gtVk = load <32 x i16>, ptr %gtkPtr, align 2\n"
                << "  br label %gt.i4Hdr\n"
                << "gt.i4Hdr:\n"
                << "  %gi4 = phi i64 [ 0, %gt.jPrep ], [ %gi4Next, %gt.i4Body ]\n"
                << "  %gi4Lim = add i64 %gi4, 4\n"
                << "  %hasGi4 = icmp sle i64 %gi4Lim, %n\n"
                << "  br i1 %hasGi4, label %gt.i4Body, label %gt.i1Hdr\n"
                << "gt.i4Body:\n";
            for (int r = 0; r < 4; ++r) {
                out << "  %giRow_" << r << " = add i64 %gi4, " << r << "\n"
                    << "  %giBase_" << r << " = mul i64 %giRow_" << r << ", %n\n"
                    << "  %giIk_" << r << " = add i64 %giBase_" << r << ", %k\n"
                    << "  %giIkp_" << r << " = getelementptr inbounds i16, ptr %d, i64 %giIk_" << r << "\n"
                    << "  %giDik_" << r << " = load i16, ptr %giIkp_" << r << ", align 2\n"
                    << "  %giVdikI_" << r << " = insertelement <32 x i16> poison, i16 %giDik_" << r << ", i32 0\n"
                    << "  %giVdik_" << r << " = shufflevector <32 x i16> %giVdikI_" << r << ", <32 x i16> poison, <32 x i32> zeroinitializer\n"
                    << "  %giOff_" << r << " = add i64 %giBase_" << r << ", %jg\n"
                    << "  %giRp_" << r << " = getelementptr inbounds i16, ptr %d, i64 %giOff_" << r << "\n"
                    << "  %giCur_" << r << " = load <32 x i16>, ptr %giRp_" << r << ", align 2\n"
                    << "  %giSum_" << r << " = add <32 x i16> %giVdik_" << r << ", %gtVk\n"
                    << "  %giRes_" << r << " = call <32 x i16> @llvm.umin.v32i16(<32 x i16> %giCur_" << r << ", <32 x i16> %giSum_" << r << ")\n"
                    << "  store <32 x i16> %giRes_" << r << ", ptr %giRp_" << r << ", align 2\n";
            }
            out << "  %gi4Next = add i64 %gi4, 4\n"
                << "  br label %gt.i4Hdr\n"
                << "gt.i1Hdr:\n"
                << "  %gi1 = phi i64 [ %gi4, %gt.i4Hdr ], [ %gi1Next, %gt.i1Body ]\n"
                << "  %hasGi1 = icmp slt i64 %gi1, %n\n"
                << "  br i1 %hasGi1, label %gt.i1Body, label %gt.jLatch\n"
                << "gt.i1Body:\n"
                << "  %gi1Base = mul i64 %gi1, %n\n"
                << "  %gi1Ik = add i64 %gi1Base, %k\n"
                << "  %gi1Ikp = getelementptr inbounds i16, ptr %d, i64 %gi1Ik\n"
                << "  %gi1Dik = load i16, ptr %gi1Ikp, align 2\n"
                << "  %gi1VdikI = insertelement <32 x i16> poison, i16 %gi1Dik, i32 0\n"
                << "  %gi1Vdik = shufflevector <32 x i16> %gi1VdikI, <32 x i16> poison, <32 x i32> zeroinitializer\n"
                << "  %gi1Off = add i64 %gi1Base, %jg\n"
                << "  %gi1Rp = getelementptr inbounds i16, ptr %d, i64 %gi1Off\n"
                << "  %gi1Cur = load <32 x i16>, ptr %gi1Rp, align 2\n"
                << "  %gi1Sum = add <32 x i16> %gi1Vdik, %gtVk\n"
                << "  %gi1Res = call <32 x i16> @llvm.umin.v32i16(<32 x i16> %gi1Cur, <32 x i16> %gi1Sum)\n"
                << "  store <32 x i16> %gi1Res, ptr %gi1Rp, align 2\n"
                << "  %gi1Next = add i64 %gi1, 1\n"
                << "  br label %gt.i1Hdr\n"
                << "gt.jLatch:\n"
                << "  %jgNext = add i64 %jg, 32\n"
                << "  br label %gt.jHdr\n"
                << "gt.tailChk:\n"
                << "  %gRem = sub i64 %n, %jg\n"
                << "  %gHasTail = icmp sgt i64 %gRem, 0\n"
                << "  br i1 %gHasTail, label %gt.tailPrep, label %done\n"
                << "gt.tailPrep:\n"
                << "  %gRem16 = trunc i64 %gRem to i16\n"
                << "  %gRemI = insertelement <32 x i16> poison, i16 %gRem16, i32 0\n"
                << "  %gRemV = shufflevector <32 x i16> %gRemI, <32 x i16> poison, <32 x i32> zeroinitializer\n"
                << "  %gtMask = icmp ult <32 x i16> " << iota32 << ", %gRemV\n"
                << "  %gtTkBase = mul i64 %k, %n\n"
                << "  %gtTkOff = add i64 %gtTkBase, %jg\n"
                << "  %gtTkPtr = getelementptr inbounds i16, ptr %d, i64 %gtTkOff\n"
                << "  %gtTVk = call <32 x i16> @llvm.masked.load.v32i16.p0(ptr %gtTkPtr, i32 2, <32 x i1> %gtMask, <32 x i16> " << z32_16 << ")\n"
                << "  br label %gt.tailLoop\n"
                << "gt.tailLoop:\n"
                << "  %gti = phi i64 [ 0, %gt.tailPrep ], [ %gtiNext, %gt.tailLoop ]\n"
                << "  %gtBase = mul i64 %gti, %n\n"
                << "  %gtIk = add i64 %gtBase, %k\n"
                << "  %gtIkp = getelementptr inbounds i16, ptr %d, i64 %gtIk\n"
                << "  %gtDik = load i16, ptr %gtIkp, align 2\n"
                << "  %gtVdikI = insertelement <32 x i16> poison, i16 %gtDik, i32 0\n"
                << "  %gtVdik = shufflevector <32 x i16> %gtVdikI, <32 x i16> poison, <32 x i32> zeroinitializer\n"
                << "  %gtOff = add i64 %gtBase, %jg\n"
                << "  %gtRp = getelementptr inbounds i16, ptr %d, i64 %gtOff\n"
                << "  %gtCur = call <32 x i16> @llvm.masked.load.v32i16.p0(ptr %gtRp, i32 2, <32 x i1> %gtMask, <32 x i16> " << z32_16 << ")\n"
                << "  %gtSum = add <32 x i16> %gtVdik, %gtTVk\n"
                << "  %gtRes = call <32 x i16> @llvm.umin.v32i16(<32 x i16> %gtCur, <32 x i16> %gtSum)\n"
                << "  call void @llvm.masked.store.v32i16.p0(<32 x i16> %gtRes, ptr %gtRp, i32 2, <32 x i1> %gtMask)\n"
                << "  %gtiNext = add i64 %gti, 1\n"
                << "  %gtMore = icmp slt i64 %gtiNext, %n\n"
                << "  br i1 %gtMore, label %gt.tailLoop, label %done\n"
                << "done:\n"
                << "  ret void\n}\n";
        }
        if (usesArgMinU16) {
            usedRawDecls.insert("declare <8 x i16> @llvm.masked.load.v8i16.p0(ptr, i32 immarg, <8 x i1>, <8 x i16>)");
            usedRawDecls.insert("declare <16 x i16> @llvm.masked.load.v16i16.p0(ptr, i32 immarg, <16 x i1>, <16 x i16>)");
            usedRawDecls.insert("declare <32 x i16> @llvm.masked.load.v32i16.p0(ptr, i32 immarg, <32 x i1>, <32 x i16>)");
            usedRawDecls.insert("declare <4 x i32> @llvm.umin.v4i32(<4 x i32>, <4 x i32>)");
            usedRawDecls.insert("declare i32 @llvm.vector.reduce.umin.v4i32(<4 x i32>)");
            usedRawDecls.insert("declare <8 x i32> @llvm.umin.v8i32(<8 x i32>, <8 x i32>)");
            usedRawDecls.insert("declare i32 @llvm.vector.reduce.umin.v8i32(<8 x i32>)");
            usedRawDecls.insert("declare <16 x i32> @llvm.umin.v16i32(<16 x i32>, <16 x i32>)");
            usedRawDecls.insert("declare i32 @llvm.vector.reduce.umin.v16i32(<16 x i32>)");
            usedRawDecls.insert("declare <32 x i16> @llvm.umin.v32i16(<32 x i16>, <32 x i16>)");
            usedRawDecls.insert("declare i16 @llvm.vector.reduce.umin.v32i16(<32 x i16>)");
            usedRawDecls.insert("declare i32 @llvm.cttz.i32(i32, i1 immarg)");

            auto makeIota16 = [](int n, int start) {
                std::string s = "<";
                for (int i = 0; i < n; ++i) {
                    if (i > 0) s += ", ";
                    s += "i16 " + std::to_string(start + i);
                }
                return s + ">";
            };
            auto makeUnpckMask = [](int n, bool hi) {
                std::string s = "<";
                bool first = true;
                for (int lane = 0; lane < n / 8; ++lane) {
                    const int base = lane * 8 + (hi ? 4 : 0);
                    for (int j = 0; j < 4; ++j) {
                        if (!first) s += ", ";
                        first = false;
                        s += "i32 " + std::to_string(base + j) + ", i32 " + std::to_string(n + base + j);
                    }
                }
                return s + ">";
            };
            const std::string iota8_16 = makeIota16(8, 0);
            const std::string iota16_16 = makeIota16(16, 0);
            const std::string iota32_16 = makeIota16(32, 0);
            const std::string splat32_16 = vecConstSplat(32, "i16", "32");
            const std::string allFFFF8 = vecConstSplat(8, "i16", "-1");
            const std::string allFFFF16 = vecConstSplat(16, "i16", "-1");
            const std::string allFFFF32 = vecConstSplat(32, "i16", "-1");
            const std::string unpckLo8 = makeUnpckMask(8, false), unpckHi8 = makeUnpckMask(8, true);
            const std::string unpckLo16 = makeUnpckMask(16, false), unpckHi16 = makeUnpckMask(16, true);
            const std::string unpckLo32 = makeUnpckMask(32, false), unpckHi32 = makeUnpckMask(32, true);

            auto emitMaskN = [&](const std::string& pfx, int n, const std::string& remVar) {
                const std::string scalarTy = "i" + std::to_string(n);
                out << "  %" << pfx << "_sh = shl i64 1, " << remVar << "\n"
                    << "  %" << pfx << "_m64 = sub i64 %" << pfx << "_sh, 1\n";
                if (n < 64) {
                    out << "  %" << pfx << "_ms = trunc i64 %" << pfx << "_m64 to " << scalarTy << "\n";
                }
                const std::string msVar = (n < 64) ? ("%" + pfx + "_ms") : ("%" + pfx + "_m64");
                out << "  %" << pfx << "_mask = bitcast " << scalarTy << " " << msVar << " to <" << n << " x i1>\n";
            };

            // Cold slow fallback for spans > 65500 elements where u16 index cannot fit in low 16 bits.
            out << "define internal i64 @tz.argmin.u16.large(ptr readonly %a, i64 %lo, i64 %hi, i64 %initBest) noinline cold sanitize_address {\n"
                << "entry:\n"
                << "  %len = sub i64 %hi, %lo\n"
                << "  %initPtr = getelementptr inbounds i16, ptr %a, i64 %initBest\n"
                << "  %initVal = load i16, ptr %initPtr, align 2\n"
                << "  %bp0 = getelementptr inbounds i16, ptr %a, i64 %lo\n"
                << "  %v0 = load <32 x i16>, ptr %bp0, align 2\n"
                << "  br label %p1.loop\n"
                << "p1.loop:\n"
                << "  %off = phi i64 [ 32, %entry ], [ %offNext, %p1.loop ]\n"
                << "  %vmin = phi <32 x i16> [ %v0, %entry ], [ %vminNext, %p1.loop ]\n"
                << "  %idx = add i64 %lo, %off\n"
                << "  %vp = getelementptr inbounds i16, ptr %a, i64 %idx\n"
                << "  %v = load <32 x i16>, ptr %vp, align 2\n"
                << "  %vminNext = call <32 x i16> @llvm.umin.v32i16(<32 x i16> %vmin, <32 x i16> %v)\n"
                << "  %offNext = add i64 %off, 32\n"
                << "  %offNextEnd = add i64 %offNext, 32\n"
                << "  %more1 = icmp sle i64 %offNextEnd, %len\n"
                << "  br i1 %more1, label %p1.loop, label %p1.tailCheck\n"
                << "p1.tailCheck:\n"
                << "  %rem = sub i64 %len, %offNext\n"
                << "  %hasTail = icmp sgt i64 %rem, 0\n"
                << "  br i1 %hasTail, label %p1.tailDo, label %p1.reduce\n"
                << "p1.tailDo:\n";
            emitMaskN("lg1", 32, "%rem");
            out << "  %tIdx = add i64 %lo, %offNext\n"
                << "  %tp = getelementptr inbounds i16, ptr %a, i64 %tIdx\n"
                << "  %vt = call <32 x i16> @llvm.masked.load.v32i16.p0(ptr %tp, i32 2, <32 x i1> %lg1_mask, <32 x i16> " << allFFFF32 << ")\n"
                << "  %vminAfter = call <32 x i16> @llvm.umin.v32i16(<32 x i16> %vminNext, <32 x i16> %vt)\n"
                << "  br label %p1.reduce\n"
                << "p1.reduce:\n"
                << "  %vminFin = phi <32 x i16> [ %vminNext, %p1.tailCheck ], [ %vminAfter, %p1.tailDo ]\n"
                << "  %minS = call i16 @llvm.vector.reduce.umin.v32i16(<32 x i16> %vminFin)\n"
                << "  %better = icmp ult i16 %minS, %initVal\n"
                << "  br i1 %better, label %p2.init, label %retInit\n"
                << "retInit:\n"
                << "  ret i64 %initBest\n"
                << "p2.init:\n"
                << "  %msIns = insertelement <32 x i16> poison, i16 %minS, i32 0\n"
                << "  %msSplat = shufflevector <32 x i16> %msIns, <32 x i16> poison, <32 x i32> zeroinitializer\n"
                << "  br label %p2.loop\n"
                << "p2.loop:\n"
                << "  %off2 = phi i64 [ 0, %p2.init ], [ %off2Next, %p2.step ]\n"
                << "  %idx2 = add i64 %lo, %off2\n"
                << "  %vp2 = getelementptr inbounds i16, ptr %a, i64 %idx2\n"
                << "  %v2 = load <32 x i16>, ptr %vp2, align 2\n"
                << "  %eq2 = icmp eq <32 x i16> %v2, %msSplat\n"
                << "  %m32 = bitcast <32 x i1> %eq2 to i32\n"
                << "  %hit2 = icmp ne i32 %m32, 0\n"
                << "  br i1 %hit2, label %p2.foundVec, label %p2.step\n"
                << "p2.foundVec:\n"
                << "  %tz2 = call i32 @llvm.cttz.i32(i32 %m32, i1 true)\n"
                << "  %tz2_64 = zext i32 %tz2 to i64\n"
                << "  %offHit2 = add i64 %off2, %tz2_64\n"
                << "  %ans2 = add i64 %lo, %offHit2\n"
                << "  ret i64 %ans2\n"
                << "p2.step:\n"
                << "  %off2Next = add i64 %off2, 32\n"
                << "  %off2NextEnd = add i64 %off2Next, 32\n"
                << "  %more2 = icmp sle i64 %off2NextEnd, %len\n"
                << "  br i1 %more2, label %p2.loop, label %p2.tail\n"
                << "p2.tail:\n";
            emitMaskN("lg2", 32, "%rem");
            out << "  %tIdx2 = add i64 %lo, %offNext\n"
                << "  %tp2 = getelementptr inbounds i16, ptr %a, i64 %tIdx2\n"
                << "  %vt2 = call <32 x i16> @llvm.masked.load.v32i16.p0(ptr %tp2, i32 2, <32 x i1> %lg2_mask, <32 x i16> " << allFFFF32 << ")\n"
                << "  %eqT = icmp eq <32 x i16> %vt2, %msSplat\n"
                << "  %eqTM = and <32 x i1> %eqT, %lg2_mask\n"
                << "  %mT32 = bitcast <32 x i1> %eqTM to i32\n"
                << "  %tzT = call i32 @llvm.cttz.i32(i32 %mT32, i1 true)\n"
                << "  %tzT64 = zext i32 %tzT to i64\n"
                << "  %offHitT = add i64 %offNext, %tzT64\n"
                << "  %ansT = add i64 %lo, %offHitT\n"
                << "  ret i64 %ansT\n}\n";

            auto emitShortWidth = [&](const std::string& tag, int n, const std::string& iotaN,
                                      const std::string& padN, const std::string& uLo, const std::string& uHi) {
                const int h = n / 2;
                const std::string vTy16 = "<" + std::to_string(n) + " x i16>";
                const std::string mTy = "<" + std::to_string(n) + " x i1>";
                const std::string vTy32 = "<" + std::to_string(h) + " x i32>";
                const std::string mlIntr = "llvm.masked.load.v" + std::to_string(n) + "i16.p0";
                const std::string uminIntr = "llvm.umin.v" + std::to_string(h) + "i32";
                const std::string redIntr = "llvm.vector.reduce.umin.v" + std::to_string(h) + "i32";
                out << tag << ".do:\n";
                emitMaskN(tag, n, "%len");
                out << "  %" << tag << "_p = getelementptr inbounds i16, ptr %a, i64 %lo\n"
                    << "  %" << tag << "_v = call " << vTy16 << " @" << mlIntr << "(ptr %" << tag << "_p, i32 2, "
                    << mTy << " %" << tag << "_mask, " << vTy16 << " " << padN << ")\n"
                    << "  %" << tag << "_lo16 = shufflevector " << vTy16 << " " << iotaN << ", " << vTy16 << " %" << tag << "_v, "
                    << "<" << n << " x i32> " << uLo << "\n"
                    << "  %" << tag << "_hi16 = shufflevector " << vTy16 << " " << iotaN << ", " << vTy16 << " %" << tag << "_v, "
                    << "<" << n << " x i32> " << uHi << "\n"
                    << "  %" << tag << "_lo32 = bitcast " << vTy16 << " %" << tag << "_lo16 to " << vTy32 << "\n"
                    << "  %" << tag << "_hi32 = bitcast " << vTy16 << " %" << tag << "_hi16 to " << vTy32 << "\n"
                    << "  %" << tag << "_m32 = call " << vTy32 << " @" << uminIntr << "(" << vTy32 << " %" << tag << "_lo32, " << vTy32 << " %" << tag << "_hi32)\n"
                    << "  %" << tag << "_r = call i32 @" << redIntr << "(" << vTy32 << " %" << tag << "_m32)\n"
                    << "  %" << tag << "_rv = lshr i32 %" << tag << "_r, 16\n"
                    << "  %" << tag << "_rv16 = trunc i32 %" << tag << "_rv to i16\n"
                    << "  %" << tag << "_bet = icmp ult i16 %" << tag << "_rv16, %initVal\n"
                    << "  %" << tag << "_ri = and i32 %" << tag << "_r, 65535\n"
                    << "  %" << tag << "_ri64 = zext i32 %" << tag << "_ri to i64\n"
                    << "  %" << tag << "_cand = add i64 %lo, %" << tag << "_ri64\n"
                    << "  %" << tag << "_ans = select i1 %" << tag << "_bet, i64 %" << tag << "_cand, i64 %initBest\n"
                    << "  ret i64 %" << tag << "_ans\n";
            };

            out << "define internal i64 @tz.argmin.u16(ptr readonly %a, i64 %lo, i64 %hi, i64 %initBest) sanitize_address {\n"
                << "entry:\n"
                << "  %len = sub i64 %hi, %lo\n"
                << "  %empty = icmp sle i64 %len, 0\n"
                << "  br i1 %empty, label %retInit, label %chkLarge\n"
                << "retInit:\n"
                << "  ret i64 %initBest\n"
                << "chkLarge:\n"
                << "  %isLarge = icmp ugt i64 %len, 65500\n"
                << "  br i1 %isLarge, label %doLarge, label %chk8\n"
                << "doLarge:\n"
                << "  %lgAns = tail call i64 @tz.argmin.u16.large(ptr %a, i64 %lo, i64 %hi, i64 %initBest)\n"
                << "  ret i64 %lgAns\n"
                << "chk8:\n"
                << "  %initPtr = getelementptr inbounds i16, ptr %a, i64 %initBest\n"
                << "  %initVal = load i16, ptr %initPtr, align 2\n"
                << "  %le8 = icmp sle i64 %len, 8\n"
                << "  br i1 %le8, label %s8.do, label %chk16\n"
                << "chk16:\n"
                << "  %le16 = icmp sle i64 %len, 16\n"
                << "  br i1 %le16, label %s16.do, label %chk32\n"
                << "chk32:\n"
                << "  %le32 = icmp sle i64 %len, 32\n"
                << "  br i1 %le32, label %s32.do, label %vec.init\n";

            emitShortWidth("s8", 8, iota8_16, allFFFF8, unpckLo8, unpckHi8);
            emitShortWidth("s16", 16, iota16_16, allFFFF16, unpckLo16, unpckHi16);
            emitShortWidth("s32", 32, iota32_16, allFFFF32, unpckLo32, unpckHi32);

            out << "vec.init:\n"
                << "  %vp0 = getelementptr inbounds i16, ptr %a, i64 %lo\n"
                << "  %vv0 = load <32 x i16>, ptr %vp0, align 2\n"
                << "  %vlo0 = shufflevector <32 x i16> " << iota32_16 << ", <32 x i16> %vv0, <32 x i32> " << unpckLo32 << "\n"
                << "  %vhi0 = shufflevector <32 x i16> " << iota32_16 << ", <32 x i16> %vv0, <32 x i32> " << unpckHi32 << "\n"
                << "  %b0Init = bitcast <32 x i16> %vlo0 to <16 x i32>\n"
                << "  %b1Init = bitcast <32 x i16> %vhi0 to <16 x i32>\n"
                << "  %idx32 = add <32 x i16> " << iota32_16 << ", " << splat32_16 << "\n"
                << "  %hasLoop = icmp sge i64 %len, 64\n"
                << "  br i1 %hasLoop, label %vec.loop, label %vec.tailCheck\n"
                << "vec.loop:\n"
                << "  %off = phi i64 [ 32, %vec.init ], [ %offNext, %vec.loop ]\n"
                << "  %b0 = phi <16 x i32> [ %b0Init, %vec.init ], [ %b0Next, %vec.loop ]\n"
                << "  %b1 = phi <16 x i32> [ %b1Init, %vec.init ], [ %b1Next, %vec.loop ]\n"
                << "  %idx = phi <32 x i16> [ %idx32, %vec.init ], [ %idxNext, %vec.loop ]\n"
                << "  %vOff = add i64 %lo, %off\n"
                << "  %vp = getelementptr inbounds i16, ptr %a, i64 %vOff\n"
                << "  %vv = load <32 x i16>, ptr %vp, align 2\n"
                << "  %vlo = shufflevector <32 x i16> %idx, <32 x i16> %vv, <32 x i32> " << unpckLo32 << "\n"
                << "  %vhi = shufflevector <32 x i16> %idx, <32 x i16> %vv, <32 x i32> " << unpckHi32 << "\n"
                << "  %c0 = bitcast <32 x i16> %vlo to <16 x i32>\n"
                << "  %c1 = bitcast <32 x i16> %vhi to <16 x i32>\n"
                << "  %b0Next = call <16 x i32> @llvm.umin.v16i32(<16 x i32> %b0, <16 x i32> %c0)\n"
                << "  %b1Next = call <16 x i32> @llvm.umin.v16i32(<16 x i32> %b1, <16 x i32> %c1)\n"
                << "  %idxNext = add <32 x i16> %idx, " << splat32_16 << "\n"
                << "  %offNext = add i64 %off, 32\n"
                << "  %offNextEnd = add i64 %offNext, 32\n"
                << "  %more = icmp sle i64 %offNextEnd, %len\n"
                << "  br i1 %more, label %vec.loop, label %vec.tailCheck\n"
                << "vec.tailCheck:\n"
                << "  %offEnd = phi i64 [ 32, %vec.init ], [ %offNext, %vec.loop ]\n"
                << "  %b0T = phi <16 x i32> [ %b0Init, %vec.init ], [ %b0Next, %vec.loop ]\n"
                << "  %b1T = phi <16 x i32> [ %b1Init, %vec.init ], [ %b1Next, %vec.loop ]\n"
                << "  %idxT = phi <32 x i16> [ %idx32, %vec.init ], [ %idxNext, %vec.loop ]\n"
                << "  %rem = sub i64 %len, %offEnd\n"
                << "  %hasTail = icmp sgt i64 %rem, 0\n"
                << "  br i1 %hasTail, label %vec.tail, label %vec.reduce\n"
                << "vec.tail:\n";
            emitMaskN("vt", 32, "%rem");
            out << "  %tOff = add i64 %lo, %offEnd\n"
                << "  %tp = getelementptr inbounds i16, ptr %a, i64 %tOff\n"
                << "  %vt = call <32 x i16> @llvm.masked.load.v32i16.p0(ptr %tp, i32 2, <32 x i1> %vt_mask, <32 x i16> " << allFFFF32 << ")\n"
                << "  %tlo = shufflevector <32 x i16> %idxT, <32 x i16> %vt, <32 x i32> " << unpckLo32 << "\n"
                << "  %thi = shufflevector <32 x i16> %idxT, <32 x i16> %vt, <32 x i32> " << unpckHi32 << "\n"
                << "  %t0 = bitcast <32 x i16> %tlo to <16 x i32>\n"
                << "  %t1 = bitcast <32 x i16> %thi to <16 x i32>\n"
                << "  %b0A = call <16 x i32> @llvm.umin.v16i32(<16 x i32> %b0T, <16 x i32> %t0)\n"
                << "  %b1A = call <16 x i32> @llvm.umin.v16i32(<16 x i32> %b1T, <16 x i32> %t1)\n"
                << "  br label %vec.reduce\n"
                << "vec.reduce:\n"
                << "  %b0F = phi <16 x i32> [ %b0T, %vec.tailCheck ], [ %b0A, %vec.tail ]\n"
                << "  %b1F = phi <16 x i32> [ %b1T, %vec.tailCheck ], [ %b1A, %vec.tail ]\n"
                << "  %bm = call <16 x i32> @llvm.umin.v16i32(<16 x i32> %b0F, <16 x i32> %b1F)\n"
                << "  %vr = call i32 @llvm.vector.reduce.umin.v16i32(<16 x i32> %bm)\n"
                << "  %vrv = lshr i32 %vr, 16\n"
                << "  %vrv16 = trunc i32 %vrv to i16\n"
                << "  %vbet = icmp ult i16 %vrv16, %initVal\n"
                << "  %vri = and i32 %vr, 65535\n"
                << "  %vri64 = zext i32 %vri to i64\n"
                << "  %vcand = add i64 %lo, %vri64\n"
                << "  %vans = select i1 %vbet, i64 %vcand, i64 %initBest\n"
                << "  ret i64 %vans\n}\n";
        }
    }
    void writeEnumTables(std::ostream& out) const {
        for (Type et : usedEnumValTables) {
            const auto& decl = *program.declaration(et);
            out << "@tz.enumval." << static_cast<int>(et) << " = private unnamed_addr constant ["
                << decl.variants.size() << " x i64] [";
            for (std::size_t i = 0; i < decl.variants.size(); ++i) {
                if (i > 0) out << ", ";
                out << "i64 " << decl.variants[i].enumValue;
            }
            out << "]\n";
        }
    }
    void writeDigitSumLuts(std::ostream& out) const {
        for (const auto& [k, b] : usedDigitSumLuts) {
            out << "@tz.dsum." << k << "." << b << " = private unnamed_addr constant ["
                << b << " x i8] [";
            for (std::int64_t v = 0; v < b; ++v) {
                if (v > 0) out << ", ";
                std::int64_t x = v, s = 0;
                while (x > 0) { s += x % k; x /= k; }
                out << "i8 " << s;
            }
            out << "]\n";
        }
    }
    std::string finish(RuntimeABI abi) {
        if (usesSysArgs) usedDecls.insert("tz_sys_set_args");
        RuntimeDiagnostics diagnostics;
        for (const auto id : diagnosticIds) diagnostics.ids.push_back(literal(quoted(id)));
        if (diagnosticFiles.empty()) diagnostics.files.push_back(literal(quoted("")));
        for (const auto& file : diagnosticFiles) diagnostics.files.push_back(literal(quoted(file)));
        diagnostics.prefix = literal("{\"id\":");
        diagnostics.fileSeparator = literal(",\"file\":");
        diagnostics.startSeparator = literal(",\"start\":");
        diagnostics.endSeparator = literal(",\"end\":");
        diagnostics.suffix = literal("}\n");
        diagnostics.minus = literal("-");
        std::ostringstream helpers;
        writeTypedHelpers(helpers);
        std::ostringstream out;
        out << "; Turi, whole program\n";
        writeRuntime(out, abi, diagnostics);
        writeArrayRuntime(out, auditCopies);
        writeStringRuntime(out);
        writeDecls(out);
        writeExternDecls(out);
        writeEnumTables(out);
        writeDigitSumLuts(out);
        writeTypes(out);
        out << helpers.str();
        writeStrings(out);
        out << bodies.str();
        out << coldOutlines.str();
        if (usesSysArgs) {
            out << "define i32 @main(i32 %argc, ptr %argv) sanitize_address {\n"
                << "  call void @tz.init()\n"
                << "  call void @tz_sys_set_args(i32 %argc, ptr %argv)\n"
                << "  call void " << symbol(program.entryMain) << "(i32 1)\n"
                << "  ret i32 0\n}\n";
        } else {
            out << "define i32 @main() sanitize_address {\n  call void @tz.init()\n"
                << "  call void " << symbol(program.entryMain) << "(i32 1)\n  ret i32 0\n}\n";
        }
        for (const auto& m : metadata) out << m << "\n";
        return out.str();
    }
};

} // namespace
} // namespace veloz::native

namespace veloz {
std::string emitLLVM(const Program& p, RuntimeABI abi, bool auditCopies) {
    const auto entry = std::find_if(p.functions.begin(), p.functions.end(),
        [&](const Function& f) { return !f.isTest && f.name == p.entryMain; });
    if (entry == p.functions.end() || !entry->params.empty() || entry->result != Type::Void) {
        Diagnostic d("E_MAIN", "El ejecutable necesita fn main() sin parámetros y con retorno void.",
                     entry == p.functions.end() ? Span{} : entry->span);
        d.expected = "fn main()";
        d.example = "fn main() { print(42) }";
        throw d;
    }
    native::ModuleWriter module(p, auditCopies);
    const native::ReturnRanges returnRanges(p);
    std::vector<native::FunctionIR> functions;
    for (const auto& f : p.functions) {
        if (!f.isTest && !f.isExternDecl) functions.push_back(native::Lowerer(p, f, returnRanges).lower());
    }
    auto safe = native::safeDepths(p, functions);
    std::vector<native::FastClone> fastClones;
    std::deque<Function> closureSpecStorage;
    native::optimizeIR(functions, safe, p.entryMain, !auditCopies, &fastClones, &p, &closureSpecStorage);
    safe = native::safeDepths(p, functions);
    std::unordered_map<std::string, std::string> fastMap;
    for (const auto& fc : fastClones) fastMap.emplace(fc.origName, fc.fastName);
    for (std::size_t i = 0; i < functions.size(); ++i) {
        const auto fit = fastMap.find(functions[i].source.name);
        if (fit != fastMap.end()) {
            const std::string slowName = functions[i].source.name + ".slow";
            module.trampoline(functions[i], fit->second, slowName);
            module.function(functions[i], safe[i], slowName, true, false, false);
        } else {
            module.function(functions[i], safe[i]);
        }
    }
    for (const auto& fc : fastClones) {
        module.function(fc.ir, fc.depthSafe, fc.fastName, false, fc.hasLoop, true);
    }
    return module.finish(abi);
}
} // namespace veloz
