import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('rb', disk=None)
try:
    m.boot()
    m.run(['usertest'], settle=20.0)
    time.sleep(8)
    log = open(m.log_path).read()
    i = log.find('user_enter')
    print("=== after user_enter ===")
    print(log[i if i>0 else 0:][:1200])
    print("=== reboot detected:", log.count('Klye OS 0.2') > 1, "boots:", log.count('Klye OS 0.2'))
finally:
    m.shutdown()
