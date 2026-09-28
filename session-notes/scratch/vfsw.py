import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('vw', disk=disk)
try:
    g.boot()
    g.run(['kpm build lua/probe.lua'], settle=2.0)
    print('=== kpm build of a name that does not exist ===')
    print('\n'.join(g.lines(10)))
    g.run(['ls -l /bin'], settle=1.5)
    print('=== /bin ===')
    print('\n'.join(g.lines(26)))
finally:
    g.shutdown()
