import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('ret', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=6.0)
    time.sleep(3)
    log = open(m.log_path).read()
    i = log.find('user_enter')
    print(log[max(0,i-200):])
    print('=== how far did the user program get? ===')
    for l in log.splitlines():
        if 'user' in l or 'r3:' in l:
            print(' ', l)
finally:
    m.shutdown()
