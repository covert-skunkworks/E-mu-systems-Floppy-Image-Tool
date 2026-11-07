# Tests

| Test | Needs | Run |
|---|---|---|
| `test_core.c`: image making, the transfers on a stand-in drive, names that must be refused | nothing | `make test` (CI, every system) |
| `-smoke`: the window starts and draws each page | a display, or xvfb | CI, every system |

The stand-in drive is an ordinary file of a floppy's size (`-fake FILE`; `:bad=N,N` makes sectors fail and `:flip=N,N` makes them
write wrongly). It is compiled in only with `FDR_TEST_DRIVE`, which only the test builds in `build/testbuild` define. The program
itself has neither it nor the scripted clicks, and `scripts/package.sh` refuses to pack a program that has them. No test touches a
real drive.
