#!/usr/bin/env python3
"""
IDM Next Native Messaging Host v0.2
===================================

浏览器扩展通过 Chrome Native Messaging 协议与本脚本通信。
本脚本接收扩展发来的下载请求，调用 IDM Next CLI 添加下载任务。

支持的操作：
  - ping:        心跳检测（检查 host 是否可用）
  - add_download: 添加单个下载任务
  - batch_add:    批量添加多个下载任务
  - get_status:   获取 IDM Next 运行状态

通信协议：
  - stdin/stdout，每条消息前 4 字节小端序长度前缀 + JSON 正文
  - 消息格式：{ "action": "...", ... }

部署方式：
  1. 将本脚本路径注册到 Native Messaging manifest JSON
  2. 将 manifest JSON 路径注册到系统（Windows 注册表 / macOS/Linux 配置目录）
  详见同目录下 README.md

依赖：Python 3.8+（仅标准库，无第三方依赖）
"""

import json
import struct
import sys
import subprocess
import os
import platform
import shutil


# ── 配置 ────────────────────────────────────────
IDM_NEXT_CLI = os.environ.get('IDM_NEXT_CLI', '')


def _resolve_cli():
    """按优先级寻找可用的 IDM Next CLI 可执行文件"""
    if platform.system() == 'Windows':
        candidates = [
            # 开发环境：项目 build 目录
            r'D:\visual studio\lianxi\MFC\idm-next\build\idm-next.exe',
            os.path.join(os.environ.get('LOCALAPPDATA', ''), 'IDMNext', 'idm-next.exe'),
            os.path.join(os.environ.get('PROGRAMFILES', ''), 'IDMNext', 'idm-next.exe'),
        ]
    else:
        candidates = [
            '/usr/local/bin/idm-next',
            '/usr/bin/idm-next',
        ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    # 最后尝试 PATH
    return shutil.which('idm-next') or ''


if not IDM_NEXT_CLI:
    IDM_NEXT_CLI = _resolve_cli()


def read_message():
    """从 stdin 读取一条 Native Messaging 消息（4字节长度前缀 + JSON）"""
    raw_length = sys.stdin.buffer.read(4)
    if len(raw_length) < 4:
        return None
    length = struct.unpack('<I', raw_length)[0]
    if length == 0 or length > 10 * 1024 * 1024:  # 最大 10MB
        return None
    data = sys.stdin.buffer.read(length).decode('utf-8')
    return json.loads(data)


def send_message(msg):
    """向 stdout 写入一条 Native Messaging 消息"""
    data = json.dumps(msg).encode('utf-8')
    sys.stdout.buffer.write(struct.pack('<I', len(data)))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def _run_cli(args, timeout=30):
    """执行 IDM Next CLI 命令，返回 (success, stdout, stderr)"""
    try:
        result = subprocess.run(
            [IDM_NEXT_CLI] + args,
            capture_output=True,
            text=True,
            timeout=timeout,
            creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == 'Windows' else 0
        )
        return result.returncode == 0, result.stdout.strip(), result.stderr.strip()
    except FileNotFoundError:
        return False, '', 'IDM Next 未找到，请确认已安装'
    except subprocess.TimeoutExpired:
        return False, '', 'IDM Next 响应超时'
    except Exception as e:
        return False, '', str(e)


def add_download(url, filename=''):
    """添加单个下载任务"""
    cmd = ['--cli', 'add', url, '--no-wait']
    if filename:
        cmd.extend(['--name', filename])

    success, stdout, stderr = _run_cli(cmd)
    if success:
        return {'success': True, 'message': f'已添加下载: {filename or url[:50]}'}
    else:
        return {'success': False, 'message': f'CLI 错误: {stderr}'}


def batch_add(items):
    """批量添加下载任务"""
    added = 0
    failed = 0
    errors = []

    for item in items:
        url = item.get('url', '')
        filename = item.get('filename', '')
        if not url:
            failed += 1
            continue

        result = add_download(url, filename)
        if result.get('success'):
            added += 1
        else:
            failed += 1
            errors.append(f'{filename or url[:30]}: {result.get("message", "失败")}')

    msg = f'已添加 {added} 个任务'
    if failed > 0:
        msg += f'，{failed} 个失败'
    if errors:
        msg += '\n' + '\n'.join(errors[:3])  # 最多显示 3 条错误
        if len(errors) > 3:
            msg += f'\n...还有 {len(errors) - 3} 个错误'

    return {'success': added > 0, 'added': added, 'failed': failed, 'message': msg}


def get_status():
    """检查 IDM Next 是否可用 + 获取任务列表概要"""
    # 先检查 CLI 是否存在
    if not os.path.isfile(IDM_NEXT_CLI) and not _which(IDM_NEXT_CLI):
        return {
            'success': False,
            'running': False,
            'message': 'IDM Next 可执行文件未找到'
        }

    # 获取任务列表
    success, stdout, stderr = _run_cli(['--cli', 'list'], timeout=10)
    if not success:
        return {
            'success': False,
            'running': False,
            'message': f'无法获取任务列表: {stderr}'
        }

    # 简单解析任务数量
    lines = stdout.strip().split('\n')
    task_count = max(0, len(lines) - 1)  # 减去表头

    return {
        'success': True,
        'running': True,
        'task_count': task_count,
        'message': f'IDM Next 运行中，当前 {task_count} 个任务'
    }


def _which(cmd):
    """检查命令是否在 PATH 中"""
    return shutil.which(cmd) is not None


def main():
    """主消息循环"""
    while True:
        msg = read_message()
        if msg is None:
            break

        action = msg.get('action', '')

        if action == 'add_download':
            url = msg.get('url', '')
            filename = msg.get('filename', '')
            if not url:
                send_message({'success': False, 'message': 'URL 不能为空'})
            else:
                send_message(add_download(url, filename))

        elif action == 'batch_add':
            items = msg.get('items', [])
            if not items:
                send_message({'success': False, 'message': '没有可下载的资源'})
            else:
                send_message(batch_add(items))

        elif action == 'get_status':
            send_message(get_status())

        elif action == 'ping':
            send_message({'success': True, 'message': 'pong', 'version': '0.2.0'})

        else:
            send_message({'success': False, 'message': f'未知操作: {action}'})


if __name__ == '__main__':
    main()
