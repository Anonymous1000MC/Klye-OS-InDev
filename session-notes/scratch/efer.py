import sys
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('efer', disk=None)
try:
    m.boot()
    print([l for l in m.gdb_raw('p/x $efer').splitlines() if l.startswith('$')])
finally:
    m.shutdown()
