import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('dmg', disk=disk)
def one(sym):
    r = g.ints([sym])
    return r[0] if r else '?'
try:
    g.boot()
    g.run(['open clock'], settle=2.0)
    for label in ('after open', 'after 4s ticking'):
        print('%-16s composite=%-6s rect=%-6s' % (
            label, one('wm.composite_count'), one('wm.damage_rect_count')))
        time.sleep(4.0)
    g.screendump('/tmp/opencode/clock.ppm')
finally:
    g.shutdown()
