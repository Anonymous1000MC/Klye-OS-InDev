import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('long', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=25.0)
    time.sleep(10)
    log = open(m.log_path).read()
    for l in log.splitlines():
        if 'user' in l or 'r3:' in l or 'PANIC' in l or 'fault' in l.lower():
            print(l)
finally:
    m.shutdown()
