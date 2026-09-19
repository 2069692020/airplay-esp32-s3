"""Pre-auth RTSP ANNOUNCE with a malformed SDP, to exercise the new clamps.

handle_announce() calls parse_sdp() unconditionally whenever the request has a
body -- no pairing, no auth -- so this reaches the clamps from a raw LAN TCP
connection to port 7000.

Announcement A declares values that used to flow straight into buffer sizing:
  a=rtpmap:96 AppleLossless/44100/8   -> 8 channels (pipeline is stereo)
  a=fmtp:96 99999 0 200 30 0 100 8 2048 0 0 999999
          ^frame_len ^bit_depth    ^num_ch          ^sample_rate
Before the fix, channels=8 made audio_decoder_decode() tell the codec its
output buffer was capacity*8*2 bytes when the caller allocated capacity*2*2,
and audio_buffer_queue_chunk() memcpy'd 8-channel PCM into a slot sized for 2.

Announcement B is well formed and must produce no warnings -- that is the
"did we start falsely rejecting real senders" control.

A /ws/logs WebSocket is opened first (hand-rolled, stdlib only) so the device's
own log lines are captured live; the server evicts it after a few seconds, so
the capture window is deliberately short.
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
OUT = 'verify_rtsp_clamp.txt'
out = io.open(OUT, 'w', encoding='utf-8')


def log(s):
    out.write(s + '\n')
    out.flush()


def info():
    with urllib.request.urlopen('http://%s/api/system/info' % DEV,
                                timeout=10) as r:
        b = r.read()
    b.decode('utf-8')
    return json.loads(b.decode('utf-8'))['info']


def rtsp(sock, method, uri, body, cseq, ctype='application/sdp'):
    hdr = '%s %s RTSP/1.0\r\nCSeq: %d\r\n' % (method, uri, cseq)
    if body:
        hdr += 'Content-Type: %s\r\nContent-Length: %d\r\n' % (ctype, len(body))
    hdr += '\r\n'
    sock.sendall(hdr.encode() + (body or b''))
    sock.settimeout(5)
    try:
        return sock.recv(4096).decode('utf-8', 'replace').split('\r\n')[0]
    except socket.timeout:
        return '(no response)'


# ---------------------------------------------------------------- log capture
lines = []
stop = threading.Event()


def ws_capture():
    key = base64.b64encode(os.urandom(16)).decode()
    s = socket.create_connection((DEV, 80), timeout=10)
    s.sendall(('GET /ws/logs HTTP/1.1\r\nHost: %s\r\n'
               'Upgrade: websocket\r\nConnection: Upgrade\r\n'
               'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n'
               % (DEV, key)).encode())
    buf = b''
    while b'\r\n\r\n' not in buf:
        c = s.recv(4096)
        if not c:
            return
        buf += c
    head, buf = buf.split(b'\r\n\r\n', 1)
    lines.append('[handshake] ' + head.split(b'\r\n')[0].decode('utf-8', 'replace'))
    s.settimeout(1.0)
    while not stop.is_set():
        try:
            c = s.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            lines.append('[ws closed by server]')
            return
        if not c:
            lines.append('[ws eof]')
            return
        buf += c
        # server->client frames are unmasked
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
            payload = buf[off:off + ln]
            buf = buf[off + ln:]
            op = b1 & 0x0F
            if op == 1:
                for t in payload.decode('utf-8', 'replace').splitlines():
                    if t.strip():
                        lines.append(t)
            elif op == 8:
                lines.append('[ws close frame]')
                return


i0 = info()
log('before: uptime=%s reset_reason=%s heap=%s airplay_state=%s'
    % (i0['uptime_s'], i0['reset_reason'], i0['free_heap'], i0.get('airplay_state')))

th = threading.Thread(target=ws_capture, daemon=True)
th.start()
time.sleep(2.5)

s = socket.create_connection((DEV, 7000), timeout=8)
log('OPTIONS -> %s' % rtsp(s, 'OPTIONS', '*', b'', 1))

BAD = (b'v=0\r\n'
       b'o=- 1 1 IN IP4 192.0.2.9\r\n'
       b's=ClampTest\r\n'
       b'c=IN IP4 192.0.2.9\r\n'
       b't=0 0\r\n'
       b'm=audio 0 RTP/AVP 96\r\n'
       b'a=rtpmap:96 AppleLossless/44100/8\r\n'
       b'a=fmtp:96 99999 0 200 30 0 100 8 2048 0 0 999999\r\n')
log('ANNOUNCE (malformed: 8 ch, frame_len 99999, bit_depth 200, rate 999999)')
log('  -> %s' % rtsp(s, 'ANNOUNCE', 'rtsp://%s/announce' % DEV, BAD, 2))
time.sleep(1.0)

GOOD = (b'v=0\r\n'
        b'o=- 1 1 IN IP4 192.0.2.9\r\n'
        b's=ControlTest\r\n'
        b'c=IN IP4 192.0.2.9\r\n'
        b't=0 0\r\n'
        b'm=audio 0 RTP/AVP 96\r\n'
        b'a=rtpmap:96 AppleLossless/44100/2\r\n'
        b'a=fmtp:96 352 0 16 40 10 14 0 4096 0 0 44100\r\n')
log('ANNOUNCE (well formed: 2 ch, 352, 16 bit, 44100) -- must NOT warn')
log('  -> %s' % rtsp(s, 'ANNOUNCE', 'rtsp://%s/announce' % DEV, GOOD, 3))
time.sleep(1.5)

log('TEARDOWN -> %s' % rtsp(s, 'TEARDOWN', 'rtsp://%s/announce' % DEV, b'', 4))
s.close()
time.sleep(4)
stop.set()
time.sleep(1.5)

i1 = info()
log('')
log('after:  uptime=%s reset_reason=%s heap=%s airplay_state=%s playback_source=%s'
    % (i1['uptime_s'], i1['reset_reason'], i1['free_heap'],
       i1.get('airplay_state'), i1.get('playback_source')))
log('uptime %s -> %s : %s' % (i0['uptime_s'], i1['uptime_s'],
    'NO REBOOT (still climbing)' if i1['uptime_s'] > i0['uptime_s']
    else '*** REBOOTED ***'))

log('')
log('=== device log captured over /ws/logs (%d lines) ===' % len(lines))
for t in lines:
    log('  ' + t)
warn = [t for t in lines if ' W ' in t or ' E ' in t or 'W (' in t or 'E (' in t]
clamp = [t for t in lines if 'Unsupported channel count' in t
         or 'Implausible' in t or 'Configured codec' in t]
log('')
log('clamp warnings seen: %d' % len(clamp))
for t in clamp:
    log('  * ' + t)
log('all W/E lines: %d' % len(warn))
for t in warn:
    log('  ! ' + t)
out.close()
print('done, %d log lines' % len(lines))
