import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('buf', disk=None)
try:
    m.boot()
    time.sleep(1)
    out = m.gdb(['(long)wallpaper_pixels[0]','(long)wallpaper_pixels[500000]'])
    print('before:', [l for l in out.splitlines() if l.startswith('$')])
    m.run(['wallpaper /home/klye/picture.png'], settle=6.0)
    time.sleep(2)
    out = m.gdb(['(long)wallpaper_pixels[0]','(long)wallpaper_pixels[500000]'])
    print('after: ', [l for l in out.splitlines() if l.startswith('$')])
    log=open(m.log_path).read()
    print('returned:', [l for l in log.splitlines() if 'png_decode returned' in l])
finally:
    m.shutdown()
