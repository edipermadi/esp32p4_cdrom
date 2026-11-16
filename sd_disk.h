#pragma once
#include <Arduino.h>

// Raw read/write access to the whole microSD card, used when the device is
// exposed to the PC as a USB disk. The card stays registered with FatFS (so
// its physical drive can be reached through diskio), but no FatFS calls are
// made while disk mode is active; the caller remounts afterwards.
namespace SdDisk {

bool begin(uint8_t pdrv, uint32_t sectorCount);  // start serving the card
void end();                                      // flush and stop
bool active();
uint32_t sectorCount();

// Byte-addressed I/O as requested by the USB stack (512-byte sectors).
int32_t read(uint32_t lba, uint32_t offset, void *buf, uint32_t len);
int32_t write(uint32_t lba, uint32_t offset, const uint8_t *buf, uint32_t len);
bool flush();  // commit buffered writes to the card

uint64_t bytesRead();
uint64_t bytesWritten();

}  // namespace SdDisk
