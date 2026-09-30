"""Boot, drive the mouse, and dump the framebuffer.

    python3 session-notes/harness/screendump.py <out.ppm> [command ...]

Mouse actions are hardcoded per scenario because there is no reliable way to
ask the guest where its pointer is.  The helper functions below are the ones
that were needed:

    click(x, y)   a press and release, after polling GDB until the guest agrees
                  it has arrived.  Slow, and it works.
    hmp_click(x, y)  raw HMP moves plus a pause, which does not poll.

Use click() when the coordinate matters and hmp_click() when only the button
state does.  A click that did not land is indistinguishable from a click on
nothing, and both look exactly like the feature being broken.

Window geometry that was verified by this: a new window is placed at x=120,
y=50 with a shadow spreading further left.  The shadow is NOT the window, and
measuring it puts the click tens of pixels off to the left of the target.
"""

import sys
import time

sys.path.insert(0, 'tools')

from qemu_harness import Guest, kill_all


def boot(tag):
    kill_all()
    guest = Guest(tag, disk=None)
    guest.boot(settle=60)
    time.sleep(4)
    return guest


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    out = args[0]
    guest = boot('dump')
    try:
        for command in args[1:]:
            guest.run([command], settle=3.0)
        time.sleep(2)
        guest.screendump(out)
        print("wrote %s" % out)
    finally:
        guest.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
