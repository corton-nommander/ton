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
