import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('neo', disk=None)
try:
    m.boot()
    m.run(['neofetch'], settle=3.0)
    time.sleep(2)
    for l in m.lines(34):
        if l.strip(): print(l)
finally:
    m.shutdown()
