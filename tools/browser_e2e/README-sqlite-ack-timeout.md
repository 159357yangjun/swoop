# Slow SQLite history write → unknown Native Host ACK

**Status:** isolated fault-injection test only, stacked on PR #26. Not a production fix. Do not merge until the actual cause and the Windows local worktree are reviewed.

## Mechanism

In `HistoryStore::recordAdded`, after `TaskController` has registered the HLS task with the GUI model, the test-only `IDM_E2E_HISTORY_WRITE_DELAY_MS` setting pauses execution immediately **before** the SQLite INSERT. The flag is ignored unless `IDM_IPC_TIMING_TRACE` is also set, and values outside 3500–5000 ms are rejected. The CI fault test uses **4200 ms**. The production/default code path and existing 3000 ms Native Host reply deadline are unchanged.

This reproduces the effect of a blocked SQLite write; it does **not** establish that real-world SQLite locking caused the historical 3402 ms stall. No artificial lock, transaction modification or retry is introduced into normal execution.

## Real Windows E2E

The CI runs `node tools/browser_e2e/browser_hls_popup_e2e.cjs --dist installer/dist --ack-delay` as a separate test step, with a fresh GUI process, isolated `IDM_DATA_DIR` and SQLite database, real Edge extension Popup, actual Native Messaging registry, GUI IPC, real local HLS and FFmpeg.

The test must prove all of the following without a second `add_download`:

1. The Popup displays an **error** for the unknown/unconfirmed ACK and does **not** mark the row as sent.
2. On reopening the Popup, no successful tracked handoff appears in `chrome.storage.local.recentHandoffs`.
3. A fresh Native Host `get_status` read-only query sees **exactly one** GUI task for the requested fixture.
4. A read-only SQLite query sees **exactly one** matching history record.
5. The HLS final MP4 is published once, FFmpeg can decode it fully, and the video-frame SHA-256 matches the source HLS.
6. Another read-only GUI/SQLite check still sees one task/record after completion.

The test makes no promise that unknown outcomes can be automatically reconciled for end users; no request-ID/idempotency feature is implemented. It verifies the **fail-closed and nonduplication** behavior under a deterministic ACK timeout.

## Non-negotiable constraints

- No lower assertions, no longer production timeouts.
- No automatic retry of a potentially committed `add_download`.
- Existing browser E2E remains a separate required green step.
- Keep original red logs for [Issue #25](https://github.com/159357yangjun/swoop/issues/25) and [Issue #27](https://github.com/159357yangjun/swoop/issues/27).
- `master`, `main`, frozen PR #23, PR #24/26 and unpublished local Windows code stay untouched.
