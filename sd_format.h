#pragma once
#include <Arduino.h>

// Partitions and formats the microSD card as FAT32:
//   1. wipes the old partition table (MBR, GPT primary + backup headers)
//   2. writes a new MBR with one primary partition covering the whole card
//      (starting at a 4 MB boundary for best SD performance)
//   3. formats that partition FAT32 (32 KB clusters) and sets the label
//
// Must be called while SD_MMC is NOT mounted; it drives the SDMMC host itself.
namespace SdFormat {

enum Result {
  CARD_ERROR,     // card did not respond / I/O error - nothing was written
  HAS_FAT,        // card already has a FAT filesystem - nothing was written
  FOREIGN,        // card holds another filesystem / partition table - nothing was written
  FORMATTED,      // card was partitioned and formatted
  FORMAT_FAILED,  // formatting started but failed
};

// force = false: format only a card that has no filesystem at all (or, with
//                AUTO_FORMAT_FOREIGN, any card without a FAT filesystem).
// force = true : format unconditionally (serial "format YES").
Result run(bool force);

const char *describe(Result r);

}  // namespace SdFormat
