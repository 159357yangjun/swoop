2026-10-02 07:1x：这五份是 IDM_KEYNAV_PROBE 定型前的中间读数，其中两份已被实测推翻——
  keynav-red-1002-063607 / -063743：K1 当时判据是"发 Ctrl+N 看弹窗"，canary 证明 offscreen
    合成按键进不了快捷键系统 ⇒ 那两条红属未归因（测具的锅）。
  keynav-red-1002-064009：context 图例写错（凭印象写成 2=Window 3=Application）。
  keynav-green-1002-064216：K1 绿但 K2 仍按错误期望断言。
  keynav-green-1002-064358：0 字节——那次用 `timeout … > file` 跑，输出根本没落盘，
    当场差点被我读成"跑过了"；实际只有 rc 可用。留档是为了记住这个形状。
承重三份在仓根 docs/：keynav-red-1002-064100.txt（先红）、keynav-green-1002-070504.txt、
keynav-final-1002-070817.txt。

（本轮追加）forward-differential-1002-232916.txt 已删：那份的 fault=off 面根本没关——
`env $E` 在 E 为空时仍执行成 `env IDM_FWD_DIAG= ...`，变量被置成空值，而判据用的是
qEnvironmentVariableIsSet（空串也算"已设置"）⇒ 两面其实是同一面。留这一句是为了记住形状：
**对照面必须用"变量不存在"来构造，不能用"变量等于空"**。正确版在 forward-differential-1002-232927.txt。
