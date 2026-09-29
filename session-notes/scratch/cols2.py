import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('c2', disk=None)
try:
    m.boot()
    m.run(['neofetch'], settle=4.0)
    time.sleep(2)
    out = m.gdb(['(int)TERMINAL_LOGICAL_COLS','(int)TERMINAL_PAD_X',
                 '(int)TEXT_ADVANCE','(int)wm.windows[0].width'])
    for l in out.splitlines():
        if l.startswith('$'): print(l)
finally:
    m.shutdown()
