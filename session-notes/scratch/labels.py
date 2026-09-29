import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('lab', disk=None)
try:
    m.boot()
    m.run(['wallpaper /home/klye/picture.png'], settle=8.0)
    time.sleep(6)
    m.screendump('/tmp/opencode/lab.ppm')
    print('ok')
finally:
    m.shutdown()
