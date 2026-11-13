#!/usr/bin/env python3
"""Retarget imported guest calls in retained bitcode for a private build.

Normal CMake builds regenerate direct_calls.py headers. This tool preserves
qualified generated modules when only new strong hooks need different callees.
It refuses definitions, preserves every function body except the requested
symbol references, rebuilds the ThinLTO summary and verifies the resulting IR.
It never modifies an input or overwrites an existing output.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def without_summary(text):
    return "\n".join(line for line in text.splitlines()
                     if not line.startswith("^") and not line.startswith("; ModuleID =")) + "\n"


def bodies(text):
    attributes = dict(re.findall(r"^attributes #(\d+) = (.+)$", text, re.M))
    result = {}
    for match in re.finditer(r"^define [^\n]*@([^ (]+)\([^\n]*\{\n.*?^\}", text, re.M | re.S):
        body = re.sub(r"#(\d+)\b", lambda m: attributes[m[1]], match[0])
        if match[1] in result:
            raise RuntimeError("Duplicate function body")
        result[match[1]] = body
    if not result:
        raise RuntimeError("Input has no function bodies")
    return result


def rebind(source, output, targets, llvm):
    source, output = source.resolve(), output.resolve()
    if source == output or output.exists():
        raise RuntimeError("An output must be new and separate from its input")
    if not targets or any(not re.fullmatch(r"sub_[0-9A-F]{8}", name) for name in targets):
        raise RuntimeError("Expected explicit guest function names")
    if source.read_bytes()[:4] != b"BC\xc0\xde":
        raise RuntimeError("Only LLVM bitcode modules are supported")
    output.parent.mkdir(parents=True, exist_ok=True)
    original_hash = sha256(source)
    before = subprocess.check_output([str(llvm / "llvm-dis"), str(source), "-o", "-"], text=True)
    expected = without_summary(before)
    imports = set(subprocess.check_output(
        [str(llvm / "llvm-nm"), "--undefined-only", "--just-symbol-name", str(source)], text=True).split())
    references = {}
    for public in sorted(set(targets)):
        original = "__imp__" + public
        if original not in imports or re.search(r"^define [^\n]*@" + original + r"\(", expected, re.M):
            raise RuntimeError("Target must be an imported implementation: " + original)
        pattern = r"@" + original + r"\b"
        references[public] = len(re.findall(pattern, expected))
        # If both declarations exist, retain the implementation's otherwise
        # identical prototype when it is renamed. Definitions are never removed.
        expected = re.sub(r"^declare [^\n]*@" + public + r"\([^\n]*\n", "", expected, flags=re.M)
        expected = re.sub(pattern, "@" + public, expected)
    expected_bodies = bodies(expected)
    with tempfile.TemporaryDirectory(prefix="guest-rebind-", dir=output.parent) as temporary:
        ram = Path(temporary)
        text, assembled, summarized = ram / "calls.ll", ram / "calls.bc", ram / "summary.bc"
        text.write_text(expected)
        subprocess.run([str(llvm / "llvm-as"), str(text), "-o", str(assembled)], check=True,
                       capture_output=True, text=True)
        subprocess.run([str(llvm / "opt"), "--verify-each", "--module-summary", str(assembled),
                        "-o", str(summarized)], check=True, capture_output=True, text=True)
        after = subprocess.check_output([str(llvm / "llvm-dis"), str(summarized), "-o", "-"], text=True)
        actual_bodies = bodies(after)
        if expected_bodies != actual_bodies:
            different = sorted(name for name in set(expected_bodies) | set(actual_bodies)
                               if expected_bodies.get(name) != actual_bodies.get(name))
            raise RuntimeError("Function bodies changed beyond their callees: " + ", ".join(different[:8]))
        remaining = set(subprocess.check_output(
            [str(llvm / "llvm-nm"), "--undefined-only", "--just-symbol-name", str(summarized)], text=True).split())
        if any("__imp__" + name in remaining or name not in remaining for name in targets):
            raise RuntimeError("Rebound import names do not match the requested hooks")
        if sha256(source) != original_hash:
            raise RuntimeError("The input changed during rebinding")
        summarized.rename(output)
    return dict(passed=True, source=str(source), source_sha256=original_hash,
                output=str(output), sha256=sha256(output), targets=sorted(set(targets)),
                symbol_references=references, preserved_function_bodies=len(expected_bodies),
                thinlto_summary_rebuilt=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--target", required=True, action="append")
    parser.add_argument("--llvm-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "clang20/bin")
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    report = rebind(args.source, args.output, args.target, args.llvm_dir.resolve())
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report[key] for key in ("passed", "preserved_function_bodies", "targets")}))


if __name__ == "__main__":
    main()
