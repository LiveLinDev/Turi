"""Per-language adapters: signature shown in the prompt, test driver, build and run commands.

The model writes only the function (plus imports/includes and helpers). The driver is appended by
the harness, runs every test and prints one canonical line per test:
  int -> decimal, bool -> true/false, float -> any float text, str -> raw text,
  list -> [a|b|c]
A runtime failure of one test must not hide the others when the language makes that cheap.
"""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ".exe" if sys.platform == "win32" else ""


def camel(name):
    parts = name.split("_")
    return parts[0] + "".join(p[:1].upper() + p[1:] for p in parts[1:])


def canon(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, list):
        return "[" + "|".join(canon(v) for v in value) + "]"
    return str(value)


def same(ret, expected_text, actual_text):
    if ret == "float":
        try:
            a, b = float(expected_text), float(actual_text)
        except ValueError:
            return False
        return abs(a - b) <= 1e-6 * max(1.0, abs(a))
    return expected_text == actual_text


def which(*names):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def _clang(cxx=False):
    exe = "clang++" if cxx else "clang"
    found = which(exe)
    if found:
        return found
    candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / (exe + EXE)
    return str(candidate) if candidate.exists() else None


class Lang:
    name = ""
    title = ""
    ext = ""
    fence = ""
    card = ""          # extra system text; only for languages absent from the training data
    reply_rule = ("Reply with exactly one fenced code block containing the function, any imports or "
                  "includes it needs and any helper functions. Do not write a main function, tests or prose.")

    def fn(self, task):
        return task.name

    def available(self):
        return True

    def signature(self, task):
        raise NotImplementedError

    def call_text(self, task, args):
        """Human-readable call used in the public examples of the prompt."""
        return f"{self.fn(task)}({', '.join(self.lit(v, t) for v, (_, t) in zip(args, task.params))})"

    def lit(self, value, typ):
        raise NotImplementedError

    def program(self, task, code):
        """Return {filename: text}; the first file is the entry point."""
        raise NotImplementedError

    def commands(self, workdir, files):
        """Return (build_cmd or None, run_cmd)."""
        raise NotImplementedError

    def notes(self, task):
        return ""


def _str_lit(value):
    return json.dumps(value, ensure_ascii=False)


# ----------------------------------------------------------------------------- Python
class Python(Lang):
    name, title, ext, fence = "python", "Python 3", ".py", "python"
    _types = {"int": "int", "float": "float", "bool": "bool", "str": "str",
              "list[int]": "list[int]", "list[str]": "list[str]"}

    def signature(self, task):
        params = ", ".join(f"{n}: {self._types[t]}" for n, t in task.params)
        return f"def {task.name}({params}) -> {self._types[task.ret]}:"

    def lit(self, value, typ):
        return repr(value)

    def program(self, task, code):
        lines = [code, "", "def _show(v):",
                 "    if isinstance(v, bool): return 'true' if v else 'false'",
                 "    if isinstance(v, float): return repr(v)",
                 "    if isinstance(v, int): return str(v)",
                 "    if isinstance(v, (list, tuple)): return '[' + '|'.join(_show(x) for x in v) + ']'",
                 "    return str(v)", ""]
        for args, _ in task.tests:
            call = f"{task.name}({', '.join(repr(a) for a in args)})"
            lines += ["try:", f"    print(_show({call}))", "except Exception:", "    print('ERR')"]
        return {"main.py": "\n".join(lines) + "\n"}

    def commands(self, workdir, files):
        return None, [sys.executable, "-I", str(workdir / "main.py")]


# ----------------------------------------------------------------------------- Turi
TURI_CARD = """Turi is a compiled, memory-safe language. Values are copied on assignment; there is no null, no GC and no manual free.

```turi
fn sumPos(nums: [int]) -> int {
    var total = 0
    for x in nums {
        if x > 0 { total += x }
    }
    total
}
fn initials(s: str) -> str {
    s.split(" ").filter(|w| w.len() > 0).map(|w| w[0..1].upper()).join("")
}
fn wordCount(s: str) -> Map<str, int> {
    var m: Map<str, int> = mapNew()
    for w in s.words() { m[w] += 1 }
    m
}
```

Types: int (64-bit), float (64-bit), bool, str, [T] arrays, T? optionals, Map<K, V>, Set<T>, tuples `(int, str)` (`let (a, b) = t`, `t.0`), struct, enum. A struct or enum may contain itself (`struct Node { value: int, next: Node? }`). No nested functions: write helper functions at top level.
Declarations: `let x = 1`, `var x = 1`, `let xs: [int] = []` (an empty array literal needs a type). The last expression of a function is its result; `return` also works.
Control flow: `if c { } else if d { } else { }`, `while c { }`, `for i in 0..n { }` (n excluded), `for i in 0..=n { }`, `for x in arr { }`, `break`, `continue`. `if` is an expression and `c ? a : b` works.
Operators: `+ - * / %` (int division truncates, `%` keeps the sign of the dividend), `== != < <= > >=`, `&& || !`, `& | ^ << >>`. Convert with `float(i)`, `int(f)`, `str(x)`. Integer overflow traps.
Arrays: `[1, 2]`, `repeat(0, n)`, `a.len()`, `a[i]`, `a[i] = v`, `a.push(v)`, `a.pop()`, `a[lo..hi]` (copy), `a.sort()` (in place), `a.map(|x| x * 2)`, `a.filter(|x| x > 0)`, `a.sum()`, `a.max()`, `a.min()`, `a.contains(x)`, `a.indexOf(x)` (-1 if absent), `a.reverse()` (new array), `a.concat(b)`, `a.any(|x| ..)`, `a.all(|x| ..)`. Out-of-range indexes trap.
Strings (immutable bytes): `s.len()`, `s[i]` is the byte as an int, `for c in s { }` gives one-character strings, `'a'` compares with both (`s[i] == 'a'`, `c == 'a'`), `ord(c)` is the code and `chr(n)` the character, `s[lo..hi]` substring, `a + b`, `strFromByte(code)`, `s.split(sep)`, `parts.join(sep)`, `s.words()`, `s.chars()`, `s.trim()`, `s.upper()`, `s.lower()`, `s.replace(a, b)`, `s.repeat(n)`, `s.reverse()`, `s.count(t)`, `s.contains(t)`, `s.startsWith(t)`, `s.endsWith(t)`, `s.indexOf(t)` (-1 if absent), `s == t`, `s < t`, `parseInt(s)`, `"x = {x}"` interpolates.
Maps and sets: `var m: Map<str, int> = mapNew()`, `m[k] = v`, `m[k] += 1`, `m[k]` (zero value if absent), `m.get(k)` returns `V?`, `m.has(k)`, `m.del(k)`, `m.len()`, `m.keys()`, `m.values()`; `var s: Set<int> = setNew()`, `s.add(x)`, `s.has(x)`.
Optionals: `Some(v)`, `None`, `match o { Some(v) => ..., None => ... }`, `o.isSome()`, `o.unwrapOr(d)`.
Math: `x.abs()`, `min(a, b)`, `max(a, b)`, `f.sqrt()`, `f.floor()`, `x.pow(n)`.
"""


class Turi(Lang):
    name, title, ext, fence = "turi", "Turi", ".turi", "turi"
    card = TURI_CARD
    _types = {"int": "int", "float": "float", "bool": "bool", "str": "str",
              "list[int]": "[int]", "list[str]": "[str]"}

    def __init__(self, compiler=None):
        self.compiler = compiler or os.environ.get("VELOZ") or str(ROOT / "build" / ("veloz" + EXE))

    def available(self):
        return Path(self.compiler).exists()

    def fn(self, task):
        return camel(task.name)

    def signature(self, task):
        params = ", ".join(f"{camel(n)}: {self._types[t]}" for n, t in task.params)
        return f"fn {self.fn(task)}({params}) -> {self._types[task.ret]}"

    def lit(self, value, typ):
        if typ == "bool":
            return "true" if value else "false"
        if typ == "str":
            return self._str(value)
        if typ == "float":
            return repr(float(value))
        if typ.startswith("list"):
            inner = typ[5:-1]
            return "[" + ", ".join(self.lit(v, inner) for v in value) + "]"
        return str(value)

    @staticmethod
    def _str(value):
        out = json.dumps(value, ensure_ascii=False)
        return out.replace("{", "\\{").replace("}", "\\}")

    def program(self, task, code):
        lines = [code, "",
                 "fn tzShowInts(a: [int]) -> str {",
                 "    var s = \"[\"",
                 "    for i in 0..a.len() {",
                 "        if i > 0 { s = s + \"|\" }",
                 "        s = s + str(a[i])",
                 "    }",
                 "    return s + \"]\"",
                 "}",
                 "fn tzShowStrs(a: [str]) -> str {",
                 "    var s = \"[\"",
                 "    for i in 0..a.len() {",
                 "        if i > 0 { s = s + \"|\" }",
                 "        s = s + a[i]",
                 "    }",
                 "    return s + \"]\"",
                 "}",
                 "fn main() {"]
        for k, (args, _) in enumerate(task.tests):
            names = []
            for j, (value, (_, typ)) in enumerate(zip(args, task.params)):
                if typ.startswith("list"):
                    var = f"t{k}a{j}"
                    lines.append(f"    let {var}: {self._types[typ]} = {self.lit(value, typ)}")
                    names.append(var)
                else:
                    names.append(self.lit(value, typ))
            call = f"{self.fn(task)}({', '.join(names)})"
            if task.ret == "list[int]":
                call = f"tzShowInts({call})"
            elif task.ret == "list[str]":
                call = f"tzShowStrs({call})"
            lines.append(f"    print({call})")
        lines.append("}")
        return {"main.turi": "\n".join(lines) + "\n"}

    def commands(self, workdir, files):
        exe = workdir / ("prog_bin" + EXE)
        return [self.compiler, "build", str(workdir / "main.turi"), "-o", str(exe)], [str(exe)]


# ----------------------------------------------------------------------------- Python syntax on the Turi compiler
TURIPY_CARD = """Write ordinary Python 3 with type hints on every function parameter and result. The code is compiled ahead of time to machine code by the Turi compiler (no interpreter), so keep to the static core of the language: functions, int/float/bool/str, list, dict, set and tuple, loops, slices, comprehensions, f-strings and the usual built-ins (len, range, sum, min, max, sorted, enumerate, zip, abs, ord, chr, str, int, float) and str/list/dict methods. No classes, no try/except, no imports other than math, no sorted(key=...)."""


class TuriPy(Turi):
    """The model writes Python; tools/pyfront translates it to Turi and the Turi compiler builds it."""
    name, title, ext, fence = "turipy", "Python 3", ".py", "python"
    # Sin tarjeta: el encargo es idéntico al de Python. Con TURIPY_CARD el modelo se volvía cauteloso
    # (anotaba cada variable y evitaba sorted, split...) y la solución pasaba de 36 a 66 tokens.
    card = ""
    _py = {"int": "int", "float": "float", "bool": "bool", "str": "str",
           "list[int]": "list[int]", "list[str]": "list[str]"}

    def fn(self, task):
        return task.name

    def signature(self, task):
        params = ", ".join(f"{n}: {self._py[t]}" for n, t in task.params)
        return f"def {task.name}({params}) -> {self._py[task.ret]}:"

    def call_text(self, task, args):
        return f"{task.name}({', '.join(repr(a) for a in args)})"

    def program(self, task, code):
        sys.path.insert(0, str(ROOT / "tools" / "pyfront"))
        import py2turi
        try:
            turi = py2turi.translate(code)
        except py2turi.Unsupported as error:
            return {"main.turi": "", "py2turi.err": f"py2turi: {error}\n"}
        except SyntaxError as error:
            return {"main.turi": "", "py2turi.err": f"SyntaxError: {error}\n"}
        files = super().program(task, turi)
        files["answer.py"] = code
        return files

    def commands(self, workdir, files):
        if "py2turi.err" in files:
            show = "import sys; sys.stderr.write(open(sys.argv[1], encoding='utf-8').read()); sys.exit(1)"
            return [sys.executable, "-I", "-c", show, str(workdir / "py2turi.err")], [sys.executable, "-c", "pass"]
        return super().commands(workdir, files)


# ----------------------------------------------------------------------------- C
class C(Lang):
    name, title, ext, fence = "c", "C (C17)", ".c", "c"

    def available(self):
        return _clang() is not None

    def _param(self, name, typ):
        return {"int": f"long long {name}", "float": f"double {name}", "bool": f"bool {name}",
                "str": f"const char *{name}",
                "list[int]": f"const long long *{name}, int {name}_len",
                "list[str]": f"const char **{name}, int {name}_len"}[typ]

    def signature(self, task):
        params = [self._param(n, t) for n, t in task.params]
        ret = {"int": "long long", "float": "double", "bool": "bool", "str": "char *",
               "list[int]": "long long *", "list[str]": "char **"}[task.ret]
        if task.ret.startswith("list"):
            params.append("int *out_len")
        sep = "" if ret.endswith("*") else " "
        return f"{ret}{sep}{task.name}({', '.join(params)})"

    def notes(self, task):
        notes = []
        if task.ret == "str":
            notes.append("The returned string must be heap-allocated with malloc; the caller frees it.")
        if task.ret.startswith("list"):
            notes.append("The returned array must be heap-allocated with malloc (the caller frees it) and "
                         "*out_len must receive its number of elements. For an empty result return any "
                         "pointer (even NULL) and set *out_len to 0.")
        if task.ret == "list[str]":
            notes.append("Every returned string must be heap-allocated too.")
        if any(t == "bool" for _, t in task.params) or task.ret == "bool":
            notes.append("bool comes from <stdbool.h>.")
        return " ".join(notes)

    def lit(self, value, typ):
        if typ == "bool":
            return "true" if value else "false"
        if typ == "str":
            return _str_lit(value)
        if typ == "float":
            return repr(float(value))
        if typ.startswith("list"):
            inner = typ[5:-1]
            return "{" + ", ".join(self.lit(v, inner) for v in value) + "}"
        return f"{value}LL" if abs(value) > 2147483647 else str(value)

    def call_text(self, task, args):
        parts = []
        for value, (_, typ) in zip(args, task.params):
            parts.append(self.lit(value, typ))
            if typ.startswith("list"):
                parts.append(str(len(value)))
        return f"{task.name}({', '.join(parts)})"

    def program(self, task, code):
        lines = [code, "", "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>",
                 "#include <stdbool.h>", "", "int main(void) {"]
        for k, (args, _) in enumerate(task.tests):
            lines.append("    {")
            call_args = []
            for j, (value, (_, typ)) in enumerate(zip(args, task.params)):
                var = f"a{j}"
                if typ == "list[int]":
                    body = self.lit(value, typ) if value else "{0}"
                    lines.append(f"        long long {var}[] = {body};")
                    call_args += [var, str(len(value))]
                elif typ == "list[str]":
                    body = self.lit(value, typ) if value else '{""}'
                    lines.append(f"        const char *{var}[] = {body};")
                    call_args += [var, str(len(value))]
                else:
                    call_args.append(self.lit(value, typ))
            if task.ret.startswith("list"):
                call_args.append("&n")
                elem = "long long" if task.ret == "list[int]" else "char *"
                fmt = "%lld" if task.ret == "list[int]" else "%s"
                lines += ["        int n = 0;",
                          f"        {elem} *r = {task.name}({', '.join(call_args)});",
                          "        printf(\"[\");",
                          f"        for (int i = 0; i < n; i++) printf(\"%s{fmt}\", i ? \"|\" : \"\", r[i]);",
                          "        printf(\"]\\n\");"]
            else:
                call = f"{task.name}({', '.join(call_args)})"
                if task.ret == "int":
                    lines.append(f"        printf(\"%lld\\n\", (long long){call});")
                elif task.ret == "float":
                    lines.append(f"        printf(\"%.17g\\n\", (double){call});")
                elif task.ret == "bool":
                    lines.append(f"        printf(\"%s\\n\", {call} ? \"true\" : \"false\");")
                else:
                    lines.append(f"        printf(\"%s\\n\", {call});")
            lines.append("        fflush(stdout);")
            lines.append("    }")
        lines += ["    return 0;", "}"]
        return {"main.c": "\n".join(lines) + "\n"}

    def commands(self, workdir, files):
        exe = workdir / ("prog" + EXE)
        return [_clang(), "-std=c17", "-O1", "-w", str(workdir / "main.c"), "-o", str(exe)], [str(exe)]


# ----------------------------------------------------------------------------- C++
class Cpp(Lang):
    name, title, ext, fence = "cpp", "C++20", ".cpp", "cpp"
    _ret = {"int": "long long", "float": "double", "bool": "bool", "str": "std::string",
            "list[int]": "std::vector<long long>", "list[str]": "std::vector<std::string>"}

    def available(self):
        return _clang(True) is not None

    def signature(self, task):
        params = []
        for n, t in task.params:
            base = self._ret[t]
            params.append(f"const {base}& {n}" if t in ("str", "list[int]", "list[str]") else f"{base} {n}")
        return f"{self._ret[task.ret]} {task.name}({', '.join(params)})"

    def lit(self, value, typ):
        if typ == "bool":
            return "true" if value else "false"
        if typ == "str":
            return f"std::string({_str_lit(value)})"
        if typ == "float":
            return repr(float(value))
        if typ.startswith("list"):
            inner = typ[5:-1]
            return self._ret[typ] + "{" + ", ".join(self.lit(v, inner) for v in value) + "}"
        return f"{value}LL"

    def call_text(self, task, args):
        def short(value, typ):
            if typ == "str":
                return _str_lit(value)
            if typ.startswith("list"):
                return "{" + ", ".join(short(v, typ[5:-1]) for v in value) + "}"
            if typ == "bool":
                return "true" if value else "false"
            return repr(value)
        return f"{task.name}({', '.join(short(v, t) for v, (_, t) in zip(args, task.params))})"

    def program(self, task, code):
        lines = [code, "", "#include <iostream>", "#include <iomanip>", "#include <string>", "#include <vector>",
                 "static void tz_show(long long v) { std::cout << v; }",
                 "static void tz_show(int v) { std::cout << v; }",
                 "static void tz_show(bool v) { std::cout << (v ? \"true\" : \"false\"); }",
                 "static void tz_show(double v) { std::cout << std::setprecision(17) << v; }",
                 "static void tz_show(const std::string& v) { std::cout << v; }",
                 "template <class T> static void tz_show(const std::vector<T>& v) {",
                 "    std::cout << '[';",
                 "    for (size_t i = 0; i < v.size(); i++) { if (i) std::cout << '|'; tz_show(v[i]); }",
                 "    std::cout << ']';",
                 "}",
                 "int main() {"]
        for args, _ in task.tests:
            call = f"{task.name}({', '.join(self.lit(v, t) for v, (_, t) in zip(args, task.params))})"
            lines.append(f"    try {{ tz_show({call}); }} catch (...) {{ std::cout << \"ERR\"; }}")
            lines.append("    std::cout << std::endl;")
        lines += ["    return 0;", "}"]
        return {"main.cpp": "\n".join(lines) + "\n"}

    def commands(self, workdir, files):
        exe = workdir / ("prog" + EXE)
        return [_clang(True), "-std=c++20", "-O1", "-w", str(workdir / "main.cpp"), "-o", str(exe)], [str(exe)]


# ----------------------------------------------------------------------------- Rust
class Rust(Lang):
    name, title, ext, fence = "rust", "Rust (2021 edition, std only)", ".rs", "rust"

    def available(self):
        return which("rustc") is not None

    def signature(self, task):
        p = {"int": "i64", "float": "f64", "bool": "bool", "str": "&str",
             "list[int]": "&[i64]", "list[str]": "&[String]"}
        r = {"int": "i64", "float": "f64", "bool": "bool", "str": "String",
             "list[int]": "Vec<i64>", "list[str]": "Vec<String>"}
        params = ", ".join(f"{n}: {p[t]}" for n, t in task.params)
        return f"fn {task.name}({params}) -> {r[task.ret]}"

    def lit(self, value, typ):
        if typ == "bool":
            return "true" if value else "false"
        if typ == "str":
            return _str_lit(value)
        if typ == "float":
            return repr(float(value))
        if typ == "list[int]":
            return "&[" + ", ".join(f"{v}i64" for v in value) + "]" if value else "&[]"
        if typ == "list[str]":
            return "&[" + ", ".join(f"String::from({_str_lit(v)})" for v in value) + "]" if value else "&[]"
        return str(value)

    def program(self, task, code):
        lines = [code, "",
                 "trait TzShow { fn tz(&self) -> String; }",
                 "impl TzShow for i64 { fn tz(&self) -> String { self.to_string() } }",
                 "impl TzShow for bool { fn tz(&self) -> String { self.to_string() } }",
                 "impl TzShow for f64 { fn tz(&self) -> String { format!(\"{:?}\", self) } }",
                 "impl TzShow for String { fn tz(&self) -> String { self.clone() } }",
                 "impl<T: TzShow> TzShow for Vec<T> {",
                 "    fn tz(&self) -> String { format!(\"[{}]\", self.iter().map(|x| x.tz()).collect::<Vec<_>>().join(\"|\")) }",
                 "}",
                 "fn main() {"]
        for args, _ in task.tests:
            call = f"{task.name}({', '.join(self.lit(v, t) for v, (_, t) in zip(args, task.params))})"
            lines.append(f"    match std::panic::catch_unwind(|| {call}) {{ Ok(v) => println!(\"{{}}\", v.tz()), Err(_) => println!(\"ERR\") }}")
        lines.append("}")
        return {"main.rs": "\n".join(lines) + "\n"}

    def commands(self, workdir, files):
        exe = workdir / ("prog" + EXE)
        return ["rustc", "--edition", "2021", "-O", "-A", "warnings", str(workdir / "main.rs"), "-o", str(exe)], [str(exe)]


# ----------------------------------------------------------------------------- Go
class Go(Lang):
    name, title, ext, fence = "go", "Go", ".go", "go"
    reply_rule = ("Reply with exactly one fenced code block containing a Go file that starts with "
                  "`package main`, imports only what it uses and defines the function plus any helpers. "
                  "Do not write func main, tests or prose.")
    _types = {"int": "int", "float": "float64", "bool": "bool", "str": "string",
              "list[int]": "[]int", "list[str]": "[]string"}

    def available(self):
        return which("go") is not None

    def fn(self, task):
        return camel(task.name)

    def signature(self, task):
        params = ", ".join(f"{camel(n)} {self._types[t]}" for n, t in task.params)
        return f"func {self.fn(task)}({params}) {self._types[task.ret]}"

    def lit(self, value, typ):
        if typ == "bool":
            return "true" if value else "false"
        if typ == "str":
            return _str_lit(value)
        if typ == "float":
            return repr(float(value))
        if typ.startswith("list"):
            inner = typ[5:-1]
            return self._types[typ] + "{" + ", ".join(self.lit(v, inner) for v in value) + "}"
        return str(value)

    def program(self, task, code):
        if "package main" not in code:
            code = "package main\n\n" + code
        lines = ["package main", "", "import (", "\t\"fmt\"", "\t\"strings\"", ")", "",
                 "func tzShow(v interface{}) string {",
                 "\tswitch x := v.(type) {",
                 "\tcase []int:",
                 "\t\tparts := make([]string, len(x))",
                 "\t\tfor i, e := range x {",
                 "\t\t\tparts[i] = fmt.Sprint(e)",
                 "\t\t}",
                 "\t\treturn \"[\" + strings.Join(parts, \"|\") + \"]\"",
                 "\tcase []string:",
                 "\t\treturn \"[\" + strings.Join(x, \"|\") + \"]\"",
                 "\tcase float64:",
                 "\t\treturn fmt.Sprintf(\"%v\", x)",
                 "\tdefault:",
                 "\t\treturn fmt.Sprint(x)",
                 "\t}",
                 "}", "",
                 "func tzRun(f func() interface{}) {",
                 "\tdefer func() {",
                 "\t\tif r := recover(); r != nil {",
                 "\t\t\tfmt.Println(\"ERR\")",
                 "\t\t}",
                 "\t}()",
                 "\tfmt.Println(tzShow(f()))",
                 "}", "",
                 "func main() {"]
        for args, _ in task.tests:
            call = f"{self.fn(task)}({', '.join(self.lit(v, t) for v, (_, t) in zip(args, task.params))})"
            lines.append(f"\ttzRun(func() interface{{}} {{ return {call} }})")
        lines.append("}")
        return {"solution.go": code + "\n", "driver.go": "\n".join(lines) + "\n"}

    def commands(self, workdir, files):
        exe = workdir / ("prog" + EXE)
        return ["go", "build", "-o", str(exe), str(workdir / "solution.go"), str(workdir / "driver.go")], [str(exe)]


def all_langs(compiler=None):
    return [Turi(compiler), TuriPy(compiler), Python(), C(), Cpp(), Rust(), Go()]


def run_candidate(lang, task, code, workdir, timeout=20):
    """Build and run one candidate. Returns a dict with stage, passed list and a short diagnostic."""
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    files = lang.program(task, code)
    for name, text in files.items():
        (workdir / name).write_text(text, encoding="utf-8", newline="\n")
    build, run = lang.commands(workdir, files)
    env = dict(os.environ)
    env.setdefault("GOFLAGS", "-mod=mod")
    env["GO111MODULE"] = "off"
    result = {"compiled": False, "passed": [], "public_ok": False, "all_ok": False, "stage": "build", "detail": ""}
    if build:
        try:
            proc = subprocess.run(build, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                  timeout=120, cwd=workdir, env=env)
        except subprocess.TimeoutExpired:
            result["detail"] = "build timeout"
            return result
        if proc.returncode != 0:
            # Keep diagnostics free of the local temp path (it is noise for the model and for the log).
            text = (proc.stderr or proc.stdout).strip()
            for prefix in (str(workdir) + os.sep, str(workdir).replace("\\", "\\\\") + "\\\\", str(workdir)):
                text = text.replace(prefix, "")
            result["detail"] = text[:1500]
            return result
    result["compiled"] = True
    result["stage"] = "run"
    try:
        proc = subprocess.run(run, capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=timeout, cwd=workdir, env=env)
    except subprocess.TimeoutExpired:
        result["detail"] = "run timeout"
        return result
    out = proc.stdout.replace("\r\n", "\n").split("\n")
    if out and out[-1] == "":
        out.pop()
    passed = []
    for i, (_, expected) in enumerate(task.tests):
        actual = out[i] if i < len(out) else "<no output>"
        passed.append(same(task.ret, canon(expected), actual))
    result["passed"] = passed
    from tasks import PUBLIC
    result["public_ok"] = all(passed[:PUBLIC])
    result["all_ok"] = all(passed)
    if lang.name == "python" and proc.returncode != 0 and not out:
        result["compiled"] = False
        result["stage"] = "build"
        result["detail"] = proc.stderr.strip()[-1500:]
        return result
    if not result["all_ok"]:
        first = next(i for i, ok in enumerate(passed) if not ok)
        args, expected = task.tests[first]
        shown = out[first] if first < len(out) else "<no output>"
        if first < PUBLIC:
            result["detail"] = (f"Example {lang.call_text(task, args)} returned {shown!r}, expected {canon(expected)!r}."
                                + (f"\n{proc.stderr.strip()[:600]}" if proc.stderr.strip() else ""))
        else:
            result["detail"] = "A hidden test failed." + (f"\n{proc.stderr.strip()[:600]}" if proc.stderr.strip() else "")
        result["stage"] = "test"
    else:
        result["stage"] = "ok"
    return result
