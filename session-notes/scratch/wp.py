import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('wp', disk=None)
try:
    m.boot()
    time.sleep(1)
    out = m.gdb(['(long)wallpaper_pixels[0]','(long)wallpaper_pixels[40000]'])
    print('BEFORE:', [l for l in out.splitlines() if l.startswith('$')])
    m.run(['wallpaper /home/klye/picture.png'], settle=4.0)
    time.sleep(2)
    out = m.gdb(['(long)wallpaper_pixels[0]','(long)wallpaper_pixels[40000]',
                 '(long)wallpaper_pixels[921600]'])
    print('AFTER: ', [l for l in out.splitlines() if l.startswith('$')])
    for v in (4293455092, 4293323763):
        print('  0x%08X -> (%02x,%02x,%02x)' % (v,(v>>16)&0xFF,(v>>8)&0xFF,v&0xFF))
finally:
    m.shutdown()
