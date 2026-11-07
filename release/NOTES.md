Floppy disks for E-mu samplers on EOS before its FAT32 support: make images, write them to a real floppy with a read-back
verify, and read real floppies, see what they hold and save them to images.

**New in 1.1:** the Read floppy page reads into memory first and lists what the floppy holds (files, presets, samples, sequences).
Saving it as an image is now a separate choice afterwards. `-info` gives the same list on the command line.

| Download | System |
|---|---|
| `floppy_image-*-windows-x64.zip` | Windows 10 / 11, 64-bit: double-click `floppy_image.exe` |
| `floppy_image-*-linux-x64.tar.gz` | Linux x86-64: needs SDL2 (`libsdl2-2.0-0`) |
| `floppy_image-*-macos-universal.zip` | macOS 11+, Apple Silicon and Intel: images only, no floppy drive access (not notarized: first run `xattr -dr com.apple.quarantine .` in the unpacked folder) |

**No firmware, banks or disk images are included.** This project is independent and not affiliated with E-mu Systems or
Creative Technology Ltd.
