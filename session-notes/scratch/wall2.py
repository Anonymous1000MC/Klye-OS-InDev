import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('w2', disk=None)
try:
    m.boot()
    m.run(['wallpaper /home/kyl/e/picture.png'.replace('kyl','klye')], settle=8.0)
    time.sleep(3)
    m.screendump('/tmp/opencode/wall.ppm')
    print('dumped')
finally:
    m.shutdown()
