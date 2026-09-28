import sys
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('bt', disk=disk)
try:
    g.boot()
    print('booted clean, terminal count', g.term_count())
    for sym in ['vfs_block_count','vfs_node_count','vfs_used_blocks']:
        print('%-18s = %s' % (sym, g.ints([sym])))
finally:
    g.shutdown()
