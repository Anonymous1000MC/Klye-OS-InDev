import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
g = Guest('vd', use_cdrom=False)
try:
    g.boot()
    g.run(['virtio'], settle=2.0)
    g.run(['vblkread 0'], settle=3.0)
    print('\n'.join(g.lines(40)))
finally:
    g.shutdown()
