#!/usr/bin/env python3
"""Step 6.5 device scenarios (get_states, rx liveness, resubscribe, stack, heap, token).

Usage (Git Bash or PowerShell; needs a background serial logger, see tools/serial_log.py, and
a Python the Windows firewall lets accept LAN connections for the mock):

    python tools/step65_check.py --ip DEVICE_IP --serial-log LOG --mock-host PC_IP \
        --mock-python <py3.15> [--cert-dir .pio/testcerts] SCENARIO [SCENARIO ...]

Scenarios: plain3m, tls3m, small, oversize, stall, stallpong, sub, stack, token.
The device must run a build with -DHAKNOB_STATES_ON_READY (except "token" and "sub" may run on
any build). Only the ha and controls config sections are changed, through POST /api/config; the
mock token is a throwaway constant. Prints the measured numbers and PASS/FAIL per check.
"""
import argparse
import http.client
import json
import os
import re
import subprocess
import sys
import time

TOKEN = "mock-good-token"
results = []


def check(ok, name, detail=""):
    results.append((ok, name))
    print(("  PASS " if ok else "  FAIL ") + name + (f"  [{detail}]" if detail else ""), flush=True)


class Dev:
    def __init__(self, ip):
        self.ip = ip

    def req(self, method, path, body=None):
        c = http.client.HTTPConnection(self.ip, 80, timeout=15)
        h = {"Host": self.ip}
        if method == "POST":
            h.update({"Content-Type": "application/json", "X-HAKnob": "1"})
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, data.decode("utf-8", "replace")

    def set_ha(self, url, token, ca="", insecure=False):
        st, raw = self.req("POST", "/api/config", json.dumps(
            {"ha": {"url": url, "ca_pem": ca, "tls_insecure": insecure, "token": token}}))
        if st != 200:
            raise RuntimeError(f"set_ha {st} {raw[:200]}")

    def set_controls(self, ids, action="none"):
        ctl = [{"entity_id": i, "label": "", "action": action, "step": 0, "press": "none"} for i in ids]
        st, raw = self.req("POST", "/api/config", json.dumps({"controls": ctl}))
        if st != 200:
            raise RuntimeError(f"set_controls {st} {raw[:300]}")

    def reboot(self):
        self.req("POST", "/api/reboot", "{}")

    def status(self):
        st, raw = self.req("GET", "/api/status")
        return json.loads(raw)


class Ser:
    def __init__(self, path):
        self.path = path

    def mark(self):
        return os.path.getsize(self.path)

    def since(self, mark):
        with open(self.path, "rb") as f:
            f.seek(mark)
            out = []
            for ln in f.read().decode("utf-8", "replace").splitlines():
                m = re.match(r"(\d+\.\d+) (.*)", ln)
                if m:
                    out.append((float(m.group(1)), m.group(2)))
            return out

    def wait(self, mark, regex, timeout):
        end = time.time() + timeout
        while time.time() < end:
            for t, ln in self.since(mark):
                if re.search(regex, ln):
                    return t, ln
            time.sleep(0.5)
        return None


class Mock:
    def __init__(self, args, margs, outfile):
        self.out = open(outfile, "w", encoding="utf-8")
        self.p = subprocess.Popen([args.mock_python, "-u", os.path.join(os.path.dirname(__file__), "mockha.py")]
                                  + margs, stdout=self.out, stderr=subprocess.STDOUT)
        time.sleep(1.5)

    def stop(self):
        self.p.kill()
        self.p.wait()
        self.out.close()


def hb(lines):
    """Parses HB lines into dicts with the epoch."""
    out = []
    for t, ln in lines:
        if ln.startswith("HB "):
            d = {k: v for k, v in re.findall(r"(\w+)=(\S+)", ln)}
            d["t"] = t
            out.append(d)
    return out


def num(d, k):
    try:
        return int(d[k])
    except (KeyError, ValueError):
        return None


def summarize_hb(lines, label):
    h = hb(lines)
    if not h:
        print(f"  no HB lines for {label}")
        return {}
    gaps = [b["t"] - a["t"] for a, b in zip(h, h[1:])]
    s = {
        "loopmax": max(num(x, "loopmax") or 0 for x in h),
        "heap_min": min(num(x, "heap_min") or 10**9 for x in h),
        "heap_low": min(num(x, "heap") or 10**9 for x in h),
        "stack_min": min(num(x, "stack") or 10**9 for x in h),
        "psram_free_min": min(num(x, "psram_free") or 10**9 for x in h),
        "hb_gap_max": max(gaps) if gaps else 0,
        "n": len(h),
    }
    print(f"  {label}: HB n={s['n']} loopmax_max={s['loopmax']} heap_min_min={s['heap_min']} "
          f"heap_low={s['heap_low']} stack_min={s['stack_min']} psram_free_min={s['psram_free_min']} "
          f"max_hb_gap={s['hb_gap_max']:.1f}s", flush=True)
    return s


def last_hb(ser, mark):
    h = hb(ser.since(mark))
    return h[-1] if h else {}


def go(dev, ser, args, name, margs, url, ids, tls=None, wait_regex=None, wait_s=90, controls_action="none",
       reboot=False):
    """Starts the mock, points the device at it, waits; returns (lines, mock_log, t0)."""
    print(f"== {name}: mock {' '.join(margs)}", flush=True)
    mf = os.path.join(args.workdir, f"mock-{name}.log")
    m = Mock(args, margs, mf)
    ca, insecure = (tls or ("", False))
    if reboot:
        # config is already in place (set by a previous run); reboot gives a fresh heap_min
        dev.set_controls(ids, controls_action)
        dev.set_ha(url, TOKEN, ca, insecure)
        time.sleep(3)
        mark = ser.mark()
        t0 = time.time()
        dev.reboot()
    else:
        dev.set_controls(ids, controls_action)
        mark = ser.mark()
        t0 = time.time()
        dev.set_ha(url, TOKEN, ca, insecure)
    r = ser.wait(mark, wait_regex, wait_s) if wait_regex else None
    return m, mark, t0, r


def finish(dev, m, ser, mark, name):
    m.stop()
    lines = ser.since(mark)
    with open(os.path.join(os.path.dirname(__file__), "..", ".pio", f"step65-{name}.serial.log"), "w",
              encoding="utf-8") as f:
        f.write("\n".join(f"{t:.3f} {l}" for t, l in lines))
    return lines


def find(lines, regex):
    return [(t, l) for t, l in lines if re.search(regex, l)]


def kv(line, key):
    m = re.search(rf"\b{key}=(\S+)", line)
    return m.group(1) if m else None


DOMS = ["light", "switch", "climate", "cover", "fan", "sensor", "media_player"]


def unique_ids(n, start=0):
    """First n mock entity ids of a supported domain (sensor is not a control domain)."""
    out, i = [], start
    while len(out) < n:
        if i % 7 != 5:
            out.append(f"{DOMS[i % 7]}.mock_{i}")
        i += 1
    return out


def states_run(dev, ser, args, name, tls, reboot=True):
    scheme = "https" if tls else "http"
    margs = ["--seconds", "170", "--states-synthetic", "3000000", "--states-count", "1500",
             "--pong-after-states", "--change-every", "2", "--rm-add-every", "3"]
    ca, insecure = ("", False)
    if tls:
        margs += ["--tls-cert", os.path.join(args.cert_dir, "self.pem"), "--tls-key", os.path.join(args.cert_dir, "self.key")]
        ca, insecure = ("", True)
    ids = unique_ids(24)
    m, mark, t0, r = go(dev, ser, args, name, margs, f"{scheme}://{args.mock_host}:8765", ids,
                        (ca, insecure), r"HA catalog done", 120, reboot=reboot)
    time.sleep(35)  # let it settle, c / r / a events flow
    pre = None
    lines = finish(dev, m, ser, mark, name)
    check(r is not None, f"{name}: HA catalog done", r[1] if r else "")
    for pat in [r"HA subscribe .*ids=24", r"HA states request id=", r"HA states retained len=", r"HA catalog done"]:
        f = find(lines, pat)
        check(bool(f), f"{name}: log '{pat}'", f[0][1] if f else "")
    s = summarize_hb(lines, name)
    fx = find(lines, r"HA catalog done")
    if fx:
        ln = fx[0][1]
        print("  " + ln)
        jm = int(kv(ln, "json_ms") or -1)
        check(jm <= 25, f"{name}: json_ms <= 25 (R8)", f"json_ms={jm} build_ms={kv(ln, 'build_ms')} json={kv(ln, 'json')}")
    check(s.get("loopmax", 999) <= 25, f"{name}: loopmax <= 25 ms", f"max={s.get('loopmax')}")
    check(s.get("stack_min", 0) >= 2048, f"{name}: stack >= 2048", f"min={s.get('stack_min')}")
    check(not find(lines, r"pong_timeout|rx_stalled|HA state=.*error="), f"{name}: no pong_timeout / errors",
          "; ".join(l for _, l in find(lines, r"pong_timeout|rx_stalled|HA state=.*error=")[:3]))
    h = hb(lines)
    seq = [x.get("cat") for x in h]
    comp = []
    for c in seq:
        if not comp or comp[-1] != c:
            comp.append(c)
    print("  cat sequence:", comp)
    check("fetching" in comp and "building" in comp and "ready" in comp, f"{name}: cat fetching->building->ready", str(comp))
    ents = sorted(set(x.get("ent") for x in h))
    print("  ent values:", ents)
    # psram: before fetch (first HB of the run when cat != fetching) and after catalog done
    pf_before = next((num(x, "psram_free") for x in h if x.get("cat") in ("none", "ready") and num(x, "psram_free")), None)
    tdone = fx[0][0] if fx else None
    pf_after = [num(x, "psram_free") for x in h if tdone and x["t"] > tdone + 1]
    jsonb = int(kv(fx[0][1], "json") or 0) if fx else 0
    if pf_before and pf_after:
        print(f"  psram_free before={pf_before} after={pf_after[0]} (delta {pf_before-pf_after[0]}, json={jsonb})")
        check(pf_before - pf_after[0] - jsonb <= 65536, f"{name}: psram back within 64 KB (minus json)",
              f"delta={pf_before-pf_after[0]} json={jsonb}")
    ready_hb = [x for x in h if x.get("cat") == "ready" and x.get("ha") == "ready"]
    if ready_hb:
        print(f"  steady: heap={ready_hb[-1]['heap']} heap_min={ready_hb[-1]['heap_min']} stack={ready_hb[-1]['stack']}")
    return lines, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True, help="device IP")
    ap.add_argument("--serial-log", required=True)
    ap.add_argument("--mock-host", required=True, help="LAN IP of this PC as seen from the device")
    ap.add_argument("--mock-python", required=True)
    ap.add_argument("--cert-dir", default=os.path.join(os.path.dirname(__file__), "..", ".pio", "testcerts"))
    ap.add_argument("--workdir", default=".")
    ap.add_argument("scenarios", nargs="+")
    args = ap.parse_args()
    dev, ser = Dev(args.ip), Ser(args.serial_log)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    for sc in args.scenarios:
        if sc == "plain3m":
            states_run(dev, ser, args, "plain3m", False)
        elif sc == "tls3m":
            states_run(dev, ser, args, "tls3m", True)
        else:
            print("unknown scenario", sc)
    bad = [n for ok, n in results if not ok]
    print(f"SUMMARY {len(results) - len(bad)}/{len(results)} checks passed")
    for n in bad:
        print("  FAILED:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
