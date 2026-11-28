#pragma once

#include "native_opt.hpp"

#include <charconv>

namespace veloz::native {

inline std::optional<std::uint64_t> unsignedLiteral(std::string_view spelling) {
    int base = 10;
    if (spelling.starts_with("0x") || spelling.starts_with("0X")) {
        base = 16;
        spelling.remove_prefix(2);
    } else if (spelling.starts_with("0b") || spelling.starts_with("0B")) {
        base = 2;
        spelling.remove_prefix(2);
    }
    std::uint64_t value = 0;
    const char* end = spelling.data() + spelling.size();
    const auto parsed = std::from_chars(spelling.data(), end, value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return value;
}

// Las tablas constantes usan el descriptor normal de un arreglo. Solo se
// comparten si los dueños son locales y todas las vistas son préstamos de lectura.
class ConstArrayReads {
    using Parameter = std::pair<const FunctionIR*, unsigned>;
    using Slots = std::unordered_set<unsigned>;

    const Program& program;
    std::unordered_map<std::string, std::vector<const FunctionIR*>> callees;

    bool immutablePointerResult(const Operation& op) const {
        if (op.op != Op::Builtin || !program.isPtr(op.result.type)) return false;
        return op.data == "slicePtr" || op.data == "ptrAdd" || op.data == "ptrCast";
    }

    bool readOnlyInput(const Operation& op, const Value& value) const {
        if (program.isArray(value.type)) {
            if (op.op == Op::Index || op.op == Op::Length || op.op == Op::Slice || op.op == Op::Clone)
                return true;
            return op.op == Op::Builtin && op.data == "simdFromArray";
        }
        if (!program.isPtr(value.type) || op.op != Op::Builtin) return false;
        if (op.data == "ptrRead") return Program::isNumericType(op.result.type) || op.result.type == Type::Bool;
        return op.data == "simdLoad" || op.data == "simdLoadAligned";
    }

    bool inspect(const FunctionIR& ir, const Operation* literal, unsigned parameter,
                 const Slots& owners, std::vector<Parameter>& calls) const {
        Slots aliases = owners;
        Slots values;
        if (literal) values.insert(literal->result.id);
        else {
            if (parameter >= ir.slots.size() ||
                (parameter < ir.slotByRef.size() && ir.slotByRef[parameter])) return false;
            aliases.insert(parameter);
        }

        bool grew = true;
        while (grew) {
            grew = false;
            for (const auto& block : ir.blocks) {
                for (const auto& op : block.ops) {
                    if (op.op == Op::Load && aliases.contains(op.slot)) {
                        grew |= values.insert(op.result.id).second;
                    } else if (!op.inputs.empty() && values.contains(op.inputs.front().id)) {
                        if (immutablePointerResult(op)) grew |= values.insert(op.result.id).second;
                        else if (op.op == Op::Store && program.isPtr(op.inputs.front().type))
                            grew |= aliases.insert(op.slot).second;
                    }
                }
            }
        }

        for (const auto& block : ir.blocks) {
            if (block.value && values.contains(block.value->id)) return false;
            for (const auto& op : block.ops) {
                if (std::any_of(op.mutArgSlots.begin(), op.mutArgSlots.end(),
                                [&](unsigned slot) { return aliases.contains(slot); })) return false;
                if (aliases.contains(op.slot)) {
                    if (op.op == Op::PlaceSet || op.op == Op::FieldSet || op.op == Op::ClearField) return false;
                    if (op.op == Op::Drop && !owners.contains(op.slot)) return false;
                    if (op.op == Op::Store && !program.isPtr(ir.slots.at(op.slot)) &&
                        (!literal || op.inputs.empty() || op.inputs[0].id != literal->result.id)) return false;
                }
                for (unsigned index = 0; index < op.inputs.size(); ++index) {
                    const Value value = op.inputs[index];
                    if (!values.contains(value.id)) continue;
                    if (op.op == Op::Call) {
                        const auto target = callees.find(op.data);
                        if (target == callees.end()) return false;
                        for (const auto* variant : target->second) {
                            if (index >= variant->source.params.size() || variant->source.params[index].byRef)
                                return false;
                            calls.emplace_back(variant, index);
                        }
                    } else if (index != 0) {
                        return false;
                    } else if (op.op == Op::Store) {
                        if (op.slot < ir.source.params.size() ||
                            (op.slot < ir.slotByRef.size() && ir.slotByRef[op.slot])) return false;
                        if (!owners.contains(op.slot) && !program.isPtr(value.type)) return false;
                    } else if (!immutablePointerResult(op) && !readOnlyInput(op, value)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

public:
    ConstArrayReads(const Program& program, const std::vector<FunctionIR>& functions,
                    const std::vector<FastClone>& clones) : program(program) {
        for (const auto& ir : functions) callees[ir.source.name].push_back(&ir);
        for (const auto& clone : clones) {
            callees[clone.origName].push_back(&clone.ir);
            if (clone.fastName != clone.origName) callees[clone.fastName].push_back(&clone.ir);
        }
    }

    bool mayShare(const FunctionIR& ir, const Operation& literal, Slots& owners) const {
        for (const auto& block : ir.blocks) {
            for (const auto& op : block.ops) {
                if (op.op == Op::Store && !op.inputs.empty() && op.inputs[0].id == literal.result.id) {
                    if (op.slot >= ir.slots.size() || ir.slots[op.slot] != literal.result.type ||
                        op.slot < ir.source.params.size() ||
                        (op.slot < ir.slotByRef.size() && ir.slotByRef[op.slot])) return false;
                    owners.insert(op.slot);
                }
            }
        }
        std::vector<Parameter> work;
        if (owners.empty() || !inspect(ir, &literal, 0, owners, work)) return false;
        // Una llamada recursiva cuenta como un préstamo si cada función del
        // componente solo lee. Ningún resultado parcial se guarda entre tablas.
        std::set<Parameter> seen;
        while (!work.empty()) {
            const Parameter next = work.back();
            work.pop_back();
            if (!seen.insert(next).second) continue;
            if (!inspect(*next.first, nullptr, next.second, {}, work)) return false;
        }
        return true;
    }
};

} // namespace veloz::native
