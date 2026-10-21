"""Decode L2CAP / AVDTP / A2DP media from a btvs pcap (or btsnoop) capture.

usage: python avdtp_dump.py capture.pcap [--media]
"""
import struct
import sys

SIG = {1: 'DISCOVER', 2: 'GET_CAPABILITIES', 3: 'SET_CONFIGURATION', 4: 'GET_CONFIGURATION',
       5: 'RECONFIGURE', 6: 'OPEN', 7: 'START', 8: 'CLOSE', 9: 'SUSPEND', 10: 'ABORT',
       11: 'SECURITY_CONTROL', 12: 'GET_ALL_CAPABILITIES', 13: 'DELAYREPORT'}
MSG = {0: 'CMD', 1: 'GENERAL_REJECT', 2: 'ACCEPT', 3: 'REJECT'}
L2SIG = {1: 'CommandReject', 2: 'ConnReq', 3: 'ConnRsp', 4: 'ConfReq', 5: 'ConfRsp', 6: 'DiscReq',
         7: 'DiscRsp', 8: 'EchoReq', 9: 'EchoRsp', 10: 'InfoReq', 11: 'InfoRsp'}
PSM = {1: 'SDP', 3: 'RFCOMM', 0x17: 'AVCTP', 0x19: 'AVDTP', 0x1b: 'AVCTP-browse'}
CAT = {1: 'MEDIA_TRANSPORT', 2: 'REPORTING', 3: 'RECOVERY', 4: 'CONTENT_PROTECTION',
       5: 'HEADER_COMPRESSION', 6: 'MULTIPLEXING', 7: 'MEDIA_CODEC', 8: 'DELAY_REPORTING'}
LDAC_FREQ = {0x20: '44.1k', 0x10: '48k', 0x08: '88.2k', 0x04: '96k', 0x02: '176.4k', 0x01: '192k'}
LDAC_CM = {4: 'mono', 2: 'dual', 1: 'stereo'}
LDAC_IE_PREFIX = bytes.fromhex('2d010000aa00')


def packets(path):
    """Yields (timestamp, sent, h4_type, payload)."""
    data = open(path, 'rb').read()
    if data[:8] == b'btsnoop\0':
        off = 16
        while off + 24 <= len(data):
            _olen, ilen, flags, _drops, ts = struct.unpack('>IIIIq', data[off:off + 24])
            pkt = data[off + 24:off + 24 + ilen]
            off += 24 + ilen
            yield ts / 1e6, not (flags & 1), pkt[0], pkt[1:]
        return
    magic = struct.unpack('<I', data[:4])[0]
    e = '<' if magic in (0xA1B2C3D4, 0xA1B23C4D) else '>'
    linktype = struct.unpack(e + 'I', data[20:24])[0]
    off = 24
    while off + 16 <= len(data):
        sec, usec, incl, _orig = struct.unpack(e + 'IIII', data[off:off + 16])
        rec = data[off + 16:off + 16 + incl]
        off += 16 + incl
        if len(rec) < incl:
            break
        if linktype == 201:  # BLUETOOTH_HCI_H4_WITH_PHDR: u32 BE direction (0 = sent), then H4
            yield sec + usec / 1e6, struct.unpack('>I', rec[:4])[0] == 0, rec[4], rec[5:]
        elif linktype == 187:  # BLUETOOTH_HCI_H4, no direction
            yield sec + usec / 1e6, None, rec[0], rec[1:]
        else:
            raise SystemExit(f'unsupported linktype {linktype}')


def caps(p):
    out = []
    i = 0
    while i + 2 <= len(p):
        cat, losc = p[i], p[i + 1]
        body = p[i + 2:i + 2 + losc]
        s = CAT.get(cat, f'cat{cat}')
        if cat == 7 and len(body) >= 2:
            codec, info = body[1], body[2:]
            if codec == 0xFF and len(info) >= 8 and info[:6] == LDAC_IE_PREFIX:
                s += (' LDAC freq=[' + ','.join(v for k, v in LDAC_FREQ.items() if info[6] & k) + ']'
                      + ' cm=[' + ','.join(v for k, v in LDAC_CM.items() if info[7] & k) + ']')
            elif codec == 0xFF and len(info) >= 6:
                vid = int.from_bytes(info[:4], 'little')
                cid = int.from_bytes(info[4:6], 'little')
                s += f' vendor=0x{vid:04x} codec=0x{cid:04x} info={info[6:].hex()}'
            else:
                s += f' codec=0x{codec:02x} info={info.hex()}'
        elif body:
            s += ' ' + body.hex()
        out.append(s)
        i += 2 + losc
    return '; '.join(out)


def l2cap_signal(t, arrow, handle, pdu, pending, chans, avdtp_pairs):
    i = 0
    while i + 4 <= len(pdu):
        code, ident, ln = pdu[i], pdu[i + 1], pdu[i + 2] | pdu[i + 3] << 8
        c = pdu[i + 4:i + 4 + ln]
        s = f'{t:9.3f} {arrow} L2CAP {L2SIG.get(code, code)}'
        if code == 2:
            psm, scid = c[0] | c[1] << 8, c[2] | c[3] << 8
            pending[(handle, ident)] = (psm, scid)
            s += f' psm={PSM.get(psm, hex(psm))} scid=0x{scid:04x}'
        elif code == 3:
            dcid, scid, res = c[0] | c[1] << 8, c[2] | c[3] << 8, c[4] | c[5] << 8
            s += f' dcid=0x{dcid:04x} scid=0x{scid:04x} result={res}'
            req = pending.get((handle, ident))
            if req and res == 0:
                chans[dcid] = chans[scid] = req[0]
                if req[0] == 0x19:
                    avdtp_pairs.append({dcid, scid})
        elif code in (4, 5):
            j = 4 if code == 4 else 6
            s += f' cid=0x{c[0] | c[1] << 8:04x}'
            while j + 2 <= len(c):
                typ, ol = c[j], c[j + 1]
                if typ & 0x7F == 1:
                    s += f' MTU={c[j + 2] | c[j + 3] << 8}'
                elif typ & 0x7F == 2:
                    s += f' flush_to={c[j + 2] | c[j + 3] << 8}'
                else:
                    s += f' opt{typ}={c[j + 2:j + 2 + ol].hex()}'
                j += 2 + ol
        elif code == 6:
            s += f' dcid=0x{c[0] | c[1] << 8:04x} scid=0x{c[2] | c[3] << 8:04x}'
        print(s)
        i += 4 + ln


def avdtp_signal(t, arrow, pdu):
    label, ptype, mtype = pdu[0] >> 4, (pdu[0] >> 2) & 3, pdu[0] & 3
    if ptype != 0:
        print(f'{t:9.3f} {arrow} AVDTP fragment type={ptype} {pdu.hex()}')
        return
    sig, body = pdu[1] & 0x3F, pdu[2:]
    s = f'{t:9.3f} {arrow} AVDTP {MSG[mtype]:<14} {SIG.get(sig, sig):<20} label={label}'
    if sig == 1 and mtype == 2:
        s += ' seps=' + ', '.join(
            f"seid{body[k] >> 2}({'SNK' if body[k + 1] & 8 else 'SRC'}{',in_use' if body[k] & 2 else ''}"
            f",media{body[k + 1] >> 4})"
            for k in range(0, len(body) - 1, 2))
    elif sig in (2, 4, 12) and mtype == 2:
        s += ' ' + caps(body)
    elif sig == 3 and mtype == 0:
        s += f' acp_seid={body[0] >> 2} int_seid={body[1] >> 2} ' + caps(body[2:])
    elif sig == 13 and mtype == 0:
        s += f' seid={body[0] >> 2} delay={(body[1] << 8 | body[2]) / 10:.1f} ms'
    elif mtype == 0 and body:
        s += f' acp_seid={body[0] >> 2}'
    elif body:
        s += ' ' + body.hex()
    print(s)


def main(path, show_media=False):
    acl = {}          # (handle, sent) -> reassembly buffer
    chans = {}        # cid -> psm
    pending = {}      # (handle, ident) -> (psm, scid)
    avdtp_pairs = []  # [{local cid, remote cid}] in open order: signaling, then media
    media = {}
    t0 = None
    for ts, sent, h4, p in packets(path):
        t0 = ts if t0 is None else t0
        t = ts - t0
        arrow = '>' if sent else '<'
        if h4 == 4 and p and p[0] == 0x03 and len(p) > 11:
            print(f'{t:9.3f} {arrow} HCI Connection Complete status={p[2]} handle={p[3] | p[4] << 8}')
        if h4 == 4 and p and p[0] == 0x05 and len(p) > 5:
            print(f'{t:9.3f} {arrow} HCI Disconnection reason=0x{p[5]:02x}')
        if h4 != 2 or len(p) < 4:
            continue
        handle = (p[0] | p[1] << 8) & 0x0FFF
        pb = (p[1] >> 4) & 3
        key = (handle, sent)
        if pb in (0, 2):
            acl[key] = bytearray(p[4:])
        elif key in acl:
            acl[key] += p[4:]
        else:
            continue
        buf = acl[key]
        if len(buf) < 4 or len(buf) < 4 + (buf[0] | buf[1] << 8):
            continue
        ln, cid = buf[0] | buf[1] << 8, buf[2] | buf[3] << 8
        pdu = bytes(buf[4:4 + ln])
        del acl[key]

        if cid == 1:
            l2cap_signal(t, arrow, handle, pdu, pending, chans, avdtp_pairs)
            continue
        if chans.get(cid) != 0x19 or not pdu:
            continue
        idx = next((n for n, g in enumerate(avdtp_pairs) if cid in g), 0)
        if idx % 2 == 1:
            st = media.setdefault(cid, {'n': 0, 'bytes': 0, 'frames': 0, 'min': 1 << 30, 'max': 0,
                                        'first': [], 'pt': pdu[1] & 0x7F if len(pdu) > 1 else None})
            st['n'] += 1
            st['bytes'] += len(pdu)
            st['min'] = min(st['min'], len(pdu))
            st['max'] = max(st['max'], len(pdu))
            st['last_t'] = t
            st.setdefault('first_t', t)
            if len(pdu) > 13:
                st['frames'] += pdu[12] & 0x0F
                if len(st['first']) < 6:
                    st['first'].append((int.from_bytes(pdu[2:4], 'big'), int.from_bytes(pdu[4:8], 'big'),
                                        pdu[12] & 0x0F, len(pdu), pdu[13:17].hex()))
            if show_media and st['n'] <= 3:
                print(f'{t:9.3f} {arrow} MEDIA len={len(pdu)} {pdu[:20].hex()}')
            continue
        avdtp_signal(t, arrow, pdu)

    for cid, st in media.items():
        dur = max(st['last_t'] - st['first_t'], 1e-9)
        print(f'\nmedia cid=0x{cid:04x}: {st["n"]} packets in {dur:.1f} s, {st["bytes"] * 8 / dur / 1000:.0f} kbit/s, '
              f'size {st["min"]}..{st["max"]}, {st["frames"]} frames '
              f'({st["frames"] / max(st["n"], 1):.2f}/packet), RTP PT={st["pt"]}')
        for seq, ts, nf, size, head in st['first']:
            print(f'   seq={seq} ts={ts} frames={nf} len={size} payload={head}')


if __name__ == '__main__':
    main(sys.argv[1], '--media' in sys.argv)
