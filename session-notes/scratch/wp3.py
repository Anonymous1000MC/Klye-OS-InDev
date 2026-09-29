import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('wp3', disk=None)
try:
    m.boot()
    m.run(['wallpaper /home/klye/picture.png'], settle=5.0)
    time.sleep(2)
    for l in m.lines(8):
        print(repr(l))
    out = m.gdb(['(int)vfs_open("/home/klye/picture.png")',
                 '(int)vfs_size(vfs_open("/home/klye/picture.png"))'])
    print([l for l in out.splitlines() if l.startswith('$')])
finally:
    m.shutdown()
