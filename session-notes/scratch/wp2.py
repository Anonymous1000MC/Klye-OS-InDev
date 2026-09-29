import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('wp2', disk=None)
try:
    m.boot()
    time.sleep(1)
    # break inside wm_set_wallpaper by watching a counter: read the surface the
    # decoder was handed, and whether png_decode returned true
    out = m.gdb(['(int)vfs_size(0)','(long)wallpaper_surface.pixels',
                 '(int)wallpaper_surface.width','(int)wallpaper_surface.height'])
    print('surface:', [l for l in out.splitlines() if l.startswith('$')])
    m.run(['wallpaper /home/klye/picture.png'], settle=4.0)
    time.sleep(2)
    log = open(m.log_path).read()
    print('serial mentions wallpaper:', [l for l in log.splitlines() if 'wallpaper' in l.lower() or 'png' in l.lower()])
    for l in m.lines(6):
        if l.strip(): print(l)
finally:
    m.shutdown()
