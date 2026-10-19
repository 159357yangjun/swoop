# IDM Next · AI 交付与 UI 修改约束
本仓库下载引擎、Native Host、Qt6 客户端和 Chrome/Edge MV3 扩展相互配合，**UI 改动不得默认改变下载请求或 IPC 行为**。

涉及 `src/gui/`、`resources/qss/`、`browser-extension/popup.*`、`browser-extension/content.js` 的新增复杂界面/重构时：
1. 阅读 `TaskUI/AGENTS.md`、`TaskUI/core/`、匹配的 `patterns/` 与 `adapters/`。
2. **G0→G1→严格 manifest 校验→G2→G3**；以 `docs/taskui/project.taskui.json` 与真实线框、任务走查和决策记录为基线。
3. **必须同时审查产品设计、信息架构、任务流程、误操作防护、状态语义、错误恢复、视觉布局和可访问性**，不得只交布局美化；先做高频主任务及空/加载/成功/失败/未知/恢复。对象操作不远离对象，不随意加永久侧栏、卡片或菜单。
4. G3 必须真实 Windows/Chrome/Edge 完成目标任务；只跑静态校验不得称“界面验收通过”。
5. **每次代码提交都要交付**完整文件变更清单（增/改/删）、Commit SHA、PR、前后关键 Diff、测试的通过/失败/未测状态、风险与回滚路径；合并必须说明目标分支与实际结果。
6. 禁止未经单独批准将 UI 批次改动直接合入 `master` 或覆盖本地未推送 Windows 文件。

规则包 `TaskUI/` 是原样收录的 v1.0.0，不在业务 PR 中直接修改。
