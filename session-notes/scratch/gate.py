import sys
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all

kill_all()
m = Guest('gate', disk=None)
try:
    m.boot()
    out = m.gdb(['(int)idt[128].type','(int)idt[128].selector',
                 '(int)idt[128].offset_low','(int)idt[128].offset_middle',
                 '(int)idt[128].offset_high','(int)idt[128].zero',
                 '(int)idt[13].type','(int)idt[13].selector'])
    for line in out.splitlines():
        if line.startswith('$'):
            print(line)
finally:
    m.shutdown()
