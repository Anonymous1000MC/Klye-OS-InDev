import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('wp5', disk=None)
try:
    m.boot()
    time.sleep(1)
    out = m.gdb(['(long)wallpaper_pixels[0]'])
    print('before:', [l for l in out.splitlines() if l.startswith('$')])
    m.run(['wallpaper /home/klye/picture.png'], settle=5.0)
    time.sleep(2)
    out = m.gdb(['(long)wallpaper_pixels[0]','(long)wallpaper_pixels[40000]','(long)wallpaper_pixels[500000]'])
    print('after: ', [l for l in out.splitlines() if l.startswith('$')])
finally:
    m.shutdown()
