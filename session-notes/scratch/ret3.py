import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('ret3', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=3.0)
    time.sleep(2)
    # One -ex per line this time: the earlier version passed a multi-line
    # string as a single -ex, so only the first line ever ran.
    exprs = ['b *0x1000000001E', 'c', 'info registers rip rsp rax rbx rcx r11 cs ss rflags',
             'x/6i $rip', 'si', 'si', 'si', 'info registers rip rsp cs ss']
    out = m.gdb(exprs)
    started = False
    for l in out.splitlines():
        if 'Breakpoint 1,' in l or l.startswith('$') or 'rip ' in l or '=>' in l:
            print(l)
finally:
    m.shutdown()
