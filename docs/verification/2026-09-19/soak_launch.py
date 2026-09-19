"""Spawn the two long-running soak jobs fully detached from this shell.

The tool's own background timeout is 10 minutes, so anything meant to run for
two hours must not be a child of the shell that launched it. DETACHED_PROCESS +
CREATE_NEW_PROCESS_GROUP gives a process that outlives this one, and the jobs
write their own logs.
"""
import subprocess
import sys
import time

DETACHED = 0x00000008 | 0x00000200  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP

jobs = [
    ('serial', ['python', 'soak_serial.py', 'COM3', '8400',
                'soak_serial_main.log', '150']),
    ('harness', ['python', 'soak_harness.py']),
]

for name, argv in jobs:
    p = subprocess.Popen(
        argv, cwd=r'<PROJECT>',
        creationflags=DETACHED,
        stdout=open(
            r'<PROJECT>\soak_%s.out' % name, 'w'),
        stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, close_fds=True)
    print('started %-8s pid=%d  %s' % (name, p.pid, ' '.join(argv)))

time.sleep(8)
print('\n-- alive after 8 s? --')
for line in subprocess.run(
        ['tasklist', '/FI', 'IMAGENAME eq python.exe', '/FO', 'CSV', '/NH'],
        capture_output=True, text=True).stdout.splitlines():
    if 'python.exe' in line:
        print('  ', line.replace('"""', '').strip())
