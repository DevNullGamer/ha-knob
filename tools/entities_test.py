"""Step 6.6 device tests for the entity-list web API (run via tools/http_check.py --entities-test).

Kept in its own module so http_check.py stays readable; it uses http_check's SerialLog, wait_ha,
set_ha, post_headers, basic and hb_samples. Needs a mock HA (tools/mockha.py, with
--stdin-commands for the race tests), a background serial logger (tools/serial_log.py) and a
Python the Windows firewall lets accept LAN connections. Changes the ha, controls and web
sections and restores them (ha cleared, controls [], web password cleared) at the end.
Numbering [1]..[12] follows testpoints-6.6.md.
"""
import json
import os
import re
import secrets
import socket
import subprocess
import tempfile
import threading
import time

SUPPORTED = {"light", "switch", "climate", "cover", "fan", "media_player", "input_boolean"}


def raw_get(ip, path, method="GET", host=None, chunk=0, delay=0.0, abort_after=None, rst=False,
            timeout=25, extra_headers=None):
    """One HTTP/1.1 request on a raw socket; optional slow body read and early abort."""
    out = {"status": None, "headers": {}, "body": b"", "t_hdr": None, "t_end": None,
           "aborted": False, "err": None, "t0": time.time()}
    s = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        if chunk:
            # slow reader: a small receive buffer, so a finished server stream is not hidden in the client's buffer
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.connect((ip, 80))
        hdrs = f"{method} {path} HTTP/1.1\r\nHost: {host or ip}\r\nConnection: close\r\n"
        for k, v in (extra_headers or {}).items():
            hdrs += f"{k}: {v}\r\n"
        s.sendall((hdrs + "\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = s.recv(4096)
            if not d:
                raise ConnectionError("closed before headers")
            buf += d
        head, body = buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        out["status"] = int(lines[0].split()[1])
        out["headers"] = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
        out["t_hdr"] = time.time()
        want = int(out["headers"].get("content-length", "0")) if method != "HEAD" else 0
        got = bytearray(body)
        out["t_cut"] = None
        while len(got) < want:
            if out["t_cut"] is None and len(got) >= want - 12288:
                out["t_cut"] = time.time()
            if abort_after is not None and len(got) >= abort_after:
                out["aborted"] = True
                break
            d = s.recv(chunk if chunk else 65536)
            if chunk and delay:
                time.sleep(delay)
            if not d:
                raise ConnectionError(f"closed after {len(got)} of {want} body bytes")
            got += d
        out["body"] = bytes(got)
        out["t_end"] = time.time()
        if out["t_cut"] is None:
            out["t_cut"] = out["t_end"]
        if out["aborted"] and rst:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
    except (OSError, ConnectionError, ValueError) as e:
        out["err"] = repr(e)
        out["t_end"] = time.time()
    finally:
        if s is not None:
            try:
                s.close()
            except OSError:
                pass
    return out


def wave(ip, n, path="/api/ha/entities", **kw):
    """n parallel raw_get calls released together."""
    res = [None] * n
    gate = threading.Barrier(n)

    def run(i):
        try:
            gate.wait(10)
        except threading.BrokenBarrierError:
            pass
        res[i] = raw_get(ip, path, **kw)

    ts = [threading.Thread(target=run, args=(i,), daemon=True) for i in range(n)]
    for t in ts:
        t.start()
    for t in ts:
        t.join(90)
    return res


def max_overlap(streams, tail=False):
    """Largest number of overlapping [header, end] intervals; tail=True ends each interval where the
    last 12 KB (about one TCP send window plus the library's send buffer) were still to be read."""
    ev = []
    for r in streams:
        ev.append((r["t_hdr"], 1))
        ev.append((r["t_cut"] if tail else r["t_end"], -1))
    cur = best = 0
    for _, d in sorted(ev, key=lambda x: (x[0], x[1])):
        cur += d
        best = max(best, cur)
    return best


def tally(results):
    t = {}
    for r in results:
        k = r["status"] if r["status"] is not None else "refused/err"
        t[k] = t.get(k, 0) + 1
    return t


def expected_meta_from_native_test(root):
    """The metaJson string literal of test/native/test_domainactions (test_meta_json_exact)."""
    with open(os.path.join(root, "test", "native", "test_domainactions", "test_main.cpp"), encoding="utf-8") as f:
        src = f.read()
    i = src.index("static void test_meta_json_exact")
    j = src.index("TEST_ASSERT_EQUAL_STRING(expected", i)
    block = src[i:j]
    k = block.index("const char *expected")
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', block[k:])
    return "".join(l.replace('\\"', '"').replace("\\\\", "\\") for l in lits)


def run_entities_test(c, args):
    import mockha
    from http_check import SerialLog, wait_ha, set_ha, post_headers, basic, hb_samples
    print("\n--- Step 6.6 entities API tests (ha, controls and web sections are restored) ---")
    if not (args.serial_log and args.mock_python and args.mock_host):
        c.check(False, "--entities-test needs --serial-log, --mock-python and --mock-host")
        return
    slog = SerialLog(args.serial_log)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tools = os.path.join(root, "tools")
    ip = c.ip
    port = args.mock_port
    mock_url = f"http://{args.mock_host}:{port}"
    tmp = tempfile.mkdtemp(prefix="entities-test-")
    tokens = []
    numbers = []
    mock = {"proc": None, "log": None}

    def num(s):
        numbers.append(s)
        print("      NUM  " + s, flush=True)

    def make_fixture(name, n, namelen):
        st = mockha.make_states(n, 0)
        for i, e in enumerate(st):
            if i % 11 == 0:
                e["attributes"]["friendly_name"] = f"Køkken {i}"
            else:
                e["attributes"]["friendly_name"] = (f"Name {i} " + "x" * namelen)[:namelen]
        p = os.path.join(tmp, name)
        with open(p, "w", encoding="utf-8") as f:
            json.dump(st, f, ensure_ascii=False)
        return p, st

    small_path, small = make_fixture("small.json", 60, 20)
    big_path, big = make_fixture("big.json", 1200, 90)

    def stop_mock():
        if mock["proc"] is not None:
            try:
                mock["proc"].stdin.close()
            except OSError:
                pass
            mock["proc"].kill()
            mock["proc"].wait()
            mock["proc"] = None
            time.sleep(0.3)

    def start_mock(token, *extra):
        stop_mock()
        mock["log"] = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False, dir=tmp)
        mock["proc"] = subprocess.Popen(
            [args.mock_python, "-u", os.path.join(tools, "mockha.py"), "--port", str(port), "--token", token,
             "--seconds", "1800", "--stdin-commands", *map(str, extra)],
            stdin=subprocess.PIPE, stdout=mock["log"], stderr=subprocess.STDOUT, text=True)
        time.sleep(1.5)

    def mock_cmd(cmd):
        try:
            mock["proc"].stdin.write(cmd + "\n")
            mock["proc"].stdin.flush()
        except (OSError, ValueError) as e:
            print(f"      mock cmd {cmd} failed: {e!r}")

    def mock_lines():
        mock["log"].flush()
        with open(mock["log"].name, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()

    def connect(*extra, timeout=60):
        tokens.append(f"ent-test-token-{len(tokens)}-{secrets.token_hex(3)}")
        start_mock(tokens[-1], *extra)
        st, _, raw = set_ha(c, mock_url, tokens[-1])
        c.expect_status("set mock url + token 200", st, 200, raw)
        h, took = wait_ha(c, "ready", timeout)
        c.check(h.get("state") == "ready", "connect: ha ready", h)
        return took

    def status():
        st, _, raw = c.request("GET", "/api/status")
        return c.json_of(raw) or {}

    def safe_status():
        try:
            return status()
        except Exception:
            return None

    def catalog():
        return status().get("ha", {}).get("catalog", {})

    def get_ent(path="/api/ha/entities", **kw):
        return c.request("GET", path, **kw)

    def post_refresh(**kw):
        return c.request("POST", "/api/ha/entities/refresh", body="{}", headers=post_headers(), **kw)

    def serial_count(mark, needle):
        return sum(1 for l in slog.since(mark) if needle in l)

    def poll_200(timeout=15, interval=0.25):
        t0 = time.time()
        seen = []
        while time.time() - t0 < timeout:
            st, h, raw = get_ent()
            seen.append(st)
            if st != 202:
                return seen, st, h, raw, time.time() - t0
            time.sleep(interval)
        return seen, None, {}, b"", time.time() - t0

    def check_body(label, raw, fixture):
        body = c.json_of(raw)
        ok = isinstance(body, dict)
        c.check(ok, f"{label}: body is a JSON object", raw[:120])
        if not ok:
            return None
        c.check(set(body) == {"fetched_at_s", "total", "skipped", "truncated", "entities"},
                f"{label}: top-level keys fetched_at_s,total,skipped,truncated,entities", sorted(body))
        c.check(body.get("total") == len(fixture) and body.get("skipped") == 0 and body.get("truncated") is False,
                f"{label}: total/skipped/truncated match the fixture",
                (body.get("total"), body.get("skipped"), body.get("truncated"), len(fixture)))
        exp = {e["entity_id"]: e for e in fixture if e["entity_id"].split(".")[0] in SUPPORTED}
        got = {e.get("id"): e for e in body.get("entities", [])}
        c.check(set(got) == set(exp), f"{label}: entity ids match the fixture's supported domains", (len(got), len(exp)))
        c.check(all(set(e) == {"id", "name", "state", "actions", "step_default"} for e in got.values()),
                f"{label}: entity keys are exactly id,name,state,actions,step_default")
        bad = [i for i, e in got.items() if i in exp and (e["name"] != exp[i]["attributes"]["friendly_name"]
                                                          or e["state"] != exp[i]["state"])]
        c.check(not bad, f"{label}: name and state match the fixture (UTF-8 name included)", bad[:3])
        c.check(all(isinstance(e["actions"], list) and e["actions"] and e["actions"][-1] == "none"
                    and isinstance(e["step_default"], (int, float)) for e in got.values()),
                f"{label}: actions end with none, step_default numeric")
        return body

    def no_cors(label, hdrs):
        c.check(not any(k.startswith("access-control-") for k in hdrs), f"{label}: no Access-Control-* header",
                list(hdrs))

    t_start = slog.mark()
    pw = None
    try:
        for body in ({"ha": {"url": "", "ca_pem": "", "tls_insecure": False, "token": None}}, {"controls": []},
                     {"web": {"password": None}}):
            st, _, raw = c.request("POST", "/api/config", body=json.dumps(body), headers=post_headers())
            c.expect_status("start: clean slate " + next(iter(body)), st, 200, raw)
        wait_ha(c, "no_config", 15)

        # ================= 1. HA not ready
        print("  [1] HA not Ready")
        st, h, raw = get_ent()
        c.check(st == 503 and c.json_of(raw) == {"error": "ha_not_connected"},
                "[1] no_config: GET entities 503 ha_not_connected", (st, raw[:100]))
        st, h, raw = post_refresh()
        c.check(st == 503, "[1] no_config: POST refresh 503", (st, raw[:100]))
        c.check(catalog() == {"state": "none", "count": 0, "error": ""}, "[1] no_config: status catalog none/0/''",
                catalog())
        tokens.append("ent-test-token-unreach")
        set_ha(c, mock_url, tokens[-1])
        h, _ = wait_ha(c, "backoff", 30)
        c.check(h.get("state") == "backoff", "[1] unreachable mock -> backoff", h)
        st, h2, raw = get_ent()
        c.check(st == 503 and c.json_of(raw) == {"error": "ha_not_connected"}, "[1] backoff: GET entities 503",
                (st, raw[:100]))
        st, h2, raw = post_refresh()
        c.check(st == 503, "[1] backoff: POST refresh 503", (st, raw[:100]))
        c.check(catalog().get("state") == "none", "[1] backoff: catalog.state none", catalog())

        # ================= 2. Ready, nothing fetched
        print("  [2] Ready, nothing fetched yet")
        connect("--states-file", small_path, "--states-delay", 4)
        time.sleep(2)
        c.check(catalog().get("state") == "none", "[2] Ready, before any GET: catalog none", catalog())
        mark = slog.mark()
        t0 = time.time()
        st, h, raw = get_ent()
        c.check(st == 202 and c.json_of(raw) == {"pending": True}, "[2] first GET 202 {pending:true}", (st, raw[:80]))
        c.check(h.get("cache-control") == "no-store", "[2] 202 has Cache-Control: no-store", h.get("cache-control"))
        cat_seen, seen = [], [st]
        while time.time() - t0 < 14:
            time.sleep(0.2)
            cs = catalog().get("state")
            if not cat_seen or cat_seen[-1] != cs:
                cat_seen.append(cs)
            st, h, raw = get_ent()
            seen.append(st)
            if st != 202:
                break
        dt = time.time() - t0
        c.check(st == 200 and dt <= 10, "[2] 200 within 10 s", f"{st} after {dt:.1f} s")
        c.check(all(x == 202 for x in seen[:-1]), "[2] every GET before the 200 was 202", seen)
        c.check("fetching" in cat_seen, "[2] status catalog showed fetching meanwhile", cat_seen)
        time.sleep(1.0)
        n_req = serial_count(mark, "HA states request id=")
        c.check(n_req == 1, "[2] exactly one 'HA states request id=' line (first + repeated GETs)", n_req)
        c.check(h.get("cache-control") == "no-store", "[2] 200 has Cache-Control: no-store", h.get("cache-control"))
        c.check(int(h.get("content-length", -1)) == len(raw), "[2] Content-Length equals body size",
                (h.get("content-length"), len(raw)))
        c.check(h.get("content-type", "").startswith("application/json"), "[2] Content-Type application/json",
                h.get("content-type"))
        no_cors("[2] 200", h)
        body = check_body("[2]", raw, small)
        f0 = body["fetched_at_s"] if body else 0
        raw0 = raw
        cs = catalog()
        c.check(cs.get("state") == "ready" and body and cs.get("count") == len(body["entities"]) and cs.get("error") == "",
                "[2] status catalog ready, count == entities", cs)
        num(f"[2] GET count {len(seen)}, statuses {sorted(set(seen))}, catalog states {cat_seen}, 200 after {dt:.1f} s, "
            f"body {len(raw)} B, fetched_at_s {f0}")

        # ================= 3 / 12. Refresh
        print("  [3/12] Refresh")
        time.sleep(1.6)
        mark = slog.mark()
        st, h, raw = post_refresh()
        c.check(st == 202 and c.json_of(raw) == {"pending": True}, "[3] POST refresh 202 {pending:true}", (st, raw[:80]))
        st, h, raw = get_ent()
        c.check(st == 202, "[3] GET right after refresh is 202, never the old body", (st, raw[:60]))
        cs = catalog()
        c.check(cs.get("state") == "fetching" and cs.get("count") == 0 and cs.get("error") == "",
                "[3] status right after refresh: fetching, count 0, error ''", cs)
        time.sleep(1.0)
        st2, _, _ = post_refresh()
        time.sleep(0.5)
        st3, _, _ = post_refresh()
        c.check(st2 == 202 and st3 == 202, "[3] POST refresh while fetching answers 202", (st2, st3))
        seen, st, h, raw, dt = poll_200()
        c.check(st == 200, "[3/12] later 200 after the refresh", (seen[-3:], dt))
        c.check(all(x == 202 for x in seen[:-1]), "[3/12] only 202 until the new 200 (never an old 200)", seen)
        body2 = c.json_of(raw) or {}
        c.check(body2.get("fetched_at_s", 0) > f0, "[3/12] fetched_at_s larger than before",
                (f0, body2.get("fetched_at_s")))
        c.check(raw != raw0, "[3/12] body is the new one")
        time.sleep(1.0)
        n_req = serial_count(mark, "HA states request id=")
        n_web = serial_count(mark, "WEB entities refresh requested")
        c.check(n_req == 1, "[3] 3 refreshes (1 + 2 while fetching) -> exactly one 'states request'", n_req)
        c.check(n_web == 3, "[3] 'WEB entities refresh requested' logged per POST", n_web)
        num(f"[3/12] fetched_at_s {f0} -> {body2.get('fetched_at_s')}")

        # ================= 4. Failure: > 4 MB in one frame
        print("  [4] Failure (states_too_large)")
        connect("--states-synthetic", 4500000, "--states-count", 1500, "--states-fragment", 5000000,
                "--states-delay", 3)
        t0 = time.time()
        seen = []
        while time.time() - t0 < 60:
            st, h, raw = get_ent()
            seen.append(st)
            if st != 202:
                break
            time.sleep(0.5)
        dt = time.time() - t0
        c.check(st == 502 and c.json_of(raw) == {"error": "fetch_failed", "detail": "states_too_large"},
                "[4] GET 502 {error:fetch_failed, detail:states_too_large}", (st, raw[:120], seen[-3:]))
        cs = catalog()
        c.check(cs.get("state") == "failed" and cs.get("error") == "states_too_large",
                "[4] status catalog failed / states_too_large", cs)
        num(f"[4] 502 after {dt:.1f} s")
        time.sleep(1.2)
        st, h, raw = post_refresh()
        c.check(st == 202, "[4] POST refresh 202", (st, raw[:80]))
        st, h, raw = get_ent()
        c.check(st == 202, "[4] GET right after refresh is no longer 502", (st, raw[:80]))
        cs = catalog()
        c.check(cs.get("state") == "fetching" and cs.get("error") == "", "[4] status after refresh: fetching, error ''", cs)
        t0 = time.time()
        seen = [st]
        while time.time() - t0 < 60:
            time.sleep(0.5)
            st, h, raw = get_ent()
            seen.append(st)
            if st != 202:
                break
        c.check(st == 502 and set(seen[:-1]) == {202}, "[4] 202 until the new fetch fails again, then 502",
                (st, sorted(set(seen))))

        # ================= prepare a ready small catalog for guards / meta
        connect("--states-file", small_path)
        seen, st, h, raw, dt = poll_200()
        c.check(st == 200, "setup: small catalog ready for [5]/[6]", seen)
        mark = slog.mark()

        # ================= 5. Guards
        print("  [5] Guards")
        resp_headers = []
        st, h, raw = c.request("POST", "/api/ha/entities/refresh", body="{}", headers={"Content-Type": "application/json"})
        c.expect_status("[5] POST refresh without X-HAKnob 403", st, 403, raw)
        resp_headers.append(h)
        st, h, raw = c.request("POST", "/api/ha/entities/refresh", body="{}", headers={"X-HAKnob": "1"})
        c.expect_status("[5] POST refresh without Content-Type 403", st, 403, raw)
        resp_headers.append(h)
        st, h, raw = c.request("POST", "/api/ha/entities/refresh", body="{}",
                               headers={"X-HAKnob": "1", "Content-Type": "text/plain"})
        c.expect_status("[5] POST refresh with Content-Type text/plain 403", st, 403, raw)
        resp_headers.append(h)
        c.check(serial_count(mark, "WEB entities refresh requested") == 0, "[5] rejected POSTs did not request a refresh")
        st, h, raw = get_ent(host="evil.example")
        c.check(st in (403, 400, 404, 421) and b"entities" not in raw, "[5] GET entities with a wrong Host is rejected",
                (st, raw[:80]))
        print(f"      wrong Host GET -> {st}")
        resp_headers.append(h)
        st, h, raw = c.request("POST", "/api/ha/entities/refresh", body="{}", headers=post_headers(), host="evil.example")
        c.check(st in (403, 400, 404, 421), "[5] POST refresh with a wrong Host is rejected", (st, raw[:80]))
        resp_headers.append(h)
        for path in ("/api/ha/entities", "/api/meta/actions"):
            st, h, raw = c.request("HEAD", path)
            c.expect_status(f"[5] HEAD {path} 404", st, 404, raw)
            resp_headers.append(h)
        for path in ("/api/ha/entities/refresh", "/api/ha/entities/"):
            st, h, raw = get_ent(path)
            c.expect_status(f"[5] GET {path} 404", st, 404, raw)
            resp_headers.append(h)
        st, h, raw = get_ent("/api/ha/entities?x=1")
        c.expect_status("[5] GET /api/ha/entities?x=1 served (200)", st, 200, raw)
        resp_headers.append(h)
        st, h, raw = c.request("GET", "/api/meta/actions")
        resp_headers.append(h)
        c.expect_status("[5] GET /api/meta/actions 200", st, 200, raw)
        for hh in resp_headers:
            no_cors("[5] response", hh)
        # web password
        pw = "ent-" + secrets.token_hex(6)
        st, _, raw = c.request("POST", "/api/config", body=json.dumps({"web": {"password": pw}}), headers=post_headers())
        c.expect_status("[5] set web password 200", st, 200, raw)
        time.sleep(0.5)
        st, h, raw = get_ent()
        c.expect_status("[5] password set: GET entities without auth 401", st, 401, raw)
        c.check(b"entities\":[" not in raw, "[5] 401 body has no entity list")
        st, h, raw = c.request("GET", "/api/meta/actions")
        c.expect_status("[5] password set: GET meta without auth 401", st, 401, raw)
        n_before = serial_count(mark, "WEB entities refresh requested")
        st, h, raw = post_refresh()
        c.expect_status("[5] password set: POST refresh without auth 401", st, 401, raw)
        c.check(serial_count(mark, "WEB entities refresh requested") == n_before, "[5] 401 POST did not request a refresh")
        st, h, raw = get_ent(headers={"Authorization": basic(pw)})
        c.expect_status("[5] password set: GET entities with auth 200", st, 200, raw)
        st, h, raw = c.request("GET", "/api/meta/actions", headers={"Authorization": basic(pw)})
        c.expect_status("[5] password set: GET meta with auth 200", st, 200, raw)
        st, _, raw = c.request("POST", "/api/config", body=json.dumps({"web": {"password": None}}),
                               headers=post_headers({"Authorization": basic(pw)}))
        c.expect_status("[5] clear web password 200", st, 200, raw)
        pw = None
        time.sleep(0.5)
        st, h, raw = get_ent()
        c.expect_status("[5] password cleared: GET entities 200 without auth", st, 200, raw)

        # ================= 6. Meta
        print("  [6] Meta")
        exp = expected_meta_from_native_test(root)
        st, h, raw = c.request("GET", "/api/meta/actions")
        c.check(len(exp) > 500, "[6] expected string parsed from test_domainactions", len(exp))
        c.check(raw == exp.encode("utf-8"), "[6] /api/meta/actions byte-equals the native test's expected metaJson",
                (len(raw), len(exp)))
        c.check(int(h.get("content-length", -1)) == len(raw), "[6] meta Content-Length equals body", h.get("content-length"))
        meta = c.json_of(raw) or {}
        meta_actions = {d["domain"]: {a["name"] for a in d["actions"]} for d in meta.get("domains", [])}

        # ================= 8. Status fields (small mock, controls)
        print("  [8] Status")
        ids = [e["entity_id"] for e in small if e["entity_id"].split(".")[0] in ("light", "switch", "climate",
                                                                               "cover", "fan")][:8]

        def set_controls(lst):
            ctl = [{"entity_id": i, "label": "", "action": "none", "step": 0, "press": "none"} for i in lst]
            st, _, raw = c.request("POST", "/api/config", body=json.dumps({"controls": ctl}), headers=post_headers())
            c.expect_status(f"[8] controls {len(lst)} entries saved", st, 200, raw)

        def hb_ent(mark_):
            hb = hb_samples(slog, mark_)
            return hb[-1][1].get("ent") if hb else None

        def settle(want_sub, timeout=15):
            t_ = time.time()
            while time.time() - t_ < timeout:
                s_ = status().get("ha", {})
                if s_.get("subscribed") == want_sub and s_.get("entities", -1) >= 0:
                    time.sleep(0.6)
                    return status().get("ha", {})
                time.sleep(0.5)
            return status().get("ha", {})

        def hb_after_status():
            """The first HB line printed after now (HB every 5 s) as an ent=E/S match."""
            mk_ = slog.mark()
            t_ = time.time()
            while time.time() - t_ < 15:
                for l_ in slog.since(mk_):
                    if " HB " in l_:
                        return re.search(r"ent=(\d+)/(\d+)", l_)
                time.sleep(0.5)
            return None

        s0 = status().get("ha", {})
        c.check(s0.get("subscribed") == 0 and s0.get("entities") == 0, "[8] no controls: subscribed 0, entities 0",
                {k: s0.get(k) for k in ("subscribed", "entities")})
        set_controls([ids[0], ids[1], ids[0], ids[2]])
        s1 = settle(3)
        time.sleep(3)
        s1 = status().get("ha", {})
        m = hb_after_status()
        c.check(s1.get("subscribed") == 3, "[8] 4 controls with a duplicate -> subscribed 3 (unique ids)",
                s1.get("subscribed"))
        c.check(s1.get("entities") == 3, "[8] entities == 3 (snapshot delivered)", s1.get("entities"))
        c.check(m is not None and int(m.group(1)) == s1.get("entities") and int(m.group(2)) == s1.get("subscribed"),
                "[8] status entities/subscribed equal HB ent=E/S", (m.group(0) if m else None, s1.get("entities"),
                                                                    s1.get("subscribed")))
        set_controls(ids[:6])
        settle(6)
        time.sleep(3)
        s2 = status().get("ha", {})
        m = hb_after_status()
        c.check(s2.get("subscribed") == 6 and s2.get("entities") == 6, "[8] after changing controls: 6 / 6",
                {k: s2.get(k) for k in ("subscribed", "entities")})
        c.check(m is not None and int(m.group(1)) == s2.get("entities") and int(m.group(2)) == s2.get("subscribed"),
                "[8] status equals HB ent=E/S after the change", (m.group(0) if m else None,))
        c.check((s1.get("subscribed"), s1.get("entities")) != (s2.get("subscribed"), s2.get("entities")),
                "[8] both values changed after the controls change")
        num(f"[8] before {s1.get('subscribed')}/{s1.get('entities')}, after {s2.get('subscribed')}/{s2.get('entities')}")
        c.check(c.json_of(c.request('GET', '/api/ha/entities')[2]) is not None, "[8] catalog still served after controls change")
        set_controls([])
        settle(0)

        # ================= 7. Load (200 KB catalog)
        print("  [7] Load")
        connect("--states-file", big_path, "--states-delay", 10, "--states-rate", 400000)
        time.sleep(3)
        base0 = status()
        samples = []
        stop = threading.Event()

        def sampler():
            while not stop.is_set():
                s_ = safe_status()
                if s_:
                    samples.append((time.time(), s_.get("heap_free"), s_.get("heap_min_free")))
                time.sleep(0.3)

        smp = threading.Thread(target=sampler, daemon=True)
        smp.start()
        phases = {}

        def window(label, mk_, t_from, settle_s=7):
            time.sleep(settle_s)
            hb = hb_samples(slog, mk_)
            lm = [kv.get("loopmax", 0) for _, kv in hb]
            hm = [kv.get("heap_min", 0) for _, kv in hb]
            hl = [kv.get("heap", 0) for _, kv in hb]
            low = [h_ for t_, h_, _ in samples if t_ >= t_from and h_ is not None]
            phases[label] = (lm, hm)
            num(f"[7] {label}: HB n={len(hb)} loopmax {lm}, HB heap {hl}, HB heap_min {hm}, "
                f"lowest sampled heap_free {min(low) if low else None}")
            return lm, hm, hb

        # clean heap_min / loopmax baseline: reboot (the ha config and the mock stay)
        st, _, raw = c.request("POST", "/api/reboot", body="{}", headers=post_headers())
        c.expect_status("[7] reboot for a clean heap_min", st, 200, raw)
        time.sleep(6)
        t_ = time.time()
        while time.time() - t_ < 90:
            s_ = safe_status()
            if s_ and s_.get("uptime_s", 999) < 60 and s_.get("ha", {}).get("state") == "ready":
                break
            time.sleep(1)
        time.sleep(3)
        base0 = status()
        num(f"[7] after reboot: uptime {base0.get('uptime_s')} s, heap_free {base0.get('heap_free')}, "
            f"heap_min_free {base0.get('heap_min_free')}")
        # P0: first fetch via GET, no load
        mk0 = slog.mark()
        tp0 = time.time()
        seen, st, h, ref, dt = poll_200(timeout=30, interval=0.5)
        c.check(st == 200, "[7] P0 (no load) catalog 200", seen[-3:])
        window("P0 fetch without load", mk0, tp0)
        time.sleep(8)
        base = status()
        heap_before = base.get("heap_free")
        hmin_before = base.get("heap_min_free")
        ref_body = c.json_of(ref) or {}
        c.check(len(ref) > 180000, "[7] catalog is ~200 KB", len(ref))
        num(f"[7] catalog {len(ref)} B, {len(ref_body.get('entities', []))} entities; heap_free before-fetch "
            f"{base0.get('heap_free')}, before load {heap_before}, heap_min_free before load {hmin_before}")

        # P0b: refresh without any HTTP traffic meanwhile (attribution of loopmax to the fetch itself)
        time.sleep(1.2)
        mkb = slog.mark()
        tpb = time.time()
        post_refresh()
        time.sleep(17)
        window("P0b refresh, no HTTP traffic during it", mkb, tpb, settle_s=2)
        seen, st, h, ref, dt = poll_200()
        ref_body = c.json_of(ref) or {}

        # P1: 20 flooding clients during a refresh (fetching + building), then after
        time.sleep(1.2)
        mk1 = slog.mark()
        tp1 = time.time()
        flood = []
        flock = threading.Lock()
        fstop = threading.Event()
        catlog = []

        seqs = {}

        def flooder(tid):
            while not fstop.is_set():
                r_ = raw_get(ip, "/api/ha/entities")
                r_["tid"] = tid
                with flock:
                    flood.append(r_)
                    seqs.setdefault(tid, []).append(r_["status"])
                time.sleep(0.05)

        def catpoll():
            while not fstop.is_set():
                s_ = safe_status()
                if s_:
                    catlog.append((time.time(), s_.get("ha", {}).get("catalog", {}).get("state")))
                time.sleep(0.05)

        fts = [threading.Thread(target=flooder, args=(i_,), daemon=True) for i_ in range(20)] + [threading.Thread(target=catpoll, daemon=True)]
        st, _, raw = post_refresh()
        c.expect_status("[7] P1 refresh 202", st, 202, raw)
        [t.start() for t in fts]
        t_wait = time.time()
        while time.time() - t_wait < 40:
            with flock:
                got200 = any(r_["status"] == 200 for r_ in flood)
            if got200:
                break
            time.sleep(0.2)
        time.sleep(2.5)
        fstop.set()
        [t.join(40) for t in fts]
        res = [r_ for r_ in flood]
        tl = tally(res)
        res_all = res
        n_err = sum(1 for r_ in res if r_["t_hdr"] is None)
        res = [r_ for r_ in res if r_["t_hdr"] is not None] + [r_ for r_ in res if r_["t_hdr"] is None and False]
        t200 = [r_["t_hdr"] for r_ in res if r_["status"] == 200]
        first200 = min(t200) if t200 else None
        c.check(first200 is not None, "[7] P1 catalog 200 reached under flood", tl)
        before = [r_ for r_ in res if first200 and r_["t_hdr"] < first200 - 0.2]
        after = [r_ for r_ in res if first200 and r_["t_hdr"] >= first200 - 0.2]
        c.check(len(before) >= 20 and all(r_["status"] in (202, None) for r_ in before),
                "[7] P1 all GETs before the catalog was ready were 202 (or TCP refusal)", (tally(before), len(before)))
        bad_seq = []
        for tid, sq in seqs.items():
            seen200 = False
            for x_ in sq:
                if x_ == 200:
                    seen200 = True
                elif seen200 and x_ == 202:
                    bad_seq.append(tid)
                    break
        c.check(not bad_seq, "[7] P1 per client: once a 200 was received, no later GET is 202 (no refresh in between)",
                bad_seq)
        c.check(all(x_ in (202, 200, 503, None) for sq in seqs.values() for x_ in sq),
                "[7] P1 only 202 / 200 / 503 / refusal answered", tally(res))
        bw = []
        cur = None
        for t_, s_ in catlog:
            if s_ == "building" and cur is None:
                cur = t_
            elif s_ != "building" and cur is not None:
                bw.append((cur, t_))
                cur = None
        in_build = [r_ for r_ in res if r_["t_hdr"] and any(a - 0.05 <= r_["t_hdr"] <= b + 0.05 for a, b in bw)]
        num(f"[7] P1 flood of 20 clients: {len(res_all)} GETs ({n_err} refused/failed), {tl}; 'building' windows {[round(b - a, 2) for a, b in bw]} s, "
            f"GETs answered during them: {tally(in_build)}")
        f200 = [r_ for r_ in res if r_["status"] == 200 and not r_["err"]]
        c.check(len({r_["body"] for r_ in f200}) == 1 and all(len(r_["body"]) == int(r_["headers"]["content-length"])
                                                                for r_ in f200),
                "[7] P1 all complete 200 bodies byte-identical and equal to Content-Length", (len(f200),))
        s503 = [r_ for r_ in res if r_["status"] == 503]
        c.check(all(json_ok(r_["body"]) == {"error": "busy"} and r_["headers"].get("retry-after") == "1" for r_ in s503),
                "[7] P1 503 bodies are {\"error\":\"busy\"} with Retry-After: 1", len(s503))
        ovs, ovt = max_overlap(f200), max_overlap(f200, tail=True)
        num(f"[7] P1 200 streams: {len(f200)}, 503 busy {len(s503)}; max overlap {ovs} "
            f"(excluding the last 12 KB of each stream: {ovt})")
        c.check(ovt <= 4, "[7] P1 at most 4 concurrent 200 streams (overlap excluding each stream's last 12 KB)", ovt)
        window("P1 flood during refresh", mk1, tp1)
        time.sleep(3)
        seen, st, h, ref, dt = poll_200()
        ref_body = c.json_of(ref) or {}
        c.check(st == 200 and all(r_["body"] == ref for r_ in f200), "[7] P1 flood 200 bodies equal the current catalog", st)

        # P2: after ready: 20 parallel slow readers, 20 fast, aborts
        mk2 = slog.mark()
        tp2 = time.time()
        r = wave(ip, 20, chunk=2048, delay=0.02)
        tl = tally(r)
        s200 = [x for x in r if x["status"] == 200 and not x["err"]]
        s503 = [x for x in r if x["status"] == 503]
        refused = [x for x in r if x["status"] is None]
        c.check(len(s200) >= 1 and all(x["status"] in (200, 503, None) for x in r),
                "[7] P2 20 parallel slow GETs: 200 / 503 busy / TCP refusal only, at least one 200", tl)
        ovs, ovt = max_overlap(s200), max_overlap(s200, tail=True)
        c.check(ovt <= 4, "[7] P2 at most 4 concurrent 200 streams (overlap excluding each stream's last 12 KB)", ovt)
        c.check(all(x["body"] == ref for x in s200), "[7] P2 every completed 200 body is byte-identical",
                [len(x["body"]) for x in s200 if x["body"] != ref][:3])
        c.check(all(json_ok(x["body"]) == {"error": "busy"} and x["headers"].get("retry-after") == "1" for x in s503),
                "[7] P2 503 bodies are {\"error\":\"busy\"} with Retry-After: 1",
                [(x["body"][:40], x["headers"].get("retry-after")) for x in s503][:2])
        c.check(len(s503) + len(refused) >= 1, "[7] P2 some of the 20 were turned away (cap effective)", tl)
        num(f"[7] P2 slow wave: {tl}, max overlap of 200 streams {ovs} (excluding last 12 KB: {ovt})")
        time.sleep(1.0)
        r = wave(ip, 20)
        tl = tally(r)
        s200 = [x for x in r if x["status"] == 200 and not x["err"]]
        c.check(len(s200) >= 1 and all(x["status"] in (200, 503, None) for x in r) and all(x["body"] == ref for x in s200),
                "[7] P2 20 parallel fast GETs: 200 / 503 / refusal only, bodies identical", tl)
        num(f"[7] P2 fast wave: {tl}, max overlap {max_overlap(s200) if s200 else 0} "
            f"(excluding last 12 KB: {max_overlap(s200, tail=True) if s200 else 0})")
        time.sleep(1.0)

        aborts = []
        gate = threading.Barrier(4)

        def ab(i):
            gate.wait(10)
            aborts.append(raw_get(ip, "/api/ha/entities", chunk=1024, delay=0.0, abort_after=3000, rst=(i % 2 == 0)))

        ts = [threading.Thread(target=ab, args=(i,)) for i in range(4)]
        [t.start() for t in ts]
        [t.join(30) for t in ts]
        c.check(len(aborts) == 4 and all(a["aborted"] for a in aborts), "[7] 4 streams aborted mid-body (2 RST, 2 FIN)",
                [(a["status"], a["aborted"], a["err"]) for a in aborts])
        t_ab = time.time()
        got4 = None
        tries = 0
        while time.time() - t_ab < 8:
            tries += 1
            rr = wave(ip, 4, chunk=4096, delay=0.005)
            if all(x["status"] == 200 and x["body"] == ref for x in rr):
                got4 = time.time() - t_ab
                break
            time.sleep(0.5)
        c.check(got4 is not None, "[7] after aborting 4 streams, 4 new parallel 200s succeed (slot released)",
                f"tries={tries}")
        num(f"[7] 4 new parallel 200 streams complete {got4 if got4 is None else round(got4, 2)} s after the aborts "
            f"(tries {tries})")
        window("P2 slow/fast waves + aborts", mk2, tp2)

        # P3: refresh replaces the catalog mid-stream
        mk3_ = slog.mark()
        tp3 = time.time()
        res_mid = [None] * 4
        started = threading.Barrier(5)

        def slow(i):
            started.wait(10)
            res_mid[i] = raw_get(ip, "/api/ha/entities", chunk=2048, delay=0.04)

        ts = [threading.Thread(target=slow, args=(i,)) for i in range(4)]
        [t.start() for t in ts]
        started.wait(10)
        time.sleep(1.5)
        st, _, raw = post_refresh()
        c.expect_status("[7] P3 refresh while 4 streams are mid-body: 202", st, 202, raw)
        [t.join(60) for t in ts]
        c.check(all(x["status"] == 200 and x["body"] == ref for x in res_mid),
                "[7] P3 streams running during the refresh complete with the old body, byte-identical",
                [(x["status"], len(x["body"]), x["err"]) for x in res_mid])
        seen, st, h, raw, dt = poll_200(timeout=25)
        c.check(st == 200 and (c.json_of(raw) or {}).get("fetched_at_s", 0) > ref_body.get("fetched_at_s", 0),
                "[7] P3 new catalog served after the refresh with larger fetched_at_s",
                (st, (c.json_of(raw) or {}).get("fetched_at_s"), ref_body.get("fetched_at_s")))
        nb = c.json_of(raw) or {}
        c.check(raw != ref and nb.get("entities") == ref_body.get("entities")
                and {k: v for k, v in nb.items() if k != "fetched_at_s"} == {k: v for k, v in ref_body.items() if k != "fetched_at_s"},
                "[7] P3 new body differs from the old one only in fetched_at_s")
        window("P3 refresh mid-stream", mk3_, tp3)

        # P4: refresh while building (polls for 'building')
        time.sleep(1.2)
        mk4 = slog.mark()
        tp4 = time.time()
        post_refresh()
        saw_build = False
        t_ = time.time()
        while time.time() - t_ < 40:
            cs = (safe_status() or {}).get("ha", {}).get("catalog", {})
            if cs.get("state") == "building":
                saw_build = True
                st, _, _ = post_refresh()
                break
            if cs.get("state") == "ready":
                break
            time.sleep(0.02)
        seen, st, h, raw, dt = poll_200(timeout=25)
        time.sleep(1.0)
        n_req = serial_count(mk4, "HA states request id=")
        c.check(n_req == 1 and st == 200, "[7] P4 refresh%s: exactly one 'states request' and a 200 afterwards"
                % (" while building" if saw_build else " (building NOT observed by polling)"), (n_req, st, seen[-3:]))
        num(f"[7] P4 'building' observed by status polling: {saw_build}; requests {n_req}")
        window("P4 refresh/building", mk4, tp4)

        stop.set()
        smp.join(5)
        rec = None
        for _ in range(40):
            s_ = safe_status()
            if s_ and s_.get("heap_free") is not None and heap_before is not None and s_["heap_free"] >= heap_before - 4096:
                rec = s_
                break
            time.sleep(0.5)
        heaps = [h_ for t_, h_, _ in samples if h_ is not None and t_ >= tp1]
        fin = rec or safe_status() or {}
        c.check(rec is not None, "[7] heap_free recovers to within 4 KB of the pre-load value",
                f"before={heap_before} after={fin.get('heap_free')}")
        num(f"[7] heap_free before load {heap_before}, lowest sampled during load {min(heaps) if heaps else None} "
            f"({len(heaps)} samples), after {fin.get('heap_free')}; heap_min_free before {hmin_before}, after "
            f"{fin.get('heap_min_free')}")
        hb = hb_samples(slog, mk1)
        ups = [kv.get("up", 0) for _, kv in hb]
        gaps = [b - a for a, b in zip(ups, ups[1:])]
        c.check(len(hb) >= 5 and max(gaps or [99]) <= 6, "[7] HB continued during the whole load (max up= gap <= 6 s)",
                (len(hb), gaps))
        lines = slog.since(mk0)
        c.check(not any(" BOOT" in x or "Guru" in x or "Backtrace" in x or "rst:0x" in x for x in lines),
                "[7] no reboot / panic on serial during the load")
        c.check((safe_status() or {}).get("uptime_s", 0) > 60, "[7] device did not reboot (uptime)")
        for lab, (lm, hm) in phases.items():
            c.check(max(lm or [999]) <= 25, f"[7] loopmax <= 25 ms in every HB: {lab}", lm)
        num(f"[7] max HB gap during load {max(gaps or [0]):.1f} s")

        # ================= 10 / 11. Refresh races a dropping HA
        print("  [10/11] Refresh vs dropping HA connection")
        connect("--states-file", small_path)
        classes = {"rst": {}, "wsclose": {}}

        def fire_post_at(t_fire, box):
            """Opens the TCP connection first, sends the POST headers at t_fire (perf_counter)."""
            s_ = socket.create_connection((ip, 80), timeout=10)
            req = (f"POST /api/ha/entities/refresh HTTP/1.1\r\nHost: {ip}\r\nContent-Type: application/json\r\n"
                   f"X-HAKnob: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").encode()
            while time.perf_counter() < t_fire:
                pass
            box["t_post"] = time.perf_counter()
            s_.sendall(req)
            data = b""
            try:
                while True:
                    d_ = s_.recv(4096)
                    if not d_:
                        break
                    data += d_
            except OSError:
                pass
            s_.close()
            box["status"] = int(data.split(b" ", 2)[1]) if data.startswith(b"HTTP/") else None
            box["body"] = data.split(b"\r\n\r\n", 1)[-1]

        def race(kind, offset_ms):
            wait_ha(c, "ready", 60)
            time.sleep(0.5)
            seen_, st_, h_, raw_, dt_ = poll_200()
            if st_ != 200:
                return None
            old = (c.json_of(raw_) or {}).get("fetched_at_s")
            time.sleep(1.3)
            mk_race = slog.mark()
            box = {}
            base_t = time.perf_counter() + 0.15
            t_cmd = base_t + offset_ms / 1000.0
            th = threading.Thread(target=fire_post_at, args=(base_t, box))
            th.start()
            while time.perf_counter() < t_cmd - 0.003:
                pass
            mock_cmd(kind)
            th.join(15)
            post_st = box.get("status")
            bad_old = 0
            obs = []
            t_ = time.time()
            last_get = (None, {}, b"")
            while time.time() - t_ < 2.5:
                try:
                    g = get_ent()
                    last_get = g
                    if g[0] == 200 and (c.json_of(g[2]) or {}).get("fetched_at_s") == old and post_st == 202:
                        bad_old += 1
                    ss = (safe_status() or {}).get("ha", {})
                    cs_ = ss.get("catalog", {})
                    obs.append((g[0], ss.get("state"), cs_.get("state"), cs_.get("count"), cs_.get("error")))
                except Exception as e:
                    obs.append(("exc", repr(e)[:30], None, None, None))
                time.sleep(0.1)
            if post_st != 202:
                cls = f"post{post_st}"
            elif any(o[2] == "failed" for o in obs):
                cls = "failed"
            elif any(o[1] not in ("ready", None) and o[2] == "none" for o in obs):
                cls = "hidden"
            else:
                cls = "normal"
            req_line = serial_count(mk_race, "HA states request id=") > 0
            if post_st == 202:
                c.check(bad_old == 0, f"[10/11] {kind} {offset_ms:+d} ms: old list never served as 200 after a 202 POST",
                        bad_old)
            if cls == "failed":
                gg = [o for o in obs if o[2] == "failed"][0]
                c.check(gg[4] == "connection_lost" and gg[0] == 502,
                        f"[10] {kind} {offset_ms:+d} ms: catalog failed / connection_lost and GET 502", gg)
                wait_ha(c, "ready", 60)
                time.sleep(0.5)
                pst, _, _ = post_refresh()
                seen2, st2, h2, raw2, dt2 = poll_200()
                f2 = (c.json_of(raw2) or {}).get("fetched_at_s", 0)
                c.check(pst == 202 and st2 == 200 and f2 > (old or 0),
                        "[10] after reconnect: refresh -> 202, then 200 with a new fetched_at_s", (pst, st2, f2, old))
            elif cls == "hidden":
                gg = [o for o in obs if o[2] == "none" and o[1] not in ("ready", None)][0]
                c.check(gg[3] == 0 and gg[4] == "" and gg[0] == 503,
                        f"[11] {kind} {offset_ms:+d} ms: catalog none/0/'' and GET 503 ha_not_connected", gg)
                wait_ha(c, "ready", 60)
                time.sleep(0.5)
                seen2, st2, h2, raw2, dt2 = poll_200()
                time.sleep(1)
                f2 = (c.json_of(raw2) or {}).get("fetched_at_s", 0)
                n2 = serial_count(mk_race, "HA states request id=")
                c.check(st2 == 200 and n2 == 1 and f2 > (old or 0),
                        "[11] after HA Ready again: one fetch (exactly one 'states request' since the POST), then 200 "
                        "with a new fetched_at_s (never the old list)", (st2, n2, f2, old, seen2[:3]))
            return cls, post_st, obs, last_get, req_line

        for kind in ("rst", "wsclose"):
            for off in range(-12, 13, 2):
                try:
                    out_ = race(kind, off)
                except Exception as e:
                    c.check(False, f"[10/11] {kind} {off:+d} ms raised", repr(e))
                    continue
                if out_ is None:
                    classes[kind].setdefault("no-catalog", []).append(off)
                    continue
                cls, post_st, obs, last_get, req_line = out_
                if cls == "failed":
                    cls = "failed_after_request" if req_line else "failed_send_failure(no request line)"
                classes[kind].setdefault(cls, []).append(off)
                print(f"      {kind} {off:+d} ms: POST {post_st} class {cls}; last GET {last_get[0]}; "
                      f"(ha,catalog) {sorted({(o[1], o[2]) for o in obs}, key=str)}")
        for kind in classes:
            num(f"[10/11] {kind} offsets (ms, mock command minus POST) by outcome class: {classes[kind]}")
        c.check(any(k.startswith("failed") or k == "hidden" for k in classes["rst"]),
                "[10] (best effort) RST: the race hit the failed/hidden path at least once", classes["rst"])
        c.check(any(k.startswith("failed") or k == "hidden" for k in classes["wsclose"]),
                "[11] (best effort) ws Close: the race hit the 'HA left Ready after the POST' path at least once",
                classes["wsclose"])
        wait_ha(c, "ready", 60)

        # ================= 9. Secrets
        print("  [9] Secrets")
        text = "\n".join(slog.since(t_start))
        for tok in tokens:
            c.check(tok not in text, f"[9] serial log never contains the token value (...{tok[-6:]})")
        c.check("access_token" not in text, "[9] serial log never contains access_token")
        c.check("token_set len=" in text or True, "[9] (info) token_set len= lines present: %s" % ("token_set len=" in text))
        st, _, raw = c.request("GET", "/api/status")
        c.check(not any(t in raw.decode() for t in tokens), "[9] /api/status has no token")
        st, _, raw = get_ent()
        c.check(not any(t in raw.decode() for t in tokens), "[9] /api/ha/entities has no token")
    finally:
        stop_mock()
        if pw:
            c.request("POST", "/api/config", body=json.dumps({"web": {"password": None}}),
                      headers=post_headers({"Authorization": basic(pw)}))
        c.request("POST", "/api/config", body=json.dumps({"web": {"password": None}}), headers=post_headers())
        c.request("POST", "/api/config", body=json.dumps({"controls": []}), headers=post_headers())
        st, _, raw = set_ha(c, "", None)
        c.expect_status("restore: ha url '' + token null 200", st, 200, raw)
        h, _ = wait_ha(c, "no_config", 15)
        c.check(h.get("state") == "no_config", "restore: ha.state no_config", h)
        st, _, raw = c.request("GET", "/api/config")
        cfg = c.json_of(raw) or {}
        c.check(cfg.get("ha", {}).get("url") == "" and cfg.get("ha", {}).get("token_set") is False
                and cfg.get("controls") == [] and cfg.get("web", {}).get("password_set") is False,
                "restore: ha cleared, controls [], web password cleared",
                (cfg.get("ha"), cfg.get("controls"), cfg.get("web")))
        print("\n      --- numbers ---")
        for n in numbers:
            print("      " + n)


def json_ok(b):
    try:
        return json.loads(b.decode("utf-8"))
    except Exception:
        return None
