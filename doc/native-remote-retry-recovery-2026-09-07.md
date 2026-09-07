# Remote retry recovery and connection scaling

The follow-up remote run used the admission-timeout correction published at
`3288fa8d`. Its final 50-connection result has `resigned=0` and `expired=0`, so
the earlier timeout/re-signing defect is no longer present. Nevertheless, one
source exhausted its retry horizon and left eight signed parents, or 128
logical transfers, unresolved. The follower completed its final catch-up with
zero proof/hash errors. Docker reported exit 2, no OOM, no restart and no
watchdog expiry. The runner stopped before reusing these accounts for 100
connections.

The reported 599-second canonical average was **50,752.91 logical TPS**. This
remains an invalid benchmark because its offered cohort did not fully drain.
The final diagnostics report 1,466,011 not-ready retry schedules, 799 timeouts,
435 canonical-state-lag retries, and one exhausted source. Those are retry
events, not distinct lost transfers. An earlier 10-connection arm also showed
quarantined sources with 16, 32 and 128 unresolved transfers.

## What the resource evidence establishes

The supplied A screenshot shows work spread across its 48 logical CPUs, mostly
at approximately 20–50% utilization, with a one-minute load average of 11.44.
That is not evidence that all 48 CPUs are saturated. B's screenshot shows much
less CPU activity and a load average of 1.33. These snapshots are not synchronized
resource measurements and cannot identify a particular serialized server stage.

The public Session Stats `BLOCK_APPLIED_transactions` API reports nine complete
minute buckets from 18:30:00 through 18:39:00 UTC averaging **50,090.67 TPS**, with
a minimum of **45,917.33** and maximum of **58,651.73**. Masterchain activity
continues through the later idle interval. These minute windows differ from the
generator's exact 599-second window. The saved
[failure and dashboard observations](benchmarks/results/native-remote-retry-followup-20260907.json)
preserve that distinction.

Low bandwidth is expected for this workload's compact sixteen-transfer signed
parents. Low client CPU usage can also mean that the client is waiting for
admission or canonical progress. Increasing its CPU/memory allowance does not
make those dependencies complete faster. The aim is more useful offered load
and completed canonical transfers; allocating unused RAM or adding duplicate
traffic is not a TPS improvement.

## Client correction

Recoverable signed-run snapshot/revision races need a bounded short retry
delay. With the exported 100 ms base and eight-retry cycle, the previous
generic exponential path could request a 25.6-second delay before resetting
its cycle; the generic exponent permits up to 102.4 seconds with other
settings, subject to the horizon clamp. More seriously, retry-horizon quarantine marked every pending parent resolved,
cleared their task map and disabled the source. The missing nonce interval
remained in the offered cohort, but the client could no longer submit its
original parents to settle that interval after the server recovered.

The correction separates stopping new source issuance from reconciling work
already offered. Exact parent bytes and proof ownership must remain available
until the existing drain deadline. A recovered cohort can complete, while a
recorded retry exhaustion still disqualifies capacity claims. Signature/hash
conflicts, incomplete proof cohorts, fatal follower failures and a real drain
timeout continue to stop reuse of the affected accounts.

The connection parser and standalone runner support **1–1024 connections**.
The number must cover the configured workers and fit the configured logical
credit budgets. Extra connections share the same global budget; they do not
multiply it. A new generator image is required for values above the old 256
limit, and the runner checks that capability before starting a sweep.

The updated dedicated-server profile uses **40 CPU equivalents, 48 GiB, ten
workers and 32 signers**, with a 43-thread scheduler. Ten workers distribute
10/50/100/300/500 connections evenly. Global initial/max congestion windows
remain 32,768/65,536; larger windows remain explicit experimental overrides.
Resource ceilings provide headroom rather than a promise to consume every CPU
or fill all 256 GB of host memory.

## Deployment and measurement

After the new TON image is published, update A and export a matching generator
image using the [existing image upgrade procedure](native-remote-client-guide.md#upgrade-the-image-for-the-admission-timeout-drain-fix).
Keep the chain, test accounts and failed-run artifacts. Import the new bundle
on B into a new client directory. A shell-script update alone cannot repair
the old binary.

Start with a ten-minute 10-connection run, then compare a fixed-image sweep:

```sh
bash run-remote-load.sh --connections 10 --duration 600
# After complete proof reconciliation and a zero-backlog drain:
bash run-remote-load.sh --connections 10 50 100 300 500 --duration 600
```

A run that recovered from retry exhaustion is an observation rather than a
capacity result. Only a clean process exit, complete proof-checked cohorts,
zero unresolved backlog and final follower catch-up permit the same accounts
to be reused in the next setup. A truly unresolved run still needs an isolated
account set or a repaired/drained chain before more load uses those nonces.

To attempt every setup within a new sweep even if one partition cannot drain,
the runner supports an explicit alternative:

```sh
bash run-remote-load.sh --source-policy isolated \
  --connections 100 300 500 --duration 600
```

With 24,576 exported sources, these three setups receive 8,192 disjoint,
lane-balanced sources each. Failed setups remain invalid, the sweep exits
nonzero if any failed, and later setups become observations while earlier
cohorts remain unresolved. Earlier traffic can still enter their chain-wide
TPS window. This changes account cardinality and is not directly comparable
with a full 24,576-source sweep. Isolation applies within that new sweep; it
does not make source ranges from an earlier failed sweep safe to reuse.

## Local validation and A/B/B/A screen

The generator and policy-test targets compiled, and **64 policy tests passed**.
CLI checks accepted 300/500/1024 connections and rejected 0/1025 and insufficient
inflight credits before startup. The companion runner's **55 integration tests
passed**, including old-image capability rejection, complete recovery followed
by another setup, and disjoint-source continuation after an incomplete setup.
Eight durable tests against the production worker also passed: real
single/batch timeout callbacks, bounded race retries, source-head timing,
scalar repair and conflicts, retry wakeup through `pump()`, complete recovery
of 128 logical transfers, and rejection at the unchanged drain deadline.

```sh
python3 lite-client/tests/native-load-worker-regression.py --build-dir build
```

Four local screens used prebuilt baseline/candidate images, unchanged runtime
libraries and the same continuously running validator. No image build or
validator restart occurred during the comparison. Both clients used ten
workers, 32 signers, four CPU equivalents, 4,096 sources, 15 seconds warm-up
and 60 seconds measured load. The older local validator retained its 18-CPU
configuration; this is not the remote 48-CPU A/B environment.

| Order | Client | Offered logical TPS | Canonical logical TPS |
| --- | --- | ---: | ---: |
| A1 | Baseline | 66,073.87 | 67,021.56 |
| B1 | Candidate | 69,055.20 | 69,078.51 |
| B2 | Candidate | 66,921.07 | 67,746.17 |
| A2 | Baseline | 68,392.53 | 67,441.63 |

All four runs exited cleanly with complete proof-checked cohorts, zero final
backlog and no hash/follower errors. Candidate canonical throughput averaged
68,412.34 TPS versus 67,231.59 for baseline: **+1.76% descriptively**. The two
comparisons were +3.07% and +0.45%; they do not establish a repeatable 2% gain.
Three offered averages were below their canonical averages, so these are
observations rather than qualified capacity measurements. Candidate not-ready
diagnostics classified all 209,800 retry events as explicit snapshot/revision
races, with zero generic not-ready errors.

Three Docker CPU snapshots inside each measurement window put the generator
near one CPU equivalent, while genesis used approximately 10–11.3. These
sparse samples reproduce the large A/B utilization difference at roughly 67k
TPS. They support retaining client CPU headroom; they do not identify the
specific server stage limiting throughput.

The [saved A/B/B/A report](benchmarks/results/native-remote-retry-local-abba-20260907.json)
contains image identities, resource snapshots, raw-artifact hashes and the
comparison limits. No new remote TPS gain is established by these changes.
