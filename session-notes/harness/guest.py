"""Boot Klye OS under QEMU and run shell commands in it.

The one harness script worth keeping.  Everything else today was a variation
on this: boot, wait for the desktop, type commands, optionally dump the screen.

    python3 session-notes/harness/guest.py <tag> [command ...]

A tag is just a name for the serial log and any screendump, so successive runs
do not overwrite each other.  With no commands it just boots, which is how to
check that the kernel still reaches the desktop.

Timings are the awkward part.  The PS/2 mouse reports movement asynchronously
and there is no way to read the guest's own pointer back reliably -- GDB against
a live guest in this project is not trustworthy, and polling it to wait for a
move to arrive is worse than useless because it times out.  So the mouse is
driven with direct HMP moves and a pause after each, and the shell commands are
given enough time to be echoed.
"""

import sys
import time

sys.path.insert(0, 'tools')

from qemu_harness import Guest, kill_all


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    tag = args[0]
    commands = args[1:]

    kill_all()
    guest = Guest(tag, disk=None)
    try:
        guest.boot(settle=60)
        time.sleep(4)
        for command in commands:
            guest.run([command], settle=6.0)
        if not commands:
            time.sleep(3)
        else:
            time.sleep(4)
        print("serial log: /tmp/opencode/%s.log" % tag)
    finally:
        guest.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
