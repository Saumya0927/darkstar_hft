# Review loop — run this for every change

Not a milestone gate. Every change, however small. The two worst defects in this project
so far — the hash mixing that cost 7.7%, and the sequence-on-requeue bug that would have
broken the reference model — both looked trivial.

## 1. Implement precisely

Know what the code does at the machine level: what allocates, what is copied, what is
touched per cache line, what the compiler can and cannot see.

## 2. Test in all three configurations

```
for p in debug relassert release; do
  cmake --build --preset $p -j8 && ctest --preset $p
done
```

`debug` catches memory and UB errors (ASan/UBSan). `relassert` and `release` catch
anything that only appears optimised.

## 3. Mutation-test any new checker

**A test that has never failed has not been tested.** Deliberately break the thing the
checker is supposed to catch, confirm it fires with a useful message, then restore and
re-confirm green. Every checker in this repo has been through this.

## 4. Review the change

Correctness, efficiency, memory behaviour, exception safety, and whether the abstraction
still says something true.

## 5. Review the blast radius

What else does this touch? Callers, the reference model, the golden files, the property
accounting, the CMake dependency graph. A change is not local just because the diff is.

## 6. Measure, if it touches the hot path

Never claim a performance effect without numbers. A/B against the previous commit, best
of N, and state the configuration. Reverting is a normal outcome.

## 7. Leak-check

```
MallocStackLogging=1 leaks --atExit -- ./build-release/<binary> [args]
```

Must use a **non-sanitised** build: ASan replaces `malloc` and hides the heap from
`leaks`, and ASan's own LeakSanitizer is a silent no-op on arm64 macOS (verified — it
missed a deliberate 4 KB leak).

## 8. Report honestly

Including what was reverted and why. The record of what did not work is as valuable as
the code that did.

---

## Platform facts that constrain all of the above

Measured on the target machine. Do not re-derive; do not assume otherwise.

- **Timer granularity is 41.667 ns** (`mach_timebase_info` = 125/3). Every userspace clock
  is the same 24 MHz counter. A median may only come from batch timing; a per-event `p50`
  is not a valid number.
- **A timer read pair costs ~41 ns** and itself occasionally stalls 18 us. Wall clock
  cannot distinguish an engine stall from OS preemption — that needs per-thread PMU counters.
- **No CPU pinning exists on macOS.** QoS class is a hint, not a binding.
- **Valgrind does not run natively on arm64 macOS.**
- **libc++ `std::unordered_map` uses prime bucket counts**, so identity hashing of
  sequential integer keys is optimal. Do not mix the hash unless the table becomes
  power-of-two open-addressed.
