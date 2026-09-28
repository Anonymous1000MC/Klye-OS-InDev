import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('r3', disk=disk)
try:
    g.boot()
    g.run(['usertest'], settle=3.0)
    print('--- terminal ---')
    print('\n'.join(g.lines(14)))
finally:
    g.shutdown()
