# MV3 in-flight Native add: durable pending UUID and read-only recovery

This feature is stacked on [PR #30](https://github.com/159357yangjun/swoop/pull/30). It does **not** modify the Native Host's 3000ms reply deadline or add retries. It must remain Draft until real Windows CI and local worktree reconciliation succeed.

## Protocol invariants

1. On every **single** `add_download`, generate UUIDv4 and first persist `{requestId, filename, at}` to `chrome.storage.local.pendingAddRequests`. If that storage write fails, **do not send** to the Native Host. The pending record contains no URL, cookies, headers, save path or IPC payload.
2. After storage commits, send the original `add_download` **once**. GUI maps UUID → real task ID. An uncertain ACK never triggers a second add.
3. If the worker remains alive, reconcile the same UUID using read-only `get_request_status` (max 3 queries). Only a verified `taskId + sessionId` may be journaled; remove pending UUID after successful journal persistence.
4. If Chrome kills the MV3 Worker before the Native callback, the pending UUID remains in `chrome.storage.local`. On a fresh worker activation or trusted Popup's `getRecentHandoffs`, query the original request ID read-only, journal a verified match and remove the pending marker. Unknown, evicted or restarted-GUI tokens **remain unresolved**. No resubmission; existing browser original is preserved.
5. Pending records are bounded to **16** (oldest evicted), live in extension storage, and are not a permanent idempotency ledger. This intentionally does **not** guarantee recovery after eviction, browser storage deletion or GUI restart.

## Real Windows acceptance

`node tools/browser_e2e/browser_hls_popup_e2e.cjs --dist installer/dist --mv3-midflight`

Uses packaged GUI and Native Host, Edge MV3 extension, actual native messaging registration and local FFmpeg-created HLS. CI opts in to the isolated 4200ms SQLite history-write delay.

- Press real Popup Download once; require that the pending UUID is committed.
- Observe the GUI enter the synchronous test-only history write delay before using CDP `ServiceWorker.stopWorker({versionId})`.
- Require an actual `stopped` event, then permit the GUI to finish **without browser retry**.
- Reopen the extension Popup to wake the MV3 worker. Require a real `running` restart event, a recovered identity in persistent handoff history, zero pending records, exactly one GUI task and SQLite record, single fault injection, independently decoded MP4 and source SHA256 equality.
- Existing Edge/Chrome, HLS/AES-128, failed HLS, request-ID idempotency, GUI restart and engine safety gates remain unchanged.

## Limitations

- For a real browser download intercepted in flight, the fresh worker **does not cancel** the original browser download on recovery; a possible duplicate physical download is preferable to silent data loss.
- The queued batch-add path remains legacy and is not covered by this per-request UUID mechanism.
- A browser storage write returning success immediately before a forced browser-wide shutdown still depends on the browser's storage durability; this test specifically kills the Service Worker after observing the committed value.
- Historical IPC delay and intermittent engine exit issues remain open; do not claim this test proves their original causes.

## First CI red and fixture correction

[CI #38140801760](https://github.com/159357yangjun/swoop/actions/runs/38140801760) failed in the **new** mid-flight scenario before sending any Native Host request. The Popup resource row was absent because the new scenario had not activated the media source tab before `loadResources()`. The existing E2E correctly does `page.bringToFront()`. The correction adds that operation and waits for the source page's injected resource token element. The full CI red is preserved; worker stop/recovery is **not yet proven** by that run.

[CI #38141722375](https://github.com/159357yangjun/swoop/actions/runs/38141722375) was a second full Windows red **before** the MV3 mid-flight test, in the pre-existing SQLite slow-ACK test's final one-shot status read. It parsed zero GUI task rows after an earlier successful one-row query and a real HLS completion. This is an ambiguous missing/transient *read-only* status response, **not** proof of a duplicate task. The fixture now requires the exact same single original task ID through bounded read-only polling after completion; all add and GUI production semantics are unchanged. A persistent missing task will still fail, and both red run logs are retained.
