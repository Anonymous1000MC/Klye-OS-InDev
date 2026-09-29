import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('lg2', disk=None)
try:
    m.boot()
    m.run(['neofetch'], settle=4.0)
    time.sleep(2)
    # ppm_count: count pixels of a colour in a box.  use the bright text colour.
    n = m.ppm_count('/tmp/opencode/l2.ppm', 0xE8, 0xE8, 0xED, 0, 0, 1280, 720)
    print('bright pixels on screen:', n)
    # ask for a screendump first
    m.screendump('/tmp/opencode/l2.ppm')
    n = m.ppm_count('/tmp/opencode/l2.ppm', 0xE8, 0xE8, 0xED, 0, 0, 1280, 720)
    print('after screendump:', n)
finally:
    m.shutdown()
