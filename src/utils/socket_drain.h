#pragma once

class QLocalSocket;

/* 把 socket 的待发队列真的推到空，再让调用方去关它；返回是否推完。
 * budgetMs 是总预算（不是单次等待）。
 *
 * 为什么要有这个函数：Windows 上 QLocalSocket 的写出走 QWindowsPipeWriter（异步），
 * 而 `waitForBytesWritten(n)` 的语义是"**这段时间内有字节写出去就返回 true**"，
 * 不是"全部推完"。所以 `write() + waitForBytesWritten(2000) + disconnectFromServer()
 * + deleteLater()` 这套写法，如果那次等待提前返回而队列里还有字节，回话就会随
 * socket 一起没了 —— 客户端表现是"连上了、什么都没收到"，
 * 而服务端日志写着"处理 0ms → 成功"。CI 第 13 步那次红的两个数正是这个形状
 * （排队 51ms、处理 0ms，同时副实例报 3 秒无回话）。
 *
 * ⚠️ 但这仍然只是**假设**，本地既证不了也证不伪：IDM_FLUSH_PROBE 拿 2MB 回话做 A/B，
 * 让对端晚 300ms 才开始读，旧写法（只等一次）与新写法（推到空）**都把 2MB 完整送到**，
 * 销毁前 bytesToWrite() 都是 0（本机 27ms 就写完了）。也就是说本机造不出
 * "返回时仍有待发字节"的状态，本函数的必要性只能由生产日志的 `送达=0/1` 判：
 * CI 出现 `送达=0` 就是这条路；若全是 `送达=1` 而客户端仍超时，就得换方向查。
 * 留着它的理由不是"证明过"，而是"严格更保守，且预算封顶不会多钉 GUI 线程"。 */
bool drainSocketBeforeClose(QLocalSocket* socket, int budgetMs);
