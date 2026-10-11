# Pending UUID recovery experiment

Draft, unmerged. Worker persists a request UUID, URL and filename before sending a non-idempotent Native Messaging add. On cold start it only calls read-only UUID lookup; confirmed handoffs are journaled and pending entries cleared. Unknown results are never replayed.

The Windows test additionally injects 4200 ms GUI history-write latency and calls CDP ServiceWorker.stopWorker during that window. It requires actual stopped/running events, exactly one task and history record, and decoded MP4 equality.

Limits: no durable GUI UUID mapping after a desktop restart; pending UUID metadata retention expires at 24 hours; automatic interception still does not cancel the original browser download unless identity is verified. The in-flight termination experiment may be rejected by Chrome if Native Messaging keeps the worker alive; a failed CI is diagnostic, not evidence of product correctness. No merge or release before independent CI acceptance and local Windows worktree audit.
