"""Valida paquetes locales, módulos e interfaces con ejecución interpretada y nativa."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "projects" / "multi"
CLI = Path(sys.argv[1]).resolve()
EXPECTED = b"12\n1\n3\n9\n2\n"


def invoke(entry, command="check", *args, timeout=30):
    return subprocess.run([CLI, command, entry, *args], capture_output=True, timeout=timeout)


def ok(entry, command="check", *args, timeout=30):
    result = invoke(entry, command, *args, timeout=timeout)
    assert result.returncode == 0, (command, entry, result.stdout, result.stderr)
    return result.stdout


def error(entry, expected, *args):
    result = invoke(entry, *args)
    assert result.returncode != 0, (args, result.stdout)
    diagnostic = json.loads(result.stderr)
    assert diagnostic["id"] == expected, (expected, diagnostic)
    return diagnostic


def replace(root, relative, old, new, count=1):
    path = root / relative
    text = path.read_text(encoding="utf-8")
    assert text.count(old) >= count, (relative, old)
    path.write_text(text.replace(old, new, count), encoding="utf-8")


def entry(root):
    return root / "app" / "src" / "app.turi"


def main():
    assert ok(entry(FIXTURE)) == b""
    assert ok(entry(FIXTURE), "run") == EXPECTED
    signatures = [json.loads(line) for line in ok(entry(FIXTURE), "sig").splitlines()]
    score = next(item for item in signatures if item["name"] == "stats.score")
    assert "T: contracts.Scored" in score["signature"] and score["hash"]
    interface = json.loads(ok(entry(FIXTURE), "sig", "contracts.Scored"))
    assert interface["signature"].startswith("interface Scored")
    helper_hash = next(item for item in signatures if item["name"] == "app.helper")["hash"]
    typed = [json.loads(line) for line in ok(entry(FIXTURE), "types", "stateValue").splitlines()]
    assert typed and all(item["file"].endswith("app.turi") for item in typed)
    costs = [json.loads(line) for line in ok(entry(FIXTURE), "costes").splitlines()]
    clone = next(item for item in costs if item["kind"] == "clone")
    assert clone["file"].endswith("stats.turi") and clone["level"] == 3
    ir = ok(entry(FIXTURE), "emit-ir", "--audit-copies").decode()
    assert "turi.app.main" in ir and "@impl." in ir and "@tz.clone" in ir

    with tempfile.TemporaryDirectory(prefix="turi-project-") as temporary:
        temporary = Path(temporary)

        def project(name):
            destination = temporary / name
            shutil.copytree(FIXTURE, destination)
            return destination

        positive = project("positive")
        executable = temporary / ("multi.exe" if os.name == "nt" else "multi")
        ok(entry(positive), "build", "-o", executable, timeout=60)
        result = subprocess.run([executable], capture_output=True, timeout=20)
        assert result.returncode == 0 and result.stdout == EXPECTED, result.stderr

        patched = project("patch")
        replacement = temporary / "helper.turi"
        replacement.write_text("fn helper(value: int) -> int { return value + 2 }", encoding="utf-8")
        ok(entry(patched), "patch", "app.helper", helper_hash, replacement)
        assert ok(entry(patched), "run").splitlines()[-1] == b"3"
        before = entry(patched).read_bytes()
        diagnostic = error(entry(patched), "E_STALE", "patch", "app.helper", helper_hash, replacement)
        assert diagnostic["file"].endswith("app.turi") and entry(patched).read_bytes() == before
        replacement.write_text("fn helper(value: int) -> int { return missing(value) }", encoding="utf-8")
        current = next(item for item in [json.loads(line) for line in ok(entry(patched), "sig").splitlines()]
                       if item["name"] == "app.helper")["hash"]
        error(entry(patched), "E_FUNCTION", "patch", "app.helper", current, replacement)
        assert entry(patched).read_bytes() == before

        fixed = project("fix")
        replace(fixed, "app/src/app.turi",
                "fn helper(value: int) -> int {\n    return value + 1\n}",
                "fn helper(value: int) -> int { value += 1; return value }")
        output = ok(entry(fixed), "fix").decode()
        assert "fn helper(var value: int)" in output and ";" not in output
        assert ";" in entry(fixed).read_text(encoding="utf-8")  # fix no publica.
        entry(fixed).write_bytes(output.encode("utf-8"))
        assert ok(entry(fixed)) == b""

        replacement.write_text("fn helper(", encoding="utf-8")
        diagnostic = error(entry(patched), "E_PARSE", "patch", "app.helper", current, replacement)
        assert Path(diagnostic["file"]).resolve() == replacement.resolve()
        assert diagnostic["span"]["start"] == len("fn helper(")
        assert entry(patched).read_bytes() == before

        broken_dependency = project("fix-dependency")
        dependency_source = broken_dependency / "contracts" / "src" / "contracts.turi"
        dependency_source.write_text(dependency_source.read_text(encoding="utf-8") +
            "\nfn broken() { let mut n = 1 }\n", encoding="utf-8")
        before = entry(broken_dependency).read_bytes()
        result = invoke(entry(broken_dependency), "fix")
        diagnostic = json.loads(result.stderr)
        assert result.returncode != 0 and result.stdout == b""
        assert diagnostic["id"] == "E_LET_MUT" and diagnostic["file"].endswith("contracts.turi")
        assert entry(broken_dependency).read_bytes() == before

        visibility = project("template-visibility")
        replace(visibility, "stats/src/stats.turi", "fn copy",
            "fn bad<T>(value: T, item: model.Item) -> T { return value }\n\nfn copy")
        diagnostic = error(entry(visibility), "E_IMPORT")
        assert diagnostic["file"].endswith("stats.turi") and "model.Item" in diagnostic["message"]

        generic_main = project("generic-main")
        replace(generic_main, "app/src/app.turi", "fn main() {", "fn main<T>(value: T) {")
        error(entry(generic_main), "E_MAIN")

        empty = project("empty-interface")
        empty_contracts = empty / "contracts" / "src" / "contracts.turi"
        empty_contracts.write_text(empty_contracts.read_text(encoding="utf-8") +
            "\ninterface Empty {}\n", encoding="utf-8")
        replace(empty, "app/src/app.turi", "fn helper",
            "impl contracts.Empty for int { fn extra(value: int) -> int { return value } }\n\nfn helper")
        error(entry(empty), "E_IMPL_METHOD_EXTRA")

        effectful = project("effects")
        replace(effectful, "app/src/app.turi",
            "fn scoreOf(value: int) -> int {\n        return value\n    }",
            "fn scoreOf(value: int) -> int {\n        print(value)\n        return value\n    }")
        effect_contracts = effectful / "contracts" / "src" / "contracts.turi"
        effect_contracts.write_text(effect_contracts.read_text(encoding="utf-8") +
            "\ninterface Pure { fn pure(value: Self) -> int }\n", encoding="utf-8")
        replace(effectful, "app/src/app.turi", "fn helper",
            "impl contracts.Pure for int { fn pure(value: int) -> int { return value } }\n\nfn helper")
        replace(effectful, "stats/src/stats.turi", "fn copy",
            "fn pureScore<T: contracts.Pure>(value: T) -> int { return pure(value) }\n\nfn copy")
        assert ok(entry(effectful), "run") == b"7\n" + EXPECTED
        effect_signatures = [json.loads(line) for line in ok(entry(effectful), "sig").splitlines()]
        effect_by_name = {item["name"]: item for item in effect_signatures}
        assert effect_by_name["stats.score"]["io"] is True
        assert effect_by_name["app.main"]["io"] is True
        assert effect_by_name["stats.pureScore"]["io"] is False
        assert effect_by_name["stats.copy"]["io"] is False

        invalid = project("manifest")
        replace(invalid, "app/veloz.pkg", "version 1.0.0", "version 1.0")
        diagnostic = error(entry(invalid), "E_MANIFEST")
        assert Path(diagnostic["file"]).name == "veloz.pkg"

        missing = project("missing-path")
        replace(missing, "app/veloz.pkg", '../stats"', '../missing"')
        error(entry(missing), "E_PACKAGE_PATH")

        wrong = project("wrong-version")
        replace(wrong, "app/veloz.pkg", "require contracts 1.1.0", "require contracts 1.2.0")
        diagnostic = error(entry(wrong), "E_PACKAGE_VERSION")
        assert diagnostic["expected"] == "contracts 1.2.0"
        assert diagnostic["actual"] == "contracts 1.1.0"

        conflict = project("conflict")
        shutil.copytree(conflict / "contracts", conflict / "contracts-v2")
        replace(conflict, "contracts-v2/veloz.pkg", "version 1.1.0", "version 2.0.0")
        replace(conflict, "stats/veloz.pkg", 'require contracts 1.1.0 "../contracts"',
                'require contracts 2.0.0 "../contracts-v2"')
        error(entry(conflict), "E_PACKAGE_CONFLICT")

        cycle = project("cycle")
        (cycle / "app" / "src" / "cycle.turi").write_text("import app\n", encoding="utf-8")
        replace(cycle, "app/src/app.turi", "import model", "import cycle\nimport model")
        error(entry(cycle), "E_IMPORT_CYCLE")

        unknown = project("unknown")
        replace(unknown, "app/src/app.turi", "import model", "import missing\nimport model")
        error(entry(unknown), "E_IMPORT")

        transitive = project("transitive")
        replace(transitive, "app/veloz.pkg", 'require contracts 1.1.0 "../contracts"\n', "")
        error(entry(transitive), "E_IMPORT_TRANSITIVE")

        duplicate = project("duplicate-module")
        (duplicate / "stats" / "src" / "model.turi").write_text("fn duplicate() {}\n", encoding="utf-8")
        error(entry(duplicate), "E_DUP_MODULE")

        duplicate_function = project("duplicate-function")
        replace(duplicate_function, "app/src/app.turi", "fn stateValue",
                "fn helper() {}\n\nfn stateValue")
        error(entry(duplicate_function), "E_DUP_FUNCTION")

        int_impl = """impl contracts.Scored for int {
    fn scoreOf(value: int) -> int {
        return value
    }
}"""
        absent = project("missing-impl")
        replace(absent, "app/src/app.turi", int_impl + "\n\n", "")
        error(entry(absent), "E_IMPL_MISSING")

        duplicated = project("duplicate-impl")
        replace(duplicated, "app/src/app.turi", "fn helper", int_impl + "\n\nfn helper")
        error(entry(duplicated), "E_DUP_IMPL")

        signature = project("signature")
        replace(signature, "app/src/app.turi", "fn scoreOf(value: int) -> int",
                "fn scoreOf(value: bool) -> int")
        error(entry(signature), "E_IMPL_SIGNATURE")

        method = project("missing-method")
        replace(method, "app/src/app.turi", int_impl,
                "impl contracts.Scored for int {\n}")
        error(entry(method), "E_IMPL_METHOD_MISSING")

        extra = project("extra-method")
        replace(extra, "app/src/app.turi", "        return value\n    }\n}\n\nimpl contracts.Scored for model.Item",
                "        return value\n    }\n    fn extra(value: int) -> int { return value }\n}\n\nimpl contracts.Scored for model.Item")
        error(entry(extra), "E_IMPL_METHOD_EXTRA")

        opaque = project("opaque")
        replace(opaque, "stats/src/stats.turi", "fn copy",
                "fn unused<T: contracts.Scored>(value: T) -> int { return missing(value) }\n\nfn copy")
        diagnostic = error(entry(opaque), "E_FUNCTION")
        assert diagnostic["file"].endswith("stats.turi")

        dependency = project("dependency-span")
        replace(dependency, "stats/src/stats.turi", "fn copy(values: [int]) -> [int] {\n    return values\n}",
                "fn copy(values: [int]) -> [int] {\n    return 1\n}")
        diagnostic = error(entry(dependency), "E_TYPE")
        source = (dependency / "stats" / "src" / "stats.turi").read_bytes()
        assert diagnostic["file"].endswith("stats.turi")
        assert source[diagnostic["span"]["start"]:diagnostic["span"]["end"]] == b"1"

        trapped = project("native-trap")
        replace(trapped, "stats/src/stats.turi", "fn main() -> int {\n    return 99\n}",
                "fn explode() -> int { return 1 / 0 }\n\nfn main() -> int {\n    return 99\n}")
        replace(trapped, "app/src/app.turi", "print(helper(1))", "print(stats.explode())")
        trap_exe = temporary / ("trap.exe" if os.name == "nt" else "trap")
        ok(entry(trapped), "build", "-o", trap_exe, timeout=60)
        result = subprocess.run([trap_exe], capture_output=True, timeout=20)
        diagnostic = json.loads(result.stderr)
        reference = json.loads(invoke(entry(trapped), "run").stderr)
        span = reference["span"]
        expected = json.dumps({
            "id": "R_DIV_ZERO", "file": reference["file"],
            "start": span["start"], "end": span["end"],
        }, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
        assert result.returncode == 1 and result.stderr == expected, (result.stderr, expected)
        assert diagnostic["file"].endswith("stats.turi")

    print("Paquetes, módulos, interfaces, edición y nativo verificados.")


if __name__ == "__main__":
    main()
