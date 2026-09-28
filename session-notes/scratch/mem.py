import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('mem', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=3.0)
    time.sleep(2)
    out = m.gdb(['x/8bx 0x10000000000', 'x/4i 0x10000000000'])
    for l in out.splitlines():
        if l.startswith('$') or '0x1000' in l: print(l)
finally:
    m.shutdown()
