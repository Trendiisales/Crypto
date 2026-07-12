#!/usr/bin/env python3
# ============================================================================
# binary_freshness_selftest.py  (S-2026-07-12)
#
# WHY THIS EXISTS: every staleness check the operator insisted on tested DATA
# freshness (feeds, book age, warm-seeds). NONE checked BINARY freshness. On
# 2026-07-12 the live crypto book was found spawning a STALE hand-compiled
# backtest/ibkrcrypto_bt from Jul-5 (pre px-quantization-fix) via bt_bin(),
# while fixes only rebuilt build/ -> every sub-$1 coin close mis-booked for
# WEEKS, invisible to all data checks. This guard closes that gap.
#
# Checks (crypto Mac book):
#   [1] CMAKE-AUTHORITATIVE staleness: `cmake --build` is a no-op iff every
#       build/ target is current with its ACTUAL source deps. If it compiles or
#       links anything, a live binary was stale -> RED. (Dependency-accurate;
#       no mtime false positives.)
#   [2] SHADOW binaries: any executable copy of a spawned binary OUTSIDE build/
#       (the dual-path trap) -> RED. bt_bin()-class stale copies must not exist.
#   [3] bt_bin() default resolves to build/ (single source of truth).
#
# RED (exit 2) = stale or shadowed live binary -> REBUILD / remove shadow BEFORE
# trusting any crypto book output.
# ============================================================================
import os, re, sys, glob, subprocess

ROOT  = "/Users/jo/Crypto"
BUILD = f"{ROOT}/build"
# binaries the production crons spawn (crontab uses build/*)
LIVE = ["fetch", "shadow_refresh", "shadow_refresh_intraday",
        "live_mark", "stall_companion", "wave_companion",
        "gui_server", "ibkrcrypto_bt"]

def main():
    fails = []

    # [1] READ-ONLY dependency check: `make -n` (dry-run) prints the recipes it WOULD
    #     run without running them. If it would compile/link anything, build/ is BEHIND
    #     source. Dependency-accurate (no mtime false positives) and NON-MUTATING
    #     (a monitor must never rebuild — audit-read-only rule).
    try:
        out = subprocess.run(["make", "-C", BUILD, "-n"],
                             capture_output=True, text=True, timeout=120).stdout
        would = [ln for ln in out.splitlines()
                 if ("Building CXX" in ln or "Linking CXX" in ln or "Building C object" in ln)]
        if would:
            fails.append("[STALE] `make -n` would re-compile/link — build/ is BEHIND source, rebuild:\n    "
                         + "\n    ".join(would[:12]))
    except Exception as e:
        fails.append(f"[MAKE] could not verify build currency: {e}")

    # [2] shadow copies of any spawned binary outside build/
    for name in LIVE:
        for q in glob.glob(f"{ROOT}/**/{name}", recursive=True):
            if (os.path.isfile(q) and os.access(q, os.X_OK) and "/.git/" not in q
                    and os.path.realpath(q) != os.path.realpath(f"{BUILD}/{name}")):
                fails.append(f"[SHADOW] {q} — a copy of build/{name} outside build/. "
                             f"The bt_bin() dual-path trap; remove it.")

    # [3] bt_bin() default must be the build/ copy
    try:
        rost = open(f"{ROOT}/include/crypto/Roster.hpp").read()
        m = re.search(r'bt_bin\(\).*?"([^"]*ibkrcrypto_bt)"', rost)
        if m and "/build/" not in m.group(1):
            fails.append(f"[BT_BIN] default = {m.group(1)} — must be build/ (single source of truth)")
    except Exception as e:
        fails.append(f"[BT_BIN] Roster.hpp unreadable: {e}")

    print(f"BINARY-FRESHNESS SELFTEST — {len(LIVE)} live crypto binaries (cmake-authoritative)")
    if fails:
        for f in fails: print("  FAIL ", f)
        print("RESULT: RED — stale/shadow binary in the live spawn path. REBUILD / remove shadow before trusting the book.")
        sys.exit(2)
    print(f"  PASS  cmake up-to-date; no shadow copies of {len(LIVE)} binaries; bt_bin->build/")
    print("RESULT: GREEN")

if __name__ == "__main__":
    main()
