#!/usr/bin/env python3
"""wave_companion_selftest.py — EFFECT-LEVEL alarm for CryptoWaveCompanion (S-2026-07-05).

WHY THIS EXISTS (operator, 05-07-2026): the wave engine produced 0 trades and NOTHING told
us whether that was "correctly dormant (bull-gated below 200DMA)" or "dead / stale-fed / logic
broken". Silence is ambiguous. This test asserts FUNCTION, not existence, and — crucially —
DISTINGUISHES the two: a coin below its 200DMA SHOULD be flat (bull-gate = BearSpotNoEdge), but a
coin ABOVE its 200DMA that is making fresh highs and STILL shows no armed companion is BROKEN and
must SCREAM. Mirrors tools/protection_selftest.py (the stall-accountant self-test) for the wave book.

CHECKS (each pass/fail independently; overall RED if any fail):
  [1] SCHEDULED + ALIVE   — wave_companion in cron AND wrote state.json in the last ALIVE_MIN.
  [2] INPUT FRESH         — BTC+ETH 1h & 1d feeds recent (a stale feed => wrong regime call, the
                            exact blind spot that made "0 trades" look fine on old prices).
  [3] SILENCE EXPLAINED   — per coin: bull-gate state + gap-to-200DMA. Below 200DMA => 0 trades is
                            CORRECT (report it, GREEN). Above 200DMA + a fresh new-high in the last
                            STAG window + 0 armed companions => BROKEN (RED, "should be screaming").
  [4] FIRES ON TRIGGER    — synthetic bull + rising +1% series in a sandbox => engine MUST arm
                            companions. Proves the arm/stack logic actually RUNS (not just exists).
  [5] COMPUTES A BOOK     — replay real data in a sandbox => both coins produce waves + closed
                            comps + finite net (deterministic faithful replay still sane).

Exit 0 = all green. Exit 1 = one or more RED. Writes a status file the surface/hook can show.
"""
import os, sys, json, subprocess, tempfile, shutil, time, csv, bisect

HOME   = os.path.expanduser("~")
CRYPTO = os.path.join(HOME, "Crypto")
BIN    = os.path.join(CRYPTO, "build", "wave_companion")
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

def _regime(coin):
    """(is_bull, gap_pct, last_px, dma) from the daily feed — bull = last close > 200DMA."""
    d = _load(os.path.join(DATA, f"{coin}USDT_1d.csv"))
    if len(d) < 201:
        return (None, None, None, None)
    closes = [c for _, c in d]
    dma = sum(closes[-201:-1]) / 200.0
    last = closes[-1]
    return (last > dma, 100.0 * (last / dma - 1.0), last, dma)

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

# ---- [2] INPUT FRESH ---------------------------------------------------------
def check_input_fresh():
    probs = []
    for coin in COINS:
        for tf, maxd in (("1h", 3.0), ("1d", 3.0)):
            rows = _load(os.path.join(DATA, f"{coin}USDT_{tf}.csv"))
            if not rows:
                probs.append(f"{coin}_{tf} MISSING"); continue
            age = (time.time()*1000 - rows[-1][0]) / 86400_000
            if age > maxd:
                probs.append(f"{coin}_{tf} {age:.1f}d stale (>{maxd:.0f}d) -> regime call on OLD prices")
    ok = not probs
    record("[2] INPUT-FRESH", ok, "BTC+ETH 1h/1d feeds fresh" if ok else "*** " + "; ".join(probs) + " ***")

# ---- [3] SILENCE EXPLAINED (dormant-vs-broken) -------------------------------
def check_silence_explained():
    st = {}
    if os.path.exists(STATE):
        try: st = json.load(open(STATE))
        except Exception: st = {}
    by = {c.get("coin"): c for c in st.get("coins", [])}
    notes, broken = [], []
    for coin in COINS:
        is_bull, gap, last, dma = _regime(coin)
        if is_bull is None:
            broken.append(f"{coin}: <201 daily bars, cannot judge regime"); continue
        opencomps = int(by.get(coin, {}).get("open_comps", 0) or 0)
        if not is_bull:
            notes.append(f"{coin} DORMANT-OK ({gap:+.1f}% below 200DMA; arms when >200DMA)")
        else:
            # bull: is price making a fresh high recently? (last 1h close == running max of last STAG_H?)
            h1 = _load(os.path.join(DATA, f"{coin}USDT_1h.csv"))
            stag_h = 48 if coin == "BTC" else 24
            recent = [p for ms, p in h1 if ms >= h1[-1][0] - stag_h*3600_000] if h1 else []
            advancing = bool(recent) and h1[-1][1] >= max(recent) * (1 - STEP)
            if advancing and opencomps == 0:
                broken.append(f"{coin}: BULL (+{gap:.1f}% >200DMA) + fresh high but 0 armed -> SHOULD BE SCREAMING")
            else:
                notes.append(f"{coin} BULL (+{gap:.1f}%), open_comps={opencomps}"
                             + ("" if advancing else ", no fresh high (between waves)"))
    ok = not broken
    detail = "; ".join(notes) if ok else "*** " + "; ".join(broken) + " *** | " + "; ".join(notes)
    record("[3] SILENCE-EXPLAINED", ok, detail)

# ---- [4] FIRES ON SYNTHETIC TRIGGER ------------------------------------------
def check_fires_on_trigger():
    sb = tempfile.mkdtemp(prefix="wavecomp_selftest_")
    try:
        ddir = os.path.join(sb, "data"); os.makedirs(ddir)
        odir = os.path.join(sb, "out")
        # 260 daily bars: flat 100 for 250 then a bull ramp so last close > 200DMA (bull-gate ON)
        day0 = 1_600_000_000_000
        for coin in COINS:
            with open(os.path.join(ddir, f"{coin}USDT_1d.csv"), "w") as f:
                f.write("open_time_ms,open,high,low,close\n")
                for i in range(260):
                    c = 100.0 if i < 250 else 100.0 + (i-249)*3.0   # ramp above the 200DMA
                    ms = day0 + i*86400_000
                    f.write(f"{ms},{c},{c},{c},{c}\n")
            # 1h: 120 bars rising +1% each -> forces MANY stacked companions (adjacent jump 1% << 300%)
            with open(os.path.join(ddir, f"{coin}USDT_1h.csv"), "w") as f:
                f.write("open_time_ms,open,high,low,close\n")
                base = day0 + 259*86400_000
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
        detail = (f"synthetic bull+rising -> companions armed={opened+closed} "
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
    check_silence_explained()
    check_fires_on_trigger()
    check_computes_book()
    overall = all(ok for _, ok, _ in results)
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    head = "GREEN -- wave engine FUNCTIONAL (dormant is explained, not broken)" if overall else "RED -- WAVE ENGINE PROBLEM"
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
