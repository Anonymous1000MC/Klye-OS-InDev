import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('step', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=2.0)
    time.sleep(1)
    # break right where the first syscall returns into user code
    cmds = 'b *0x1000000001E\nc\ninfo registers rip rsp rax rcx r11 cs ss\n'
    cmds += 'x/4i $rip\n'
    cmds += 'si\ninfo registers rip rsp cs ss\n'
    cmds += 'si\ninfo registers rip rsp cs ss\n'
    out = m.gdb_raw(cmds)
    started=False
    for l in out.splitlines():
        if l.startswith('0x') and ':' in l: started=True
        if started: print(l)
finally:
    m.shutdown()
