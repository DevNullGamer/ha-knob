"""Mock of the HA WebSocket API for on-device checks of Step 5 (not a unit test).

Run with a Python the Windows firewall allows on the LAN, e.g.
  C:\\Users\\Jesper\\AppData\\Local\\Python\\pythoncore-3.15-64\\python.exe tools/mockha.py --seconds 300
Options: --token T (the accepted token), --no-pong (never answer pings), --port P,
--seconds N (run time). Answers subscribe_entities with a success result.
The access token is never printed (only its length).
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
    op = h[0] & 0x0F
    ln = h[1] & 0x7F
    masked = h[1] & 0x80
    if ln == 126:
        ln = struct.unpack(">H", recv_exact(c, 2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", recv_exact(c, 8))[0]
    mask = recv_exact(c, 4) if masked else b"\0\0\0\0"
    data = bytearray(recv_exact(c, ln))
    for i in range(ln):
        data[i] ^= mask[i % 4]
    return op, bytes(data)

def send_text(c, obj):
    p = json.dumps(obj).encode()
    h = bytes([0x81])
    if len(p) < 126:
        h += bytes([len(p)])
    elif len(p) < 65536:
        h += bytes([126]) + struct.pack(">H", len(p))
    else:
        h += bytes([127]) + struct.pack(">Q", len(p))
    c.sendall(h + p)

def handle(c, addr, args):
    t0 = time.time()
    log = lambda *a: print(f"[{time.time()-t0:6.1f}s {addr[0]}]", *a, flush=True)
    req = b""
    while b"\r\n\r\n" not in req:
        req += c.recv(1024)
    lines = req.decode(errors="replace").split("\r\n")
    log("REQ", lines[0], "|", [l for l in lines[1:] if l][:8])
    key = [l.split(":", 1)[1].strip() for l in lines if l.lower().startswith("sec-websocket-key")][0]
    acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Accept: {acc}\r\n\r\n").encode())
    send_text(c, {"type": "auth_required", "ha_version": "2099.1.0-mock"})
    while True:
        op, data = recv_frame(c)
        if op == 8:
            log("CLOSE frame")
            return
        if op != 1:
            log("op", op)
            continue
        msg = json.loads(data)
        shown = dict(msg)
        if "access_token" in shown:
            shown["access_token"] = f"<len {len(msg['access_token'])}>"
        log("RX", shown)
        t = msg.get("type")
        if t == "auth":
            if msg.get("access_token") == args.token:
                send_text(c, {"type": "auth_ok", "ha_version": "2099.1.0-mock"})
            else:
                send_text(c, {"type": "auth_invalid", "message": "Invalid access token or password"})
                time.sleep(0.2)
                return
        elif t == "subscribe_entities":
            send_text(c, {"id": msg["id"], "type": "result", "success": True, "result": None})
        elif t == "ping":
            if args.no_pong:
                log("not answering ping")
            else:
                send_text(c, {"id": msg["id"], "type": "pong"})

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--token", default="mock-good-token")
    ap.add_argument("--no-pong", action="store_true")
    ap.add_argument("--seconds", type=int, default=60)
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
        threading.Thread(target=lambda: (lambda: (handle_safe(c, addr, args)))(), daemon=True).start()

def handle_safe(c, addr, args):
    try:
        handle(c, addr, args)
    except Exception as e:
        print("conn end:", e, flush=True)
    finally:
        c.close()

if __name__ == "__main__":
    main()
