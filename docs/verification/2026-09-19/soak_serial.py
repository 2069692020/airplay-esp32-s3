"""Self-healing continuous serial capture.

Two things this has to survive, both specific to this rig:
  - The console is USB-Serial-JTAG, so a chip reset re-enumerates the USB
    device and the open handle can go quiet while looking fine. Reopen
    periodically. Reopening is only safe because dtr/rts are held False across
    open() and close() -- verified on this board, an open/close pair with those
    lines low does NOT reset the chip (uptime keeps climbing).
  - The Windows console is GBK and raises UnicodeEncodeError mid-capture, and
    readline() drops bytes on USB-CDC, so everything goes to a UTF-8 file read
    in blocks.

An idle device legitimately prints nothing for minutes, so the reopen is
time-based and unconditional rather than error-triggered.
"""
import io
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 8100
OUTPATH = sys.argv[3] if len(sys.argv) > 3 else 'soak_serial.log'
REOPEN_EVERY = int(sys.argv[4]) if len(sys.argv) > 4 else 120

out = io.open(OUTPATH, 'a', encoding='utf-8', newline='\n')


def stamp():
    return time.strftime('%H:%M:%S')


def wline(s):
    out.write('[%s] %s\n' % (stamp(), s))
    out.flush()


def open_port():
    sp = serial.Serial()
    sp.port = PORT
    sp.baudrate = 115200
    sp.dtr = False
    sp.rts = False
    sp.timeout = 1.0
    sp.open()
    # belt and braces: some backends apply the lines on open()
    sp.dtr = False
    sp.rts = False
    return sp


wline('=== capture start port=%s for %ds, reopen every %ds ==='
      % (PORT, SECONDS, REOPEN_EVERY))

sp = open_port()
wline('port open (dtr/rts held low)')

partial = ''
deadline = time.time() + SECONDS
last_rx = time.time()
last_beat = time.time()
nbytes = 0
resets = 0

while time.time() < deadline:
    try:
        chunk = sp.read(sp.in_waiting or 1)
        if chunk:
            nbytes += len(chunk)
            last_rx = time.time()
            buf = partial + chunk.decode('utf-8', 'replace')
            partial = ''
            lines = buf.split('\n')
            partial = lines.pop()
            for ln in lines:
                ln = ln.rstrip('\r')
                if ln.strip():
                    out.write('[%s] %s\n' % (stamp(), ln))
            out.flush()
    except Exception as e:
        wline('!!! read error %r -- reopening' % (e,))
        try:
            sp.dtr = False
            sp.rts = False
            sp.close()
        except Exception:
            pass
        time.sleep(1.0)
        try:
            sp = open_port()
            resets += 1
            wline('reopened (%d)' % resets)
        except Exception as e2:
            wline('!!! reopen failed %r, retrying in 5s' % (e2,))
            time.sleep(5.0)
        continue

    # the handle can look alive but be mute after USB re-enumeration
    if time.time() - last_rx > REOPEN_EVERY:
        try:
            sp.dtr = False
            sp.rts = False
            sp.close()
        except Exception:
            pass
        time.sleep(0.4)
        try:
            sp = open_port()
            resets += 1
            wline('-- quiet for %ds, reopened (%d) --'
                  % (REOPEN_EVERY, resets))
        except Exception as e:
            wline('!!! reopen during quiet period failed %r' % (e,))
        last_rx = time.time()

    if time.time() - last_beat > 600:
        wline('-- heartbeat: %d bytes, %d reopens --' % (nbytes, resets))
        last_beat = time.time()

try:
    sp.dtr = False
    sp.rts = False
    sp.close()
except Exception:
    pass
wline('=== capture end, %d bytes, %d reopens ===' % (nbytes, resets))
out.close()
