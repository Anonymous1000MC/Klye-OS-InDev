import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('chk', disk=None)
try:
    m.boot()
    m.run(['help'], settle=3.0)
    time.sleep(2)
    for l in m.lines(20):
        if l.strip(): print(l)
    print('--- serial tail ---')
    log = open(m.log_path).read()
    print(log[-400:])
finally:
    m.shutdown()
