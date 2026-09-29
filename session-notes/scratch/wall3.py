import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('w3', disk=None)
try:
    m.boot()
    m.run(['wallpaper /home/klye/picture.png'], settle=8.0)
    time.sleep(3)
    out = m.gdb(['(int)wm.wallpaper->width','(int)wm.wallpaper->height',
                 '(long)wm.wallpaper->pixels[0]','(long)wm.wallpaper->pixels[40000]'])
    for l in out.splitlines():
        if l.startswith('$'): print(l)
    log=open(m.log_path).read()
    print('wallpaper lines:', [l for l in log.splitlines() if 'wallpaper' in l])
finally:
    m.shutdown()
