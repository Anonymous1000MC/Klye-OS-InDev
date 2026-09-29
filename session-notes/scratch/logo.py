import sys, time
sys.path.insert(0, 'tools')
from qemu_harness import Guest, kill_all
kill_all()
m = Guest('logo', disk=None)
try:
    m.boot()
    m.run(['neofetch'], settle=4.0)
    time.sleep(2)
    # crop the logo region of the terminal and print it as ascii
    m.screendump('/tmp/opencode/logo.ppm')
    hdr = open('/tmp/opencode/logo.ppm','rb').read(64).split()
    print('ppm header:', hdr[:4])
finally:
    m.shutdown()
