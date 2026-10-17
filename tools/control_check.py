"""Step 7.6 control tester: injects TIN lines into the knob over USB serial (test firmware built
with -DHAKNOB_TEST_INPUT) and checks the call log written by tools/mockha.py --call-log.

Never resets the board (DTR/RTS kept low, like serial_log.py). Close any serial monitor or
serial_log first. Needs pyserial (the PlatformIO Python has it):
  %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/control_check.py ...

Start the mock first, e.g.
  python tools/mockha.py --seconds 600 --states-file tools/fixtures/control_states.json ^
      --call-log calls.jsonl --call-apply

Generic mode (one scenario):
  control_check.py --port COM14 --call-log calls.jsonl --clear-log \\
      --tin "rot +3" --expect-call "light.turn_on entity=light.knob_dimmer data={\\"brightness_pct\\":65}"
  --tin CMD            send "TIN CMD" (repeatable; --tin-gap S between lines, default 0.3)
  --repeat N --interval S   send each --tin line N times, S seconds apart (rate-limit tests;
                       e.g. --tin "rot +1" --repeat 250 --interval 0.02)
  --expect-call SPEC   "domain.service [entity=E] [data=JSON]" (repeatable, in order); data is a
                       subset match; numbers compare by value (22 == 22.0)
  --expect-count N     exactly N new calls in total (default: number of --expect-call)
  --max-calls N        at most N new calls (use instead of --expect-count for burst tests)
  --last-data JSON     the last new call's data must contain this subset
  --wait S             wait up to S seconds for the expected calls (default 5), then --settle S
                       (default 1.0) to catch extra calls
  --clear-log          truncate the call log first (else only calls after the start count)

Suite mode (every action and press of tools/fixtures/control_states.json):
  control_check.py --port COM14 --call-log calls.jsonl --suite \\
      --controls light.knob_dimmer:brightness:toggle,light.knob_color:hue:none,...
  --controls lists the configured controls in page order as entity:action:press (action/press
  "none" = skip that part). The tool assumes the device shows the first control; it sends
  "swipe left" between controls (--no-nav to skip, e.g. if you set pages by hand). For each
  control it sends "rot +1" and "short" and checks the expected call for the fixture's initial
  values and default steps (see EXPECT_ROT / EXPECT_PRESS; fan percentage and volume are
  checked loosely). Configure the controls (web UI or PUT /api/config) before the run.

Common: --serial-log FILE tees every received serial line ("<epoch> <line>", like serial_log.py;
only one program can hold COM14), --listen S just logs for S seconds; --dry-run prints the lines instead of opening the port (self-test); --require-echo
fails if the device did not log "TIN <cmd>" for every line sent. Exit code 0 = all passed.
"""
import argparse
import json
import sys
import threading
import time

try:
    import serial
except ImportError:  # --dry-run works without pyserial
    serial = None


# Expected call per (entity, encoder action) for "rot +1" and per (entity, press) for "short",
# with the fixture's initial values and the default steps. data None = only the keys of
# KEYS_ONLY are required; a dict = subset match.
EXPECT_ROT = {
    ("light.knob_dimmer", "brightness"): ("light.turn_on", {"brightness_pct": 55}),
    ("light.knob_color", "brightness"): ("light.turn_on", {"brightness_pct": 85}),
    ("light.knob_color", "hue"): ("light.turn_on", {"hs_color": [40, 50]}),
    ("light.knob_color", "color_temp"): ("light.turn_on", {"color_temp_kelvin": 3100}),
    ("climate.knob_heat", "target_temp"): ("climate.set_temperature", {"temperature": 22.0}),
    ("climate.knob_off", "target_temp"): ("climate.set_temperature", {"temperature": 19.5}),
    ("cover.knob_blind", "position"): ("cover.set_cover_position", {"position": 50}),
    ("fan.knob_fan", "percentage"): ("fan.set_percentage", {"percentage": None}),
    ("media_player.knob_player", "volume"): ("media_player.volume_set", {"volume_level": None}),
}
EXPECT_PRESS = {
    ("light.knob_dimmer", "toggle"): ("light.toggle", None),
    ("light.knob_color", "toggle"): ("light.toggle", None),
    ("climate.knob_heat", "toggle"): ("climate.turn_off", None),
    ("climate.knob_off", "toggle"): ("climate.turn_on", None),
    ("cover.knob_blind", "toggle"): ("cover.toggle", None),
    ("cover.knob_blind", "stop"): ("cover.stop_cover", None),
    ("fan.knob_fan", "toggle"): ("fan.toggle", None),
    ("media_player.knob_player", "play_pause"): ("media_player.media_play_pause", None),
    ("media_player.knob_player", "toggle"): ("media_player.toggle", None),
    ("media_player.knob_player", "mute"): ("media_player.volume_mute", {"is_volume_muted": True}),
    ("switch.knob_switch", "toggle"): ("switch.toggle", None),
    ("input_boolean.knob_boolean", "toggle"): ("input_boolean.toggle", None),
}


def subset(want, got):
    """want is contained in got; numbers by value, None in want = key present."""
    if isinstance(want, dict):
        return isinstance(got, dict) and all(k in got and subset(v, got[k]) for k, v in want.items())
    if isinstance(want, list):
        return isinstance(got, list) and len(want) == len(got) and all(subset(a, b) for a, b in zip(want, got))
    if want is None:
        return True
    if isinstance(want, (int, float)) and isinstance(got, (int, float)) and not isinstance(want, bool):
        return abs(want - got) < 1e-6
    return want == got


def parse_spec(spec):
    """'domain.service [entity=E] [data=JSON]' -> (service, entity|None, data|None)."""
    head, _, rest = spec.partition(" ")
    entity = data = None
    rest = rest.strip()
    while rest:
        if rest.startswith("entity="):
            val, _, rest = rest[7:].partition(" ")
            entity = val
        elif rest.startswith("data="):
            data = json.loads(rest[5:])
            rest = ""
        else:
            raise SystemExit(f"bad --expect-call part: {rest!r}")
        rest = rest.strip()
    return head, entity, data


class Knob:
    def __init__(self, args):
        self.dry = args.dry_run
        self.sent = 0
        self.echoes = []
        self.ser = None
        self.stop = False
        self.logf = open(args.serial_log, "a", encoding="utf-8", errors="replace") if args.serial_log else None
        if not self.dry:
            if serial is None:
                raise SystemExit("pyserial missing: use the PlatformIO Python")
            s = serial.Serial()
            s.port = args.port
            s.baudrate = 115200
            s.timeout = 0.1
            s.dtr = False
            s.rts = False
            s.open()
            self.ser = s
            threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        buf = b""
        while not self.stop:
            try:
                d = self.ser.read(4096)
            except (serial.SerialException, OSError):
                return
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode("utf-8", "replace").rstrip("\r")
                if self.logf:
                    self.logf.write(f"{time.time():.3f} {line}\n")
                    self.logf.flush()
                if "TIN " in line:
                    self.echoes.append(line[line.index("TIN "):])

    def tin(self, cmd):
        line = f"TIN {cmd}\n"
        if len(line) > 40:
            raise SystemExit(f"TIN line too long (max 40 chars): {line!r}")
        self.sent += 1
        if self.dry:
            print("TIN>", line.strip(), flush=True)
        else:
            self.ser.write(line.encode())
            self.ser.flush()

    def close(self):
        self.stop = True
        time.sleep(0.2)
        if self.logf:
            self.logf.close()
        if self.ser:
            self.ser.close()


class CallLog:
    def __init__(self, path):
        self.path = path

    def clear(self):
        open(self.path, "w").close()

    def read(self):
        out = []
        try:
            with open(self.path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        out.append(json.loads(line))
        except FileNotFoundError:
            pass
        return out


def wait_calls(log, base, want, wait, settle):
    """Waits until len(new) >= want (or the timeout), then settle seconds for extra calls."""
    end = time.time() + wait
    while time.time() < end and len(log.read()) - base < want:
        time.sleep(0.1)
    time.sleep(settle)
    return log.read()[base:]


def call_name(c):
    return f"{c['domain']}.{c['service']}"


def match(call, service, entity, data):
    return (call_name(call) == service and (entity is None or call["entity"] == entity)
            and (data is None or subset(data, call.get("data") or {})))


def run_generic(args, knob, log):
    exp = [parse_spec(s) for s in args.expect_call]
    base = len(log.read())
    for rep in range(args.repeat):
        for cmd in args.tin:
            knob.tin(cmd)
            if args.repeat > 1:
                time.sleep(args.interval)
            else:
                time.sleep(args.tin_gap)
    want = args.expect_count if args.expect_count is not None else len(exp)
    new = wait_calls(log, base, want if args.max_calls is None else 0, args.wait, args.settle)
    ok = True
    msgs = []
    for i, (service, entity, data) in enumerate(exp):
        if i >= len(new) or not match(new[i], service, entity, data):
            ok = False
            msgs.append(f"call {i + 1}: expected {service} entity={entity} data={data}, got "
                        f"{call_name(new[i]) + ' ' + json.dumps(new[i]['data']) if i < len(new) else 'nothing'}")
    if args.max_calls is not None:
        if len(new) > args.max_calls:
            ok = False
            msgs.append(f"{len(new)} calls > max {args.max_calls}")
    elif len(new) != want:
        ok = False
        msgs.append(f"{len(new)} new calls, expected {want}")
    if args.last_data:
        want_last = json.loads(args.last_data)
        if not new or not subset(want_last, new[-1].get("data") or {}):
            ok = False
            msgs.append(f"last call data {new[-1].get('data') if new else None} lacks {want_last}")
    span = (new[-1]["time"] - new[0]["time"]) if len(new) > 1 else 0.0
    print(f"calls={len(new)} span={span:.2f}s last={json.dumps(new[-1]['data']) if new else None}")
    for m in msgs:
        print("FAIL", m)
    print("PASS" if ok else "FAIL")
    return ok


def run_suite(args, knob, log):
    controls = []
    for part in args.controls.split(","):
        bits = part.strip().split(":")
        if len(bits) != 3:
            raise SystemExit(f"--controls entry must be entity:action:press, got {part!r}")
        controls.append(bits)
    all_ok = True
    results = []
    for idx, (entity, action, press) in enumerate(controls):
        if idx > 0 and not args.no_nav:
            knob.tin("swipe left")
            time.sleep(args.nav_wait)
        cases = []
        if action != "none":
            cases.append((f"{entity} rot {action}", "rot +1", EXPECT_ROT.get((entity, action))))
        if press != "none":
            cases.append((f"{entity} press {press}", "short", EXPECT_PRESS.get((entity, press))))
        for name, cmd, exp in cases:
            if exp is None:
                results.append((name, False, "no expectation in the table"))
                all_ok = False
                continue
            service, data = exp
            base = len(log.read())
            knob.tin(cmd)
            new = wait_calls(log, base, 1, args.wait, args.settle)
            if len(new) == 1 and match(new[0], service, entity, data):
                results.append((name, True, f"{call_name(new[0])} {json.dumps(new[0]['data'])}"))
            else:
                all_ok = False
                got = "; ".join(f"{call_name(c)} {c['entity']} {json.dumps(c['data'])}" for c in new)
                results.append((name, False, f"expected {service} {data}, got [{got}]"))
            time.sleep(args.tin_gap)
    for name, ok, detail in results:
        print(("PASS " if ok else "FAIL ") + name + ": " + detail)
    print("PASS" if all_ok else "FAIL", f"({sum(1 for r in results if r[1])}/{len(results)})")
    return all_ok


def main():
    ap = argparse.ArgumentParser(description="Drive TIN input on the knob and check the mock's call log")
    ap.add_argument("--port", default="COM14")
    ap.add_argument("--call-log", default=None, metavar="FILE")
    ap.add_argument("--clear-log", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--require-echo", action="store_true")
    ap.add_argument("--serial-log", default=None, metavar="FILE",
                    help="append every received serial line as '<epoch> <line>' (the port can only "
                         "be open once, so use this instead of serial_log.py while checking)")
    ap.add_argument("--listen", type=float, default=0.0, metavar="S",
                    help="only log the serial port for S seconds (no TIN, no call check)")
    ap.add_argument("--tin", action="append", default=[], metavar="CMD")
    ap.add_argument("--tin-gap", type=float, default=0.3)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--interval", type=float, default=0.02)
    ap.add_argument("--expect-call", action="append", default=[], metavar="SPEC")
    ap.add_argument("--expect-count", type=int, default=None)
    ap.add_argument("--max-calls", type=int, default=None)
    ap.add_argument("--last-data", default=None, metavar="JSON")
    ap.add_argument("--wait", type=float, default=5.0)
    ap.add_argument("--settle", type=float, default=1.0)
    ap.add_argument("--suite", action="store_true")
    ap.add_argument("--controls", default="")
    ap.add_argument("--no-nav", action="store_true")
    ap.add_argument("--nav-wait", type=float, default=0.8)
    args = ap.parse_args()
    if args.suite and not args.controls:
        ap.error("--suite needs --controls")
    if args.listen:
        knob = Knob(args)
        time.sleep(args.listen)
        knob.close()
        return
    if not args.call_log:
        ap.error("--call-log is required")
    if not args.suite and not args.tin:
        ap.error("give --tin ... or --suite")
    log = CallLog(args.call_log)
    if args.clear_log:
        log.clear()
    knob = Knob(args)
    try:
        ok = run_suite(args, knob, log) if args.suite else run_generic(args, knob, log)
        if not args.dry_run:
            time.sleep(0.3)
            print(f"tin_sent={knob.sent} tin_echoed={len(knob.echoes)}")
            if args.require_echo and len(knob.echoes) < knob.sent:
                print("FAIL device logged fewer TIN lines than sent")
                ok = False
    finally:
        knob.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
