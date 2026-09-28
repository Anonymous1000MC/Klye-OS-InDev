import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('bp', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=2.0)
    time.sleep(1)
    # break on the syscall instruction in the copied user image (base+0x1C etc)
    for a in (0x1000000001C, 0x1000000002E, 0x10000000040):
        out = m.gdb_raw('b *0x%x' % a + '\ninfo registers rip rsp rax rcx r11 cs ss')
        for l in out.splitlines():
            if any(k in l for k in ('rip','rsp','rax','rcx','r11','cs ','ss ','Breakpoint')):
                print('0x%x: %s' % (a, l.strip()))
    print('=== continue and see which hit ===')
    out = m.gdb_raw('c\ninfo registers rip rsp rax rcx r11 cs ss')
    for l in out.splitlines():
        if any(k in l for k in ('rip','rsp','rax ','rcx','r11','cs ','ss ')):
            print(l.strip())
finally:
    m.shutdown()
