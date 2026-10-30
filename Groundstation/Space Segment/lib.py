"""
Fly Vector - Ground Station library.

Same function names and same structure as the original stub file.
The gaps are filled in and the transport now speaks ICD v0.1:

    TC|<seq>|<dest>|<timetag>|<command>|<args>
    <type>|<seq>|<source>|<obt>|<params>

One message per line, terminated by '\\n', fields separated by '|',
maximum 512 bytes. No PUS, no CCSDS, no checksums (out of scope per
the assignment description and the ICD).
"""

import json  # kept for compatibility; the MIB is now plain text
import queue
import socket
import struct  # kept for compatibility; no longer used
import sys
import threading
import time

"""
GLOBALS
"""
EXIT_WORDS = {"exit", "quit", "q"}
_mib = {}                    # command name -> {target, argument, description}
_tm_defs = {}                # telemetry name -> {type, format, description}
_reasons = {}                # reason code -> description
_seq_count = 0
_pending = {}                # seq -> (command name, time sent)
_uplink_q = queue.Queue()    # ground -> platform
_downlink_q = queue.Queue()  # platform -> ground
_stdin_q = queue.Queue()     # operator input
RESPONSE_TIMEOUT_S = 5.0     # applies to acceptance only

# Console noise control.
#   SHOW_HK = False  routine housekeeping goes to the log only, so the
#                    line you are typing is never overwritten.
#   SHOW_HK = True   everything on screen, as before.
SHOW_HK = False
DISPLAY_LOG = "tm_display.log"   # full human-readable stream, for tail -f

PF_HOST = "127.0.0.1"
PF_PORT = 5001               # ICD: PF listens here for the ground
MAX_LINE = 512               # ICD: maximum bytes on the wire

_sock = None
_last_tm_seq = 0             # for downlink sequence gap detection

DEFAULT_MIB = {
    "PING":       {"target": "PF", "argument": "-",           "description": "Link check"},
    "SET_OBT":    {"target": "PF", "argument": "hh:mm:ss",    "description": "Set onboard clock"},
    "SET_MODE":   {"target": "PF", "argument": "SAFE,TARGET", "description": "Mode change"},
    "GET_HK":     {"target": "PF", "argument": "-",           "description": "Request housekeeping"},
    "PL_POWER":   {"target": "PL", "argument": "ON,OFF",      "description": "Switch payload"},
    "TAKE_IMAGE": {"target": "PL", "argument": "-",           "description": "Take an image"},
}

"""
INITIALISATION
"""


def startGroundTime():
    """
    Starts the ground time for the ground station.
    Keeps both a monotonic reference (for elapsed time and timeouts) and
    the wall clock (used to set the onboard time).
    """
    global _ground_time, _wall_time
    _ground_time = time.monotonic()
    _wall_time = time.time()


def _now() -> float:
    """Monotonic ground elapsed time (s)."""
    return time.monotonic() - _ground_time


def _wall_hms() -> str:
    """Local wall clock as hh:mm:ss, the ICD time format."""
    return time.strftime("%H:%M:%S", time.localtime())


def loadMIB(path="mib.txt"):
    """
    Load the Mission Information Base.

    Plain text, pipe separated, three sections marked by their comment
    headers. Blank lines and comments are ignored.
    """
    global _mib, _tm_defs, _reasons
    _mib, _tm_defs, _reasons = {}, {}, {}
    section = None

    try:
        with open(path) as f:
            for raw in f:
                line = raw.strip()
                if not line:
                    continue
                if line.startswith("#"):
                    upper = line.upper()
                    if "COMMAND DEFINITIONS" in upper:
                        section = "tc"
                    elif "TELEMETRY DEFINITIONS" in upper or "TELEMENTRY" in upper:
                        section = "tm"
                    elif "REASON CODES" in upper:
                        section = "rc"
                    continue

                parts = [p.strip() for p in line.split("|")]
                if section == "tc" and len(parts) >= 4:
                    _mib[parts[0].upper()] = {
                        "target": parts[1].upper(),
                        "argument": parts[2],
                        "description": parts[3],
                    }
                elif section == "tm" and len(parts) >= 4:
                    _tm_defs[parts[0].upper()] = {
                        "type": parts[1],
                        "format": parts[2],
                        "description": parts[3],
                    }
                elif section == "rc" and len(parts) >= 2:
                    _reasons[parts[0].upper()] = parts[1]

        print(f"[MIB] loaded {len(_mib)} commands, "
              f"{len(_tm_defs)} telemetry types, {len(_reasons)} reason codes")
    except FileNotFoundError:
        _mib = dict(DEFAULT_MIB)
        print(f"[MIB] {path} not found, using built-in defaults ({len(_mib)} commands)")


def _reader_thread():
    """
    Receive bytes from the platform and split them into lines.

    TCP is a byte stream, not a message stream: two messages can arrive
    in one recv() and one message can arrive split across two. Anything
    incomplete stays in the buffer until the rest turns up.
    """
    buf = b""
    while True:
        try:
            data = _sock.recv(4096)
        except OSError:
            break
        if not data:
            print("[GND] platform link lost")
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("ascii", errors="replace").rstrip("\r")
            if text:
                _downlink_q.put(text)
        if len(buf) > MAX_LINE:
            print("[ERR] oversized line discarded")
            buf = b""


def _writer_thread():
    """Drain the uplink queue onto the socket, one line at a time."""
    while True:
        packet = _uplink_q.get()
        try:
            _sock.sendall((packet + "\n").encode("ascii"))
        except OSError:
            print("[ERR] uplink failed, link down")
            break


def _stdin_thread():
    """Feed operator input into the queue so readComms stays non-blocking."""
    for line in sys.stdin:
        _stdin_q.put(line)


def startComms(host=PF_HOST, port=PF_PORT):
    """
    Starts the communications for the ground station.
    Opens the TCP link to the platform and starts the reader, writer and
    console threads. Retries for a few seconds so the ground station can
    be started before or after the platform.
    """
    global _sock
    for attempt in range(10):
        try:
            _sock = socket.create_connection((host, port), timeout=2)
            _sock.settimeout(None)
            break
        except OSError:
            time.sleep(0.5)
    else:
        print(f"[ERR] cannot reach platform at {host}:{port}")
        sys.exit(1)

    threading.Thread(target=_reader_thread, daemon=True).start()
    threading.Thread(target=_writer_thread, daemon=True).start()
    threading.Thread(target=_stdin_thread, daemon=True).start()
    try:
        open(DISPLAY_LOG, "w").close()      # fresh log each run
    except OSError:
        pass

    print(f"[GND] connected to platform at {host}:{port}")
    if not SHOW_HK:
        print(f"[GND] housekeeping is hidden here; full stream in {DISPLAY_LOG}")
        print(f"[GND] watch it with:  tail -f {DISPLAY_LOG}")
    print("[GND] try: SET_OBT / SET_MODE TARGET / PL_POWER ON / TAKE_IMAGE @+30 / quit")
    print("[GND] ---------------------------------------------------------------")


"""
CONSOLE
"""


def readComms():
    """
    Retrieves incoming data from standard in and processes it accordingly.
    Returns a line typed by the operator, or None. Non-blocking.
    """
    try:
        return _stdin_q.get_nowait().strip() or None
    except queue.Empty:
        return None


def checkComms(line):
    """
    Validate operator input against the MIB. Returns the cleaned command
    line, or None. Typing exit / quit / q shuts the ground station down.
    """
    if line.lower() in EXIT_WORDS:
        print("[GND] shutdown")
        sys.exit(0)

    body = line.split("@", 1)[0].strip()
    parts = body.split()
    if not parts:
        return None

    name = parts[0].upper()
    if name not in _mib:
        print(f"[ERR] unknown command '{parts[0]}'")
        return None

    allowed = _mib[name]["argument"]
    args = parts[1:]

    if allowed == "-" and args:
        print(f"[ERR] {name} takes no argument")
        return None
    if allowed != "-" and not args:
        if name == "SET_OBT":
            pass            # filled in from the ground clock by createTC
        else:
            print(f"[ERR] {name} needs an argument ({allowed})")
            return None
    if "," in allowed and args and args[0].upper() not in allowed.split(","):
        print(f"[ERR] {name} argument must be one of {allowed}")
        return None

    return line.strip()


def createTC(name):
    """
    Build a telecommand from its MIB definition.

    Accepts the cleaned console line, e.g. "SET_MODE TARGET" or
    "TAKE_IMAGE @14:32:10" or "TAKE_IMAGE @+20" (relative, expanded here
    on the ground so that what goes on the wire is always absolute).
    """
    body, _, tag = name.partition("@")
    parts = body.split()
    cmd = parts[0].upper()
    args = " ".join(parts[1:]).upper()

    tag = tag.strip()
    if not tag:
        timetag = "NOW"
    elif tag.startswith("+"):
        when = time.localtime(time.time() + int(tag[1:]))
        timetag = time.strftime("%H:%M:%S", when)
    else:
        timetag = tag

    if cmd == "SET_OBT" and not args:
        args = _wall_hms()

    seq = handleSequenceCount()
    dest = _mib[cmd]["target"]
    packet = f"TC|{seq}|{dest}|{timetag}|{cmd}|{args}"

    if len(packet) + 1 > MAX_LINE:
        print("[ERR] telecommand exceeds the 512 byte line limit")
        return None

    return {"seq": seq, "name": cmd, "dest": dest,
            "timetag": timetag, "args": args, "packet": packet}


def timestampCommand(tc):
    """Applies a timestamp to the telecommand using the ground time."""
    if tc:
        tc["t_ground"] = _now()
    return tc


def displayResponse(response):
    """Displays the response to a command on the console."""
    print(f"[GND {_now():9.3f}] {response}")


"""
UPLINK
"""


def sendTelecommand(tc):
    """
    Sends a telecommand over the communications interface and records it
    as outstanding so that monitorForResponse can time it out.
    """
    if not tc:
        return
    _uplink_q.put(tc["packet"])
    _pending[tc["seq"]] = (tc["name"], _now(), False)
    displayResponse(f"TC sent: {tc['name']} (seq {tc['seq']})")


def handleSequenceCount():
    """
    Manages the telecommand sequence counter.
    ICD: starts at 1, increments per telecommand, wraps at 65535.
    """
    global _seq_count
    _seq_count = _seq_count % 65535 + 1
    return _seq_count


def monitorForResponse():
    """
    Monitors the downlink for responses to sent telecommands and flags a
    timeout if no acknowledgement arrives.
    """
    now = _now()
    for seq, (name, t, accepted) in list(_pending.items()):
        if accepted:
            continue        # acceptance arrived; execution may be minutes away
        if now - t > RESPONSE_TIMEOUT_S:
            displayResponse(f"TIMEOUT: no acceptance for '{name}' (seq {seq})")
            del _pending[seq]


"""
DOWNLINK
"""


def receiveTelemetry():
    """Return one raw telemetry line, or None. Non-blocking."""
    try:
        return _downlink_q.get_nowait()
    except queue.Empty:
        return None


def checkTelemetry(raw_tm):
    """
    Validates received telemetry and decodes it using the MIB.

    ICD format: <type>|<seq>|<source>|<obt>|<params>
    Also reports a sequence gap on the downlink, which is the ground
    half of the sequence counting requirement.
    """
    global _last_tm_seq

    fields = raw_tm.split("|")
    if len(fields) != 5:
        print(f"[ERR] bad TM, expected 5 fields: {raw_tm}")
        return None

    kind, seq, source, obt, params = fields
    if kind.upper() not in _tm_defs and kind.upper() not in ("ACK", "NAK", "HK", "EV"):
        print(f"[ERR] unknown telemetry type '{kind}'")
        return None

    try:
        seq = int(seq)
    except ValueError:
        print(f"[ERR] bad TM sequence '{seq}'")
        return None

    if _last_tm_seq and seq > _last_tm_seq + 1:
        print(f"[GAP] telemetry sequence gap: expected {_last_tm_seq + 1}, got {seq}")
    _last_tm_seq = seq

    decoded = {}
    for item in params.split(";"):
        if "=" in item:
            k, v = item.split("=", 1)
            decoded[k] = v

    return {"kind": kind.upper(), "seq": seq, "source": source.upper(),
            "obt": obt, "params": decoded, "raw": raw_tm}


def timestampTelemetry(tm):
    """Records the ground time at which the telemetry was received."""
    tm["t_ground"] = _now()
    tm["t_wall"] = _wall_hms()
    return tm


def displayTelemetry(tm):
    """
    Displays telemetry on the console and clears any matching pending
    telecommand once the platform reports it executed.
    """
    line = f"[TM {tm['t_wall']}] {tm['raw']}"

    # Every message goes to the display log, which a second pane can
    # follow with: tail -f tm_display.log
    try:
        with open(DISPLAY_LOG, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass

    # Only non-routine telemetry reaches the console. Housekeeping
    # arrives about once a second and would otherwise overwrite the
    # command the operator is part-way through typing.
    if SHOW_HK or tm["kind"] != "HK":
        print(line)

    if tm["kind"] in ("ACK", "NAK"):
        ref = tm["params"].get("ref")
        try:
            ref = int(ref)
        except (TypeError, ValueError):
            return

        if tm["kind"] == "NAK":
            reason = tm["params"].get("reason", "?")
            name = _pending.pop(ref, ("?", 0, False))[0]
            displayResponse(f"REJECTED '{name}' (seq {ref}): "
                            f"{reason} - {_reasons.get(reason, '')}")
        elif tm["params"].get("stage") == "EXECUTED":
            name = _pending.pop(ref, ("?", 0, False))[0]
            displayResponse(f"EXECUTED '{name}' (seq {ref})")
        else:
            # ICD: acceptance only. Stop the timeout clock but keep the
            # entry so the later execution report can still be matched.
            if ref in _pending:
                name, t, _ = _pending[ref]
                _pending[ref] = (name, t, True)
            else:
                name = "?"
            displayResponse(f"ACCEPTED '{name}' (seq {ref})")


def logTelemetry(tm, path="tm_log.csv"):
    """
    Writes timestamped telemetry to the log, as the assignment requires.
    """
    with open(path, "a") as f:
        f.write(f"{tm['t_wall']},{tm['t_ground']:.3f},{tm['kind']},"
                f"{tm['seq']},{tm['source']},{tm['obt']},{tm['raw']}\n")
