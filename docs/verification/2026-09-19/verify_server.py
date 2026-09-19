"""Local test server for verifying two web_radio fixes on the device.

Routes
  /radio/list.m3u  playlist with one RELATIVE entry (song.mp3) and one
                   ROOT-ABSOLUTE entry (/abs.mp3)
  /radio/song.mp3  what the relative entry must resolve to after the fix
  /song.mp3        what it resolved to BEFORE the fix (site root)
  /abs.mp3         the root-absolute entry, must be requested either way
  /slow.mp3        sends a few hundred bytes then holds the socket open,
                   which trips the device's 3 s stall watchdog

Every request path is appended to verify_http_server.log so the driver can
assert on what the device actually asked for.
"""
import http.server
import io
import threading
import time

LOG = 'verify_http_server.log'
_lock = threading.Lock()

MP3_JUNK = b'\xff\xfb\x90\x00' + bytes(range(256)) * 4


def silent_mp3(nbytes):
    """Syntactically valid MPEG-1 Layer III silence.

    Header FF FB 90 00 = MPEG1 / Layer III / no CRC / 128 kbps / 44.1 kHz /
    stereo / no padding, which makes the frame 144*128000/44100 = 417 bytes.
    All-zero frame bodies decode to silence.  The device consumes this at
    ~16 KB/s, so 300 KB keeps its 256 KB ring buffer non-empty for ~18 s --
    long enough for the 3 s stall watchdog at the bottom of its decode loop
    to fire.  (A short dribble instead drains the ring instantly, and the loop
    `continue`s past the watchdog, so only esp_http_client's own 10 s read
    timeout ever fires.)
    """
    frame = b'\xff\xfb\x90\x00' + b'\x00' * (417 - 4)
    return frame * (nbytes // len(frame) + 1)


SLOW_BODY = silent_mp3(300 * 1024)


def note(msg):
    with _lock:
        with io.open(LOG, 'a', encoding='utf-8') as f:
            f.write('%.3f %s\n' % (time.time(), msg))


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        note('REQ %s' % (fmt % args))

    def _send(self, body, ctype='audio/mpeg'):
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def do_GET(self):
        p = self.path.split('?')[0]
        note('GET %s' % p)
        if p == '/radio/list.m3u':
            pl = ('#EXTM3U\r\n'
                  '#EXTINF:12,Relative Entry\r\n'
                  'song.mp3\r\n'
                  '#EXTINF:12,Root Absolute Entry\r\n'
                  '/abs.mp3\r\n')
            self._send(pl.encode('utf-8'), 'audio/x-mpegurl')
        elif p == '/slow.mp3':
            # Send a full buffer's worth of decodable silence, announce more
            # than we will ever deliver, then hold the socket open.  Download
            # progress freezes while the device's ring stays non-empty, which
            # is exactly the condition its 3 s stall watchdog exists for.
            self.send_response(200)
            self.send_header('Content-Type', 'audio/mpeg')
            self.send_header('Content-Length', str(len(SLOW_BODY) * 10))
            self.end_headers()
            try:
                self.wfile.write(SLOW_BODY)
                self.wfile.flush()
                note('SLOW sent %d bytes, now holding' % len(SLOW_BODY))
                time.sleep(60)
            except OSError as e:
                note('SLOW write ended: %r' % (e,))
            note('SLOW released')
        else:
            self._send(MP3_JUNK)

    def do_HEAD(self):
        note('HEAD %s' % self.path)
        self.send_response(200)
        self.send_header('Content-Type', 'audio/mpeg')
        self.send_header('Content-Length', str(len(MP3_JUNK)))
        self.end_headers()


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == '__main__':
    io.open(LOG, 'w', encoding='utf-8').close()
    srv = Server(('0.0.0.0', 8899), H)
    note('server listening on 0.0.0.0:8899')
    srv.serve_forever()
