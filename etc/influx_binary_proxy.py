#!/usr/bin/env python3
"""UDP proxy: decode the firmware's binary symbol-table wire protocol back to
InfluxDB line protocol and forward it. Pair with a WITH_BINARY_TELE build.

Datagram = <compressor_id:1B> <payload>.  id 0 = raw, 1 = tamp.
payload  = (<varint len> <frame>)*  ; frame[0] = FrameT (1=data, 2=table).
  table : (<SID varint> <name\\0> <DT byte>)*  <0-SID>     (defines symbols)
  data  : <SID(meas)> (<SID tagK><SID tagV>)* 0
                       (<SID fieldK><raw LE value>)* 0  <ts_ms varint>
Field datatype is per-symbol (from the table); SID 0 reserved as terminator.
A symbol table is kept per source device (UDP source ip). See sym_line_protocol.h.

Usage:
  influx_binary_proxy.py --listen 0.0.0.0:8086              # decode + print
  influx_binary_proxy.py --listen :8086 \\
      --influx https://influx.fabi.me:8086 --db open_pe \\
      --user openpe --password ... --precision ms           # decode + forward
  influx_binary_proxy.py --test path/to/blob.bin            # offline self-test
"""
import argparse, socket, struct, sys

try:
    import tamp
except ImportError:
    tamp = None

# WireDT -> (struct fmt, width). Str(0) has no value (appears only as a SID).
DT = {1: ('?', 1), 2: ('<b', 1), 3: ('<B', 1), 4: ('<h', 2), 5: ('<H', 2),
      6: ('<i', 4), 7: ('<I', 4), 8: ('<e', 2), 9: ('<f', 4), 10: ('<d', 8)}
DT_STR = 0


class Reader:
    def __init__(self, b): self.b, self.i = b, 0
    def varint(self):
        v, s = 0, 0
        while True:
            x = self.b[self.i]; self.i += 1
            v |= (x & 0x7F) << s
            if not (x & 0x80): return v
            s += 7
    def cstr(self):
        j = self.b.index(0, self.i); s = self.b[self.i:j].decode(); self.i = j + 1; return s
    def byte(self):
        x = self.b[self.i]; self.i += 1; return x
    def take(self, n):
        x = self.b[self.i:self.i + n]; self.i += n; return x
    def eof(self): return self.i >= len(self.b)


def fmt_field(dt, raw):
    fmt, _ = DT[dt]
    v = struct.unpack(fmt, raw)[0]
    if dt == 1:  return 'true' if v else 'false'         # bool
    if dt <= 7:  return f'{v}i'                            # int -> influx integer
    return f'{v:.6g}'                                      # float


def decode_datagram(dg, tables, src):
    """Yield influx line-protocol strings from one datagram. tables[src] persists."""
    if not dg:
        return
    cid, payload = dg[0], dg[1:]
    if cid == 1:
        if tamp is None:
            raise RuntimeError("datagram is tamp-compressed but 'tamp' is not installed (pip install tamp)")
        payload = tamp.decompress(payload)
    elif cid != 0:
        raise ValueError(f"unknown compressor id {cid}")
    tab = tables.setdefault(src, {})        # SID -> (name, dt)
    r = Reader(payload)
    while not r.eof():
        flen = r.varint()
        frame = Reader(r.take(flen))
        ft = frame.byte()
        if ft == 2:                          # table
            while True:
                sid = frame.varint()
                if sid == 0: break
                tab[sid] = (frame.cstr(), frame.byte())
        elif ft == 1:                        # data
            def name(sid): return tab[sid][0]
            meas = name(frame.varint())
            tags = []
            while frame.b[frame.i] != 0:
                k = name(frame.varint()); v = name(frame.varint()); tags.append(f'{k}={v}')
            frame.i += 1                     # tag terminator
            fields = []
            while frame.b[frame.i] != 0:
                sid = frame.varint(); _, dt = tab[sid]
                fields.append(f'{name(sid)}={fmt_field(dt, frame.take(DT[dt][1]))}')
            frame.i += 1                     # field terminator
            ts = frame.varint()
            line = meas + (',' + ','.join(tags) if tags else '') + ' ' + ','.join(fields) + ' ' + str(ts)
            yield line
        else:
            raise ValueError(f"unknown frame type {ft}")


def self_test(path):
    blob = open(path, 'rb').read()
    print(f"raw frames: {len(blob)} B")
    for cid, label in ((0, 'raw'), (1, 'tamp')):
        if cid == 1 and tamp is None:
            print("(skip tamp: not installed)"); continue
        payload = tamp.compress(blob) if cid == 1 else blob
        dg = bytes([cid]) + payload
        lines = list(decode_datagram(dg, {}, ('test', 0)))
        print(f"\nid={cid} ({label}): datagram {len(dg)} B -> {len(lines)} points")
        for ln in lines[:3]:
            print("  " + ln)


def serve(args):
    host, _, port = args.listen.rpartition(':')
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host or '0.0.0.0', int(port)))
    print(f"listening udp {host or '0.0.0.0'}:{port}", file=sys.stderr)
    fwd = None
    if args.influx:
        import urllib.request, base64
        url = f"{args.influx}/write?db={args.db}&precision={args.precision}"
        hdr = {}
        if args.user:
            hdr['Authorization'] = 'Basic ' + base64.b64encode(f'{args.user}:{args.password}'.encode()).decode()
        def fwd(lines):
            data = '\n'.join(lines).encode()
            req = urllib.request.Request(url, data=data, headers=hdr, method='POST')
            try: urllib.request.urlopen(req, timeout=5).read()
            except Exception as e: print(f"forward error: {e}", file=sys.stderr)
    tables = {}
    while True:
        dg, src = sock.recvfrom(65535)
        try:
            lines = list(decode_datagram(dg, tables, src))   # full (ip,port): NAT shares ip, distinct ports
        except Exception as e:
            print(f"decode error from {src}: {e}", file=sys.stderr); continue
        if fwd: fwd(lines)
        else:
            for ln in lines: print(ln)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--listen', default='0.0.0.0:8086')
    ap.add_argument('--influx'); ap.add_argument('--db', default='open_pe')
    ap.add_argument('--user'); ap.add_argument('--password', default='')
    ap.add_argument('--precision', default='ms')
    ap.add_argument('--test', metavar='BLOB')
    a = ap.parse_args()
    if a.test: self_test(a.test)
    else: serve(a)
