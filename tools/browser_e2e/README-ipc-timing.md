# IDM Next IPC add acknowledgement diagnostics

## Repro and hypotheses

[Issue #25](https://github.com/159357yangjun/swoop/issues/25) records the exact frozen PR #24 HEAD where
CI attempt 1 passed and attempt 2 failed even though the GUI logged a created HLS task.

- Attempt 2 (same SHA) GUI: HLS task `#2000000` was created and the GUI logged
  `IPC add: callback returned taskId=2000000`; aggregate handler processing duration ~3402ms.
- Browser E2E: `.row.sent` never became visible within the original 18-second gate.
- Native Host `forwardToGui` currently waits 3000ms for a reply. That is an
  **investigation hypothesis**, not yet proven to be the cause of the Popup failure.
- Earlier startup-race CI [#38114460537, attempt 1](https://github.com/159357yangjun/swoop/actions/runs/38114460537/attempts/1)
  independently recorded a client request sent and no IPC reply within 3006ms.

## Instrumentation only

- `IDM_IPC_TIMING_TRACE` enables cumulative monotonic markers inside the HLS
  branch of `TaskController::internalAddTask`: entered, model_added,
  pending_registered, status_synced, history_recorded, return.
- The packaged Windows browser E2E enables that flag for its own GUI process.
- On a Popup `.row.sent` timeout, the test logs a read-only snapshot of row
  counts, selected/sent classes, button disabled state, toast text, and whether
  `recentHandoffs` contains a session-bound identity. It then **rethrows the
  original Playwright failure**.
- No add retry, business-logic change, assertion relaxation, or timeout increase.
  Timing markers exclude request payload, URL, filename, save path, and tokens.

## Disposition

This is a temporary Draft diagnostic stacked atop PR #24. Reproduce and
classify the issue before proposing any functional fix. Archive both red
and green CI logs. Do not merge to `master` / `main`, release, or touch
unpublished Windows local commits.

## Instrumentation follow-up

Earlier diagnostic run [#38121196767 attempt 2](https://github.com/159357yangjun/swoop/actions/runs/38121196767/attempts/2) passed but did not print GUI phase markers because the harness logged GUI stderr only on failure. The current harness now emits **only matched, sanitized** `HLS_ADD_TIMING` phase markers on both green and red outcomes, after the original pass/fail assertions. Those markers reveal where callback latency accrues without printing GUI request URLs or modifying the 3000 ms Native Host deadline. This change itself has not yet been independently Windows-verified.
