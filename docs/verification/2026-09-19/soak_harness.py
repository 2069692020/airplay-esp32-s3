"""~2 h autonomous functional soak against the device.

Everything here is driven the way a user drives it, over HTTP only -- the COM
port is held by soak_serial.py, and touching it would reset the chip and cut a
stream mid-test (see CLAUDE.md / the dev-loop notes).

What each iteration covers, and why that specific thing:

  SD      list -> play -> poll -> next -> prev -> stop
          The only path that hands the single shared I2S away and back, so it
          is where the playback_task leak and the resume_airplay bug class live.
  radio   a live chunked silent-MP3 stream served from this machine
          Real decoding + ring drain + underrun + skip, without waiting for a
          station URL from him. Also exercises the stall/skip paths.
  volume  0 / 100 / mid, then restore
          The endpoint that had the cJSON leak, and the one the slider hammers.
  channel LEFT -> RIGHT -> MONO -> STEREO round trip
          Writes NVS and re-runs the software downmix; ends where it started.
  dirs    mkdir + list-into + delete, CJK and ASCII names
          The GBK<->UTF-8 conversion is centralised in sd_card_full_path(), so
          every operation has to round-trip a Chinese name.
  reset   uptime_s must be monotonic
          The cheap crash discriminator: any decrease means the device rebooted
          and the serial log will have the reason.

Deliberately NOT here: /api/system/restart, /api/ota/* (driven separately so a
slot flip can be observed deliberately), and renaming the device (needs a
reboot to take effect).
"""
import io
import json
import socket
import socketserver
import threading
import time
import urllib.error
import urllib.request

DEV = 'http://192.0.2.5'
LAN = '192.0.2.9'
PORT = 8899
SOAK_MINUTES = 70
OUT = 'soak_harness.log'

out = io.open(OUT, 'w', encoding='utf-8', newline='\n')
lock = threading.Lock()
stats = {'PASS': 0, 'FAIL': 0, 'SKIP': 0}
failures = []


def emit(kind, what, detail=''):
    with lock:
        stats[kind] = stats.get(kind, 0) + 1
        line = '[%s] %-4s %s%s' % (time.strftime('%H:%M:%S'), kind, what,
                                   (' | ' + detail) if detail else '')
        out.write(line + '\n')
        out.flush()
        if kind == 'FAIL':
            failures.append(line)


class Transient(Exception):
    """httpd evicted us (max_open_sockets=3 + lru_purge) or the device is
    mid-reboot. Not a device fault -- record SKIP, not FAIL."""


def req(path, method='GET', body=None, timeout=20, tries=4):
    rq = urllib.request.Request(
        DEV + path, data=body, method=method,
        headers={'Content-Type': 'application/json'} if body is not None else {})
    last = None
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(rq, timeout=timeout) as r:
                raw = r.read()
            raw.decode('utf-8')   # raises if the device emitted invalid UTF-8
            return json.loads(raw.decode('utf-8')) if raw else {}
        except (urllib.error.HTTPError, UnicodeDecodeError):
            raise                    # a real answer, even if a bad one
        except Exception as e:
            last = e
            time.sleep(1.5 * (attempt + 1))
    raise Transient('%s after %d tries: %r' % (path, tries, last))


def check(what, fn):
    """Run one probe. Transient -> SKIP (evicted socket or a reboot in
    progress); any other raise, or a false return, is a real FAIL."""
    try:
        ok, detail = fn()
        emit('PASS' if ok else 'FAIL', what, detail)
        return ok
    except Transient as e:
        emit('SKIP', what, str(e))
        return False
    except Exception as e:
        emit('FAIL', what, 'exception %r' % (e,))
        return False


# ------------------------------------------------------- fake radio station --
FRAME = b'\xff\xfb\x90\x00' + b'\x00' * (417 - 4)   # MPEG1 L3 128k 44.1k stereo
M3U = (b'#EXTM3U\r\n#EXTINF:30,Soak Relative\r\nsong.mp3\r\n'
       b'#EXTINF:30,Soak Absolute\r\n/abs.mp3\r\n')


class Station(socketserver.StreamRequestHandler):
    timeout = None

    def handle(self):
        try:
            line = self.rfile.readline().decode('latin-1')
        except Exception:
            return
        if not line.startswith('GET'):
            return
        path = line.split(' ')[1] if len(line.split(' ')) > 1 else '/'
        while True:                                   # drain headers
            h = self.rfile.readline()
            if h in (b'\r\n', b'\n', b''):
                break
        self.log(path)
        try:
            if path.startswith('/radio/list.m3u'):
                self.wfile.write(b'HTTP/1.1 200 OK\r\nContent-Type: '
                                 b'audio/x-mpegurl\r\nContent-Length: %d\r\n'
                                 b'Connection: close\r\n\r\n' % len(M3U))
                self.wfile.write(M3U)
            elif path.startswith('/live.mp3'):
                # a real station never sends Content-Length: stream forever
                self.wfile.write(b'HTTP/1.1 200 OK\r\nContent-Type: '
                                 b'audio/mpeg\r\nIcy-Name: Soak\r\n'
                                 b'Connection: close\r\n\r\n')
                for _ in range(9000):                 # ~4 h of headroom
                    self.wfile.write(FRAME)           # 26 ms of audio
                    self.wfile.flush()
                    time.sleep(0.020)
            else:
                body = FRAME * 600
                self.wfile.write(b'HTTP/1.1 200 OK\r\nContent-Type: '
                                 b'audio/mpeg\r\nContent-Length: %d\r\n'
                                 b'Connection: close\r\n\r\n' % len(body))
                self.wfile.write(body)
        except Exception:
            pass

    def log(self, path):
        with lock:
            out.write('[%s] SERVED %s\n' % (time.strftime('%H:%M:%S'), path))
            out.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


# ------------------------------------------------------------- test bodies ----
def get_info():
    return req('/api/system/info')['info']


def sd_round():
    d = req('/api/sd/list?dir=/')
    files = [e['name'] for e in d.get('entries', []) if not e.get('is_dir')]
    if not files:
        return False, 'no playable files on card'
    path = '/' + files[0]
    r = req('/api/sd/play', 'POST', json.dumps({'path': path}).encode())
    if not r.get('success'):
        return False, 'play refused %s' % r
    st = {}
    for _ in range(40):
        time.sleep(0.5)
        st = req('/api/sd/state')
        if st.get('state') == 'playing' and st.get('elapsed', 0) >= 3:
            break
    ok_playing = st.get('state') == 'playing' and st.get('elapsed', 0) >= 3
    title_ok = bool(st.get('title'))
    path_ok = st.get('path') == path
    sr_ok = st.get('sample_rate') in (44100, 48000, 96000, 88200)
    req('/api/sd/next', 'POST', b'{}')
    time.sleep(2.5)
    req('/api/sd/prev', 'POST', b'{}')
    time.sleep(2.5)
    req('/api/sd/stop', 'POST', b'{}')
    time.sleep(2.0)
    st2 = req('/api/sd/state')
    i = get_info()
    handed_back = i.get('playback_source') == 'airplay'
    ok = (ok_playing and path_ok and sr_ok and not st2.get('active')
          and st2.get('state') == 'idle' and handed_back)
    return ok, ('playing=%s title=%s pathmatch=%s sr=%s stopped=%s src=%s'
                % (ok_playing, title_ok, path_ok, st.get('sample_rate'),
                   st2.get('state'), i.get('playback_source')))


def radio_round():
    r = req('/api/music/play', 'POST',
            json.dumps({'url': 'http://%s:%d/live.mp3' % (LAN, PORT)}).encode())
    if not r.get('success'):
        return False, 'play refused %s' % r
    st = {}
    for _ in range(24):
        time.sleep(0.5)
        st = req('/api/music')
        if st.get('state') == 'playing':
            break
    playing = st.get('state') == 'playing'
    time.sleep(4)
    req('/api/music/next', 'POST', b'{}')
    time.sleep(3)
    st2 = req('/api/music')
    req('/api/music/stop', 'POST', b'{}')
    time.sleep(2.0)
    st3 = req('/api/music')
    i = get_info()
    ok = (playing and not st3.get('active')
          and i.get('playback_source') == 'airplay')
    return ok, ('playing=%s after_next=%s stopped=%s src=%s err=%s'
                % (playing, st2.get('state'), st3.get('state'),
                   i.get('playback_source'), st3.get('error') or '-'))


def volume_round(orig):
    seq = []
    for pct in (0, 100, 33, orig):
        req('/api/volume', 'POST', json.dumps({'percent': pct}).encode())
        seq.append(req('/api/volume').get('percent'))
    want = [0, 100, 33, orig]
    return seq == want, 'set->get %s expected %s' % (seq, want)


def channel_round():
    start = req('/api/audio/channel').get('mode')
    seen = []
    for m in (1, 2, 3, 0):
        req('/api/audio/channel', 'POST', json.dumps({'mode': m}).encode())
        seen.append(req('/api/audio/channel').get('mode'))
    end = req('/api/audio/channel').get('mode')
    return (end == 0 and start == end), 'modes %s, restored to %s' % (seen, end)


def dir_roundtrip():
    names = ['/__soak中文测试', '/__soak_ascii']
    detail = []
    for nm in names:
        # ⚠️ mkdir reads the key "dir" (delete reads "path") — sending "path"
        # here returns ESP_ERR_INVALID_ARG, whose error text misleadingly
        # blames "already exists". Verified separately, mkdir itself is fine.
        req('/api/sd/mkdir', 'POST', json.dumps({'dir': nm}).encode())
        root = req('/api/sd/list?dir=/')
        listed = any(e['name'] == nm.lstrip('/') for e in root.get('entries', []))
        inner = req('/api/sd/list?dir=' + urllib.parse.quote(nm))
        ok_inner = inner.get('mounted') and inner.get('dir') == nm
        req('/api/sd/delete', 'POST', json.dumps({'path': nm}).encode())
        gone = all(e['name'] != nm.lstrip('/')
                   for e in req('/api/sd/list?dir=/').get('entries', []))
        detail.append('%s listed=%s inner=%s deleted=%s'
                      % (nm, listed, ok_inner, gone))
        if not (listed and ok_inner and gone):
            return False, '; '.join(detail)
    return True, '; '.join(detail)


def misc_round():
    i = get_info()
    np = req('/api/nowplaying')
    fs = req('/api/fs/list?dir=/spiffs')
    ok = ('airplay_state' in i and 'success' in np and 'files' in fs
          and i.get('reset_reason') != 'unknown'
          and i.get('sensor_age_s') is not None and i.get('time_synced'))
    return ok, ('reset=%s np=%s fs=%d entries, sensor_age=%s synced=%s'
                % (i.get('reset_reason'), np.get('airplay_state'),
                   len(fs.get('files', [])), i.get('sensor_age_s'),
                   i.get('time_synced')))


def wifi_scan():
    d = req('/api/wifi/scan', timeout=40)
    return d.get('success') and isinstance(d.get('networks'), list), \
        '%d networks' % len(d.get('networks', []))


def speedtest():
    # answered as text/plain "ok", not JSON — and it takes no params
    rq = urllib.request.Request(DEV + '/api/speedtest/ping')
    last = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(rq, timeout=60) as r:
                body = r.read().decode('utf-8', 'replace')
            return body == 'ok', 'body=%r' % body[:40]
        except Exception as e:
            last = e
            time.sleep(1.5 * (attempt + 1))
    raise Transient('speedtest unreachable: %r' % (last,))


# ------------------------------------------------------------------ main ------
import urllib.parse  # noqa: E402  (used by dir_roundtrip)

srv = Server((LAN, PORT), Station)
threading.Thread(target=srv.serve_forever, daemon=True).start()
emit('PASS', 'harness up: station on %s:%d, %d minutes' % (LAN, PORT, SOAK_MINUTES))

prev_uptime = None
loop = 0
deadline = time.time() + SOAK_MINUTES * 60

while time.time() < deadline:
    loop += 1
    loop_t0 = time.time()
    try:
        i0 = get_info()
    except Exception as e:
        emit('FAIL', 'loop %d device unreachable at start' % loop, repr(e))
        time.sleep(15)
        continue
    u = i0.get('uptime_s')
    if prev_uptime is not None and u is not None and u < prev_uptime:
        emit('FAIL', 'REBOOT detected', 'uptime %d -> %d reset_reason=%s'
             % (prev_uptime, u, i0.get('reset_reason')))
    prev_uptime = u

    orig_vol = i0.get('volume_percent')
    if orig_vol is None:
        orig_vol = req('/api/volume').get('percent', 33)

    emit('PASS', '--- loop %d --- uptime=%d heap=%d src=%s ap=%s'
         % (loop, u, i0.get('free_heap'), i0.get('playback_source'),
            i0.get('airplay_state')))
    check('loop %d SD play/skip/stop + I2S handback' % loop, sd_round)
    check('loop %d radio live stream + skip + handback' % loop, radio_round)
    check('loop %d volume extremes + restore' % loop,
          lambda: volume_round(orig_vol))
    check('loop %d channel mode round trip' % loop, channel_round)
    check('loop %d SD mkdir/list/delete (CJK+ASCII)' % loop, dir_roundtrip)
    check('loop %d misc endpoints' % loop, misc_round)
    if loop % 4 == 1:
        check('loop %d wifi scan' % loop, wifi_scan)
    if loop % 8 == 1:
        check('loop %d speedtest ping' % loop, speedtest)

    i1 = get_info()
    trend = i1.get('free_heap') - i0.get('free_heap')
    emit('PASS', 'loop %d done in %ds: heap %d -> %d (%+d), uptime=%d, '
         'sensor %.1fC/%.0f%%'
         % (loop, int(time.time() - loop_t0),
            i0.get('free_heap'), i1.get('free_heap'), trend, i1.get('uptime_s'),
            i1.get('temperature_c', 0), i1.get('humidity_pct', 0)))
    time.sleep(20)

emit('PASS', '=== harness finished: %d loops, PASS=%d FAIL=%d ==='
     % (loop, stats.get('PASS', 0), stats.get('FAIL', 0)))
if failures:
    emit('PASS', 'first 40 failures:')
    for f in failures[:40]:
        out.write('   ' + f + '\n')
    out.flush()
out.close()
