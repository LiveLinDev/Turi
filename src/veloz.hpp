#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace veloz {
// UTF-8 byte offsets, half-open. file indexes Program::files in a project.
struct Span { std::size_t start = 0, end = 0, file = 0; };
// Modo estricto: solo la grafía canónica (sin `;`, `elif`, `let mut`, snake_case...) y sin inferir
// mutabilidad. Lo usan `veloz fix` (para reescribir a la forma canónica) y `--strict` / VELOZ_STRICT=1.
// Por defecto el front-end es tolerante: acepta lo que solo tiene una lectura posible.
inline bool envEnabled(const char* name) {
#ifdef _WIN32
    char* env = nullptr;
    std::size_t length = 0;
    const bool on = !_dupenv_s(&env, &length, name) && env && env[0] == '1';
    std::free(env);
    return on;
#else
    const char* env = std::getenv(name);
    return env && env[0] == '1';
#endif
}
inline bool& strictSyntax() {
    static bool strict = envEnabled("VELOZ_STRICT");
    return strict;
}
struct Edit { Span span; std::string replacement; };
struct Diagnostic : std::exception {
    std::string id, message;
    Span span;
    std::string expected, actual;
    std::vector<Span> related;
    std::optional<Edit> fix;
    std::string example;
    std::string file, source;
    Diagnostic(std::string code, std::string description, Span at)
        : id(std::move(code)), message(std::move(description)), span(at) {}
    const char* what() const noexcept override { return message.c_str(); }
};
template<class F>
std::optional<Diagnostic> captureDiagnostic(F&& action) {
    try {
        action();
        return {};
    } catch (Diagnostic error) {
        return std::move(error);
    }
}
enum class Type {
    Int, Bool, Str, Void, Unknown, IntArray, Float, StringBuilder,
    File, TcpListener, Socket, Mutex, Lock, Scope, Region,
    I8, I16, I32, U8, U16, U32, U64, F32,
    NamedBase = 32
};
// 2^30: lo mismo que supone la vía rápida (kFastArrayLenMax + 1); el producto de dos longitudes
// sigue cabiendo en 64 bits, y la longitud en los 32 bits de %array.
inline constexpr std::int64_t maxArrayLength = 1LL << 30;
inline constexpr std::uint64_t maxTableBuckets = 1ULL << 24;
inline constexpr std::int64_t maxTableCap = (maxTableBuckets * 7) / 8;
inline constexpr std::int64_t maxPoolCap = 14'680'064;
inline constexpr std::int64_t maxRingCapacity = 1LL << 24;
inline constexpr std::int64_t maxStringLength = 0x3FFFFFFFLL;
enum class ArrayAllocation { None, Region, Owner };
inline std::string typeName(Type t) {
    switch (t) {
        case Type::Int: return "int"; case Type::Bool: return "bool";
        case Type::Str: return "str"; case Type::Void: return "void";
        case Type::IntArray: return "[int]"; case Type::Float: return "float";
        case Type::StringBuilder: return "StringBuilder";
        case Type::File: return "File";
        case Type::TcpListener: return "TcpListener";
        case Type::Socket: return "Socket";
        case Type::Mutex: return "Mutex";
        case Type::Lock: return "Lock";
        case Type::Scope: return "Scope";
        case Type::Region: return "Region";
        case Type::I8: return "i8";
        case Type::I16: return "i16";
        case Type::I32: return "i32";
        case Type::U8: return "u8";
        case Type::U16: return "u16";
        case Type::U32: return "u32";
        case Type::U64: return "u64";
        case Type::F32: return "f32";
        default: return "?";
    }
}
enum class ExprKind { Integer, Float, Boolean, String, Interp, Name, Call, MethodCall, AssocCall, BoxDyn, DynCall, Closure, ClosureCall, Match, If, Unary, Binary, Construct, Field, Index, ArrayLit, Slice, Try };
struct ClosureExpr;
struct MatchExpr;
struct IfExpr;
struct ClosurePtr {
    ClosureExpr* ptr = nullptr;
    ClosurePtr() = default;
    explicit ClosurePtr(ClosureExpr* p) : ptr(p) {}
    inline ClosurePtr(const ClosurePtr& other);
    inline ClosurePtr(ClosurePtr&& other) noexcept;
    inline ClosurePtr& operator=(const ClosurePtr& other);
    inline ClosurePtr& operator=(ClosurePtr&& other) noexcept;
    inline ~ClosurePtr();
    explicit operator bool() const noexcept { return ptr != nullptr; }
    ClosureExpr* operator->() const noexcept { return ptr; }
    ClosureExpr& operator*() const noexcept { return *ptr; }
};
struct MatchPtr {
    MatchExpr* ptr = nullptr;
    MatchPtr() = default;
    explicit MatchPtr(MatchExpr* p) : ptr(p) {}
    inline MatchPtr(const MatchPtr& other);
    inline MatchPtr(MatchPtr&& other) noexcept;
    inline MatchPtr& operator=(const MatchPtr& other);
    inline MatchPtr& operator=(MatchPtr&& other) noexcept;
    inline ~MatchPtr();
    explicit operator bool() const noexcept { return ptr != nullptr; }
    MatchExpr* operator->() const noexcept { return ptr; }
    MatchExpr& operator*() const noexcept { return *ptr; }
};
struct IfPtr {
    IfExpr* ptr = nullptr;
    IfPtr() = default;
    explicit IfPtr(IfExpr* p) : ptr(p) {}
    inline IfPtr(const IfPtr& other);
    inline IfPtr(IfPtr&& other) noexcept;
    inline IfPtr& operator=(const IfPtr& other);
    inline IfPtr& operator=(IfPtr&& other) noexcept;
    inline ~IfPtr();
    explicit operator bool() const noexcept { return ptr != nullptr; }
    IfExpr* operator->() const noexcept { return ptr; }
    IfExpr& operator*() const noexcept { return *ptr; }
};
struct Expr {
    ExprKind kind; Span span;
    // Decoded string, identifier, literal spelling or operator.
    std::string text;
    std::vector<Expr> args;
    Type type = Type::Unknown; // annotated by check
    // Construct: text = "Point" or "Search.Found"; struct arguments have field labels.
    // Field: text = field name, args[0] = base.
    std::vector<std::string> labels{};
    std::vector<std::size_t> fieldIndices{}; // source arguments -> declared field indices
    // repeat -> Region for a direct nonescaping local binding, otherwise Owner.
    // An array Name copied on binding, assignment or return allocates an Owner.
    ArrayAllocation allocation = ArrayAllocation::None;
    bool isMutArg = false;
    bool isResourceMove = false;
    std::vector<Type> typeArgs{};
    bool isEnumConstruct = false;
    Type assocType = Type::Unknown;
    ClosurePtr closure{};
    MatchPtr matchExpr{};
    IfPtr ifExpr{};
};
enum class SafetyTier { Verified = 0, Trusted = 1, Raw = 2 };
enum class StmtKind { Let, Var, Assign, Call, If, While, For, ForEach, Return, Break, Continue, Assert, Print, Match, IndexSet, FieldSet, PlaceSet, SafetyBlock };
struct LValueStep {
    bool isIndex = false;
    std::string field{};
    std::size_t fieldIdx = 0;
    Type resultType = Type::Unknown;
};
struct MatchArm;
struct Stmt {
    StmtKind kind; Span span;
    std::string name; // binding/assignment target or for iterator
    Type annotation = Type::Unknown; // optional local annotation
    std::vector<Expr> exprs; // For: lower, upper; Return: zero or one; FieldSet: [val] or [idx, val]; PlaceSet: [indices..., val]
    std::vector<Stmt> body, otherwise;
    std::vector<MatchArm> arms;
    std::string label{}; // loop label or break/continue target label
    std::vector<std::string> labels{}; // FieldSet nested field names
    std::vector<std::size_t> fieldIndices{}; // FieldSet resolved field indices
    std::vector<Type> fieldTypes{}; // FieldSet intermediate/final field types
    std::vector<LValueStep> placeSteps{}; // PlaceSet mixed .field and [index] chain
    SafetyTier safety = SafetyTier::Verified; // SafetyBlock tier
};
struct MatchArm {
    std::string variant; // "Search.Found" or "_"
    Span span;
    std::vector<std::string> names; // positional field bindings ("_" allowed)
    std::vector<Stmt> body;
    std::optional<Expr> resultExpr{}; // used by ExprKind::Match arms
    std::vector<Type> typeArgs{};
};
struct MatchExpr {
    Expr scrutinee;
    std::vector<MatchArm> arms;
};
inline MatchPtr::MatchPtr(const MatchPtr& other)
    : ptr(other.ptr ? new MatchExpr(*other.ptr) : nullptr) {}
inline MatchPtr::MatchPtr(MatchPtr&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
inline MatchPtr& MatchPtr::operator=(const MatchPtr& other) {
    if (this != &other) {
        MatchExpr* copy = other.ptr ? new MatchExpr(*other.ptr) : nullptr;
        delete ptr;
        ptr = copy;
    }
    return *this;
}
inline MatchPtr& MatchPtr::operator=(MatchPtr&& other) noexcept {
    if (this != &other) {
        delete ptr;
        ptr = other.ptr;
        other.ptr = nullptr;
    }
    return *this;
}
inline MatchPtr::~MatchPtr() { delete ptr; }
struct IfExpr {
    Expr condition;
    std::vector<Stmt> thenBody;
    std::optional<Expr> thenResult;
    std::vector<Stmt> elseBody;
    std::optional<Expr> elseResult;
};
inline IfPtr::IfPtr(const IfPtr& other)
    : ptr(other.ptr ? new IfExpr(*other.ptr) : nullptr) {}
inline IfPtr::IfPtr(IfPtr&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
inline IfPtr& IfPtr::operator=(const IfPtr& other) {
    if (this != &other) {
        IfExpr* copy = other.ptr ? new IfExpr(*other.ptr) : nullptr;
        delete ptr;
        ptr = copy;
    }
    return *this;
}
inline IfPtr& IfPtr::operator=(IfPtr&& other) noexcept {
    if (this != &other) {
        delete ptr;
        ptr = other.ptr;
        other.ptr = nullptr;
    }
    return *this;
}
inline IfPtr::~IfPtr() { delete ptr; }
struct Param { std::string name; Type type; Span span; bool writable = false; bool byRef = false; bool owned = false; };
struct ClosureExpr {
    std::vector<Param> params;
    Type result = Type::Unknown;
    std::vector<Stmt> body;
    bool isExprBody = false;
    std::size_t id = 0;
};
inline ClosurePtr::ClosurePtr(const ClosurePtr& other)
    : ptr(other.ptr ? new ClosureExpr(*other.ptr) : nullptr) {}
inline ClosurePtr::ClosurePtr(ClosurePtr&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
inline ClosurePtr& ClosurePtr::operator=(const ClosurePtr& other) {
    if (this != &other) {
        ClosureExpr* copy = other.ptr ? new ClosureExpr(*other.ptr) : nullptr;
        delete ptr;
        ptr = copy;
    }
    return *this;
}
inline ClosurePtr& ClosurePtr::operator=(ClosurePtr&& other) noexcept {
    if (this != &other) {
        delete ptr;
        ptr = other.ptr;
        other.ptr = nullptr;
    }
    return *this;
}
inline ClosurePtr::~ClosurePtr() { delete ptr; }
struct Function {
    std::string name; Span span; std::vector<Param> params;
    Type result = Type::Void; std::vector<Stmt> body;
    bool isTest = false; // sourceName is the decoded test title
    std::size_t bodyStart = 0; // first byte following the opening brace
    std::vector<Type> generics; // named type IDs scoped to this function
    std::vector<Type> constraints; // one interface or Unknown per generic
    std::vector<std::pair<Type, Type>> methodConstraints; // concrete Self -> interface
    std::vector<Type> substitutedTypes;
    std::string sourceName, module;
    bool isImpl = false, isSpecialization = false, hasDefaultBody = false;
    bool reqPure = false, reqNoAlloc = false, reqCheckFree = false, reqIo = false;
    bool noCopy = false, arenaHint = false, compactHint = false;
    SafetyTier safety = SafetyTier::Verified;
    bool isExternC = false;
    bool isExternDecl = false;
    std::string cSymbol{};
    std::vector<Expr> requiresClauses{};
    std::vector<Expr> ensuresClauses{};
};
struct Variant {
    std::string name; Span span; std::vector<Param> fields;
    std::optional<Expr> enumValueExpr{}; std::int64_t enumValue = 0;
};
struct DataDecl {
    Type type; bool isEnum; Span span;
    std::vector<Variant> variants; // struct has one variant named after itself
    std::string module;
    bool hasExplicitEnumValues = false;
    std::vector<Type> generics{};
    bool reprC = false;
    bool packed = false;
    std::size_t explicitAlign = 0;
};
struct ConstDecl {
    std::string name, sourceName, module;
    Type type = Type::Unknown;
    Expr init{};
    Span span{};
    std::int64_t intVal = 0;
    double floatVal = 0.0;
    bool boolVal = false;
    std::string strVal{};
    std::uint64_t uintVal = 0;
};
struct ImportDecl { std::string name; Span span; };
struct InterfaceDecl {
    Type type; Span span; std::vector<Function> methods; std::string module;
};
struct ImplDecl {
    Type interface, target; Span span; std::vector<Function> methods; std::string module;
    std::vector<Type> generics{};
    std::vector<Type> constraints{};
};
struct SourceFile { std::string path, text; };
struct ModuleInfo {
    std::string name, package; std::vector<std::string> imports; std::size_t file = 0;
};
struct Program {
    struct TypeApp { Type result; std::string baseName; std::vector<Type> args; };
    struct DynVtable { Type iface, target; std::vector<std::string> methods; };
    struct ClosureInfo {
        std::size_t id = 0;
        std::string fnName;
        Type fnType = Type::Unknown;
        Type envStruct = Type::Void;
        std::vector<Type> paramTypes;
        Type result = Type::Void;
    };
    std::vector<Function> functions;
    // Tipos T para los que el programa modifica alguna celda Shared<T> en su sitio (sharedSet o
    // sharedUpdate). El contenido de las demás celdas no cambia nunca tras crearlas, así que se
    // puede leer prestado sin copiarlo.
    std::vector<Type> mutatedCells;
    bool anyCellMutated = false;
    // Análisis de regiones de celdas (native_ir.hpp), calculado una vez por programa.
    mutable std::shared_ptr<void> cellRegionCache;
    mutable std::size_t cellRegionCacheFns = 0;
    bool cellMutated(Type elem) const {
        if (anyCellMutated) return true;
        for (Type t : mutatedCells)
            if (t == elem) return true;
        return false;
    }
    std::vector<std::string> typeNames;
    std::vector<std::pair<Type, Type>> arrayElems;
    std::vector<TypeApp> typeApps;
    std::vector<DataDecl> data;
    std::vector<ConstDecl> consts;
    std::vector<InterfaceDecl> interfaces;
    std::vector<ImplDecl> impls;
    std::vector<DynVtable> dynVtables;
    std::vector<ClosureInfo> closures;
    std::vector<ImportDecl> imports;
    std::vector<SourceFile> files;
    std::vector<ModuleInfo> modules;
    std::vector<std::string> rootModules;
    std::string source;
    std::string entryMain = "main";
    std::vector<std::string> programArgs{};
    std::vector<std::string> linkLibs{};
    const ConstDecl* constant(const std::string& name) const {
        for (const auto& c : consts) if (c.name == name) return &c;
        return nullptr;
    }
    Type arrayOf(Type elem) {
        if (elem == Type::Int) return Type::IntArray;
        for (const auto& [arr, el] : arrayElems) if (el == elem) return arr;
        Type arr = intern("[" + nameOf(elem) + "]");
        arrayElems.emplace_back(arr, elem);
        return arr;
    }
    bool isArray(Type type) const {
        if (type == Type::IntArray) return true;
        for (const auto& [arr, el] : arrayElems) if (arr == type) { (void)el; return true; }
        return false;
    }
    Type elemOf(Type type) const {
        if (type == Type::IntArray) return Type::Int;
        for (const auto& [arr, el] : arrayElems) if (arr == type) return el;
        return Type::Unknown;
    }
    Type typeApp(const std::string& baseName, const std::vector<Type>& args) {
        for (const auto& app : typeApps)
            if (app.baseName == baseName && app.args == args) return app.result;
        std::string display;
        if (baseName == "@Option" && args.size() == 1) {
            display = nameOf(args[0]) + "?";
        } else if (baseName == "@Result" && args.size() == 2) {
            display = "Result<" + nameOf(args[0]) + ", " + nameOf(args[1]) + ">";
        } else if (baseName == "@Map" && args.size() == 2) {
            display = "Map<" + nameOf(args[0]) + ", " + nameOf(args[1]) + ">";
        } else if (baseName == "@Set" && args.size() == 1) {
            display = "Set<" + nameOf(args[0]) + ">";
        } else if (baseName == "@Pool" && args.size() == 1) {
            display = "Pool<" + nameOf(args[0]) + ">";
        } else if (baseName == "@Handle" && args.size() == 1) {
            display = "Handle<" + nameOf(args[0]) + ">";
        } else if (baseName == "@Shared" && args.size() == 1) {
            display = "Shared<" + nameOf(args[0]) + ">";
        } else if (baseName == "@Task" && args.size() == 1) {
            display = "Task<" + nameOf(args[0]) + ">";
        } else if (baseName == "@Frozen" && args.size() == 1) {
            display = "Frozen<" + nameOf(args[0]) + ">";
        } else if (baseName == "@Dyn" && args.size() == 1) {
            display = "dyn " + nameOf(args[0]);
        } else if (baseName == "@Ptr" && args.size() == 1) {
            display = "Ptr<" + nameOf(args[0]) + ">";
        } else if (baseName == "@MutPtr" && args.size() == 1) {
            display = "MutPtr<" + nameOf(args[0]) + ">";
        } else if (baseName == "@Atomic" && args.size() == 1) {
            display = "Atomic<" + nameOf(args[0]) + ">";
        } else if (baseName == "@RingQueue" && args.size() == 1) {
            display = "RingQueue<" + nameOf(args[0]) + ">";
        } else if (baseName.starts_with("@Simd") && baseName.size() > 5 && args.size() == 1) {
            display = "Simd<" + nameOf(args[0]) + ", " + baseName.substr(5) + ">";
        } else if (baseName == "@Fn" && !args.empty()) {
            display = "fn(";
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (i > 1) display += ", ";
                display += nameOf(args[i]);
            }
            display += ")";
            if (args[0] != Type::Void) display += " -> " + nameOf(args[0]);
        } else {
            display = baseName + "<";
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i) display += ", ";
                display += nameOf(args[i]);
            }
            display += ">";
        }
        Type res = intern(display);
        typeApps.push_back({res, baseName, args});
        return res;
    }
    Type optionOf(Type inner) { return typeApp("@Option", {inner}); }
    Type resultOf(Type ok, Type err) { return typeApp("@Result", {ok, err}); }
    Type mapOf(Type key, Type val) { return typeApp("@Map", {key, val}); }
    Type setOf(Type elem) { return typeApp("@Set", {elem}); }
    Type poolOf(Type elem) { return typeApp("@Pool", {elem}); }
    Type handleOf(Type elem) { return typeApp("@Handle", {elem}); }
    Type sharedOf(Type elem) { return typeApp("@Shared", {elem}); }
    Type taskOf(Type elem) { return typeApp("@Task", {elem}); }
    Type frozenOf(Type elem) { return typeApp("@Frozen", {elem}); }
    Type dynOf(Type iface) { return typeApp("@Dyn", {iface}); }
    Type ptrOf(Type elem) { return typeApp("@Ptr", {elem}); }
    Type mutPtrOf(Type elem) { return typeApp("@MutPtr", {elem}); }
    Type atomicOf(Type elem) { return typeApp("@Atomic", {elem}); }
    Type ringQueueOf(Type elem) { return typeApp("@RingQueue", {elem}); }
    Type simdOf(Type elem, unsigned lanes) { return typeApp("@Simd" + std::to_string(lanes), {elem}); }
    Type fnOf(Type ret, const std::vector<Type>& params) {
        std::vector<Type> args;
        args.reserve(1 + params.size());
        args.push_back(ret);
        for (Type p : params) args.push_back(p);
        return typeApp("@Fn", args);
    }
    bool isFn(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Fn" && !app->args.empty();
    }
    Type fnResult(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Fn" && !app->args.empty()) ? app->args[0] : Type::Unknown;
    }
    std::vector<Type> fnParams(Type t) const {
        const auto* app = typeAppOf(t);
        if (!app || app->baseName != "@Fn" || app->args.size() <= 1) return {};
        return {app->args.begin() + 1, app->args.end()};
    }
    const TypeApp* typeAppOf(Type t) const {
        for (const auto& app : typeApps) if (app.result == t) return &app;
        return nullptr;
    }
    bool isOption(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Option" && app->args.size() == 1;
    }
    Type optionInner(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Option" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isResult(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Result" && app->args.size() == 2;
    }
    Type resultOk(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Result" && app->args.size() == 2) ? app->args[0] : Type::Unknown;
    }
    Type resultErr(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Result" && app->args.size() == 2) ? app->args[1] : Type::Unknown;
    }
    bool isMap(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Map" && app->args.size() == 2;
    }
    Type mapKey(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Map" && app->args.size() == 2) ? app->args[0] : Type::Unknown;
    }
    Type mapVal(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Map" && app->args.size() == 2) ? app->args[1] : Type::Unknown;
    }
    bool isSet(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Set" && app->args.size() == 1;
    }
    Type setElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Set" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isPool(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Pool" && app->args.size() == 1;
    }
    Type poolElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Pool" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isHandle(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Handle" && app->args.size() == 1;
    }
    Type handleElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Handle" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isShared(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Shared" && app->args.size() == 1;
    }
    Type sharedElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Shared" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isTask(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Task" && app->args.size() == 1;
    }
    Type taskElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Task" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isFrozen(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Frozen" && app->args.size() == 1;
    }
    Type frozenElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Frozen" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isDyn(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Dyn" && app->args.size() == 1;
    }
    Type dynInterface(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Dyn" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isPtr(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Ptr" && app->args.size() == 1;
    }
    bool isMutPtr(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@MutPtr" && app->args.size() == 1;
    }
    bool isAnyPtr(Type t) const {
        const auto* app = typeAppOf(t);
        return app && (app->baseName == "@Ptr" || app->baseName == "@MutPtr") && app->args.size() == 1;
    }
    Type ptrElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && (app->baseName == "@Ptr" || app->baseName == "@MutPtr") && app->args.size() == 1)
            ? app->args[0] : Type::Unknown;
    }
    bool isAtomic(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@Atomic" && app->args.size() == 1;
    }
    Type atomicElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@Atomic" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isRingQueue(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName == "@RingQueue" && app->args.size() == 1;
    }
    Type ringQueueElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName == "@RingQueue" && app->args.size() == 1) ? app->args[0] : Type::Unknown;
    }
    bool isSimd(Type t) const {
        const auto* app = typeAppOf(t);
        return app && app->baseName.starts_with("@Simd") && app->baseName.size() > 5 && app->args.size() == 1;
    }
    Type simdElem(Type t) const {
        const auto* app = typeAppOf(t);
        return (app && app->baseName.starts_with("@Simd") && app->baseName.size() > 5 && app->args.size() == 1)
            ? app->args[0] : Type::Unknown;
    }
    unsigned simdLanes(Type t) const {
        const auto* app = typeAppOf(t);
        if (!app || !app->baseName.starts_with("@Simd") || app->baseName.size() <= 5 || app->args.size() != 1)
            return 0u;
        return static_cast<unsigned>(std::stoul(app->baseName.substr(5)));
    }
    Type intern(std::string_view name) {
        std::size_t index = 0;
        for (; index < typeNames.size(); ++index) if (typeNames[index] == name) break;
        if (index == typeNames.size()) typeNames.emplace_back(name);
        return static_cast<Type>(static_cast<int>(Type::NamedBase) + index);
    }
    std::string nameOf(Type type) const {
        if (type < Type::NamedBase) return typeName(type);
        return typeNames.at(static_cast<std::size_t>(type) - static_cast<std::size_t>(Type::NamedBase));
    }
    const DataDecl* declaration(Type type) const {
        for (const auto& d : data) if (d.type == type) return &d;
        return nullptr;
    }
    static bool isPrimitiveResource(Type t) {
        return t == Type::File || t == Type::TcpListener || t == Type::Socket ||
               t == Type::Mutex || t == Type::Lock || t == Type::Scope || t == Type::Region;
    }
    static bool isSignedInt(Type t) {
        return t == Type::Int || t == Type::I8 || t == Type::I16 || t == Type::I32;
    }
    static bool isUnsignedInt(Type t) {
        return t == Type::U8 || t == Type::U16 || t == Type::U32 || t == Type::U64;
    }
    static bool isIntType(Type t) {
        return isSignedInt(t) || isUnsignedInt(t);
    }
    static bool isFloatType(Type t) {
        return t == Type::Float || t == Type::F32;
    }
    static bool isNumericType(Type t) {
        return isIntType(t) || isFloatType(t);
    }
    static unsigned intBitWidth(Type t) {
        switch (t) {
            case Type::I8:
            case Type::U8: return 8u;
            case Type::I16:
            case Type::U16: return 16u;
            case Type::I32:
            case Type::U32: return 32u;
            case Type::Int:
            case Type::U64: return 64u;
            default: return 0u;
        }
    }
    bool isResourceImpl(Type t, std::vector<Type>& seen) const {
        if (isPrimitiveResource(t)) return true;
        if (t < Type::NamedBase) return false;
        if (isTask(t)) return true;
        if (isFrozen(t) || isHandle(t) || isShared(t) || isDyn(t) || isFn(t) || isMap(t) || isSet(t) || isPool(t) || isAnyPtr(t) || isSimd(t) || isAtomic(t) || isRingQueue(t)) return false;
        if (isArray(t)) return isResourceImpl(elemOf(t), seen);
        if (isOption(t)) return isResourceImpl(optionInner(t), seen);
        if (isResult(t)) return isResourceImpl(resultOk(t), seen) || isResourceImpl(resultErr(t), seen);
        if (std::find(seen.begin(), seen.end(), t) != seen.end()) return false;
        seen.push_back(t);
        const DataDecl* d = declaration(t);
        if (!d) return false;
        for (const auto& v : d->variants) {
            for (const auto& f : v.fields) {
                if (isResourceImpl(f.type, seen)) return true;
            }
        }
        return false;
    }
    bool isResource(Type t) const {
        if (isPrimitiveResource(t)) return true;
        if (t < Type::NamedBase) return false;
        std::vector<Type> seen;
        return isResourceImpl(t, seen);
    }
    bool isSendableImpl(Type t, std::vector<Type>& seen) const {
        if (t == Type::Lock || t == Type::Scope || t == Type::Region || t == Type::Unknown) return false;
        if (t < Type::NamedBase) return true;
        if (isFrozen(t) || isSimd(t) || isAtomic(t) || isRingQueue(t)) return true;
        if (isTask(t) || isShared(t) || isDyn(t) || isFn(t) || isAnyPtr(t)) return false;
        if (isArray(t)) return isSendableImpl(elemOf(t), seen);
        if (isOption(t)) return isSendableImpl(optionInner(t), seen);
        if (isResult(t)) return isSendableImpl(resultOk(t), seen) && isSendableImpl(resultErr(t), seen);
        if (isMap(t)) return isSendableImpl(mapKey(t), seen) && isSendableImpl(mapVal(t), seen);
        if (isSet(t)) return isSendableImpl(setElem(t), seen);
        if (isPool(t)) return isSendableImpl(poolElem(t), seen);
        if (isHandle(t)) return isSendableImpl(handleElem(t), seen);
        if (std::find(seen.begin(), seen.end(), t) != seen.end()) return true;
        seen.push_back(t);
        const DataDecl* d = declaration(t);
        if (!d) return false;
        for (const auto& v : d->variants) {
            for (const auto& f : v.fields) {
                if (!isSendableImpl(f.type, seen)) return false;
            }
        }
        return true;
    }
    bool isSendable(Type t) const {
        std::vector<Type> seen;
        return isSendableImpl(t, seen);
    }
    std::string_view sourceOf(Span span) const {
        if (span.file < files.size()) return files[span.file].text;
        return source;
    }
    std::string fileOf(Span span) const {
        if (span.file < files.size()) return files[span.file].path;
        return {};
    }
    bool visible(const std::string& from, const std::string& symbol) const {
        if (from.empty() || symbol.empty() || symbol.starts_with("@")) return true;
        const auto dot = symbol.find('.');
        if (dot == std::string::npos) return true;
        const auto imported = symbol.substr(0, dot);
        if (imported == from) return true;
        for (const auto& module : modules)
            if (module.name == from)
                return std::find(module.imports.begin(), module.imports.end(), imported) != module.imports.end();
        return false;
    }
};
Program parse(std::string_view source);
// Tipos del programa que se guardan en celdas porque se contienen a sí mismos (semantics.cpp).
std::vector<std::string> recursiveTypeNames(const Program& program);
void parseInto(Program& program, std::string_view source, std::string_view module, std::size_t file);
void check(Program& program); // throws Diagnostic; populates Expr::type
struct Limits { std::uint64_t steps = 10000000; unsigned depth = 256; };
// main() has zero parameters and returns void; optional for check.
void runMain(const Program& program, std::ostream& output, Limits limits = {}, std::istream* input = nullptr);
void runTests(const Program& program, std::ostream& output, Limits limits = {});
enum class RuntimeABI { Windows, Linux, Darwin };
#if defined(_WIN32)
inline constexpr auto hostABI = RuntimeABI::Windows;
#elif defined(__APPLE__)
inline constexpr auto hostABI = RuntimeABI::Darwin;
#else
inline constexpr auto hostABI = RuntimeABI::Linux;
#endif
std::string emitLLVM(const Program& program, RuntimeABI abi = hostABI, bool auditCopies = false);
}
