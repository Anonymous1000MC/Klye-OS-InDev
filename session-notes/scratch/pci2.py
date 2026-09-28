import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('pci2', disk=disk)
try:
    g.boot()
    g.run(['pci'], settle=2.0)
    for line in g.lines(10):
        body = line.split('| ',1)[-1]
        print(repr(body))
    print('--- device fields from gdb ---')
    for i in range(4):
        v = g.ints(['pci_devices[%d].class_code' % i, 'pci_devices[%d].subclass' % i,
                    'pci_devices[%d].bar_count' % i])
        print(i, v)
finally:
    g.shutdown()
