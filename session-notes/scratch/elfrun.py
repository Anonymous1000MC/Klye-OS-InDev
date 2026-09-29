import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('elf', disk=None)
try:
    m.boot()
    m.run(['elf /bin/hello.elf'], settle=12.0)
    time.sleep(5)
    m.screendump('/tmp/opencode/elf.ppm')
    print("=== serial ===")
    log = open(m.log_path).read()
    i = log.find('COMPOSITOR TASK')
    print(log[i:] if i > 0 else log[-800:])
    print("=== terminal lines from the framebuffer ===")
    for l in m.lines(40):
        if l.strip(): print(' ', l)
finally:
    m.shutdown()
