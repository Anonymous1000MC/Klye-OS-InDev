import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('fin', disk=None)
try:
    m.boot()
    # close the terminal so the desktop is fully visible
    m.run(['wallpaper /home/klye/picture.png'], settle=10.0)
    time.sleep(8)
    a = m.ints(['(int)gfx_present_count()'])
    m.screendump('/tmp/opencode/fin.ppm')
    b = m.ints(['(int)gfx_present_count()'])
    print('present before/after dump:', a, b)
    for l in open(m.log_path).read().splitlines():
        if 'png:' in l or 'returned' in l: print(l)
finally:
    m.shutdown()
