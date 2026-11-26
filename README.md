# esp32p4_cdrom

ESP32-P4 based USB CD-ROM / mass-storage emulator.

Copy `.iso` files to a microSD card, plug the board into a PC, and choose an
image from the serial console. The PC sees a real optical drive with a disc
inserted, so you can boot OS installers or old software CDs, or install from
images, without burning discs. In a second mode the board exposes the whole
microSD card as a read/write USB disk, so new ISOs can be copied over the same
USB cable.

## Features

- **Virtual CD-ROM / DVD-ROM drive** over high-speed USB 2.0 (480 Mbit/s).
  The board reports itself as an MMC optical drive (SCSI peripheral type 5),
  not as a USB stick, so the PC handles it like a physical CD/DVD drive.
- **Automatic CD or DVD profile.** Images up to ~800 MB (405,000 blocks) are
  presented as CD-ROM and larger ones as DVD-ROM.
- **USB disk (MSC) mode** exposes the whole microSD as a removable read/write
  disk for copying ISOs from the PC.
- **Mode saved in NVS:** the board boots into whichever mode (`cdrom` / `msc`)
  was set last.
- **Last image remembered:** the ISO that was inserted is re-inserted after a
  reset or power cycle.
- **Serial console** with line editing (backspace, arrow keys, history) and
  TAB completion of commands and image names, for listing, inserting,
  deleting and renaming images.
- **Disc change and eject:** the host sees media-change events. The PC's
  "Eject" command and the host tray lock (prevent removal) are honoured.
- **Auto-format:** a blank or non-FAT card can be partitioned and formatted
  FAT32 automatically or with `format YES`.

## Hardware

| Item          | Specification                                                     |
|---------------|-------------------------------------------------------------------|
| Board         | Guition JC-ESP32P4-M3                                             |
| SoC           | ESP32-P4 (dual-core RISC-V, up to 360 MHz on rev 1.x, 400 MHz on v3) |
| Memory        | 16 MB flash, 32 MB PSRAM                                          |
| USB (device)  | ESP32-P4 USB 2.0 OTG high-speed port, carries the CD-ROM / disk   |
| USB (console) | USB-C UART port, 115200 baud                                      |
| Storage       | microSD on SDMMC slot 0, 4-bit, 40 MHz (20 MHz fallback)          |
| SD pins       | CLK 43, CMD 44, D0 39, D1 40, D2 41, D3 42, I/O power from LDO ch. 4 |

The ESP32-C6 co-processor on the board is not used. Pins and other settings
are in [`config.h`](config.h).

## Specifications

| Parameter            | Value                                                            |
|----------------------|------------------------------------------------------------------|
| USB identity         | "Espressif" / "P4 Virtual CD-ROM"                                |
| SCSI inquiry         | `ESP32P4 ` / `Virtual CD-ROM  ` / `1.00` (disk mode: `microSD Card`) |
| Device type (cdrom)  | MMC CD/DVD drive, removable, read-only                           |
| Device type (msc)    | Removable direct-access disk, read/write, 512-byte sectors       |
| Logical block size   | 2048 bytes (CD/DVD mode 1 data)                                  |
| MMC profiles         | CD-ROM (0x0008), DVD-ROM (0x0010)                                |
| Image format         | `.iso` (2048-byte sector, single data track)                     |
| Card filesystem      | FAT32 (images in the card root, `ISO_DIR`)                       |
| Max image size       | 4 GB − 1 byte (FAT32 file size limit)                            |
| Read cache           | 2 × 512 KB PSRAM buffers with background read-ahead              |
| Disk-mode buffers    | 256 KB write-combining + 64 KB read-ahead (PSRAM)                |

Supported MMC/SCSI commands include INQUIRY, READ(10/12), READ CD,
READ CAPACITY, READ TOC/PMA/ATIP (formats 0, 1, 2), GET CONFIGURATION,
GET EVENT STATUS NOTIFICATION, READ DISC/TRACK INFORMATION, MODE SENSE,
START STOP UNIT (eject/load), PREVENT ALLOW MEDIUM REMOVAL, READ DISC STRUCTURE,
MECHANISM STATUS and GET PERFORMANCE.

Image reads bypass the FatFS file layer. When an image is inserted, its FAT
cluster chain is converted into a list of contiguous extents. Host reads then
become large multi-sector transfers straight from the card, with no FAT walking
on a seek and no 2 GB VFS offset limit.

## Building

Arduino IDE or `arduino-cli` with the **esp32 core 3.3.x** (tested with 3.3.12).

| Tools menu option | Setting                                              |
|-------------------|------------------------------------------------------|
| Board             | ESP32P4 Dev Module                                   |
| Chip Variant      | "Before v3.00" for rev 1.x chips (most M3 boards)    |
| USB Mode          | USB-OTG (TinyUSB)                                    |
| USB CDC On Boot   | Disabled (the console stays on the UART port)        |
| USB MSC On Boot   | Disabled (the sketch provides its own MSC device)    |
| Flash Size        | 16MB                                                 |
| PSRAM             | Enabled                                              |

### Command line

The `Makefile` wraps `arduino-cli` with the settings above:

```
make                 # compile into ./build
make upload          # compile and flash over the UART USB-C port
make monitor         # serial console at 115200
make flash           # upload, then monitor
make info            # show FQBN and port
```

Override as needed: `make upload PORT=/dev/ttyUSB1 CHIP=postv3`.

## Usage

1. Format the microSD card FAT32 (or let the board do it, see `format YES`).
2. Connect the UART USB-C port to a terminal at 115200 baud. Use a VT100
   terminal for full line editing: `picocom`, `minicom`, `screen` or
   `idf.py monitor`. The Arduino Serial Monitor works too, but only with
   backspace editing.
3. Connect the high-speed USB port to the PC.

Typical workflow to add images:

```
cdrom> set-mode msc        # PC now sees the microSD as a USB disk
                           # copy .iso files to it, then "Eject" it on the PC
cdrom> set-mode cdrom      # back to the virtual CD-ROM
cdrom> ls
*  0     4812.3 MB  ubuntu-24.04.iso
   1      650.0 MB  win98.iso
2 image(s), 5462.3 MB, 25012.4 MB free on card  (* = inserted)
cdrom> mount 1
```

### Console commands

| Command                       | Description                                          |
|-------------------------------|------------------------------------------------------|
| `ls` / `list`                 | List ISO images with index and size, and free space  |
| `mount <n\|name>`             | Insert an image by index or file name                |
| `next`                        | Insert the next image (wraps around)                 |
| `prev`                        | Insert the previous image (wraps around)             |
| `eject`                       | Remove the disc                                      |
| `load`                        | Re-insert the last image                             |
| `rm <n\|name>` / `remove`     | Delete an image (the inserted one is ejected first)  |
| `mv <n\|name> <new>` / `rename` | Rename an image (`.iso` added if missing)          |
| `rescan`                      | Re-read the ISO folder                               |
| `set-mode msc`                | Expose the whole microSD as a USB disk               |
| `set-mode cdrom`              | Return to the virtual CD-ROM                         |
| `get-mode`                    | Print the current mode (`cdrom` or `msc`)            |
| `info`                        | Status and statistics                                |
| `format YES`                  | Wipe all partitions and format the card FAT32        |
| `help [command]`              | Command overview, or details for one command         |

Press TAB to complete command names, `set-mode` / `help` arguments and image
names (after `mount`, `rm`, `mv`, ...). With several matches, keep pressing
TAB to cycle through them. Put file names that contain spaces in quotes:
`mv 2 "Ubuntu 24.04.iso"`.

Image commands (`ls`, `mount`, `rm`, `mv`, ...) are only available in cdrom
mode. In msc mode the PC owns the card.

## Notes

- Always "Eject" / "Safely Remove" the disk on the PC before `set-mode cdrom`,
  so all writes are on the card.
- `format YES` and auto-format (`AUTO_FORMAT` in `config.h`) erase **all**
  data on the card.
- ISOs larger than 4 GB cannot be stored on FAT32. Split dual-layer DVD images
  are not supported.

## License

MIT, see [LICENSE](LICENSE).
