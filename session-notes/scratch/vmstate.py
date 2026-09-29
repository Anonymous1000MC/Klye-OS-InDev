import sys, time, subprocess
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('vms', disk=None)
try:
    m.boot()
    m.run(['elf /bin/hello.elf'], settle=6.0)
    time.sleep(2)
    cmds = ['info registers cs ss cr0 cr4',
            'p/x $eflags',
            'p (long)tasks[3].frame.rip',
            'p (long)tasks[3].frame.cs',
            'p (long)tasks[3].frame.rsp',
            'p (long)tasks[3].frame.ss',
            'p/x gdt[4].limit_low',
            'p/x gdt[4].granularity']
    args = ['gdb','-batch','-nx','build/klye.elf','-ex','target remote '+m.gdb_path]
    for c in cmds: args += ['-ex', c]
    args += ['-ex','detach']
    r = subprocess.run(args, capture_output=True, text=True, timeout=60)
    for l in r.stdout.splitlines():
        if l.startswith('$') or 'cs ' in l or 'ss ' in l or 'cr0' in l or 'cr4' in l:
            print(l)
    if r.stderr.strip(): print('ERR:', r.stderr.strip()[-200:])
finally:
    m.shutdown()
