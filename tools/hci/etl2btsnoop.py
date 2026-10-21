"""Raw HCI packets from a Windows Bluetooth WPR trace -> btsnoop + text dump.

Input is `tracerpt BthTracing.etl -o events.xml -of XML` output. BTHPORT event
402 (channel BTH-HCI/HCIRAW) carries the packet in BIP_Type / BIP_Data with
BIP_Type 1 = command (host -> controller), 2 = event (controller -> host).
The BluetoothStack.wprp profile does NOT log ACL data, so L2CAP/AVDTP payloads
are absent; use btvs.exe from the Bluetooth Test Platform for those.

usage: python etl2btsnoop.py events.xml out.btsnoop out.txt
"""
import re
import struct
import sys
from datetime import datetime

H4 = {1: 1, 2: 4}  # BIP_Type -> H4 packet type
EV = re.compile(r'<EventID>(402)</EventID>.*?SystemTime="([^"]+)".*?'
                r'Name="BIP_Type">(\d+)<.*?Name="BIP_Data">0x([0-9A-Fa-f]*)<', re.S)


def events(path):
    raw = open(path, 'rb')
    enc = 'utf-16' if raw.read(2) in (b'\xff\xfe', b'\xfe\xff') else 'utf-8'
    raw.close()
    buf = ''
    with open(path, encoding=enc, errors='replace') as f:
        for chunk in iter(lambda: f.read(1 << 22), ''):
            buf += chunk
            last = buf.rfind('</Event>')
            if last < 0:
                continue
            for part in buf[:last].split('</Event>'):
                if 'BTH-BTHPORT' in part:
                    m = EV.search(part)
                    if m:
                        yield m.group(1), m.group(2), int(m.group(3)), bytes.fromhex(m.group(4))
            buf = buf[last + 8:]


def parse_time(s):
    s = re.sub(r'(\.\d{6})\d*', r'\1', s)
    s = re.sub(r'([+-]\d\d):(\d\d)$', r'\1\2', s)
    return datetime.strptime(s, '%Y-%m-%dT%H:%M:%S.%f%z')


def main(xml, snoop_path, txt_path):
    out = open(snoop_path, 'wb')
    out.write(b'btsnoop\0' + struct.pack('>II', 1, 1002))  # version 1, H4
    txt = open(txt_path, 'w', encoding='utf-8')
    n = 0
    for evid, ts, typ, data in events(xml):
        h4 = H4.get(typ)
        if h4 is None:
            continue
        sent = h4 == 1
        pkt = bytes([h4]) + data
        flags = (0 if sent else 1) | (2 if h4 in (1, 4) else 0)
        t = parse_time(ts)
        us = int(t.timestamp() * 1e6) + 0x00DCDDB30F2F8000  # btsnoop: microseconds since year 0
        out.write(struct.pack('>IIIIq', len(pkt), len(pkt), flags, 0, us) + pkt)
        txt.write(f"{t.strftime('%H:%M:%S.%f')} {'>' if sent else '<'} {h4} {data.hex()}\n")
        n += 1
    print(f'{n} packets')


if __name__ == '__main__':
    main(*sys.argv[1:4])
