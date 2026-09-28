import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('probe', disk=disk)
try:
    g.boot()
    g.run(['open e1m1'], settle=3.0)
    time.sleep(3)
    print('ints:', g.ints(['doom3d_prepared','doom3d_seg_count','doom3d_solid_count',
                           'doom3d_view_x','doom3d_view_y','doom3d_view_angle',
                           'doom3d_view_floor','doom3d_view_z']))
    h = g.gdb(['doom3d_prepared','doom3d_seg_count','doom3d_view_x','doom3d_view_y',
               'doom3d_view_angle','doom3d_view_floor'])
    print('gdb raw:\n', h)
finally:
    g.shutdown()
