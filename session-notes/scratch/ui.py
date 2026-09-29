import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('ui', disk=None)
try:
    m.boot()
    m.run(['help','files','apps'], settle=3.0)
    time.sleep(2)
    for l in m.lines(40):
        if l.strip(): print(l)
finally:
    m.shutdown()
