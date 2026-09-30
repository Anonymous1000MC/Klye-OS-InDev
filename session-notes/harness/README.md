# Harness scripts

Small QEMU drivers used to test the desktop.  They live here rather than in
/tmp because /tmp is a tmpfs and does not survive a reboot, which cost a day of
rediscovery once already.

    python3 session-notes/harness/guest.py <tag> [command ...]
        boot, run shell commands, leave the serial log in /tmp/opencode/<tag>.log

    python3 session-notes/harness/screendump.py <out.ppm> [command ...]
        boot, run commands, write a screendump

## Driving the mouse

The guest's pointer position cannot be read back reliably.  GDB against a live
guest in this project is untrustworthy and polling it to wait for a move to
arrive times out, so the mouse is driven with direct HMP moves and a pause.

`Guest.click(x, y)` polls until the guest reports the pointer has arrived.  It
works and it is slow.  `Guest.q.hmp('mouse_move N 0')` does not poll and needs
a sleep after it.

**A click that did not land is indistinguishable from a click on nothing**, and
both look exactly like a feature being broken.  Two sessions went into this:
the zoom button appeared dead because the click was 32 pixels to the left of
the window, and a resize appeared to work when nothing had been resized.

The geometry that was measured, for reference: a new window is placed at x=120,
y=50.  The shadow spreads further left, and the shadow is not the window.

## Reading a screendump

    qemu writes a P6 PPM.  Parse the header, then index (y * width + x) * 3.
    Print coverage as a grey ramp to see anti-aliased text as greys, and find a
    window by scanning for its surface colour rather than by measuring an edge,
    which finds the shadow instead of the window.
