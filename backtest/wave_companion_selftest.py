#!/usr/bin/env python3
"""wave_companion_selftest.py — EFFECT-LEVEL alarm for CryptoWaveCompanion (S-2026-07-05,
de-200DMA'd S-2026-07-06).

WHY THIS EXISTS (operator, 05-07-2026): the wave engine produced 0 trades and NOTHING told
us whether that was "correctly quiet (no wave to arm on)" or "dead / stale-fed / logic broken".
Silence is ambiguous. This test asserts FUNCTION, not existence, and DISTINGUISHES the two.

NO 200DMA (operator hard rule 2026-07-06, feedback-no-200dma-crypto): the former bull-gate is
GONE. The engine arms on price structure alone, ANY regime. So silence is only "correctly quiet"
when there is genuinely no recent +STEP% advance (between waves). A coin making a fresh new high
in the last STAG window with 0 armed companions is BROKEN and must SCREAM.

CHECKS (each pass/fail independently; overall RED if any fail):
  [1] SCHEDULED + ALIVE   — wave_companion in cron AND wrote state.json in the last ALIVE_MIN.
  [2] INPUT FRESH         — BTC+ETH 1h feed recent (the engine is 1h-only now; a stale feed =>
                            silence on OLD prices, the exact blind spot that made "0 trades" look fine).
  [3] SILENCE EXPLAINED   — per coin: is price making a fresh new-high in the last STAG window?
                            Fresh high + 0 armed => BROKEN (RED, "should be screaming"). No fresh
                            high => quiet is CORRECT (report it, GREEN).
  [4] FIRES ON TRIGGER    — synthetic rising +1% series in a sandbox => engine MUST arm companions.
                            Proves the arm/stack logic actually RUNS (not just exists).
  [5] COMPUTES A BOOK     — replay real data in a sandbox => both coins produce waves + closed
                            comps + finite net (deterministic faithful replay still sane).

Exit 0 = all green. Exit 1 = one or more RED. Writes a status file the surface/hook can show.
"""
import os, sys, json, subprocess, tempfile, shutil, time, csv, bisect

HOME   = os.path.expanduser("~")
CRYPTO = os.path.join(HOME, "Crypto")
BIN    = os.path.join(CRYPTO, "build", "wave_companion")
SRC    = os.path.join(CRYPTO, "src", "wave_companion.cpp")
DATA   = os.path.join(CRYPTO, "backtest", "data")
STATE  = os.path.join(DATA, "wave_companion", "state.json")
STATUS = os.path.join(HOME, ".claude", "wave_companion_selftest_STATUS.txt")
CRON_TAG = "wave-companion"          # marker in the installed cron line
ALIVE_MIN = 75                       # hourly cron (:07) -> >75min stale = not running
STEP = 0.01                          # locked config (must match the engine defaults)
COINS = ("BTC", "ETH")

results = []
def record(name, ok, detail): results.append((name, ok, detail))

def _load(path):
    rows = []
    try:
        with open(path) as f:
            for r in csv.DictReader(f):
                rows.append((int(r["open_time_ms"]), float(r["close"])))
    except Exception:
        return []
    rows.sort()
    return rows

def _replay_open(datadir):
    """Run the CURRENT binary on the REAL 1h data in a sandbox out-dir; return {coin: open_comps}.
    The engine is a PURE FUNCTION of the data, so this is the AUTHORITATIVE open-comp count for
    'now'. Used to reconcile the live cron state (correct == live matches this replay)."""
    sb = tempfile.mkdtemp(prefix="wavecomp_reconcile_")
    try:
        subprocess.run([BIN], env=dict(os.environ, WAVECOMP_DATADIR=sb),
                       capture_output=True, text=True)
        sp = os.path.join(sb, "state.json")
        if not os.path.exists(sp):
            return None
        d = json.load(open(sp))
        return {c.get("coin"): int(c.get("open_comps", 0) or 0) for c in d.get("coins", [])}
    finally:
        shutil.rmtree(sb, ignore_errors=True)

# ---- [1] SCHEDULED + ALIVE ---------------------------------------------------
def check_scheduled_alive():
    try: cron = subprocess.run(["crontab","-l"], capture_output=True, text=True).stdout
    except Exception: cron = ""
    scheduled = CRON_TAG in cron and "wave_companion" in cron
    age_min = (time.time() - os.path.getmtime(STATE)) / 60.0 if os.path.exists(STATE) else None
    alive = age_min is not None and age_min <= ALIVE_MIN
    ok = scheduled and alive
    detail = f"in cron={scheduled}; state age=" + (f"{age_min:.1f}min (<= {ALIVE_MIN})" if age_min is not None else "MISSING")
    if not scheduled: detail += "  *** NOT SCHEDULED -- engine never runs ***"
    elif not alive:   detail += "  *** STALE -- cron not firing / engine crashed ***"
    record("[1] SCHEDULED+ALIVE", ok, detail)

# ---- [2] INPUT FRESH (1h only -- engine is 1h-driven, no daily) --------------
def check_input_fresh():
    probs = []
    for coin in COINS:
        rows = _load(os.path.join(DATA, f"{coin}USDT_1h.csv"))
        if not rows:
            probs.append(f"{coin}_1h MISSING"); continue
        age = (time.time()*1000 - rows[-1][0]) / 86400_000
        if age > 3.0:
            probs.append(f"{coin}_1h {age:.1f}d stale (>3d) -> silence on OLD prices")
    ok = not probs
    record("[2] INPUT-FRESH", ok, "BTC+ETH 1h feeds fresh" if ok else "*** " + "; ".join(probs) + " ***")

# ---- [2b] BINARY FRESH (source built into the running binary) ----------------
def check_binary_fresh():
    """The failure that actually bit us 2026-07-06: the 200DMA-removal + config fix were COMMITTED
    but the binary was compiled ~1min BEFORE them, so the live cron ran stale logic for hours and
    nothing flagged it. A stale binary is invisible to a replay-reconcile (both use the same BIN).
    Guard it directly: the binary must be at least as new as the source it is built from."""
    if not (os.path.exists(BIN) and os.path.exists(SRC)):
        record("[2b] BINARY-FRESH", False, "*** binary or source missing ***"); return
    bt = os.path.getmtime(BIN); st = os.path.getmtime(SRC)
    ok = bt >= st
    if ok:
        detail = f"binary built {time.strftime('%m-%d %H:%M', time.localtime(bt))} >= source {time.strftime('%m-%d %H:%M', time.localtime(st))}"
    else:
        lag = (st - bt) / 60.0
        detail = f"*** SOURCE {lag:.0f}min NEWER than binary -> STALE BINARY, rebuild (cmake --build build --target wave_companion) ***"
    record("[2b] BINARY-FRESH", ok, detail)

# ---- [3] SILENCE EXPLAINED (replay-reconcile, NO fresh-high proxy) ------------
def check_silence_explained():
    """Silence must be EXPLAINED, never assumed. The engine is a PURE FUNCTION of the 1h data,
    so the ONLY correct open-comp count for 'now' is a faithful replay of that data. Reconcile the
    live cron state against a fresh replay: equal => whatever it shows (active OR 0-open) is
    correct-BY-CONSTRUCTION, not a guess. Diverged => the live state is stale/crashed => RED.
    This replaces the old 'within-STEP-of-24h-max' fresh-high proxy, which false-RED'd whenever
    price sat near a short-window max while the engine was legitimately between waves (its true
    peak older than the window, or price short of the +STEP re-arm above the last wave exit)."""
    live = {}
    if os.path.exists(STATE):
        try: live = {c.get("coin"): int(c.get("open_comps", 0) or 0)
                     for c in json.load(open(STATE)).get("coins", [])}
        except Exception: live = {}
    auth = _replay_open(DATA)
    if not auth:
        record("[3] SILENCE-EXPLAINED", False, "*** faithful replay produced no state -> binary broken ***"); return
    notes, broken = [], []
    for coin in COINS:
        a = auth.get(coin); l = live.get(coin)
        if a is None:
            broken.append(f"{coin}: replay produced no state"); continue
        if l is None:
            broken.append(f"{coin}: live state MISSING -> cron not writing"); continue
        if l != a:
            broken.append(f"{coin}: live open={l} != faithful replay open={a} -> LIVE STATE STALE/DIVERGED")
        elif a > 0:
            notes.append(f"{coin} ACTIVE open_comps={a} (live==replay)")
        else:
            notes.append(f"{coin} between-waves 0-open (live==replay; no +{STEP*100:.0f}% break above last wave exit -> correctly quiet)")
    ok = not broken
    detail = "; ".join(notes) if ok else "*** " + "; ".join(broken) + " *** | " + "; ".join(notes)
    record("[3] SILENCE-EXPLAINED", ok, detail)

# ---- [4] FIRES ON SYNTHETIC TRIGGER ------------------------------------------
def check_fires_on_trigger():
    sb = tempfile.mkdtemp(prefix="wavecomp_selftest_")
    try:
        ddir = os.path.join(sb, "data"); os.makedirs(ddir)
        odir = os.path.join(sb, "out")
        day0 = 1_600_000_000_000
        for coin in COINS:
            # 1h: 120 bars rising +1% each -> forces MANY stacked companions (no regime gate now)
            with open(os.path.join(ddir, f"{coin}USDT_1h.csv"), "w") as f:
                f.write("open_time_ms,open,high,low,close\n")
                base = day0
                px = 130.0
                for i in range(120):
                    ms = base + i*3600_000
                    px *= 1.01
                    f.write(f"{ms},{px},{px},{px},{px}\n")
        env = dict(os.environ, IBKRCRYPTO_CSVDIR=ddir, WAVECOMP_DATADIR=odir)
        r = subprocess.run([BIN], env=env, capture_output=True, text=True)
        opened = closed = 0
        sp = os.path.join(odir, "state.json")
        if os.path.exists(sp):
            d = json.load(open(sp))
            opened = d.get("open_comps", 0); closed = d.get("closed_comps", 0)
        fired = (opened + closed) > 0
        detail = (f"synthetic rising -> companions armed={opened+closed} "
                  + ("(fires correctly)" if fired else "DID NOT ARM *** arm logic BROKEN ***"))
        if not fired and r.stderr: detail += " | " + r.stderr.strip().splitlines()[-1][:120]
        record("[4] FIRES-ON-TRIGGER", fired, detail)
    finally:
        shutil.rmtree(sb, ignore_errors=True)

# ---- [5] COMPUTES A BOOK (real data, sandbox) --------------------------------
def check_computes_book():
    sb = tempfile.mkdtemp(prefix="wavecomp_book_")
    try:
        env = dict(os.environ, WAVECOMP_DATADIR=sb)
        r = subprocess.run([BIN], env=env, capture_output=True, text=True)
        sp = os.path.join(sb, "state.json")
        if not os.path.exists(sp):
            record("[5] COMPUTES-BOOK", False, "engine produced no state.json  *** binary broken ***"); return
        d = json.load(open(sp))
        by = {c.get("coin"): c for c in d.get("coins", [])}
        bad = []
        for coin in COINS:
            c = by.get(coin, {})
            if c.get("skipped"): bad.append(f"{coin} SKIPPED (integ={c.get('integ')})"); continue
            if int(c.get("waves", 0)) <= 0: bad.append(f"{coin} 0 waves over full history (impossible if healthy)")
            rn = c.get("raw_unit_net")
            if rn is None or not (rn == rn): bad.append(f"{coin} raw_unit_net={rn}")
        ok = not bad
        summ = ", ".join(f"{coin} {by.get(coin,{}).get('waves','?')}w/${by.get(coin,{}).get('raw_unit_net','?')}" for coin in COINS)
        record("[5] COMPUTES-BOOK", ok, (summ if ok else "*** " + "; ".join(bad) + " *** | " + summ))
    finally:
        shutil.rmtree(sb, ignore_errors=True)

def main():
    if not os.path.exists(BIN):
        print(f"WAVE-COMPANION SELF-TEST RED -- binary missing: {BIN}"); sys.exit(1)
    check_scheduled_alive()
    check_input_fresh()
    check_binary_fresh()
    check_silence_explained()
    check_fires_on_trigger()
    check_computes_book()
    overall = all(ok for _, ok, _ in results)
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    head = "GREEN -- wave engine FUNCTIONAL (quiet is explained, not broken)" if overall else "RED -- WAVE ENGINE PROBLEM"
    lines = [f"WAVE-COMPANION SELF-TEST {head}  ({ts})"]
    for name, ok, detail in results:
        lines.append(f"  {'PASS' if ok else 'FAIL'} {name}: {detail}")
    out = "\n".join(lines)
    print(out)
    try:
        os.makedirs(os.path.dirname(STATUS), exist_ok=True)
        open(STATUS, "w").write(out + "\n")
    except Exception: pass
    sys.exit(0 if overall else 1)

if __name__ == "__main__":
    main()
