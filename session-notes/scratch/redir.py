import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('rd', disk=disk)
try:
    g.boot()
    g.run(['echo hello > /home/klye/t.txt'], settle=1.5)
    print('=== redirect write ===');  print('\n'.join(g.lines(8)))
    g.run(['cat /home/klye/t.txt'], settle=1.5)
    print('=== read back ===');  print('\n'.join(g.lines(8)))
    g.run(['ls /home/klye'], settle=1.5)
    print('=== dir ===');  print('\n'.join(g.lines(12)))
finally:
    g.shutdown()
