#pragma once
#include <Arduino.h>

// Read-only access to an ISO file on the FAT32 microSD card.
//
// At open time the file's FAT cluster chain is converted into a list of
// contiguous extents, so data reads go straight to the card as large
// multi-sector transfers (no FAT walking per seek, no 2 GB VFS offset limit).
// Data is served from two PSRAM buffers; a background task prefetches the
// next chunk while the host consumes the current one.
namespace IsoImage {

bool begin();  // allocate buffers, start the prefetch task
bool open(const char *fatPath, const char *displayName);  // fatPath like "0:/foo.iso"
void close();
bool isOpen();
uint32_t blockCount();  // number of 2048-byte blocks
const char *name();

// Read `len` bytes starting at byte offset `pos` of the image.
bool read(uint64_t pos, void *dst, uint32_t len);

uint64_t bytesServed();
uint32_t cacheMisses();

}  // namespace IsoImage
