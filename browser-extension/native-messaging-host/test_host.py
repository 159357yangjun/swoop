#!/usr/bin/env python3
"""
test_host.py — IDM Next Native Messaging Host 测试脚本 v0.2

测试所有支持的操作：
  - ping:         心跳检测
  - add_download:  添加单个下载
  - batch_add:     批量添加下载
  - get_status:    获取 IDM Next 运行状态
"""

import json
import struct
import subprocess
import sys

host_bat = r'D:\visual studio\lianxi\MFC\idm-next\browser-extension\native-messaging-host\idm_next_host.bat'

# 启动 host
proc = subprocess.Popen(
    [host_bat],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=False
)

def send(msg):
    data = json.dumps(msg).encode('utf-8')
    proc.stdin.write(struct.pack('<I', len(data)) + data)
    proc.stdin.flush()

def recv():
    raw = proc.stdout.read(4)
    if len(raw) < 4:
        return None
    length = struct.unpack('<I', raw)[0]
    data = proc.stdout.read(length).decode('utf-8')
    return json.loads(data)

try:
    # 1. 测试 ping
    print('1. Sending ping...')
    send({'action': 'ping'})
    resp = recv()
    print('   Response:', resp)
    assert resp and resp.get('success'), 'ping failed'
    print('   ✓ Host connected, version:', resp.get('version'))

    # 2. 测试 add_download
    print('\n2. Sending add_download...')
    send({'action': 'add_download', 'url': 'https://www.example.com/', 'filename': 'test.html'})
    resp = recv()
    print('   Response:', resp)
    assert resp and resp.get('success'), 'add_download failed'
    print('   ✓ Single download added')

    # 3. 测试 batch_add
    print('\n3. Sending batch_add...')
    items = [
        {'url': 'https://example.com/file1.zip', 'filename': 'file1.zip'},
        {'url': 'https://example.com/file2.zip', 'filename': 'file2.zip'},
        {'url': 'https://example.com/file3.zip', 'filename': 'file3.zip'},
    ]
    send({'action': 'batch_add', 'items': items})
    resp = recv()
    print('   Response:', resp)
    assert resp and resp.get('success', 0) > 0, 'batch_add failed'
    print(f"   ✓ Batch added: {resp.get('added', 0)} succeeded, {resp.get('failed', 0)} failed")

    # 4. 测试 get_status
    print('\n4. Sending get_status...')
    send({'action': 'get_status'})
    resp = recv()
    print('   Response:', resp)
    if resp and resp.get('running'):
        print(f"   ✓ IDM Next running, {resp.get('task_count', 0)} tasks")
    else:
        print('   ⚠ IDM Next not running (expected if GUI is closed)')

    # 5. 测试未知操作
    print('\n5. Sending unknown action...')
    send({'action': 'foobar'})
    resp = recv()
    print('   Response:', resp)
    assert resp and not resp.get('success'), 'unknown action should fail'
    print('   ✓ Unknown action rejected')

    print('\n=== All tests passed! ===')

except Exception as e:
    print(f'\n✗ Error: {e}')
    sys.exit(1)
finally:
    proc.stdin.close()
    proc.wait(timeout=5)
    if proc.stderr:
        err = proc.stderr.read().decode('utf-8', errors='ignore')
        if err.strip():
            print('\nHost stderr:', err[:200])
