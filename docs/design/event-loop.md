# Event-driven main loop (vtltape / vtllibrary)

Tracking issue: [#272](https://github.com/markh794/mhvtl/issues/272). This is the
design agreed in that thread, to be built as a series of small PRs. It replaces
the sleep-polling main loop with a loop that waits in `poll()` on whatever can
wake it.

## Today

`vtltape` and `vtllibrary` run the same loop (`main()` in
`usr/cmd/vtltape.c` and `usr/cmd/vtllibrary.c`):

1. read one SysV queue message with `IPC_NOWAIT` (`msgrcv`);
2. call `vtl_transport->poll_cmd()` (`include/transport.h`), which never blocks
   for any transport;
3. when idle, `usleep(sleep_time)`, then add `Backoff` (default 400 µs) up to
   1 s. A SCSI command or a queue message resets `sleep_time` to
   `MIN_SLEEP_TIME` (5 µs).

The `sleep_time` value does four unrelated jobs:

- **Latency** — after each command a missed poll adds 5, 405, 805 µs… before
  the next one is seen. `st` sends one command at a time, so this gap lands on
  every block.
- **Idle CPU** — the backoff is what keeps an idle daemon from spinning.
- **Idle state** — `vtltape` reports `MHVTL_STATE_IDLE` / `MHVTL_STATE_LOADED_IDLE`
  once its `sleep_time` has grown past `0xf000`; `vtllibrary` reports
  `MHVTL_STATE_IDLE` once its interval (named `pollInterval` there) has grown
  past `0x18000`.
- **Shutdown** — an `exit` message sets `time_to_exit`, but the loop acts on it
  only after the interval has grown past the exit gate (`0xf000` in both
  daemons), which stands in for "no command in flight" without proving it.

The two daemons' loops differ in these constants (and in the interval's name)
by accident, not design — one reason to share a single loop, below.

The polling cost is measured on [#254](https://github.com/markh794/mhvtl/issues/254#issuecomment-6215072178)
and summarised in #272: at `Backoff 10` versus `Backoff 400`, TCMU 64 KiB
throughput roughly doubles for writes and triples for reads, and the kernel
module's 256 KiB writes rise ~2.2×. Those figures are the motivation; this
document does not restate them as its own measurements.

## Target

Each daemon waits in `poll()` on the set of descriptors that can wake it: the
transport, a timer (`timerfd`), signals (`signalfd`) and, eventually, the
control channel. `poll_cmd()` stays the way a command is claimed and decoded;
the loop only changes *when* it is called.

### Decisions (from the #272 thread)

These are settled and this design is built on them:

- **`exit` quiesces.** On shutdown the daemon stops accepting new commands and
  lets the commands it has already accepted finish, so the SCSI layer above
  sees a clean shutdown. (The old "wait for the interval to grow" logic was a
  workaround for a since-fixed kernel-module oops on shutdown; the aim now is
  only to play nice with the upper SCSI layer.) A `quiesce` transport op
  depends on this.
- **Move messages stay ordered.** The `vtltape ↔ vtllibrary` move handshake
  depends on messages arriving in order; any new control channel must preserve
  it (see *Control channel* below).
- **Idle thresholds become named durations.** Nothing in the loop needs today's
  exact timing; the `0xf000`-derived thresholds become named deadlines from the
  last activity (for example `IDLE_AFTER`, `LOADED_IDLE_AFTER`). `Backoff`
  stays only as long as a fallback timer exists.
- **A Unix socket control channel is acceptable** as the long-term replacement
  for SysV, provided external tools keep working during a transition, and it is
  welcome if it simplifies the IPC-resource cleanup (today the last daemon
  standing frees the SysV resources).
- **Kernel `.poll` is gated on whether TCMU is the end state.** If TCMU is where
  the module is heading, step 4 (a `.poll` in `mhvtl.ko`) can be deferred or
  skipped, with old modules running on the fallback timer until they go. This is
  the one open product decision below.

### Shared loop, selected by transport

Mark's steer: the loop currently differs between `vtltape` and `vtllibrary` by
accident, not design, and keeping the two in sync by hand has not worked. The
loop moves into its own translation unit shared by both daemons, so they cannot
drift. The transport-specific waiting is isolated so the loop can be compiled
for one backend or another:

- `usr/event_loop.c` — the transport-agnostic loop: build the `poll()` set
  (transport wake fd + `timerfd` + `signalfd` + later the control fd), dispatch
  to `poll_cmd()`, run the idle/exit/`SIGHUP` state machine on named deadlines.
- A per-transport source providing the wake descriptor, selected by make target
  (`event_loop_kmod.c` for the kernel module, `event_loop_tcmu.c` for TCMU),
  mirroring how the build already chooses transports. A transport that cannot
  supply a wake fd (an old `mhvtl.ko`) leaves the loop on the fallback timer.

Both `vtltape` and `vtllibrary` link the shared loop and pass in their own
command-handling callback, so the two are identical except for what they do with
a decoded command.

### Transports

A new optional op returns a descriptor that becomes readable when `poll_cmd()`
has work. `poll_cmd()` is unchanged.

- **TCMU** — return the handler socket. The handler side already waits in
  `poll()` on the TCMU fd (`usr/cmd/mhvtl_tcmu_handler.c`); only `vtltape`'s
  side polls (`usr/transport_tcmu.c`). No protocol change.
- **Userland** — return the listening socket until a harness connects, then the
  connection. This also makes the userland transport the test bed for the loop.
- **`mhvtl.ko`** — `mhvtl_fops` has no `.poll` (`kernel/mhvtl.c`). Supporting it
  needs a per-LU wait queue woken when a command lands on `cmd_list` and on LU
  removal, a `.poll` that registers on the wait queue before checking
  `cmd_list` (so no wakeup is missed), and a `VTL_GET_FEATURES` ioctl so new
  userspace can detect support (an old module answers `ENOTTY` and the daemon
  falls back to the timer). `VTL_POLL_AND_GET_HEADER` does not change, so old
  userspace keeps working with a new module. This is step 4, gated on the
  decision above.

### Control channel

SysV queues cannot be polled, and the library is not a plain FIFO consumer:
during a move, `recv_reply_from()` waits for one drive's reply and holds other
messages for `recv_msg()` (`usr/utils/q.c`, `usr/smc.c`).

- **Short term: keep SysV, check it on a 10–50 ms timer.** This bounds queue
  latency with no new IPC and is the first loop step.
- **Long term: a pollable control channel.** A `SOCK_SEQPACKET` Unix socket per
  daemon, with correlation IDs for replies, served alongside SysV until
  `vtlcmd`, the bay monitor and `mhvtl-control` move over. `SEQPACKET` keeps
  message boundaries and in-order delivery, so the move handshake's ordering
  guarantee is preserved. The socket also concentrates IPC cleanup in the
  socket's own lifetime instead of the "last daemon frees the SysV ids" dance.
- **Not proposed: a thread doing blocking `msgrcv()`.** It would compete with
  `recv_reply_from()` for the same message type and could take a drive reply or
  reorder held messages.

### What replaces the interval

- **Idle state** — a deadline from the last activity, with named durations
  instead of thresholds derived from the backoff.
- **Exit** — stop accepting work, quiesce the transport, exit once nothing is
  pending, instead of "the interval got large enough".
- **Mount completion and signals** — `SIGALRM`-driven mount completion becomes a
  `timerfd`; `SIGHUP` (config re-read) and the shutdown signals are read through
  a `signalfd` and acted on at a safe point between commands, as `SIGHUP`
  already is today (it only raises `reread_config_requested`, and
  `do_rereadconfig()` runs from the loop).

## Plan

Each step is its own PR with its own tests.

1. **Characterization tests for today's behaviour** — idle state, exit, `SIGHUP`,
   and message order during a move. *(Status: idle state and exit are covered by
   `tests/test_queue_backoff.c` from [#271](https://github.com/markh794/mhvtl/pull/271);
   move message ordering by `tests/test_q_reply.c`; `SIGHUP` config re-read by
   `tests/test_sighup_reread.c`. Step 1 complete.)*
2. **The event loop** with `timerfd` / `signalfd`, still calling `poll_cmd()`,
   with a 10–50 ms fallback timer for SysV and old modules, in the shared
   `event_loop.c`. No kernel change.
3. **The transport wake-fd op** for TCMU and userland. No kernel change.
4. **`.poll` and `VTL_GET_FEATURES` in `mhvtl.ko`.** Kernel change. **Gated on
   the open question below.**
5. **The Unix control socket alongside SysV**, starting with the library↔drive
   move messages.
6. **Move `vtlcmd`, the bay monitor and `mhvtl-control` to the socket.** SysV
   stays for an announced period, then its cleanup logic is removed.

## Open question

**Is TCMU the end state for the drive/library transport?** If yes, step 4 (kernel
`.poll`) is deferred or skipped and old modules ride the fallback timer until the
module is retired. If the kernel module stays a first-class backend — the #254
benchmarks still show it ahead of TCMU even at `Backoff 10` — step 4 is worth
doing so module users also get an event-driven loop. Steps 1–3 and 5–6 do not
depend on the answer; only step 4 does.

## Scope notes

- Steps 5–6 change how external tools (`vtlcmd`, the bay monitor,
  `mhvtl-control`) talk to the daemons. The socket interface and the IPC-cleanup
  rework are reviewed as their own design before any code, so SysV users are not
  broken.
- The already-merged fixes that came out of this investigation —
  [#271](https://github.com/markh794/mhvtl/pull/271) (poll again soon after a
  queue message), [#316](https://github.com/markh794/mhvtl/pull/316) (fail a
  MOVE MEDIUM when the drive does not answer, giving `recv_reply_from()` a
  deadline) and [#320](https://github.com/markh794/mhvtl/pull/320) (re-register a
  drive daemon after it restarts) — are stop-gaps and prerequisites, not the
  event loop itself.
