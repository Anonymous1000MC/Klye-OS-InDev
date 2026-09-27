#!/usr/bin/env python3
"""QEMU test harness for Klye OS.

Boots the ISO under QEMU with a gdb stub and a serial log, drives the guest
over QMP, and reads guest state either through the gdb stub or by dumping the
screendump to a PPM.

Note: /tmp/opencode gets cleaned periodically, so this file has to be
recreatable.  It depends on these guest symbols:
    term_lines / term_lengths / term_count   terminal scrollback
    wm.*                                      window manager state
    hosts[]                                   Lua hosts
"""

import os
import re
import subprocess
import time

REPO = '/run/media/daffa/abe1503c-e264-426b-a21a-8697892d03e6/klye-os'
ELF = os.path.join(REPO, 'build/klye.elf')
ISO = os.path.join(REPO, 'klye.iso')

QMAP = {
    'a': 'a', 'b': 'b', 'c': 'c', 'd': 'd', 'e': 'e', 'f': 'f', 'g': 'g',
    'h': 'h', 'i': 'i', 'j': 'j', 'k': 'k', 'l': 'l', 'm': 'm', 'n': 'n',
    'o': 'o', 'p': 'p', 'q': 'q', 'r': 'r', 's': 's', 't': 't', 'u': 'u',
    'v': 'v', 'w': 'w', 'x': 'x', 'y': 'y', 'z': 'z',
    '0': '0', '1': '1', '2': '2', '3': '3', '4': '4', '5': '5', '6': '6',
    '7': '7', '8': '8', '9': '9',
    ' ': 'spc', '.': 'dot', ',': 'comma', '-': 'minus', '=': 'equal',
    '/': 'slash', '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe',
    '[': 'bracket_left', ']': 'bracket_right', '`': 'grave_accent',
    '\n': 'ret', ':': 'shift_semicolon',
}


class Qmp:
    def __init__(self, path):
        import socket
        import json
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(120):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.5)
        else:
            raise RuntimeError('qmp socket never appeared')
        self.file = self.sock.makefile('rw', encoding='utf-8', newline='\n')
        self.json = json
        self._read()                      # greeting
        self.cmd('qmp_capabilities')

    def _read(self):
        while True:
            line = self.file.readline()
            if not line:
                return {}
            line = line.strip()
            if not line:
                continue
            try:
                return self.json.loads(line)
            except ValueError:
                continue

    def cmd(self, name, **kw):
        payload = {'execute': name}
        if kw:
            payload['arguments'] = kw
        self.file.write(self.json.dumps(payload) + '\n')
        self.file.flush()
        while True:
            msg = self._read()
            if 'event' in msg:
                continue
            if 'error' in msg:
                raise RuntimeError('%s: %s' % (name, msg['error']))
            return msg.get('return')

    def hmp(self, line):
        return self.cmd('human-monitor-command', **{'command-line': line})


class Guest:
    def __init__(self, tag, iso=ISO, mem='512M', port=4444, disk=None):
        self.tag = tag
        self.log_path = '/tmp/opencode/%s.log' % tag
        self.gdb_path = '127.0.0.1:%d' % port
        self.qmp_path = '/tmp/opencode/%s.qmp' % tag
        for path in (self.log_path, self.qmp_path):
            if os.path.exists(path):
                os.unlink(path)
        disk = disk or '/tmp/opencode/klye.img'
        self.disk_path = disk
        self.proc = subprocess.Popen([
            'qemu-system-x86_64',
            '-cdrom', iso,
            '-drive', 'file=%s,format=raw,if=ide,index=0,media=disk' % disk,
            '-m', mem,
            '-serial', 'file:%s' % self.log_path,
            '-display', 'none',
            '-gdb', 'tcp:' + self.gdb_path,
            '-qmp', 'unix:%s,server,nowait' % self.qmp_path,
            '-no-reboot',
        ], stdout=subprocess.DEVNULL,
            stderr=open('/tmp/opencode/%s.err' % tag, 'w'))
        time.sleep(1.0)
        self.q = Qmp(self.qmp_path)

    # ------------------------------------------------------------- serial --
    def serial(self, pattern):
        try:
            with open(self.log_path) as fh:
                return [l.rstrip() for l in fh if re.search(pattern, l)]
        except OSError:
            return []

    def wait_ready(self, timeout=90):
        limit = time.time() + timeout
        while time.time() < limit:
            if any('Klye OS ready' in l for l in self.serial('Klye OS ready')):
                return True
            time.sleep(1.0)
        return False

    def boot(self, settle=60):
        """Wait for the ready banner, then for the splash animation to hand
        over to the desktop.  The animation is several seconds long, so tests
        that click the dock too early find an empty dock and silently do
        nothing."""
        self.wait_ready()
        deadline = time.time() + settle
        while time.time() < deadline:
            try:
                if self.ints(['wm.slot_count'])[0] > 0:
                    break
            except Exception:
                pass
            time.sleep(2.0)
        time.sleep(2.0)
        return self

    # --------------------------------------------------------------- gdb --
    # gdb collapses a run of 4 or more identical characters into
    # "'3' <repeats 14 times>", which any regex reading the string back has to
    # undo.  Turning the collapsing off keeps `print` output literal, so a
    # terminal line comes back as one quoted string.  Without this, lines()
    # silently truncated at the first repeated run: 1/3 read back as "0."
    GDB_PRELUDE = ['-ex', 'set print repeats 0']

    def gdb(self, exprs, timeout=120):
        args = ['gdb', '-batch', '-nx'] + self.GDB_PRELUDE + [
            '-ex', 'target remote ' + self.gdb_path]
        for e in exprs:
            args += ['-ex', 'print ' + e]
        args += ['-ex', 'detach', ELF]
        return subprocess.run(args, capture_output=True, text=True,
                              timeout=timeout).stdout

    def gdb_raw(self, expr, timeout=120):
        """Run one expression and return the raw gdb stdout."""
        args = ['gdb', '-batch', '-nx'] + self.GDB_PRELUDE + [
            '-ex', 'target remote ' + self.gdb_path,
            '-ex', expr,
            '-ex', 'detach', ELF]
        return subprocess.run(args, capture_output=True, text=True,
                              timeout=timeout).stdout

    def ints(self, exprs):
        out = self.gdb(exprs)
        return [int(x) for x in re.findall(r'^\$\d+ = (-?\d+)', out, re.M)]

    def bytes_at(self, symbol, count):
        """Read `count` bytes from a symbol via gdb's x/ command."""
        out = self.gdb_raw('x/%dub %s' % (count, symbol))
        data = bytearray()
        for line in out.splitlines():
            if not line.startswith('0x') or ':' not in line:
                continue
            body = line.split(':', 1)[1]
            for tok in body.split():
                tok = tok.strip()
                if tok.startswith('0x') and len(tok) == 4:
                    try:
                        data.append(int(tok, 16))
                    except ValueError:
                        pass
                elif len(tok) == 2 and all(c in '0123456789abcdefABCDEF'
                                           for c in tok):
                    data.append(int(tok, 16))
        return bytes(data[:count])

    # ---------------------------------------------------------- terminal --
    def lines(self, count=None):
        """Terminal scrollback, oldest first, formatted 'index | text'.

        Each row is fetched as its own gdb expression so the output stays
        aligned: asking for interleaved text and length expressions makes the
        numbering ambiguous.
        """
        total = self.term_count()
        if count is None or count > total:
            count = total
        first = total - count
        out = self.gdb(['term_lines[%d]' % i for i in range(first, total)])
        texts = {}
        for line in out.splitlines():
            m = re.match(r'^\$(\d+) = ', line)
            if not m:
                continue
            n = int(m.group(1))
            body = line.split('=', 1)[1].strip()
            m2 = re.match(r'^"((?:[^"\\]|\\.)*)"', body)
            if not m2:
                continue
            # with `set print repeats 0` the whole array comes back as one
            # string, NUL padding included, so drop the padding
            texts[n] = re.sub(r'(?:\\000)+$', '', m2.group(1))
        return ['%3d | %s' % (first + i, texts.get(i + 1, ''))
                for i in range(count)]

    def term_count(self):
        return self.ints(['term_count'])[0]

    def clear_terminal(self):
        self.key('a')
        self.enter()
        time.sleep(0.4)

    # ------------------------------------------------------------- input --
    def key(self, s):
        for ch in s:
            code = QMAP.get(ch, ch)
            self.q.cmd('input-send-event', events=[
                {'type': 'key', 'data': {'down': True,
                                         'key': {'type': 'qcode', 'data': code}}},
                {'type': 'key', 'data': {'down': False,
                                         'key': {'type': 'qcode', 'data': code}}},
            ])
            time.sleep(0.12)

    def enter(self):
        self.q.cmd('input-send-event', events=[
            {'type': 'key', 'data': {'down': True,
                                     'key': {'type': 'qcode', 'data': 'ret'}}},
            {'type': 'key', 'data': {'down': False,
                                     'key': {'type': 'qcode', 'data': 'ret'}}},
        ])
        time.sleep(0.3)

    def cmdline(self, text):
        self.key(text)
        self.enter()

    def mouse_to(self, tx, ty, chunk=200):
        for _ in range(40):
            x, y = self.ints(['wm.mouse_x', 'wm.mouse_y'])
            dx, dy = tx - x, ty - y
            if abs(dx) <= 1 and abs(dy) <= 1:
                return (x, y)
            sx = max(-chunk, min(chunk, dx))
            sy = max(-chunk, min(chunk, dy))
            if sx == 0 and sy == 0:
                sx = 1 if dx >= 0 else -1
            self.q.hmp('mouse_move %d %d' % (sx, sy))
            time.sleep(0.18)

    def click(self, tx, ty):
        self.mouse_to(tx, ty)
        self.q.hmp('mouse_button 1')
        time.sleep(0.4)
        self.q.hmp('mouse_button 0')
        time.sleep(0.6)

    def hold_button(self, tx, ty):
        self.mouse_to(tx, ty)
        self.q.hmp('mouse_button 1')
        time.sleep(0.3)

    def release_button(self):
        self.q.hmp('mouse_button 0')
        time.sleep(0.4)

    # ------------------------------------------------------------ driving --
    def open_terminal(self):
        """The desktop starts with no window; click the dock terminal icon."""
        slot, sy = self.dock_icon_of('klye-sh')
        if slot is None:
            return False
        self.click(slot, sy)
        time.sleep(1.0)
        return True

    def dock_icon_of(self, program):
        """Return (centre_x, centre_y) of a dock launcher slot."""
        info = self.ints(['wm.slot_count', 'wm.dock_y'])
        count, dock_y = info[0], info[1]
        kinds = self.ints(['wm.slots[%d].kind' % i for i in range(count)])
        for i, kind in enumerate(kinds):
            if kind != 2:
                continue
            idx = self.ints(['wm.slots[%d].launcher' % i])[0]
            names = self.gdb(['launchers[%d].file' % idx])
            for line in names.splitlines():
                # the name can be followed by stale bytes, so match loosely
                if line.startswith('$1') and program in line:
                    x, w = self.ints(['wm.slots[%d].x' % i,
                                      'wm.slots[%d].width' % i])
                    return (x + w // 2, dock_y + 22)
        return (None, None)

    def run(self, commands, settle=1.2):
        if not self.open_terminal():
            return False
        for cmd in commands:
            self.cmdline(cmd)
            time.sleep(settle)
        return True

    # ------------------------------------------------------------ capture --
    def screendump(self, path=None):
        path = path or ('/tmp/opencode/%s.ppm' % self.tag)
        self.q.cmd('screendump', filename=path)
        time.sleep(1.5)
        return path

    def ppm_pixel(self, path, x, y, width=1280):
        with open(path, 'rb') as fh:
            data = fh.read()
        i = data.index(b'255\n') + 4
        px = data[i:]
        o = (y * width + x) * 3
        return (px[o], px[o + 1], px[o + 2])

    def ppm_count(self, path, colour, x0, y0, x1, y1, width=1280):
        with open(path, 'rb') as fh:
            data = fh.read()
        i = data.index(b'255\n') + 4
        px = data[i:]
        total = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                o = (y * width + x) * 3
                if (px[o], px[o + 1], px[o + 2]) == colour:
                    total += 1
        return total

    def shutdown(self):
        try:
            self.proc.kill()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except Exception:
            pass


TERM_MAX_LINES = 512


def kill_all():
    subprocess.run(['pkill', '-9', '-f', '^qemu-system-x86_64'],
                   capture_output=True)
