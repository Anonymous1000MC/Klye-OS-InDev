import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all

kill_all()
m = Guest('r3', disk=None)
try:
    m.boot()
    print('booted, desktop up')
    ok = m.run(['usertest'], settle=10.0)
    print('typed:', ok)
    time.sleep(5)
    log = open(m.log_path).read()
    i = log.find('COMPOSITOR TASK')
    print('--- after boot ---')
    print(log[i:])
finally:
    m.shutdown()
