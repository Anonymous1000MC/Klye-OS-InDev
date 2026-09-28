import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
import os
disk = os.path.join('/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os','build','klye.img')
g = Guest('e1m1', disk=disk)
try:
    g.boot()
    g.cmdline('kpm build e1m1.lua')
    time.sleep(2)
    g.cmdline('open e1m1')
    time.sleep(4)
    g.screendump('/tmp/opencode/e1m1.ppm')
    print('--- terminal tail ---')
    print('\n'.join(g.lines(25)))
finally:
    g.shutdown()
