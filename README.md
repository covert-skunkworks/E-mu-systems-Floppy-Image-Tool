# floppy_image

Floppy disks for E-mu samplers running EOS before its FAT32 support (FAT32 arrived with EOS 4.7): the Emulator IV family and
the Ultra models. A window program (C, SDL2, [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear)) that:

1. **makes floppy images**: blank, or holding files you pick;
2. **writes an image to a real floppy**, then reads it all back and compares;
3. **reads a real floppy to an image**, reporting any bad sectors.

> This project is independent. It is not affiliated with, endorsed by or connected to E-mu Systems or Creative Technology
> Ltd. E-mu, Emulator and EOS are trademarks of their owners, used here only to name the hardware. **No firmware, banks or disk
> images are included.**

## Which format to use

EOS has two floppy formats. Both are 1.44 MB, and every EOS tested reads both.

| | EOS floppy | DOS floppy |
|---|---|---|
| Holds | **one bank**, spanning several floppies if needed | several small files |
| Capacity | 1439 KB a floppy, up to 99 floppies (4 tested) | 1423 KB in all, no spanning |
| A PC can read it | no (no file system) | yes |
| EOS can make one | yes: Disk > Utils > Format on every version, then Save | not before 4.70 (Format makes only the first kind), but 2.51a on read and save to one made elsewhere |
| How EOS lists it | F000 Floppy Folder, B000 the bank's name | the label as the folder (4.70: F999 Root), banks numbered in directory order, 8.3 names |
| Use it for | anything, certainly any bank over 1.4 MB | a few small banks |

Formats that do **not** work, tried in the emulator on all seven versions:

- **720 KB floppies**, FAT or EOS format: "invalid folder" or "Unknown file type". The tool makes only 1.44 MB images. It can still
  copy a 720 KB floppy to and from a same-sized image, for archiving.
- **EMU3** (the EOS hard-disk file system) on a floppy: EOS does not recognise it and treats it as an empty floppy.

### What was tested

Each firmware ran in an E-mu EIV emulator (headless, with the image in its floppy drive), driven by scripted keys. Results:

| EOS | Machine | Blank floppy | EOS floppy load | DOS floppy load | Bank on 4 floppies |
|---|---|---|---|---|---|
| 2.51a | classic | identical | same | same | same |
| 2.80f | classic | identical | same | same | **fails**: "Sample Memory Full" at disk 3 |
| 3.00b | classic | identical | same | same | same |
| 4.10 | classic | identical | same | same | same |
| 4.62b | classic | identical | same | same | same |
| 4.10a | Ultra | identical | same | same | same |
| 4.70 | Ultra | identical | same | same | same |


- **Blank** compares a floppy formatted and saved by that EOS with the tool's image of the same (empty) bank, byte for byte.
- **Same** means all of sound memory, by hash, equals what a load of the same file from a hard disk leaves there: every sample
  arrived, in the right byte order. The single-floppy bank is a 102 KB bank with 2 samples, the 4-floppy one a 4.9 MB bank with 14.
  The images were made with this program; 4.70 is there for comparison.
- 70 banks on a DOS floppy list and load on 2.80f and later. **2.51a lists only the first 50**, the limit its manual gives; the
  window warns about this. The directory holds 224.
- A bank with samples, saved to a floppy by EOS 4.62 after a hard-disk load, matches the tool's images of that bank, spanned
  over 4 floppies too, but for about 6 bytes a sample that EOS rewrites as it loads. EOS asks for "Disk 2 of 4" and so on, in order.
- **EOS 2.80f and spanning:** single floppies are fine, but the 4-floppy load fails, and there is no baseline to judge it by: its own
  Save to floppy wrote only 12.9 KB of this 4.9 MB bank, without the samples. Keep banks to one floppy on 2.80f.
- **Banks that do not span:** two 16 MB banks tried (X-teme Lead, Eosmosis RAM) load from a hard disk, but on floppies EOS 4.62 stops
  with "Sample Memory Full" at disk 8 or 9. EOS's own Save of the first computes a garbage size ("Insert Disk 15 of ?"), and it cannot
  load back what it saved ("End of File Error"), so the tool's format is not the cause. They hold stale values in samples' unused
  parameters, the likely cause, not proven.
- No real sampler or real floppy has been used: see "Still to check on real hardware".

### Still to check on real hardware

Everything above ran on the original firmware in the emulator. These need a real drive or a real unit:

- The drive check: `floppy_image -drives` must list the PC's floppy drive. A USB floppy drive is expected to map to
  `\Device\Floppy<n>` on Windows, but this has not been seen.
- Reading a floppy, and writing one with its verify. Only the stand-in drive has been used so far. Linux drive access has not run
  on any real drive.
- **The sample byte order on a real unit:** make an EOS floppy of a bank with samples, load it, and listen. The firmware's behaviour is
  clear in the emulator, but the unit is the final word.
- That a floppy written on a PC's drive reads on the sampler's drive.

### Sample byte order

**A bank file cannot simply be copied onto a floppy.** On a hard disk EOS keeps samples little-endian, but on a floppy,
in either format, it keeps them big-endian. EOS runs on big-endian Motorola CPUs (68020, ColdFire) and moves floppy data to sound
memory as plain CPU words with no swap, which is the likely reason, and it writes floppies the same way. A bank copied unchanged lists and loads, and every sample plays as noise.

So the tool swaps the sample data when it makes an image, and says so. Consequences:

- A bank on a floppy made here is not byte-identical to the file you started from. On a DOS floppy, don't copy such a file
  back to a hard disk from a PC: load it on the sampler and save it from there.
- A bank file that came off a floppy is detected by its samples (audio moves in small steps, the wrong byte order in huge ones)
  and left alone. That check is a heuristic: a bank of pure noise could fool it.
- Only sample data is swapped: `E3S1` chunks from the lower of their start offsets to the chunk's end. Files that are not banks go
  on a DOS floppy untouched.

### The EOS floppy format

No file system. Sector 0 is a header; the bank's bytes run from sector 1 to the end of the floppy and on to the next. EOS's
Format writes only the tracks, so an empty floppy is all fill bytes (`55`), and they remain after the end of the bank.
Big-endian:

| Offset | Size | Field |
|---|---|---|
| `00` | 16 | `Emulator 4 Fdisk` |
| `10` | 16 | bank name, space padded |
| `20` | 4 | 0 |
| `24` | 4 | 1 |
| `28` | 4 | this disk, from 1 |
| `2C` | 4 | disks in the set |
| `30` | 4 | bank size in bytes |
| `34` | 4 | form type: `E4B0` or `E4Br` |
| `38` | 4 | 0 |
| `3C` | 4 | bank size in sectors, rounded up |
| `40` | 8 | 0 |
| `48` | 4 | bank size in sectors, again |

On EOS's own floppies the rest of the sector is a stale buffer on every disk after the first; the tool writes zeros there.

## Using it

Double-click the program. Files can be dropped on the window.

- **Make image:** choose the format and add the bank or files (nothing added: a blank floppy). The image is made in memory, and
  from there you choose: **Write to floppy...** goes straight to a floppy with no file on the PC, **Save as image file...** keeps a
  file, and you can do both with the same image. A bank that needs several floppies saves as `name_1of3.img`, `name_2of3.img` and so on.
  What was made stays in memory until you make another or close the program.
- **Write to floppy:** choose the drive, and what to write: what you just made, or an image file. **Write and verify...** names
  both in a box and asks first; its default is Cancel. After each disk of a set the page moves on to the next. The floppy must
  already be formatted: on the sampler, or on a PC.
- **Read floppy:** choose the drive and the image file, **Read floppy**. A sector that stays unreadable after retries is saved as
  zeros and listed with its cylinder, head and sector; the image is written anyway.

The same work from a command line (`floppy_image -help` lists it):

```bash
floppy_image -new eos -o bank.img MyBank.E4B
```
```bash
floppy_image -new dos -o small.img -label SAMPLES One.E4B Two.E4B
```
```bash
floppy_image -info bank_1of3.img
```
```bash
floppy_image -drives
```
```bash
floppy_image -read A: backup.img
```
```bash
floppy_image -write A: bank.img -yes
```

### Safety

This program writes raw to a disk, so what it will write to is restricted, and all these must pass:

- **Windows:** the drive letter must be `DRIVE_REMOVABLE` *and* map to `\Device\Floppy<n>`; USB sticks and memory cards map to
  `\Device\HarddiskVolume<n>` and are never listed. On opening, the geometry must be a 3.5" floppy type, 512-byte sectors, 2 heads,
  80 cylinders, 18 or 9 sectors a track. A hard disk or a USB stick cannot be opened by naming it either.
- **Linux:** `/dev/fd0` or `/dev/fd1` of the floppy major with a floppy geometry; or a USB floppy, `/dev/sdX`, only if sysfs says it is
  removable and exactly 2880 or 1440 sectors long, and it is not mounted.
- The image's size must equal the floppy's. Drives are opened read-only unless writing. On Windows a write first locks and
  dismounts the volume.
- Saving an image, and the `-smoke` screenshot, only ever writes to an ordinary file. A path that names a device (`/dev/sda`,
  `\\.\PhysicalDrive0`, `NUL`) or a folder is refused.
- The drive list holds only the PC's floppy drives. The tests' stand-in drive exists only in separate test builds
  (`make test`, `make testgui`), and `scripts/package.sh` refuses to pack a program that has it.

### Systems

| System | Images | Real floppy |
|---|---|---|
| Windows 10 / 11 | yes | yes; may need "Run as administrator" |
| Linux | yes | yes, `/dev/fdN` and USB floppies; built-in drives need the `floppy` group, USB floppies need root. Do not join the `disk` group: it also opens the hard disks |
| macOS | yes | **no.** Apple's USB floppy support could not be tried, and untried code has no place on a path that writes raw to a disk |

Ensoniq (800 KB, 1.6 MB) and Akai floppies use geometries a PC's floppy driver cannot read or write; they are out of scope.

## Building

Nuklear and the font are in `third_party/`; only SDL2 comes from the system.

| System | Needs | Build |
|---|---|---|
| Windows | [MSYS2](https://www.msys2.org): `pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2` | `make` in the UCRT64 shell |
| Linux | gcc, make, pkg-config, `libsdl2-dev` | `make` |
| macOS | Xcode command line tools, `brew install sdl2` | `make` |

`make test` runs the unit tests, which need neither SDL nor a drive. `tests/README.md` has the rest.

## Releases

Every push builds all three systems and runs the tests in GitHub Actions. To make a release:

```bash
bash scripts/release.sh 1.1 --push
```

That builds and tests this system's package, asks, then tags `v1.1` and pushes the tag; Actions builds all three systems and
publishes a GitHub Release. Packages hold the program, a README and licences.

## Licence

GPLv3, see [LICENSE](LICENSE). Built in: [SDL2](https://www.libsdl.org) (zlib), [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) (public domain / MIT),
the Droid Sans font (Apache 2.0).
