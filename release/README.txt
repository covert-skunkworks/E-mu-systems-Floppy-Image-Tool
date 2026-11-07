floppy_image: floppy disks for E-mu samplers running EOS before FAT32 support
(Emulator IV, E4X, E4XT, E-Synth, E6400, and the Ultra models).

Double-click the program. Three pages:

  Make image        A floppy image from a bank or a few files, or a blank one,
                    made in memory: write it straight to a floppy, save it as an
                    image file, or both.
                    EOS floppy: EOS's own format. One bank, on as many floppies
                    as it takes. Works from EOS 2.51a on.
                    DOS floppy: several small files. A PC can read it too.
  Write to floppy   Writes what you made, or an image file, to a real floppy, then
                    reads it all back and compares. It asks first, naming the
                    drive and the image.
  Read floppy       Saves a real floppy to an image file and reports any bad
                    sectors. The drive is opened read-only.

Only real floppy drives are ever offered: never hard disks, USB sticks or memory
cards. The floppy has to be formatted already, on the sampler or on a PC.

Windows: if Windows refuses access to the drive, run the program as administrator.
Linux: for a built-in drive, join the "floppy" group. For a USB floppy, run as root.
Do not join the "disk" group: it also gives access to your hard disks.
macOS: images only. Write them to floppy on Windows or Linux.

No firmware, banks or disk images are included.

This project is independent and not affiliated with E-mu Systems or Creative
Technology Ltd. E-mu, Emulator and EOS are trademarks of their owners.

Licence: GPLv3. Built in: SDL2 (zlib licence), Nuklear (public domain / MIT), the
Droid Sans font (Apache 2.0). See the licenses folder.
