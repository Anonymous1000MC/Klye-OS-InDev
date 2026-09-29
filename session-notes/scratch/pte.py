import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('pte', disk=None)
try:
    m.boot()
    m.run(['elf /bin/hello.elf'], settle=5.0)
    time.sleep(2)
    out = m.gdb(['(long)vm_to_physical(0x10000001000ULL)',
                 '(int)last_fault.error','(long)last_fault.rip','(long)last_fault.cr2'])
    for l in out.splitlines():
        if l.startswith('$'): print(l)
finally:
    m.shutdown()
