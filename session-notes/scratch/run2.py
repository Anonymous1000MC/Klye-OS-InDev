import sys, time, os
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('r2', disk=disk)
try:
    g.boot()
    print('booted, terminal count', g.term_count())
    for cmd in ['kpm build e1m1.lua', 'open e1m1']:
        g.cmdline(cmd)
        time.sleep(3)
        print('=== after %r ===' % cmd)
        print('\n'.join(g.lines(30)))
    g.screendump('/tmp/opencode/e1m1.ppm')
    print('screenshot saved')
finally:
    g.shutdown()
