import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('raw', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=20.0)
    time.sleep(6)
    log = open(m.log_path).read()
    i = log.find('user_enter');
    if i < 0: i = 0
    print(repr(log[i:]))
finally:
    m.shutdown()
