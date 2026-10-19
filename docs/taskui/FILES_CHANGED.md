# UI-00 / UI-01 · 文件交付清单
**本批仅设计、规则、CI，未修改 UI/引擎运行代码**。所有文件的真实差异应以 GitHub PR Files 为准。

| 操作 | 路径 | 作用 |
| --- | --- | --- |
| 新增（29 个源文件） | `TaskUI/**` | 资料库 TaskUI v1.0.0 原样收录，源码完整性逐文件 SHA256 验证 |
| 新增 | `AGENTS.md` | UI 工作流与每次提交必须交付完整文件/代码变更 |
| 新增 | `docs/taskui/SHA256SUMS` | 29 文件 SHA256 清单 |
| 新增 | `docs/taskui/README.md` | 验证方法、阶段状态 |
| 新增 | `docs/taskui/UI-00-AUDIT.md` | 现有 Qt6 与 MV3 代码审计、风险与 G3 待测项 |
| 新增 | `docs/taskui/project.taskui.json` | G0/G1 项目级任务—动作—屏幕映射 |
| 新增 | `docs/taskui/UI-01-WIREFRAME.md` | 桌面、Popup、诊断工作区设计线框 |
| 新增 | `docs/taskui/UI-01-TASK-WALKTHROUGH.md` | 七个真实场景的计划脚本，全部未测试 |
| 新增 | `docs/taskui/UI-01-DECISIONS.md` | 入口、交互状态、布局与非目标决策 |
| 新增 | `.github/workflows/taskui-design.yml` | 原包完整性、正反例和项目 manifest 的自动闸门 |

**测试状态**：原始包的本地 9/9 单元测试及示例严格检查通过；项目 manifest 的 CI 严格检查需要提交后验证，G3 未执行。任何 CI 结果须在报告中按实际事实更新。
**风险与回滚**：只添加文件；目标集成分支可通过 Revert 本批提交回滚。不强行删除 `master` 或合并旧 Draft PR。
