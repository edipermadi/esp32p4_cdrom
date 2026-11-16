#pragma once
#include <Arduino.h>

// USB Mass Storage (bulk-only, SCSI transparent) interface that presents itself
// as an MMC CD-ROM / DVD-ROM drive (peripheral device type 5) backed by IsoImage.
namespace CdromUsb {

// Register the MSC interface. Must be called before USB.begin().
void begin();

// Tell the host a disc was inserted / removed. Call mediaInserted() after
// IsoImage::open() succeeded and mediaRemoved() before IsoImage::close().
void mediaInserted();
void mediaRemoved();
bool mediaPresent();

// True when the host has locked the tray (PREVENT MEDIUM REMOVAL).
bool locked();

// Host-initiated tray actions (START STOP UNIT with LoEj). Each call returns
// true once per request.
bool takeHostEject();
bool takeHostLoad();

// Disk mode: expose the raw microSD (SdDisk) as a removable read/write disk
// instead of the CD-ROM. Switch only while USB is disconnected.
void setDiskMode(bool on);
bool diskMode();
// True once after the PC ejected the disk ("Safely Remove Hardware").
bool takeDiskEjected();

}  // namespace CdromUsb
