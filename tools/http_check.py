#!/usr/bin/env python3
"""LAN HTTP checks against a running HA Knob (plan.md §3.8, §4 Step 4).

Usage:
    python tools/http_check.py --ip 192.168.1.143 [--hostname ha-knob] [--reboot]
                               [--web-password-test]

Only uses the Python standard library. Every request uses the device IP as the Host
header unless stated otherwise, like a browser opening http://<ip>/.

Safety: never touches the wifi or ha sections. The only state-changing requests are
 - a device patch with an invalid hostname (must be rejected with 400),
 - with --web-password-test: setting a random web password and clearing it again
   (always cleared in a finally block),
 - with --reboot: POST /api/reboot.
Exit code 0 when every check passes, 1 otherwise.
"""

import argparse
import base64
import http.client
import json
import secrets
import socket
import sys
import time

TIMEOUT = 8


class Checker:
    def __init__(self, ip, port=80):
        self.ip = ip
        self.port = port
        self.failures = []
        self.passes = 0
        self.responses = []  # (label, status, headers) for header-wide checks

    # -- HTTP ---------------------------------------------------------------------------

    def request(self, method, path, body=None, headers=None, host=None, chunked=False,
                label=None):
        hdrs = {"Host": host if host is not None else self.ip}
        hdrs.update(headers or {})
        conn = http.client.HTTPConnection(self.ip, self.port, timeout=TIMEOUT)
        try:
            conn.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
            for k, v in hdrs.items():
                conn.putheader(k, v)
            data = body.encode("utf-8") if isinstance(body, str) else body
            if chunked:
                conn.putheader("Transfer-Encoding", "chunked")
                conn.endheaders()
                if data:
                    conn.send(b"%x\r\n%s\r\n" % (len(data), data))
                conn.send(b"0\r\n\r\n")
            else:
                if data is not None:
                    conn.putheader("Content-Length", str(len(data)))
                conn.endheaders()
                if data:
                    try:
                        conn.send(data)
                    except (BrokenPipeError, ConnectionResetError):
                        pass  # server may answer 413 and close before reading the body
            resp = conn.getresponse()
            raw = resp.read()
            headers_out = {k.lower(): v for k, v in resp.getheaders()}
            self.responses.append((label or f"{method} {path}", resp.status, headers_out))
            return resp.status, headers_out, raw
        finally:
            conn.close()

    def json_of(self, raw):
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return None

    # -- Assertions ---------------------------------------------------------------------

    def check(self, ok, name, detail=""):
        if ok:
            self.passes += 1
            print(f"PASS  {name}")
        else:
            self.failures.append(f"{name}: {detail}")
            print(f"FAIL  {name}: {detail}")
        return ok

    def expect_status(self, name, got, want, raw=b""):
        return self.check(got == want, name, f"status {got}, want {want}, body {raw[:200]!r}")


def scan_cycle(c, n):
    """Waits for a fresh scan (one started after the previous result) to finish."""
    t0 = time.time()
    first_age = None
    scan = None
    st, raw = 0, b""
    started_new = False
    while time.time() - t0 < 40:
        try:
            st, _, raw = c.request("GET", "/api/wifi/scan", label=f"scan {n}")
        except (OSError, http.client.HTTPException) as e:
            c.check(False, f"scan {n} request", repr(e))
            return
        scan = c.json_of(raw)
        if st != 200 or not isinstance(scan, dict):
            break
        if first_age is None:
            first_age = scan.get("age_s")
        if scan.get("scanning"):
            started_new = True
        elif started_new:
            break
        time.sleep(1)
    c.expect_status(f"scan {n}: 200", st, 200, raw)
    ok = isinstance(scan, dict) and started_new and not scan.get("scanning")
    c.check(ok, f"scan {n}: a new scan ran and finished within 40 s", str(scan)[:200])
    if isinstance(scan, dict):
        nets = scan.get("networks") or []
        c.check(scan.get("failed") is False, f"scan {n}: failed false", str(scan)[:200])
        c.check(len(nets) > 0, f"scan {n}: networks > 0", len(nets))
        print(f"      scan {n}: {time.time() - t0:.1f} s, failed={scan.get('failed')} "
              f"networks={len(nets)} age_s={scan.get('age_s')}")


def post_headers(extra=None):
    h = {"Content-Type": "application/json", "X-HAKnob": "1"}
    h.update(extra or {})
    return h


def basic(password):
    return "Basic " + base64.b64encode(f"admin:{password}".encode()).decode()


def run(args):
    c = Checker(args.ip)
    hostname = args.hostname

    # 1. Status
    st, h, raw = c.request("GET", "/api/status")
    c.expect_status("GET /api/status (Host=ip) 200", st, 200, raw)
    status = c.json_of(raw) or {}
    c.check(h.get("content-type", "").startswith("application/json"), "status is JSON",
            h.get("content-type"))
    wifi = status.get("wifi", {})
    c.check(wifi.get("mode") in ("sta", "ap_sta"), "wifi.mode sta/ap_sta", wifi.get("mode"))
    print(f"      wifi.mode={wifi.get('mode')} connected={wifi.get('connected')} "
          f"ip={wifi.get('ip')} ap_active={wifi.get('ap_active')} fw={status.get('fw')}")
    c.check(wifi.get("ip") == args.ip, "wifi.ip is the device IP", wifi.get("ip"))
    for key in ("fw", "uptime_s", "heap_free", "psram_free", "touch_variant", "ha"):
        c.check(key in status, f"status has {key}", list(status.keys()))
    for host in (f"{hostname}.local", hostname, f"{hostname.upper()}.LOCAL:80", f"{args.ip}:80"):
        st, _, raw = c.request("GET", "/api/status", host=host)
        c.expect_status(f"GET /api/status Host={host} 200", st, 200, raw)

    # 2. Config redaction
    st, h, raw = c.request("GET", "/api/config")
    c.expect_status("GET /api/config 200", st, 200, raw)
    cfg = c.json_of(raw) or {}
    c.check(cfg.get("wifi", {}).get("password_set") is True, "wifi.password_set true",
            cfg.get("wifi"))
    c.check(isinstance(cfg.get("ha", {}).get("token_set"), bool), "ha.token_set bool", cfg.get("ha"))
    c.check(isinstance(cfg.get("web", {}).get("password_set"), bool), "web.password_set bool",
            cfg.get("web"))
    secret_keys = []

    def walk(o, path=""):
        if isinstance(o, dict):
            for k, v in o.items():
                p = f"{path}.{k}" if path else k
                lk = k.lower()
                if any(s in lk for s in ("pass", "psk", "token", "secret", "ap_pw", "key")) \
                        and not lk.endswith("_set"):
                    secret_keys.append(p)
                walk(v, p)
        elif isinstance(o, list):
            for i, v in enumerate(o):
                walk(v, f"{path}[{i}]")

    walk(cfg)
    c.check(not secret_keys, "config has no secret-like keys besides *_set", secret_keys)
    text = raw.decode("utf-8", "replace").lower()
    c.check("ap_pw" not in text and "psk" not in text, "config has no ap_pw/psk", "")
    device_section = cfg.get("device")
    c.check(isinstance(device_section, dict), "config has device section", cfg.keys())

    # 3. POST without X-HAKnob
    st, _, raw = c.request("POST", "/api/config", body="{}",
                           headers={"Content-Type": "application/json"})
    c.expect_status("POST without X-HAKnob 403", st, 403, raw)

    # 4. Foreign Host
    st, _, raw = c.request("POST", "/api/config", body="{}", headers=post_headers(),
                           host="evil.example")
    c.expect_status("POST Host=evil.example 403", st, 403, raw)
    st, _, raw = c.request("GET", "/api/config", host="evil.example")
    c.expect_status("GET /api/config Host=evil.example 403", st, 403, raw)
    st, _, raw = c.request("GET", "/", host="evil.example")
    c.check(st in (403,), "GET / Host=evil.example 403 (AP down)", f"status {st}")

    # 5. Invalid hostname -> 400 with field, nothing applied
    if isinstance(device_section, dict):
        bad = dict(device_section)
        bad["hostname"] = "Bad_Host!"
        st, _, raw = c.request("POST", "/api/config", body=json.dumps({"device": bad}),
                               headers=post_headers())
        c.expect_status("POST invalid hostname 400", st, 400, raw)
        body = c.json_of(raw) or {}
        c.check(body.get("field") == "device.hostname", "400 has field device.hostname", body)
        c.check("error" in body, "400 has error", body)
        st, _, raw = c.request("GET", "/api/config")
        after = (c.json_of(raw) or {}).get("device")
        c.check(after == device_section, "device section unchanged after rejected patch",
                f"{after} vs {device_section}")

    # 6. OPTIONS / PUT
    st, _, raw = c.request("OPTIONS", "/api/config",
                           headers={"Origin": "http://evil.example",
                                    "Access-Control-Request-Method": "POST"})
    c.expect_status("OPTIONS 405", st, 405, raw)
    st, _, raw = c.request("PUT", "/api/config", body="{}", headers=post_headers())
    c.expect_status("PUT 405", st, 405, raw)
    st, h, raw = c.request("GET", "/api/status", headers={"Origin": "http://evil.example"})
    c.expect_status("GET with Origin 200", st, 200, raw)

    # 8. Static files
    static = {"/": ("text/html",), "/index.html": ("text/html",),
              "/app.js": ("application/javascript", "text/javascript"),
              "/style.css": ("text/css",)}
    for path, types in static.items():
        st, h, raw = c.request("GET", path)
        c.expect_status(f"GET {path} 200", st, 200, raw)
        ct = h.get("content-type", "")
        c.check(any(ct.startswith(t) for t in types), f"{path} content-type", ct)
        c.check(len(raw) > 0, f"{path} non-empty", len(raw))

    # 11. Body limits / content type
    big = "{\"device\":{\"hostname\":\"" + "a" * 17000 + "\"}}"
    try:
        st, _, raw = c.request("POST", "/api/config", body=big, headers=post_headers())
        c.expect_status("POST > 16 KB 413", st, 413, raw)
    except (OSError, http.client.HTTPException) as e:
        c.check(False, "POST > 16 KB 413", f"exception {e!r}")
    exact = b"{" + b" " * (16384 - 2) + b"}"  # exactly 16 KB, valid empty object
    st, _, raw = c.request("POST", "/api/config", body=exact, headers=post_headers())
    c.check(st != 413, "POST exactly 16 KB not 413", f"status {st} {raw[:120]!r}")
    st, _, raw = c.request("POST", "/api/config", body="{}", headers=post_headers(), chunked=True)
    c.expect_status("chunked POST 411", st, 411, raw)
    st, _, raw = c.request("POST", "/api/config", body="{}",
                           headers={"Content-Type": "text/plain", "X-HAKnob": "1"})
    c.expect_status("POST text/plain 403", st, 403, raw)
    st, _, raw = c.request("POST", "/api/config", body="{}",
                           headers={"Content-Type": "application/x-www-form-urlencoded",
                                    "X-HAKnob": "1"})
    c.expect_status("POST form-urlencoded 403", st, 403, raw)
    st, _, raw = c.request("POST", "/api/config", body="{}",
                           headers=post_headers({"X-HAKnob": "0"}))
    c.expect_status("POST X-HAKnob: 0 403", st, 403, raw)
    st, _, raw = c.request("POST", "/api/config", body="not json", headers=post_headers())
    c.expect_status("POST malformed JSON 400", st, 400, raw)

    # 12. Unknown API path, Wi-Fi scan
    st, h, raw = c.request("GET", "/api/x")
    c.expect_status("GET /api/x 404", st, 404, raw)
    c.check(c.json_of(raw) is not None, "404 body is JSON", raw[:100])
    for path in ("/nope", "/api/nope/x", "/INDEX.HTML", "/App.js", "/../index.html",
                 "/%2e%2e/index.html", "/%2E%2E/index.html", "//index.html", "/./index.html",
                 "/index.html/", "/data/index.html", "/.git/config"):
        st, h, raw = c.request("GET", path)
        c.expect_status(f"GET {path} 404", st, 404, raw)
    # Informational: a %00 suffix is decoded and truncated by the server to the listed file
    # (same file, no traversal). Reported, not asserted.
    st, _, _ = c.request("GET", "/index.html%00")
    print(f"      info: GET /index.html%00 -> {st}")
    for i in range(args.scan_cycles):
        scan_cycle(c, i + 1)

    # 10. Web password
    if args.web_password_test:
        pw = "t3st-" + secrets.token_hex(6)
        print(f"      test web password: {pw}")
        try:
            st, _, raw = c.request("POST", "/api/config",
                                   body=json.dumps({"web": {"password": pw}}), headers=post_headers())
            c.expect_status("set web password 200", st, 200, raw)
            time.sleep(0.5)
            for path in ("/", "/api/status", "/api/config", "/app.js"):
                st, h, raw = c.request("GET", path, label=f"GET {path} unauth")
                c.expect_status(f"GET {path} without auth 401", st, 401, raw)
                c.check(pw not in raw.decode("utf-8", "replace"), f"401 body of {path} has no password")
            st, h, raw = c.request("GET", "/api/status", headers={"Authorization": basic("wrong")})
            c.expect_status("GET /api/status wrong password 401", st, 401, raw)
            st, h, raw = c.request("POST", "/api/config", body="{}", headers=post_headers())
            c.expect_status("POST without auth 401", st, 401, raw)
            for path in ("/", "/api/status"):
                st, h, raw = c.request("GET", path, headers={"Authorization": basic(pw)})
                c.expect_status(f"GET {path} with auth 200", st, 200, raw)
            st, _, raw = c.request("GET", "/api/config", headers={"Authorization": basic(pw)})
            cfg2 = c.json_of(raw) or {}
            c.check(cfg2.get("web", {}).get("password_set") is True, "web.password_set true")
            c.check(pw not in raw.decode("utf-8", "replace"), "config does not echo web password")
        finally:
            st, _, raw = c.request("POST", "/api/config", body='{"web":{"password":null}}',
                                   headers=post_headers({"Authorization": basic(pw)}))
            if st == 401:  # password was never set; clear without auth
                st, _, raw = c.request("POST", "/api/config", body='{"web":{"password":null}}',
                                       headers=post_headers())
            c.expect_status("clear web password 200", st, 200, raw)
        time.sleep(0.5)
        st, _, raw = c.request("GET", "/api/status")
        c.expect_status("GET /api/status after clear 200 (no auth)", st, 200, raw)
        st, _, raw = c.request("GET", "/api/config")
        c.check((c.json_of(raw) or {}).get("web", {}).get("password_set") is False,
                "web.password_set false at the end", raw[:200])

    # 9 + 6. Headers on every collected response
    required = {
        "content-security-policy": None,
        "x-frame-options": "DENY",
        "x-content-type-options": "nosniff",
        "referrer-policy": None,
    }
    missing = []
    cors = []
    for label, st, h in c.responses:
        for k, v in required.items():
            if k not in h or (v is not None and h[k].strip().upper() != v.upper()):
                missing.append(f"{label} [{st}] {k}={h.get(k)}")
        for k in h:
            if k.startswith("access-control-"):
                cors.append(f"{label} [{st}] {k}")
    c.check(not missing, f"security headers on all {len(c.responses)} responses", missing[:10])
    c.check(not cors, f"no Access-Control-* on any of {len(c.responses)} responses", cors[:10])

    # 13. Reboot
    if args.reboot:
        st, _, raw = c.request("POST", "/api/reboot", body="{}", headers=post_headers())
        c.expect_status("POST /api/reboot 200", st, 200, raw)
        t0 = time.time()
        went_down = False
        back = None
        first_ip_ok = None
        while time.time() - t0 < 120:
            time.sleep(1)
            try:
                st, _, raw = c.request("GET", "/api/status", host=f"{hostname}.local")
                s = c.json_of(raw) or {}
                if went_down or s.get("uptime_s", 10**9) < 30:
                    back = (time.time() - t0, s)
                    break
            except (OSError, http.client.HTTPException):
                went_down = True
        c.check(back is not None, "device back after reboot", "no answer within 120 s")
        if back:
            s = back[1]
            print(f"      back after {back[0]:.1f} s, uptime_s={s.get('uptime_s')} "
                  f"wifi={s.get('wifi')}")
            for _ in range(30):
                st, _, raw = c.request("GET", "/api/status")
                if st == 200:
                    first_ip_ok = time.time() - t0
                    break
                time.sleep(1)
            c.check(first_ip_ok is not None, "Host=ip accepted again after reboot",
                    "still not 200 after 30 s")
            if first_ip_ok:
                print(f"      Host=ip accepted {first_ip_ok:.1f} s after the reboot request")

    print(f"\n{c.passes} passed, {len(c.failures)} failed")
    for f in c.failures:
        print(f"  FAILED: {f}")
    return 0 if not c.failures else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ip", required=True)
    p.add_argument("--hostname", default="ha-knob")
    p.add_argument("--reboot", action="store_true", help="also test POST /api/reboot")
    p.add_argument("--scan-cycles", type=int, default=1,
                   help="number of complete Wi-Fi scan cycles to require (default 1)")
    p.add_argument("--web-password-test", action="store_true",
                   help="set a random web password, test 401/200, then clear it")
    args = p.parse_args()
    socket.setdefaulttimeout(TIMEOUT)
    sys.exit(run(args))


if __name__ == "__main__":
    main()
