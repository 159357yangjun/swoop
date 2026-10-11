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
