# Native collator delivery diagnostics

The `external_delivery_*` fields in a candidate's wall-work statistics explain
where the native collator waits for external work. They are additive telemetry;
they do not change admission, queue ordering, refill policy or deadlines.

## Timed waits

For each stage (`first_work`, `fragment_refill`, `post_commit_idle`), samples are
partitioned by producer state at the actual await entry:

- `producer_pending`: the callback's producer epoch has not been observed
  complete. This can include selection, blocked publication or an unobserved
  completion marker; it does **not** prove admissible work is ready.
- `no_producer`: no unfinished producer epoch was observed at entry. New ingress
  can still arrive during the wait.

For example, `external_delivery_fragment_refill_producer_pending_s` sums the
elapsed wall seconds of fragment-refill awaits entered with a pending producer.
Every stage/state combination reports `_s`, `_calls`, `_work_wakes`,
`_marker_wakes`, `_timeouts` and `_errors`. The four outcome counts sum to
`_calls`; a batch containing both work and a completion marker is a work wake.

The timer starts immediately before the bounded queue await and ends after it
returns. It excludes the preceding nonblocking probe and refill-policy
decision. It includes coroutine/actor scheduling and queue-delivery time, so
it is not a measurement of CPU service time. A producer state sampled at entry
is not a measurement of how that state changes during the wait.

For **first-work waits only**, the `no_producer` bucket also has two disjoint
subsets with the same six suffixes:

- `external_delivery_first_work_no_producer_uninstalled_*`: the callback's
  producer epoch was zero. The pool had not begun installing this callback's
  first producer epoch at the sampled instant. This includes a callback still
  in the Collator → Manager → Pool dispatch path or the beginning of pool
  installation before its epoch opens.
- `external_delivery_first_work_no_producer_completed_*`: the epoch was
  nonzero and its completion had already been observed. The callback was
  installed, but no unfinished producer epoch was observed. New committed
  ingress, canonical reconciliation or reactivation may reopen one later.

The two subsets sum to the existing first-work/no-producer bucket for every
field. They do not change its meaning or add time to any legacy total. Epoch
and pending status come from one sample using the existing two acquire loads;
no extra queue lock or per-message timing is introduced. `uninstalled` measures
the **entire ensuing await** entered before installation, not isolated mailbox
latency: selection, publication and later eligible ingress can happen during
that wait. Likewise `completed` does not prove global admission starvation,
because eligibility depends on this callback's exact branch and nonce floors.

For an `uninstalled` first-work entry, four additional fields isolate the
actual await's overlap with the **first epoch opening**:

- `external_delivery_first_work_no_producer_uninstalled_pre_epoch_s`: await
  time before that first epoch opens, bounded to the measured interval.
- `external_delivery_first_work_no_producer_uninstalled_post_epoch_s`: the
  remainder of that same await after the first epoch opens.
- `external_delivery_first_work_no_producer_uninstalled_split_calls`: number
  of split awaits; this matches the existing `uninstalled_calls` count.
- `external_delivery_first_work_no_producer_uninstalled_epoch_unobserved`:
  awaits whose first epoch was still unobserved at return, including timeout
  or cancellation before installation. Their entire duration is pre-epoch.

The two durations sum to `uninstalled_s`. The immutable monotonic timestamp is
written once by the serialized pool producer, before release-publishing its
first epoch. Readers acquire a nonzero epoch before reading the timestamp.
Reopening later epochs never resets it. This adds one clock read per callback
lifetime, no per-message clock read or queue lock.

The split point is `clamp(first_epoch_time, await_start, await_end)`; if the
epoch is unobserved it is `await_end`. This handles installation between the
entry sample and timer start, or between await return and timestamp observation,
without negative or out-of-interval durations. The timestamp marks the existing
first-epoch opening near the **start** of `install_collator_queue`, after initial
sorting/wrapping and before synchronous prefill. Thus pre-epoch time includes
dispatch and early installation work; post-epoch time can include prefill,
pump scheduling and eligible ingress. It is not an installation-complete
timestamp, and elapsed time before the first-work await is not measured here.

## Already-published work

Every nonblocking work-driven native probe records `_s`, `_calls` and
`_work_wakes` under one of:

- `external_delivery_probe_published`: at probe entry, a completed native push
  had published more messages than this callback had consumed. This is a
  conservative guarantee of work already published before the probe.
- `external_delivery_probe_unconfirmed`: that lower bound was zero. A bounded
  push can publish a prefix before its producer resumes to report completion,
  so this label does **not** mean the queue was empty.

Publication accounting excludes completion markers and counts physical native
messages. It reads the existing callback-local accounting under its mutex;
reservations alone never count as completed publication. The probe timer
covers the nonblocking queue call through resumed collator delivery, not the
age of the oldest queued message. Work arriving after probe entry can produce
a successful `unconfirmed` probe.

Large `published` probe latency points to queue handoff or actor scheduling
while work is known to be available. Large pending-producer waits with few
work wakes instead require producer-side selection/publication evidence.
Long `no_producer` waits can be deliberate packing waits, so reducing them
without checking block size and throughput is not justified.

## Accounting and comparisons

These wall timers overlap existing `external_wait_*` / `wait_externals`
timers. Never add them to the old total. The old kind timer includes its probe
and policy work, and remains unchanged for historical comparisons. Delivery
fields are omitted from CPU-work statistics.

Use accepted-block samples from the same fully contained measurement window
as the canonical TPS result. Compute mean latency as summed `_s` divided by
summed `_calls`, rather than averaging per-block means. Preserve marker-only,
timeout and error outcomes instead of silently excluding them. Pending status
and publication lower bounds refer only to the exact callback's producer;
they do not combine competing candidate branches.
