# UI-01 · 真实任务走查脚本（尚未执行）
**状态：PLANNED**。此处是待执行脚本；没有 Windows 实操截图/录屏就不能写 PASS。高频/低频基于产品用途假设，尚无用户行为数据。

| 任务 | 起始条件 | 操作路径（manifest ID） | 可观察完成标准 | 失败与恢复检查 | 当前结果 |
| --- | --- | --- | --- | --- | --- |
| T-D1 桌面新建 | 桌面端已打开 | A-create-task | 出现身份明确的新任务 | URL 校验失败；可取消 | **未测试** |
| T-D2 管理任务 | 列表有两个不同状态任务 | A-filter-category→A-select-task→A-control-task | 仅目标行改变状态 | 不可执行按钮禁用，失败可解释 | **未测试** |
| T-D3 排错 | 人为制造允许的 HTTP 404 | A-select-task→A-open-detail→A-read-error | 用户看到具体错误及下一步 | 错误文本可选择且不截断 | **未测试** |
| T-P1 单文件 | 当前页有可下载的公开文件 | A-filter-resources→A-select-resource→A-submit-resource | UI 分辨“资源发现/被桌面接收/最终完成” | Native Host 拒绝或 ACK 未知不误报成功 | **未测试** |
| T-P2 多文件 | 当前页≥3个不同资源 | A-filter-resources→A-select-many→A-submit-batch | 数量与桌面接收结果对应 | 部分失败不静默重试 | **未测试** |
| T-P3 断连 | Windows GUI 停止/Native Host 未注册 | A-check-connection→A-open-diagnostics | 清楚指出可验证原因和手动修复方式 | 只读检查；可返回资源 | **未测试；诊断页 G2 待实现** |
| T-P5 部分成功 | 批量3个文件，桌面仅接收其中2个，返回仅含 success/failed 聚合结果 | A-submit-batch→A-review-batch-result | 显示“部分接收/具体对象未知”，不把前2项标为成功 | 保留用户选择上下文；不盲目重试 | **未测试；结果 UI G2 待实现** |
| T-P4 未确认 | MV3 请求已发送但 ACK 丢失 | A-refresh-handoff | 展示未知数与原请求结果 | 没有额外 add_download；旧 UUID 不丢失 | **未测试（实机 UI）** |

## G3 数据收集标准
- 桌面：Windows 1366×768、1920×1080，100%/125%/150% DPI，浅色/深色。
- 浏览器：Chrome/Edge 开发者模式加载插件，Popup 400×560，验证“零资源”“多个资源”“候选及最近转交双展开”“连接错误”。
- 每项记录 **实际操作步骤/时间、截图或录屏文件路径、开始状态、结束状态、焦点 Tab 顺序、Escape 返回、错误恢复、复测结论**。
- 任何未来测试实现不存在的功能必须写 BLOCKED（未实现），不能伪造用户完成任务。

## 边界
既有 CI 中的 Native/MV3/HLS 自动化保障下载链路，不等于本页列出的 G3 用户任务和人工理解性测试已通过。
