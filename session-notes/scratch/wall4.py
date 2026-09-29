import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('w4', disk=None)
try:
    m.boot()
    m.run(['wallpaper /home/klye/picture.png'], settle=3.0)
    time.sleep(6)
    m.screendump('/tmp/opencode/wall.ppm')
    log=open(m.log_path).read()
    print('present count now:', m.gdb(['(int)gfx_present_count()']))
    for l in m.gdb(['(int)wm.chrome_dirty']).splitlines():
        if l.startswith('$'): print('chrome_dirty', l)
finally:
    m.shutdown()
