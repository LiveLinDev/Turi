#include "tooling.hpp"
#include "commands.hpp"
#include "project.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_map>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef VELOZ_VERSION
#define VELOZ_VERSION "dev"
#endif

namespace veloz {
namespace {

struct JsonEntry;

struct JsonVal {
    enum Kind { Null, Bool, Int, Double, String, Array, Object };
    Kind kind = Null;
    bool bval = false;
    std::int64_t ival = 0;
    double dval = 0.0;
    std::string sval;
    std::vector<JsonVal> arr;
    std::vector<JsonEntry> obj;

    JsonVal();
    ~JsonVal();
    JsonVal(const JsonVal&);
    JsonVal(JsonVal&&) noexcept;
    JsonVal& operator=(const JsonVal&);
    JsonVal& operator=(JsonVal&&) noexcept;

    const JsonVal* get(std::string_view key) const;
    std::string getStr(std::string_view key, std::string_view def = {}) const;
    std::optional<std::int64_t> getInt(std::string_view key) const;
    bool getBool(std::string_view key, bool def = false) const;
};

struct JsonEntry {
    std::string first;
    JsonVal second;
};

inline JsonVal::JsonVal() = default;
inline JsonVal::~JsonVal() = default;
inline JsonVal::JsonVal(const JsonVal&) = default;
inline JsonVal::JsonVal(JsonVal&&) noexcept = default;
inline JsonVal& JsonVal::operator=(const JsonVal&) = default;
inline JsonVal& JsonVal::operator=(JsonVal&&) noexcept = default;

inline const JsonVal* JsonVal::get(std::string_view key) const {
    if (kind != Object) return nullptr;
    for (const auto& [k, v] : obj) {
        if (k == key) return &v;
    }
    return nullptr;
}
inline std::string JsonVal::getStr(std::string_view key, std::string_view def) const {
    const auto* v = get(key);
    return (v && v->kind == String) ? v->sval : std::string(def);
}
inline std::optional<std::int64_t> JsonVal::getInt(std::string_view key) const {
    const auto* v = get(key);
    if (!v || v->kind != Int) return std::nullopt;
    return v->ival;
}
inline bool JsonVal::getBool(std::string_view key, bool def) const {
    const auto* v = get(key);
    return (v && v->kind == Bool) ? v->bval : def;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80u) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800u) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
}

struct JsonParser {
    std::string_view src;
    std::size_t pos = 0;

    void skipWs() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
            ++pos;
    }
    bool parseHex4(std::uint32_t& out) {
        if (pos + 4 > src.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = src[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(10 + c - 'a');
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(10 + c - 'A');
            else return false;
        }
        return true;
    }
    std::optional<std::string> parseString() {
        if (pos >= src.size() || src[pos] != '"') return std::nullopt;
        ++pos;
        std::string out;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20u) return std::nullopt;
            if (c == '\\') {
                if (pos >= src.size()) return std::nullopt;
                char e = src[pos++];
                if (e == '"' || e == '\\' || e == '/') out += e;
                else if (e == 'b') out += '\b';
                else if (e == 'f') out += '\f';
                else if (e == 'n') out += '\n';
                else if (e == 'r') out += '\r';
                else if (e == 't') out += '\t';
                else if (e == 'u') {
                    std::uint32_t cp = 0;
                    if (!parseHex4(cp)) return std::nullopt;
                    if (cp >= 0xD800u && cp <= 0xDBFFu && pos + 6 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
                        const std::size_t saved = pos;
                        pos += 2;
                        std::uint32_t lo = 0;
                        if (parseHex4(lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                        } else {
                            pos = saved;
                        }
                    }
                    appendUtf8(out, cp);
                } else return std::nullopt;
            } else {
                out += c;
            }
        }
        return std::nullopt;
    }
    std::optional<JsonVal> parseValue(int depth = 0) {
        if (depth > 128) return std::nullopt;
        skipWs();
        if (pos >= src.size()) return std::nullopt;
        char c = src[pos];
        if (c == '"') {
            auto s = parseString();
            if (!s) return std::nullopt;
            JsonVal v;
            v.kind = JsonVal::String;
            v.sval = std::move(*s);
            return v;
        }
        if (c == '{') {
            ++pos;
            skipWs();
            JsonVal v;
            v.kind = JsonVal::Object;
            if (pos < src.size() && src[pos] == '}') { ++pos; return v; }
            while (pos < src.size()) {
                skipWs();
                auto key = parseString();
                if (!key) return std::nullopt;
                skipWs();
                if (pos >= src.size() || src[pos] != ':') return std::nullopt;
                ++pos;
                auto val = parseValue(depth + 1);
                if (!val) return std::nullopt;
                v.obj.push_back(JsonEntry{std::move(*key), std::move(*val)});
                skipWs();
                if (pos < src.size() && src[pos] == ',') { ++pos; continue; }
                if (pos < src.size() && src[pos] == '}') { ++pos; return v; }
                return std::nullopt;
            }
            return std::nullopt;
        }
        if (c == '[') {
            ++pos;
            skipWs();
            JsonVal v;
            v.kind = JsonVal::Array;
            if (pos < src.size() && src[pos] == ']') { ++pos; return v; }
            while (pos < src.size()) {
                auto elem = parseValue(depth + 1);
                if (!elem) return std::nullopt;
                v.arr.push_back(std::move(*elem));
                skipWs();
                if (pos < src.size() && src[pos] == ',') { ++pos; continue; }
                if (pos < src.size() && src[pos] == ']') { ++pos; return v; }
                return std::nullopt;
            }
            return std::nullopt;
        }
        if (src.substr(pos).starts_with("true")) { pos += 4; JsonVal v; v.kind = JsonVal::Bool; v.bval = true; return v; }
        if (src.substr(pos).starts_with("false")) { pos += 5; JsonVal v; v.kind = JsonVal::Bool; v.bval = false; return v; }
        if (src.substr(pos).starts_with("null")) { pos += 4; return JsonVal{}; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const std::size_t start = pos;
            bool isFlt = false;
            if (src[pos] == '-') ++pos;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
            if (pos < src.size() && src[pos] == '.') {
                isFlt = true; ++pos;
                while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
            }
            if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
                isFlt = true; ++pos;
                if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
                while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
            }
            std::string numStr(src.substr(start, pos - start));
            JsonVal v;
            if (isFlt) {
                v.kind = JsonVal::Double;
                char* endp = nullptr;
                v.dval = std::strtod(numStr.c_str(), &endp);
                if (endp != numStr.c_str() + numStr.size()) return std::nullopt;
            } else {
                v.kind = JsonVal::Int;
                auto [endp, ec] = std::from_chars(numStr.data(), numStr.data() + numStr.size(), v.ival);
                if (ec != std::errc{} || endp != numStr.data() + numStr.size()) return std::nullopt;
            }
            return v;
        }
        return std::nullopt;
    }
};

std::optional<JsonVal> parseJsonDoc(std::string_view input) {
    JsonParser p{input, 0};
    auto v = p.parseValue();
    if (!v) return std::nullopt;
    p.skipWs();
    if (p.pos != input.size()) return std::nullopt;
    return v;
}

std::string dumpJson(const JsonVal& v) {
    switch (v.kind) {
        case JsonVal::Null: return "null";
        case JsonVal::Bool: return v.bval ? "true" : "false";
        case JsonVal::Int: return std::to_string(v.ival);
        case JsonVal::Double: {
            std::ostringstream ss;
            ss << v.dval;
            return ss.str();
        }
        case JsonVal::String: return jsonString(v.sval);
        case JsonVal::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                out += dumpJson(v.arr[i]);
            }
            return out + "]";
        }
        case JsonVal::Object: {
            std::string out = "{";
            for (std::size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out += ',';
                out += jsonString(v.obj[i].first) + ":" + dumpJson(v.obj[i].second);
            }
            return out + "}";
        }
    }
    return "null";
}

struct TempDirGuard {
    fs::path dir;
    explicit TempDirGuard(std::string_view prefix) {
        std::random_device rd;
        std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        std::ostringstream ss;
        ss << prefix << std::hex << r;
        dir = fs::temp_directory_path() / ss.str();
        fs::create_directories(dir);
    }
    ~TempDirGuard() {
        if (!dir.empty()) {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    }
    TempDirGuard(TempDirGuard&& other) noexcept : dir(std::move(other.dir)) { other.dir.clear(); }
    TempDirGuard& operator=(TempDirGuard&& other) noexcept {
        if (this != &other) {
            if (!dir.empty()) {
                std::error_code ec;
                fs::remove_all(dir, ec);
            }
            dir = std::move(other.dir);
            other.dir.clear();
        }
        return *this;
    }
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
};

struct DiscardBuf : std::streambuf {
    int_type overflow(int_type c) override { return traits_type::not_eof(c); }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};

std::pair<std::size_t, std::size_t> offsetToZeroBasedLineCol(std::string_view src, std::size_t bytePos) {
    const std::size_t p = std::min(bytePos, src.size());
    std::size_t line = 0;
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i < p; ++i) {
        if (src[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    std::size_t col = 0;
    for (std::size_t i = lineStart; i < p; ++i) {
        if ((static_cast<unsigned char>(src[i]) & 0xC0u) != 0x80u) ++col;
    }
    return {line, col};
}

std::size_t zeroBasedLineColToOffset(std::string_view src, int targetLine, int targetCol) {
    int line = 0;
    std::size_t i = 0;
    while (i < src.size() && line < targetLine) {
        if (src[i++] == '\n') ++line;
    }
    int col = 0;
    while (i < src.size() && src[i] != '\n' && col < targetCol) {
        ++i;
        while (i < src.size() && (static_cast<unsigned char>(src[i]) & 0xC0u) == 0x80u) ++i;
        ++col;
    }
    return i;
}

std::string diagToJsonString(const Diagnostic& d, const fs::path& fallbackFile,
                             std::string_view fallbackSrc, const Program* prog) {
    std::string actualFile(d.file);
    std::string_view actualSource = d.source;
    if (actualFile.empty() && prog && !prog->fileOf(d.span).empty()) actualFile = prog->fileOf(d.span);
    if (actualFile.empty()) actualFile = pathText(fallbackFile);
    if (actualSource.empty()) actualSource = prog ? prog->sourceOf(d.span) : fallbackSrc;
    if (actualSource.empty()) actualSource = fallbackSrc;
    auto [line0, col0] = offsetToZeroBasedLineCol(actualSource, d.span.start);
    return "{\"id\":" + jsonString(d.id) + ",\"message\":" + jsonString(d.message) +
           ",\"file\":" + jsonString(actualFile) +
           ",\"start\":" + std::to_string(d.span.start) + ",\"end\":" + std::to_string(d.span.end) +
           ",\"line\":" + std::to_string(line0 + 1) + ",\"column\":" + std::to_string(col0 + 1) + "}";
}

bool readRpcFrame(std::istream& in, std::string& outBody, bool& usedContentLength, bool allowLineDelimited) {
    outBody.clear();
    while (in) {
        int ch = in.peek();
        if (ch == std::char_traits<char>::eof()) return false;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            in.get();
            continue;
        }
        break;
    }
    if (!in) return false;
    if (allowLineDelimited && (in.peek() == '{' || in.peek() == '[')) {
        usedContentLength = false;
        if (!std::getline(in, outBody)) return false;
        if (!outBody.empty() && outBody.back() == '\r') outBody.pop_back();
        return !outBody.empty();
    }
    usedContentLength = true;
    std::size_t contentLength = 0;
    bool hasLength = false;
    std::string headerLine;
    while (std::getline(in, headerLine)) {
        if (!headerLine.empty() && headerLine.back() == '\r') headerLine.pop_back();
        if (headerLine.empty()) break;
        const auto colon = headerLine.find(':');
        if (colon != std::string::npos) {
            std::string key = headerLine.substr(0, colon);
            for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (key == "content-length") {
                std::size_t pos = colon + 1;
                while (pos < headerLine.size() && (headerLine[pos] == ' ' || headerLine[pos] == '\t')) ++pos;
                std::size_t parsed = 0;
                auto [endp, ec] = std::from_chars(headerLine.data() + pos, headerLine.data() + headerLine.size(), parsed);
                if (ec == std::errc{} && endp == headerLine.data() + headerLine.size() && parsed <= 64u * 1024u * 1024u) {
                    contentLength = parsed;
                    hasLength = true;
                }
            }
        }
    }
    if (!hasLength || !in) return false;
    outBody.resize(contentLength);
    in.read(outBody.data(), static_cast<std::streamsize>(contentLength));
    return in.gcount() == static_cast<std::streamsize>(contentLength);
}

void writeRpcFrame(const std::string& jsonBody, bool useContentLength) {
    if (useContentLength) {
        std::cout << "Content-Length: " << jsonBody.size() << "\r\n\r\n" << jsonBody << std::flush;
    } else {
        std::cout << jsonBody << '\n' << std::flush;
    }
}

std::string rpcIdJson(const JsonVal* idVal) {
    if (!idVal) return "null";
    if (idVal->kind == JsonVal::Int || idVal->kind == JsonVal::String || idVal->kind == JsonVal::Null)
        return dumpJson(*idVal);
    return "null";
}

std::string mcpToolSchema(std::string_view name, std::string_view desc, std::string_view propsJson,
                          std::string_view requiredJson) {
    return "{\"name\":" + jsonString(name) + ",\"description\":" + jsonString(desc) +
           ",\"inputSchema\":{\"type\":\"object\",\"properties\":" + std::string(propsJson) +
           ",\"required\":" + std::string(requiredJson) + "}}";
}

std::string mcpToolResult(std::string_view text, bool isError) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + jsonString(text) +
           "}],\"isError\":" + std::string(isError ? "true" : "false") + "}";
}

struct LoadedMcpInput {
    std::optional<TempDirGuard> tmp;
    fs::path file;
    std::string source;
    SourceOverrides overrides;
};

LoadedMcpInput prepareMcpInput(const JsonVal& args) {
    LoadedMcpInput res;
    const auto fileStr = args.getStr("file");
    const auto* srcVal = args.get("source");
    std::error_code ec;
    if (!fileStr.empty() && fs::exists(pathFrom(fileStr), ec)) {
        res.file = pathFrom(fileStr);
        if (srcVal && srcVal->kind == JsonVal::String) {
            res.source = srcVal->sval;
            auto canon = fs::weakly_canonical(res.file, ec);
            res.overrides[ec ? pathText(res.file) : pathText(canon)] = res.source;
        } else {
            res.source = readFile(res.file);
        }
    } else if (srcVal && srcVal->kind == JsonVal::String) {
        res.tmp.emplace("veloz_mcp_");
        fs::path fname = fileStr.empty() ? fs::path("snippet.turi") : pathFrom(fileStr).filename();
        if (fname.empty()) fname = "snippet.turi";
        res.file = res.tmp->dir / fname;
        res.source = srcVal->sval;
        replaceFile(res.file, res.source);
    } else if (!fileStr.empty()) {
        res.file = pathFrom(fileStr);
        res.source = readFile(res.file);
    } else {
        throw Diagnostic("E_USAGE", "Se requiere 'file' o 'source'", {});
    }
    return res;
}

std::string handleMcpToolCall(const JsonVal& params, bool& outIsError) {
    outIsError = false;
    const std::string tool = params.getStr("name");
    const JsonVal emptyObj;
    const JsonVal& args = (params.get("arguments") && params.get("arguments")->kind == JsonVal::Object)
        ? *params.get("arguments") : emptyObj;

    if (tool == "turi_explain") {
        const auto id = args.getStr("id");
        if (id.empty()) { outIsError = true; return "Missing required argument: id"; }
        try {
            return explain(id);
        } catch (const Diagnostic& d) {
            outIsError = true;
            return diagToJsonString(d, {}, {}, nullptr);
        }
    }

    Program prog;
    bool loaded = false;
    LoadedMcpInput in;
    try {
        in = prepareMcpInput(args);
        if (tool == "turi_fmt") {
            prog = parse(in.source);
            loaded = true;
            return formatSource(in.source);
        }
        if (tool == "turi_fix") {
            struct StrictScope {
                bool previous = strictSyntax();
                StrictScope() { strictSyntax() = true; }
                ~StrictScope() { strictSyntax() = previous; }
            } strictScope; // fix reescribe a la forma canónica
            std::string cur = in.source;
            SourceOverrides ov = in.overrides;
            for (unsigned edits = 0; edits < 64; ++edits) {
                try {
                    Program p = loadProject(in.file, ov);
                    check(p);
                    checkEffects(p);
                    DiscardBuf db;
                    std::ostream q(&db);
                    runTests(p, q);
                    return cur;
                } catch (const Diagnostic& d) {
                    if (!d.fix || (d.id != "E_SEMICOLON" && d.id != "E_LET_MUT" &&
                        d.id != "E_ELIF" && d.id != "E_PRINTLN" && d.id != "E_IMMUTABLE")) {
                        return diagToJsonString(d, in.file, cur, nullptr);
                    }
                    std::error_code ec;
                    const auto canon = fs::weakly_canonical(in.file, ec);
                    const auto origin = !d.file.empty() ? d.file : std::string{};
                    if (!origin.empty() && fs::weakly_canonical(pathFrom(origin), ec) != canon) {
                        return diagToJsonString(d, in.file, cur, nullptr);
                    }
                    cur.replace(d.fix->span.start, d.fix->span.end - d.fix->span.start, d.fix->replacement);
                    ov[ec ? pathText(in.file) : pathText(canon)] = cur;
                }
            }
            return cur;
        }
        prog = loadProject(in.file, in.overrides);
        loaded = true;
        if (tool == "turi_check") {
            check(prog);
            checkEffects(prog);
            if (auto ml = args.getInt("max_level")) {
                checkMemoryLevels(prog, static_cast<int>(*ml));
            }
            DiscardBuf db;
            std::ostream q(&db);
            runTests(prog, q);
            return "{\"ok\":true,\"diagnostics\":[]}";
        }
        if (tool == "turi_sig" || tool == "turi_slice" || tool == "turi_types" || tool == "turi_costs") {
            std::vector<std::string> cmdArgs;
            if (tool == "turi_sig") cmdArgs.push_back("sig");
            else if (tool == "turi_slice") cmdArgs.push_back("slice");
            else if (tool == "turi_types") cmdArgs.push_back("types");
            else cmdArgs.push_back("costes");
            cmdArgs.push_back(pathText(in.file));
            if (auto sym = args.getStr("symbol"); !sym.empty()) cmdArgs.push_back(sym);
            if (args.getBool("explain_checks", false) && tool == "turi_costs") cmdArgs.push_back("--explain-checks");
            if (auto ml = args.getInt("max_level"); ml && tool == "turi_costs") {
                cmdArgs.push_back("--max-level");
                cmdArgs.push_back(std::to_string(*ml));
            }
            if (auto mt = args.getInt("max_tokens")) {
                cmdArgs.push_back("--max-tokens");
                cmdArgs.push_back(std::to_string(*mt));
            }
            if (auto off = args.getInt("offset")) {
                cmdArgs.push_back("--offset");
                cmdArgs.push_back(std::to_string(*off));
            }
            return agentCommand(cmdArgs, in.file, in.source, prog);
        }
        if (tool == "turi_patch") {
            const auto sym = args.getStr("symbol");
            const auto hash = args.getStr("hash");
            const auto repl = args.getStr("replacement");
            if (sym.empty() || hash.empty() || repl.empty() || in.tmp) {
                outIsError = true;
                return "turi_patch requires on-disk 'file', 'symbol', 'hash', and 'replacement'";
            }
            TempDirGuard ptmp("veloz_patch_");
            const auto patchFile = ptmp.dir / "patch.turi";
            replaceFile(patchFile, repl);
            std::vector<std::string> cmdArgs{
                args.getBool("body_only", false) ? "patch-body" : "patch",
                pathText(in.file), sym, hash, pathText(patchFile)
            };
            agentCommand(cmdArgs, in.file, in.source, prog);
            return "{\"ok\":true}";
        }
        if (tool == "turi_run") {
            check(prog);
            checkEffects(prog);
            Limits limits;
            if (auto steps = args.getInt("steps"); steps && *steps > 0) limits.steps = static_cast<std::uint64_t>(*steps);
            std::ostringstream out;
            runMain(prog, out, limits, nullptr);
            return "{\"ok\":true,\"stdout\":" + jsonString(out.str()) + "}";
        }
        outIsError = true;
        return "Unknown tool: " + tool;
    } catch (const Diagnostic& d) {
        if (tool == "turi_check") {
            return "{\"ok\":false,\"diagnostic\":" + diagToJsonString(d, in.file, in.source, loaded ? &prog : nullptr) + "}";
        }
        outIsError = true;
        return diagToJsonString(d, in.file, in.source, loaded ? &prog : nullptr);
    } catch (const std::exception& e) {
        outIsError = true;
        return std::string("Error: ") + e.what();
    }
}

std::string uriToFilePath(std::string_view uri) {
    std::string raw(uri);
    if (raw.starts_with("file://")) raw = raw.substr(7);
    if (raw.starts_with("localhost/")) raw = raw.substr(9);
    std::string decoded;
    decoded.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            int h1 = hexVal(raw[i + 1]), h2 = hexVal(raw[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                decoded += static_cast<char>((h1 << 4) | h2);
                i += 2;
                continue;
            }
        }
        decoded += raw[i];
    }
#ifdef _WIN32
    if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':') {
        decoded.erase(decoded.begin());
    }
#endif
    return decoded;
}

void publishLspDiagnostics(const std::string& uri, const fs::path& filePath, const std::string& src) {
    std::error_code ec;
    std::optional<TempDirGuard> tmp;
    fs::path loadPath = filePath;
    SourceOverrides ov;
    if (!fs::exists(loadPath, ec)) {
        tmp.emplace("veloz_lsp_");
        fs::path fn = filePath.filename();
        if (fn.empty()) fn = "buffer.turi";
        loadPath = tmp->dir / fn;
        replaceFile(loadPath, src);
    } else {
        const auto canon = fs::weakly_canonical(loadPath, ec);
        ov[ec ? pathText(loadPath) : pathText(canon)] = src;
    }
    Program prog;
    bool loaded = false;
    std::string diagArr = "[]";
    try {
        prog = loadProject(loadPath, ov);
        loaded = true;
        check(prog);
        checkEffects(prog);
        DiscardBuf db;
        std::ostream q(&db);
        runTests(prog, q);
    } catch (const Diagnostic& d) {
        std::string_view diagSrc = !d.source.empty() ? std::string_view(d.source)
            : (loaded ? prog.sourceOf(d.span) : std::string_view(src));
        if (diagSrc.empty()) diagSrc = src;
        auto [sl, sc] = offsetToZeroBasedLineCol(diagSrc, d.span.start);
        auto [el, ec2] = offsetToZeroBasedLineCol(diagSrc, std::max(d.span.end, d.span.start + (d.span.start < diagSrc.size() ? 1u : 0u)));
        diagArr = "[{\"range\":{\"start\":{\"line\":" + std::to_string(sl) + ",\"character\":" + std::to_string(sc) +
                  "},\"end\":{\"line\":" + std::to_string(el) + ",\"character\":" + std::to_string(ec2) +
                  "}},\"severity\":1,\"code\":" + jsonString(d.id) + ",\"source\":\"veloz\",\"message\":" +
                  jsonString(d.message) + "}]";
    } catch (const std::exception& e) {
        diagArr = "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}},\"severity\":1,\"code\":\"E_HOST\",\"source\":\"veloz\",\"message\":" +
                  jsonString(e.what()) + "}]";
    }
    const std::string notif = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":" +
                              jsonString(uri) + ",\"diagnostics\":" + diagArr + "}}";
    writeRpcFrame(notif, true);
}

std::string findHoverMarkdown(const Program& prog, const fs::path& filePath, std::size_t bytePos) {
    std::error_code ec;
    const auto targetCanon = fs::weakly_canonical(filePath, ec);
    std::optional<std::size_t> fileIdx;
    for (std::size_t i = 0; i < prog.files.size(); ++i) {
        if (fs::weakly_canonical(pathFrom(prog.files[i].path), ec) == targetCanon) {
            fileIdx = i;
            break;
        }
    }
    auto inTarget = [&](const Span& sp) {
        if (fileIdx && sp.file != *fileIdx) return false;
        return sp.start <= bytePos && bytePos <= std::max(sp.end, sp.start + 1);
    };

    std::size_t bestLen = std::numeric_limits<std::size_t>::max();
    std::string bestInfo;
    auto consider = [&](const Span& sp, const std::string& info) {
        if (!inTarget(sp) || info.empty()) return;
        const std::size_t len = sp.end >= sp.start ? (sp.end - sp.start) : 0;
        if (len <= bestLen) {
            bestLen = len;
            bestInfo = info;
        }
    };

    for (const auto& c : prog.consts) {
        consider(c.span, "```turi\nconst " + c.sourceName + ": " + prog.nameOf(c.type) + "\n```");
    }
    for (const auto& d : prog.data) {
        std::string k = d.isEnum ? "enum " : "struct ";
        consider(d.span, "```turi\n" + k + prog.nameOf(d.type) + "\n```");
    }
    auto visitExpr = [&](auto& selfE, const Expr& e) -> void {
        if (e.type != Type::Unknown) {
            std::string desc = "`" + prog.nameOf(e.type) + "`";
            if (!e.text.empty()) desc = "**" + e.text + "**: " + desc;
            consider(e.span, desc);
        }
        for (const auto& a : e.args) selfE(selfE, a);
        if (e.matchExpr) {
            selfE(selfE, e.matchExpr->scrutinee);
            for (const auto& arm : e.matchExpr->arms)
                if (arm.resultExpr) selfE(selfE, *arm.resultExpr);
        }
        if (e.ifExpr) {
            selfE(selfE, e.ifExpr->condition);
            if (e.ifExpr->thenResult) selfE(selfE, *e.ifExpr->thenResult);
            if (e.ifExpr->elseResult) selfE(selfE, *e.ifExpr->elseResult);
        }
    };
    auto visitBody = [&](auto& selfB, const std::vector<Stmt>& body) -> void {
        for (const auto& st : body) {
            for (const auto& ex : st.exprs) visitExpr(visitExpr, ex);
            selfB(selfB, st.body);
            selfB(selfB, st.otherwise);
            for (const auto& arm : st.arms) selfB(selfB, arm.body);
        }
    };
    for (const auto& fn : prog.functions) {
        if (fn.isTest || fn.isSpecialization) continue;
        std::string sig = "fn " + (fn.sourceName.empty() ? fn.name : fn.sourceName) + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i) sig += ", ";
            sig += fn.params[i].name + ": ";
            if (fn.params[i].writable) sig += "mut ";
            sig += prog.nameOf(fn.params[i].type);
        }
        sig += ")";
        if (fn.result != Type::Void) sig += " -> " + prog.nameOf(fn.result);
        for (const auto& p : fn.params) {
            consider(p.span, "**" + p.name + "**: `" + prog.nameOf(p.type) + "`");
        }
        visitBody(visitBody, fn.body);
        if (bestInfo.empty() && inTarget(fn.span)) bestInfo = "```turi\n" + sig + "\n```";
    }
    return bestInfo;
}

int spawnHidden(const fs::path& exePath) {
#ifdef _WIN32
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hNull = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hNull != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hNull;
        si.hStdError = hNull;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::wstring cmd = L"\"" + exePath.wstring() + L"\"";
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (hNull != INVALID_HANDLE_VALUE) CloseHandle(hNull);
    if (!ok) return 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    std::string exeStr = pathText(exePath);
    char* argvExec[] = {exeStr.data(), nullptr};
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exeStr.c_str(), &fa, nullptr, argvExec, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return 1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return 1;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
#endif
}

} // namespace

int runMcpServer() {
    std::string msg;
    bool useCL = false;
    while (readRpcFrame(std::cin, msg, useCL, true)) {
        auto parsed = parseJsonDoc(msg);
        if (!parsed || parsed->kind != JsonVal::Object) {
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}", useCL);
            continue;
        }
        const JsonVal* idVal = parsed->get("id");
        const std::string method = parsed->getStr("method");
        if (!idVal) continue;
        const std::string idJson = rpcIdJson(idVal);

        if (method == "initialize") {
            const std::string res =
                "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"veloz\",\"version\":" +
                jsonString(VELOZ_VERSION) + "}}";
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"result\":" + res + "}", useCL);
        } else if (method == "ping") {
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"result\":{}}", useCL);
        } else if (method == "tools/list") {
            std::string tools = "[";
            tools += mcpToolSchema("turi_check", "Check types, memory safety, effects, and run tests for a Turi file or snippet.",
                                   "{\"file\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},\"max_level\":{\"type\":\"integer\"}}", "[]") + ",";
            tools += mcpToolSchema("turi_sig", "List Turi function/struct signatures, hashes, and effects.",
                                   "{\"file\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},\"symbol\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"max_tokens\":{\"type\":\"integer\"}}", "[]") + ",";
            tools += mcpToolSchema("turi_slice", "Extract minimal dependency slice around a Turi symbol.",
                                   "{\"file\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},\"symbol\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"max_tokens\":{\"type\":\"integer\"}}", "[\"symbol\"]") + ",";
            tools += mcpToolSchema("turi_types", "Inspect inferred expression types in a Turi file or function.",
                                   "{\"file\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},\"symbol\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"max_tokens\":{\"type\":\"integer\"}}", "[]") + ",";
            tools += mcpToolSchema("turi_costs", "Inspect memory levels and eliminated/kept safety checks.",
                                   "{\"file\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},\"symbol\":{\"type\":\"string\"},\"max_level\":{\"type\":\"integer\"},\"explain_checks\":{\"type\":\"boolean\"},\"offset\":{\"type\":\"integer\"},\"max_tokens\":{\"type\":\"integer\"}}", "[]") + ",";
            tools += mcpToolSchema("turi_fmt", "Format Turi source canonically.",
                                   "{\"file\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"}}", "[]") + ",";
            tools += mcpToolSchema("turi_fix", "Return auto-fixed Turi source for verified mechanical repairs.",
                                   "{\"file\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"}}", "[]") + ",";
            tools += mcpToolSchema("turi_patch", "Apply optimistic hash-verified patch to a symbol or function body.",
                                   "{\"file\":{\"type\":\"string\"},\"symbol\":{\"type\":\"string\"},\"hash\":{\"type\":\"string\"},\"replacement\":{\"type\":\"string\"},\"body_only\":{\"type\":\"boolean\"}}", "[\"file\",\"symbol\",\"hash\",\"replacement\"]") + ",";
            tools += mcpToolSchema("turi_explain", "Explain a Turi diagnostic or runtime trap code.",
                                   "{\"id\":{\"type\":\"string\"}}", "[\"id\"]") + ",";
            tools += mcpToolSchema("turi_run", "Execute a Turi program in the checked interpreter.",
                                   "{\"file\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},\"steps\":{\"type\":\"integer\"}}", "[]");
            tools += "]";
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"result\":{\"tools\":" + tools + "}}", useCL);
        } else if (method == "tools/call") {
            const JsonVal emptyObj;
            const JsonVal& params = (parsed->get("params") && parsed->get("params")->kind == JsonVal::Object)
                ? *parsed->get("params") : emptyObj;
            bool isErr = false;
            std::string outText = handleMcpToolCall(params, isErr);
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"result\":" + mcpToolResult(outText, isErr) + "}", useCL);
        } else {
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}", useCL);
        }
    }
    return 0;
}

int runLspServer() {
    std::unordered_map<std::string, std::string> docs;
    bool shutdownReq = false;
    std::string msg;
    bool useCL = true;
    while (readRpcFrame(std::cin, msg, useCL, false)) {
        auto parsed = parseJsonDoc(msg);
        if (!parsed || parsed->kind != JsonVal::Object) continue;
        const JsonVal* idVal = parsed->get("id");
        const std::string method = parsed->getStr("method");
        const JsonVal emptyObj;
        const JsonVal& params = (parsed->get("params") && parsed->get("params")->kind == JsonVal::Object)
            ? *parsed->get("params") : emptyObj;

        if (method == "initialize") {
            const std::string res =
                "{\"capabilities\":{\"textDocumentSync\":1,\"hoverProvider\":true,\"documentFormattingProvider\":true},"
                "\"serverInfo\":{\"name\":\"veloz-lsp\",\"version\":" + jsonString(VELOZ_VERSION) + "}}";
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + rpcIdJson(idVal) + ",\"result\":" + res + "}", true);
        } else if (method == "shutdown") {
            shutdownReq = true;
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + rpcIdJson(idVal) + ",\"result\":null}", true);
        } else if (method == "exit") {
            return shutdownReq ? 0 : 1;
        } else if (method == "textDocument/didOpen") {
            const auto* td = params.get("textDocument");
            if (td) {
                const std::string uri = td->getStr("uri");
                const std::string text = td->getStr("text");
                docs[uri] = text;
                publishLspDiagnostics(uri, pathFrom(uriToFilePath(uri)), text);
            }
        } else if (method == "textDocument/didChange") {
            const auto* td = params.get("textDocument");
            const auto* changes = params.get("contentChanges");
            if (td && changes && changes->kind == JsonVal::Array && !changes->arr.empty()) {
                const std::string uri = td->getStr("uri");
                const std::string text = changes->arr.back().getStr("text");
                docs[uri] = text;
                publishLspDiagnostics(uri, pathFrom(uriToFilePath(uri)), text);
            }
        } else if (method == "textDocument/didSave") {
            const auto* td = params.get("textDocument");
            if (td) {
                const std::string uri = td->getStr("uri");
                const fs::path p = pathFrom(uriToFilePath(uri));
                std::string text;
                if (const auto* tv = td->get("text"); tv && tv->kind == JsonVal::String) text = tv->sval;
                else if (auto it = docs.find(uri); it != docs.end()) text = it->second;
                else try { text = readFile(p); } catch (...) {}
                docs[uri] = text;
                publishLspDiagnostics(uri, p, text);
            }
        } else if (method == "textDocument/didClose") {
            const auto* td = params.get("textDocument");
            if (td) {
                const std::string uri = td->getStr("uri");
                docs.erase(uri);
                writeRpcFrame("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":" +
                              jsonString(uri) + ",\"diagnostics\":[]}}", true);
            }
        } else if (method == "textDocument/hover" && idVal) {
            const auto* td = params.get("textDocument");
            const auto* pos = params.get("position");
            std::string result = "null";
            if (td && pos) {
                const std::string uri = td->getStr("uri");
                const fs::path fp = pathFrom(uriToFilePath(uri));
                std::string text;
                if (auto it = docs.find(uri); it != docs.end()) text = it->second;
                else try { text = readFile(fp); } catch (...) {}
                std::error_code ec;
                std::optional<TempDirGuard> tmp;
                fs::path loadPath = fp;
                SourceOverrides ov;
                if (!fs::exists(loadPath, ec) && !text.empty()) {
                    tmp.emplace("veloz_lsph_");
                    fs::path fn = fp.filename();
                    if (fn.empty()) fn = "buffer.turi";
                    loadPath = tmp->dir / fn;
                    replaceFile(loadPath, text);
                } else if (!text.empty()) {
                    const auto canon = fs::weakly_canonical(loadPath, ec);
                    ov[ec ? pathText(loadPath) : pathText(canon)] = text;
                }
                try {
                    Program prog = loadProject(loadPath, ov);
                    check(prog);
                    const std::size_t off = zeroBasedLineColToOffset(
                        text, static_cast<int>(pos->getInt("line").value_or(0)),
                        static_cast<int>(pos->getInt("character").value_or(0)));
                    const std::string md = findHoverMarkdown(prog, loadPath, off);
                    if (!md.empty()) result = "{\"contents\":{\"kind\":\"markdown\",\"value\":" + jsonString(md) + "}}";
                } catch (...) {}
            }
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + rpcIdJson(idVal) + ",\"result\":" + result + "}", true);
        } else if (method == "textDocument/formatting" && idVal) {
            const auto* td = params.get("textDocument");
            std::string edits = "[]";
            if (td) {
                const std::string uri = td->getStr("uri");
                std::string text;
                if (auto it = docs.find(uri); it != docs.end()) text = it->second;
                else try { text = readFile(pathFrom(uriToFilePath(uri))); } catch (...) {}
                try {
                    const std::string fmt = formatSource(text);
                    if (fmt != text) {
                        auto [el, ec2] = offsetToZeroBasedLineCol(text, text.size());
                        edits = "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":" +
                                std::to_string(el) + ",\"character\":" + std::to_string(ec2) +
                                "}},\"newText\":" + jsonString(fmt) + "}]";
                    }
                } catch (...) {}
            }
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + rpcIdJson(idVal) + ",\"result\":" + edits + "}", true);
        } else if (idVal) {
            writeRpcFrame("{\"jsonrpc\":\"2.0\",\"id\":" + rpcIdJson(idVal) + ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}", true);
        }
    }
    return shutdownReq ? 0 : 1;
}

int runNewCommand(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        throw Diagnostic("E_USAGE", "Uso: veloz new NOMBRE [--template cli|web|gpu|lib] [--dir RUTA]", {});
    }
    const std::string name = args[1];
    std::string tmpl = "cli";
    fs::path parentDir = fs::current_path();
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--template" && i + 1 < args.size()) tmpl = args[++i];
        else if (args[i] == "--dir" && i + 1 < args.size()) parentDir = pathFrom(args[++i]);
        else throw Diagnostic("E_USAGE", "Uso: veloz new NOMBRE [--template cli|web|gpu|lib] [--dir RUTA]", {});
    }
    if (tmpl != "cli" && tmpl != "web" && tmpl != "gpu" && tmpl != "lib") {
        throw Diagnostic("E_USAGE", "Plantilla inválida; usa cli, web, gpu o lib", {});
    }
    if (name.empty() || name.front() < 'a' || name.front() > 'z' ||
        !std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        })) {
        throw Diagnostic("E_USAGE", "El nombre del paquete debe empezar con a-z y usar solo letras o dígitos", {});
    }
    const fs::path projDir = parentDir / pathFrom(name);
    std::error_code ec;
    if (fs::exists(projDir, ec) && (!fs::is_directory(projDir, ec) || !fs::is_empty(projDir, ec))) {
        throw Diagnostic("E_IO", "El directorio destino ya existe y no está vacío: " + pathText(projDir), {});
    }
    fs::create_directories(projDir / "src");

    std::string pkg = "package " + name + "\nversion 0.1.0\n";
    std::string entryRel = "src/main.turi";
    std::string code;
    if (tmpl == "cli") {
        pkg += "require cli 0.1.0 \"std\"\nrequire data 0.1.0 \"std\"\n";
        code = "import cli\nimport data\n\nfn greet(who: str) -> str {\n    return \"Hola, \" + who\n}\n\n"
               "fn main() {\n    var spec = cli.newSpec(\"" + name + "\", \"Turi CLI\")\n"
               "    cli.addStr(mut spec, \"name\", \"n\", \"Turi\", \"Target name\")\n"
               "    let parsed = cli.parseArgs(spec, [\"--name\", \"Mundo\"])\n"
               "    match parsed {\n        Ok(p) => {\n"
               "            let target = cli.getStr(p, \"name\").unwrapOr(\"Turi\")\n"
               "            let payload = data.JsonVal.Obj([data.JsonField { key: \"msg\", value: data.JsonVal.Str(greet(target)) }])\n"
               "            print(data.toJson(payload))\n        }\n"
               "        Err(e) => eprint(e)\n    }\n}\n\n"
               "test \"greet\" { assert greet(\"AI\") == \"Hola, AI\" }\n";
    } else if (tmpl == "web") {
        pkg += "require web 0.1.0 \"std\"\n";
        code = "import servidor\n\nextern \"C\" fn apiHello(ctx: MutPtr<void>) {\n    servidor.jsonStart(ctx)\n"
               "    servidor.jsonStr(ctx, \"app\", \"" + name + "\")\n    servidor.jsonBool(ctx, \"ok\", true)\n"
               "    servidor.jsonEnd(ctx, 200)\n}\n\nfn routeSetup(srv: servidor.Servidor) {\n"
               "    servidor.staticRoute(srv, \"/health\", 200, \"text/plain; charset=utf-8\", \"ok\")\n"
               "    servidor.getRoute(srv, \"/\", fnPtr(apiHello))\n}\n\nfn main() {\n"
               "    let srv = servidor.newServer(\"127.0.0.1\", 0, 2)\n    routeSetup(srv)\n"
               "    match servidor.startBg(srv) {\n        Ok(p) => {\n            print(\"Listening on port \" + str(p))\n"
               "            servidor.stop(srv)\n        }\n        Err(e) => eprint(e)\n    }\n"
               "    servidor.free(srv)\n}\n\ntest \"buildable\" { assert 1 + 1 == 2 }\n";
    } else if (tmpl == "gpu") {
        pkg += "require gpu 0.1.0 \"std\"\n";
        code = "import gpu\n\nfn main() {\n    let a = [f32(1.0), f32(2.0), f32(3.0), f32(4.0)]\n"
               "    let b = [f32(2.0), f32(0.0), f32(1.0), f32(2.0)]\n"
               "    var c = repeat(f32(0.0), 4)\n"
               "    let _ = gpu.matmulAutoF32(a, b, mut c, 2, 2, 2)\n"
               "    print(str(c.len()) + \" GPU=\" + str(gpu.isAvailable()))\n}\n\n"
               "test \"small_matmul\" {\n    var out = [f32(0.0)]\n"
               "    let r = gpu.matmulAutoF32([f32(1.0), f32(2.0)], [f32(3.0), f32(4.0)], mut out, 1, 2, 1)\n"
               "    assert r.isOk() && out[0] == f32(11.0)\n}\n";
    } else {
        entryRel = "src/" + name + ".turi";
        code = "fn addFast(a: int, b: int) -> int {\n    return a + b\n}\n\n"
               "test \"addFast\" { assert addFast(20, 22) == 42 }\n";
    }

    const fs::path pkgPath = projDir / "veloz.pkg";
    const fs::path entryPath = projDir / pathFrom(entryRel);
    const fs::path readmePath = projDir / "README.md";
    replaceFile(pkgPath, pkg);
    replaceFile(entryPath, code);
    replaceFile(readmePath, "# " + name + "\n\nGenerated with `veloz new " + name + " --template " + tmpl + "`.\n\n"
                            "```sh\nveloz check " + entryRel + "\nveloz build " + entryRel + " -o " + name + " --release\n```\n");

    std::cout << "{\"created\":" << jsonString(pathText(projDir)) << ",\"template\":" << jsonString(tmpl)
              << ",\"files\":[" << jsonString(pathText(pkgPath)) << "," << jsonString(pathText(entryPath))
              << "," << jsonString(pathText(readmePath)) << "]}\n";
    return 0;
}

int runBenchCommand(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        throw Diagnostic("E_USAGE", "Uso: veloz bench ARCHIVO [--rounds N] [--cpu ARCH] [--extreme]", {});
    }
    const fs::path file = pathFrom(args[1]);
    int rounds = 5;
    BuildOptions opt;
    opt.release = true;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--rounds" && i + 1 < args.size()) {
            const auto& v = args[++i];
            auto [endp, ec] = std::from_chars(v.data(), v.data() + v.size(), rounds);
            if (ec != std::errc{} || endp != v.data() + v.size() || rounds < 1 || rounds > 100)
                throw Diagnostic("E_USAGE", "--rounds debe estar entre 1 y 100", {});
        } else if (args[i] == "--cpu" && i + 1 < args.size()) {
            opt.cpu = args[++i];
            if (opt.cpu != "x86-64-v2" && opt.cpu != "x86-64-v3" &&
                opt.cpu != "x86-64-v4" && opt.cpu != "znver4" && opt.cpu != "native")
                throw Diagnostic("E_USAGE", "CPU inválido en --cpu", {});
        } else if (args[i] == "--extreme") {
            opt.extreme = true;
        } else {
            throw Diagnostic("E_USAGE", "Uso: veloz bench ARCHIVO [--rounds N] [--cpu ARCH] [--extreme]", {});
        }
    }

    Program prog = loadProject(file);
    check(prog);
    checkEffects(prog);
    opt.sourcePath = pathText(fs::weakly_canonical(file));

    TempDirGuard tmp("veloz_bench_");
#ifdef _WIN32
    const fs::path exePath = tmp.dir / "bench_target.exe";
#else
    const fs::path exePath = tmp.dir / "bench_target";
#endif
    buildBinary(prog, exePath, opt);
    const auto binBytes = fs::file_size(exePath);

    if (spawnHidden(exePath) != 0) throw Diagnostic("E_BENCH", "Falló el calentamiento del binario", {});
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(rounds));
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        if (spawnHidden(exePath) != 0) throw Diagnostic("E_BENCH", "El binario falló en la iteración de benchmark", {});
        const auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const double minMs = sorted.front();
    const double medMs = sorted.size() % 2 == 1
        ? sorted[sorted.size() / 2]
        : 0.5 * (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]);
    const std::size_t p95Idx = std::min(sorted.size() - 1, static_cast<std::size_t>(std::ceil(0.95 * sorted.size()) - 1.0));
    const double p95Ms = sorted[p95Idx];
    const double meanMs = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());

    std::ostringstream ss;
    ss << "{\"file\":" << jsonString(pathText(file)) << ",\"rounds\":" << rounds
       << ",\"binary_bytes\":" << binBytes
       << ",\"min_ms\":" << minMs << ",\"median_ms\":" << medMs
       << ",\"p95_ms\":" << p95Ms << ",\"mean_ms\":" << meanMs << "}\n";
    std::cout << ss.str();
    return 0;
}

} // namespace veloz
