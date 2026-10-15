#!/usr/bin/env python3
"""LAN HTTP checks against a running HA Knob (plan.md §3.8, §4 Step 4).

Usage:
    python tools/http_check.py --ip 192.168.1.143 [--hostname ha-knob] [--reboot]
                               [--web-password-test] [--tls-config-test]
                               [--ha-test --mock-python PY --mock-host PC_IP
                                [--mock-port 8765] [--serial-log FILE]]
                               [--ws-test --mock-python PY --mock-host PC_IP --serial-log FILE]
                               [--tls-test --cert-dir DIR --mock-python PY --mock-host PC_IP
                                --serial-log FILE]

Only uses the Python standard library. Every request uses the device IP as the Host
header unless stated otherwise, like a browser opening http://<ip>/.

Step 5 (always): the status "ha" block, POST /api/ha/retry (200 / 403 without the CSRF
header / 404 on GET).

--tls-config-test (Step 5b.3; changes the ha section with a fake token and always restores it to
url "" / no CA / token null): /api/config shows ca_pem and tls_insecure and never a token; 400
{error, field} for requires_https, not_a_certificate, conflicts_with_ca_pem, not_a_string,
not_a_boolean, invalid_der and token_required (TLS weakening on an https url with a token
stored); a valid https + CA + token saves and is returned normalised; tightening keeps the token.

--ha-test (Step 5; changes the ha section and always restores it to url "" / token null):
runs tools/mockha.py with --mock-python (a Python the Windows firewall lets accept LAN
connections) and checks https -> connects (nothing listening: connect_failed/backoff, ha.tls bundle), ready + ha_version, wrong
token -> auth_failed with exactly one connect attempt, no pong -> reconnect, mock killed ->
backoff -> recovery, and that no token value reaches /api/config, /api/status or the
serial log. --serial-log FILE is a file a background serial logger appends to (lines of
"<epoch> <text>"); it is used to count "HA connect attempt" lines and to grep for tokens.

--ws-test (Step 5b.2; same ha restore as --ha-test; needs --serial-log): runs mockha.py
scenarios against the own WebSocket transport: ready + heap numbers, HB heap_min=/loopmax=
and /api/status heap_min_free, 1 MB and 3 MB messages after auth (HA rx len= ms=, state stays
ready, HB continues, >= 300 KB/s, PSRAM recovers), 5 MB dropped (reason=too_large) and still
ready ~40 s later, fragmented auth_ok, ws PING answered with the same payload, Close 1001 ->
closed_by_server + reconnect, Close without code, upgrade 502 -> handshake_failed: http 502,
coalesced handshake, close before 101 -> handshake_failed: closed, mock killed / stalled
mid-3 MB (connection_lost, PSRAM freed, no reboot). Prints the measured numbers at the end.

--tls-test (Step 5b.4; same ha restore; needs --cert-dir from tools/gen_test_certs.sh and
--serial-log written by tools/serial_log.py): wss against tools/mockha.py with throwaway certs:
cn-ip + CA-A ready (custom_ca, handshake <= 1500 ms, TLS 1.2, ws PING/PONG, Close 1001),
wrong CA / bundle mode / IP-SAN-only leaf -> tls_cert_rejected with no WebSocket request, insecure
ready + "WARN HA tls verify=off", self-signed leaf as CA ready, https at a plain HTTP server and a
TLS 1.3-only server -> tls_handshake_failed, stalled handshake -> tls_timeout (loopmax <= 6.5 s,
backoff continues), unparsable CA -> tls_ca_invalid without TCP connect, 3 MB over TLS, heap gate
with a ~4 KB CA bundle, 20 kill/restart cycles (no leak, heap_min >= 70 KB), no maxlength on the
CA textarea and > 4096 chars -> 400 length. Prints the measured numbers at the end.

Safety: without --ha-test/--ws-test/--tls-test never touches the wifi or ha sections. The only state-changing requests are
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


# Throwaway public test certificate (RSA-2048 self-signed, no key); also in
# test/native/test_config_tls/test_certs.h.
TLS_TEST_CA = """-----BEGIN CERTIFICATE-----
MIIDETCCAfmgAwIBAgIUDpzdr37XTTj3C6/ahUAwRlXcM48wDQYJKoZIhvcNAQEL
BQAwGDEWMBQGA1UEAwwNVGVzdCBSU0EgUm9vdDAeFw0yNjEwMTUxODI1MTZaFw0z
NjEwMTIxODI1MTZaMBgxFjAUBgNVBAMMDVRlc3QgUlNBIFJvb3QwggEiMA0GCSqG
SIb3DQEBAQUAA4IBDwAwggEKAoIBAQC2mzSXgaopTusx389VWeJiuv5E1EKnALu3
u7VUr/Tp2FYRyV449gxohLiprmD6hKBuJEsNAOZAefiPi2Y7kXIEFjep6XdzkhXp
uzMbAr8WG0Sm7P+58U1vFgOR5DZd3fvf6uxiExQHRPUcS67YxxTSn0d5np0WRpf0
eXS0unovQAYfC58cKpCXIqt/NXfcqngEL4GCb4oCl+uiXmRcO2V3Ei1MpFzRP1ik
4IcdRjvxdyjy78g3YT9oG3VSXR3CGvD0b8Jayy4pBhpEUinXfjzOukZ6mth9tPMa
3aW+h/wye5VWia5/iPON1jbHmH7ObU5UCZjUnIrT3r4UgP0wKvf9AgMBAAGjUzBR
MB0GA1UdDgQWBBT+8kBFBdF7OQ3qL6w4LKpHdvGx0jAfBgNVHSMEGDAWgBT+8kBF
BdF7OQ3qL6w4LKpHdvGx0jAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUA
A4IBAQAAFM2gAU/uq7Hxnyb/xXtBg034VYIsW8mZuW0sHjgdMQVmKKo9vu8zEhMv
Bp5SqwlaYtSmhmjZquRzT8uD2PH1vDqZYrhihMmPmneC0DmoYa7lAnfh5oq6DTyM
qXgplPD/ATfRjQNUn2iqe0QGbot84v6VDzaEV6GDtc6MTEV6fAlrJQ958CrbOFH8
ahgAucdGzAoTAQzKoIV8St46rzrUJyciKx4WA8WXRo8g1vXyyU4a6q/plsn34CfZ
NNmjD5OwsO0oMzWYz06ZtU9mwH5VjNHplT3qSZNpSXlcKHdqMr5P5HA9rrX+nwoh
arP8Z30Fpkin39mlKbxmze8tgtL3
-----END CERTIFICATE-----
"""


def run_tls_config_test(c):
    """Step 5b.3: TLS settings in /api/config (no connect). Restores ha to no config."""
    print("\n--- Step 5b.3 TLS config API tests (ha section is restored at the end) ---")
    fake = "faketoken-tls-test-0123456789"
    url = "https://tls-test.invalid:8443"
    ca = TLS_TEST_CA

    def post_ha(ha):
        st, _, raw = c.request("POST", "/api/config", body=json.dumps({"ha": ha}),
                               headers=post_headers())
        return st, raw, (c.json_of(raw) or {})

    def get_ha():
        st, _, raw = c.request("GET", "/api/config")
        return st, raw, ((c.json_of(raw) or {}).get("ha", {}))

    def rejected(name, ha, field, error):
        st, raw, body = post_ha(ha)
        c.check(st == 400 and body.get("field") == field and body.get("error") == error,
                name, f"status {st}, body {raw[:200]!r}, want {field}/{error}")

    try:
        st, raw, ha = get_ha()
        c.expect_status("T: GET /api/config 200", st, 200, raw)
        c.check(isinstance(ha.get("ca_pem"), str), "T: ha.ca_pem is a string", ha)
        c.check(isinstance(ha.get("tls_insecure"), bool), "T: ha.tls_insecure is a bool", ha)
        c.check("token" not in ha, "T: ha has no token key", ha)

        # Clean start, then store a fake token with an https URL (no TLS settings).
        st, raw, body = post_ha({"url": "", "ca_pem": "", "tls_insecure": False, "token": None})
        c.expect_status("T: reset to no config", st, 200, raw)
        st, raw, body = post_ha({"url": url, "ca_pem": "", "tls_insecure": False, "token": fake})
        c.expect_status("T: https url + token saved", st, 200, raw)
        st, raw, ha = get_ha()
        c.check(ha.get("url") == url and ha.get("token_set") is True and ha.get("ca_pem") == ""
                and ha.get("tls_insecure") is False, "T: GET shows https url, token_set, default TLS", ha)
        c.check(fake not in raw.decode("utf-8", "replace"), "T: GET /api/config has no token value")

        # Validation errors -> 400 {error, field}
        rejected("T: http url + CA -> requires_https", {"url": "http://tls-test.invalid:8123", "ca_pem": ca,
                                                         "tls_insecure": False, "token": fake},
                 "ha.ca_pem", "requires_https")
        rejected("T: http url + insecure -> requires_https", {"url": "http://tls-test.invalid:8123",
                                                               "ca_pem": "", "tls_insecure": True, "token": fake},
                 "ha.tls_insecure", "requires_https")
        rejected("T: empty url + insecure -> requires_https", {"url": "", "ca_pem": "", "tls_insecure": True},
                 "ha.tls_insecure", "requires_https")
        rejected("T: private key -> not_a_certificate", {"url": url, "ca_pem": "-----BEGIN PRIVATE KEY-----\nAAAA\n"
                                                         "-----END PRIVATE KEY-----\n", "tls_insecure": False},
                 "ha.ca_pem", "not_a_certificate")
        rejected("T: cert + key -> not_a_certificate", {"url": url, "ca_pem": ca + "-----BEGIN PRIVATE KEY-----\n"
                                                        "AAAA\n-----END PRIVATE KEY-----\n", "tls_insecure": False},
                 "ha.ca_pem", "not_a_certificate")
        rejected("T: CA + insecure -> conflicts_with_ca_pem", {"url": url, "ca_pem": ca, "tls_insecure": True,
                                                                "token": fake},
                 "ha.tls_insecure", "conflicts_with_ca_pem")
        rejected("T: ca_pem number -> not_a_string", {"url": url, "ca_pem": 5}, "ha.ca_pem", "not_a_string")
        rejected("T: tls_insecure string -> not_a_boolean", {"url": url, "tls_insecure": "yes"},
                 "ha.tls_insecure", "not_a_boolean")
        rejected("T: truncated cert -> invalid_der or invalid_base64",
                 {"url": url, "ca_pem": "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n",
                  "tls_insecure": False}, "ha.ca_pem", "invalid_der")
        # Weakening on the https url with the fake token stored and no new token -> token_required.
        rejected("T: insecure on without token -> token_required", {"url": url, "ca_pem": "", "tls_insecure": True},
                 "ha.token", "token_required")
        rejected("T: CA added without token -> token_required", {"url": url, "ca_pem": ca, "tls_insecure": False},
                 "ha.token", "token_required")
        rejected("T: CA added with empty token -> token_required", {"url": url, "ca_pem": ca, "tls_insecure": False,
                                                                     "token": ""}, "ha.token", "token_required")
        st, raw, ha = get_ha()
        c.check(ha.get("ca_pem") == "" and ha.get("tls_insecure") is False and ha.get("token_set") is True,
                "T: rejected patches changed nothing", ha)

        # Valid https + CA + new fake token saves and comes back normalised.
        messy = "\r\n" + ca.replace("\n", " \t\r\n") + "\r\n\r\n"
        st, raw, body = post_ha({"url": url, "ca_pem": messy, "tls_insecure": False, "token": fake + "2"})
        c.expect_status("T: https + CA (CRLF, trailing blanks) + token saved", st, 200, raw)
        st, raw, ha = get_ha()
        c.check(ha.get("ca_pem") == ca, "T: ca_pem returned normalised (LF, one trailing LF)",
                repr(ha.get("ca_pem"))[:120])
        c.check(ha.get("tls_insecure") is False and ha.get("token_set") is True, "T: tls_insecure false, token_set", ha)
        c.check(fake not in raw.decode("utf-8", "replace"), "T: GET /api/config has no token value (2)")

        # Same CA resent in another spelling without a token: accepted, token kept.
        st, raw, body = post_ha({"url": url, "ca_pem": ca.replace("\n", "\r\n"), "tls_insecure": False})
        c.expect_status("T: same CA resent (CRLF) without token accepted", st, 200, raw)
        st, raw, ha = get_ha()
        c.check(ha.get("ca_pem") == ca and ha.get("token_set") is True, "T: CA unchanged, token kept", ha)

        # Tightening keeps the token; weakening again needs one.
        st, raw, body = post_ha({"url": url, "ca_pem": "", "tls_insecure": False})
        c.expect_status("T: CA removed without token accepted", st, 200, raw)
        st, raw, ha = get_ha()
        c.check(ha.get("ca_pem") == "" and ha.get("token_set") is True, "T: CA removed, token kept", ha)
        st, raw, body = post_ha({"url": url, "ca_pem": "", "tls_insecure": True, "token": fake + "3"})
        c.expect_status("T: insecure on with token saved", st, 200, raw)
        st, raw, body = post_ha({"url": url, "ca_pem": "", "tls_insecure": False})
        c.expect_status("T: insecure off without token accepted", st, 200, raw)
        st, raw, ha = get_ha()
        c.check(ha.get("tls_insecure") is False and ha.get("token_set") is True, "T: insecure off, token kept", ha)
    finally:
        st, raw, body = post_ha({"url": "", "ca_pem": "", "tls_insecure": False, "token": None})
        c.expect_status("T: restore ha to no config", st, 200, raw)
        st, raw, ha = get_ha()
        c.check(ha.get("url") == "" and ha.get("ca_pem") == "" and ha.get("tls_insecure") is False
                and ha.get("token_set") is False, "T: ha restored (no url, no CA, no token)", ha)
        h, _ = wait_ha(c, "no_config", 15)
        c.check(h.get("state") == "no_config", "T: ha.state no_config after restore", h)


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
        # A. https: since 5b.4 it connects (wss); nothing listens on the port yet -> backoff
        mark = slog.mark() if slog else 0
        st, _, raw = set_ha(c, f"https://{args.mock_host}:{args.mock_port}", fake_https)
        c.expect_status("A: set https url + fake token 200", st, 200, raw)
        h, _ = wait_ha(c, "backoff", 25)
        c.check(h.get("state") == "backoff" and "tls_not_supported" not in h.get("last_error", ""),
                "A: https -> tries to connect (backoff), no tls_not_supported", h)
        c.check(h.get("tls") == "bundle", "A: ha.tls == bundle for an https url without CA", h)
        n = attempts(mark)
        if n is not None:
            c.check(n >= 1, "A: 'HA connect attempt' on serial for https", n)
        st, _, raw = c.request("GET", "/api/config")
        cfgj = c.json_of(raw) or {}
        c.check(fake_https not in raw.decode("utf-8", "replace"),
                "A: /api/config does not contain the token")
        c.check(cfgj.get("ha", {}).get("token_set") is True
                and cfgj.get("ha", {}).get("url") == f"https://{args.mock_host}:{args.mock_port}",
                "A: config ha.token_set true, url kept", cfgj.get("ha"))
        st, _, raw = c.request("GET", "/api/status")
        c.check(fake_https not in raw.decode("utf-8", "replace"),
                "A: /api/status does not contain the token")
        st, _, raw = c.request("POST", "/api/ha/retry", body="{}", headers=post_headers())
        c.expect_status("A: retry in backoff 200", st, 200, raw)
        time.sleep(2)
        c.check(ha_status(c).get("state") != "no_config", "A: retry does not leave the https config")

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


def hb_samples(slog, mark):
    """Parses HB lines appended since mark: [(epoch, dict of key=int)]."""
    out = []
    for l in slog.since(mark):
        if " HB " not in l:
            continue
        parts = l.split()
        try:
            ts = float(parts[0])
        except ValueError:
            continue
        kv = {}
        for p in parts[2:]:
            if "=" in p:
                k, v = p.split("=", 1)
                kv[k] = int(v) if v.lstrip("-").isdigit() else v
        out.append((ts, kv))
    return out


def run_ws_test(c, args):
    """Step 5b.2 scenarios: own WebSocket transport, large messages, ws PING, Close, handshake."""
    print("\n--- Step 5b.2 on-device WebSocket transport tests (ha section is restored at the end) ---")
    if not args.serial_log:
        c.check(False, "--ws-test needs --serial-log")
        return
    slog = SerialLog(args.serial_log)
    root = os.path.dirname(os.path.abspath(__file__))
    mock_path = os.path.join(root, "mockha.py")
    good = "ws-test-token-" + secrets.token_hex(4)
    mock_url = f"http://{args.mock_host}:{args.mock_port}"
    mock = {"proc": None, "log": None}

    def stop_mock():
        if mock["proc"] is not None:
            mock["proc"].kill()
            mock["proc"].wait()
            mock["proc"] = None
            time.sleep(0.3)

    tokens = [good]

    def start_mock(*extra):
        stop_mock()
        mock["log"] = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        mock["proc"] = subprocess.Popen(
            [args.mock_python, mock_path, "--port", str(args.mock_port), "--token", tokens[-1],
             "--seconds", "900", *map(str, extra)], stdout=mock["log"], stderr=subprocess.STDOUT)
        time.sleep(1.5)

    def mock_lines():
        mock["log"].flush()
        with open(mock["log"].name, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()

    def status():
        st, _, raw = c.request("GET", "/api/status")
        return c.json_of(raw) or {}

    def safe_status():
        try:
            return status()
        except (OSError, http.client.HTTPException):
            return None

    def serial_since(mark, needle):
        return [l for l in slog.since(mark) if needle in l]

    def connect(*extra, expect="ready", timeout=30):
        tokens.append(f"{good}-{len(tokens)}")
        start_mock(*extra)
        mark = slog.mark()
        st, _, raw = set_ha(c, mock_url, tokens[-1])
        c.expect_status("set mock url + token 200", st, 200, raw)
        h, took = wait_ha(c, expect, timeout)
        return mark, h, took

    def psram_wait(base, timeout=10):
        t0 = time.time()
        free = None
        while time.time() - t0 < timeout:
            s = safe_status()
            if s:
                free = s.get("psram_free")
                if free is not None and free >= base - 64 * 1024:
                    return free, time.time() - t0
            time.sleep(0.5)
        return free, None

    results = []

    def record(name, **kw):
        results.append((name, kw))

    try:
        # --- S0: ready, heap numbers, status/HB fields
        mark, h, took = connect()
        c.check(h.get("state") == "ready", "S0: ready over the own transport", h)
        time.sleep(8)
        s = status()
        c.check(isinstance(s.get("heap_min_free"), int) and s["heap_min_free"] > 0,
                "S0: /api/status has heap_min_free", s.get("heap_min_free"))
        c.check(s.get("heap_min_free", 0) <= s.get("heap_free", 0),
                "S0: heap_min_free <= heap_free", (s.get("heap_min_free"), s.get("heap_free")))
        hb = hb_samples(slog, mark)
        c.check(len(hb) > 0 and all("heap_min" in kv and "loopmax" in kv for _, kv in hb),
                "S0: HB lines carry heap_min= and loopmax=", hb[-1:] )
        base_psram = s.get("psram_free")
        base_heap = s.get("heap_free")
        base_loop = max([kv.get("loopmax", 0) for _, kv in hb] or [0])
        record("S0 ready", ready_ms=int((took or 0) * 1000), heap_free=base_heap,
               heap_min=s.get("heap_min_free"), psram_free=base_psram, loopmax_base=base_loop)
        print(f"      baseline: heap_free={base_heap} heap_min={s.get('heap_min_free')} "
              f"psram_free={base_psram} loopmax={base_loop}")

        # --- S1: 1 MB and 3 MB after auth
        mark, h, _ = connect("--big-after-auth", 1048576, 3145728, "--after-auth-delay", 3)
        c.check(h.get("state") == "ready", "S1: ready before the big messages", h)
        t0 = time.time()
        states, fails = [], 0
        psram_min = None
        while time.time() - t0 < 24:
            s = safe_status()
            if s is None:
                fails += 1
            else:
                states.append(s.get("ha", {}).get("state"))
                pf = s.get("psram_free")
                if pf is not None:
                    psram_min = pf if psram_min is None else min(psram_min, pf)
            time.sleep(0.4)
        c.check(fails <= 1, "S1: /api/status answered throughout the transfers", fails)
        c.check(all(x == "ready" for x in states), "S1: state stays ready", sorted(set(states)))
        rx = serial_since(mark, "HA rx len=")
        got = {}
        for l in rx:
            try:
                ln = int(l.split("len=")[1].split()[0])
                ms = int(l.split("ms=")[1].split()[0])
                got[ln] = ms
            except (IndexError, ValueError):
                pass
        for want in (1048576, 3145728):
            c.check(want in got, f"S1: 'HA rx len={want} ms=' logged", rx[-4:])
            if want in got:
                kbs = want / 1024.0 / max(got[want], 1) * 1000.0
                c.check(kbs >= 300, f"S1: throughput {want} >= 300 KB/s", f"{kbs:.0f} KB/s ({got[want]} ms)")
                record(f"S1 {want // 1048576} MB rx", ms=got[want], kb_per_s=int(kbs))
        hb = hb_samples(slog, mark)
        tsl = [t for t, _ in hb]
        gaps = [b - a for a, b in zip(tsl, tsl[1:])]
        c.check(len(hb) >= 4 and (not gaps or max(gaps) < 8.5), "S1: HB continued (max gap < 8.5 s)",
                f"n={len(hb)} max_gap={max(gaps) if gaps else None}")
        lm = [kv.get("loopmax", 0) for _, kv in hb]
        hm = [kv.get("heap_min", 0) for _, kv in hb]
        print(f"      loopmax samples during/after: {lm}  (baseline {base_loop})  heap_min last {hm[-1:]}")
        record("S1 loopmax", values=lm, baseline=base_loop, heap_min_last=(hm[-1] if hm else None))
        free, rec = psram_wait(base_psram)
        c.check(rec is not None, "S1: psram_free back within 64 KB of baseline within 10 s",
                f"free={free} base={base_psram}")
        print(f"      psram min during transfers={psram_min} (drop {None if psram_min is None else base_psram - psram_min}), "
              f"recovered after {rec}")
        record("S1 psram", min_free=psram_min, recovered_s=rec)
        c.check(not serial_since(mark, "reason=too_large") and not serial_since(mark, "reason=no_memory"),
                "S1: nothing dropped")

        # --- S2: 5 MB dropped, still ready 40 s later
        mark, h, _ = connect("--big-after-auth", 5242880, "--after-auth-delay", 2)
        time.sleep(14)
        dropped = serial_since(mark, "HA frame dropped")
        c.check(any("reason=too_large" in l for l in dropped), "S2: 'HA frame dropped ... reason=too_large'",
                dropped[-2:])
        c.check(ha_status(c).get("state") == "ready", "S2: ready right after the drop")
        time.sleep(40)
        h = ha_status(c)
        c.check(h.get("state") == "ready", "S2: still ready ~40 s later (ping/pong worked)", h)
        free, rec = psram_wait(base_psram)
        c.check(rec is not None, "S2: psram back to baseline after drop", f"free={free} base={base_psram}")
        record("S2 5 MB drop", ready_after=h.get("state"), dropped_line=(dropped[-1][:100] if dropped else None))

        # --- S3: fragmented auth_ok (+ ws PING in between), S4: ws PING answered
        mark, h, _ = connect("--fragment-auth-ok", "--ws-ping-after-auth", "--after-auth-delay", 2)
        c.check(h.get("state") == "ready", "S3: fragmented auth_ok (with PING between fragments) -> ready", h)
        c.check(h.get("ha_version") == "2099.1.0-mock", "S3: ha_version parsed from the fragmented auth_ok", h)
        time.sleep(4)
        ml = mock_lines()
        c.check(any("RX PONG payload=b'mid'" in l for l in ml), "S3: PONG for the PING inside the fragments (mid)")
        c.check(any("RX PONG payload=b'hb-123'" in l for l in ml), "S4: PONG echoes the PING payload hb-123", ml[-4:])
        c.check(not any("not masked" in l for l in ml), "S4: all client frames masked")
        c.check(ha_status(c).get("state") == "ready", "S4: still ready after PING/PONG")

        # --- S5: Close 1001 -> closed_by_server, reconnect
        mark, h, _ = connect("--close-after-auth", 1001, "--after-auth-delay", 2)
        h2, _ = wait_ha(c, "backoff", 15, "closed_by_server")
        c.check(h2.get("state") == "backoff" and "closed_by_server" in h2.get("last_error", ""),
                "S5: Close 1001 -> backoff + closed_by_server", h2)
        time.sleep(3)
        ml = mock_lines()
        c.check(any("RX CLOSE code=1001" in l for l in ml) or any("RX CLOSE code=" in l for l in ml),
                "S5: device replied with a Close frame", [l for l in ml if "CLOSE" in l][:2])
        h3, _ = wait_ha(c, "ready", 20)
        c.check(h3.get("state") == "ready", "S5: reconnects and reaches ready again", h3)
        c.check(len([l for l in ml if "REQ GET" in l]) >= 1 and
                len([l for l in mock_lines() if "REQ GET" in l]) >= 2, "S5: second connection attempt seen by the mock")

        # --- S6: Close without code -> empty Close reply
        mark, h, _ = connect("--close-after-auth-nocode", "--after-auth-delay", 2)
        h2, _ = wait_ha(c, "backoff", 15, "closed_by_server")
        c.check(h2.get("state") == "backoff" and "closed_by_server" in h2.get("last_error", ""),
                "S6: Close without code -> closed_by_server", h2)
        time.sleep(2)
        cl = [l for l in mock_lines() if "RX CLOSE" in l]
        c.check(len(cl) >= 1 and "code=None len=0" in cl[0], "S6: Close reply has an empty body", cl[:2])

        # --- S7: upgrade 502
        mark, h, _ = connect("--upgrade-status", 502, expect="backoff", timeout=20)
        h, _ = wait_ha(c, "backoff", 20, "handshake_failed: http 502")
        c.check("handshake_failed: http 502" in h.get("last_error", ""), "S7: upgrade 502 -> handshake_failed: http 502", h)

        # --- S8: coalesced handshake
        mark, h, _ = connect("--coalesce-handshake")
        c.check(h.get("state") == "ready", "S8: 101 + auth_required in one segment -> ready", h)

        # --- S9: server closes before 101
        mark, h, _ = connect("--close-before-101", expect="backoff", timeout=20)
        h, _ = wait_ha(c, "backoff", 20, "handshake_failed: closed")
        c.check("handshake_failed: closed" in h.get("last_error", ""), "S9: close before 101 -> handshake_failed: closed", h)

        # --- S10: kill mid 3 MB
        mark, h, _ = connect("--kill-mid-transfer", 3145728, "--after-auth-delay", 3)
        c.check(h.get("state") == "ready", "S10: ready before the kill", h)
        h2, _ = wait_ha(c, "backoff", 25, "connection_lost")
        c.check(h2.get("state") == "backoff" and "connection_lost" in h2.get("last_error", ""),
                "S10: killed mid-transfer -> connection_lost", h2)
        up = status().get("uptime_s", 0)
        c.check(up > 60, "S10: device did not reboot (uptime)", up)
        c.check(not serial_since(mark, "BOOT") and not serial_since(mark, "Guru Meditation"),
                "S10: no reboot / panic on serial")
        hb = hb_samples(slog, mark)
        stop_mock()  # no reconnect storms while measuring
        time.sleep(3)
        free, rec = psram_wait(base_psram)
        c.check(rec is not None, "S10: PSRAM freed after the killed transfer", f"free={free} base={base_psram}")
        record("S10 kill mid", psram_recovered_s=rec, psram_free=free)

        # --- S11: stall mid 3 MB (record only)
        mark, h, _ = connect("--stall-mid-transfer", 3145728, "--after-auth-delay", 3)
        c.check(h.get("state") == "ready", "S11: ready before the stall", h)
        t0 = time.time()
        seen = None
        while time.time() - t0 < 70:
            hh = safe_status()
            if hh and hh.get("ha", {}).get("state") != "ready":
                seen = (hh["ha"]["state"], hh["ha"].get("last_error"), round(time.time() - t0, 1))
                break
            time.sleep(1)
        print(f"      stall mid-transfer outcome: {seen}")
        c.check(seen is not None, "S11: a stalled transfer is detected (ping/pong timeout) within 70 s", seen)
        record("S11 stall", outcome=seen)
        stop_mock()
        time.sleep(3)
        free, rec = psram_wait(base_psram)
        c.check(rec is not None, "S11: PSRAM freed after the stall ends", f"free={free} base={base_psram}")
    finally:
        stop_mock()
        st, _, raw = set_ha(c, "", None)
        c.expect_status("restore: ha url '' + token null 200", st, 200, raw)
        h, _ = wait_ha(c, "no_config", 15)
        c.check(h.get("state") == "no_config", "restore: ha.state no_config", h)
        st, _, raw = c.request("GET", "/api/config")
        hc = (c.json_of(raw) or {}).get("ha", {})
        c.check(hc.get("url") == "" and hc.get("token_set") is False, "restore: ha url '' and token_set false", hc)
        text = "\n".join(slog.since(0))
        for tok in tokens:
            c.check(tok not in text, f"serial log never contains the token value ({tok[-6:]})")
        c.check("access_token" not in text, "serial log never contains access_token")
        st, _, raw = c.request("GET", "/api/status")
        c.check(not any(t in raw.decode("utf-8", "replace") for t in tokens),
                "/api/status does not contain a token")
        print("\n      --- scenario numbers ---")
        for name, kw in results:
            print(f"      {name}: {kw}")


def tls_serial(slog, mark, needle):
    return [l for l in slog.since(mark) if needle in l]


def run_tls_test(c, args):
    """Step 5b.4 device matrix T1-T14, T16 + heap gate: wss against tools/mockha.py with throwaway
    certificates from tools/gen_test_certs.sh (--cert-dir). Restores ha to no config."""
    print("\n--- Step 5b.4 on-device wss tests (ha section is restored at the end) ---")
    if not (args.serial_log and args.cert_dir):
        c.check(False, "--tls-test needs --serial-log and --cert-dir")
        return
    slog = SerialLog(args.serial_log)
    root = os.path.dirname(os.path.abspath(__file__))
    mock_path = os.path.join(root, "mockha.py")
    cd = args.cert_dir

    def pem(name):
        with open(os.path.join(cd, name), encoding="utf-8") as f:
            return f.read()

    def cert(name):
        return ["--tls-cert", os.path.join(cd, name + ".pem"), "--tls-key", os.path.join(cd, name + ".key")]

    ca_a, ca_b, self_pem = pem("ca-a.pem"), pem("ca-b.pem"), pem("self.pem")
    big_bundle, bad_ca = pem("bigbundle.pem"), pem("bad-ca.pem")
    port = args.mock_port
    base = f"https://{args.mock_host}:{port}"
    tokens = []
    mock = {"proc": None, "log": None}
    http_srv = {"proc": None, "log": None, "dir": None}
    results = []

    def record(name, **kw):
        results.append((name, kw))

    def stop_mock():
        if mock["proc"] is not None:
            mock["proc"].kill()
            mock["proc"].wait()
            mock["proc"] = None
            time.sleep(0.3)

    def start_mock(*extra, port_=None):
        stop_mock()
        mock["log"] = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        mock["proc"] = subprocess.Popen(
            [args.mock_python, mock_path, "--port", str(port_ or port), "--token", tokens[-1],
             "--seconds", "1500", *map(str, extra)], stdout=mock["log"], stderr=subprocess.STDOUT)
        time.sleep(1.5)

    def mock_lines():
        mock["log"].flush()
        with open(mock["log"].name, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()

    def status():
        st, _, raw = c.request("GET", "/api/status")
        return c.json_of(raw) or {}

    def safe_status():
        try:
            return status()
        except (OSError, http.client.HTTPException):
            return None

    def post_ha(url, ca="", insecure=False, token=True):
        ha = {"url": url, "ca_pem": ca, "tls_insecure": insecure}
        if token:
            tokens.append(f"tls-test-token-{len(tokens)}-{secrets.token_hex(3)}")
            ha["token"] = tokens[-1]
        st, _, raw = c.request("POST", "/api/config", body=json.dumps({"ha": ha}), headers=post_headers())
        return st, raw

    def scenario(label, mock_args, ca="", insecure=False, url=None, want_state="ready", want_err=None,
                 timeout=30, start=True):
        """Starts the mock (token = the one set below), points the device at it, waits for the state."""
        # The old mock goes first and the new settings are saved while nothing listens, so the
        # device can never talk to the new mock with the previous scenario's TLS settings.
        tokens.append(f"tls-test-token-{len(tokens)}-{secrets.token_hex(3)}")
        stop_mock()
        mark = slog.mark()
        ha = {"url": url or base, "ca_pem": ca, "tls_insecure": insecure, "token": tokens[-1]}
        st, _, raw = c.request("POST", "/api/config", body=json.dumps({"ha": ha}), headers=post_headers())
        c.expect_status(f"{label}: config saved", st, 200, raw)
        if start:
            start_mock(*mock_args)
        h, took = wait_ha(c, want_state, timeout, want_err)
        return mark, h, took

    def tls_ok_ms(mark):
        out = []
        for l in tls_serial(slog, mark, "HA tls ok ms="):
            try:
                out.append((int(l.split("ms=")[1].split()[0]), int(l.split("heap=")[1].split()[0])))
            except (IndexError, ValueError):
                pass
        return out

    def hb_max(mark, key):
        vals = [kv.get(key, 0) for _, kv in hb_samples(slog, mark)]
        return max(vals) if vals else None

    def no_ws_traffic(label):
        ml = mock_lines()
        c.check(not any(" REQ " in l for l in ml) and not any(" RX " in l for l in ml),
                f"{label}: mock saw no WebSocket request and no auth", [l for l in ml if " REQ " in l or " RX " in l][:3])

    try:
        # ---- start from a clean slate (an earlier aborted run may have left a config behind)
        clear = {"ha": {"url": "", "ca_pem": "", "tls_insecure": False, "token": None}}
        st, _, raw = c.request("POST", "/api/config", body=json.dumps(clear), headers=post_headers())
        c.expect_status("start: ha cleared", st, 200, raw)
        wait_ha(c, "no_config", 15)
        s0 = status()
        c.check(isinstance(s0.get("ha", {}).get("tls"), str), "T14: /api/status ha.tls present (no config)", s0.get("ha"))
        c.check(s0.get("ha", {}).get("tls") == "none", "T14: ha.tls == none without a config", s0.get("ha"))

        # ---- T1 cn-ip + CA-A -> ready custom_ca ; T13 PING/PONG over TLS
        mark, h, took = scenario("T1", cert("cn-ip") + ["--ws-ping-after-auth", "--after-auth-delay", 2], ca=ca_a)
        c.check(h.get("state") == "ready", "T1: cn-ip leaf + CA-A -> ready", h)
        c.check(h.get("tls") == "custom_ca", "T1/T14: ha.tls == custom_ca", h)
        c.check(h.get("ha_version") == "2099.1.0-mock", "T1: ha_version over TLS", h)
        oks = tls_ok_ms(mark)
        c.check(bool(oks) and oks[0][0] <= 1500, "T1: 'HA tls ok ms=' <= 1500", oks[:2])
        c.check(bool(tls_serial(slog, mark, "HA tls mode=custom_ca host=")), "T1: serial 'HA tls mode=custom_ca host='")
        c.check(not tls_serial(slog, mark, "verify=off"), "T1: no 'verify=off' warning in custom_ca mode")
        time.sleep(4)
        ml = mock_lines()
        c.check(any("TLS handshake ok" in l and "TLSv1.2" in l for l in ml), "T1: mock negotiated TLS 1.2",
                [l for l in ml if "TLS handshake" in l][:2])
        c.check(any("RX PONG payload=b'hb-123'" in l for l in ml), "T13: ws PING over TLS answered with same payload", ml[-4:])
        c.check(not any("not masked" in l for l in ml), "T13: client frames masked over TLS")
        s = status()
        time.sleep(4)
        s = status()
        heap_conn = s.get("heap_free")
        c.check(isinstance(heap_conn, int) and heap_conn >= 90 * 1024, "heap: internal free >= 90 KB while connected (cn-ip/CA-A)",
                heap_conn)
        record("T1", handshake_ms=oks[0][0] if oks else None, heap_at_tls_ok=oks[0][1] if oks else None,
               heap_free_connected=heap_conn, heap_min=s.get("heap_min_free"),
               ready_after_s=round(took or 0, 1))
        st, _, raw = c.request("GET", "/api/config")
        hc = (c.json_of(raw) or {}).get("ha", {})
        c.check(hc.get("ca_pem") == ca_a and hc.get("tls_insecure") is False and hc.get("token_set") is True,
                "T14: GET /api/config shows ca_pem (normalised), tls_insecure false, token_set", {k: (v if k != "ca_pem" else len(v)) for k, v in hc.items()})
        c.check(not any(t in raw.decode("utf-8", "replace") for t in tokens), "T16: /api/config has no token")
        st, _, raw = c.request("GET", "/api/status")
        c.check(not any(t in raw.decode("utf-8", "replace") for t in tokens) and "BEGIN CERTIFICATE" not in raw.decode("utf-8", "replace"),
                "T16: /api/status has no token and no CA text")

        # ---- T13 Close over TLS
        mark, h, _ = scenario("T13b", cert("cn-ip") + ["--close-after-auth", 1001, "--after-auth-delay", 2], ca=ca_a)
        h2, _ = wait_ha(c, "backoff", 15, "closed_by_server")
        c.check(h2.get("state") == "backoff" and "closed_by_server" in h2.get("last_error", ""),
                "T13: Close 1001 over TLS -> backoff + closed_by_server", h2)
        time.sleep(2)
        c.check(any("RX CLOSE code=1001" in l for l in mock_lines()), "T13: device replied to the Close over TLS",
                [l for l in mock_lines() if "CLOSE" in l][:2])
        h3, _ = wait_ha(c, "ready", 20)
        c.check(h3.get("state") == "ready", "T13: reconnects over TLS after the Close", h3)

        # ---- T2 CA-B for a CA-A leaf -> tls_cert_rejected, no WS traffic
        mark, h, _ = scenario("T2", cert("cn-ip"), ca=ca_b, want_state="backoff", want_err="tls_cert_rejected")
        c.check("tls_cert_rejected" in h.get("last_error", ""), "T2: wrong CA -> tls_cert_rejected", h)
        c.check(h.get("tls") == "custom_ca", "T2: ha.tls custom_ca", h)
        time.sleep(3)
        no_ws_traffic("T2")
        c.check(any("TLS handshake failed" in l for l in mock_lines()), "T2: mock saw the TLS handshake fail (client alert)")
        c.check(bool(tls_serial(slog, mark, "HA tls failed error=tls_cert_rejected")), "T2: serial 'HA tls failed error=tls_cert_rejected'",
                tls_serial(slog, mark, "HA tls failed")[:1])

        # ---- T3 bundle mode vs the mock -> rejected
        mark, h, _ = scenario("T3", cert("cn-ip"), ca="", want_state="backoff", want_err="tls_cert_rejected")
        c.check("tls_cert_rejected" in h.get("last_error", ""), "T3: bundle mode vs private CA -> tls_cert_rejected", h)
        c.check(h.get("tls") == "bundle", "T3/T14: ha.tls == bundle", h)
        c.check(bool(tls_serial(slog, mark, "HA tls mode=bundle host=")), "T3: serial 'HA tls mode=bundle'")
        time.sleep(2)
        no_ws_traffic("T3")

        # ---- T4 insecure -> ready + WARN
        mark, h, took = scenario("T4", cert("cn-ip"), insecure=True)
        c.check(h.get("state") == "ready", "T4: insecure -> ready", h)
        c.check(h.get("tls") == "insecure", "T4/T14: ha.tls == insecure", h)
        time.sleep(1)
        c.check(bool(tls_serial(slog, mark, "WARN HA tls verify=off")), "T4: serial 'WARN HA tls verify=off'")
        c.check(bool(tls_serial(slog, mark, "HA tls mode=insecure host=")), "T4: serial 'HA tls mode=insecure'")
        oks = tls_ok_ms(mark)
        record("T4", handshake_ms=oks[0][0] if oks else None, ready_after_s=round(took or 0, 1))
        # insecure also accepts an unrelated certificate (self) - trust is not checked at all
        mark, h, _ = scenario("T4b", cert("self"), insecure=True)
        c.check(h.get("state") == "ready", "T4: insecure accepts a self-signed leaf", h)

        # ---- T5 san-ip + CA-A -> rejected (mbedTLS has no IP SAN matching)
        mark, h, _ = scenario("T5", cert("san-ip"), ca=ca_a, want_state="backoff", want_err="tls_cert_rejected")
        c.check("tls_cert_rejected" in h.get("last_error", ""), "T5: IP-SAN-only leaf -> tls_cert_rejected", h)
        time.sleep(2)
        no_ws_traffic("T5")

        # ---- T6 self-signed leaf pasted as CA -> ready
        mark, h, _ = scenario("T6", cert("self"), ca=self_pem)
        c.check(h.get("state") == "ready", "T6: self-signed leaf as CA -> ready", h)
        c.check(h.get("tls") == "custom_ca", "T6: ha.tls custom_ca", h)

        # ---- T7 https at a plain HTTP server -> tls_handshake_failed, no plaintext fallback
        http_srv["dir"] = tempfile.mkdtemp(prefix="plainhttp_")
        http_srv["log"] = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        p7 = port + 1
        http_srv["proc"] = subprocess.Popen([args.mock_python, "-I", "-m", "http.server", str(p7), "--bind", "0.0.0.0"],
                                            cwd=http_srv["dir"], stdout=http_srv["log"], stderr=subprocess.STDOUT)
        time.sleep(1.5)
        stop_mock()
        mark, h, _ = scenario("T7", [], ca=ca_a, url=f"https://{args.mock_host}:{p7}", start=False,
                              want_state="backoff", want_err="tls_handshake_failed")
        c.check("tls_handshake_failed" in h.get("last_error", ""), "T7: https at a plain HTTP port -> tls_handshake_failed", h)
        time.sleep(3)
        http_srv["log"].flush()
        with open(http_srv["log"].name, encoding="utf-8", errors="replace") as f:
            hl = f.read()
        c.check("api/websocket" not in hl and "Upgrade" not in hl and not any(t in hl for t in tokens),
                "T7: the plain server never saw a WebSocket request or token (no plaintext fallback)", hl[-300:])
        http_srv["proc"].kill()
        http_srv["proc"].wait()
        http_srv["proc"] = None

        # ---- T8 TLS 1.3 only -> tls_handshake_failed
        mark, h, _ = scenario("T8", cert("cn-ip") + ["--tls13-only"], ca=ca_a, want_state="backoff",
                              want_err="tls_handshake_failed")
        c.check("tls_handshake_failed" in h.get("last_error", ""), "T8: TLS 1.3-only server -> tls_handshake_failed", h)
        time.sleep(2)
        no_ws_traffic("T8")

        # ---- T9 stalled handshake -> tls_timeout, block <= 6.5 s, backoff continues
        mark, h, _ = scenario("T9", ["--stall"], ca=ca_a, want_state="backoff", want_err="tls_timeout", timeout=40)
        c.check("tls_timeout" in h.get("last_error", ""), "T9: stalled TLS handshake -> tls_timeout", h)
        time.sleep(32)  # at least two more attempts (1 s, 2 s, 4 s backoff plus 4 s timeouts)
        att = len(tls_serial(slog, mark, "HA connect attempt"))
        c.check(att >= 3, "T9: backoff continues (>= 3 attempts in ~45 s)", att)
        lm = hb_max(mark, "loopmax")
        c.check(lm is not None and 3500 <= lm <= 6500, "T9: single loop block (HB loopmax) between 3.5 and 6.5 s", lm)
        record("T9", loopmax_ms=lm, attempts=att)
        stop_mock()

        # ---- T10 structurally valid but unparsable CA -> tls_ca_invalid with no TCP connect
        mark, h, _ = scenario("T10", cert("cn-ip"), ca=bad_ca, want_state="backoff", want_err="tls_ca_invalid")
        c.check("tls_ca_invalid" in h.get("last_error", ""), "T10: unparsable CA -> tls_ca_invalid", h)
        time.sleep(4)
        c.check(not any("CONNECT from" in l for l in mock_lines()), "T10: the device made no TCP connection to the mock",
                [l for l in mock_lines() if "CONNECT" in l][:2])
        stop_mock()

        # ---- T11 3 MB after auth over TLS
        mark, h, _ = scenario("T11", cert("cn-ip") + ["--big-after-auth", 3145728, "--after-auth-delay", 3], ca=ca_a)
        c.check(h.get("state") == "ready", "T11: ready before the 3 MB message", h)
        time.sleep(3)
        fails, states = 0, []
        t0 = time.time()
        while time.time() - t0 < 20:
            s = safe_status()
            if s is None:
                fails += 1
            else:
                states.append(s.get("ha", {}).get("state"))
            time.sleep(0.5)
        rx = tls_serial(slog, mark, "HA rx len=")
        got = None
        for l in rx:
            try:
                ln = int(l.split("len=")[1].split()[0])
                ms = int(l.split("ms=")[1].split()[0])
                if ln == 3145728:
                    got = ms
            except (IndexError, ValueError):
                pass
        c.check(got is not None, "T11: 3 MB message received over TLS ('HA rx len=3145728')", rx[-3:])
        if got:
            kbs = 3145728 / 1024.0 / max(got, 1) * 1000.0
            c.check(kbs >= 300, "T11: throughput >= 300 KB/s", f"{kbs:.0f} KB/s ({got} ms)")
        c.check(all(x == "ready" for x in states), "T11: state stays ready", sorted(set(states)))
        lm_all = hb_max(mark, "loopmax")
        lines = slog.since(mark)
        idx = next((i for i, l in enumerate(lines) if "HA state=ready" in l), 0)
        ready_hb = []
        for l in lines[idx:]:
            if " HB " in l and "loopmax=" in l:
                try:
                    ready_hb.append(int(l.split("loopmax=")[1].split()[0]))
                except ValueError:
                    pass
        record("T11", rx_ms=got, kb_per_s=int(3145728 / 1024.0 / max(got, 1) * 1000.0) if got else None,
               loopmax_ms_ready_to_end=ready_hb, loopmax_ms_whole_run=lm_all, status_failures=fails)

        # ---- heap gate with a near-4096-char CA bundle (one of its certs is CA-A)
        norm_len = len(big_bundle.replace("\r\n", "\n").strip()) + 1
        print(f"      bigbundle normalised length {norm_len}")
        mark, h, took = scenario("H", cert("cn-ip"), ca=big_bundle)
        c.check(h.get("state") == "ready", f"H: ready with a {norm_len}-char CA bundle (CA-A is one of 3)", h)
        c.check(h.get("tls") == "custom_ca", "H: ha.tls custom_ca", h)
        time.sleep(6)
        s = status()
        oks = tls_ok_ms(mark)
        hf, hm = s.get("heap_free"), s.get("heap_min_free")
        c.check(isinstance(hf, int) and hf >= 90 * 1024, "H: internal free heap >= 90 KB connected (big CA)", hf)
        c.check(isinstance(hm, int) and hm >= 70 * 1024, "H: heap_min >= 70 KB (big CA)", hm)
        c.check(bool(oks) and oks[0][0] <= 1500, "H: handshake <= 1500 ms (big CA)", oks[:1])
        record("H big CA", ca_chars=norm_len, heap_free=hf, heap_min=hm, handshake_ms=oks[0][0] if oks else None,
               heap_at_tls_ok=oks[0][1] if oks else None)

        # ---- T12 20 kill/restart cycles (custom CA, cn-ip)
        mark, h, _ = scenario("T12", cert("cn-ip"), ca=ca_a)
        c.check(h.get("state") == "ready", "T12: ready before the kill/restart cycles", h)
        time.sleep(5)
        heaps = {}
        s = status()
        heaps[0] = s.get("heap_free")
        for i in range(1, 21):
            n_before = len(tls_ok_ms(mark))
            start_mock(*cert("cn-ip"))  # kills the old mock (connection dropped), starts a new one
            deadline = time.time() + 60
            while time.time() < deadline and len(tls_ok_ms(mark)) <= n_before:
                time.sleep(1)
            h, _ = wait_ha(c, "ready", 25)
            if h.get("state") != "ready":
                c.check(False, f"T12: cycle {i} reached ready", h)
                break
            time.sleep(4)
            s = safe_status() or {}
            heaps[i] = s.get("heap_free")
            print(f"      cycle {i:2d}: heap_free={heaps[i]} heap_min={s.get('heap_min_free')}")
        s = status()
        done = max(heaps)
        c.check(done == 20, "T12: all 20 kill/restart cycles completed", done)
        if done == 20:
            drift_settled = abs(heaps[20] - heaps[10])
            drift_total = heaps[0] - heaps[20]
            c.check(drift_settled <= 2048, "T12: heap after settling (cycle 10 -> 20) within 2 KB", f"{drift_settled} B")
            c.check(drift_total <= 4096, "T12 (info): heap start -> cycle 20 drift <= 4 KB (warm-up allocations)", f"{drift_total} B")
            if drift_total > 2048:
                print(f"      NOTE start->20 drift {drift_total} B exceeds 2 KB (first cycles warm up), settled drift {drift_settled} B")
        hm = s.get("heap_min_free")
        c.check(isinstance(hm, int) and hm >= 70 * 1024, "T12: heap_min >= 70 KB after the cycles", hm)
        record("T12", heaps={k: v for k, v in heaps.items() if k in (0, 1, 5, 10, 15, 20)}, heap_min=hm)
        stop_mock()

        # ---- T14/UI: no maxlength on the CA textarea in the served page; > 4096 chars -> 400 length
        st, _, raw = c.request("GET", "/")
        page = raw.decode("utf-8", "replace")
        i = page.find('id="ha-ca-pem"')
        tag = page[page.rfind("<", 0, i): page.find(">", i) + 1] if i >= 0 else ""
        c.check(i >= 0 and "maxlength" not in tag, "UI: CA textarea served without maxlength", tag)
        too_long = ca_a + "A" * 4100
        st, raw = post_ha(base, ca=too_long)
        body = c.json_of(raw) or {}
        c.check(st == 400 and body.get("field") == "ha.ca_pem" and body.get("error") == "length",
                "UI: > 4096-char CA POST -> 400 length ha.ca_pem", f"{st} {raw[:150]!r}")
    finally:
        stop_mock()
        if http_srv["proc"] is not None:
            http_srv["proc"].kill()
            http_srv["proc"].wait()
        body = {"ha": {"url": "", "ca_pem": "", "tls_insecure": False, "token": None}}
        st, _, raw = c.request("POST", "/api/config", body=json.dumps(body), headers=post_headers())
        c.expect_status("restore: ha cleared (url, CA, insecure, token)", st, 200, raw)
        h, _ = wait_ha(c, "no_config", 15)
        c.check(h.get("state") == "no_config", "restore: ha.state no_config", h)
        st, _, raw = c.request("GET", "/api/config")
        hc = (c.json_of(raw) or {}).get("ha", {})
        c.check(hc.get("url") == "" and hc.get("ca_pem") == "" and hc.get("tls_insecure") is False
                and hc.get("token_set") is False, "restore: url '', no CA, not insecure, no token", hc)
        text = "\n".join(slog.since(0))
        for tok in tokens:
            c.check(tok not in text, f"T16: serial log never contains the token value (...{tok[-8:]})")
        c.check("access_token" not in text, "T16: serial log never contains access_token")
        c.check("BEGIN CERTIFICATE" not in text and "MIID" not in text and "MIIC" not in text,
                "T16: serial log never contains CA text")
        print("\n      --- scenario numbers ---")
        for name, kw in results:
            print(f"      {name}: {kw}")


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

    if args.tls_config_test:
        run_tls_config_test(c)
    if args.ha_test:
        run_ha_test(c, args)
    if args.ws_test:
        run_ws_test(c, args)
    if args.tls_test:
        run_tls_test(c, args)

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
    p.add_argument("--tls-config-test", action="store_true",
                   help="Step 5b.3 ha.ca_pem / ha.tls_insecure API tests (restores the ha section)")
    p.add_argument("--ha-test", action="store_true",
                   help="Step 5 on-device tests against tools/mockha.py (restores the ha section)")
    p.add_argument("--ws-test", action="store_true",
                   help="Step 5b.2 on-device WebSocket transport tests (needs --serial-log; restores the ha section)")
    p.add_argument("--tls-test", action="store_true",
                   help="Step 5b.4 on-device wss matrix (needs --cert-dir, --serial-log; restores the ha section)")
    p.add_argument("--cert-dir", help="directory written by tools/gen_test_certs.sh (never commit it)")
    p.add_argument("--mock-python", help="Python executable that may accept LAN connections")
    p.add_argument("--mock-host", help="LAN IP of this PC as seen from the device")
    p.add_argument("--mock-port", type=int, default=8765)
    p.add_argument("--serial-log", help="file appended to by a background serial logger")
    args = p.parse_args()
    if (args.ha_test or args.ws_test or args.tls_test) and not (args.mock_python and args.mock_host):
        p.error("--ha-test / --ws-test need --mock-python and --mock-host")
    if args.ws_test and not args.serial_log:
        p.error("--ws-test needs --serial-log")
    if args.tls_test and not (args.serial_log and args.cert_dir):
        p.error("--tls-test needs --serial-log and --cert-dir")
    socket.setdefaulttimeout(TIMEOUT)
    sys.exit(run(args))


if __name__ == "__main__":
    main()
