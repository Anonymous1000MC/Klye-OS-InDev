import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('r3', disk=disk)
try:
    g.boot()
    print('ok =', g.run(['open e1m1'], settle=3.0))
    time.sleep(4)
    print('--- terminal ---')
    print('\n'.join(g.lines(40)))
    g.screendump('/tmp/opencode/e1m1.ppm')
finally:
    g.shutdown()
