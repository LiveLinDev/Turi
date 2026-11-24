#pragma once
#include "veloz.hpp"
#include <algorithm>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace veloz {

struct VariantLayout {
    std::vector<unsigned> physOrder;
    std::vector<unsigned> logicalToPhys;
    std::vector<unsigned> logicalToLlvm;
    std::vector<std::size_t> physOffsets;
    std::vector<std::size_t> llvmPrePad;
    std::size_t size = 0;
    std::size_t align = 1;
    std::size_t llvmNaturalAlign = 1;
};

struct TypeLayout {
    bool isEnum = false;
    bool multiPayload = false;
    bool packed = false;
    bool reprC = false;
    unsigned payloadVariants = 0;
    unsigned singlePayloadVariant = 0;
    std::string tagTy = "i8";
    std::size_t tagSize = 1;
    std::string wordTy = "i8";
    std::size_t wordBytes = 1;
    std::size_t numWords = 0;
    std::size_t payPrePad = 0;
    unsigned payFieldIdx = 1;
    std::size_t naturalSize = 0;
    std::size_t trailingPad = 0;
    std::size_t explicitAlign = 0;
    std::size_t size = 0;
    std::size_t align = 1;
    std::size_t llvmNaturalAlign = 1;
    std::vector<std::size_t> singlePayPrePad;
    std::vector<unsigned> singlePayLogicalToLlvm;
    std::vector<VariantLayout> variants;
};

class LayoutCalculator {
    const Program& program;
    mutable std::map<Type, TypeLayout> layoutCache;

public:
    explicit LayoutCalculator(const Program& p) : program(p) {}

    static constexpr std::size_t alignUp(std::size_t v, std::size_t a) {
        return a <= 1 ? v : ((v + a - 1) / a) * a;
    }

    std::pair<std::size_t, std::size_t> sizeAlignOf(Type type) const {
        if (type == Type::IntArray || program.isArray(type)) return {16, 8};
        if (program.isMap(type) || program.isSet(type)) return {32, 8};
        if (program.isPool(type)) return {24, 8};
        if (program.isHandle(type)) return {8, 4};
        if (program.isShared(type) || program.isTask(type) || program.isFrozen(type) ||
            program.isAtomic(type) || program.isRingQueue(type)) return {8, 8};
        if (program.isDyn(type) || program.isFn(type)) return {16, 8};
        if (program.isAnyPtr(type)) return {8, 8};
        if (program.isSimd(type)) {
            const auto [es, ea] = sizeAlignOf(program.simdElem(type));
            (void)ea;
            const std::size_t sz = es * program.simdLanes(type);
            return {sz, sz};
        }
        if (type >= Type::NamedBase) {
            const auto& tl = layoutOf(type);
            return {tl.size, tl.align};
        }
        switch (type) {
            case Type::Bool:
            case Type::I8:
            case Type::U8:
                return {1, 1};
            case Type::I16:
            case Type::U16:
                return {2, 2};
            case Type::I32:
            case Type::U32:
            case Type::F32:
                return {4, 4};
            case Type::Int:
            case Type::U64:
            case Type::Float:
                return {8, 8};
            case Type::Str:
            case Type::StringBuilder:
                return {16, 8};
            case Type::File:
            case Type::TcpListener:
            case Type::Socket:
            case Type::Mutex:
            case Type::Lock:
            case Type::Scope:
            case Type::Region:
                return {8, 8};
            case Type::Void:
                return {0, 1};
            default:
                throw std::logic_error("unresolved type during layout calculation");
        }
    }

    std::size_t llvmNaturalAlignOf(Type type) const {
        if (type == Type::IntArray || program.isArray(type) ||
            program.isMap(type) || program.isSet(type) || program.isPool(type) ||
            program.isShared(type) || program.isTask(type) || program.isFrozen(type) ||
            program.isAtomic(type) || program.isRingQueue(type) ||
            program.isDyn(type) || program.isFn(type) || program.isAnyPtr(type)) {
            return 8;
        }
        if (program.isHandle(type)) return 4;
        if (program.isSimd(type)) return sizeAlignOf(type).second;
        if (type >= Type::NamedBase) return layoutOf(type).llvmNaturalAlign;
        switch (type) {
            case Type::Bool:
            case Type::I8:
            case Type::U8:
            case Type::Void:
                return 1;
            case Type::I16:
            case Type::U16:
                return 2;
            case Type::I32:
            case Type::U32:
            case Type::F32:
                return 4;
            default:
                return 8;
        }
    }

    VariantLayout computeVariantLayout(const Variant& v, bool keepOrder, bool packed) const {
        VariantLayout vl;
        const std::size_t k = v.fields.size();
        vl.physOrder.resize(k);
        vl.logicalToPhys.resize(k);
        vl.logicalToLlvm.resize(k);
        vl.physOffsets.resize(k);
        vl.llvmPrePad.assign(k, 0);
        std::vector<std::pair<std::size_t, std::size_t>> sa(k);
        for (std::size_t i = 0; i < k; ++i) {
            sa[i] = sizeAlignOf(v.fields[i].type);
            vl.physOrder[i] = static_cast<unsigned>(i);
        }
        if (!keepOrder && !packed) {
            std::stable_sort(vl.physOrder.begin(), vl.physOrder.end(), [&](unsigned a, unsigned b) {
                if (sa[a].second != sa[b].second) return sa[a].second > sa[b].second;
                if (sa[a].first != sa[b].first) return sa[a].first > sa[b].first;
                return a < b;
            });
        }
        std::size_t off = 0;
        std::size_t llvmOff = 0;
        std::size_t maxA = 1;
        std::size_t maxNatA = 1;
        unsigned curLlvmIdx = 0;
        for (unsigned phys = 0; phys < static_cast<unsigned>(k); ++phys) {
            const unsigned logIdx = vl.physOrder[phys];
            vl.logicalToPhys[logIdx] = phys;
            const auto [fs, rawA] = sa[logIdx];
            const std::size_t fa = packed ? 1 : rawA;
            const std::size_t natA = packed ? 1 : llvmNaturalAlignOf(v.fields[logIdx].type);
            off = alignUp(off, fa);
            if (!packed && alignUp(llvmOff, natA) < off) {
                vl.llvmPrePad[phys] = off - llvmOff;
                ++curLlvmIdx;
            }
            vl.logicalToLlvm[logIdx] = curLlvmIdx++;
            vl.physOffsets[phys] = off;
            off += fs;
            llvmOff = off;
            if (!packed) {
                maxA = std::max(maxA, fa);
                maxNatA = std::max(maxNatA, natA);
            }
        }
        vl.align = packed ? 1 : maxA;
        vl.llvmNaturalAlign = packed ? 1 : maxNatA;
        vl.size = packed ? off : alignUp(off, maxA);
        return vl;
    }

    const TypeLayout& layoutOf(Type type) const {
        if (auto it = layoutCache.find(type); it != layoutCache.end()) return it->second;
        const auto* declPtr = program.declaration(type);
        if (!declPtr) throw std::logic_error("missing DataDecl during layout calculation");
        const auto& decl = *declPtr;
        TypeLayout tl;
        tl.isEnum = decl.isEnum;
        tl.reprC = !decl.isEnum && decl.reprC;
        tl.packed = !decl.isEnum && decl.packed;
        tl.explicitAlign = decl.explicitAlign;
        const bool keepOrder = tl.reprC || tl.packed;
        for (std::size_t vi = 0; vi < decl.variants.size(); ++vi) {
            tl.variants.push_back(computeVariantLayout(decl.variants[vi], keepOrder, tl.packed));
            if (!decl.variants[vi].fields.empty()) {
                ++tl.payloadVariants;
                tl.singlePayloadVariant = static_cast<unsigned>(vi);
            }
        }
        if (!tl.isEnum) {
            if (!tl.variants.empty()) {
                tl.naturalSize = tl.variants[0].size;
                tl.align = tl.variants[0].align;
                tl.llvmNaturalAlign = tl.variants[0].llvmNaturalAlign;
            }
            if (!tl.packed && tl.explicitAlign > 0) tl.align = std::max(tl.align, tl.explicitAlign);
            tl.size = tl.packed ? tl.naturalSize : alignUp(tl.naturalSize, tl.align);
            tl.trailingPad = tl.size > tl.naturalSize ? (tl.size - tl.naturalSize) : 0;
        } else {
            const bool smallTag = decl.variants.size() <= 256;
            tl.tagTy = smallTag ? "i8" : "i32";
            tl.tagSize = smallTag ? 1 : 4;
            if (tl.payloadVariants == 0) {
                tl.naturalSize = tl.tagSize;
                tl.align = tl.tagSize;
                tl.llvmNaturalAlign = tl.tagSize;
            } else if (tl.payloadVariants == 1) {
                const auto& pv = decl.variants[tl.singlePayloadVariant];
                const auto& pvl = tl.variants[tl.singlePayloadVariant];
                tl.singlePayPrePad.assign(pv.fields.size(), 0);
                tl.singlePayLogicalToLlvm.assign(pv.fields.size(), 0);
                std::size_t off = tl.tagSize;
                std::size_t llvmOff = tl.tagSize;
                std::size_t maxA = tl.tagSize;
                std::size_t maxNatA = tl.tagSize;
                unsigned curLlvmIdx = 1;
                for (std::size_t pi = 0; pi < pvl.physOrder.size(); ++pi) {
                    const unsigned logIdx = pvl.physOrder[pi];
                    const Type ft = pv.fields[logIdx].type;
                    const auto [fs, fa] = sizeAlignOf(ft);
                    const std::size_t natA = llvmNaturalAlignOf(ft);
                    off = alignUp(off, fa);
                    if (alignUp(llvmOff, natA) < off) {
                        tl.singlePayPrePad[pi] = off - llvmOff;
                        ++curLlvmIdx;
                    }
                    tl.singlePayLogicalToLlvm[logIdx] = curLlvmIdx++;
                    off += fs;
                    llvmOff = off;
                    maxA = std::max(maxA, fa);
                    maxNatA = std::max(maxNatA, natA);
                }
                tl.align = maxA;
                tl.llvmNaturalAlign = maxNatA;
                tl.naturalSize = alignUp(off, maxA);
            } else {
                tl.multiPayload = true;
                std::size_t maxPaySize = 0;
                std::size_t maxPayAlign = 1;
                for (std::size_t vi = 0; vi < decl.variants.size(); ++vi) {
                    if (decl.variants[vi].fields.empty()) continue;
                    maxPaySize = std::max(maxPaySize, tl.variants[vi].size);
                    maxPayAlign = std::max(maxPayAlign, tl.variants[vi].align);
                }
                if (maxPayAlign >= 8) { tl.wordTy = "i64"; tl.wordBytes = 8; }
                else if (maxPayAlign >= 4) { tl.wordTy = "i32"; tl.wordBytes = 4; }
                else { tl.wordTy = "i8"; tl.wordBytes = 1; }
                tl.numWords = std::max<std::size_t>(1, (maxPaySize + tl.wordBytes - 1) / tl.wordBytes);
                const std::size_t payAlign = std::max(tl.wordBytes, maxPayAlign);
                const std::size_t payOff = alignUp(tl.tagSize, payAlign);
                if (alignUp(tl.tagSize, tl.wordBytes) < payOff) {
                    tl.payPrePad = payOff - tl.tagSize;
                    tl.payFieldIdx = 2;
                } else {
                    tl.payPrePad = 0;
                    tl.payFieldIdx = 1;
                }
                tl.align = std::max(tl.tagSize, payAlign);
                tl.llvmNaturalAlign = std::max(tl.tagSize, tl.wordBytes);
                tl.naturalSize = alignUp(payOff + tl.numWords * tl.wordBytes, tl.align);
            }
            if (tl.explicitAlign > 0) tl.align = std::max(tl.align, tl.explicitAlign);
            tl.size = alignUp(tl.naturalSize, tl.align);
            tl.trailingPad = tl.size > tl.naturalSize ? (tl.size - tl.naturalSize) : 0;
        }
        return layoutCache.emplace(type, std::move(tl)).first->second;
    }
};

inline bool isBitcastableImpl(const Program& p, Type t, std::vector<Type>& seen) {
    if (Program::isNumericType(t) || p.isAnyPtr(t) || p.isSimd(t)) return true;
    if (t < Type::NamedBase) return false;
    if (p.isArray(t) || p.isMap(t) || p.isSet(t) || p.isPool(t) || p.isHandle(t) ||
        p.isShared(t) || p.isTask(t) || p.isFrozen(t) || p.isAtomic(t) || p.isRingQueue(t) ||
        p.isDyn(t) || p.isFn(t))
        return false;
    if (std::find(seen.begin(), seen.end(), t) != seen.end()) return false;
    seen.push_back(t);
    const auto* decl = p.declaration(t);
    if (!decl || decl->isEnum || decl->variants.empty()) return false;
    for (const auto& f : decl->variants[0].fields) {
        if (!isBitcastableImpl(p, f.type, seen)) return false;
    }
    seen.pop_back();
    return true;
}

inline bool isBitcastable(const Program& p, Type t) {
    std::vector<Type> seen;
    return isBitcastableImpl(p, t, seen);
}

} // namespace veloz
