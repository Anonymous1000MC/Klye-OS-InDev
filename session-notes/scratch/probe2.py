import sys, time
sys.path.insert(0, '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/tools')
from qemu_harness import Guest
disk = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os/build/klye.img'
g = Guest('p2', disk=disk)
try:
    g.boot()
    g.run(['open e1m1'], settle=3.0)
    time.sleep(2)
    print('=== terminal ===')
    print('\n'.join(g.lines(30)))
    for i in range(4):
        try:
            print('host %d:'%i, g.ints(['hosts[%d].used'%i,'hosts[%d].failed'%i,
                                         'hosts[%d].draw_count'%i,'hosts[%d].width'%i,
                                         'hosts[%d].height'%i,'hosts[%d].frame'%i]))
        except Exception as e:
            print('host %d: %s'%(i,e)); break
    print('windows:', g.ints(['wm.window_count','wm.focused']))
    for i in range(4):
        try:
            print('win %d:'%i, g.ints(['wm.windows[%d].app'%i,'wm.windows[%d].lua'%i,
                                        'wm.windows[%d].script'%i,'wm.windows[%d].x'%i,
                                        'wm.windows[%d].y'%i,'wm.windows[%d].width'%i,
                                        'wm.windows[%d].height'%i]))
        except Exception as e:
            print('win %d: %s'%(i,e)); break
finally:
    g.shutdown()
