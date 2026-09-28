import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('rows', disk=disk)
def call(expr):
    try:
        for line in g.gdb([expr]).splitlines():
            if line.startswith('$1'):
                return line.split('=',1)[1].strip()
    except Exception as e:
        return 'err:%s' % str(e)[:40]
    return '?'
try:
    g.boot()
    g.run(['open clock'], settle=2.0)
    print('present_count ', call('gfx_present_count()'))
    print('last_rows     ', call('gfx_last_present_rows()'))
    print('screen height ', 720)
    time.sleep(3.0)
    print('--- after 3s of the clock ticking ---')
    print('present_count ', call('gfx_present_count()'))
    print('last_rows     ', call('gfx_last_present_rows()'))
finally:
    g.shutdown()
