import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('tick', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=20.0)
    time.sleep(6)
    log = open(m.log_path).read()
    print("boots:", log.count('Klye OS 0.2'), " (1 = no reboot)")
    for l in log.splitlines():
        if 'user' in l or 'r3:' in l or 'PANIC' in l: print(' ', l)
    out = m.gdb(['(int)scheduler_current_task()'])
    print('current task:', [l for l in out.splitlines() if l.startswith('$')])
finally:
    m.shutdown()
