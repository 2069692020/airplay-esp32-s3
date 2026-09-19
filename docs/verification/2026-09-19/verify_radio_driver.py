"""Drive the device against verify_server.py and assert on what it requested.

Usage: python verify_radio_driver.py <label>
Writes verify_radio_<label>.txt.

TEST 1 (m3u relative resolution) -- play a playlist whose entries are
`song.mp3` (relative) and `/abs.mp3` (root-absolute).  Before the fix the
relative entry resolved against the site root, so the device asked for
/song.mp3; after the fix it must ask for /radio/song.mp3.

TEST 2 (stall self-heal) -- serve ~300 KB of decodable silence then hold the
socket.  The device's ring stays non-empty while download progress freezes, so
its 3 s stall watchdog fires: it sets s_track_failed AND aborts the download.
Before the fix `aborted` was taken from s_abort_http alone, so the error-stop
was skipped and `if (aborted) continue` replayed the SAME track forever --
visible here as repeated requests for /slow.mp3.  After the fix there must be
exactly one, and the final state must be `error` (not `idle`, which the
"playlist finished" branch used to clobber it with, hiding the message from
the UI).
"""
import io
import json
import sys
import threading
import time
import urllib.request

import verify_server as vs

DEV = 'http://192.0.2.5'
HOST = 'http://192.0.2.9:8899'
LABEL = sys.argv[1] if len(sys.argv) > 1 else 'run'
OUT = 'verify_radio_%s.txt' % LABEL

_requests = []
_orig_note = vs.note


def tee(msg):
    _orig_note(msg)
    if msg.startswith('GET '):
        _requests.append((time.time(), msg[4:]))


vs.note = tee

out = io.open(OUT, 'w', encoding='utf-8')


def log(s):
    out.write(s + '\n')
    out.flush()
    print(s.encode('ascii', 'replace').decode('ascii'))


def raw(p, method='GET', body=None):
    rq = urllib.request.Request(
        DEV + p, data=body, method=method,
        headers={'Content-Type': 'application/json'} if body else {})
    with urllib.request.urlopen(rq, timeout=25) as r:
        return r.read()


def getj(p):
    b = raw(p)
    b.decode('utf-8')
    return json.loads(b.decode('utf-8'))


def post(p, o):
    b = raw(p, 'POST', json.dumps(o).encode())
    b.decode('utf-8')
    return json.loads(b.decode('utf-8'))


srv = vs.Server(('0.0.0.0', 8899), vs.H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
log('[%s] test server up on 0.0.0.0:8899' % LABEL)

info = None
for _ in range(60):
    try:
        info = getj('/api/system/info')['info']
        break
    except Exception:
        time.sleep(2)
if info is None:
    raise SystemExit('device not reachable')
log('device: uptime=%s reset_reason=%s heap=%s'
    % (info.get('uptime_s'), info.get('reset_reason'), info.get('free_heap')))

# ---------------------------------------------------------------- test 1
log('')
log('=== TEST 1: m3u relative entry resolution ===')
_requests.clear()
log('play: %s' % json.dumps(post('/api/music/play',
                                {'url': HOST + '/radio/list.m3u'}),
                           ensure_ascii=False))
for i in range(10):
    time.sleep(1)
    try:
        st = getj('/api/music')
        log('  t+%2ds state=%-10s count=%s index=%s cur_url=%s'
            % (i + 1, st.get('state'), st.get('count'), st.get('index'),
               st.get('cur_url')))
    except Exception as e:
        log('  poll error %r' % e)
post('/api/music/stop', {})
time.sleep(1.5)
asked = [p for _, p in _requests]
log('requested, in order: %s' % asked)
if '/radio/song.mp3' in asked:
    v1 = 'FIXED - relative entry resolved against the playlist directory'
elif '/song.mp3' in asked:
    v1 = 'BROKEN - relative entry resolved against the site root'
else:
    v1 = 'INCONCLUSIVE - the device never reached the test server'
log('VERDICT 1: %s' % v1)
log('VERDICT 1b: root-absolute entry %s'
    % ('requested /abs.mp3 (correct)' if '/abs.mp3' in asked
       else 'NOT requested'))

# ---------------------------------------------------------------- test 2
log('')
log('=== TEST 2: stall self-heal must stop, not replay forever ===')
_requests.clear()
log('play: %s' % json.dumps(post('/api/music/play', {'url': HOST + '/slow.mp3'}),
                           ensure_ascii=False))
last = None
for i in range(45):
    time.sleep(1)
    try:
        st = getj('/api/music')
        n = sum(1 for _, p in _requests if p == '/slow.mp3')
        last = (st.get('state'), st.get('error'), n)
        log('  t+%2ds state=%-10s error=%-42s slow_requests=%d'
            % (i + 1, st.get('state'), st.get('error') or '-', n))
    except Exception as e:
        log('  t+%2ds poll error %r' % (i + 1, e))
post('/api/music/stop', {})
time.sleep(1)

n_slow = sum(1 for _, p in _requests if p == '/slow.mp3')
state = last[0] if last else '?'
err = last[1] if last else ''
log('total /slow.mp3 requests over 45 s: %d ; final state=%s error=%s'
    % (n_slow, state, err))
if n_slow >= 2:
    v2 = ('BROKEN - replayed the same track %d times in 45 s '
          '(infinite retry loop)' % n_slow)
elif state == 'error':
    v2 = 'FIXED - one request, stopped, and the error is visible to the UI'
elif state == 'idle' and err:
    v2 = ('PARTIAL - one request and it stopped, but state=idle hides the '
          'error message from the web UI')
else:
    v2 = 'INCONCLUSIVE - state=%s error=%s requests=%d' % (state, err, n_slow)
log('VERDICT 2: %s' % v2)

info = getj('/api/system/info')['info']
log('')
log('device after both tests: uptime=%s heap=%s playback_source=%s '
    'airplay_state=%s reset_reason=%s'
    % (info.get('uptime_s'), info.get('free_heap'),
       info.get('playback_source'), info.get('airplay_state'),
       info.get('reset_reason')))
out.close()
