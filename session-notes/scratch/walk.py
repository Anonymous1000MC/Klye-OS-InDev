import sys, time, subprocess
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('walk', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=6.0)
    time.sleep(2)
    # Walk a page the ring 3 test executes from, and a high physical frame.
    for sym,val in (('heap frame 0x1590000',0x1590000),):
        pass
    cmds = ['p/x $cr4','p/x $cr3',
            'p/x ((unsigned long *)((*(unsigned long *)($cr3))[2]))[0]',
            'p/x (((unsigned long *)((*(unsigned long *)($cr3))[2]))[0])']
    args=['gdb','-batch','-nx','build/klye.elf','-ex','target remote '+m.gdb_path]
    for c in cmds: args += ['-ex',c]
    args += ['-ex','detach']
    r=subprocess.run(args,capture_output=True,text=True,timeout=60)
    for l in r.stdout.splitlines():
        if l.startswith('$'): print(l)
finally:
    m.shutdown()
