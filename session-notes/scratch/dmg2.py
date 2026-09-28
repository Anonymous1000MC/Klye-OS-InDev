import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('dmg2', disk=disk)
def call(expr):
    try:
        out = g.gdb([expr])
        for line in out.splitlines():
            if line.startswith('$1'):
                return line.split('=')[1].strip()
    except Exception as e:
        return 'err'
    return '?'
try:
    g.boot()
    g.run(['open clock'], settle=2.0)
    a = (call('wm.composite_count()'), call('gfx_present_count()'),
         call('gfx_last_present_rows()'))
    time.sleep(5.0)
    b = (call('wm.composite_count()'), call('gfx_present_count()'),
         call('gfx_last_present_rows()'))
    for name, x, y in zip(('composites','presents','last rows'), a, b):
        print('%-11s %8s -> %-8s  delta %s' % (name, x, y, int(y)-int(x)))
    g.screendump('/tmp/opencode/clock.ppm')
finally:
    g.shutdown()
