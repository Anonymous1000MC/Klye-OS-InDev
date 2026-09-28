import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('vq', disk=disk)
try:
    g.boot()
    for sym in ['vfs_block_count','vfs_node_count','vfs_used_blocks','vfs_ready']:
        try: print('%-20s = %s' % (sym, g.ints([sym])))
        except Exception as e: print('%-20s : %s' % (sym, str(e)[:50]))
    g.run(['kpm build lua/e1m1.lua'], settle=2.0)
    print('\n'.join(g.lines(8)))
finally:
    g.shutdown()
