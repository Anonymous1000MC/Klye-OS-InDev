# Writing Klye apps in KBY bytecode

KBY is the low-level app format. It predates Lua support and is still useful
when you want something with no interpreter in the loop, but **for new apps
write Lua** — see [lua.md](lua.md). This page documents KBY so the existing
`.kby` programs stay maintainable.

The KBY VM, assembler and opcode table live in `kby.c`, `tools/kbasm.c` and
`include/kby_ops.h`. The in-kernel assembler is `kas.c`; it shares the opcode
table with the host tool so the two cannot drift.

## Running one

```bash
kbyrun hello.kbin        # run for its output
open panel               # run in a window
kpm build hello.kby      # assemble /home/klye/hello.kby into /bin
```

`kpm build` assembles inside the kernel and produces byte-identical output to
`build/kbasm` on the host. This is verified for every program in the tree.

## The machine

A stack machine. Operands live on a 64-slot stack, each call frame has 32
private locals, and there are 64 nested call frames.

```
.nop
.push8 N      .push32 N       .pushstr "s"
.pop          .dup            .load I      .store I
.add .sub .mul .div .mod .cmp
.jmp L  .jz L  .jnz L  .call L  .ret  .halt  .num
.print  .println
.clear .rect .rounded .border .pixel .circle .line .text .textc
.vfs_exists .vfs_size .vfs_read .vfs_write .vfs_append
.key_poll .mouse_x .mouse_y .mouse_down
.ticks  .frame  .vsync  .win_open  .win_close
```

`.num` converts the value on top of the stack to a number, and `.textc` draws
a string centred on `x` instead of left-aligned from it.

Labels are `@name` on a line of their own, referenced as `@name`. Forward
references work.

## The four rules that will cost you an hour if you forget them

**1. Every draw op pops its colour from the stack, so push it first.**

```kby
.push32 0xFF6FD3FF      ; the colour comes first
.rounded 24 20 150 34 6
```

Push the colour *before* the op, not after. If you forget, the op silently
uses whatever colour was left over from the previous one and the whole UI comes
out in the wrong hues. This is by far the most common mistake.

**2. `.cmp` returns 1 when the top of the stack is less than the value below
it.**

```kby
.push8 2
.load 0        ; below
.cmp           ; 1 if 2 < n, i.e. the base case
.jnz @base
```

Note the operand order: the *top* is the right-hand comparison.

**3. `.sub` computes below minus top.**

```kby
.load 0
.push8 1
.sub           ; n - 1, not 1 - n
```

**4. The stack must balance by the end of every loop iteration.**

A leak does not error. It fills the 64-slot stack after a few dozen frames
and the app dies with `stack overflow`, which is a long way from the mistake
that caused it. If a loop pushes without popping, that is the bug.

`.vsync` yields for the rest of the frame. Put it at the end of a frame loop,
or the compositor will run the whole loop in one slice and the display list
will overflow. Filling the display list is not fatal — it yields too — but a
script without `.vsync` never actually animates.

## Draw ops take geometry inline

```kby
.rounded X Y W H RADIUS
.rect     X Y W H
.text     X Y "string"
.circle   X Y RADIUS
.line     X Y WIDTH
```

`X`/`Y` are added to the window origin. For `text`, `Y` is the **baseline**.

## Locals and calls

`.load I` / `.store I` read and write the *current frame's* locals, so
recursion is safe — each activation gets its own:

```kby
@fib
.store 0              ; n arrives on the stack, stash it
.push8 2
.load 0
.cmp
.jnz @base
.load 0
.push8 1
.sub
.call @fib            ; recursion
.store 1
...
.load 1
.load 2
.add
.ret
@base
.load 0
.ret
```

## Errors

A faulting program stops with a message naming the instruction, for example
`undefined label at pc 0x3a`, and a windowed program keeps its last good
frame. `kbyrun` prints the steps it took and the result on the stack.

## Writing new ones

The easiest path is to copy `rootfs/home/klye/kby/react.kby`, which shows
input, `.vsync` and a per-frame loop, and to change the drawing. Assemble
with `kpm build react.kby` from the terminal — no host toolchain needed.
