# IDM Next · TaskUI G0/G1 设计基线
- **UI-00**：`UI-00-AUDIT.md` + `UI-00-PRODUCT-DESIGN-AUDIT.md`，基于代码的布局、业务交互、状态、防呆和产品设计风险；不是实测截图.
- **UI-01**：`project.taskui.json`，任务—屏幕—动作一一可追溯，辅以 `UI-01-WIREFRAME.md`、`UI-01-TASK-WALKTHROUGH.md`、`UI-01-DECISIONS.md`、`UI-01-STATE-CONTRACT.md`。
- **规则**：仓库 `TaskUI/` 原样收录资料库 TaskUI v1.0.0 共 29 个文件，逐文件 SHA256 见 `SHA256SUMS`。不要修改原始 29 个文件；需要项目化扩展请放 `docs/taskui/`。
- **平台**：Manifest 以 Windows 桌面产品为主，包含 Chrome/Edge MV3 Popup 和局部向导。辅助平台规则按 `TaskUI/adapters/web.md` 应用。
- **状态**：G0/G1 静态证据已编制，G2/G3 未执行；`test_plan.status=planned`。

## 复现验证
```bash
sha256sum -c docs/taskui/SHA256SUMS
python -m unittest discover -s TaskUI/tests -v
python TaskUI/tools/taskui_check.py TaskUI/examples/good.project.taskui.json --strict
python TaskUI/tools/taskui_check.py docs/taskui/project.taskui.json --strict
```
**通过不是“真实界面可用”**。G3 必须按 `UI-01-TASK-WALKTHROUGH.md` 对真实 Windows/Chrome/Edge 记录复现步骤、屏幕尺寸、截图、焦点、状态与问题。
