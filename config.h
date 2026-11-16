#pragma once
// ---------------------------------------------------------------------------
// User configuration for the ESP32-P4 USB CD-ROM emulator
// Target board: Guition JC-ESP32P4-M3 (ESP32-P4, 16MB flash, 32MB PSRAM,
// ESP32-C6 wireless co-processor on SDIO slot 1 - unused here)
// ---------------------------------------------------------------------------

// ---- microSD (SDMMC slot 0, 4-bit) -----------------------------------------
// The P4 slot-0 IOMUX pins are CLK=43 CMD=44 D0=39 D1=40 D2=41 D3=42 and the
// card I/O is powered from the on-chip LDO channel 4. These are also the
// defaults of the "ESP32P4 Dev Module" variant. Change them only if your board
// is wired differently.
#define SD_PIN_CLK 43
#define SD_PIN_CMD 44
#define SD_PIN_D0 39
#define SD_PIN_D1 40
#define SD_PIN_D2 41
#define SD_PIN_D3 42
#define SD_LDO_CHANNEL 4    // -1 = card I/O is not powered from an on-chip LDO
#define SD_1BIT_MODE false  // true = use only D0 (slower, fewer pins)
#define SD_FREQ_KHZ 40000   // 40 MHz high-speed; falls back to 20 MHz on failure

// Folder on the card that holds the .iso files ("/" = card root).
#define ISO_DIR "/"

// ---- Read cache (PSRAM) ------------------------------------------------------
// Two buffers: one serves the host while the other is filled ahead of time.
#define CACHE_BUF_SIZE (512 * 1024)   // bytes per buffer (multiple of 2048)
#define CACHE_MISS_FILL (64 * 1024)   // first read on a random seek (fast reply)

// ---- USB identity --------------------------------------------------------------
#define USB_VENDOR_STR "Espressif"
#define USB_PRODUCT_STR "P4 Virtual CD-ROM"
#define SCSI_VENDOR "ESP32P4 "          // 8 chars
#define SCSI_PRODUCT "Virtual CD-ROM  "  // 16 chars
#define SCSI_REVISION "1.00"             // 4 chars

// Images larger than this many 2048-byte blocks are presented as a DVD-ROM
// instead of a CD-ROM (~800 MB, slightly above a 90-minute CD).
#define CD_MAX_BLOCKS 405000UL

// ---- USB disk mode (copy files from the PC to the microSD) -------------------
// Serial command "set-mode msc" makes the device appear as a read/write USB
// disk of the whole microSD card instead of a CD-ROM; "set-mode cdrom" returns.
#define DISK_PRODUCT "microSD Card    "  // 16 chars, SCSI product id in disk mode
#define DISK_WRITE_BUF (256 * 1024)      // write-combining buffer (PSRAM)
#define DISK_READ_AHEAD (64 * 1024)      // read-ahead buffer (PSRAM)

// ---- Auto-format ---------------------------------------------------------------
// If the card cannot be mounted because it holds no FAT filesystem, the old
// partition table is wiped, one full-size primary partition is created and it
// is formatted FAT32. ALL DATA ON THE CARD IS LOST.
#define AUTO_FORMAT 1  // 0 = never format automatically
// false: auto-format only cards with no partition table and no filesystem at
//        all (blank / garbage). Cards holding exFAT, NTFS, GPT or other
//        partitions are left alone - use the serial command "format YES".
// true : auto-format every card that has no FAT filesystem.
#define AUTO_FORMAT_FOREIGN true
#define SD_FORMAT_LABEL "CDROM"  // volume label (max 11 chars)
