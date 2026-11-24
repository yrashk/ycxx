"""Replay the normal cold CTest command with the two #1230 registrations."""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
SOURCE = ROOT / "tests/daemon/CMakeLists.txt"
GENERATED = BUILD / "tests/daemon/CTestTestfile.cmake"
TEMP = BUILD / "Testing/Temporary"
COMPARE = "daemon.compare"
COST = 400
ANNOTATION = (
    "# Overlap the comparison with other tests even before CTest has timing history (#1230).\n"
    f"set_tests_properties({COMPARE} PROPERTIES COST {COST})\n"
)
START = re.compile(r"\s*Start\s+\d+:\s+(\S+)\s*")
FINISH = re.compile(r"\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)\s+\.+\s+(.+?)\s+(\d+\.\d+) sec\s*")
TOTAL = re.compile(r"Total Test time \(real\)\s*=\s*(\d+\.\d+) sec\s*")
COMPARE_PASS = re.compile(
    r"^daemon\.compare: (\d+) tests x 4 actions, two directories, edits, fallback and a killed daemon: "
    r"identical, from one daemon until it was killed$",
    re.MULTILINE,
)


def output(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def inventory():
    data = json.loads(output("ctest", "--test-dir", BUILD, "--show-only=json-v1"))
    return {
        entry["name"]: (
            entry["command"],
            {prop["name"]: prop["value"] for prop in entry.get("properties", [])},
        )
        for entry in data["tests"]
    }


def registrations():
    baseline = SOURCE.read_text()
    lines = [line for line in baseline.splitlines(keepends=True)
             if line.startswith(f"add_test(NAME {COMPARE} ")]
    if len(lines) != 1 or ANNOTATION in baseline:
        raise RuntimeError("Expected the exact baseline registration for #1230")
    a = GENERATED.read_bytes()
    before = inventory()
    try:
        SOURCE.write_text(baseline.replace(lines[0], lines[0] + ANNOTATION, 1))
        subprocess.run(["cmake", "-S", ROOT, "-B", BUILD], cwd=ROOT, check=True)
        b = GENERATED.read_bytes()
        after = inventory()
    finally:
        SOURCE.write_text(baseline)
        GENERATED.write_bytes(a)
    if ("COST" in before[COMPARE][1] or after[COMPARE][1].pop("COST", None) != COST
            or list(before) != list(after) or before != after):
        raise RuntimeError("CTest inventories must differ only in daemon.compare COST")
    if any(not re.fullmatch(r"[A-Za-z0-9_.+-]+", name) for name in before):
        raise RuntimeError("Unexpected test name format in the CTest inventory")
    return {"A": a, "B": b}, before


def resources():
    samples = {"load_average": os.getloadavg()}
    if platform.system() == "Darwin":
        samples.update({
            name.lower(): int(value)
            for name, value in re.findall(r"^(Pageouts|Swapins|Swapouts):\s+(\d+)\.",
                                          output("vm_stat"), re.MULTILINE)
        })
    return samples


def trial(index, arm, expected, reports):
    label = f"{index}-{arm}"
    print(f"Starting cold CTest {label}", flush=True)
    (TEMP / "CTestCostData.txt").unlink(missing_ok=True)
    last_log = TEMP / "LastTest.log"
    last_log.unlink(missing_ok=True)
    before = resources()
    start = {}
    finish = {}
    total = None
    peak = 0
    zero = None
    with subprocess.Popen(
        ["ctest", "--test-dir", BUILD, "--output-on-failure", "-j", "3"],
        cwd=ROOT, env={**os.environ, "LC_ALL": "C"}, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    ) as process:
        for line in process.stdout:
            now = time.monotonic()
            if line.startswith("Test project "):
                zero = now
            if zero is None:
                continue
            elapsed = round(now - zero, 3)
            if match := START.fullmatch(line):
                name = match[1]
                if name in expected:
                    start[name] = elapsed
                    peak = max(peak, len(start) - len(finish))
                    if name == COMPARE:
                        print(f"{label} comparison started at {elapsed:.3f}s", flush=True)
            elif match := FINISH.fullmatch(line):
                name, status, duration = match.groups()
                if name in expected:
                    finish[name] = (elapsed, float(duration), status == "Passed")
            elif match := TOTAL.fullmatch(line):
                total = float(match[1])
        code = process.wait()
    match = COMPARE_PASS.search(last_log.read_text(errors="replace")) if last_log.is_file() else None
    selected = int(match[1]) if match else None

    with (reports / f"{label}-tests.tsv").open("w") as report:
        report.write("test\tpassed\tstart_s\tfinish_s\tduration_s\n")
        for name in sorted(expected):
            done, duration, passed = finish.get(name, (None, None, False))
            report.write(f"{name}\t{int(passed)}\t{start.get(name, '')}\t"
                         f"{'' if done is None else done}\t{'' if duration is None else duration}\n")

    compare_end, compare_duration, _ = finish.get(COMPARE, (None, None, False))
    compare_start = start.get(COMPARE)
    last_other = max((name for name in finish if name != COMPARE),
                     key=lambda name: finish[name][0], default=None)
    other_end = finish[last_other][0] if last_other else None
    passed = sum(result[2] for result in finish.values())
    valid = (
        code == 0 and expected.keys() == start.keys() == finish.keys() and passed == len(expected)
        and selected is not None and total is not None and peak == 3
    )
    return {
        "trial": label, "valid": valid, "exit_code": code,
        "expected_count": len(expected), "passed_count": passed, "peak_ctest_jobs": peak,
        "comparison_source_cases": selected, "comparison_start_s": compare_start,
        "comparison_duration_s": compare_duration, "comparison_end_s": compare_end,
        "last_other_test": last_other, "last_other_end_s": other_end,
        "comparison_solo_tail_s": (
            round(max(0, compare_end - max(compare_start, other_end)), 3)
            if None not in (compare_start, compare_end, other_end) else None
        ),
        "ctest_total_s": total, "resources_before": before, "resources_after": resources(),
    }


def compiler_hash():
    checksum = hashlib.sha256()
    with (BUILD / "bin/ycxx").open("rb") as binary:
        for block in iter(lambda: binary.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def main(reports):
    reports.mkdir(parents=True, exist_ok=True)
    if "CMAKE_BUILD_TYPE:STRING=Debug" not in (BUILD / "CMakeCache.txt").read_text().splitlines():
        raise RuntimeError("Expected the normal Debug CI product")
    product = compiler_hash()
    variants, expected = registrations()
    if compiler_hash() != product:
        raise RuntimeError("The compiler changed while generating the candidate registration")
    metadata = {
        "head": output("git", "rev-parse", "HEAD"),
        "baseline": output("git", "rev-parse", "HEAD^"),
        "libycxx": output("git", "rev-parse", "HEAD:deps/libycxx"),
        "compiler_sha256": product, "ctest": output("ctest", "--version").splitlines()[0],
        "machine": platform.machine(), "system_release": platform.release(),
        "runner_image": os.environ.get("ImageVersion"),
        "logical_cpus": os.cpu_count(), "case_count": len(expected), "jobs": 3, "order": "ABBA",
    }
    (reports / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    results = []
    try:
        for index, arm in enumerate("ABBA", 1):
            GENERATED.write_bytes(variants[arm])
            result = trial(index, arm, expected, reports)
            (reports / f"{index}-{arm}.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result), flush=True)
            results.append(result)
    finally:
        GENERATED.write_bytes(variants["A"])
    if compiler_hash() != product:
        raise RuntimeError("The compiler changed during the trials")
    if not all(result["valid"] for result in results):
        raise SystemExit("At least one CTest trial failed or had an incomplete timing record")
    if len({result["comparison_source_cases"] for result in results}) != 1:
        raise SystemExit("daemon.compare reported different case counts across trials")
    timings = [result["ctest_total_s"] for result in results]
    deltas = {"AB_candidate_minus_baseline_s": round(timings[1] - timings[0], 2),
              "BA_candidate_minus_baseline_s": round(timings[2] - timings[3], 2)}
    (reports / "paired-deltas.json").write_text(json.dumps(deltas, indent=2) + "\n")
    print(json.dumps(deltas), flush=True)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
