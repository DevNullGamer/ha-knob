#!/usr/bin/env python3
"""Step 6.5 device scenarios, second part (see tools/step65_check.py for the helpers and usage).

    python tools/step65_scen.py --serial-log LOG --mock-python PY --workdir DIR SCENARIO ...

Scenarios: p3m, t3m (3 MB / 1500 entities, plain / TLS, clean reboot), small, cancel, stall,
oversize, sub, stack, token. The device must run a -DHAKNOB_STATES_ON_READY build.
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import step65_check as c  # noqa: E402
from step65_check import (TOKEN, Dev, Ser, Mock, check, find, go, hb, kv, num, results, summarize_hb,  # noqa: E402
                          finish, unique_ids)

BASE = {"plain_heap": 115000, "plain_heap_min": 105700, "tls_heap": 106000, "tls_heap_min": 93000}


def hb_after(lines, regex):
    t = next((t for t, l in lines if re.search(regex, l)), None)
    return [x for x in hb(lines) if t is not None and x["t"] >= t]


def r8_checks(name, lines):
    fx = find(lines, r"HA catalog done")
    if not fx:
        check(False, f"{name}: catalog done present")
        return
    ln = fx[0][1]
    print("  " + ln)
    smax = int(kv(ln, "json_slice_max_ms") or 999)
    reg = int(kv(ln, "json_regrow") or 999)
    check(smax <= 9, f"{name}: json_slice_max_ms <= 9", f"{smax} (json_ms={kv(ln, 'json_ms')} passes={kv(ln, 'json_passes')})")
    check(reg == 0, f"{name}: json_regrow == 0", str(reg))
    hs = hb_after(lines, r"HA states request")
    lm = [num(x, "loopmax") for x in hs]
    print("  loopmax per HB (receive..after done):", lm)
    check(max(lm or [999]) <= 25, f"{name}: loopmax <= 25 in every HB", str(max(lm or [999])))
    tdone = fx[0][0]
    seq = []
    for x in hs:
        if x["t"] < tdone - 0.001 and x.get("cat") not in seq:
            seq.append(x.get("cat"))
    after = [x.get("cat") for x in hs if x["t"] >= tdone]
    print("  cat before done:", seq, " first HB cat after done:", after[:1])
    check("ready" not in seq, f"{name}: cat never 'ready' before catalog done", str(seq))


def heap_gate(name, lines, tls):
    hs = [x for x in hb_after(lines, r"HA state=ready") if x.get("cat") == "ready"]
    if not hs:
        check(False, f"{name}: HB with cat=ready")
        return
    last = hs[-1]
    heap, hmin = num(last, "heap"), num(last, "heap_min")
    bh = BASE["tls_heap" if tls else "plain_heap"]
    bm = BASE["tls_heap_min" if tls else "plain_heap_min"]
    print(f"  {name}: heap={heap} heap_min={hmin} (v0.5.2 baseline heap {bh}, heap_min {bm})")
    check(heap >= bh - 4096, f"{name}: heap >= baseline-4KB", f"{heap} vs {bh}")
    check(hmin >= bm - 4096, f"{name}: heap_min >= baseline-4KB", f"{hmin} vs {bm}")


def run_states(dev, ser, args, name, tls, count=1500, nbytes=3000000, nctl=24, extra=()):
    scheme = "https" if tls else "http"
    margs = ["--seconds", "150", "--states-synthetic", str(nbytes), "--states-count", str(count),
             "--pong-after-states", "--change-every", "2", "--rm-add-every", "3"] + list(extra)
    insecure = False
    if tls:
        margs += ["--tls-cert", os.path.join(args.cert_dir, "self.pem"),
                  "--tls-key", os.path.join(args.cert_dir, "self.key")]
        insecure = True
    m, mark, t0, r = go(dev, ser, args, name, margs, f"{scheme}://{args.mock_host}:8765", unique_ids(nctl),
                        ("", insecure), r"HA catalog done", 120, reboot=True)
    time.sleep(35)
    lines = finish(dev, m, ser, mark, name)
    check(r is not None, f"{name}: HA catalog done")
    for pat in [rf"HA subscribe .*ids={nctl}", r"HA states request id=", r"HA states retained len=", r"HA catalog done"]:
        f = find(lines, pat)
        check(bool(f), f"{name}: log '{pat}'", f[0][1] if f else "")
    summarize_hb(lines, name)
    r8_checks(name, lines)
    bad = [x for x in find(lines, r"pong_timeout|rx_stalled|error=|Guru|Backtrace") if "network_down" not in x[1]]
    check(not bad, f"{name}: no pong_timeout/rx_stalled/errors/reset", "; ".join(l for _, l in bad[:3]))
    hs = hb_after(lines, r"HA states request")
    stk = min([num(x, "stack") or 0 for x in hs] or [0])
    check(stk >= 2048, f"{name}: stack >= 2048", str(stk))
    print("  ent values:", sorted(set(x.get("ent") for x in hs)))
    fx = find(lines, r"HA catalog done")
    pre = [num(x, "psram_free") for x in hb(lines) if x.get("cat") == "none" and num(x, "psram_free")]
    post = [num(x, "psram_free") for x in hb(lines) if fx and x["t"] > fx[0][0] + 1]
    jb = int(kv(fx[0][1], "json") or 0) if fx else 0
    if pre and post:
        d = pre[-1] - post[-1]
        print(f"  psram_free before={pre[-1]} after={post[-1]} delta={d} json={jb}")
        check(d - jb <= 65536, f"{name}: psram back within 64 KB (minus json)", f"delta={d}")
    heap_gate(name, lines, tls)
    return lines


def cancel_json(dev, ser, args):
    print("== cancel: URL switches between two mocks around build / JSON phase", flush=True)
    base = ["--seconds", "300", "--states-synthetic", "3000000", "--states-count", "1500"]
    ma = Mock(args, base + ["--port", "8765"], os.path.join(args.workdir, "mock-cancel-a.log"))
    mb = Mock(args, base + ["--port", "8766"], os.path.join(args.workdir, "mock-cancel-b.log"))
    dev.set_controls(unique_ids(5), "none")
    t_start = time.time()
    mark = ser.mark()
    port = 8765
    dev.set_ha(f"http://{args.mock_host}:{port}", TOKEN)
    for d in [1.60 + 0.05 * k for k in range(18)]:
        r = ser.wait(mark, r"HA states retained", 60)
        if not r:
            check(False, f"cancel: retained for delay {d}")
            break
        time.sleep(d)
        port = 8766 if port == 8765 else 8765
        dev.set_ha(f"http://{args.mock_host}:{port}", TOKEN)
        mark = ser.mark()
        time.sleep(0.3)
    ser.wait(mark, r"HA catalog done", 60)
    time.sleep(10)
    ma.stop()
    mb.stop()
    lines = [(t, l) for t, l in ser.since(0) if t >= t_start]
    with open(os.path.join(args.workdir, "serial-cancel.log"), "w", encoding="utf-8") as f:
        f.write("\n".join(f"{t:.3f} {l}" for t, l in lines))
    done = find(lines, r"HA catalog done")
    retained = find(lines, r"HA states retained")
    cfgs = find(lines, r"HA config url")
    print(f"  retained={len(retained)} catalog done={len(done)} url switches={len(cfgs)}")
    cancelled = 0
    for t, _ in retained:
        nxt = next((tc for tc, _ in cfgs if tc >= t), None)
        if nxt is not None and not any(t <= td <= nxt for td, _ in done):
            cancelled += 1
    print(f"  fetches cancelled between 'retained' and the next switch: {cancelled}")
    check(cancelled >= 1, "cancel: at least one fetch cancelled after retained", str(cancelled))
    check(len(done) <= len(retained), "cancel: not more 'catalog done' than retained", f"{len(done)} <= {len(retained)}")
    last_cfg = cfgs[-1][0] if cfgs else 0
    check(any(td > last_cfg for td, _ in done), "cancel: final fetch completes (catalog done after the last switch)")
    h = hb(lines)
    ups = [num(x, "up") for x in h]
    check(all(b >= a for a, b in zip(ups, ups[1:])), "cancel: HB uptime monotonic (no reboot/crash)")
    check(not find(lines, r"Guru|Backtrace|rst:0x"), "cancel: no crash markers")
    print("  cat sequence tail:", [x.get("cat") for x in h][-6:])


def stall_tests(dev, ser, args):
    ids = unique_ids(3)
    name = "stall"
    margs = ["--seconds", "100", "--states-synthetic", "3000000", "--states-count", "1500", "--stall-states"]
    m, mark, t0, r = go(dev, ser, args, name, margs, f"http://{args.mock_host}:8765", ids, None, r"error=rx_stalled", 45)
    time.sleep(2)
    lines = finish(dev, m, ser, mark, name)
    stall = find(lines, r"HA rx stalled")
    req = find(lines, r"HA states request")
    check(bool(stall), "stall: 'HA rx stalled bytes= ms=' logged", stall[0][1] if stall else "")
    err = find(lines, r"error=rx_stalled")
    check(bool(err), "stall: error=rx_stalled", err[0][1] if err else "; ".join(l for _, l in find(lines, r"error=")[:3]))
    mocklog = open(os.path.join(args.workdir, "mock-stall.log"), encoding="utf-8").read()
    print("  mock STALL line:", re.findall(r"\[\s*([\d.]+)s [^\]]*\] STALL states.*", mocklog)[:1])
    if stall and req:
        print(f"  request -> rx stalled: {stall[0][0] - req[0][0]:.1f} s")
    name = "stallpong"
    margs = ["--seconds", "150", "--states-synthetic", "3000000", "--states-count", "1500", "--stall-states",
             "--pong-after-states", "--states-delay", "13", "--states-rate", "90000"]
    m, mark, t0, r = go(dev, ser, args, name, margs, f"http://{args.mock_host}:8765", ids, None,
                        r"error=(pong_timeout|rx_stalled)", 90)
    time.sleep(2)
    lines = finish(dev, m, ser, mark, name)
    pt = find(lines, r"error=pong_timeout")
    rs = find(lines, r"error=rx_stalled")
    print("  ", [l for _, l in find(lines, r"error=|rx stalled")[:4]])
    check(bool(pt) and not rs, "stallpong: pong_timeout (expected), not rx_stalled", f"pong_timeout={len(pt)} rx_stalled={len(rs)}")


def oversize(dev, ser, args, single=False):
    name = "oversize1" if single else "oversize"
    margs = ["--seconds", "120", "--states-synthetic", "4500000", "--states-count", "1500"]
    if single:
        margs += ["--states-fragment", "8000000"]
    m, mark, t0, r = go(dev, ser, args, name, margs, f"http://{args.mock_host}:8765", unique_ids(3), None,
                        r"HA catalog failed", 60)
    time.sleep(40)
    lines = finish(dev, m, ser, mark, name)
    for pat in [r"HA frame dropped.*reason=too_large", r"HA catalog failed error=states_too_large"]:
        f = find(lines, pat)
        check(bool(f), f"oversize: '{pat}'", f[0][1] if f else "")
    connects = find(lines, r"HA connect attempt")
    check(len(connects) <= 1, "oversize: no reconnect after the drop", f"connect attempts={len(connects)}")
    h = hb(lines)
    check(bool(h) and h[-1].get("ha") == "ready", "oversize: ha=ready stays", h[-1].get("ha") if h else "")
    print("  last HB cat:", h[-1].get("cat") if h else None)


def sub_tests(dev, ser, args):
    print("== sub: duplicate id, controls change, unrelated save", flush=True)
    margs = ["--seconds", "200", "--states-synthetic", "30000", "--states-count", "40", "--change-every", "2",
             "--rm-add-every", "3"]
    ids = unique_ids(4)
    m = Mock(args, margs, os.path.join(args.workdir, "mock-sub.log"))
    dev.set_controls([], "none")
    dev.set_ha(f"http://{args.mock_host}:8765", TOKEN)
    time.sleep(8)
    mark = ser.mark()
    dev.set_controls(ids + [ids[0]], "none")
    r = ser.wait(mark, r"HA subscribe .*ids=4", 10)
    check(bool(r), "sub: duplicate entity -> ids=4 (unique)", r[1] if r else "")
    time.sleep(10)
    ln = hb(ser.since(mark))
    check(bool(ln) and ln[-1].get("ent", "").endswith("/4"), "sub: HB ent=<n>/4", ln[-1].get("ent") if ln else "")
    print("  ent over time:", [x.get("ent") for x in ln])
    mark = ser.mark()
    dev.set_controls(ids[:2], "none")
    time.sleep(3)
    lines = ser.since(mark)
    check(bool(find(lines, r"HA unsubscribe")) and bool(find(lines, r"HA subscribe .*ids=2")),
          "sub: change -> HA unsubscribe + HA subscribe ids=2",
          "; ".join(l for _, l in find(lines, r"HA (un)?subscribe")))
    check(not find(lines, r"HA connect attempt"), "sub: no new HA connect on controls change")
    mark = ser.mark()
    ctl = [{"entity_id": i, "label": "L", "action": "none", "step": 0, "press": "none"} for i in ids[:2]]
    st, raw = dev.req("POST", "/api/config", json.dumps({"controls": ctl}))
    time.sleep(2)
    cfg = json.loads(dev.req("GET", "/api/config")[1])
    dsec = dict(cfg["device"])
    orig = dsec["accel"]
    dsec["accel"] = not orig
    dev.req("POST", "/api/config", json.dumps({"device": dsec}))
    time.sleep(2)
    dsec["accel"] = orig
    dev.req("POST", "/api/config", json.dumps({"device": dsec}))
    time.sleep(3)
    lines = ser.since(mark)
    check(st == 200, "sub: label-only controls save accepted")
    check(not find(lines, r"HA (un)?subscribe|HA connect attempt"),
          "sub: unrelated saves (label, device.accel) do not resubscribe",
          "; ".join(l for _, l in find(lines, r"HA (un)?subscribe|HA connect")[:3]))
    m.stop()


def stack_tests(dev, ser, args):
    name = "stack"
    margs = ["--seconds", "90", "--deep-result", "5", "19", "20", "25", "--after-auth-delay", "6"]
    m, mark, t0, r = go(dev, ser, args, name, margs, f"http://{args.mock_host}:8765", unique_ids(3), None, None, 0)
    time.sleep(40)
    lines = finish(dev, m, ser, mark, name)
    mock = open(os.path.join(args.workdir, "mock-stack.log"), encoding="utf-8").read()
    print("  mock:", re.findall(r"SENT deep result nesting=\d+", mock))
    h = hb_after(lines, r"HA state=ready")
    sm = [num(x, "stack") for x in h]
    print("  stack= per HB:", sm)
    check(bool(sm) and min(sm) >= 2048, "stack: stack >= 2048 after deep results", str(min(sm) if sm else None))
    check(not find(lines, r"rst:0x|Guru|Backtrace"), "stack: no crash after 25-level message")
    check(all(x.get("ha") == "ready" for x in h), "stack: ha=ready throughout", str(set(x.get("ha") for x in h)))
    print("  related lines:", [l for _, l in find(lines, r"dropped|HA event|deser")[:3]])


def token_grep(args):
    hits = 0
    paths = [args.serial_log] + [os.path.join(args.workdir, f) for f in os.listdir(args.workdir)
                                 if f.endswith(".log") and f != os.path.basename(args.serial_log)]
    paths += [os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".pio", f)
              for f in os.listdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".pio"))
              if f.startswith("step65-") and f.endswith(".log")]
    for pth in paths:
        try:
            txt = open(pth, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        hits += txt.count(TOKEN)
        print(f"  grep {os.path.basename(pth)}: {len(txt)} bytes, token occurrences {txt.count(TOKEN)}, "
              f"'token_set len=' lines {len(re.findall(r'token_set len=', txt))}")
    check(hits == 0, "token: token value never appears in serial/mock logs")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True, help="device IP")
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--mock-host", required=True, help="LAN IP of this PC as seen from the device")
    ap.add_argument("--mock-python", required=True)
    ap.add_argument("--cert-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".pio", "testcerts"))
    ap.add_argument("--workdir", default=".")
    ap.add_argument("scenarios", nargs="+")
    args = ap.parse_args()
    dev, ser = Dev(args.ip), Ser(args.serial_log)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    for sc in args.scenarios:
        print(f"##### {sc}", flush=True)
        if sc == "p3m":
            run_states(dev, ser, args, "plain3m", False)
        elif sc == "t3m":
            run_states(dev, ser, args, "tls3m", True)
        elif sc == "t3m_d":
            run_states(dev, ser, args, "tls3m-delayed", True, extra=["--states-delay", "7"])
        elif sc == "p3m_d":
            run_states(dev, ser, args, "plain3m-delayed", False, extra=["--states-delay", "3"])
        elif sc == "small":
            run_states(dev, ser, args, "small", False, count=20, nbytes=20000, nctl=10)
        elif sc == "cancel":
            cancel_json(dev, ser, args)
        elif sc == "stall":
            stall_tests(dev, ser, args)
        elif sc == "oversize":
            oversize(dev, ser, args)
        elif sc == "oversize1":
            oversize(dev, ser, args, True)
        elif sc == "sub":
            sub_tests(dev, ser, args)
        elif sc == "stack":
            stack_tests(dev, ser, args)
        elif sc == "token":
            token_grep(args)
        else:
            print("unknown scenario", sc)
    bad = [n for ok, n in results if not ok]
    print(f"SUMMARY {len(results) - len(bad)}/{len(results)} checks passed")
    for n in bad:
        print("  FAILED:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
