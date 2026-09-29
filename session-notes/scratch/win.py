import sys, time, subprocess
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('win', disk=None)
try:
    m.boot()
    m.run(['elf /bin/hello.elf'], settle=6.0)
    time.sleep(1)
    for sym in ('vm_user_base','vm_user_next','vm_user_limit','vm_window_base','vm_window_limit'):
        out = m.gdb(['(long)%s' % sym])
        for l in out.splitlines():
            if l.startswith('$'): print(sym, l)
finally:
    m.shutdown()
