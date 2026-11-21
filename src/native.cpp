#include "native_ir.hpp"
#include "native_runtime.hpp"
#include "native_arrays.hpp"

#include <charconv>
#include <map>
#include <sstream>

namespace veloz::native {
namespace {

std::string llvmType(Type type) {
    if (type >= Type::NamedBase) return "%t" + std::to_string(static_cast<int>(type));
    switch (type) {
    case Type::Int: return "i64";
    case Type::Bool: return "i1";
    case Type::Str: return "%text";
    case Type::IntArray: return "%array";
    case Type::Void: return "void";
    default: throw std::logic_error("unresolved type during LLVM emission");
    }
}

std::string reg(Value v) { return "%v" + std::to_string(v.id); }
std::string operand(Value v) { return llvmType(v.type) + " " + reg(v); }

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
    std::ostringstream bodies;
    bool emitsRegion = false;

    static void fieldList(std::ostream& out, const Variant& variant) {
        out << "{ ";
        for (std::size_t i = 0; i < variant.fields.size(); ++i) {
            if (i > 0) out << ", ";
            out << llvmType(variant.fields[i].type);
        }
        out << " }";
    }
    void writeTypes(std::ostream& out) const {
        for (const auto& decl : program.data) {
            out << llvmType(decl.type) << " = type ";
            if (decl.isEnum) {
                out << "{ i32";
                for (const auto& variant : decl.variants) {
                    out << ", ";
                    fieldList(out, variant);
                }
                out << " }";
            } else {
                fieldList(out, decl.variants.at(0));
            }
            out << "\n";
        }
    }
    void construct(const Operation& op) {
        const auto& decl = *program.declaration(op.result.type);
        const auto result = reg(op.result);
        const auto type = llvmType(op.result.type);
        std::string aggregate = "zeroinitializer";
        if (decl.isEnum) {
            const auto tagged = op.inputs.empty() ? result : result + ".tag";
            bodies << "  " << tagged << " = insertvalue " << type << " " << aggregate
                   << ", i32 " << op.slot << ", 0\n";
            aggregate = tagged;
        } else if (op.inputs.empty()) {
            bodies << "  " << result << " = freeze " << type << " zeroinitializer\n";
        }
        for (std::size_t i = 0; i < op.inputs.size(); ++i) {
            const auto name = i + 1 == op.inputs.size() ? result : result + ".field" + std::to_string(i);
            bodies << "  " << name << " = insertvalue " << type << " " << aggregate
                   << ", " << operand(op.inputs[i]) << ", ";
            if (decl.isEnum) bodies << op.slot + 1 << ", ";
            bodies << op.fields.at(i) << "\n";
            aggregate = name;
        }
    }
    std::string literal(const std::string& text) {
        const auto [it, inserted] = pool.emplace(text, static_cast<unsigned>(pool.size()));
        return "%text { ptr @tz.s" + std::to_string(it->second) + ", i64 " + std::to_string(text.size()) + " }";
    }
    std::string diagnostic(std::string id, Span span) {
        return literal("{\"id\":\"" + id + "\",\"start\":" + std::to_string(span.start)
            + ",\"end\":" + std::to_string(span.end) + "}\n");
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
    void binary(const Operation& op) {
        const auto left = op.inputs[0];
        const auto right = op.inputs[1];
        const auto result = reg(op.result);
        const auto& token = op.data;
        if (token == "rangeIncrement") {
            bodies << "  " << result << " = add " << operand(left) << ", " << reg(right) << "\n";
            return;
        }
        if (token == "+" || token == "-" || token == "*" || token == "/" || token == "%") {
            static const std::map<std::string, std::string> functions{
                {"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"}, {"%", "rem"}};
            bodies << "  " << result << " = call i64 @tz." << functions.at(token) << "("
                   << operand(left) << ", " << operand(right) << ", ";
            if (token == "/" || token == "%") bodies << diagnostic("R_DIV_ZERO", op.span) << ", ";
            bodies << diagnostic("R_OVERFLOW", op.span) << ")\n";
            return;
        }
        if (left.type == Type::Str) {
            const auto same = token == "==" ? result : result + ".eq";
            bodies << "  " << same << " = call i1 @tz.eq(" << operand(left) << ", " << operand(right) << ")\n";
            if (token == "!=") bodies << "  " << result << " = xor i1 " << same << ", true\n";
            return;
        }
        static const std::map<std::string, std::string> predicates{
            {"==", "eq"}, {"!=", "ne"}, {"<", "slt"}, {"<=", "sle"}, {">", "sgt"}, {">=", "sge"}};
        bodies << "  " << result << " = icmp " << predicates.at(token) << " " << operand(left)
               << ", " << reg(right) << "\n";
    }
    void operation(const Operation& op) {
        const auto result = reg(op.result);
        const auto type = llvmType(op.result.type);
        const auto first = [&] { return operand(op.inputs.at(0)); };
        switch (op.op) {
        case Op::Int: {
            std::int64_t n{};
            const auto parsed = std::from_chars(op.data.data(), op.data.data() + op.data.size(), n);
            if (parsed.ec != std::errc{} || parsed.ptr != op.data.data() + op.data.size()) {
                throw std::logic_error("invalid checked integer literal");
            }
            bodies << "  " << result << " = add i64 0, " << n << "\n";
            break;
        }
        case Op::Bool: bodies << "  " << result << " = or i1 false, " << op.data << "\n"; break;
        case Op::Text: bodies << "  " << result << " = freeze " << literal(op.data) << "\n"; break;
        case Op::Load:
            bodies << "  " << result << " = load " << type << ", ptr %slot" << op.slot << "\n";
            break;
        case Op::Store: bodies << "  store " << first() << ", ptr %slot" << op.slot << "\n"; break;
        case Op::Call:
            bodies << "  call void @tz.guard(i1 %atLimit, " << diagnostic("R_DEPTH", op.span) << ")\n";
            bodies << "  ";
            if (op.result.type != Type::Void) bodies << result << " = ";
            bodies << "call " << type << " " << symbol(op.data) << "(i32 %childDepth";
            for (const auto v : op.inputs) bodies << ", " << operand(v);
            bodies << ")\n";
            break;
        case Op::Length: bodies << "  " << result << " = extractvalue " << first() << ", 1\n"; break;
        case Op::ReadByte:
            bodies << "  " << result << ".raw = call i32 @getchar()\n"
                   << "  " << result << " = sext i32 " << result << ".raw to i64\n";
            break;
        case Op::RepeatHeap:
        case Op::RepeatRegion:
            bodies << "  " << result << " = call %array @tz.repeat(" << first() << ", " << operand(op.inputs[1])
                   << ", ptr " << (op.op == Op::RepeatRegion ? "%arena" : "null") << ", i1 "
                   << (op.initialize ? "true" : "false") << ", "
                   << diagnostic("R_ARRAY_SIZE", op.span) << ", " << diagnostic("R_ALLOC", op.span) << ")\n";
            break;
        case Op::Clone:
            bodies << "  " << result << " = call %array @tz.clone(" << first() << ", "
                   << diagnostic("R_ALLOC", op.span) << ")\n";
            break;
        case Op::Drop: bodies << "  call void @tz.drop(ptr %slot" << op.slot << ")\n"; break;
        case Op::ClearOwner: bodies << "  store %array zeroinitializer, ptr %slot" << op.slot << "\n"; break;
        case Op::RegionMark: bodies << "  " << result << " = call %mark @tz.mark(ptr %arena)\n"; break;
        case Op::RegionRewind:
            bodies << "  call void @tz.rewind(ptr %arena, %mark " << reg(op.inputs.at(0)) << ")\n";
            break;
        case Op::Index:
        case Op::IndexSet:
            bodies << "  " << result << ".ptr = call ptr @tz.at(" << first() << ", " << operand(op.inputs.at(1))
                   << ", " << diagnostic("R_BOUNDS", op.span) << ")\n";
            if (op.op == Op::Index) bodies << "  " << result << " = load i64, ptr " << result << ".ptr\n";
            else bodies << "  store " << operand(op.inputs.at(2)) << ", ptr " << result << ".ptr\n";
            break;
        case Op::Construct: construct(op); break;
        case Op::Field:
            bodies << "  " << result << " = extractvalue " << first();
            for (auto field : op.fields) bodies << ", " << field;
            bodies << "\n";
            break;
        case Op::Tag:
            bodies << "  " << result << ".tag = extractvalue " << first() << ", 0\n"
                   << "  " << result << " = zext i32 " << result << ".tag to i64\n";
            break;
        case Op::Negate:
            bodies << "  " << result << " = call i64 @tz.sub(i64 0, " << first() << ", "
                   << diagnostic("R_OVERFLOW", op.span) << ")\n";
            break;
        case Op::Not: bodies << "  " << result << " = xor " << first() << ", true\n"; break;
        case Op::Binary: binary(op); break;
        case Op::Assert:
            bodies << "  " << result << ".bad = xor " << first() << ", true\n"
                   << "  call void @tz.guard(i1 " << result << ".bad, " << diagnostic("R_ASSERT", op.span) << ")\n";
            break;
        case Op::Print:
            switch (op.inputs[0].type) {
            case Type::Int: bodies << "  call void @tz.printInt(" << first() << ")\n"; break;
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
        switch (block.exit) {
        case Exit::Return:
            if (emitsRegion) bodies << "  call void @tz.finish(ptr %arena)\n";
            bodies << "  ret " << (block.value ? operand(*block.value) : "void") << "\n";
            break;
        case Exit::Jump: bodies << "  br label %b" << block.yes << "\n"; break;
        case Exit::Branch:
            bodies << "  br " << operand(*block.value) << ", label %b" << block.yes << ", label %b" << block.no << "\n";
            break;
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
    explicit ModuleWriter(const Program& program) : program(program) {}
    void function(const FunctionIR& ir) {
        const auto& f = ir.source;
        emitsRegion = ir.usesRegion;
        bodies << "define internal " << llvmType(f.result) << " " << symbol(f.name) << "(i32 %depth";
        for (unsigned i = 0; i < f.params.size(); ++i) bodies << ", " << llvmType(f.params[i].type) << " %arg" << i;
        bodies << ") sanitize_address {\nentry:\n  %atLimit = icmp uge i32 %depth, 256\n"
               << "  %childDepth = add i32 %depth, 1\n";
        if (ir.usesRegion) bodies << "  %arena = alloca ptr\n  store ptr null, ptr %arena\n";
        for (unsigned i = 0; i < ir.slots.size(); ++i) {
            bodies << "  %slot" << i << " = alloca " << llvmType(ir.slots[i]) << "\n";
            if (ir.slots[i] == Type::IntArray) bodies << "  store %array zeroinitializer, ptr %slot" << i << "\n";
        }
        for (unsigned i = 0; i < f.params.size(); ++i) {
            bodies << "  store " << llvmType(f.params[i].type) << " %arg" << i << ", ptr %slot" << i << "\n";
        }
        bodies << "  br label %b0\n";
        for (unsigned i = 0; i < ir.blocks.size(); ++i) writeBlock(i, ir.blocks[i]);
        bodies << "}\n";
    }
    std::string finish() {
        std::ostringstream out;
        out << "; Turi, whole program\n";
        writeRuntime(out);
        writeArrayRuntime(out);
        writeTypes(out);
        writeStrings(out);
        out << bodies.str() << "define i32 @main() sanitize_address {\n  call void @tz.init()\n"
            << "  call void @turi.main(i32 1)\n  ret i32 0\n}\n";
        return out.str();
    }
};

} // namespace
} // namespace veloz::native

namespace veloz {
std::string emitLLVM(const Program& p) {
    const auto entry = std::find_if(p.functions.begin(), p.functions.end(),
        [](const Function& f) { return !f.isTest && f.name == "main"; });
    if (entry == p.functions.end() || !entry->params.empty() || entry->result != Type::Void) {
        Diagnostic d("E_MAIN", "El ejecutable necesita fn main() sin parámetros y con retorno void.",
                     entry == p.functions.end() ? Span{} : entry->span);
        d.expected = "fn main()";
        d.example = "fn main() { print(42) }";
        throw d;
    }
    native::ModuleWriter module(p);
    for (const auto& f : p.functions) {
        if (!f.isTest) module.function(native::Lowerer(p, f).lower());
    }
    return module.finish();
}
} // namespace veloz
