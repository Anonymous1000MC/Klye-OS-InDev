import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all

kill_all()
m = Guest('dbg', disk=None)
try:
    assert m.wait_ready(120), 'never ready'
    time.sleep(6)
    print('slot_count/dock_y:', m.ints(['wm.slot_count', 'wm.dock_y']))
    cnt = m.ints(['wm.slot_count'])[0]
    kinds = m.ints(['wm.slots[%d].kind' % i for i in range(cnt)])
    print('kinds:', kinds)
    for i, k in enumerate(kinds):
        if k == 2:
            idx = m.ints(['wm.slots[%d].launcher' % i])[0]
            print(i, 'launcher idx', idx, repr(m.gdb(['launchers[%d].file' % idx])))
    print('icon_of klye-sh:', m.dock_icon_of('klye-sh'))
finally:
    m.shutdown()
