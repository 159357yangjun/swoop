# Real browser Popup → HLS acceptance

This gate runs on the packaged Windows distribution, not against mock GUI handlers.

## Entry point

`node tools/browser_e2e/browser_hls_popup_e2e.cjs --dist installer/dist`

The CI job first installs Playwright/Chromium dependencies, builds Qt, packages the GUI and Native Host, and verifies FFmpeg availability. This script registers the packaged native host under temporary per-user Chrome and Edge NativeMessagingHosts keys, refuses to overwrite an existing registration, starts the packaged GUI with `IDM_DATA_DIR` pointing to an isolated temporary directory, and removes its registration on exit.

It generates local HLS video segments using FFmpeg, opens Edge and Chrome for Testing with the actual MV3 extension and real DOM/media discovery, loads the production `popup.html` into an extension-owned tab, selects the discovered HLS resource and clicks the Popup Download button. **The real toolbar icon opening gesture itself is not automated.** The native host, GUI IPC and download engine are not mocked.

## Strict positive and negative gates

For successful handoff, the extension must persist a positive numeric task ID and a matching session ID. It must report the same identity while being queried read-only until the GUI reaches a completed terminal state. The script checks the user-visible Popup status, final MP4 stream layout/duration, independent decoding and SHA-256 equality of raw decoded video frames against the original HLS playlist.

A separate Edge negative fixture references a missing segment. That case must reach a real terminal failure, propagate a reason and leave no final MP4.

The script intentionally submits each resource only once. An uncertain acknowledgement must **never** trigger a second add request.

## Known initial question

The Popup's visible Download button currently routes one selected resource through `batchDownload`, which reports aggregate counts; the separately implemented `downloadWithOptions` route persists `taskId + sessionId`. The new CI gate is expected to verify whether the user-facing single-selection flow really satisfies task identity tracking. Keep the first red log if it fails; fix the cause rather than weakening the gate.

This remains a Draft stacked on PR #23. It is **not** release acceptance or proof of integration with unpublished Windows local changes.

## Preserved initial red and fixture correction

The first real Windows attempt [#38111104899](https://github.com/159357yangjun/swoop/actions/runs/38111104899) failed at `Edge media discovery timed out`. The initial fixture rendered `<video src="...m3u8">` and independently fetched the same pathname with `?seed=real`. Since the extension intentionally deduplicates by full URL, these were two distinct URLs, while the E2E erroneously required exactly one match **by pathname**. Commit `6ad4a766` makes the request URLs identical, retaining the strict one-resource check. This fixture diagnosis does not establish that the entire E2E has passed; the next Windows run must prove that. Preserve the original red run in the evidence record.

## Second red: real handoff reached GUI but lost persistent task identity

[Windows CI #38113661520](https://github.com/159357yangjun/swoop/actions/runs/38113661520) reached the *actual packaged GUI*: original GUI stderr recorded `IPC add: callback returned taskId=2000000` after the Edge Popup operation. The test then failed at `Edge session-bound handoff timed out`. Source inspection confirms a one-resource click uses `batchDownload` / Native Host `batch_add`, whose response only includes aggregate counts, so it never goes through the existing `rememberHandoff` path. This PR changes **only one-item Popup sends** to `downloadWithOptions`, preserving queue mode and one-shot submission. Multi-item batch requests retain the old behavior; dedicated Node regression cases cover both.

The original test also looked up journal entries by `url`, but persisted records intentionally hold `filename`, `taskId` and `sessionId` (not URL). It now matches the unique fixture filename. Finally the GUI log showed native TS fallback, not MP4. The test now explicitly sets `ffmpegCustomPath` to the runner's real FFmpeg executable in isolated settings, preserving the strict final MP4 and decoded SHA-256 verification. These changes do not alter any HLS download engine code or relax the user-visible status assertions.

The first and second red runs remain permanently referenced here. **The new combined head still requires an independent Windows CI before passing.**

## Follow-up diagnosis (2026-10-11)

Initial Windows CI #38111104899 failed at `Edge media discovery timed out` before any Native Host handoff. The real network discovery `FILE_PATTERNS` did not contain `.m3u8`. This branch now adds that extension to the media request pattern and type inference; the fixture issues only one HTTP(S) playlist fetch rather than competing video and fetch URLs. These are genuine detection/fixture corrections, not relaxed assertions. Subsequent validation is required for the new head. Preserve the original failed run.
