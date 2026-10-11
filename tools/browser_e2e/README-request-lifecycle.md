# Request UUID lifecycle and MV3 restart boundary tests

Stacked Draft PR on top of PR #29. No product code changes.

## Real packaged Windows Native Host / GUI protocol gate
`node tools/browser_e2e/browser_hls_popup_e2e.cjs --dist installer/dist --request-lifecycle`

- Submit one fresh UUIDv4 with an intentionally broken local HLS playlist; require a real task ID and GUI session ID.
- Read-only `get_request_status` must return the same identity.
- Deliberately send the **same UUID** a second time, solely to test GUI idempotency: it must return the first task identity without adding another task. This is a test assertion, **not** a browser automatic retry.
- An invalid UUID in add and a never-seen UUID in lookup must fail without creating tasks.
- The packaged GUI is terminated and restarted with the same isolated data directory; the original request UUID must no longer resolve in the new GUI session. The read-only query must not change task state.

## Node/VM simulated MV3 restart
- Reinitialize the extension background JavaScript in a fresh VM but reuse the same `chrome.storage.local` object. A verified handoff survives as a journal entry, and a status refresh is read-only.
- An unresolved add creates no false journal entry after VM restart, and no add replay occurs.
- This is a **simulated JavaScript/worker restart**, not an actual Chrome MV3 service-worker kill during an in-flight add. Durable pending UUID recovery remains a separate future feature.

## Constraints
Existing Chrome/Edge production E2E, real 4200ms history-delay recovery, strict 3000ms host reply, 18s Popup assertion, HLS/AES, single-instance, and packaging tests remain untouched. No merges or releases; no modification to local unpublished Windows commits. Prior CI red attempts remain part of the audit trail.
