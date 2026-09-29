import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('wp4', disk=None)
try:
    m.boot()
    m.run(['wallpaper /home/klye/picture.png'], settle=5.0)
    time.sleep(2)
    log = open(m.log_path).read()
    for l in log.splitlines():
        if 'wallpaper' in l.lower() or 'png' in l.lower(): print(l)
finally:
    m.shutdown()
