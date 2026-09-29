import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('wp6', disk=None)
try:
    m.boot()
    time.sleep(1)
    out = m.gdb(['(long)wm.wallpaper','(long)wallpaper_surface.pixels',
                 '(long)&wallpaper_surface','(int)wallpaper_surface.width'])
    print('boot:', [l for l in out.splitlines() if l.startswith('$')])
    m.run(['wallpaper /home/klye/picture.png'], settle=5.0)
    time.sleep(2)
    out = m.gdb(['(long)wm.wallpaper','(long)wallpaper_surface.pixels',
                 '(long)wallpaper_surface.pixels[0]'])
    print('after:', [l for l in out.splitlines() if l.startswith('$')])
finally:
    m.shutdown()
