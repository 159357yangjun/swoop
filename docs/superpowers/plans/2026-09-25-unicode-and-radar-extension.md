# Swoop Unicode GUI and Radar Extension Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use inline execution with the task checklist below. Each task is verified before the next task starts.

**Goal:** 修复 Windows GUI 菜单中文乱码，并将现有 MV3 后台扩展升级为 A 方案 Radar 弹窗，支持当前页面资源扫描、最近任务和 native messaging 下载。

**Architecture:** 资源编译链显式使用 UTF-8；扩展保持原生 MV3，不引入构建工具。Service worker 继续负责下载拦截、右键菜单和 native host，popup 负责交互，content script 只负责当前页候选资源提取。

**Tech Stack:** C11/Win32, GNU windres, Chrome/Edge Manifest V3, vanilla HTML/CSS/JavaScript, Python unittest, Node `--check`.

---

### Task 1: Add the failing encoding regression test

**Files:**
- Create: `packaging/test_resource_encoding.py`

- [ ] **Step 1: Write the failing test**

```python
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ResourceEncodingTest(unittest.TestCase):
    def test_resource_script_declares_utf8(self):
        text = (ROOT / "src" / "gui" / "resources.rc").read_text(encoding="utf-8")
        self.assertIn("#pragma code_page(65001)", text)

    def test_makefile_passes_utf8_codepage_to_windres(self):
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("--codepage=65001", text)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python packaging/test_resource_encoding.py`

Expected: FAIL because `resources.rc` and the `windres` command do not yet contain the UTF-8 declarations.

### Task 2: Implement and verify UTF-8 resource compilation

**Files:**
- Modify: `src/gui/resources.rc` at the first resource directive
- Modify: `Makefile` at the `build/resources.res` recipe

- [ ] **Step 1: Add the minimal implementation**

Add `#pragma code_page(65001)` before the first resource string in `resources.rc`, and change the resource recipe to:

```make
build/resources.res: src/gui/resources.rc src/gui/app.ico src/gui/app.manifest src/common/version.h
	mkdir -p build && $(WINDRES) --codepage=65001 --output-format=coff $< -o $@
```

- [ ] **Step 2: Run the regression test**

Run: `python packaging/test_resource_encoding.py`

Expected: PASS.

- [ ] **Step 3: Verify the local resource compiler accepts the option**

Run: `& 'C:\Qt\Tools\mingw1310_64\bin\windres.exe' --help | Select-String codepage`

Expected: the help output includes `--codepage`.

### Task 3: Add the failing extension contract and candidate extraction tests

**Files:**
- Create: `extension/test_extension.py`
- Create: `extension/test_content.js`

- [ ] **Step 1: Write the manifest contract test**

The Python test loads `extension/manifest.json`, asserts `action.default_popup` is `popup.html`, checks that `content.js` is declared for `<all_urls>`, and verifies every referenced extension file exists.

- [ ] **Step 2: Write the candidate extraction test**

The Node test loads `content.js` with `require`, passes a fake document containing duplicate HTTP links, a video source, a data URL, and a mailto URL, then asserts the result contains only unique HTTP(S) candidates with normalized filenames and types.

- [ ] **Step 3: Run both tests to verify they fail**

Run: `python extension/test_extension.py` and `node extension/test_content.js`

Expected: FAIL because the popup/content files and exported candidate collector do not exist yet.

### Task 4: Implement the Radar extension shell and content scanner

**Files:**
- Modify: `extension/manifest.json`
- Create: `extension/popup.html`
- Create: `extension/popup.css`
- Create: `extension/popup.js`
- Create: `extension/content.js`
- Create: `extension/icons/swoop.svg`

- [ ] **Step 1: Extend the manifest**

Add `action.default_popup`, `scripting`, and `storage` permissions; add a `content_scripts` entry for `content.js` on `<all_urls>`; add the SVG icon under `action.default_icon` and `icons`.

- [ ] **Step 2: Implement the pure candidate collector first**

Implement `collectCandidates(doc, pageUrl)` to inspect `a[href]`, `video[src]`, `audio[src]`, and `source[src]`; accept only HTTP(S); derive a filename from the URL or `download` attribute; assign `video`, `audio`, `archive`, `document`, or `file`; deduplicate by URL; cap results at 40; export the function only when `module` exists for Node tests.

- [ ] **Step 3: Add the content-script message handler**

Handle `{type:"scan"}` with `{ok:true,pageTitle,items}` and return an explicit error for unsupported pages without throwing into the page.

- [ ] **Step 4: Run extraction and syntax tests**

Run: `node extension/test_content.js` and `node --check extension/content.js`

Expected: PASS.

### Task 5: Implement popup interaction and native-host status

**Files:**
- Modify: `extension/background.js`
- Create: `extension/popup.html`
- Create: `extension/popup.css`
- Create: `extension/popup.js`

- [ ] **Step 1: Add background message routing**

Handle popup messages `ping`, `download`, and `recent`; route downloads through the existing `sendToHost`; store the newest five successful handoffs in `chrome.storage.local`; preserve the existing failure behavior that does not cancel the browser download.

- [ ] **Step 2: Build the Radar popup**

Use the approved dark-blue/acid-lime/warm-white direction. Include a connection badge, scan button, open-main-window button, resource list with per-item handoff actions, recent-task list, and an error panel with the exact host registration command.

- [ ] **Step 3: Connect popup actions**

On open, request `ping` and `recent`; on scan, query the active tab and send `scan`; on item click, send the resource payload with the active tab URL as `referer`; update the UI after each result.

- [ ] **Step 4: Run extension contract and syntax tests**

Run: `python extension/test_extension.py`, `node --check extension/background.js`, `node --check extension/content.js`, and `node --check extension/popup.js`.

Expected: all PASS.

### Task 6: Include the complete extension in release packages and documentation

**Files:**
- Modify: `packaging/make_release.py` optional file list
- Modify: `README.md` browser extension section

- [ ] **Step 1: Add all extension runtime files to the package**

Include `popup.html`, `popup.css`, `popup.js`, `content.js`, and `icons/swoop.svg` alongside the existing manifest/background files.

- [ ] **Step 2: Document Radar installation and use**

Explain loading the `extension` folder, registering the native host, opening the Swoop popup, scanning a page, and the fallback behavior when the host is unavailable.

- [ ] **Step 3: Verify the package contents**

Run: `python packaging/test_make_release.py` and `python packaging/make_release.py`; inspect the zip member list and assert every extension runtime file is present.

### Task 7: Build, test, version, and publish the next release

**Files:**
- Modify: `src/common/version.h` from `0.1.0` to `0.2.0` and tag to `v0.2.0`

- [ ] **Step 1: Run the full local verification**

Run: `make clean`, `make all`, `./swoop_selftest.exe`, `./swoop_nmhost.exe --selftest`, all Python/Node extension tests, and `make dist`.

Expected: all commands exit 0; package name is `swoop-0.2.0-win64.zip`.

- [ ] **Step 2: Inspect the final diff and worktree**

Run: `git diff --check` and `git status --short`.

Expected: no whitespace errors; only intended source, test, documentation, and version changes.

- [ ] **Step 3: Commit and push `main`**

Use commit message `feat: add Radar browser extension and UTF-8 resources`, then push `main`.

- [ ] **Step 4: Create and push the release tag**

Create annotated tag `v0.2.0` at the verified commit and push it to `origin`; let `release.yml` build and publish the formal GitHub Release.

- [ ] **Step 5: Verify the published release**

Confirm the release page contains `swoop-0.2.0-win64.zip`, `SHA256SUMS.txt`, and the complete extension files inside the zip.
