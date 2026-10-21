"""Save the pcap stream that btvs.exe -Mode Wireshark serves on TCP 24352 to a file.

usage: python pcap_tcp_dump.py out.pcap [seconds] [port]
"""
import socket
import sys
import time

out, secs = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 120
port = int(sys.argv[3]) if len(sys.argv) > 3 else 24352
deadline = time.time() + secs
while True:
    try:
        s = socket.create_connection(('127.0.0.1', port), timeout=2)
        break
    except OSError:
        if time.time() > deadline:
            sys.exit('btvs is not listening')
        time.sleep(0.5)
total = 0
with open(out, 'wb') as f:
    s.settimeout(1)
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        f.write(chunk)
        f.flush()
        total += len(chunk)
print(f'{total} bytes')
