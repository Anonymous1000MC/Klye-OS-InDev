# Lessons

Mistakes made here, so they are not made again. Mostly about how to find out
things, since that is where the time went.

## The test can be the bug

1. **A canary that reports through a syscall is not a canary.** Reporting each
   failure with `write(1,...)` uses rax, rdi, rsi and rdx -- three of which were
   themselves under test. The first failure destroyed the evidence for the four
   after it, so five reported failures were really one failure plus four of its
   own making. Report through a **memory operand** instead: no register is
   touched, so the count is the count. (`c9066ae`)

2. **`orb` is not a store.** The canary ORed a letter into a byte already
   holding `.`, and `0x2E | 0x47` and `0x2E | 0x49` are both `0x6F`. Two
   different registers were indistinguishable in the output, and the result read
   as corrupt memory rather than as two failures. Use `movb`. A reporting
   encoding that is not injective hides exactly the thing you are looking for.

3. **A value nobody checks is a value nobody knows.** The canary set `r10` and
   never compared it, so when I broke `r10` myself I did not notice and built
   reasoning on top of it. If the test sets a register, check it -- especially
   if your own code has started using it as scratch.

4. **Check every register the code touches, including the ones you added last.**
   Same bug as (3), reached from the other side.

## Measuring

5. **Print the values, not the verdict.** "These four are wrong" cost hours of
   theorising. "These four became X" would have answered it in one run. When a
   test says *that* something is wrong, the next step is *what* it became.

6. **Binary through the guest is a trap.** `write()` stops at NUL, so raw
   register dumps truncate, and biasing bytes to dodge NUL puts them in the
   0x40-0xFF range where they become multi-byte UTF-8 and your offset math
   silently breaks. Twice I read a number, believed it, and only caught it
   because it disagreed with the pass/fail table. **Print ASCII hex in-program.**

7. **Re-run before believing a fix.** Zeroing `edx` before `wrmsr` sounded
   obviously right and was equally wrong -- it dropped the high half of a
   pointer. The test caught it immediately. The fix that was right was derived,
   not guessed.

8. **A blank log is a failed harness, not a failed kernel.** `tr.py` has been
   typing commands that never reach the shell. When a result is suspiciously
   empty, check the harness before believing the negative.

## Reasoning

9. **A theory that matches the symptom exactly can still be wrong.** Claude's
   argument-marshalling theory predicted a `mov %r10, %r8` shift chain that
   produced precisely the failing set {rdi, rsi, rdx, r8}. The stub contains no
   such instruction. It had been reverse-engineered from my register list. Read
   the source before adopting a theory that fits -- especially one handed to
   you, and especially when the fit is this good.

10. **Its warning was still worth having.** The same message told me to test
    `r10`, which was genuinely broken. Take the parts of an AI's answer that
    name a measurement from the parts that assert a mechanism.

11. **A wrong fix that is obviously right is common.** `xorl %edx, %edx` before
    a 64-bit `wrmsr` reads as obviously correct. The base is `edx:eax` -- two
    32-bit halves, and `edx` is not spare, it is the top half.

12. **When the faulting instruction is innocent, the bug is in the path.** Three
    separate wrong explanations (bad bias, missing relocation, missing `mmap`)
    all assumed the loader was at fault. All three were in the twenty
    instructions after the register pops. The faulting instruction was doing
    exactly what it was written to do with a value the kernel had already
    destroyed.

13. **Do not present a guess as a measurement.** A canary failure count of five
    was reported as five specific registers before the encoding bug was found.
    The count was wrong and so was the attribution. The fix was to make the
    reporting injective and then re-read it.

## Getting unblocked

14. **Fix the test before reading the failure.** Two of the four return-path
    bugs were found immediately once the canary stopped lying; before that,
    hours went into theories that the canary was actively contradicting.

15. **An external tip is a lever, not a plan.** A well-scoped question -- with
    the facts, the ruled-out list, and one specific ask -- is worth sending. An
    open "what's wrong" reliably returns a plausible mechanism that fits the
    symptoms and contradicts the code. Narrow questions narrow answers.
