"""Mock of the HA WebSocket API for on-device checks (Steps 5 and 5b.2; not a unit test).

Run with a Python the Windows firewall allows on the LAN, e.g.
  C:\\Users\\Jesper\\AppData\\Local\\Python\\pythoncore-3.15-64\\python.exe tools/mockha.py --seconds 300

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
It answers subscribe_entities with a success result. The access token is never printed (only
its length).
"""
import argparse, base64, hashlib, json, socket, struct, threading, time

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


def handle(c, addr, args):
    t0 = time.time()
    log = lambda *a: print(f"[{time.time()-t0:7.2f}s {addr[0]}]", *a, flush=True)
    send_lock = threading.Lock()

    def send(b):
        with send_lock:
            c.sendall(b)

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
                send(text({"type": "auth_invalid", "message": "Invalid access token or password"}))
                time.sleep(0.2)
                return
        elif t == "subscribe_entities":
            send(text({"id": msg["id"], "type": "result", "success": True, "result": None}))
        elif t == "ping":
            if args.no_pong:
                log("not answering ping")
            else:
                send(text({"id": msg["id"], "type": "pong"}))


def handle_safe(c, addr, args):
    try:
        handle(c, addr, args)
    except Exception as e:
        print("conn end:", e, flush=True)
    finally:
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
    args = ap.parse_args()
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
        threading.Thread(target=handle_safe, args=(c, addr, args), daemon=True).start()


if __name__ == "__main__":
    main()
