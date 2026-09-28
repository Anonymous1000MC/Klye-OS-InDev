import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('vfs', disk=disk)
try:
    g.boot()
    print('=== boot: vfs lines ===')
    for line in g.lines(40):
        if 'VIRTUAL' in line or 'bytes in-memory' in line or 'in use' in line:
            print(line)
    print('=== the command that used to fail ===')
    g.run(['kpm build lua/e1m1.lua'], settle=2.0)
    print('\n'.join(g.lines(12)))
    print('=== write a file, then read it back ===')
    big = 'x' * 20000
    g.run(['echo hello > /home/klye/big.txt'], settle=1.5)
    print('\n'.join(g.lines(8)))
finally:
    g.shutdown()
