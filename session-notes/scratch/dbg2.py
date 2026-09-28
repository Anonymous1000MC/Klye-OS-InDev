import sys, time, subprocess
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all, ELF
kill_all()
m = Guest('dbg2', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=3.0)
    time.sleep(2)
    cmds = ['x/6i 0x10000000000',
            'b *0x1000000001E',
            'c',
            'info registers rip rsp rax rbx rcx r11 cs ss',
            'x/4i $rip',
            'detach']
    args = ['gdb','-batch','-nx','-ex','set print repeats 0',
            '-ex','target remote '+m.gdb_path]
    for c in cmds: args += ['-ex', c]
    r = subprocess.run(args, capture_output=True, text=True, timeout=180)
    print(r.stdout[-3000:])
    if r.stderr.strip(): print('STDERR:', r.stderr[-600:])
finally:
    m.shutdown()
