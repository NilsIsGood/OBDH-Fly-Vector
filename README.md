# R7023E OBDH Model — Space Segment

Working prototype of the platform OBC (PF) and payload ICU (PL), plus a
throwaway ground console for testing. Implements ICD v0.1.

Written in C11 with POSIX sockets. No external dependencies.

---

## Build and run

```sh
make            # builds pf, pl and gnd_stub
./demo.sh       # scripted end-to-end demonstration, about 60 seconds
```

To drive it by hand, in three terminals:

```sh
./pf            # terminal 1 — starts first, binds 5001 and 5002
./pl            # terminal 2 — connects in
./gnd_stub      # terminal 3 — type commands here
```

Then at the ground console:

```
obt                 set the onboard clock from this computer
mode TARGET         8 second transition
power ON            payload on
image               capture now
image @+30          capture 30 seconds from now
image @14:32:10     capture at an absolute time
typo                send a deliberately bad command
quit
```

`make clean` removes the binaries and the generated logfiles.

---

## What each program is

| File | Role | Notes |
| --- | --- | --- |
| `obdh.h` / `obdh.c` | Link layer | Line buffering, field splitting, clock and socket helpers. Shared by all three. |
| `pf.c` | Platform OBC | Modes, OBT clock, validation, time-tag queue, housekeeping. The gatekeeper. |
| `pl.c` | Payload ICU | Power state, 5 second capture, image log. Deliberately dumb. |
| `gnd_stub.c` | Test console | **Not the ground deliverable.** A harness, and a reference for the wire format. |

Each program is a 100 ms tick loop built on `poll()`. Nothing blocks, so
a program can service its links, run its timers and emit cyclic
housekeeping in the same loop without threads.

---

## Requirements status

Verified by running `./demo.sh` and reading the output.

| Requirement | Status | Evidence in the demo |
| --- | --- | --- |
| Three separate interacting programs | Met | Three processes, two TCP links |
| Platform has SAFE and one other mode | Met | `MODE_START` / `MODE_DONE` events |
| Mode transitions take time | Met | 8 s, `MODE_TRANSITION_MS` |
| Mode acquisition confirmed by event TM | Met | `EV MODE_DONE` |
| Payload rejected in SAFE | Met | `NAK reason=WRONG_MODE` |
| Return to SAFE forces payload off | Met | `EV PL_AUTO_OFF;cause=SAFE_ENTRY` |
| Invalid commands rejected with a reply | Met | `NAK reason=UNKNOWN_CMD` |
| Accepted commands acknowledged | Met | `ACK stage=ACCEPTED` then `EXECUTED` |
| Time-tagged telecommands | Met | `image @+10`, then `EV TC_DUE` |
| Overdue time tag rejected | Met | `NAK reason=TIMETAG_OVERDUE` |
| PF and Gnd on the same time base | Met | `SET_OBT` from ground wall clock |
| SCET upcounts until OBT is set | Met | `obt_valid=0`, time shown as `00:00:0x` |
| OBT settable by telecommand | Met | `EV OBT_SET;from=...;to=...` |
| Clock reported in cyclic housekeeping | Met | `HK` every 2 s |
| Payload ON/OFF simulated | Met | `pl_state` in payload housekeeping |
| Payload confirms start and finish of a take | Met | `IMAGE_START`, `IMAGE_DONE` 5 s later |
| PF forwards payload info to ground | Met | Payload `HK` relayed, renumbered |
| Payload writes a take log with the time | Met | `pl_images.log` |
| Command type identification | Met | `dest` field, `PF` or `PL` |
| Sequence count | Met | Four independent counters, `SEQ_GAP` event |
| Cyclic HK versus event TM | Met | `HK` on a timer, `EV` on change |
| Ground reception timestamp and logfile | Met | `gnd_tm.log`, `(rx hh:mm:ss)` on screen |
| Running everything in UTC | Not done | Local time by choice; one-line change in `wall_sec_of_day()` |
| Real image capture | Not done | Level 2 and 3 work, see below |

---

## Two deliberate design decisions worth defending in the report

**The payload is dumb on purpose.** PL receives four instructions, none
of which carry a time tag, a ground sequence number or any mode
context. All validation and all scheduling live in PF. This is why the
payload can be moved onto other hardware cheaply, and it matches how a
real ICU relates to a platform OBC.

**An unknown command still advances the sequence counter.** Sequence
checking runs before catalogue checking, as the ICD validation order
specifies, so a rejected command still consumes its number. This
mirrors PUS, where acceptance reporting precedes content validation.
The consequence to know about: injecting a raw command with an
arbitrary sequence number will desynchronise the ground counter for the
rest of the session. The `typo` console verb exists for this reason.

---

## Known deviations from the ICD

- `EV PF_BOOT` is emitted when the ground connects, not at process
  start. An event generated before a link exists cannot be delivered,
  and there is no onboard event store in this model.
- Midnight rollover is not handled. A time tag that crosses midnight
  will be treated as overdue. Out of scope per the ICD.
- There is no checksum. Out of scope per the ICD.

---

## Extending to Level 2 and Level 3

The hardware boundary is two functions in `pl.c`:

```c
static void pl_capture_start(void);   /* begin a take */
static int  pl_capture_poll(void);    /* 1 when the take has finished */
```

Level 1 implements them by writing a log entry and waiting 5 seconds.

- **Level 2 (Raspberry Pi or Arduino):** `pl.c` compiles unchanged on a
  Pi. Run `./pl <platform-ip>` and nothing else moves. For an Arduino,
  reimplement those two functions over a serial link.
- **Level 3 (Mavic):** reimplement the same two functions to trigger
  the shutter. Note that Litchi exposes no live API, so the realistic
  route is physical actuation of the controller's shutter button via
  the Arduino, which reuses the Level 2 hardware.

Nothing in `pf.c`, `obdh.c` or the ICD changes for any of this. That is
the point of the abstraction.

One thing worth putting in the report: the photograph lands on the
drone's SD card and never passes through PF. That is exactly what the
assignment says about real payload data chains bypassing the platform
and going to the MMU. It reads as deliberate design rather than a
shortcut.

---

## For the documentation team

Function names in the code map one-to-one onto the Space Segment
function tree, by design. The prefixes are the tree's second level:

| Prefix | Function tree branch |
| --- | --- |
| `lb_*`, `tcp_*`, `send_line`, `split_fields` | Link and framing |
| `pf_clock_*` | Onboard time |
| `pf_mode_*` | Mode management |
| `pf_tc_validate`, `pf_preconditions` | Telecommand validation |
| `pf_tc_execute` | Telecommand execution |
| `pf_queue_*` | Time-tag scheduling |
| `pf_tm_*` | Telemetry generation |
| `pf_link_*` | Link servicing |
| `pl_clock_*` | Payload time sync |
| `pl_capture_*` | Payload data generation |
| `pl_tm_*` | Payload telemetry |

The traceability matrix can be generated from this. One function
definition, one code item, one test, as the assignment asks.
