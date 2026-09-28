import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('vt', disk=disk, extra=[
    '-device', 'virtio-blk-pci,drive=vdisk0',
    '-drive', 'file=/tmp/opencode/vdisk.img,format=raw,if=none,id=vdisk0',
])
try:
    g.boot()
    g.run(['pci map'], settle=2.0)
    g.run(['virtio'], settle=1.0)
    g.run(['vblkread 0'], settle=3.0)
    print('\n'.join(g.lines(30)))
finally:
    g.shutdown()
