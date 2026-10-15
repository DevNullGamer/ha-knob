#!/usr/bin/env python3
"""LAN HTTP checks against a running HA Knob (plan.md §3.8, §4 Step 4).

Usage:
    python tools/http_check.py --ip 192.168.1.143 [--hostname ha-knob] [--reboot]
                               [--web-password-test]
                               [--ha-test --mock-python PY --mock-host PC_IP
                                [--mock-port 8765] [--serial-log FILE]]

Only uses the Python standard library. Every request uses the device IP as the Host
header unless stated otherwise, like a browser opening http://<ip>/.

Step 5 (always): the status "ha" block, POST /api/ha/retry (200 / 403 without the CSRF
header / 404 on GET).

--ha-test (Step 5; changes the ha section and always restores it to url "" / token null):
runs tools/mockha.py with --mock-python (a Python the Windows firewall lets accept LAN
connections) and checks https -> no_config/tls_not_supported, ready + ha_version, wrong
token -> auth_failed with exactly one connect attempt, no pong -> reconnect, mock killed ->
backoff -> recovery, and that no token value reaches /api/config, /api/status or the
serial log. --serial-log FILE is a file a background serial logger appends to (lines of
"<epoch> <text>"); it is used to count "HA connect attempt" lines and to grep for tokens.

Safety: without --ha-test never touches the wifi or ha sections. The only state-changing requests are
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
import os
import secrets
import socket
import subprocess
import sys
import tempfile
import time

TIMEOUT = 8
HA_STATES = ("no_config", "connecting", "authenticating", "subscribing", "ready", "auth_failed",
             "backoff")


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


class SerialLog:
    """Read access to the file a background serial logger appends to."""

    def __init__(self, path):
        self.path = path

    def mark(self):
        try:
            return os.path.getsize(self.path)
        except OSError:
            return 0

    def since(self, mark):
        try:
            with open(self.path, "rb") as f:
                f.seek(mark)
                return f.read().decode("utf-8", "replace").splitlines()
        except OSError:
            return []


def ha_status(c):
    st, _, raw = c.request("GET", "/api/status")
    return (c.json_of(raw) or {}).get("ha", {}) if st == 200 else {}


def wait_ha(c, want_state, timeout, error_contains=None):
    """Polls /api/status until ha.state == want_state (and last_error contains the text)."""
    t0 = time.time()
    h = {}
    while time.time() - t0 < timeout:
        try:
            h = ha_status(c)
        except (OSError, http.client.HTTPException):
            h = {}
        if h.get("state") == want_state and (error_contains is None
                                             or error_contains in h.get("last_error", "")):
            return h, time.time() - t0
        time.sleep(0.5)
    return h, None


def set_ha(c, url, token):
    body = json.dumps({"ha": {"url": url, "token": token}})
    return c.request("POST", "/api/config", body=body, headers=post_headers())


def run_ha_test(c, args):
    print("\n--- Step 5 on-device HA tests (ha section is restored at the end) ---")
    slog = SerialLog(args.serial_log) if args.serial_log else None
    root = os.path.dirname(os.path.abspath(__file__))
    mock_path = os.path.join(root, "mockha.py")
    good = "mock-good-token-" + secrets.token_hex(4)
    wrong = "wrong-token-" + secrets.token_hex(4)
    fake_https = "fake-https-token-" + secrets.token_hex(4)
    mock_url = f"http://{args.mock_host}:{args.mock_port}"
    mock = {"proc": None, "log": None}

    def stop_mock():
        if mock["proc"] is not None:
            mock["proc"].kill()
            mock["proc"].wait()
            mock["proc"] = None
            time.sleep(0.3)

    def start_mock(extra=()):
        stop_mock()
        mock["log"] = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        mock["proc"] = subprocess.Popen(
            [args.mock_python, mock_path, "--port", str(args.mock_port), "--token", good,
             "--seconds", "600", *extra], stdout=mock["log"], stderr=subprocess.STDOUT)
        time.sleep(1.5)

    def mock_lines():
        if mock["log"] is None:
            return []
        mock["log"].flush()
        with open(mock["log"].name, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()

    def attempts(mark):
        if slog is None:
            return None
        return sum(1 for l in slog.since(mark) if "HA connect attempt" in l)

    secrets_seen = [good, wrong, fake_https]
    try:
        # A. https: refused, no connection attempt
        mark = slog.mark() if slog else 0
        st, _, raw = set_ha(c, "https://ha.example.com", fake_https)
        c.expect_status("A: set https url + fake token 200", st, 200, raw)
        h, _ = wait_ha(c, "no_config", 10, "tls_not_supported")
        c.check(h.get("state") == "no_config" and "tls_not_supported" in h.get("last_error", ""),
                "A: https -> no_config + tls_not_supported", h)
        time.sleep(5)
        h = ha_status(c)
        c.check(h.get("state") == "no_config", "A: still no_config after 5 s", h)
        n = attempts(mark)
        if n is not None:
            c.check(n == 0, "A: no 'HA connect attempt' on serial for https", n)
        st, _, raw = c.request("GET", "/api/config")
        cfgj = c.json_of(raw) or {}
        c.check(fake_https not in raw.decode("utf-8", "replace"),
                "A: /api/config does not contain the token")
        c.check(cfgj.get("ha", {}).get("token_set") is True
                and cfgj.get("ha", {}).get("url") == "https://ha.example.com",
                "A: config ha.token_set true, url kept", cfgj.get("ha"))
        st, _, raw = c.request("GET", "/api/status")
        c.check(fake_https not in raw.decode("utf-8", "replace"),
                "A: /api/status does not contain the token")
        st, _, raw = c.request("POST", "/api/ha/retry", body="{}", headers=post_headers())
        c.expect_status("A: retry in no_config still 200", st, 200, raw)
        time.sleep(2)
        c.check(ha_status(c).get("state") == "no_config", "A: retry does not leave no_config")
        if slog:
            c.check(attempts(mark) == 0, "A: still no attempts after retry")

        # B. mock HA: ready + version
        start_mock()
        mark = slog.mark() if slog else 0
        st, _, raw = set_ha(c, mock_url, good)
        c.expect_status("B: set mock url + good token 200", st, 200, raw)
        h, took = wait_ha(c, "ready", 25)
        c.check(h.get("state") == "ready", "B: ready", h)
        c.check(h.get("ha_version") == "2099.1.0-mock", "B: ha_version from auth_ok", h)
        c.check(h.get("last_error") == "", "B: last_error empty when ready", h)
        if took is not None:
            print(f"      ready after {took:.1f} s")
        if slog:
            c.check(attempts(mark) == 1, "B: exactly one connect attempt", attempts(mark))
        st, _, raw = c.request("GET", "/api/config")
        c.check(good not in raw.decode("utf-8", "replace"), "B: /api/config does not contain the token")

        # C. wrong token -> auth_failed, exactly one attempt
        mark = slog.mark() if slog else 0
        st, _, raw = set_ha(c, mock_url, wrong)
        c.expect_status("C: set wrong token 200", st, 200, raw)
        h, _ = wait_ha(c, "auth_failed", 25)
        c.check(h.get("state") == "auth_failed", "C: auth_failed", h)
        c.check("auth_invalid" in h.get("last_error", ""), "C: last_error mentions auth_invalid", h)
        time.sleep(40)  # longer than any backoff step: must not reconnect
        h = ha_status(c)
        c.check(h.get("state") == "auth_failed", "C: still auth_failed after 40 s", h)
        n = attempts(mark)
        if n is not None:
            c.check(n == 1, "C: exactly one 'HA connect attempt' line", n)
        c.check(any("REQ GET" in l for l in mock_lines()), "C: mock saw the connection")
        # retry leaves auth_failed (wrong token again -> back to auth_failed)
        mark = slog.mark() if slog else 0
        st, _, raw = c.request("POST", "/api/ha/retry", body="{}", headers=post_headers())
        c.expect_status("C: retry 200", st, 200, raw)
        time.sleep(4)
        n = attempts(mark)
        if n is not None:
            c.check(n == 1, "C: retry makes exactly one new attempt", n)
        c.check(ha_status(c).get("state") == "auth_failed", "C: auth_failed again after retry")

        # D. good token again
        st, _, raw = set_ha(c, mock_url, good)
        c.expect_status("D: set good token 200", st, 200, raw)
        h, _ = wait_ha(c, "ready", 25)
        c.check(h.get("state") == "ready", "D: ready with the new token", h)

        # E. mock killed -> backoff -> recovery
        stop_mock()
        h, _ = wait_ha(c, "backoff", 20)
        c.check(h.get("state") == "backoff", "E: backoff after the mock is killed", h)
        time.sleep(8)
        h = ha_status(c)
        c.check(h.get("state") in ("backoff", "connecting"), "E: still retrying while mock is down", h)
        start_mock()
        h, took = wait_ha(c, "ready", 60)
        c.check(h.get("state") == "ready", "E: recovered to ready after the mock restarted", h)
        if took is not None:
            print(f"      recovered {took:.1f} s after restart")

        # F. mock stops answering pings -> reconnect
        mark = slog.mark() if slog else 0
        start_mock(("--no-pong",))  # replaces the running mock: the connection drops
        h, _ = wait_ha(c, "ready", 60)
        c.check(h.get("state") == "ready", "F: ready against the no-pong mock", h)
        h, took = wait_ha(c, "backoff", 60, "pong_timeout")
        c.check(h.get("state") == "backoff" and "pong_timeout" in h.get("last_error", ""),
                "F: missing pong -> backoff with pong_timeout (within 60 s)", h)
        if took is not None:
            print(f"      pong_timeout after {took:.1f} s")
        h, _ = wait_ha(c, "ready", 30)
        c.check(h.get("state") == "ready", "F: reconnected after pong_timeout", h)
        if slog:
            c.check(any("pong_timeout" in l for l in slog.since(mark)), "F: pong_timeout in serial log")
    finally:
        stop_mock()
        st, _, raw = set_ha(c, "", None)
        c.expect_status("restore: ha url '' + token null 200", st, 200, raw)
        h, _ = wait_ha(c, "no_config", 15)
        c.check(h.get("state") == "no_config", "restore: ha.state no_config", h)
        st, _, raw = c.request("GET", "/api/config")
        hc = (c.json_of(raw) or {}).get("ha", {})
        c.check(hc.get("url") == "" and hc.get("token_set") is False,
                "restore: ha url '' and token_set false", hc)
        if slog:
            text = "\n".join(slog.since(0))
            for sec in secrets_seen:
                c.check(sec not in text, f"serial log never contains the token value ({sec[:12]}...)")
            c.check("access_token" not in text, "serial log never contains access_token")


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
    ha_block = status.get("ha")
    if c.check(isinstance(ha_block, dict), "status.ha is an object", ha_block):
        c.check(set(ha_block.keys()) >= {"state", "ha_version", "last_error"},
                "status.ha has state, ha_version, last_error", ha_block)
        c.check(ha_block.get("state") in HA_STATES, "status.ha.state is a known state",
                ha_block.get("state"))
        c.check(isinstance(ha_block.get("ha_version"), str) and isinstance(ha_block.get("last_error"), str),
                "status.ha.ha_version / last_error are strings", ha_block)
        c.check("token" not in json.dumps(ha_block).lower(), "status.ha has no token", ha_block)
        print(f"      ha={ha_block}")
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

    # Step 5: POST /api/ha/retry
    st, _, raw = c.request("POST", "/api/ha/retry", body="{}", headers=post_headers())
    c.expect_status("POST /api/ha/retry 200", st, 200, raw)
    c.check((c.json_of(raw) or {}).get("ok") is True, "retry body ok:true", raw[:100])
    st, _, raw = c.request("POST", "/api/ha/retry", body="{}",
                           headers={"Content-Type": "application/json"})
    c.expect_status("POST /api/ha/retry without X-HAKnob 403", st, 403, raw)
    st, _, raw = c.request("POST", "/api/ha/retry", body="{}", headers=post_headers(),
                           host="evil.example")
    c.expect_status("POST /api/ha/retry Host=evil.example 403", st, 403, raw)
    st, _, raw = c.request("GET", "/api/ha/retry")
    c.expect_status("GET /api/ha/retry 404", st, 404, raw)

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

    if args.ha_test:
        run_ha_test(c, args)

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
    p.add_argument("--ha-test", action="store_true",
                   help="Step 5 on-device tests against tools/mockha.py (restores the ha section)")
    p.add_argument("--mock-python", help="Python executable that may accept LAN connections")
    p.add_argument("--mock-host", help="LAN IP of this PC as seen from the device")
    p.add_argument("--mock-port", type=int, default=8765)
    p.add_argument("--serial-log", help="file appended to by a background serial logger")
    args = p.parse_args()
    if args.ha_test and not (args.mock_python and args.mock_host):
        p.error("--ha-test needs --mock-python and --mock-host")
    socket.setdefaulttimeout(TIMEOUT)
    sys.exit(run(args))


if __name__ == "__main__":
    main()
