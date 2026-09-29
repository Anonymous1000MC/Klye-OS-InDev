import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('pan', disk=None)
try:
    m.boot()
    m.run(['wallpaper /home/klye/picture.png'], settle=6.0)
    time.sleep(3)
    m.screendump('/tmp/opencode/pan.ppm')
    log = open(m.log_path).read()
    print('--- serial tail ---')
    print(log[-600:])
finally:
    m.shutdown()
