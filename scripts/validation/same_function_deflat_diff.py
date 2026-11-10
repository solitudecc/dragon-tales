#!/usr/bin/env python3
"""Execute the actual same-function deflatten pass and native scalar oracles.

This builds a small IR producer against an existing libdragon; it never builds
or changes the repository. The producer emits before/after IR for PHI payload,
load-backed PHI payload, and PHI/load mixed known/unknown state dispatchers.
Both modules run at native O0 and O2 with a separately compiled C oracle.
Every normal product also gets one specified, single-result-bit negative
control. There are no skips, exclusions, result normalizations, or emulation.

Example (Ubuntu VM):
  python3 scripts/validation/same_function_deflat_diff.py \
    --repo ~/dragon-tales-overlap \
    --library ~/dragon-tales-overlap/build/lib/libdragon.so \
    --work /tmp/same-function-deflat-current
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


VALUES = (0, 1, 2, 3, 10, 20, (1 << 63) - 1, 1 << 63, (1 << 64) - 1)
PHI_NAMES = ("phi_payload", "load_phi_payload")
MIXED_NAMES = ("mixed_state", "load_mixed_state")
CONTROL_KEY = ("phi_payload", 0, None)
PHI_ROW = re.compile(
    r"(phi_payload|load_phi_payload) left=([01]) actual=(-?\d+) "
    r"expected=(-?\d+) match=([01])")
MIXED_ROW = re.compile(
    r"(mixed_state|load_mixed_state) known=([01]) unknown=(\d+) "
    r"actual=(\d+) expected=(\d+) match=([01])")
SUMMARY = re.compile(r"comparisons=(\d+) mismatches=(\d+) skips=0 control=([01])")
PASS_STATS = re.compile(
    r"changed=([01]) dispatchers=(\d+) rewritten=(\d+) edges=(\d+) "
    r"unresolved=(\d+) verify_invalid=([01])\n?")


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            value.update(block)
    return value.hexdigest()


def expected_rows() -> dict[tuple[str, int, int | None], int]:
    rows = {(name, left, None): 10 if left else 20
            for name in PHI_NAMES for left in (0, 1)}
    for name in MIXED_NAMES:
        for known in (0, 1):
            for unknown in VALUES:
                state = 1 if known else unknown
                rows[(name, known, unknown)] = 11 if state == 1 else (22 if state == 2 else 33)
    assert len(rows) == 40
    return rows


def parse_native(output: str, returncode: int, control: bool) -> dict:
    expected = expected_rows()
    actual: dict[tuple[str, int, int | None], int] = {}
    summary = None
    mismatches = []
    for line in output.splitlines():
        match = PHI_ROW.fullmatch(line)
        if match:
            name, branch, value, stated_expected, stated_match = match.groups()
            key = (name, int(branch), None)
        else:
            match = MIXED_ROW.fullmatch(line)
            if match:
                name, branch, unknown, value, stated_expected, stated_match = match.groups()
                key = (name, int(branch), int(unknown))
            else:
                match = SUMMARY.fullmatch(line)
                if not match or summary is not None:
                    raise ValueError(f"unexpected or duplicate native output: {line!r}")
                summary = tuple(map(int, match.groups()))
                continue
        if summary is not None or key in actual or key not in expected:
            raise ValueError(f"unexpected/duplicate/out-of-order native row: {key}")
        value = int(value)
        if int(stated_expected) != expected[key]:
            raise ValueError(f"driver oracle disagrees with independent expected value: {key}")
        matches = value == expected[key]
        if int(stated_match) != int(matches):
            raise ValueError(f"driver match flag disagrees with actual value: {key}")
        actual[key] = value
        if not matches:
            mismatches.append({"key": list(key), "actual": value, "expected": expected[key]})
    if set(actual) != set(expected):
        raise ValueError(f"native coverage mismatch: missing={set(expected) - set(actual)}")
    if summary != (len(expected), len(mismatches), int(control)):
        raise ValueError(f"native summary mismatch: {summary}")
    if returncode != int(bool(mismatches)):
        raise ValueError(f"native exit code mismatch: rc={returncode} differences={len(mismatches)}")
    return {"comparisons": len(actual), "mismatches": mismatches, "skips": 0,
            "actual": actual, "returncode": returncode}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--clang", default="clang-21")
    parser.add_argument("--llvm-config", default="llvm-config-21")
    args = parser.parse_args()
    repo, library = args.repo.expanduser().resolve(), args.library.expanduser().resolve()
    work = args.work.expanduser().absolute()
    if work.exists():
        parser.error("--work must be a new path; existing output is never overwritten")
    if not (repo / "include/dragon/lifters/llvm/LLVMDeflattening.hpp").is_file():
        parser.error("--repo does not contain the deflatten API header")
    if not library.is_file():
        parser.error("--library is not a file")
    work.mkdir(parents=True)
    report = {"repo": str(repo), "library": str(library), "work": str(work),
              "normal_comparisons": 0, "normal_mismatches": 0,
              "control_comparisons": 0, "control_mismatches": 0,
              "specified_controls": 0, "controls_detected": 0,
              "skips": 0, "exclusions": 0, "normalizations": 0,
              "runs": [], "errors": []}
    command_index = 0
    run_env = os.environ.copy()
    run_env["LD_LIBRARY_PATH"] = str(library.parent)
    run_env["DYLD_LIBRARY_PATH"] = str(library.parent)

    def run(label: str, command: list[str], *, check: bool = True) -> subprocess.CompletedProcess:
        nonlocal command_index
        command_index += 1
        prefix = work / f"{command_index:02d}-{label}"
        started = time.monotonic()
        try:
            completed = subprocess.run(command, env=run_env, cwd=work,
                                       text=True, capture_output=True, timeout=180)
        except subprocess.TimeoutExpired as error:
            def raw(value):
                return value if isinstance(value, bytes) else (value or "").encode()
            prefix.with_suffix(".stdout").write_bytes(raw(error.stdout))
            prefix.with_suffix(".stderr").write_bytes(raw(error.stderr))
            with (work / "commands.jsonl").open("a") as stream:
                stream.write(json.dumps({"label": label, "argv": command,
                                         "returncode": None, "timeout": True,
                                         "elapsed_seconds": time.monotonic() - started,
                                         "stdout": prefix.with_suffix(".stdout").name,
                                         "stderr": prefix.with_suffix(".stderr").name},
                                        sort_keys=True) + "\n")
            raise RuntimeError(f"{label} timed out; retained partial stdout/stderr") from error
        prefix.with_suffix(".stdout").write_text(completed.stdout)
        prefix.with_suffix(".stderr").write_text(completed.stderr)
        metadata = {"label": label, "argv": command, "returncode": completed.returncode,
                    "elapsed_seconds": time.monotonic() - started,
                    "stdout": prefix.with_suffix(".stdout").name,
                    "stderr": prefix.with_suffix(".stderr").name}
        with (work / "commands.jsonl").open("a") as stream:
            stream.write(json.dumps(metadata, sort_keys=True) + "\n")
        if check and completed.returncode:
            raise RuntimeError(f"{label} failed rc={completed.returncode}; see {metadata['stderr']}")
        return completed

    status = 2
    try:
        library_hash = digest(library)
        report["library_sha256"] = library_hash
        sources = work / "sources"
        sources.mkdir()
        here = Path(__file__).resolve().parent
        source_paths = []
        for name in (Path(__file__).name, "same_function_deflat_native.cpp",
                     "same_function_deflat_driver.c"):
            origin = here / name
            destination = sources / name
            shutil.copyfile(origin, destination)
            source_paths.append(destination)
        report["sources"] = {str(path.relative_to(work)): digest(path) for path in source_paths}
        report["repo_head"] = run("repo-head", ["git", "-C", str(repo), "rev-parse", "HEAD"]).stdout.strip()
        report["repo_status"] = run("repo-status", ["git", "-C", str(repo), "status", "--short"]).stdout
        report["source_pins"] = {}
        for name in ("lib/LLVMDeflattening.cpp", "lib/CallEffectSummary.cpp",
                     "include/dragon/lifters/llvm/LLVMDeflattening.hpp",
                     "include/dragon/lifters/llvm/CallEffectSummary.hpp"):
            report["source_pins"][name] = digest(repo / name)
        report["inputs"] = {"phi_left": [0, 1], "mixed_known": [0, 1],
                            "mixed_unknown": list(VALUES), "fixtures": list(PHI_NAMES + MIXED_NAMES)}
        report["inputs_sha256"] = hashlib.sha256(
            json.dumps(report["inputs"], sort_keys=True).encode()).hexdigest()
        for label, executable in (("cxx-version", args.cxx), ("clang-version", args.clang)):
            run(label, [executable, "--version"])
        includedir = run("llvm-includedir", [args.llvm_config, "--includedir"]).stdout.strip()
        generator = work / "generator"
        run("compile-generator", [args.cxx, "-std=c++20", "-O0", "-I" + str(repo / "include"),
                                  "-I" + includedir, str(sources / "same_function_deflat_native.cpp"),
                                  str(library), "-Wl,-rpath," + str(library.parent), "-o", str(generator)])
        if digest(library) != library_hash:
            raise RuntimeError("library changed while the generator was being compiled")
        generated = run("actual-pass", [str(generator), str(work)])
        match = PASS_STATS.fullmatch(generated.stdout)
        if not match or int(match.group(6)):
            raise ValueError("actual pass output/IR verification failed")
        report["pass_stats"] = dict(zip(
            ("changed", "dispatchers", "rewritten", "edges", "unresolved", "verify_invalid"),
            map(int, match.groups())))
        if digest(library) != library_hash:
            raise RuntimeError("library changed while the actual pass was running")
        report["ir_sha256"] = {stage: digest(work / f"{stage}.ll") for stage in ("before", "after")}
        for optimization in ("O0", "O2"):
            for stage in ("before", "after"):
                label = f"{stage}-{optimization}"
                executable = work / label
                run("compile-" + label, [args.clang, "-" + optimization, "-Wno-override-module",
                                         str(work / f"{stage}.ll"),
                                         str(sources / "same_function_deflat_driver.c"),
                                         "-o", str(executable)])
                normal_result = run(label, [str(executable)], check=False)
                normal = parse_native(normal_result.stdout, normal_result.returncode, False)
                control_result = run(label + "-control", [str(executable), "control"], check=False)
                control = parse_native(control_result.stdout, control_result.returncode, True)
                differences = [key for key, value in normal["actual"].items()
                               if value != control["actual"][key]]
                control_valid = (differences == [CONTROL_KEY] and
                                 control["actual"][CONTROL_KEY] == (normal["actual"][CONTROL_KEY] ^ 1) and
                                 any(tuple(row["key"]) == CONTROL_KEY for row in control["mismatches"]))
                report["normal_comparisons"] += normal["comparisons"]
                report["normal_mismatches"] += len(normal["mismatches"])
                report["control_comparisons"] += control["comparisons"]
                report["control_mismatches"] += len(control["mismatches"])
                report["specified_controls"] += 1
                report["controls_detected"] += int(control_valid)
                report["runs"].append({"stage": stage, "optimization": optimization,
                                       "normal_comparisons": normal["comparisons"],
                                       "normal_mismatches": normal["mismatches"],
                                       "control_comparisons": control["comparisons"],
                                       "control_mismatches": control["mismatches"],
                                       "specified_control_detected": control_valid,
                                       "normal_returncode": normal_result.returncode,
                                       "control_returncode": control_result.returncode})
        report["complete"] = (report["normal_comparisons"] == 160 and
                              report["control_comparisons"] == 160 and
                              report["specified_controls"] == report["controls_detected"] == 4)
        status = int(not report["complete"] or bool(report["normal_mismatches"]))
    except Exception as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
    report["returncode"] = status
    report["artifact_sha256"] = {str(path.relative_to(work)): digest(path)
                                 for path in sorted(work.rglob("*")) if path.is_file()}
    (work / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({key: report[key] for key in (
        "normal_comparisons", "normal_mismatches", "control_comparisons", "control_mismatches",
        "specified_controls", "controls_detected", "skips", "errors", "returncode")}, sort_keys=True))
    print(work / "report.json")
    return status


if __name__ == "__main__":
    sys.exit(main())
