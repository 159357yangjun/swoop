# Read-only add reconciliation protocol (Draft PR #29)

1. Browser creates a UUIDv4 for each **single** `add_download`, not for batch requests.
2. Native Host forwards `requestId` through the existing GUI IPC `add` command. GUI validates the UUID, records it against the returned task ID before sending the original ACK, and refuses to create another task for the same token.
3. The mapping lives **only** for the current GUI process (256 most recent UUIDs). No URLs, paths or request bodies are stored in this mapping. A lost request/status after a crash or eviction remains **unknown**, not "not created".
4. On Native Host unavailability or an uncertain ACK, extension calls `get_request_status`, which forwards the **read-only** `request_status` IPC query; at most three lookups, never another `add_download`.
5. A verified task ID/session ID is journaled before returning successful handoff. A failed journal write preserves the browser's original download. No status query claims the file was downloaded.
6. The opt-in 4200 ms slow SQLite history-write fixture holds GUI's add ACK beyond Native Host's unchanged 3000 ms budget. It must recover a real identity, find exactly one GUI task and one database row, and verify the final MP4 decoded-frame SHA256.

**Threat and scope boundaries:** The UUID is an opaque bearer token for read-only task identity in the local Native Host channel. Its entropy comes from `crypto.randomUUID()`; malformed tokens are rejected. This is not a generalized durable idempotency store. The extension does not currently persist a preflight pending-ID journal across MV3 worker restarts, and batch adds still use legacy aggregate counts. These require independent design and review rather than silently expanding this PR.

No master/main or local worktree mutations. Do not merge/release until Windows CI and pre-merge local worktree audit complete.
