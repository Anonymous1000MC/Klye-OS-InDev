import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('wall', disk=None)
try:
    m.boot()
    m.run(['wallpaper /home/klye/picture.png',
           'wallpaper /nope.png',
           'wallpaper /home/klye/picture.png'], settle=6.0)
    time.sleep(3)
    for l in m.lines(30):
        if 'wallpaper' in l or 'user' in l: print(l)
finally:
    m.shutdown()
