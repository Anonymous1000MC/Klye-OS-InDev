import sys
import time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all

disk = sys.argv[1]
kill_all()
time.sleep(1)
g = Guest('where', iso='klye.iso', port=4610, disk=disk)
time.sleep(6)
out = g.gdb(['$rip', '$rsp'])
for line in out.splitlines():
    if line.startswith('$'):
        print('  ', line.strip())
# where is that address?
out2 = g.gdb(['(char *)0'])
out3 = g.gdb(['info symbol $rip'])
print('  --- symbol ---')
for line in out3.splitlines():
    print('  ', line)
print('  serial bytes:', len(open(g.log_path, 'rb').read()))
g.shutdown()
