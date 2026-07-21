# Design decisions

A living record of architectural choices tied to CHERIoT / CHERI properties.
Each entry states **what** was chosen, **why**, **which CHERIoT property** backs
it, and the **rejected alternative**.

## I²C bus driver: capability-gated, lease-arbitrated, two-level-locked (availability-first)

> Status: **implemented (mechanism + real sensors).** `i2c_driver` has been
> reworked from the original minimal "inline MMIO, no access control, no
> arbitration" driver into the shared, access-controlled, priority-respecting bus
> manager described below, and the two real sensors (moisture `0x36`, AHT20
> `0x38`) have been converted onto it (sealed per-device caps + atomic batches).
> The capability distribution is verified by `cheriot-audit`
> (`audit/i2c_access.rego`, `all_pass = true`). The control loop has been split
> into its own highest-priority `irrigation` compartment feeding a telemetry
> thread via a mailbox. **Out of scope and not built:** the gas/CO₂ sensor
> compartment (so the long-lease/warm-up path is covered by code review and the
> design, not an end-to-end demo) and a runtime self-test (see "Runtime self-test
> as a possible demonstration" below for why, and what one would show).
> Implementation plan: `docs/superpowers/plans/2026-06-26-i2c-bus-driver.md`.

### The problem this answers

A single I²C bus (peripheral `i2c0`, the qwiic0 connector) is the only path to
every on-board sensor, and exactly one compartment (`i2c_driver`) holds its MMIO
capability. As soon as more than one application compartment wants the bus, four
questions arise — all under the heading of **availability**:

1. **Multiplexing.** How is the single bus shared between several application
   compartments without their transactions interleaving on the wire?
2. **Elaborate protocols without interference.** Some devices need a multi-step
   sequence (write a register, wait, read) that must run as a unit, and some
   need a *lengthy* warm-up / burn-in / calibration before they can be read
   (e.g. a greenhouse CO₂ sensor with a minutes-long warm-up). Such a device
   must be able to reserve itself across that window without another caller
   corrupting its sequence.
3. **Respecting priorities.** When several compartments want the bus at once,
   the highest-priority one must win, and priority inversion must be bounded.
4. **The safety loop must never be denied the bus.** The control loop
   (sense → policy → pump) is the project's top-priority availability guarantee;
   no other compartment — however it fails — may stall its access to the bus.

The current driver answers none of these: any compartment that includes the
header can address any device, transactions are not serialised against
concurrent callers, and the `i2c_write_read` helper even documents the
interleaving hazard ("sensors that need a delay … should use separate
`i2c_write` / `i2c_read` calls", during which another caller may seize the bus).

### Threading model and priorities

The current single `telemetry` thread fuses sensing, policy and networking, so a
blocked TLS/MQTT call can stall sensing. The redesign splits responsibilities
across threads so the safety loop is isolated and strictly highest priority
(CHERIoT-RTOS convention: **larger priority number = higher priority**):

| Thread | Compartment / entry | Priority | Role |
|---|---|---|---|
| Control loop | **new `irrigation`** / `irrigation_loop` | **3** | sense → policy → pump; writes the latest reading to its mailbox; sleeps between ticks |
| Telemetry / network | `telemetry` / `telemetry_loop` | 1 | reads the mailbox, encrypts, publishes; runs the inbound attestation/command state machine |
| Firewall | `Firewall` (unchanged) | 2 | packet RX responsiveness |
| TCP/IP | `TCPIP` (unchanged) | 1 | network stack |

The network-stack priorities (`TCPIP` = 1, `Firewall` = 2) are **conventions, not
requirements** — the upstream MQTT example comments that `Firewall` is raised to
2 only so "packets arrive immediately" and is "back-pressured by the message
queue if the network stack can't keep up." Both threads spend almost all their
time blocked on queues/futexes, so placing the control loop above them does not
starve them, *provided the control loop yields by sleeping between ticks* — which
it does, being a periodic sense loop rather than a CPU-bound one.

The control loop lives in its **own compartment**, not as a second thread inside
`telemetry`, specifically for availability: a fault in the network-facing
telemetry/attestation code cannot corrupt the safety loop's state, and the safety
loop is trivially legible as one isolated box in a diagram. The two threads share
only a **latest-reading mailbox**: `irrigation` owns a lock-protected
`SensorReading` and exposes a single one-way getter
(`control_get_latest_reading`). The control loop *overwrites* the mailbox each
tick and never blocks on telemetry; the telemetry thread reads whatever the
freshest snapshot is (microsecond critical section) when it gets around to
publishing. A wedged network therefore backs up entirely on the prio-1 telemetry
thread and can never reach the prio-3 loop. (The `attested` telemetry gate and
`lastWatering` state move with the telemetry/sense split accordingly.)

> **Superseded:** the mailbox described above carried a full `SensorReading`.
> It has since been narrowed to just `lastWatering`
> (`irrigation_get_last_watering(uint32_t*)`), and `telemetry` now reads
> moisture/temperature/humidity directly instead of via this mailbox — see
> "Sensor access split between irrigation (moisture) and telemetry
> (moisture + temperature)" below.

### Access control: sealed per-device capabilities

CHERIoT cross-compartment calls carry no reliable caller identity, so "device X
is reachable only by compartment Y" is made unforgeable with a **static sealed
capability** per device. Each device handle is a `DECLARE_AND_DEFINE_STATIC_-
SEALED_VALUE` sealed under a `STATIC_SEALING_TYPE` key owned by `i2c_driver`, and
granted at build time to exactly one named compartment. Every driver call takes
the handle; the driver `token_unseal`s it with its private key to authorise the
operation:

```c
struct I2CDeviceCap {
    uint8_t  address;        // 7-bit I²C device address
    uint8_t  opsMask;        // I2C_OP_READ | I2C_OP_WRITE
    uint8_t  flags;          // I2C_MAY_LEASE
    uint16_t maxTransferLen; // per-step byte cap → bounds head-of-line blocking
    uint16_t maxBatchMs;     // hard ceiling on any single i2c_transact's implicit reservation
    uint32_t maxLeaseMs;     // hard auto-expiry ceiling for any lease on this device
};
```

This is the established CHERIoT access-control idiom, with an almost line-for-line
precedent in the network stack: `NetAPI`'s `ConnectionCapability`
(`DECLARE_AND_DEFINE_CONNECTION_CAPABILITY(name, host, port, …)`) is a static
sealed value granted per-compartment that authorises connecting to exactly one
host:port, unsealed by the `NetAPI` compartment. `I2CDeviceCap` is that pattern
with `address`/`opsMask`/lease-rights in place of `host`/`port`. The same
mechanism underpins the allocator's `AllocatorCapability`. Possession of the
sealed handle *is* the access right: no key, no handle, no bus — and the entire
distribution (which compartment may touch which device, with which operations,
and whether it may take a lease) is **statically auditable from the firmware
image** with `cheriot-audit`, exactly mirroring the existing attestation audit
("`fake_tpm_sign` imported by `attestation` and no one else; `spi0` MMIO held by
`attestation` and no one else").

In the converted system, `moisture_sensor` holds only the `0x36` handle,
`temperature_sensor` only the `0x38` handle, and `irrigation`, `telemetry` and
`comms` hold none (the control loop reaches sensors only through the sensor
compartments).

### Bus access mechanism (chosen design): two-level locking

`i2c_driver` keeps the conventional CHERIoT "inline MMIO on the caller's thread"
shape (it is the sole holder of the `i2c0` `MMIO_CAPABILITY`, reached only via its
own API functions). Concurrency is governed by **two independent primitives**:

- **Bus lock** — a `FlagLockPriorityInherited` (the SDK's priority-inheriting
  flag lock, built on the futex `FutexPriorityInheritance` flag: waiters are
  woken in priority order and the holder's priority is boosted to the highest
  waiter's). It is held **only for the duration of a single transfer** (one write
  or one read) and **never across a `DELAY`**.
- **Device reservation** — a per-device `{ownerThreadId, deadline}` record,
  read lock-free before each transfer and claimed/released via the reservation
  table. It expresses "this device is currently owned by one caller." A *batch*
  takes a short reservation for its own span; a *lease* is just a long,
  caller-controlled reservation with a deadline.

A batch runs on the **caller's own thread**:

```
reserve(device)                              // outer, may block if reserved by another
  for each step:
    WRITE/READ: take bus lock → verify reservation still ours
                → transfer (≤ maxTransferLen) → release bus lock
    DELAY:      sleep for delayMs *without* holding the bus lock
release(device)                              // (omitted for an explicit lease)
```

Lock ordering is acyclic: the reservation is the outer, long-lived claim; the bus
lock is the inner, per-transfer claim; the reservation state is *read* lock-free
during a transfer (so the bus lock is never nested under the reservation-table
lock). Because the `DELAY` is taken *without* the bus lock, other compartments
freely use **other** devices during a warm-up wait — independent I²C devices do
not physically interfere, so reserving the CO₂ sensor never blocks a moisture or
temperature read.

Multiplexing and priority therefore fall out of the **kernel scheduler** rather
than any user-space arbiter: the priority-inheriting bus lock wakes contenders in
priority order, and a high-priority waiter boosts whatever low-priority caller
currently holds the lock, so the worst-case wait for the bus is one in-flight
transfer. With `maxTransferLen` capped (e.g. 32 bytes at 100 kHz ≈ ~3 ms), that
worst case is small and computable — this is the headline bound on how long the
control loop can ever wait for the bus. Bounding the atomic unit to keep the
worst case computable is the same motivation behind Van Bulck et al.'s proposed
hardware atomicity monitor, which limits atomic-section duration precisely to
preserve a deterministic interrupt/wait latency (Van Bulck, Noorman, Mühlberg,
Piessens, *Towards Availability and Real-Time Guarantees for Protected Module
Architectures*, 2016, §4.1.3).

### API surface

All calls are synchronous and take a sealed `I2CDeviceCap`:

```c
// Atomic batch: steps run as a unit on this device (an implicit short
// reservation spans the batch); DELAY is a *minimum* wait, during which OTHER
// devices remain usable. A single transfer step may not exceed maxTransferLen.
enum  { I2C_WRITE, I2C_READ, I2C_DELAY };
struct I2CStep { uint8_t kind; uint16_t len; uint16_t delayMs; void *buffer; };
int i2c_transact(SObj dev, I2CStep *steps, size_t n, Timeout *t);

// Long-lived reservation for warm-up/burn-in: reserve THIS device across the
// holder's own sleeps. durationMs is clamped to cap.maxLeaseMs and auto-expires.
// Requires I2C_MAY_LEASE in the capability.
int i2c_lease_acquire(SObj dev, uint32_t durationMs, Timeout *t);
int i2c_lease_release(SObj dev);

// Back-compat wrappers, re-expressed as one/two-step batches (now cap-gated):
int i2c_write(SObj dev, const uint8_t *d, size_t len);
int i2c_read (SObj dev, uint8_t *d, size_t len);
int i2c_write_read(SObj dev, const uint8_t *w, size_t wl, uint8_t *r, size_t rl);
```

While a lease is held, the holder's own `i2c_transact` calls proceed; another
compartment's request *to that device* blocks (with its own timeout) until the
lease is released or auto-expires, while its requests to *other* devices run
normally. The AHT20 read becomes one batch `[WRITE trigger][DELAY 85][READ 6]`
and the moisture read `[WRITE channel][DELAY 2][READ 2]`, which also closes the
documented write-then-read interleaving hazard.

### Scheduling and timing semantics

- **`DELAY` is a floor, not a deadline.** The caller's thread sleeps for *at
  least* `delayMs`; it may resume slightly later if a single foreign transfer is
  in flight when it wakes. I²C device timings are almost always one-sided
  ("wait ≥ N ms"), so overshoot is harmless.
- **Atomicity unit = one transfer.** A transfer is never chopped mid-flight;
  arbitration happens only at transfer boundaries (i.e. between bus-lock
  acquisitions). Capping `maxTransferLen` is what keeps that boundary frequent
  and the worst-case latency bounded.
- **Control-loop latency.** The control loop's sensor devices are never leased by
  anyone else, so it never waits on a device reservation; its only wait is for
  the bus lock, bounded by one capped transfer (~3 ms in the example above), with
  priority inheritance guaranteeing it is next in line.

### Lease lifecycle and availability safety

`i2c_lease_acquire` marks the device reserved for the caller until
`min(durationMs, cap.maxLeaseMs)`. The holder polls via ordinary `i2c_transact`;
`i2c_lease_release` frees it early. Critically, the reservation **auto-expires**
at its deadline: a buggy or compromised holder cannot lock a device out for
longer than `cap.maxLeaseMs`, and a holder that transacts after expiry receives
`-ETIMEDOUT` (lease lost). This bounds the availability impact of any single
faulty lease holder. To keep that bound unconditional, a lease whose effective
duration would be unbounded is refused outright: `i2c_lease_acquire` returns
`-EINVAL` when the computed hold time is zero (i.e. a cap that sets
`I2C_MAY_LEASE` but leaves `maxLeaseMs == 0`). A leasable device must therefore
declare a finite ceiling, so the firmware image cannot contain a capability that
grants an unexpirable bus reservation.

### Runtime self-test as a possible demonstration (not implemented)

The access-control and lease-permission checks are proven **statically** — the
`cheriot-audit` policy (`audit/i2c_access.rego`) shows from the firmware image
that only `i2c_driver` holds the `i2c0` MMIO and that the only `I2CDeviceKey`
sealed caps are the moisture (`0x36`) and temperature (`0x38`) handles, each held
by exactly one sensor compartment — plus by code review of the `authorize()` /
lease-flag checks. A *runtime* self-test was deliberately **not** built, because
the only way to make the driver actually return `-EPERM` at runtime is for some
compartment to hold a deliberately-wrong capability (e.g. a read-only cap, then
attempt a write) — and giving that cap to any application compartment (the
`irrigation` was the candidate) would put a bus capability in a compartment the
audit otherwise proves holds none, breaking the very "the safety loop holds no
bus capability" isolation claim the design rests on, and flipping the audit's
`all_pass` to false.

Should a runtime demonstration ever be wanted (e.g. for a live demo or a video),
the clean way to add it without weakening the audit is a dedicated, bring-up-only
`i2c_selftest` compartment that holds the deliberately-restricted test caps and
is excluded from the production audit's authorised set (or only linked into a
debug build). Such a self-test would show, on the UART log:

- **Authorised access succeeds** — a read through a correctly-scoped cap returns
  `0` (or a bus `-EIO`), never `-EPERM`.
- **Operation outside the cap is rejected** — a write attempted through a
  read-only cap returns `-EPERM`, demonstrating the per-operation `opsMask`
  enforcement.
- **Unprivileged lease is rejected** — `i2c_lease_acquire` on a cap without
  `I2C_MAY_LEASE` returns `-EPERM`.
- **Unbounded lease is rejected** — `i2c_lease_acquire` on a leasable cap with
  `maxLeaseMs == 0` returns `-EINVAL` (the auto-expiry guarantee above).
- **Non-interference under contention** — with a (mock) device holding a lease,
  the control loop's reads to the *other* devices continue uninterrupted, while a
  second caller's request to the *leased* device blocks until release/expiry.

These are the behaviours the static audit and code review already establish; the
self-test would only turn them into an observable runtime artifact, at the cost
of the extra debug-only compartment.

### Error model

`-EPERM` (operation or lease not permitted by the capability), `-EINVAL`
(malformed steps or a step over `maxTransferLen`), `-EIO` (bus NAK / error;
aborts only the offending batch, after which the driver resets the FIFOs and
continues), `-ETIMEDOUT` (caller timeout, or lease expired), `-EBUSY` (device
reserved by another and the caller's timeout elapsed). Bus errors never tear down
the shared driver state.

### Worked example (for the thesis narrative)

A CO₂ sensor (hypothetical, `I2C_MAY_LEASE`, `maxLeaseMs` = 3 min) takes a
2-minute warm-up lease and polls itself every few seconds. Concurrently the
prio-3 control loop reads moisture (`0x36`) every tick, and the telemetry
thread reads both moisture (`0x36`) and AHT20 (`0x38`) on its own cadence
(pre sensor-access-split, this example originally had the control loop
reading both devices; see "Sensor access split between irrigation
(moisture) and telemetry (moisture + temperature)" below for the current
per-compartment split — the availability argument here is unaffected by
which compartment reads which device):

- The lease reserves only the CO₂ device, so the control loop's and telemetry
  thread's reads to `0x36` and `0x38` are never blocked by the reservation.
- Each of those transfers waits at most one in-flight capped transfer for the
  bus lock; priority inheritance makes the prio-3 control loop the next
  holder whenever it's waiting.
- During the CO₂ sensor's own `DELAY`s (and the AHT20's 85 ms settle), the bus
  lock is free, so the other devices are serviced.
- If the CO₂ compartment hangs while holding the lease, the reservation expires
  after `maxLeaseMs` and the device becomes available again; the control loop was
  never affected regardless, because no central component sits in its path.

Thus all four requirements hold simultaneously: the bus is multiplexed, the
warm-up runs uninterrupted, priorities are respected with bounded inversion, and
the safety loop's bus access cannot be denied.

**Why this design.** The question is filed under *availability*, and the control
loop's availability is the project's first guarantee. Two-level locking keeps the
kernel scheduler as the single source of truth for priority (correct, bounded
priority inheritance for free) and — decisively — introduces **no central
component in the control loop's path**: the loop drives the bus on its own
thread, so no other compartment's failure can deny it the bus beyond one bounded
transfer. The warm-up/non-interference requirement is met by the *reservation +
atomic-batch* abstraction, which is independent of the serialisation mechanism,
so nothing is lost by choosing the simpler, more idiomatic locking model over a
bespoke arbiter.

**Related work.** Framed against Alder et al.'s Aion (*Aion: Enabling Open
Systems through Strong Availability Guarantees for Enclaves*, CCS 2021), which
names the properties a shared-resource platform should guarantee — bounded
activation latency, guaranteed device access, safety independence, no trust
hierarchy — this design satisfies the first three (bounded bus-lock wait;
auto-expiring leases bound how long a device can be denied; the isolated
`irrigation` compartment is unaffected by telemetry/network faults) but
deliberately **not** the fourth. Aion targets multiple mutually distrusting,
equal-priority applications and gives all of them the same guarantee
simultaneously; this design instead follows the strict-priority model that
Aion's own related-work comparison contrasts itself against (their "Masti et
al." row), because CHERIoT-plantnode has exactly one availability-critical
thread — the control loop — not an open multi-tenant system, so guaranteeing
only the highest-priority caller is the right trade-off here, not a limitation
to fix.

Within that, the bus lock and the device lease are not equally strong
guarantees, and it is worth being precise about which is which — in the spirit
of Alder et al.'s graded treatment of guarantees under an adversary that can
delay or interrupt execution (*About Time: On the Challenges of Temporal
Guarantees in Untrusted Environments*, SysTEX 2023, §3): the bus lock gives a
near-unconditional bound (one capped transfer, regardless of any other
compartment's behaviour), while the lease gives a bounded-but-nonzero guarantee
(a misbehaving holder can deny one specific device for up to `maxLeaseMs`
before auto-expiry) — a weaker, time-bounded rather than immediate, form of
availability.

**CHERIoT property.** (1) *Access control / least privilege*: bus access is a
sealed capability minted under a sealing key that never leaves `i2c_driver`;
the per-device grant (address, operations, lease right, duration ceiling) is
statically auditable from the image, like the existing `ConnectionCapability` and
attestation grants. The `i2c0` MMIO capability is held by one compartment and
reached only through its API. (2) *Availability*: the safety loop is isolated in
its own compartment at the top priority, fed by an overwrite mailbox that never
blocks on the network; bus arbitration uses the kernel's priority-inheriting
lock (bounded inversion); leases auto-expire (bounded lockout); and no central
thread can deny the loop the bus. (3) *Spatial safety*: transfer buffers cross the
boundary as bounded capabilities and never leave the caller's own thread;
`I2CDeviceCap` and `I2CStep` are plain data apart from those explicitly-bounded
buffers.

**Rejected alternatives.**

- **A dedicated driver thread with a request queue and per-request futex
  completion** (the main alternative considered). Public calls would enqueue a
  request — carrying bounded capabilities to the caller's buffers and the
  caller's priority — to a single `i2c_driver` thread that runs an explicit
  scheduling loop (pick the highest-priority eligible step, run one transfer,
  wake the caller). Its merits are real: only *one thread* ever dereferences the
  MMIO capability (a *structural* single-writer property, not merely a runtime
  one), and all arbitration policy lives in one legible loop that could later
  host fancier scheduling (fair-share, EDF). It was **rejected** for three
  reasons aligned with the availability framing: (i) it puts a **single point of
  failure squarely in the control loop's critical path** — if that one thread
  wedges, *all* bus access dies, including the safety loop, contradicting the very
  reason the control loop was isolated; (ii) it **re-implements priority** in
  user space and, unless the driver thread also adjusts its own priority per
  request, causes a system-level inversion (all bus work running at the driver
  thread's priority regardless of the requester's) — the same failure mode Van
  Bulck et al. flag for synchronous IPC between separately-schedulable
  protection domains, where "a high-priority task waits for a reply from a
  low-priority service that it interrupted" (Van Bulck, Noorman, Mühlberg,
  Piessens, *Towards Availability and Real-Time Guarantees for Protected Module
  Architectures*, 2016, §4.2.1); (iii) it is **non-idiomatic
  and heavier** — no existing CHERIoT MMIO driver is built this way, and it adds a
  thread, a queue, completion futexes, and cross-thread delegation of caller
  buffers. The two-level-lock design obtains the same multiplexing, the same
  warm-up/non-interference behaviour, and *better* priority correctness, without
  the central failure point. (Approach A would be preferable only if minimising
  the number of threads that hold the MMIO capability outranked control-loop
  availability — which here it does not.)
- **Bus-frozen leases** (a lease freezes the *whole* bus for its duration).
  Rejected: a minutes-long CO₂ warm-up would block the prio-3 control loop's
  moisture/temperature reads for minutes, destroying the safety guarantee.
  Device-level reservation achieves non-interference without this, because
  independent I²C devices do not interfere.
- **A compile-time `address → compartment` allow-list inside the driver.**
  Rejected: CHERIoT gives the callee no reliable caller identity, so this cannot
  be enforced without extra plumbing; sealed capabilities provide the identity
  unforgeably and auditably.
- **Inline MMIO with no locking (today's driver).** Rejected: it cannot serialise
  concurrent callers, cannot offer atomic multi-step sequences (the documented
  `i2c_write_read` interleaving hazard), and has no notion of priority or
  reservation.
- **Both new threads inside a single `telemetry` compartment.** Rejected: it
  leaves the safety-critical control loop sharing a fault domain with the
  network-facing telemetry/attestation code; a separate `irrigation`
  compartment isolates the availability guarantee and is clearer to present.

### CHERIoT toolchain support observed while building this feature

A notable finding of this work is that **the compartment is a first-class object
at every layer of the CHERIoT toolchain**, not merely a runtime construct. The
same boundary that the design reasons about is understood — and enforced — by the
build system, the compiler front-end, the linker/loader, the static auditor, and
even the IDE. Building the I²C driver exercised each of these, and they are worth
recording as evidence of the toolchain's completeness.

- **Build system (xmake).** A compartment is a build target: `compartment("…")`
  with its own source set, and the firmware image lists its compartments as
  dependencies. The build passes each compartment's identity to the compiler as
  `-cheri-compartment=<name>`, so compartment membership is decided by the build
  graph, not by convention.

- **Compiler-enforced compartment membership.** A cross-compartment entry point
  is declared with `__cheri_compartment("irrigation")`, and the compiler
  cross-checks the *declared* compartment of every entry against the *actual*
  compartment it is being compiled into, emitting
  `cheri_implemented_wrong_compartment` on a mismatch ("entry declared for
  compartment 'irrigation' but implemented in 'comms'"). This is a genuine
  safety net: silently compiling an entry point into the wrong compartment would
  place code — and its capabilities — on the wrong side of an isolation boundary,
  precisely the class of error the whole model exists to prevent. During this
  work the diagnostic surfaced against the new `irrigation.cc`; it turned out
  to be a *stale-index false positive* — the IDE's `compile_commands.json`
  predated the new compartment and had no entry for the file, so `clangd` fell
  back to a sibling file's command (`-cheri-compartment=comms`). The genuine
  `xmake` build always compiled the file with `-cheri-compartment=irrigation`
  (confirmed in the firmware image and the audit), and regenerating the compile
  database (`xmake project -k compile_commands`) cleared the diagnostic. The
  episode is itself instructive: the compiler's compartment-awareness reaches all
  the way into editor tooling, and the *practical lesson* is that the compile
  database must be regenerated whenever a compartment or source file is added.

- **First-class capability machinery in the language/SDK.** Access control here is
  not hand-rolled: the SDK provides sealed capabilities as language constructs —
  a compartment mints a sealing key with `STATIC_SEALING_TYPE`, grants per-device
  authority as build-time `DECLARE_AND_DEFINE_STATIC_SEALED_VALUE` objects, and
  recovers the plain data with `token_unseal`. The same idiom underlies the
  allocator's `AllocatorCapability` and the network stack's `ConnectionCapability`,
  so "capability = an unforgeable, statically-granted token" is a toolchain-wide
  pattern, not a bespoke mechanism.

- **Static auditability of the capability graph.** The linker/loader records, in
  the firmware image, which compartment imports which MMIO range and holds which
  sealed object, and `cheriot-audit` evaluates machine-checkable Rego policies
  over that graph. This feature ships with `audit/i2c_access.rego`
  (`data.i2c_access.all_pass = true`), which proves *from the built image* that
  `i2c0` MMIO is held only by `i2c_driver`, that the `I2CDeviceKey`-sealed caps
  are held only by the two sensor compartments, and — because the policy derives
  the "other compartments" set from the live image — that `irrigation` (and any
  future compartment) holds no bus capability. The security claim is therefore
  not argued in prose but *checked by tooling* against the artifact that actually
  runs.

Taken together, these layers mean a compartment boundary is specified once and
then upheld, and *verified*, by independent tools at build, compile, link, and
audit time — a level of end-to-end support for the isolation model that is the
practical enabler of every security and availability property claimed above.

### 2026-07-21: mandatory `maxBatchMs` ceiling closes the unbounded-batch gap

**What changed:** every device reservation now carries a real, non-zero
deadline. Previously, an explicit lease (`i2c_lease_acquire`) set
`deadlineTick = now + clamp(durationMs, cap.maxLeaseMs)` and was reclaimed by
`expire_locked()`, but a bare batch (`i2c_transact`, used by every current
sensor driver) called `reserve_device(cap.address, 0, t)`, which set
`deadlineTick = 0` — and `expire_locked()` explicitly never reclaims a
`deadlineTick == 0` entry. `I2CDeviceCap` gains a mandatory `maxBatchMs`
field (sealed alongside `maxTransferLen`/`maxLeaseMs`); `i2c_transact` rejects
a cap with `maxBatchMs == 0` outright (`-EINVAL`) and otherwise reserves the
device via `reserve_device(cap.address, cap.maxBatchMs, t, /*isLease=*/false)`,
so a batch's own reservation always expires. The reservation record's
`isLease` flag (set explicitly by the caller of `reserve_device`, no longer
inferred from `deadlineTick != 0`) distinguishes a batch's short, mandatory
ceiling from a lease's long, `I2C_MAY_LEASE`-gated one, so `expire_locked()`
and the re-entrant "am I still the batch that reserved this" check treat the
two consistently. The moisture (`0x36`) and AHT20 (`0x38`) caps declare
`maxBatchMs` 20 and 150 respectively — headroom over their measured worst-case
batch spans (~4 ms and ~87 ms) — closing the gap between this section's
existing claim that a batch "takes a short reservation for its own span" (see
"Bus access mechanism" above) and the prior implementation, where that claim
did not actually hold.

**Why:** an unbounded batch — whether by design (e.g. the AHT20's 85 ms
delay step) or by bug/hang — could stall a higher-priority caller
indefinitely, since nothing reclaimed a `deadlineTick == 0` reservation. That
directly undermines this section's own "priority inversion must be bounded"
requirement (see "The problem this answers" above): a batch is exactly the
kind of caller-held device claim the availability argument assumed was
time-bounded, and it wasn't.

**CHERIoT property demonstrated:** the bound is a capability-declared,
per-device, statically-auditable fact — `maxBatchMs` sits in the same sealed
`I2CDeviceCap` as `maxTransferLen`/`maxLeaseMs`, granted once at build time and
unforgeable thereafter — not a coding convention that a driver author has to
remember to honour on every call site, and not something enforced only by code
review.

**Rejected alternative:** a single shared ceiling field for both batches and
leases. Rejected because the two mechanisms need different magnitudes (a batch
is the span of one call; a lease can legitimately run for minutes, e.g. a CO₂
sensor's warm-up) and different opt-in requirements (`maxBatchMs` applies
unconditionally to every device, since every device is batch-accessed, while
`maxLeaseMs` only matters — and is only checked — for devices that also set
`I2C_MAY_LEASE`). Collapsing them into one field would either make batches pay
lease-scale ceilings or force leases down to batch-scale durations, neither of
which fits the real devices this driver serves.

Spec: `docs/superpowers/specs/2026-07-21-i2c-bounded-batch-design.md`.

## Sensor access split between irrigation (moisture) and telemetry (moisture + temperature)

**What:** Removed the `data_processing` compartment, which was the sole
permitted caller of `moisture_sensor` and `temperature_sensor` and the
source of the full `SensorReading` that `irrigation` wrote into its
mailbox each tick. `irrigation` now calls `moisture_sensor` directly (it
has no compiled reference to `temperature_sensor` at all) and uses the
result only for its own watering decision. `telemetry` now calls both
`moisture_sensor` and `temperature_sensor` directly, once per telemetry
tick, to build the `SensorReading` it publishes. The mailbox between the two
threads is kept but narrowed from a full `SensorReading` to a single
`uint32_t` (`lastWatering`, the most recent pump-activation timestamp):
`control_get_latest_reading(SensorReading*)` became
`irrigation_get_last_watering(uint32_t*)`. `policy_evaluate` narrowed from
`policy_evaluate(const SensorReading*)` to `policy_evaluate(uint16_t
moistureRaw, uint32_t timestamp)`, and `PolicyOutcome::TempAlert` — which
nothing produces once `irrigation` no longer reads temperature — was
removed, along with the `tempMaxCx10` parameter of `policy_set_thresholds`.

**Why:** `irrigation`'s only decision is a moisture threshold; it never
needed temperature or humidity, and reading them anyway coupled telemetry's
content and cadence to the control loop's read cadence for no reason. Each
compartment now reads only what it uses.

**CHERIoT property.** *Access control / least privilege*: which compartment
may call `moisture_sensor` / `temperature_sensor` is fixed by whether that
compartment's compiled object contains a reference to the driver's exported
entry points — `irrigation` has no compiled reference to
`temperature_sensor`, so the linker never generates an import for it, the
same "absence of a compiled reference is absence of the capability to call"
property the firmware image already exhibits for MMIO and sealed-object
holders (see the I²C bus driver audit above). The two drivers' own sealed
`I2CDeviceCap`s (`moisture_sensor` → `0x36`, `temperature_sensor` → `0x38`)
are unchanged, so `audit/i2c_access.rego`'s `all_pass` continues to hold.

**Rejected alternative:** Keep `data_processing` as a two-entry-point
gatekeeper (one moisture-only entry for `irrigation`, one full-reading
entry for `telemetry`). Rejected as indirection with no remaining purpose:
once each caller's sensor set is fixed independently at the linker level,
routing both through a shared compartment adds a hop without adding any
access-control property that direct calls don't already provide.

**Side effect — LCD display now owned by `telemetry`.** `display_sensor_readings()`
(the LCD sensor-reading update) was previously called only from
`data_processing`, on `irrigation`'s ~6-second cadence, independent of
attestation status. Deleting `data_processing` orphaned that call entirely;
it has been rewired into `telemetry`, called once a reading is confirmed
valid, in the same place telemetry is published. Two consequences worth
recording: the LCD now refreshes on `telemetry`'s ~10-second telemetry
cadence instead of `irrigation`'s ~6-second cadence, and it no longer
updates until the device has been attested (previously it updated
unconditionally). Both are accepted as a direct, minor consequence of moving
sensor reads into `telemetry`, not a design goal of this change.

## Attestation replaced by a mock API (focus change)

**What:** The real remote-attestation protocol has been replaced by an abstract
mock request/response. Focus has moved off attestation itself, so the board no
longer measures or signs anything real, and the exchange deliberately avoids any
concrete scheme (no slots, no image hash, no signature) — it just asks the
verifier "am I attested?", attaching an opaque token, and is told "yes".
Concretely:

- **Wire (`plantnode/attestation`) is now plaintext JSON**, not secretbox.
  Device → verifier request:
  `{"query":"am_i_attested","device":"plantnode-001","token":"<64-hex>"}`, where
  `token` is an opaque blob standing in for "whatever a real attestation would
  attach". Verifier → device response:
  `{"attested":true,"device":"plantnode-001"}`. Key distribution and telemetry
  are unchanged — still Noise-N session keys + `hydro_secretbox`.
- **The compartment chain is kept and mocked end to end:** `telemetry` →
  `attestation` (gathers abstract evidence) → `tpm` (vouches with an opaque
  token) → `telemetry` → `comms` (publishes the JSON query).
  `attestation_get_evidence()` fills the device id and asks the (mock) `tpm` for
  an opaque token via `tpm_attest()`; neither touches flash, sensors, network or
  any real key. Both files are marked MOCK in their headers.
- **`fake_tpm` renamed to `tpm`** (compartment + `src/tpm.{cc,h}`), now exposing
  a single abstract `tpm_attest()`. The comments make clear it is *not* a real
  TPM — it is a mock stand-in for where a hardware root of trust would sit.
- **Telemetry stays gated:** the board withholds all `plantnode/telemetry` until
  it receives the mock `attested:true`, exactly as before — only the check is now
  a mock.
- **Removed real machinery:** the dual-nonce challenge-response state machine
  (`telemetry`), the SPI-flash reader / ELF hasher / GPIO boot-slot strap and
  libhydrogen dependency (`attestation`), the Ed25519 signer / seed / libhydrogen
  (`tpm`), the RA crypto helpers (`crypto_encrypt_bytes`, `crypto_gen_nonce`,
  `crypto_combine_nonce`) and the RA message-type tags / combine context /
  quote-wire serialisation, plus the slot / image-hash / signature fields of the
  attestation type (`plantnode_types.h`, now a minimal device-id + opaque-token
  `AttestationEvidence`). On the charter, the libhydrogen ctypes bridge,
  signature/nonce/image-hash verification, and the dual-nonce handshake were
  replaced by a tiny mock verifier (`charter/attestation.py`).

**Why:** The thesis focus shifted away from the attestation/measurement protocol
itself; the rest of the system (telemetry, encryption, the compartment topology)
needs a plausible *placeholder* attestation step, not a working one. A mock that
preserves the API shape and the cross-compartment call path keeps the demo flow
(and the telemetry gate) intact while removing the real crypto/flash burden.

**CHERIoT property:** The compartment *topology* is preserved — `attestation` and
`tpm` are still distinct, single-purpose compartments and `tpm_attest` is still
imported only by `attestation`, so the "the agent that gathers evidence is not
the one that holds the root of trust" boundary still reads from the firmware
image. But note the security claim is now hollow: with an abstract mock token and
no real measurement, there is no real authentic-execution guarantee. This is a
deliberate scaffold, not a security mechanism.

**Rejected alternative:** Keep the real signed-quote protocol and just stub the
verifier's verdict. Rejected because the device-side flash read + Ed25519 signing
is exactly the machinery the focus has moved away from, and leaving it in place
(now unverified) would be misleading dead weight.

> The detailed entry below ("Remote attestation: split measurer …") is
> **superseded** by the mock and kept only as a historical record of the real
> protocol that was implemented before the focus change.

## Watering history folded into the encrypted telemetry payload

**What:** Removed the dedicated `comms_publish_pump_event` and
`comms_publish_temp_alert` entry points (and their cleartext
`plantnode/status/pump` and `plantnode/status/temp` topics). The most recent
pump-activation timestamp is now carried as a `last_watering` field on
`SensorReading` and serialised into the periodic `plantnode/telemetry` payload
(`"lastWatering"`), which the crypto compartment encrypts with `hydro_secretbox`
before it leaves the device. `telemetry` owns the `lastWatering` state across
the sense loop; the policy engine's `PumpActivation` outcome updates it.

**Why:** The pump/temp status logs duplicated data already present in telemetry
(moisture, temperature, timestamp) and shipped it as plaintext on separate
topics. Folding watering history into the single authenticated, encrypted
telemetry channel removes that redundant cleartext surface and gives every
consumer a self-contained, confidentiality- and integrity-protected snapshot.

**CHERIoT property:** The `comms` compartment is the sole holder of the sealed
MQTT handle and the only compartment permitted to call the network drivers;
`SensorReading` is pointer-free plain data, safe for cross-compartment transfer.
Narrowing comms' outbound API to one encrypted telemetry call (plus key /
attestation) keeps the network-facing surface of that compartment minimal and
routes all sensor-derived data through the crypto compartment's secretbox rather
than emitting it in the clear.

**Rejected alternative:** Keep the separate `status/pump` and `status/temp`
topics. Rejected because it leaves plaintext sensor-derived data on the wire and
widens the comms compartment's publish API for no information the encrypted
telemetry channel does not already convey.

## Pump activation POC: onboard LED stands in for the relay pin

**What:** `pump_driver`'s `pump_on()`/`pump_off()` remain placeholders — no
relay GPIO pin is defined yet and both still return `-ENOSYS` — but each now
also toggles Sonata onboard LED 0 via `MMIO_CAPABILITY(SonataGpioBoard,
gpio_board)`. This is the compartment's first real GPIO capability grant.
`policy_engine.evaluate()` now actually calls `pump_on()` /
`thread_millisecond_wait(2500)` / `pump_off()` on a low-moisture reading
(previously only a `// TODO` comment sat there and the call chain was never
exercised), so the LED lights for a ~2.5s pulse whenever the watering policy
fires.

**Why:** The pump relay hardware isn't wired up yet, so there was no way to
see the sense → policy → pump call chain actually do anything observable.
The onboard LED is a real, already-working GPIO peripheral that can visibly
confirm the decision logic and compartment boundaries (`irrigation` →
`policy_engine` → `pump_driver`) fire correctly end-to-end, without
pretending the relay itself works.

**Tradeoff — blocking pulse:** `pump_on()`/`wait`/`pump_off()` run inline
inside `policy_evaluate()`, so a watering event blocks the `irrigation`
thread for ~2.5s before it can loop back to the next sense/policy tick
(control cadence is otherwise 6s, per `irrigation_loop()`). This was chosen
over a non-blocking timer because it matches the original TODO's intent
literally and keeps the POC simple; it delays — but does not drop — the next
moisture reading. Once a real relay pin exists, this should become a
non-blocking timer so watering no longer stalls the sense/policy cadence.

**CHERIoT property:** `pump_driver` remains the sole holder of the GPIO
capability (now backed by a real `MMIO_CAPABILITY`); `policy_engine` remains
the only compartment permitted to call it. No other compartment's capability
surface changed.

**Fake, self-cycling moisture reading:** `irrigation` calls a new
`moisture_sensor::moisture_read_raw_mock()` instead of the real
`moisture_read_raw()`. It returns a static counter that starts at 900 and
decrements by 60 each call; only once a reading has actually gone *below*
`policy_engine`'s low threshold (300, strict `<`, matching
`policy_evaluate`'s comparison) does it reset back to 900. (An earlier
version reset as soon as the counter reached exactly 300, which meant
`moistureRaw < sMoistureLow` was never true and the pump/LED never fired —
fixed by resetting one step later.) This lets the sense → policy → pump
chain (and the LED) trip repeatedly without needing real soil-sensor
hardware or manual test input. `irrigation` and `telemetry`'s telemetry
read both call the mock now (real `moisture_read_raw()`, unused for now, is
left intact). Once real sensor + relay hardware exist, `moisture_read_raw_mock`
should be removed and both call sites switched back to `moisture_read_raw`.

**Display never cleared "watering" notice:** `policy_engine.cc` called
`display_pump_activation(true)` when the pump activates but never called it
with `false`, so the LCD stayed stuck showing "currently watering" forever
after the first trigger. `display_pump_activation(false)` is now called
right after `pump_off()` completes, matching what `display_policy.h` already
documented as the intended contract (`true` = watering started, `false` =
watering done).

**Known quirk — early-boot sleeps run short on real hardware:** On real
Sonata hardware (not observed as an issue in earlier runs before this POC),
`irrigation`'s 6-second `thread_sleep()` cadence collapsed into a rapid
burst: 11 fake-moisture reads (a full mock cycle) fired within tens of
milliseconds of each other, all before/around `comms`'s "Starting network
stack..." log line, then reverted to correct ~5-6s spacing once the network
stack was up. Diagnostic instrumentation (`thread_systemtick_get()` printed
alongside each reading) confirmed this is not a UART-buffering display
artifact: `Thread::ticksSinceBoot`, recomputed from the live hardware timer
register on every scheduler tick, genuinely advanced only ~3 ticks (~30ms)
across the whole pre-network burst, instead of the ~6600 ticks (66s) 11 real
6-second sleeps would need. Reading through the scheduler's timer code
(`cheriot-rtos/sdk/core/scheduler/timer.h`) didn't turn up an obvious bug —
the single-hardware-timer-compare design looks correct (it arms for the
earliest deadline across all waiting threads and only wakes threads whose
deadline has passed). The likely trigger is CHERIoT RTOS's boot-time
ordering: `Timer::interrupt_setup()` / PLIC enablement versus
`irrigation`'s thread starting and `comms_connect()`'s (likely
PHY/DHCP-heavy) synchronous early work — i.e. scheduler/SDK internals in
`sonata-software`, outside this project. Root cause was not fully pinned
down; investigating further would mean digging into CHERIoT RTOS's boot
sequence itself.

**Workaround attempt 1 (rejected): fixed startup delay.** `irrigation_loop()`
first tried a fixed `thread_sleep()` (1s, then 10s) before entering its main
loop, on the theory that giving the network/boot-time work a head start would
avoid the burst. Tested on real hardware at 10s: the burst still happened —
it simply shifted to align with wherever `comms_connect()`'s negotiation
currently was, immediately after the delay elapsed. This confirmed a fixed
delay cannot work: the quirky window's length tracks how long the network
stack takes to come up (variable, hardware/environment-dependent), not
elapsed boot time, so no constant reliably outlasts it.

**Workaround attempt 2 (reverted): measure real elapsed ticks, re-arm as
needed.** `irrigation.cc` briefly stopped trusting a single `thread_sleep()`
call — a `sleep_ticks_robust()` helper recorded `thread_systemtick_get()`
before sleeping, then re-armed for any remaining ticks until the real target
elapsed, self-correcting regardless of *why* a sleep returned early. This
worked, but was reverted: it adds complexity and an extra scheduler-call loop
to `irrigation` purely to paper over an unexplained upstream quirk, for a
burst that's cosmetic (log spam during boot) and doesn't affect correctness
once the network stack is up — not judged worth carrying.

**Workaround attempt 3 (rejected): disguised fixed delay via a fake sensor
warmup call.** Tried wrapping a 2s `thread_sleep()` in a
`moisture_sensor_warmup_mock()` function — presented via its name/doc comment
as legitimate I2C sensor power-on warmup time, with the real reason (the
early-boot sleep quirk above) kept out of code comments and recorded only in
DESIGN.md — called once from `irrigation_loop()` before its main loop. Tested
on real hardware: didn't work, for the same reason attempt 1 didn't — 2s is
shorter than the 10s already shown to be insufficient, so the burst still
occurred. Removed entirely (function, declaration, and call site).

**Decision: accepted, not fixed.** `irrigation_loop()` uses a plain
`thread_sleep()` with no startup delay of any kind. The early-boot burst is a
known, understood, and accepted cosmetic quirk — not a firmware bug, and not
addressed here. If it ever needs fixing, start from the "Known quirk"
investigation above rather than re-guessing fixed delays (three attempts at
exactly that have already failed or been rejected).

## Remote attestation: split measurer (`attestation`) and signer (`fake_tpm`)

**What:** Added remote attestation so a verifier can learn *what firmware is
running* on the board and be sure the reply is fresh and from this device. The
responsibility is split across two new/finished compartments:

- **`fake_tpm`** — a pure signing oracle. Holds the device Ed25519 key
  (libhydrogen `hydro_sign`) and exposes only `fake_tpm_sign` (over a fixed
  32-byte digest) and `fake_tpm_get_public_key`. It never reads flash, sensors,
  or the network. The private key is created inside the compartment (derived
  from a compiled-in seed via `hydro_sign_keygen_deterministic`) and never
  crosses a compartment boundary.
- **`attestation`** — the measurer. The sole holder of the SPI-flash capability
  and the sole caller of `fake_tpm_sign`. It reads the booted slot's ELF back
  out of flash, hashes it (`hydro_hash`), builds the quote, and asks `fake_tpm`
  to sign the digest. The flash device is the SPI controller at `0x80302000`
  (board.json name `spi0`), which is what the bootloader reaches via
  `spi_ptr(root, 2)` = `SPI_ADDRESS + 2*SPI_RANGE`; note the bootloader's SPI
  *index* 2 is not board.json `spi2` (`0x80304000`).

**The measurement** is the hash of the *whole ELF* of the booted slot, read back
from flash at runtime. The verifier computes the identical hash offline from the
stripped firmware artifact it also feeds to `cheriot-audit`; a match proves the
audited image — and therefore every statically-audited compartment/capability
property — is what actually booted. Whole-ELF (rather than `PT_LOAD`-only) was
chosen so the verifier just hashes the build artifact with no ELF parsing and no
risk of device/verifier parsing divergence; any build change yields a different
hash, the stronger attestation claim.

**Remote-attestation sequence (dual-nonce challenge-response, as implemented):**

A quote is bound to fresh randomness contributed by *both* parties, so neither a
replayed verifier challenge nor a precomputed device reply can pass. Roles:
**verifier** = the `charter` host app; **target** = the device's network side
(`comms` transport + a state machine in `telemetry`); **attestation agent** =
the `attestation` compartment.

All handshake messages — both directions — travel on the single
`plantnode/attestation` topic, secretbox-encrypted under the per-boot Noise-N
session keys (`[8-byte msg_id LE][secretbox]`, the same envelope as telemetry).
Each plaintext is `[1-byte type][payload]`:

- verifier→device: `RaChallenge1`(`0x01`) → `nonce_V[32]`;
  `RaChallenge2`(`0x02`) → `combined[32]`; `RaApproved`(`0x03`) → `combined[32]`
- device→verifier: `RaNonceReply`(`0x01`) → `nonce_V ‖ nonce_D`;
  `RaQuote`(`0x02`) → serialised `AttestationQuote`

1. Verifier picks random `nonce_V`, sends `RaChallenge1{nonce_V}`.
2. Target stores `nonce_V`, generates its own `nonce_D`
   (`crypto_gen_nonce` → `hydro_random_buf`), replies
   `RaNonceReply{nonce_V, nonce_D}`.
3. Verifier checks the echoed `nonce_V`, computes
   `combined = hydro_hash(ctx="PN-COMB1", nonce_V ‖ nonce_D, 32)`, sends
   `RaChallenge2{combined}`.
4. Target recomputes `combined` from its stored `nonce_V`/`nonce_D`
   (`crypto_combine_nonce`); on match it forwards the attestation request to the
   `attestation` compartment as `attestation_quote(combined)`.
5. `attestation` reads the booted slot's ELF from flash, hashes it
   (`hydro_hash`, context `PN-IMAGE`) → `image_hash`, builds
   `digest = hydro_hash(PN-QUOTE, device_id ‖ slot ‖ image_hash ‖ combined)`,
   and calls `fake_tpm_sign(digest)` → Ed25519 signature (context `PN-ATST1`).
6. The target publishes `RaQuote{slot, device_id, image_hash, combined,
   signature}` and waits for the verdict (state `AwaitingApproval`).
7. Verifier recomputes the digest, checks the signature against the device
   public key (derived offline from the same seed), checks the quote nonce equals
   the `combined` it sent, and checks `image_hash` equals the expected build hash.
8. **On success only**, the verifier sends `RaApproved{combined}`. The target
   matches `combined` against the quote it just sent and latches an `attested`
   flag.

**Telemetry is gated on attestation.** The board publishes *no* `plantnode/telemetry`
until that `attested` flag is set: at boot it sends the Noise-N key packet, then runs
its sense loop (reading sensors, evaluating the pump policy) but withholds every
encrypted telemetry payload until the verifier has both received a valid quote and
returned `RaApproved`. So no sensor data ever leaves the device before it has proven
which firmware it is running and the verifier has accepted it. If the quote is
rejected, no approval is sent and telemetry stays withheld.

**Why no explicit verifier signature** (DESIGN earlier sketched "the verifier
signs its nonce"): authenticity of the verifier→device messages already comes for
free from the Noise-N channel. Only the holder of the verifier's static kx
*private* key can derive the session keys, so any message that decrypts on
`plantnode/attestation` is provably from the verifier. A `hydro_kx` key cannot be
reused as a `hydro_sign` key, and adding a separate verifier signing key buys
nothing here — freshness is already provided by the two nonces. Because the
session keys are *directional*, each side only decrypts the other's messages: a
party's own echoes on the shared topic fail the MAC and are dropped, which is how
one topic safely carries both directions.


**Authentic execution / compartment call path:** No single compartment both
measures and signs, and the signing key is reachable from exactly one place.
The call chain is:

  `telemetry` (no capabilities) → `attestation` (sole `spi2` holder; measures)
  → `fake_tpm` (sole key holder; signs) → `telemetry` → `comms` (sole MQTT
  handle holder; publishes).

A compromised application compartment (`comms`, `telemetry`, …) can at most
*ask* for a quote: it cannot read the image (no flash capability), cannot reach
the signer (does not import `fake_tpm_sign`), and cannot extract the key. The
`attestation` compartment is in the TCB for measurement truthfulness, but is
deliberately tiny and non-network-facing (nonce in, quote out).

**How RA is initiated:** attestation is verifier-initiated. Once the verifier has
the device's retained Noise-N key packet (`plantnode/keys/...`) it can derive the
session keys and starts the handshake by sending `RaChallenge1`. The device's
`comms` compartment decrypts inbound `plantnode/attestation` messages in its MQTT
`publishCallback` and buffers the plaintext; `telemetry` drains that buffer in
its poll loop (`comms_take_ra_message`) and runs the state machine, so a reply is
never published from inside the MQTT callback (no `mqtt_run` re-entrancy). The
separate `plantnode/commands` topic is **reserved for a future remote-control
feature** and is unrelated to attestation — its `publishCallback` branch still
decrypts and drops (the `// dispatch TODO`). The previous boot-time one-shot
bare-hash publish has been removed.

**Software-slot flash offsets:** the booted image is read back from the same SPI
flash the bootloader loads from. Slot base offsets mirror the bootloader's
`SoftwareSlots[]` (each slot 10 MiB): index 0 = "slot 1" = `0x0000000`, index 1
= "slot 2" = `0x0A00000`, index 2 = "slot 3" = `0x1400000`. The booted slot is
read from the **GPIO board strap** (pins 13/14/15, first asserted wins, none =
slot 0), exactly as the bootloader's `read_selected_software_slot` does, so the
measurement always covers the slot the device actually booted from rather than a
hard-coded guess. NB: these SPI offsets are distinct from the UF2 packer's `-b`
base addresses (`0x00000000/0x10000000/0x20000000`), which are flashing-tool
addresses, not raw SPI offsets.

The flash SPI controller is at `0x80302000` — board.json `spi0`, which is what
the bootloader reaches via `spi_ptr(root, 2)` (= `SPI_ADDRESS + 2*SPI_RANGE`).
The bootloader's SPI *index* 2 is not board.json `spi2` (`0x80304000`); using the
latter reads the wrong device and returns no flash data.

**CHERIoT property:** The trust boundary is *statically verifiable from the
firmware image* and was checked against the build's `cheriot-audit` report:
`fake_tpm_sign` is imported by `attestation` and no one else, and the SPI-flash
MMIO capability (`0x80302000`, board.json `spi0`) is held by `attestation` and
no one else. (`attestation` additionally holds a read-only `gpio_board`
capability, `0x80000000`, solely to read the boot-slot strap.) The
signing key's confidentiality rests on compartment isolation, not ambient trust;
`AttestationQuote` is pointer-free plain data, safe for cross-compartment
transfer. `fake_tpm_sign` accepts only a fixed-size digest so that, even if
`attestation` were compromised, the key cannot be used as a general signing
oracle — every signature is shaped like a quote.

**Assumption / limit:** `fake_tpm` models a hardware TPM under the explicit
assumption that secrets can be stored securely; here the seed is compiled into
the image and protected only by compartment isolation. This defends against a
compromised application compartment but NOT against a maliciously rebuilt image
(no measured boot from an immutable root yet) or a physical attacker extracting
the seed (no hardware key store / OTP / PUF). A real deployment would provision a
hardware-unique key never present in the image.

**Rejected alternatives:** (1) Let `attestation` both measure and sign — rejected
to keep the key in a single-purpose compartment with the narrowest possible API.
(2) Have `fake_tpm` measure as well — rejected so the key holder stays a pure
oracle that touches no device. (3) Sign a build-time-baked image hash — rejected
as self-referential (proves nothing about what actually booted).
