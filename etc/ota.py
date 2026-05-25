import datetime

import requests

fallback_hosts = [
    # (ip, port, name),
    # ('192.168.4.3', 0, 'flat'),
    # ('192.168.4.2', 0, 'fry'), # 192.168.4.3
]

"""

1. build (idf.py build)
2. start web server to serve firmware binary
    python3 -m http.server 9000upd
3. discover hosts
4 iterate hosts
    > ota http://192.168.1.161:9000/build/fugu-firmware.bin 

idf.py build

"""""
import asyncio
import atexit
import os
import re
import socket
import subprocess
import sys
import time

from etc.fugu.discover import discover_scope_servers
from etc.fugu.fugu import FuguDevice
from etc.fugu.transport import SocketTransport
from etc.fugu_console import scan_nat_async

HTTP_PORT = 9000


def ensure_http_server():
    """Spawn `python3 -m http.server` serving the repo root if port is free."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        if s.connect_ex(('127.0.0.1', HTTP_PORT)) == 0:
            return  # something already listening
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    print(f'starting http.server on :{HTTP_PORT} (cwd={repo_root})')
    proc = subprocess.Popen([sys.executable, '-m', 'http.server', str(HTTP_PORT)],
                            cwd=repo_root, stderr=subprocess.DEVNULL)
    atexit.register(proc.terminate)
    time.sleep(.5)


hosts = [(h, 23, n) for h, _, n in discover_scope_servers()]
hosts += asyncio.run(scan_nat_async(reachable_only=True))
hosts = hosts or fallback_hosts

if not hosts:
    print('no hosts discovered!')
    sys.exit(1)

print(hosts)



async def send_ota_command(addr, port, name):
    print('\n', name)
    st = SocketTransport(addr, port=port or SocketTransport.DEFAULT_PORT, timeout=10)
    fd = FuguDevice(st, block=True, prefix=name)
    fd.verbose = True
    ota_progress = 0
    success = False

    def on_message(rx):
        nonlocal ota_progress, success
        m = re.match(r'.*ota: Download Progress:\s*([0-9.]+)\s*%.*', rx)
        if m:
            ota_progress = float(m[1])
        if 'OTA Succeed' in rx:
            if ota_progress != 100:
                print('OTA Succeed msg but progress ', ota_progress, '!= 100')
                ota_progress = 101
            else:
                success = True

    fd.on_message = on_message

    private_ip, *_ = st.sock.getsockname()
    # fd.wait_for_pwm_state()
    fd.write(f"ping\n")  # clear command buffer on device
    time.sleep(.1)

    url = f"http://{private_ip}:9000/build/fugu-firmware.bin"
    import email.utils
    print('firmware mtime', email.utils.parsedate_to_datetime(requests.head(url).headers['Last-Modified']).astimezone().isoformat())
    fd.write(f"ota {url}\n")
    # fd.write(f"reset\n")
    # print(fd.prefix, fd.pwm_state)
    while ota_progress < 100:
        if not st.check_connection():
            print(fd.prefix, 'connection to device unexpectedly closed @ota_progress=', ota_progress)
            return False
        await asyncio.sleep(.3)

    if ota_progress != 100:
        return False

    print(fd.prefix, 'waiting for device to close the connection..')
    t_wait_start = time.time()
    while st.check_connection():
        if time.time() - t_wait_start > 30:
            # device may have rebooted without sending FIN; TCP keepalive should have
            # tripped check_connection by now — bail out instead of spinning forever
            print(fd.prefix, 'still "connected" after 30s, forcing close')
            st.close()
            break
        time.sleep(.2)
    print(fd.prefix, 'closed the connection')
    fd.close()

    if not success:
        print(fd.prefix, 'didnt send a success msg')
        return False

    # now try to re-connect
    print(fd.prefix, 'waiting for device to come online again')
    for _ in range(10):
        time.sleep(1)
        st = SocketTransport(addr, port=port or SocketTransport.DEFAULT_PORT)
        try:
            fd = FuguDevice(st, block=True, prefix=name)
        except (ConnectionRefusedError, TimeoutError):
            continue
        print(fd.prefix, 'device back online! OTA successful (probably TODO check ver)')
        break
    else:
        print(fd.prefix, 'device didnt come online in time')
        return False

    return True

async def send_ota_command_try(addr, port, name):
    try:
        return await send_ota_command(addr,port,name)
    except Exception as e:
        print('err', addr, name, e)
        return False



async def main():
    ensure_http_server()
    time.sleep(1)
    res = await asyncio.gather(*[send_ota_command_try(ip, port, name) for ip, port, name in hosts])
    res = dict(zip((name for _, _, name in hosts), res))
    for name, ok in res.items():
        print('%20s: %s' % (name, '✅' if ok else '❌'))
    return all(res.values())


sys.exit(0 if asyncio.run(main()) else 1)
