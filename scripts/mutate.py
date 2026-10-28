#!/usr/bin/env python3
"""Mutation testing under ADR 0010 SC31.6.

    scripts/mutate.py <set>          e.g. scripts/mutate.py stage6_layer1

Runs every mutant in scripts/mutants/<set>.py twice: as built, and with
the KASSERTs of the set's BLIND files disabled. Each run builds the
self-test kernel, boots it in QEMU and classifies the outcome:

  KILLED      the suite ran and failed: a CHECK failed, the kernel
              panicked outside an assertion, or a test hung
  ASSERT-ONLY the only failure was a KASSERT panic (not a kill)
  SURVIVED    the suite passed
  EQUIVALENT  the suite passed and the set argues equivalence
  BUILD-FAIL  the mutant did not compile or link (not a kill)
  STALE       the pattern was not found exactly once (not a kill)

Every KILLED result carries its reason, taken from the boot log. The
report goes to build/mutation/<set>.md. Exit status is 0 only if every
mutant is KILLED or EQUIVALENT in both modes.
"""

import importlib.util
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = "build/test/tarka.iso"
LOG = "build/test/selftest.log"
BLIND_PATCH = "\n#undef KASSERT\n#define KASSERT(cond) ((void)sizeof(cond))\n"


def load_set(name):
    path = os.path.join(ROOT, "scripts", "mutants", name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read(path):
    with open(os.path.join(ROOT, path)) as f:
        return f.read()


def write(path, text):
    with open(os.path.join(ROOT, path), "w") as f:
        f.write(text)


def blind(text):
    """Disable KASSERT after the file's last #include."""
    last = None
    for m in re.finditer(r"^#include .*\n", text, re.M):
        last = m
    if last is None:
        raise SystemExit("blind: no #include to anchor on")
    return text[:last.end()] + BLIND_PATCH + text[last.end():]


def build():
    r = subprocess.run(["make", "-j%d" % os.cpu_count(), ISO], cwd=ROOT,
                       capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr)[-2000:]


def boot():
    r = subprocess.run(["scripts/run-tests.sh", ISO], cwd=ROOT,
                       capture_output=True, text=True,
                       env=dict(os.environ, TEST_TIMEOUT=os.environ.get("TEST_TIMEOUT", "60")))
    try:
        log = read(LOG)
    except OSError:
        log = ""
    return r.returncode, r.stdout + r.stderr, log


def running_test(log, order):
    """The test that was running when the log ends."""
    done = re.findall(r"^  \[(?:PASS|FAIL)\] (\S+)", log, re.M)
    if not done:
        return order[0] if order else "?"
    i = order.index(done[-1]) if done[-1] in order else -1
    return order[i + 1] if 0 <= i < len(order) - 1 else "after " + done[-1]


def classify(out, log, order):
    fails = re.findall(r"^  \[FAIL\] (\S+)", log, re.M)
    checks = re.findall(r"^    FAIL (.*)$", log, re.M)
    panic = re.search(r"\*\*\* KERNEL PANIC: (.*)", log)
    if "selftest: running" not in log:
        return "HARNESS-FAIL", "the suite never started"
    if fails or checks:
        names = ", ".join(dict.fromkeys(fails)) or running_test(log, order)
        first = checks[0] if checks else ""
        return "KILLED", "test %s: FAIL %s" % (names, first)
    if panic:
        msg = panic.group(1).strip()
        where = running_test(log, order)
        if msg.startswith("assertion failed"):
            return "ASSERT-ONLY", "in %s: %s" % (where, msg)
        return "KILLED", "panic in %s: %s" % (where, msg)
    if "timed out" in out:
        return "KILLED", "hang in %s (timeout)" % running_test(log, order)
    if "self-tests passed" in out:
        return "SURVIVED", ""
    return "HARNESS-FAIL", out.strip().splitlines()[-1] if out.strip() else "no output"


def run_mode(mutset, mode, order):
    results = {}
    originals = {f: read(f) for f in set([m["file"] for m in mutset.MUTANTS] + mutset.BLIND)}
    try:
        for m in mutset.MUTANTS:
            for f, t in originals.items():
                write(f, blind(t) if mode == "blind" and f in mutset.BLIND else t)
            src = read(m["file"])
            n = src.count(m["old"])
            if n != 1:
                results[m["id"]] = ("STALE", "pattern found %d times" % n)
                continue
            write(m["file"], src.replace(m["old"], m["new"]))
            ok, blog = build()
            if not ok:
                results[m["id"]] = ("BUILD-FAIL", blog.strip().splitlines()[-1])
                continue
            t0 = time.time()
            _, out, log = boot()
            verdict, reason = classify(out, log, order)
            if verdict == "SURVIVED" and m.get("equivalent"):
                verdict, reason = "EQUIVALENT", m["equivalent"]
            results[m["id"]] = (verdict, reason)
            print("  %s %-6s %-26s %-11s %s (%.0fs)" % (m["id"], mode, m["name"], verdict,
                  reason[:110], time.time() - t0), flush=True)
    finally:
        for f, t in originals.items():
            write(f, t)
    return results


def baseline(mutset, mode):
    originals = {f: read(f) for f in mutset.BLIND}
    try:
        if mode == "blind":
            for f, t in originals.items():
                write(f, blind(t))
        ok, blog = build()
        if not ok:
            raise SystemExit("baseline (%s) does not build:\n%s" % (mode, blog))
        _, out, log = boot()
        if "self-tests passed" not in out:
            raise SystemExit("baseline (%s) does not pass:\n%s" % (mode, out[-2000:]))
        return re.findall(r"^  \[PASS\] (\S+)", log, re.M)
    finally:
        for f, t in originals.items():
            write(f, t)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    name = sys.argv[1]
    if not re.fullmatch(r"[a-z0-9_]+", name):
        raise SystemExit("bad set name: %r" % name)
    mutset = load_set(name)
    head = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                          capture_output=True, text=True).stdout.strip()
    res = {}
    for mode in ("normal", "blind"):
        print("== %s: baseline" % mode, flush=True)
        order = baseline(mutset, mode)
        print("== %s: %d tests pass; %d mutants" % (mode, len(order), len(mutset.MUTANTS)), flush=True)
        res[mode] = run_mode(mutset, mode, order)

    good = ("KILLED", "EQUIVALENT")
    lines = ["# Mutation results: %s" % name, "",
             "Commit `%s`. Rule: ADR 0010 SC31.6. Blind mode disables KASSERT in: %s." %
             (head, ", ".join("`%s`" % f for f in mutset.BLIND)), "",
             "| ID | Mutant | File | Normal | Blind | Expected catcher |",
             "|---|---|---|---|---|---|"]
    for m in mutset.MUTANTS:
        lines.append("| %s | `%s` | `%s` | %s | %s | %s |" % (
            m["id"], m["name"], m["file"].split("/")[-1],
            res["normal"][m["id"]][0], res["blind"][m["id"]][0], m["expect"]))
    lines += ["", "## Reasons", ""]
    for m in mutset.MUTANTS:
        lines.append("- **%s `%s`**" % (m["id"], m["name"]))
        for mode in ("normal", "blind"):
            v, r = res[mode][m["id"]]
            lines.append("  - %s: %s. %s" % (mode, v, r.replace("|", "\\|")))
    total = len(mutset.MUTANTS)
    for mode in ("normal", "blind"):
        k = sum(v == "KILLED" for v, _ in res[mode].values())
        e = sum(v == "EQUIVALENT" for v, _ in res[mode].values())
        lines.append("")
        lines.append("%s: %d/%d killed, %d equivalent, %d other." % (mode, k, total, e, total - k - e))
    report = "\n".join(lines) + "\n"
    os.makedirs(os.path.join(ROOT, "build", "mutation"), exist_ok=True)
    write(os.path.join("build", "mutation", name + ".md"), report)
    print(report)
    bad = [m["id"] for m in mutset.MUTANTS for mode in res if res[mode][m["id"]][0] not in good]
    if subprocess.run(["git", "diff", "--quiet"], cwd=ROOT).returncode != 0:
        raise SystemExit("the source tree was not restored")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
