#!/usr/bin/env python3
# tools/batch1/integration_gate.py -- KROFNE FORK (2133), Batch-1 TEST CONTRACT layer 3.
#
# The PRODUCTION BUILD-GRAPH gate. Layer 1/2/5 tests prove the pure decisions; this gate
# proves the build graph actually COMPILES THE TESTED CODE INTO THE DLL. The failure it
# exists for (review finding R, found at HEAD 49db879a): send_backlog.cpp existed, its
# pure core was selftested green, and the production wrapper that parks refused critical
# sends was NOT IN THE CMake TARGET -- the DLL would have linked without it and every
# SendCritical call would have been a link error (or, worse, a stale object from an old
# build graph). Tests that pass while the ship target excludes the code are false green.
#
# Assertions (each names itself on failure):
#   G1  every Batch-1 production .cpp exists in the tree
#   G2  every Batch-1 production .cpp is listed in the CMake target votv-coop
#   G2b send_backlog.cpp specifically listed (the R regression)
#   G2c drone_take_sync.cpp specifically listed
#   G3  no duplicate TU entries in the votv-coop source list
#   G4  no file(GLOB ...) anywhere in CMakeLists.txt (explicit list only)
#   G5  all ReliableKind enum values unique; fork kinds 121-124 present with their ids
#   G6  payload sizes: every `static_assert(sizeof(X) == N)` in protocol.h is TRUE --
#       verified by COMPILING a probe (the declared number vs the real layout); falls
#       back to a structural parse check when no C++ compiler is available
#   G7  fork-critical kinds 121-124 never ride a bare SendReliable/SendReliableToSlot
#       outside send_backlog.cpp (the one documented send wrapper). Receive/dispatch
#       sites are read-only by design and never send; any new send site must go through
#       SendCritical or be added to the explicit allowlist below WITH a reason.
#   G8  the Node2/Wine headless gate in session/shutdown.cpp (VOTVCOOP_HEADLESS_SKIP_WINDOW_TITLE)
#       is a SINGLE decision point confined to the diagnostic title query: exactly one
#       GetWindowTextW call site exists, the gate sits AFTER the SetWindowLongPtrW subclass
#       and BEFORE an unconditional GetWindowRect, the skip path cannot return/exit out of
#       Install(), and the rest of the shutdown installation (CoopWndProc subclass + both
#       DoShutdown close-signal call sites + IsShuttingDown) is untouched. Regression for the
#       runtime-proven Wine hang fix: the opt-in gate must remove ONLY the WM_GETTEXT hang,
#       never any install step.
#
# Runs BEFORE the expensive dependency build (pure stdlib; the G6 probe uses whatever
# C++ compiler is on PATH -- the same one CI then configures the DLL with).
#
# Exit 0 = gate green. Any violation = exit 1 with G-codes.

import os
import re
import subprocess
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VOTVCOOP = os.path.abspath(os.path.join(HERE, "..", "..", "src", "votv-coop"))

FAILURES = []
CHECKS = 0


def fail(code, msg):
    FAILURES.append((code, msg))
    print(f"GATE FAIL [{code}] {msg}")


def ok(code, msg):
    print(f"GATE ok   [{code}] {msg}")


def check(code, cond, msg_ok, msg_fail):
    global CHECKS
    CHECKS += 1
    if cond:
        ok(code, msg_ok)
    else:
        fail(code, msg_fail)


# ---- the Batch-1 production TU set (explicit, NO glob -- the gate IS the list) --------------
# The fork's own/modified TUs on branch krofne/b133-playability-batch1 (git diff 2f32d1af..HEAD).
# When Batch-1 grows, extend THIS list; the gate then forces CMake to follow.
BATCH1_TUS = [
    "src/coop/net/send_backlog.cpp",
    "src/coop/dev/batch1_smoke.cpp",
    "src/coop/interactables/drone_take_sync.cpp",
    "src/coop/interactables/drone_sync.cpp",
    "src/coop/player/sleep_sync.cpp",
    "src/coop/props/container_contents_sync.cpp",
    "src/coop/props/prop_drop_intent.cpp",
    "src/coop/props/host_spawn_watcher.cpp",
    "src/coop/dispatch/event_dispatch_intent.cpp",
    "src/coop/dispatch/event_dispatch_state.cpp",
    "src/coop/session/subsystems.cpp",
    "src/ue_wrap/devices/drone.cpp",
]

# Pure headers the headless tests compile -- also must exist (they ARE the tested code).
BATCH1_PURE_HEADERS = [
    "include/coop/net/send_backlog.h",
    "include/coop/props/container_extract_wire.h",
    "include/coop/props/extract_pairing.h",
    "include/coop/player/sleep_dilation_ownership.h",
    "include/coop/interactables/drone_take_sync.h",
    "include/ue_wrap/core/bool_mask.h",
]

# G7 allowlist: documented sites where a fork-critical kind may appear near a bare
# SendReliable/SendReliableToSlot. KEEP EMPTY unless a new site is genuinely justified;
# each entry needs the reason recorded here.
SEND_BYPASS_ALLOWLIST = {
    # "src/coop/example.cpp": "reason",
}

FORK_KIND_NAMES = [
    "DroneActionRequest",
    "DroneActionResult",
    "ContainerExtractIntent",
    "ContainerExtractResult",
]

FORK_KIND_IDS = {"DroneActionRequest": 121, "DroneActionResult": 122,
                 "ContainerExtractIntent": 123, "ContainerExtractResult": 124}


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_cpp_comments(text):
    """Blank out // and /* */ comments while PRESERVING string/char literals and offsets
    (removed characters are replaced 1:1 with spaces, so every position from the original
    text stays valid for ordering checks). String-literal aware so a '//' inside a literal
    is not mistaken for a comment."""
    out = list(text)
    i, n = 0, len(text)
    state = "code"   # code | line | block | str | wide_str | char
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"
                out[i] = out[i + 1] = " "
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out[i] = out[i + 1] = " "
                i += 2
                continue
            if c == 'L' and nxt == '"':
                state = "wide_str"
                i += 2
                continue
            if c == '"':
                state = "str"
                i += 1
                continue
            if c == "'":
                state = "char"
                i += 1
                continue
            i += 1
        elif state == "line":
            if c == "\n":
                state = "code"
            else:
                out[i] = " "
            i += 1
        elif state == "block":
            if c == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                state = "code"
                i += 2
                continue
            if c != "\n":
                out[i] = " "
            i += 1
        else:  # inside a literal
            if c == "\\":
                i += 2
                continue
            if (state == "str" and c == '"') or (state == "wide_str" and c == '"') \
                    or (state == "char" and c == "'"):
                state = "code"
            i += 1
    return "".join(out)


def cmake_votvcoop_sources(cmake_text):
    """Extract the add_library(votv-coop SHARED ...) source list (explicit entries only)."""
    m = re.search(r"add_library\s*\(\s*votv-coop\s+SHARED(.*?)\)", cmake_text, re.S)
    if not m:
        return None, "add_library(votv-coop SHARED ...) block not found"
    body = m.group(1)
    entries = []
    for tok in body.split():
        tok = tok.strip()
        if tok.endswith(".cpp") or tok.endswith(".c") or tok.endswith(".h"):
            entries.append(tok.replace("${CMAKE_CURRENT_SOURCE_DIR}/", ""))
    return entries, None


def parse_fork_kinds(protocol_text):
    """Parse `Name = N,` entries INSIDE the `enum class ReliableKind` block only (other
    enums in the header legitimately reuse small ids -- only ReliableKind uniqueness is
    a wire invariant)."""
    kinds = {}
    m = re.search(r"enum\s+class\s+ReliableKind[^{]*\{(.*?)\n\};", protocol_text, re.S)
    if not m:
        return kinds
    for em in re.finditer(r"^\s{4}(\w+)\s*=\s*(\d+)\s*,", m.group(1), re.M):
        kinds[em.group(1)] = int(em.group(2))
    return kinds


def parse_static_assert_sizes(protocol_text):
    """All `static_assert(sizeof(X) == N, ...)` pairs from protocol.h."""
    out = []
    for m in re.finditer(r"static_assert\s*\(\s*sizeof\s*\(\s*(\w+)\s*\)\s*==\s*(\d+)\s*,", protocol_text):
        out.append((m.group(1), int(m.group(2))))
    return out


def find_compiler():
    for c in ("g++", "clang++", "cl"):
        p = shutil.which(c)
        if p:
            return c, p
    return None, None


def build_size_probe(compiler, include_dir, workdir):
    """Compile a probe that PRINTS sizeof() for every declared static_assert size and
    compare the real layout against the declared number. Returns (declared, real) dict."""
    protocol_h = os.path.join(include_dir, "coop", "net", "protocol.h")
    pairs = parse_static_assert_sizes(read(protocol_h))
    if not pairs:
        return None, "no static_assert(sizeof(...)) declarations parsed from protocol.h"
    lines = ["#include \"coop/net/protocol.h\"", "#include <cstdio>",
             "using namespace coop::net;", "int main() {"]
    for name, _n in pairs:
        lines.append(f'    std::printf("{name}=%zu\\n", sizeof({name}));')
    lines.append("    return 0;\n}")
    probe_cpp = os.path.join(workdir, "b1_size_probe.cpp")
    with open(probe_cpp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    exe = os.path.join(workdir, "b1_size_probe")
    if compiler == "cl":
        cmd = ["cl", "/nologo", "/EHsc", "/std:c++20", "/utf-8",
               "/I" + include_dir, "/Fe" + exe, probe_cpp]
        shell = True  # cl needs the dev environment variables
    else:
        cmd = [compiler, "-std=c++20", "-I", include_dir, probe_cpp, "-o", exe]
        shell = False
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, shell=shell, timeout=120)
        if r.returncode != 0:
            return None, f"probe compile failed: {r.stderr[-800:]}"
        r2 = subprocess.run([exe], capture_output=True, text=True, timeout=60)
        if r2.returncode != 0:
            return None, f"probe run failed: {r2.stderr[-400:]}"
    except Exception as e:  # noqa: BLE001
        return None, f"probe execution error: {e}"
    real = {}
    for line in r2.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            try:
                real[k.strip()] = int(v.strip())
            except ValueError:
                pass
    return dict(pairs), real


def main():
    print(f"== Batch-1 integration gate ==\n   target: {VOTVCOOP}\n")

    # ---- G1: production TUs + pure headers exist ------------------------------------------
    for rel in BATCH1_TUS + BATCH1_PURE_HEADERS:
        p = os.path.join(VOTVCOOP, rel)
        check("G1", os.path.isfile(p), f"{rel} exists",
              f"required file missing from the tree: {rel}")

    # ---- G4: no globs ---------------------------------------------------------------------
    cmake_path = os.path.join(VOTVCOOP, "CMakeLists.txt")
    cmake_text = read(cmake_path)
    check("G4", not re.search(r"\bfile\s*\(\s*GLOB", cmake_text, re.I),
          "CMakeLists.txt uses no file(GLOB) for sources",
          "CMakeLists.txt contains file(GLOB) -- the build graph must be an explicit list")

    # ---- G2/G2b/G2c/G3: the target includes the batch, exactly once -----------------------
    entries, err = cmake_votvcoop_sources(cmake_text)
    if err or entries is None:
        fail("G2", f"could not parse the votv-coop source list: {err}")
        print(f"\n== gate: {CHECKS} checks, {len(FAILURES)} failures ==\nBUILD-GRAPH: FAIL")
        return 1
    listed = set(entries)
    for rel in BATCH1_TUS:
        in_target = rel in listed
        check("G2", in_target, f"{rel} listed in target votv-coop",
              f"{rel} NOT in the votv-coop target sources -- the DLL would not contain it")
    check("G2b", "src/coop/net/send_backlog.cpp" in listed,
          "send_backlog.cpp listed (the R regression: the backlog wrapper was absent from "
          "the target at HEAD 49db879a)",
          "send_backlog.cpp missing from the votv-coop target (R regression)")
    check("G2c", "src/coop/interactables/drone_take_sync.cpp" in listed,
          "drone_take_sync.cpp listed", "drone_take_sync.cpp missing from the votv-coop target")
    dupes = sorted({e for e in entries if entries.count(e) > 1})
    check("G3", not dupes, f"no duplicate TU entries ({len(entries)} listed)",
          f"duplicate TU entries in votv-coop: {dupes}")

    # ---- G5: fork kinds unique + present --------------------------------------------------
    protocol_h = os.path.join(VOTVCOOP, "include", "coop", "net", "protocol.h")
    proto_text = read(protocol_h)
    all_kinds = parse_fork_kinds(proto_text)
    ids = list(all_kinds.values())
    dup_ids = sorted({i for i in ids if ids.count(i) > 1})
    check("G5", not dup_ids, f"all {len(ids)} ReliableKind values unique",
          f"duplicate ReliableKind values in protocol.h: {dup_ids}")
    for name, want in FORK_KIND_IDS.items():
        got = all_kinds.get(name)
        check("G5", got == want,
              f"{name} = {want}", f"{name} expected {want}, found {got}")

    # ---- G6: payload sizes: declared == real (compile-probed when possible) ---------------
    compiler, _ = find_compiler()
    if compiler:
        with tempfile.TemporaryDirectory() as td:
            declared, real = build_size_probe(compiler, os.path.join(VOTVCOOP, "include"), td)
        if declared is None:
            fail("G6", f"size probe unusable ({real}); falling back to declaration parse only")
            pairs = parse_static_assert_sizes(proto_text)
            check("G6", len(pairs) >= 4, f"{len(pairs)} size declarations parsed",
                  "too few size declarations parsed from protocol.h")
        else:
            bad = {k: (v, real.get(k)) for k, v in declared.items() if real.get(k) != v}
            check("G6", not bad,
                  f"all {len(declared)} declared payload sizes match the compiled layout "
                  f"(probe: {compiler})",
                  f"declared vs compiled size mismatches: {bad}")
    else:
        pairs = parse_static_assert_sizes(proto_text)
        check("G6", len(pairs) >= 4,
              f"no compiler on PATH -- {len(pairs)} size declarations parsed structurally "
              "(install a compiler for the full compiled probe)",
              "no compiler AND no size declarations parsed")

    # ---- G7: fork kinds never bypass SendCritical ------------------------------------------
    src_root = os.path.join(VOTVCOOP, "src")
    kind_re = re.compile("|".join(FORK_KIND_NAMES))
    send_re = re.compile(r"\bSendReliable(?:ToSlot)?\s*\(")
    bypasses = []
    for dirpath, _dirs, files in os.walk(src_root):
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, VOTVCOOP).replace("\\", "/")
            text = read(full).splitlines()
            for i, line in enumerate(text):
                if not kind_re.search(line):
                    continue
                window = "\n".join(text[max(0, i - 8): i + 8])
                if not send_re.search(window):
                    continue
                # A bare send near a fork kind. Tolerated ONLY in send_backlog.cpp (the
                # documented wrapper) or the explicit allowlist.
                if rel == "src/coop/net/send_backlog.cpp":
                    continue
                if rel in SEND_BYPASS_ALLOWLIST:
                    continue
                bypasses.append(f"{rel}:{i + 1}")
    check("G7", not bypasses,
          "fork kinds 121-124 never ride a bare SendReliable outside send_backlog.cpp",
          f"fork-critical kinds bypass SendCritical at: {bypasses} -- route them through "
          "coop::net::send_backlog::SendCritical or document the site in SEND_BYPASS_ALLOWLIST")

    # ---- G8: the Wine headless gate removes ONLY the title query ---------------------------
    shutdown_rel = "src/coop/session/shutdown.cpp"
    shutdown_path = os.path.join(VOTVCOOP, shutdown_rel)
    if os.path.isfile(shutdown_path):
        code = strip_cpp_comments(read(shutdown_path))
        ENV_TOKEN = "VOTVCOOP_HEADLESS_SKIP_WINDOW_TITLE"

        gwtw = [m.start() for m in re.finditer(r"\bGetWindowTextW\s*\(", code)]
        check("G8a", len(gwtw) == 1,
              f"exactly {len(gwtw)} GetWindowTextW call site (the only WM_GETTEXT hang surface)",
              f"expected exactly 1 GetWindowTextW call site in {shutdown_rel}, found {len(gwtw)}")

        env_uses = [m.start() for m in re.finditer(ENV_TOKEN, code)]
        check("G8b", len(env_uses) == 1,
              f"{ENV_TOKEN} is a single decision point",
              f"expected exactly 1 code reference to {ENV_TOKEN} (single decision point), "
              f"found {len(env_uses)}")
        bootchk = "BOOTCHK" in code
        check("G8b", bootchk,
              "the skip path emits the BOOTCHK diagnostic",
              f"no BOOTCHK diagnostic in the skip path of {shutdown_rel}")

        subclass = re.search(r"SetWindowLongPtrW\s*\(\s*best\s*,", code)
        gate = re.search(r"GetEnvironmentVariableW\s*\(", code)
        consume = re.search(r"\bif\s*\(\s*s_headlessSkipWindowTitle\s*\)", code)
        rect = re.search(r"GetWindowRect\s*\(", code)
        ordered = (subclass and gate and consume and rect
                   and gwtw and subclass.start() < gate.start() < consume.start()
                   < gwtw[0] < rect.start())
        check("G8c", ordered,
              "gate order: SetWindowLongPtrW(subclass) < env gate < if(skip) < GetWindowTextW < GetWindowRect",
              "the headless gate is not confined to the diagnostic dump: required order "
              "SetWindowLongPtrW < GetEnvironmentVariableW(gate) < if(skip) < GetWindowTextW < "
              "GetWindowRect broken -- the gate must not sit before (and thus bypass) the "
              "subclass, and GetWindowRect must still run after the skip")

        # The SKIP BRANCH must not bail out of Install(): no return/exit between the skip
        # decision and the unconditional GetWindowRect. (The gate predicate lambda's own
        # `return` lives BEFORE the if -- it evaluates the env var, it does not leave Install.)
        if consume and rect:
            span = code[consume.start():rect.start()]
            bailout = re.search(r"\breturn\b|\bExitProcess\s*\(|\bTerminateProcess\s*\(|\bexit\s*\(", span)
            check("G8d", not bailout,
                  "skip path cannot return/exit out of Install() -- GetWindowRect and the "
                  "diagnostic log still run",
                  f"a return/exit appears between the skip branch and GetWindowRect in {shutdown_rel} "
                  "-- the headless gate would bypass the rest of the diagnostic/install path")
        else:
            check("G8d", False,
                  "",
                  f"skip-branch 'if (s_headlessSkipWindowTitle)' or GetWindowRect not found in "
                  f"{shutdown_rel} -- cannot prove the skip path stays inside Install()")

        check("G8e", bool(re.search(r"SetWindowLongPtrW\s*\(\s*best\s*,\s*GWLP_WNDPROC\s*,"
                                    r"\s*reinterpret_cast<LONG_PTR>\(\s*&CoopWndProc\s*\)", code)),
              "CoopWndProc subclassing via SetWindowLongPtrW(best, GWLP_WNDPROC, &CoopWndProc) intact",
              f"the CoopWndProc subclass call changed shape in {shutdown_rel}")
        doshutdown_calls = len(re.findall(r"\bDoShutdown\s*\(\s*\)\s*;", code))
        check("G8e", doshutdown_calls >= 2,
              f"DoShutdown() still invoked from the wndproc close paths ({doshutdown_calls} call sites)",
              f"expected >= 2 DoShutdown(); call sites in {shutdown_rel} (WM_CLOSE + "
              "WM_QUERYENDSESSION) -- the shutdown handler must stay wired")
        check("G8e", bool(re.search(r"\bIsShuttingDown\s*\(", code)) and
                     bool(re.search(r"void\s+DoShutdown\s*\(\s*\)", code)),
              "IsShuttingDown() and the DoShutdown definition intact",
              f"DoShutdown/IsShuttingDown definitions missing from {shutdown_rel}")
    else:
        fail("G8a", f"{shutdown_rel} missing from the tree -- cannot verify the Wine headless gate")

    # ---- summary ----------------------------------------------------------------------------
    print(f"\n== gate: {CHECKS} checks, {len(FAILURES)} failures ==")
    if FAILURES:
        print("BUILD-GRAPH: FAIL")
        return 1
    print("BUILD-GRAPH: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
