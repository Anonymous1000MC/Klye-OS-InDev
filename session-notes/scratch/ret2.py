import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('ret2', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=15.0)
    time.sleep(5)
    log = open(m.log_path).read()
    i = log.find('user_enter')
    print(log[max(0,i-100):])
finally:
    m.shutdown()
