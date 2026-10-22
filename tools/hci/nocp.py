"""Transmit pacing from HCI "Number Of Completed Packets" events (no ACL payload needed):
packets the controller finished per second on the busiest ACL handle, and the longest
gap between completions while streaming.

usage: python nocp.py capture.pcap [first_second] [last_second]
"""
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from avdtp_dump import packets  # noqa: E402

path = sys.argv[1]
t_from = float(sys.argv[2]) if len(sys.argv) > 2 else 0
t_to = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
events = defaultdict(list)  # handle -> [(t, count)]
t0 = None
for ts, sent, h4, p in packets(path):
    t0 = ts if t0 is None else t0
    if h4 != 4 or len(p) < 3 or p[0] != 0x13:
        continue
    n = p[2]
    for i in range(n):
        off = 3 + 4 * i
        if off + 4 > len(p):
            break
        handle = (p[off] | p[off + 1] << 8) & 0x0FFF
        count = p[off + 2] | p[off + 3] << 8
        events[handle].append((ts - t0, count))

if not events:
    sys.exit('no Number Of Completed Packets events')
handle = max(events, key=lambda h: sum(c for _, c in events[h]))
ev = [(t, c) for t, c in events[handle] if t_from <= t <= t_to]
per_s = defaultdict(int)
for t, c in ev:
    per_s[int(t)] += c
secs = sorted(per_s)
busy = [s for s in secs if per_s[s] > 20]
gaps = [b[0] - a[0] for a, b in zip(ev, ev[1:]) if a[0] >= (busy[0] if busy else 0) and b[0] <= (busy[-1] + 1 if busy else 0)]
print(f'handle 0x{handle:03x}: {sum(c for _, c in ev)} packets in {len(ev)} events')
if busy:
    rates = [per_s[s] for s in busy]
    print(f'streaming {busy[0]}..{busy[-1]} s: {sum(rates) / len(rates):.1f} packets/s (min {min(rates)}, max {max(rates)})')
    print(f'longest gap between completions while streaming: {max(gaps) * 1000 if gaps else 0:.0f} ms')
    print('per second: ' + ' '.join(str(per_s[s]) for s in range(busy[0], busy[-1] + 1)))
