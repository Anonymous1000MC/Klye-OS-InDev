import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('st', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=4.0)
    time.sleep(2)
    out = m.gdb(['(long)user_syscall_rip','(long)user_syscall_rflags',
                 '(int)syscall_count','(int)user_exit_code'])
    for l in out.splitlines():
        if l.startswith('$'): print(l)
finally:
    m.shutdown()
