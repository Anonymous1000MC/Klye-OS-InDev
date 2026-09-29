import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('w5', disk=None)
try:
    m.boot()
    out = m.gdb(['(long)wm.wallpaper','(long)&wallpaper_surface','(long)wallpaper_pixels'])
    for l in out.splitlines():
        if l.startswith('$'): print(l)
    m.run(['wallpaper /home/klye/picture.png'], settle=3.0)
    time.sleep(2)
    print('--- after ---')
    out = m.gdb(['(long)wm.wallpaper','(long)wallpaper_pixels',
                 '(long)wallpaper_pixels[0]','(long)wallpaper_pixels[40000]'])
    for l in out.splitlines():
        if l.startswith('$'): print(l)
finally:
    m.shutdown()
