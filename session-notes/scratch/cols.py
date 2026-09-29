import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('cols', disk=None)
try:
    m.boot()
    m.run(['neofetch'], settle=4.0)
    time.sleep(2)
    out = m.gdb(['(int)wm.windows[0].width','(int)wm.windows[0].height',
                 '(int)gfx_width()','(int)gfx_height()','(int)wm.focused'])
    for l in out.splitlines():
        if l.startswith('$'): print(l)
finally:
    m.shutdown()
