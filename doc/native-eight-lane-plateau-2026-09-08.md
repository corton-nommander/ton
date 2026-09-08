# Eight-lane throughput plateau — 8 September 2026

The user reports that switching to eight lanes increased block production to approximately 40–50 blocks/s, while **Canonical native transfers per second remained approximately 50,000**. The earlier four-lane reference was also approximately 50k. This is no reported TPS gain; it is not a validated paired result or proof that eight lanes can never scale.

The run's exact timestamps, image IDs, effective client settings and final proof/drain record were not supplied. The public dashboard at `http://2.59.170.242:18000/` was unreachable during this review. No remote configuration, database, image or workload was changed. MyLocalTonDocker `b5f5e76` is the support implementation, not a verified identity for the running images.

If both rates cover the same canonical basechain blocks and interval, 50,000 transfers divided by 40–50 blocks implies approximately **1,000–1,250 transfers per block**. If the block chart includes masterchain blocks, subtract those first. Higher block frequency with unchanged aggregate work indicates smaller average payloads. It does not establish whether unique offered load, admission, processing or observation is limiting throughput.

## Concrete implementation leads

1. **Shared admission coordination.** One `ExtMessagePool` actor serves the validator ([manager.cpp](../validator/manager.cpp)). State reads and signature workers can run asynchronously, but admission coordination remains shared across lanes. `pin_native_admission_snapshot()` extracts configuration from the exact masterchain state; the batch path calls it before admission work and again after verification. If the masterchain block changed, surviving signed-run admissions receive a snapshot-change retry ([ext-message-pool.cpp](../validator/impl/ext-message-pool.cpp)). Earlier runs showed many `not_ready` responses, but the new run's `not_ready_by_reason` split is needed before attributing its plateau to this path.
2. **More fixed work per transfer.** More, smaller blocks require more block setup, validation, state/proof finalization and storage operations. Signed-run signature validation creates and joins helper threads for each sufficiently large invocation ([transaction.cpp](../crypto/block/transaction.cpp)). The physical preset has 40 scheduler threads, a 44-CPU quota and up to eight native helper workers per invocation. Profile thread creation, scheduler delay and useful CPU work before tuning parallelism or introducing a reusable bounded executor.
3. **Ingress gaps and block packing.** Collation uses short refill and post-commit waits; it need not hold a successful block until the outer candidate timeout. Correlate pending work and queue age with these waits before changing packing policy. The configured 18,432 logical delivery budget is not evidence that candidates fill it, and is not the protocol batch maximum.

Work-driven shard production skips normal target-rate sleeps ([block-producer.cpp](../validator/consensus/block-producer.cpp)). `TON_SIMPLEX_MAX_TPS_CANDIDATE_TIMEOUT_MS=8000` and the finalize reserve allocate failure/work deadlines, rather than imposing those delays between successful blocks. The plateau gives no evidence for reducing them.

## Revised next steps

Keep four lanes as the incumbent comparison; retain the existing eight-lane chain for diagnosis. Do not change its depth variables to 2 or reset it. Do not promote eight lanes as a TPS improvement or expand to sixteen yet.

First obtain one complete 600-second result with unique offered, admitted and canonical TPS; actual congestion window and cap counters; RTT; `not_ready_by_reason`; per-lane transfer/block counts; follower lag; and proof/drain status. Record CPU profiles and admission/producer queue timing on A during the same interval. Dashboard averages alone cannot determine whether the client supplies enough useful work.

Prioritize admission stage timings and caching immutable configuration by the exact masterchain state. If snapshot retries dominate, evaluate one bounded refresh and revalidation using the original RPC deadline, preserving activation, signature domain, lane locality, current nonce/balance and reservation checks. Never remove the snapshot guard simply to improve the score. If profiling instead shows fixed block or helper-thread costs dominate, test a bounded reusable executor and packing policy separately.

Change client windows only when the actual window/cap counters show a limit; extra connections divide the same global budget. Compare prebuilt images with repeated ten-minute controls and unchanged workload accounting. No new TPS gain is claimed by this analysis.
