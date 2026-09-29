import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('ver', disk=None)
try:
    m.boot()
    m.run(['wallpaper /home/klye/picture.png',
           'wallpaper /missing.png',
           'wallpaper /home/klye/picture.png'], settle=8.0)
    time.sleep(6)
    for l in m.lines(12):
        if l.strip(): print(l)
    m.screendump('/tmp/opencode/ver.ppm')
finally:
    m.shutdown()
