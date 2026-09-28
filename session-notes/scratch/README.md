# Scratch scripts

Throwaway drivers used to boot the guest under QEMU and read the serial log.
Kept because they are how anything here gets verified, and because several of
them are the *correct* version of a command the harness gets wrong.

Run any of them from the repo root with `python3 session-notes/scratch/<name>.py`.
They need the ISO built first: `make -B klye.iso`.

## The ones that matter for ring 3

- `raw.py`   -- boots, types `usertest` at the shell, prints the **whole**
                serial log unfiltered. This is the one to use. Several earlier
                scripts grepped or tail-ed the log and hid the trace line that
                was sitting in it.
- `long.py`  -- same, filtered to the user/r3 lines for a quick look.
- `tick.py`  -- checks whether the guest rebooted (counts "Klye OS 0.2").
- `reboot.py -- the check that caught the reboot.
- `gate.py`  -- reads a live IDT gate out of the running guest over gdb.

## Do not trust these

- `mem.py`, `step.py`, `bp.py`, `ret.py`, `ret2.py`, `ret3.py`, `dbg2.py`,
  `gate.py` -- every one of these called `Guest.gdb()` or `gdb_raw()` for
  something that is not a `print`. `gdb()` prepends `print ` to all of its
  arguments, so `b *addr`, `c`, `x/4i` and `info registers` ran as print
  statements and did nothing, reporting a clean negative. Use a hand-built
  gdb argv instead. Even then the remote connection timed out here, so treat
  gdb output as unreliable and instrument the kernel over serial instead.
- `dmg2.py`, `probe*.py`, `crop.py`, `rows.py`, `win.py`, `show.py`,
  `vdisk_test.py`, `pcitest.py`, `virtio.py`, `vfs*.py`, `run_e1m1.py` --
  older, from earlier topics. Kept for reference only.
