"""Mock of the HA WebSocket API for on-device checks (Steps 5, 5b.2 and 5b.4; not a unit test).

Run with a Python the Windows firewall allows on the LAN, e.g.
  "%LOCALAPPDATA%\\Python\\<your Python>\\python.exe" tools/mockha.py --seconds 300

Basic options
  --port P            listen port (default 8765)
  --token T           the accepted access token (default mock-good-token)
  --seconds N         run time (default 60)
  --no-pong           never answer the device's "ping" messages
Scenario options (Step 5b.2; ws:// only)
  --upgrade-status C  answer the WebSocket upgrade with HTTP status C (e.g. 502) instead of 101
  --close-before-101  accept the TCP connection and close it without answering the upgrade
  --coalesce-handshake  send the 101 response and the auth_required frame in one TCP write
  --fragment-auth-ok  send auth_ok as 3 fragments with a ws PING between them (and a pause)
  --after-auth-delay S  seconds between auth_ok and the actions below (default 1.0)
Actions after auth_ok, in this order (all after --after-auth-delay):
  --ws-ping-after-auth  send a ws PING (payload "hb-123"); the PONG payload is logged
  --big-after-auth BYTES [BYTES ...]  send a synthetic type=result text frame of exactly BYTES
                        bytes (id 999) per value, 2 s apart (--big-fragment-bytes N sends it in
                        N-byte fragments instead of one frame)
  --kill-mid-transfer BYTES  announce a BYTES text frame, send half of it, close the socket
  --stall-mid-transfer BYTES  announce a BYTES text frame, send half of it, keep the socket open
  --rst-after-auth    abort the connection with a TCP RST
  --close-after-auth CODE  send a ws Close frame with status CODE (e.g. 1001)
  --close-after-auth-nocode  send a ws Close frame with an empty body
The mock logs "RX PONG payload=..." for ws pongs and "RX CLOSE code=..." for the device's
Close reply (code=None when the body was empty), and "ERROR client frame not masked".
TLS options (Step 5b.4; the mock then speaks wss)
  --tls-cert F --tls-key F  wrap accepted sockets with TLS (Python ssl, TLS 1.2 and 1.3 allowed)
  --tls13-only        require TLS 1.3 (the device only speaks 1.2: handshake must fail)
  --stall             accept the TCP connection and never answer the TLS ClientHello
                      (also without --tls-cert); the socket is closed after 20 s
Per connection the mock logs "TLS handshake ok/failed ..."; a rejected certificate shows up as
"TLS handshake failed" and no "REQ" line (no WebSocket request, no auth, no token sent).
Entity/state options (Step 6.5)
  --states-file F     JSON file with the entity state objects ([{"entity_id":..,"state":..,
                      "attributes":{..}},..]) used for get_states and subscribe_entities
  --states-synthetic BYTES  generate the list instead; a get_states result of about BYTES bytes
                      (junk attributes pad each entity; they are filtered out on the device)
  --states-count N    number of synthetic entities (default 300); entity i is
                      <domain>.mock_<i> with domain = light, switch, climate, cover, fan,
                      sensor, media_player for i % 7 = 0..6
  --states-fragment N get_states is sent as N-byte fragments (default 16384); a pong may be
                      sent between fragments
  --pong-after-states hold the send lock for the whole get_states message so a pong (or any
                      other frame) is queued behind it
  --states-rate BPS   throttle get_states to about BPS bytes/s
  --states-after-pings N  answer get_states only after N pings were answered, plus 1 s
  --states-delay S    wait S seconds before the first get_states fragment (< 15 s)
  --stall-states      send half of the get_states message, then nothing at all (no pong)
  --change-every S    every S seconds send a "c" diff (state + brightness) for the first
                      entity of each subscription
  --rm-add-every S    every S seconds remove ("r") and then re-add ("a") the last entity of each
                      subscription, so the device's ent= count moves (c diffs alone do not)
  --deep-result N [N ..]  after auth send a small type=result (id 998) nested N levels, 2 s apart
Step 6.6 option
  --stdin-commands    read commands from stdin, one per line: "rst" aborts every open connection
                      with a TCP RST (SO_LINGER 0), "wsclose" sends a ws Close frame (1001) on
                      every open connection (used to race a refresh against a dropping HA)
Step 7.6 call_service options (every call is logged "CALL id=<n> <domain>.<service> entity=<e>
data=<json>"; the default answer is HA's success result with a context)
  --call-log FILE     append one JSON line per call: time, n (running count), id, domain,
                      service, entity, data
  --call-delay S      answer each call after S seconds
  --call-error CODE:MSG  answer with success=false, error {code, message} (every call, or only
                      every Nth with --call-error-every N)
  --call-silent       never answer calls
  --call-close-after N  drop the connection (no answer) when the Nth call (count over the whole
                      mock run) arrives
  --call-apply        update the fixture entity from the call and send a "c" diff to the
                      subscriptions containing it after --echo-delay S (default 0.2); skipped
                      for error replies. Handles turn_on (brightness_pct, hs_color,
                      color_temp_kelvin), turn_off, toggle, set_temperature, set_cover_position,
                      set_percentage, volume_set, volume_mute, media_play_pause
  --echo-stale        first send the previous value of the changed attributes, then the new one
                      --echo-stale-gap S (default 0.5) later
Step 8.3 options
  --auth-invalid-message TEXT  the auth_invalid message (default "Invalid access token or password");
                      JSON-escaped, so a real newline can be sent from a Python string
  --auth-ok-delay S   wait S seconds before answering a valid auth with auth_ok (device stays
                      Authenticating)
  --set-state-after S ENTITY STATE  S seconds after each subscribe, send a "c" diff that sets the
                      entity's state (e.g. unavailable)
  --unsubscribe-error TEXT  answer unsubscribe_events with success=false and this message
Logs: "RX get_states", "SENT states <bytes> in <s>", "SENT c-diff", "RX unsubscribe_events".
It answers subscribe_entities with a success result (and, with a fixture, an "a" event). The access token is never printed (only
its length).
"""
import argparse, base64, hashlib, json, socket, ssl, struct, threading, time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def recv_exact(c, n):
    b = b""
    while len(b) < n:
        d = c.recv(n - len(b))
        if not d:
            raise ConnectionError("closed")
        b += d
    return b


def recv_frame(c):
    h = recv_exact(c, 2)
    fin = bool(h[0] & 0x80)
    op = h[0] & 0x0F
    ln = h[1] & 0x7F
    masked = bool(h[1] & 0x80)
    if ln == 126:
        ln = struct.unpack(">H", recv_exact(c, 2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", recv_exact(c, 8))[0]
    mask = recv_exact(c, 4) if masked else b"\0\0\0\0"
    data = bytearray(recv_exact(c, ln))
    for i in range(ln):
        data[i] ^= mask[i % 4]
    return fin, op, masked, bytes(data)


def hdr(op, n, fin=True):
    h = bytes([(0x80 if fin else 0) | op])
    if n < 126:
        return h + bytes([n])
    if n < 65536:
        return h + bytes([126]) + struct.pack(">H", n)
    return h + bytes([127]) + struct.pack(">Q", n)


def frame(op, payload, fin=True):
    return hdr(op, len(payload), fin) + payload


def text(obj):
    return frame(1, json.dumps(obj).encode())


def big_payload(n):
    """A {"id":999,"type":"result",...} message of exactly n bytes."""
    head = b'{"id":999,"type":"result","success":true,"result":"'
    tail = b'"}'
    return head + b"x" * (n - len(head) - len(tail)) + tail


def make_states(count, total_bytes):
    """Synthetic HA state list: count entities, padded with junk attributes to ~total_bytes."""
    doms = ["light", "switch", "climate", "cover", "fan", "sensor", "media_player"]
    out = []
    for i in range(count):
        d = doms[i % 7]
        if d == "light":
            k = i // 7
            modes = ["onoff"] if k % 3 == 0 else (["brightness"] if k % 3 == 1 else ["color_temp", "hs"])
            attrs = {"supported_color_modes": modes, "friendly_name": f"Køkken lys {i}", "brightness": 128}
            if "hs" in modes:
                attrs["hs_color"] = [30.0, 50.0]
                attrs["color_temp_kelvin"] = 3000
            st = "on"
        elif d == "climate":
            attrs = {"supported_features": 1, "temperature": 21.5, "current_temperature": 20.0,
                     "min_temp": 7, "max_temp": 35, "target_temp_step": 0.5, "friendly_name": f"Climate {i}"}
            st = "heat"
        elif d == "cover":
            attrs = {"supported_features": 15, "current_position": 40, "friendly_name": f"Cover {i}"}
            st = "open"
        elif d == "fan":
            attrs = {"supported_features": 1, "percentage": 50, "percentage_step": 33.33, "friendly_name": f"Fan {i}"}
            st = "on"
        elif d == "media_player":
            attrs = {"supported_features": 4, "volume_level": 0.42, "is_volume_muted": False, "friendly_name": f"Media {i}"}
            st = "playing"
        elif d == "switch":
            attrs = {"friendly_name": f"Switch {i}"}
            st = "off"
        else:
            attrs = {"friendly_name": f"Sensor {i}", "unit_of_measurement": "W"}
            st = "123"
        out.append({"entity_id": f"{d}.mock_{i}", "state": st, "attributes": attrs,
                    "last_changed": "2026-10-16T10:00:00+00:00", "last_updated": "2026-10-16T10:00:00+00:00",
                    "context": {"id": "01ABC", "parent_id": None, "user_id": None}})
    base = len(json.dumps(out, separators=(",", ":")))
    pad = max(0, (total_bytes - base) // max(1, count))
    if pad > 0:
        for e in out:
            left, k = pad, 0
            while left > 0:
                n = min(left, 60000)
                e["attributes"][f"junk{k}"] = "j" * max(0, n - 12)
                left -= n
                k += 1
    return out


CALL_LOCK = threading.Lock()   # guards CALL_COUNT, the call log file and entity mutation
CALL_COUNT = [0]


def _find(states, eid):
    for e in states or []:
        if e["entity_id"] == eid:
            return e
    return None


def apply_call(states, domain, service, eid, data):
    """Applies a call_service to the mock entity (--call-apply). Returns (old, new) dicts of
    {"s": state, "a": {changed attrs}} or None when nothing changed / unknown entity."""
    e = _find(states, eid)
    if e is None:
        return None
    a = e["attributes"]
    old = {"s": e["state"], "a": {}}
    new = {"s": e["state"], "a": {}}

    def setattr_(k, v):
        old["a"][k] = a.get(k)
        new["a"][k] = v
        a[k] = v

    st = e["state"]
    if service == "turn_on":
        new["s"] = "heat" if domain == "climate" else ("playing" if domain == "media_player" else "on")
        if "brightness_pct" in data:
            setattr_("brightness", int(data["brightness_pct"] * 255 / 100 + 0.5))
        if "hs_color" in data:
            setattr_("hs_color", list(data["hs_color"]))
        if "color_temp_kelvin" in data:
            setattr_("color_temp_kelvin", data["color_temp_kelvin"])
    elif service == "turn_off":
        new["s"] = "off"
    elif service == "toggle":
        if domain == "cover":
            new["s"] = "closed" if st in ("open", "opening") else "open"
        elif domain == "media_player":
            new["s"] = "off" if st != "off" else "on"
        else:
            new["s"] = "off" if st == "on" else "on"
    elif service == "set_temperature":
        setattr_("temperature", data.get("temperature"))
    elif service == "set_cover_position":
        setattr_("current_position", data.get("position"))
    elif service == "set_percentage":
        p = data.get("percentage")
        setattr_("percentage", p)
        new["s"] = "off" if p == 0 else "on"
    elif service == "volume_set":
        setattr_("volume_level", data.get("volume_level"))
    elif service == "volume_mute":
        setattr_("is_volume_muted", data.get("is_volume_muted"))
    elif service == "media_play_pause":
        new["s"] = "paused" if st == "playing" else "playing"
    e["state"] = new["s"]
    return old, new


def load_states(args):
    if args.states_file:
        with open(args.states_file, encoding="utf-8") as f:
            return json.load(f)
    if args.states_synthetic:
        return make_states(args.states_count, args.states_synthetic)
    return None


def handle(c, addr, args):
    t0 = time.time()
    log = lambda *a: print(f"[{time.time()-t0:7.2f}s {addr[0]}]", *a, flush=True)
    send_lock = threading.Lock()

    def send(b):
        with send_lock:
            c.sendall(b)

    states = args.states_list
    pings_answered = [0]
    subs = {}  # subscribe id -> entity ids

    def compressed(e):
        a = {k: v for k, v in e["attributes"].items() if not k.startswith("junk")}
        return {"s": e["state"], "a": a, "c": "01ABC", "lc": 1760000000.0}

    def send_states(mid):
        """Answers get_states with the whole list, in fragments (see --pong-after-states)."""
        try:
            if args.states_delay:
                time.sleep(args.states_delay)
            if args.states_after_pings:
                while pings_answered[0] < args.states_after_pings:
                    time.sleep(0.2)
                time.sleep(1.0)
            body = json.dumps({"id": mid, "type": "result", "success": True, "result": states or []},
                              separators=(",", ":")).encode()
            n = len(body)
            step = args.states_fragment
            ts = time.time()
            hold = args.pong_after_states
            if hold:
                send_lock.acquire()
            try:
                i = 0
                while i < n:
                    if args.stall_states and i >= n // 2:
                        log(f"STALL states at {i}/{n} bytes (nothing more is sent)")
                        if not hold:
                            send_lock.acquire()
                        time.sleep(3600)
                        return
                    chunk = frame(1 if i == 0 else 0, body[i:i + step], i + step >= n)
                    if hold:
                        c.sendall(chunk)
                    else:
                        send(chunk)
                    i += step
                    if args.states_rate:
                        time.sleep(step / args.states_rate)
            finally:
                if hold:
                    send_lock.release()
            log(f"SENT states {n} bytes in {time.time()-ts:.2f}s")
        except OSError as e:
            log("states send aborted:", e)

    def send_diff(eid, part):
        """Sends a "c" diff for eid to this connection's subscriptions that contain it."""
        diff = {"+": {"a": part["a"], "lc": time.time()}}
        diff["+"]["s"] = part["s"]
        for sid, ids in list(subs.items()):
            if eid in ids:
                send(text({"id": sid, "type": "event", "event": {"c": {eid: diff}}}))
                log(f"SENT c-diff sid={sid} {eid} state={part['s']} attrs={json.dumps(part['a'], separators=(',', ':'))}")

    def answer_call(msg, n, domain, service, eid, data):
        try:
            if args.call_delay:
                time.sleep(args.call_delay)
            err = None
            if args.call_error:
                every = args.call_error_every or 1
                if n % every == 0:
                    err = args.call_error.split(":", 1)
                    err = {"code": err[0], "message": err[1] if len(err) > 1 else ""}
            if err is None and args.call_apply:
                with CALL_LOCK:
                    res = apply_call(states, domain, service, eid, data or {})
                if res is not None:
                    old, new = res
                    threading.Thread(target=echo, args=(eid, old, new), daemon=True).start()
            if args.call_silent:
                log(f"call {n}: not answering")
                return
            if err:
                send(text({"id": msg["id"], "type": "result", "success": False, "error": err}))
                log(f"SENT call {n} error {err['code']}")
            else:
                send(text({"id": msg["id"], "type": "result", "success": True,
                           "result": {"context": {"id": "01MOCK%d" % n, "parent_id": None, "user_id": None},
                                      "response": None}}))
                log(f"SENT call {n} ok")
        except OSError as e:
            log("call answer aborted:", e)

    def echo(eid, old, new):
        try:
            time.sleep(args.echo_delay)
            if args.echo_stale and old["a"]:
                send_diff(eid, {"s": old["s"], "a": old["a"]})
                time.sleep(args.echo_stale_gap)
            send_diff(eid, new)
        except OSError as e:
            log("echo aborted:", e)

    def rm_add_loop(sid, entity):
        eid = entity["entity_id"]
        while sid in subs:
            time.sleep(args.rm_add_every)
            if sid not in subs:
                return
            try:
                send(text({"id": sid, "type": "event", "event": {"r": [eid]}}))
                log(f"SENT r sid={sid} {eid}")
                time.sleep(args.rm_add_every)
                if sid not in subs:
                    return
                send(text({"id": sid, "type": "event", "event": {"a": {eid: compressed(entity)}}}))
                log(f"SENT a sid={sid} {eid}")
            except OSError:
                return

    def change_loop(sid, eid):
        k = 0
        while sid in subs:
            time.sleep(args.change_every)
            if sid not in subs:
                return
            k += 1
            diff = {"+": {"s": "off" if k % 2 else "on", "a": {"brightness": (k * 17) % 255},
                          "lc": 1760000000.0 + k}}
            try:
                send(text({"id": sid, "type": "event", "event": {"c": {eid: diff}}}))
                log(f"SENT c-diff sid={sid} {eid} k={k}")
            except OSError:
                return

    req = b""
    while b"\r\n\r\n" not in req:
        d = c.recv(1024)
        if not d:
            return
        req += d
    lines = req.decode(errors="replace").split("\r\n")
    log("REQ", lines[0], "|", [l for l in lines[1:] if l][:8])
    if args.close_before_101:
        log("closing before the 101")
        return
    key = [l.split(":", 1)[1].strip() for l in lines if l.lower().startswith("sec-websocket-key")][0]
    acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    if args.upgrade_status != 101:
        c.sendall(f"HTTP/1.1 {args.upgrade_status} Mock\r\nContent-Length: 0\r\n\r\n".encode())
        time.sleep(0.5)
        return
    resp = ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {acc}\r\n\r\n").encode()
    first = text({"type": "auth_required", "ha_version": "2099.1.0-mock"})
    if args.coalesce_handshake:
        send(resp + first)
    else:
        send(resp)
        time.sleep(0.05)
        send(first)

    def after_auth():
        time.sleep(args.after_auth_delay)
        try:
            if args.ws_ping_after_auth:
                log("SEND ws PING payload=hb-123")
                send(frame(9, b"hb-123"))
            for n in args.big_after_auth:
                p = big_payload(n)
                ts = time.time()
                if args.big_fragment_bytes:
                    step = args.big_fragment_bytes
                    for i in range(0, len(p), step):
                        send(frame(1 if i == 0 else 0, p[i:i + step], i + step >= len(p)))
                else:
                    send(frame(1, p))
                log(f"SENT big {n} bytes in {time.time()-ts:.2f}s")
                time.sleep(2)
            for n in args.deep_result:
                inner = "1"
                for _ in range(n):
                    inner = "[" + inner + "]"
                send(frame(1, ('{"id":998,"type":"result","success":true,"result":' + inner + "}").encode()))
                log(f"SENT deep result nesting={n}")
                time.sleep(2)
            if args.kill_mid_transfer:
                p = big_payload(args.kill_mid_transfer)
                send(hdr(1, len(p)) + p[: len(p) // 2])
                log("KILL mid-transfer")
                c.shutdown(socket.SHUT_RDWR)
                c.close()
                return
            if args.stall_mid_transfer:
                p = big_payload(args.stall_mid_transfer)
                send(hdr(1, len(p)) + p[: len(p) // 2])
                log("STALL mid-transfer (socket kept open)")
                return
            if args.rst_after_auth:
                log("RST (SO_LINGER 0)")
                c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                c.close()
                return
            if args.close_after_auth is not None:
                log("SEND close", args.close_after_auth)
                send(frame(8, struct.pack(">H", args.close_after_auth) + b"going away"))
            elif args.close_after_auth_nocode:
                log("SEND close (empty body)")
                send(frame(8, b""))
        except OSError as e:
            log("after-auth action aborted:", e)

    while True:
        fin, op, masked, data = recv_frame(c)
        if not masked:
            log("ERROR client frame not masked")
        if op == 8:
            code = struct.unpack(">H", data[:2])[0] if len(data) >= 2 else None
            log(f"RX CLOSE code={code} len={len(data)}")
            return
        if op == 10:
            log(f"RX PONG payload={data!r}")
            continue
        if op == 9:
            log("RX PING")
            continue
        if op != 1:
            log("RX op", op)
            continue
        msg = json.loads(data)
        shown = dict(msg)
        if "access_token" in shown:
            shown["access_token"] = f"<len {len(msg['access_token'])}>"
        log("RX", shown)
        t = msg.get("type")
        if t == "auth":
            if msg.get("access_token") == args.token:
                if args.auth_ok_delay:
                    time.sleep(args.auth_ok_delay)
                ok = json.dumps({"type": "auth_ok", "ha_version": "2099.1.0-mock"}).encode()
                if args.fragment_auth_ok:
                    a, b, cc = ok[:5], ok[5:12], ok[12:]
                    send(frame(1, a, False) + frame(9, b"mid") + frame(0, b, False))
                    time.sleep(0.2)
                    send(frame(0, cc, True))
                else:
                    send(frame(1, ok))
                threading.Thread(target=after_auth, daemon=True).start()
            else:
                send(text({"type": "auth_invalid",
                           "message": args.auth_invalid_message or "Invalid access token or password"}))
                time.sleep(0.2)
                return
        elif t == "subscribe_entities":
            send(text({"id": msg["id"], "type": "result", "success": True, "result": None}))
            ids = msg.get("entity_ids") or []
            subs[msg["id"]] = ids
            log(f"SUBSCRIBE id={msg['id']} ids={len(ids)}")
            if states:
                added = {e["entity_id"]: compressed(e) for e in states if e["entity_id"] in ids}
                send(text({"id": msg["id"], "type": "event", "event": {"a": added}}))
                if args.rm_add_every and len(added) > 1:
                    last = [e for e in states if e["entity_id"] in ids][-1]
                    threading.Thread(target=rm_add_loop, args=(msg["id"], last), daemon=True).start()
                if args.set_state_after and args.set_state_after[1] in added:
                    def set_state(sid=msg["id"]):
                        time.sleep(float(args.set_state_after[0]))
                        eid, st = args.set_state_after[1], args.set_state_after[2]
                        try:
                            send(text({"id": sid, "type": "event", "event": {"c": {eid: {"+": {"s": st}}}}}))
                            log(f"SENT c-diff sid={sid} {eid} state={st}")
                        except OSError:
                            pass
                    threading.Thread(target=set_state, daemon=True).start()
                if args.change_every and added:
                    threading.Thread(target=change_loop, args=(msg["id"], next(iter(added))),
                                     daemon=True).start()
        elif t == "unsubscribe_events":
            log(f"RX unsubscribe_events subscription={msg.get('subscription')}")
            subs.pop(msg.get("subscription"), None)
            if args.unsubscribe_error is not None:
                send(text({"id": msg["id"], "type": "result", "success": False,
                           "error": {"code": "unknown_error", "message": args.unsubscribe_error}}))
            else:
                send(text({"id": msg["id"], "type": "result", "success": True, "result": None}))
        elif t == "get_states":
            threading.Thread(target=send_states, args=(msg["id"],), daemon=True).start()
        elif t == "call_service":
            domain, service = msg.get("domain"), msg.get("service")
            eid = (msg.get("target") or {}).get("entity_id")
            data = msg.get("service_data")
            with CALL_LOCK:
                CALL_COUNT[0] += 1
                n = CALL_COUNT[0]
                log(f"CALL id={msg.get('id')} {domain}.{service} entity={eid} "
                    f"data={json.dumps(data, separators=(',', ':'))}")
                if args.call_log:
                    with open(args.call_log, "a", encoding="utf-8") as f:
                        f.write(json.dumps({"time": time.time(), "n": n, "id": msg.get("id"),
                                            "domain": domain, "service": service, "entity": eid,
                                            "data": data}, separators=(",", ":")) + "\n")
            if args.call_close_after and n == args.call_close_after:
                log(f"CLOSE after call {n}")
                c.shutdown(socket.SHUT_RDWR)
                return
            threading.Thread(target=answer_call, args=(msg, n, domain, service, eid, data),
                             daemon=True).start()
        elif t == "ping":
            if args.no_pong:
                log("not answering ping")
            else:
                send(text({"id": msg["id"], "type": "pong"}))
                pings_answered[0] += 1


def tls_conn(c, addr, args, ctx):
    """Handshake (or stall) on a freshly accepted socket, then run the normal mock."""
    t0 = time.time()
    if args.stall:
        print(f"STALL {addr} (TCP accepted, no TLS answer)", flush=True)
        time.sleep(20)
        c.close()
        return
    try:
        c.settimeout(10)
        sc = ctx.wrap_socket(c, server_side=True)
        sc.settimeout(None)
    except Exception as e:
        print(f"TLS handshake failed {addr} after {time.time()-t0:.2f}s: {e!r}", flush=True)
        c.close()
        return
    print(f"TLS handshake ok {addr} {sc.version()} {sc.cipher()[0]} {time.time()-t0:.2f}s", flush=True)
    handle_safe(sc, addr, args)


ACTIVE = set()
ACTIVE_LOCK = threading.Lock()


def stdin_loop():
    import sys
    for line in sys.stdin:
        cmd = line.strip()
        with ACTIVE_LOCK:
            conns = list(ACTIVE)
        print(f"STDIN {cmd} conns={len(conns)}", flush=True)
        for c in conns:
            try:
                if cmd == "rst":
                    c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                    c.close()
                elif cmd == "wsclose":
                    c.sendall(frame(8, struct.pack(">H", 1001) + b"going away"))
            except Exception as e:
                print("stdin cmd failed:", e, flush=True)


def handle_safe(c, addr, args):
    with ACTIVE_LOCK:
        ACTIVE.add(c)
    try:
        handle(c, addr, args)
    except Exception as e:
        print("conn end:", e, flush=True)
    finally:
        with ACTIVE_LOCK:
            ACTIVE.discard(c)
        try:
            c.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description="Mock HA WebSocket server (see module docstring)")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--token", default="mock-good-token")
    ap.add_argument("--no-pong", action="store_true")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--upgrade-status", type=int, default=101)
    ap.add_argument("--close-before-101", action="store_true")
    ap.add_argument("--coalesce-handshake", action="store_true")
    ap.add_argument("--fragment-auth-ok", action="store_true")
    ap.add_argument("--after-auth-delay", type=float, default=1.0)
    ap.add_argument("--ws-ping-after-auth", action="store_true")
    ap.add_argument("--big-after-auth", type=int, nargs="+", default=[], metavar="BYTES")
    ap.add_argument("--big-fragment-bytes", type=int, default=0)
    ap.add_argument("--kill-mid-transfer", type=int, default=0, metavar="BYTES")
    ap.add_argument("--stall-mid-transfer", type=int, default=0, metavar="BYTES")
    ap.add_argument("--rst-after-auth", action="store_true")
    ap.add_argument("--close-after-auth", type=int, default=None, metavar="CODE")
    ap.add_argument("--close-after-auth-nocode", action="store_true")
    ap.add_argument("--tls-cert", default=None)
    ap.add_argument("--tls-key", default=None)
    ap.add_argument("--tls13-only", action="store_true")
    ap.add_argument("--stall", action="store_true")
    ap.add_argument("--states-file", default=None)
    ap.add_argument("--states-synthetic", type=int, default=0, metavar="BYTES")
    ap.add_argument("--states-count", type=int, default=300)
    ap.add_argument("--states-fragment", type=int, default=16384)
    ap.add_argument("--pong-after-states", action="store_true")
    ap.add_argument("--states-rate", type=int, default=0, metavar="BPS")
    ap.add_argument("--states-after-pings", type=int, default=0, metavar="N")
    ap.add_argument("--states-delay", type=float, default=0.0, metavar="S")
    ap.add_argument("--stall-states", action="store_true")
    ap.add_argument("--change-every", type=float, default=0.0, metavar="S")
    ap.add_argument("--rm-add-every", type=float, default=0.0, metavar="S")
    ap.add_argument("--stdin-commands", action="store_true")
    ap.add_argument("--deep-result", type=int, nargs="+", default=[], metavar="N")
    ap.add_argument("--auth-ok-delay", type=float, default=0.0, metavar="S")
    ap.add_argument("--set-state-after", nargs=3, default=None, metavar=("S", "ENTITY", "STATE"))
    ap.add_argument("--auth-invalid-message", default=None, metavar="TEXT")
    ap.add_argument("--unsubscribe-error", default=None, metavar="TEXT")
    ap.add_argument("--call-log", default=None, metavar="FILE")
    ap.add_argument("--call-delay", type=float, default=0.0, metavar="S")
    ap.add_argument("--call-error", default=None, metavar="CODE:MSG")
    ap.add_argument("--call-error-every", type=int, default=0, metavar="N")
    ap.add_argument("--call-silent", action="store_true")
    ap.add_argument("--call-close-after", type=int, default=0, metavar="N")
    ap.add_argument("--call-apply", action="store_true")
    ap.add_argument("--echo-delay", type=float, default=0.2, metavar="S")
    ap.add_argument("--echo-stale", action="store_true")
    ap.add_argument("--echo-stale-gap", type=float, default=0.5, metavar="S")
    args = ap.parse_args()
    args.states_list = load_states(args)
    ctx = None
    if args.tls_cert or args.tls13_only or args.stall:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        if args.tls13_only:
            ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        if args.tls_cert:
            ctx.load_cert_chain(args.tls_cert, args.tls_key)
        elif not args.stall:
            ap.error("--tls13-only needs --tls-cert/--tls-key")
    if args.stdin_commands:
        threading.Thread(target=stdin_loop, daemon=True).start()
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", args.port))
    s.listen(4)
    s.settimeout(1)
    end = time.time() + args.seconds
    print("listening", args.port, flush=True)
    while time.time() < end:
        try:
            c, addr = s.accept()
        except socket.timeout:
            continue
        print("CONNECT from", addr, flush=True)
        if ctx is not None or args.stall:
            threading.Thread(target=tls_conn, args=(c, addr, args, ctx), daemon=True).start()
        else:
            threading.Thread(target=handle_safe, args=(c, addr, args), daemon=True).start()


if __name__ == "__main__":
    main()
