# UI-00 / UI-01 · 本批完整文件变更清单
## 版本与操作范围
- 基线：`9d21a4357eaf6fa57f19e53f50c37ca3c7e9e9ea`（PR #31 集成分支）。
- 本批共 **41 个新增文件**（基于前一提交树逐项对比）；原有运行代码文件修改 **0**，删除 **0**。开发分支中的后续提交只是修订这些新文件。
- 原包 `TaskUI/` 29 文件保持内容不变并逐项校验 SHA-256。
- 对用户可见的真实改动以本批 PR 的 **Files changed** 页面以及完整 `.patch` 为准。
- 主要文档：`UI-00-PRODUCT-DESIGN-AUDIT.md` 与 `UI-01-STATE-CONTRACT.md`，包含产品设计而非仅布局。

## 逐文件路径（全部 41 个）
| 序号 | 变更类型 | 仓库相对路径 | 说明 |
| ---: | --- | --- | --- |
| 1 | 新增 | \`.github/workflows/taskui-design.yml\` | 项目 TaskUI CI 闸门 |
| 2 | 新增 | \`AGENTS.md\` | AI 开发规范及完整代码交付约束 |
| 3 | 新增 | \`TaskUI/.github/workflows/taskui.yml\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 4 | 新增 | \`TaskUI/.gitignore\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 5 | 新增 | \`TaskUI/AGENTS.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 6 | 新增 | \`TaskUI/LICENSE\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 7 | 新增 | \`TaskUI/README.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 8 | 新增 | \`TaskUI/adapters/desktop.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 9 | 新增 | \`TaskUI/adapters/touch.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 10 | 新增 | \`TaskUI/adapters/web.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 11 | 新增 | \`TaskUI/checklists/human-review.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 12 | 新增 | \`TaskUI/core/00-task-analysis.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 13 | 新增 | \`TaskUI/core/01-placement.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 14 | 新增 | \`TaskUI/core/02-layout-interaction.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 15 | 新增 | \`TaskUI/core/03-gates.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 16 | 新增 | \`TaskUI/examples/bad-diagnostics.json\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 17 | 新增 | \`TaskUI/examples/bad.project.taskui.json\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 18 | 新增 | \`TaskUI/examples/decision-log.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 19 | 新增 | \`TaskUI/examples/good.project.taskui.json\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 20 | 新增 | \`TaskUI/examples/layout-wireframe.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 21 | 新增 | \`TaskUI/examples/task-tests.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 22 | 新增 | \`TaskUI/patterns/business-workflow.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 23 | 新增 | \`TaskUI/patterns/content-editor.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 24 | 新增 | \`TaskUI/patterns/guided-wizard.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 25 | 新增 | \`TaskUI/patterns/monitoring-analysis.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 26 | 新增 | \`TaskUI/patterns/object-management.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 27 | 新增 | \`TaskUI/prompts/design.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 28 | 新增 | \`TaskUI/prompts/review.md\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 29 | 新增 | \`TaskUI/schemas/project.schema.json\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 30 | 新增 | \`TaskUI/tests/test_taskui_check.py\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 31 | 新增 | \`TaskUI/tools/taskui_check.py\` | 原版 TaskUI v1.0.0 规则/校验器/示例（原样纳入） |
| 32 | 新增 | \`docs/taskui/FILES_CHANGED.md\` | 完整文件路径交付清单 |
| 33 | 新增 | \`docs/taskui/README.md\` | 验证方法与阶段状态 |
| 34 | 新增 | \`docs/taskui/SHA256SUMS\` | 原版 29 文件逐项 SHA-256 |
| 35 | 新增 | \`docs/taskui/UI-00-AUDIT.md\` | 现有 Qt6/Popup 代码布局审计 |
| 36 | 新增 | \`docs/taskui/UI-00-PRODUCT-DESIGN-AUDIT.md\` | 产品设计、交互缺陷、状态风险与优先级 |
| 37 | 新增 | \`docs/taskui/UI-01-DECISIONS.md\` | 入口、状态和误操作设计决策 |
| 38 | 新增 | \`docs/taskui/UI-01-STATE-CONTRACT.md\` | 浏览器-桌面完整状态机与错误恢复契约 |
| 39 | 新增 | \`docs/taskui/UI-01-TASK-WALKTHROUGH.md\` | 真实用户任务 G3 测试脚本（未执行） |
| 40 | 新增 | \`docs/taskui/UI-01-WIREFRAME.md\` | 主要工作区与诊断草图 |
| 41 | 新增 | \`docs/taskui/project.taskui.json\` | G0/G1 项目级任务-屏幕-动作映射 |

## 验收状态与剩余工作
- 第一版 TaskUI 源完整性、checker 测试、好坏示例和项目 manifest 严格检查已通过 GitHub CI；修订稿请以最新工作流结果为准。
- **G0/G1 文档与清单完成**；G2 不涉及任何桌面端/插件运行时代码；G3 真实任务与截图仍未执行。
- **现存高优先级设计问题未修复**：批量部分成功的对象身份错误推断、点击“选择目录/画质”退化为直接提交、连接诊断缺少定位步骤。
- **回滚**：在集成分支对本批 Git commit / PR 执行 Revert；不要改动未上传的 Windows 本地代码。
