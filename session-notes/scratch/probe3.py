import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('p3', disk=disk)
try:
    g.boot()
    g.run(['open e1m1'], settle=3.0)
    time.sleep(2)
    for sym in ['hosts[0].used','hosts[0].failed','hosts[0].draw_count',
                'hosts[0].previous_count','hosts[0].list_changed',
                'doom3d_prepared','doom3d_seg_count','doom3d_view_x',
                'doom3d_view_angle']:
        try:
            print('%-28s = %s' % (sym, g.ints([sym])))
        except Exception as e:
            print('%-28s : %s' % (sym, str(e)[:60]))
    try:
        print('error text:', repr(g.gdb(['hosts[0].error'])))
    except Exception as e:
        print('error text: %s' % str(e)[:60])
finally:
    g.shutdown()
