# ⑩ sniff capstone 取证残留清点（删除前留档，2026-10-06）

夹具源 `/media/dom.mp4`（由 tools/disk_e2e_server.py 的 build_media() **本轮现算**，不是抄日志）：
  size=200012 sha256=267989ec01812b908365ae5d16e8a4ba6fe3ff615e96db53a8f11092a0f91fb9

| 文件 | size | sha256 | mtime |
|---|---|---|---|
| `dom.mp4` | 200012 | `267989ec01812b908365ae5d16e8a4ba6fe3ff615e96db53a8f11092a0f91fb9` | 2026-10-06 07:54:38 |
| `dom (1).mp4` | 200012 | `267989ec01812b908365ae5d16e8a4ba6fe3ff615e96db53a8f11092a0f91fb9` | 2026-10-06 08:01:53 |
| `dom (2).mp4` | 200012 | `267989ec01812b908365ae5d16e8a4ba6fe3ff615e96db53a8f11092a0f91fb9` | 2026-10-06 08:04:38 |

三份逐字节全等于夹具源（Python 侧 `d == src` 三处均 True）⇒ 同名策略下每次写入都完整正确，缺陷只在"名字被改了但 GUI 不回读"。
本轮按所有者指示删除这三个测试残留；sniff 的 capstone 取证点改为向 GUI 现取最终落点名（见 §7-65 第七节）。
