import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('term', disk=None)
try:
    m.boot()
    m.run(['echo hello world', 'neofetch'], settle=3.0)
    time.sleep(2)
    m.screendump('/tmp/opencode/t.ppm')
    print('lines from guest memory:')
    for l in m.lines(24):
        if l.strip(): print('  ', l)
finally:
    m.shutdown()
