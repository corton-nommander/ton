# Remote ten-minute drain failure and client correction

The user-reported run `20260907T160249Z-298611/01-10-connections` completed its 600-second measurement, then remained at 32 unresolved logical transfers throughout its 180-second drain. Docker reported exit 2, `OOMKilled=false`, no restart, and no watchdog expiry. The runner correctly stopped before reusing those accounts for another arm.

The reported final progress value was **63,831.51 canonical logical TPS** over a planned 599-second complete-block-time window. This is an observation from an **invalid, incompletely drained run**, not an accepted capacity result. The generator image was `sha256:e016826b0bb5c4e11d01dd7b31cf5aae3843c9066720f2ad5affecfdcc59b731`, with four CPU equivalents, 8 GiB, six workers and six signers. The complete final generator record and final stderr have been requested; the large supplied record has `final:false` and describes an earlier measurement sample.

## Confirmed retry bug

The validator emits timeout code 652 with `external message admission deadline expired` when the admission request deadline passes. `NativeLoadWorker::handle_task_error` correctly counted that as a timeout, but independently matched the word `expired` and called `sign_task(..., true)`. That replaced a still-valid signed parent rather than retrying its exact BOC and hash.

The supplied nonfinal sample has `timeouts=39`, `submission_errors_by_reason.expired=39`, and `resigned=39`. These counters support execution of that erroneous branch. Admission may already own the original hash when its callback times out. A replacement can encounter that reservation, retry until the source's retry horizon expires, and leave the source disabled for drain repair. This is a plausible explanation for two stranded sixteen-transfer intervals, but the exact 32-transfer attribution requires the requested final counters and quarantine messages.

TON fix commit **`a1e4c988`**:

- Admission/query deadline expiry, cancellation, and transport/parse errors preserve the original signed authorization.
- Explicit native payload-expiry responses retain renewal; the existing local `valid_until` check also remains.
- Retention or suffix eviction does not by itself establish that this parent's authorization expired.
- A timed-out drain logs a bounded source sample after the final proof poll, including missing logical counts, source indices, nonce positions and task state. It exposes no private keys or BOCs.

The completion requirements remain unchanged: proof-consistent measured and total cohorts, zero unresolved backlog, a final contiguous follower catch-up, and a clean process exit. Increasing the drain timeout or ignoring the last 32 transfers would conceal the failure rather than correct it.

## Resource and progress defaults

The user confirmed that B has 48 CPUs and 256 GB RAM. The standalone runner now defaults to the visible `server48` profile: **32 CPU equivalents, 32 GiB, eight workers and 24 signers**, using a 33-thread native scheduler. It upgrades older imported six-worker presets without modifying the original environment. `--profile preset` plus explicit six-worker/signer overrides reproduces the old resource control. The default connection sweep remains 10/50/100.

Initial/max admission windows stay at 32,768/65,536, with in-flight and canonical backlog budgets unchanged. In the supplied sample, the actual congestion window was approximately 8,174; no acknowledgments were clipped by the maximum, and no query-credit stalls occurred. That evidence does not justify automatically widening these limits while correcting retries and CPU/signing capacity.

The old live field `canonical_chain_measure_avg_tps` divided observed transfers by the **entire planned** 599-second window from its beginning. Its rising values were not a live throughput ramp. Host progress now calls it `canonical_chain_measure_planned_window_avg_tps` and includes its denominator; raw generator fields and final measurement calculations remain unchanged. Failed final summaries also retain compact retry, expiry, repair and backlog counters to avoid dumping the full log merely to diagnose a tail.

## Validation and deployment

The native generator and policy-test targets built successfully, including the bounded drain diagnostics. **61 native policy tests passed**, including regressions for real admission deadline messages, genuine payload expiry, error origin, and retention/suffix eviction. **46 remote integration tests passed**, followed by the focused final-diagnostics regression. These use local builds or simulated Docker; they do not constitute a new remote TPS run.

The native fix requires a newly published TON image and an updated exported generator image. A host-script update alone supplies only the resource/progress changes. Follow the [image upgrade procedure](native-remote-client-guide.md#upgrade-the-image-for-the-admission-timeout-drain-fix), keep the existing chain and old failure artifacts, then obtain a valid ten-minute single arm before another sweep. No TPS gain from the fix or new resource defaults has yet been measured.
