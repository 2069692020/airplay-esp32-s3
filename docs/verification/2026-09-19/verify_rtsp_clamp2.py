"""One capture window, both the malformed and the well-formed ANNOUNCE.

The previous attempt claimed "0 warnings for the well-formed SDP" from a
/ws/logs capture whose lines were all timestamped ~172 s while the requests
were sent at ~222 s -- i.e. the ring buffer served stale content and proved
nothing.  This version sends both announcements inside a single capture and
prints every captured line with its device timestamp, so it is checkable that
both requests landed inside the window.
"""
import base64
import io
import json
import os
import socket
import struct
import threading
import time
import urllib.request

DEV = '192.0.2.5'
OUT = 'verify_rtsp_clamp2.txt'
out = io.open(OUT, 'w', encoding='utf-8')
lines = []
stop = threading.Event()


def log(s):
    out.write(s + '\n')
    out.flush()


def info():
    with urllib.request.urlopen('http://%s/api/system/info' % DEV,
                                timeout=10) as r:
        b = r.read()
    b.decode('utf-8')
    return json.loads(b.decode('utf-8'))['info']


def ws():
    key = base64.b64encode(os.urandom(16)).decode()
    s = socket.create_connection((DEV, 80), timeout=10)
    s.sendall(('GET /ws/logs HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n'
               'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
               'Sec-WebSocket-Version: 13\r\n\r\n' % (DEV, key)).encode())
    buf = b''
    while b'\r\n\r\n' not in buf:
        c = s.recv(4096)
        if not c:
            return
        buf += c
    _, buf = buf.split(b'\r\n\r\n', 1)
    s.settimeout(1.0)
    while not stop.is_set():
        try:
            c = s.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            return
        if not c:
            return
        buf += c
        while len(buf) >= 2:
            b1, b2 = buf[0], buf[1]
            ln = b2 & 0x7F
            off = 2
            if ln == 126:
                if len(buf) < 4:
                    break
                ln = struct.unpack('>H', buf[2:4])[0]
                off = 4
            elif ln == 127:
                if len(buf) < 10:
                    break
                ln = struct.unpack('>Q', buf[2:10])[0]
                off = 10
            if len(buf) < off + ln:
                break
            pay = buf[off:off + ln]
            buf = buf[off + ln:]
            if (b1 & 0x0F) == 1:
                for t in pay.decode('utf-8', 'replace').splitlines():
                    if t.strip():
                        lines.append(t)
            elif (b1 & 0x0F) == 8:
                return


def rtsp(s, m, u, b, c):
    h = '%s %s RTSP/1.0\r\nCSeq: %d\r\n' % (m, u, c)
    if b:
        h += 'Content-Type: application/sdp\r\nContent-Length: %d\r\n' % len(b)
    h += '\r\n'
    s.sendall(h.encode() + (b or b''))
    s.settimeout(5)
    try:
        return s.recv(4096).decode('utf-8', 'replace').split('\r\n')[0]
    except socket.timeout:
        return '(no response)'


def sdp(ch, frame_len, bit_depth, num_ch, rate, name):
    return ('v=0\r\no=- 1 1 IN IP4 192.0.2.9\r\ns=%s\r\n'
            'c=IN IP4 192.0.2.9\r\nt=0 0\r\nm=audio 0 RTP/AVP 96\r\n'
            'a=rtpmap:96 AppleLossless/44100/%d\r\n'
            'a=fmtp:96 %d 0 %d 40 10 14 %d 4096 0 0 %d\r\n'
            % (name, ch, frame_len, bit_depth, num_ch, rate)).encode()


i0 = info()
log('capture start: device uptime=%s' % i0['uptime_s'])
th = threading.Thread(target=ws, daemon=True)
th.start()
time.sleep(2.5)

s = socket.create_connection((DEV, 7000), timeout=8)
log('marker A: OPTIONS -> %s' % rtsp(s, 'OPTIONS', '*', b'', 1))
time.sleep(0.6)

log('marker B: MALFORMED ANNOUNCE (8ch / frame_len 99999 / 200bit / 999999Hz)'
    ' -> %s' % rtsp(s, 'ANNOUNCE', 'rtsp://%s/a' % DEV,
                    sdp(8, 99999, 200, 8, 999999, 'Malformed'), 2))
time.sleep(1.2)

log('marker C: WELL-FORMED stereo ANNOUNCE (2ch / 352 / 16bit / 44100)'
    ' -> %s' % rtsp(s, 'ANNOUNCE', 'rtsp://%s/a' % DEV,
                    sdp(2, 352, 16, 2, 44100, 'RealStereo'), 3))
time.sleep(1.2)

log('marker D: legitimate MONO ANNOUNCE (1ch / 352 / 16bit / 44100)'
    ' -> %s' % rtsp(s, 'ANNOUNCE', 'rtsp://%s/a' % DEV,
                    sdp(1, 352, 16, 1, 44100, 'RealMono'), 4))
time.sleep(1.2)

log('marker E: TEARDOWN -> %s' % rtsp(s, 'TEARDOWN', 'rtsp://%s/a' % DEV,
                                      b'', 5))
s.close()
time.sleep(3)
stop.set()
time.sleep(1.5)

i1 = info()
log('capture end: device uptime=%s heap=%s' % (i1['uptime_s'], i1['free_heap']))
log('reboot? %s' % ('NO - uptime still climbing'
                    if i1['uptime_s'] > i0['uptime_s'] else '*** YES ***'))
log('')
log('=== captured lines mentioning rtsp_handlers / rtsp_server (%d total '
    'captured) ===' % len(lines))
rel = [t for t in lines
       if 'rtsp_handlers' in t or 'rtsp_server' in t or 'New client' in t]
for t in rel:
    log('  ' + t)
log('')
warn = [t for t in rel if t.strip().startswith('W (') or t.strip().startswith('E (')]
log('clamp warnings inside this window: %d' % len(warn))
for t in warn:
    log('  * ' + t)
out.close()
print('captured %d lines, %d relevant, %d warnings' % (len(lines), len(rel), len(warn)))
