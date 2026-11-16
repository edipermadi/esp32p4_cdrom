#include "cdrom_usb.h"
#include "config.h"
#include "iso_image.h"
#include "sd_disk.h"

#include "esp32-hal-tinyusb.h"

// The Arduino core ships its own tud_msc_* callbacks in USBMSC.cpp. They are
// only linked when the USBMSC class is used, so this sketch must NOT use
// USBMSC / FirmwareMSC: the callbacks below replace them.

namespace CdromUsb {

enum : uint8_t { EVT_NONE = 0, EVT_EJECT_REQUEST = 1, EVT_NEW_MEDIA = 2, EVT_MEDIA_REMOVAL = 3 };

static volatile bool s_present;
static volatile bool s_unitAttention;
static volatile uint8_t s_mediaEvent;
static volatile bool s_prevent;
static volatile bool s_hostEject;
static volatile bool s_hostLoad;
static volatile bool s_diskMode;
static volatile bool s_diskEjected;

static const uint16_t PROFILE_NONE = 0x0000;
static const uint16_t PROFILE_CDROM = 0x0008;
static const uint16_t PROFILE_DVDROM = 0x0010;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static inline void put16(uint8_t *p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}
static inline void put24(uint8_t *p, uint32_t v) {
  p[0] = v >> 16;
  p[1] = v >> 8;
  p[2] = v;
}
static inline void put32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
static inline uint16_t get16(const uint8_t *p) {
  return (uint16_t)p[0] << 8 | p[1];
}
static inline uint32_t get24(const uint8_t *p) {
  return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}
static inline uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// Writes a 4-byte address field, either as LBA or as MSF (0, M, S, F).
static void putAddr(uint8_t *p, uint32_t lba, bool msf) {
  if (!msf) {
    put32(p, lba);
    return;
  }
  uint32_t l = lba + 150;
  p[0] = 0;
  p[1] = l / (75 * 60);
  p[2] = (l / 75) % 60;
  p[3] = l % 75;
}

static uint32_t blocks() {
  return s_present ? IsoImage::blockCount() : 0;
}

static bool isDvd() {
  return blocks() > CD_MAX_BLOCKS;
}

static uint16_t currentProfile() {
  if (!s_present) return PROFILE_NONE;
  return isDvd() ? PROFILE_DVDROM : PROFILE_CDROM;
}

static int32_t fail(uint8_t key, uint8_t asc, uint8_t ascq = 0) {
  tud_msc_set_sense(0, key, asc, ascq);
  return -1;
}
static int32_t failNoMedium() {
  return fail(SCSI_SENSE_NOT_READY, 0x3A, 0x00);
}
static int32_t failInvalidField() {
  return fail(SCSI_SENSE_ILLEGAL_REQUEST, 0x24, 0x00);
}
static int32_t failLbaRange() {
  return fail(SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
}

// Reads `count` 2048-byte blocks into `buffer` for READ(12) / READ CD, which go
// through the generic SCSI callback and so are limited to one buffer.
static int32_t readBlocks(uint32_t lba, uint32_t count, void *buffer, uint16_t bufsize) {
  if (!s_present) return failNoMedium();
  if ((uint64_t)lba + count > blocks()) return failLbaRange();
  if ((uint64_t)count * 2048 > bufsize) {
    log_w("read of %lu blocks via generic path exceeds %u byte buffer", (unsigned long)count, bufsize);
    return failInvalidField();
  }
  if (!IsoImage::read((uint64_t)lba * 2048, buffer, count * 2048)) return fail(SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
  return count * 2048;
}

// ---------------------------------------------------------------------------
// MMC command handlers. Each returns the full response length (callers clip
// to the allocation length) or a negative value on error.
// ---------------------------------------------------------------------------
static int32_t cmdReadToc(const uint8_t *cdb, uint8_t *b) {
  if (!s_present) return failNoMedium();
  bool msf = cdb[1] & 0x02;
  uint8_t format = cdb[2] & 0x0F;
  if (format == 0 && (cdb[9] & 0xC0)) format = cdb[9] >> 6;  // old SFF-8020 location
  uint8_t track = cdb[6];
  uint32_t n = 4;

  switch (format) {
    case 0:  // formatted TOC
      if (track > 1 && track != 0xAA) return failInvalidField();
      b[2] = 1;  // first track
      b[3] = 1;  // last track
      if (track <= 1) {
        uint8_t *d = b + n;
        memset(d, 0, 8);
        d[1] = 0x14;  // ADR=1, CONTROL=4 (data track)
        d[2] = 1;
        putAddr(d + 4, 0, msf);
        n += 8;
      }
      {
        uint8_t *d = b + n;
        memset(d, 0, 8);
        d[1] = 0x14;
        d[2] = 0xAA;  // lead-out
        putAddr(d + 4, blocks(), msf);
        n += 8;
      }
      break;

    case 1:  // multi-session info
      b[2] = 1;
      b[3] = 1;
      memset(b + 4, 0, 8);
      b[5] = 0x14;
      b[6] = 1;
      putAddr(b + 8, 0, msf);
      n = 12;
      break;

    case 2: {  // raw TOC (always MSF)
      b[2] = 1;
      b[3] = 1;
      struct {
        uint8_t point;
        uint8_t m, s, f;
      } ent[4];
      uint32_t lo = blocks() + 150;
      ent[0] = {0xA0, 1, 0x00, 0};  // first track number, disc type CD-ROM
      ent[1] = {0xA1, 1, 0, 0};     // last track number
      ent[2] = {0xA2, (uint8_t)(lo / 4500), (uint8_t)((lo / 75) % 60), (uint8_t)(lo % 75)};
      ent[3] = {0x01, 0, 2, 0};  // track 1 at 00:02:00
      for (auto &e : ent) {
        uint8_t *d = b + n;
        memset(d, 0, 11);
        d[0] = 1;     // session
        d[1] = 0x14;  // ADR/CONTROL
        d[3] = e.point;
        d[8] = e.m;
        d[9] = e.s;
        d[10] = e.f;
        n += 11;
      }
      break;
    }

    default: return failInvalidField();
  }
  put16(b, n - 2);
  return n;
}

static uint32_t addFeature(uint8_t *b, uint32_t n, uint8_t rt, uint16_t start, uint16_t code, uint8_t verFlags, const uint8_t *data,
                           uint8_t len) {
  if (code < start) return n;
  if (rt == 2 && code != start) return n;
  put16(b + n, code);
  b[n + 2] = verFlags;  // version << 2 | persistent << 1 | current
  b[n + 3] = len;
  if (len) memcpy(b + n + 4, data, len);
  return n + 4 + len;
}

static int32_t cmdGetConfiguration(const uint8_t *cdb, uint8_t *b) {
  uint8_t rt = cdb[1] & 0x03;
  uint16_t start = get16(cdb + 2);
  uint16_t profile = currentProfile();
  uint32_t n = 8;
  memset(b, 0, 8);
  put16(b + 6, profile);

  // 0x0000 Profile List
  {
    uint8_t d[8] = {0};
    put16(d, PROFILE_DVDROM);
    d[2] = profile == PROFILE_DVDROM;
    put16(d + 4, PROFILE_CDROM);
    d[6] = profile == PROFILE_CDROM;
    n = addFeature(b, n, rt, start, 0x0000, 0x03, d, 8);
  }
  // 0x0001 Core: physical interface = USB, DBE
  {
    uint8_t d[8] = {0, 0, 0, 0x08, 0x01, 0, 0, 0};
    n = addFeature(b, n, rt, start, 0x0001, 0x0B, d, 8);
  }
  // 0x0002 Morphing
  {
    uint8_t d[4] = {0x00, 0, 0, 0};
    n = addFeature(b, n, rt, start, 0x0002, 0x07, d, 4);
  }
  // 0x0003 Removable Medium: tray, eject, lock
  {
    uint8_t d[4] = {(uint8_t)(0x20 | 0x08 | 0x01 | (s_prevent ? 0x04 : 0)), 0, 0, 0};
    n = addFeature(b, n, rt, start, 0x0003, 0x03, d, 4);
  }
  if (profile != PROFILE_NONE) {
    // 0x0010 Random Readable
    uint8_t d[8] = {0};
    put32(d, 2048);
    put16(d + 4, profile == PROFILE_DVDROM ? 16 : 1);
    n = addFeature(b, n, rt, start, 0x0010, 0x01, d, 8);
  }
  if (profile == PROFILE_CDROM) {
    // 0x001E CD Read
    uint8_t d[4] = {0};
    n = addFeature(b, n, rt, start, 0x001E, 0x09, d, 4);
  }
  if (profile == PROFILE_DVDROM) {
    // 0x001F DVD Read
    n = addFeature(b, n, rt, start, 0x001F, 0x01, nullptr, 0);
  }
  // 0x0100 Power Management
  n = addFeature(b, n, rt, start, 0x0100, 0x03, nullptr, 0);

  put32(b, n - 4);
  return n;
}

static int32_t cmdGetEventStatus(const uint8_t *cdb, uint8_t *b) {
  if (!(cdb[1] & 0x01)) return failInvalidField();  // only polled mode
  uint8_t request = cdb[4];
  b[3] = 0x10;  // supported classes: media
  if (!(request & 0x10)) {
    put16(b, 2);
    b[2] = 0x80;  // NEA: no event available
    return 4;
  }
  put16(b, 6);
  b[2] = 0x04;  // media class
  uint8_t evt = s_mediaEvent;
  s_mediaEvent = EVT_NONE;
  b[4] = evt;
  b[5] = s_present ? 0x02 : 0x00;  // media present, door closed
  b[6] = 0;
  b[7] = 0;
  return 8;
}

static int32_t cmdReadDiscInfo(const uint8_t *cdb, uint8_t *b) {
  if (!s_present) return failNoMedium();
  if (cdb[1] & 0x07) return failInvalidField();  // only standard disc info
  memset(b, 0, 34);
  put16(b, 32);
  b[2] = 0x0E;  // last session complete, disc finalized
  b[3] = 1;     // first track
  b[4] = 1;     // sessions
  b[5] = 1;     // first track in last session
  b[6] = 1;     // last track in last session
  b[7] = 0x20;  // URU
  b[8] = 0x00;  // CD-DA or CD-ROM
  put32(b + 16, 0xFFFFFFFF);
  put32(b + 20, 0xFFFFFFFF);
  return 34;
}

static int32_t cmdReadTrackInfo(const uint8_t *cdb, uint8_t *b) {
  if (!s_present) return failNoMedium();
  uint8_t type = cdb[1] & 0x03;
  uint32_t addr = get32(cdb + 2);
  bool ok = (type == 0 && addr < blocks()) || (type == 1 && (addr == 1 || addr == 0xFF)) || (type == 2 && addr == 1);
  if (!ok) return failInvalidField();
  memset(b, 0, 36);
  put16(b, 34);
  b[2] = 1;     // track
  b[3] = 1;     // session
  b[5] = 0x04;  // data track, uninterrupted
  b[6] = 0x01;  // data mode 1
  put32(b + 24, blocks());
  put32(b + 28, blocks() - 1);
  return 36;
}

static uint32_t modePage(uint8_t page, bool changeable, uint8_t *p) {
  switch (page) {
    case 0x01:  // read error recovery
      memset(p, 0, 12);
      p[0] = 0x01;
      p[1] = 0x0A;
      if (!changeable) p[3] = 5;
      return 12;
    case 0x0D:  // CD device parameters
      memset(p, 0, 8);
      p[0] = 0x0D;
      p[1] = 0x06;
      if (!changeable) {
        p[5] = 60;  // seconds per minute
        p[7] = 75;  // frames per second
      }
      return 8;
    case 0x1A:  // power condition
      memset(p, 0, 12);
      p[0] = 0x1A;
      p[1] = 0x0A;
      return 12;
    case 0x2A:  // CD/DVD capabilities and mechanical status
      memset(p, 0, 22);
      p[0] = 0x2A;
      p[1] = 0x14;
      if (changeable) return 22;
      p[2] = 0x08 | 0x01;                                       // reads DVD-ROM, CD-R
      p[4] = 0x30;                                              // mode 2 form 1/2
      p[6] = 0x20 | 0x08 | 0x01 | (s_prevent ? 0x02 : 0x00);  // tray, eject, lock
      put16(p + 8, 8467);                                       // 48x (obsolete field)
      put16(p + 10, 256);                                       // volume levels
      put16(p + 12, 512);                                       // buffer size kB
      put16(p + 14, 8467);
      return 22;
    default: return 0;
  }
}

static int32_t cmdModeSense10(const uint8_t *cdb, uint8_t *b) {
  uint8_t pc = cdb[2] >> 6;
  uint8_t page = cdb[2] & 0x3F;
  if (pc == 3) return fail(SCSI_SENSE_ILLEGAL_REQUEST, 0x39, 0x00);  // saving not supported
  memset(b, 0, 8);
  uint32_t n = 8;
  if (page == 0x3F) {
    static const uint8_t all[] = {0x01, 0x0D, 0x1A, 0x2A};
    for (uint8_t pg : all) n += modePage(pg, pc == 1, b + n);
  } else {
    uint32_t len = modePage(page, pc == 1, b + n);
    if (!len) return failInvalidField();
    n += len;
  }
  put16(b, n - 2);
  return n;
}

static int32_t cmdReadSubChannel(const uint8_t *cdb, uint8_t *b) {
  bool msf = cdb[1] & 0x02;
  bool subq = cdb[2] & 0x40;
  uint8_t format = cdb[3];
  memset(b, 0, 16);
  b[1] = 0x15;  // no current audio status
  if (!subq) return 4;
  if (format != 1) return failInvalidField();
  uint8_t *d = b + 4;
  d[0] = 0x01;
  d[1] = 0x14;
  d[2] = 1;  // track
  d[3] = 1;  // index
  putAddr(d + 4, 0, msf);
  putAddr(d + 8, 0, msf);
  put16(b + 2, 12);
  return 16;
}

static int32_t cmdMechanismStatus(uint8_t *b) {
  memset(b, 0, 8);
  return 8;
}

static int32_t cmdGetPerformance(const uint8_t *cdb, uint8_t *b) {
  uint8_t type = cdb[10];
  memset(b, 0, 24);
  if (type != 0 || get16(cdb + 8) == 0) {
    put32(b, 4);
    return 8;
  }
  put32(b, 20);
  uint8_t *d = b + 8;
  put32(d + 0, 0);
  put32(d + 4, 8467);
  put32(d + 8, blocks() ? blocks() - 1 : 0);
  put32(d + 12, 8467);
  return 24;
}

static int32_t cmdReadDiscStructure(const uint8_t *cdb, uint8_t *b) {
  if (!s_present) return failNoMedium();
  if (!isDvd()) return fail(SCSI_SENSE_ILLEGAL_REQUEST, 0x30, 0x02);  // incompatible medium
  uint8_t format = cdb[7];
  if (format == 0x00) {  // physical format information
    memset(b, 0, 4 + 2048);
    put16(b, 2048 + 2);
    uint8_t *d = b + 4;
    d[0] = 0x01;  // DVD-ROM, version 1
    d[1] = 0x0F;  // 120 mm, no max rate specified
    d[2] = 0x01;  // single layer, embossed
    d[3] = 0x10;
    put32(d + 4, 0x030000);
    put32(d + 8, 0x030000 + blocks() - 1);
    return 4 + 2048;
  }
  if (format == 0x01) {  // copyright information: no protection
    memset(b, 0, 8);
    put16(b, 6);
    return 8;
  }
  return failInvalidField();
}

static int32_t cmdReportKey(const uint8_t *cdb, uint8_t *b) {
  if (!s_present) return failNoMedium();
  if (!isDvd()) return fail(SCSI_SENSE_ILLEGAL_REQUEST, 0x30, 0x02);
  if ((cdb[10] & 0x3F) != 0x08) return failInvalidField();  // only RPC state
  memset(b, 0, 8);
  put16(b, 6);
  b[4] = (4 << 3) | 5;  // no region set, 4 vendor resets, 5 user changes left
  b[5] = 0xFF;          // region mask: none
  b[6] = 0x01;          // RPC phase II
  return 8;
}

static int32_t cmdReadCd(const uint8_t *cdb, uint8_t *buffer, uint16_t bufsize) {
  uint8_t sectorType = (cdb[1] >> 2) & 0x07;
  uint32_t lba = get32(cdb + 2);
  uint32_t count = get24(cdb + 6);
  uint8_t flags = cdb[9];
  if (sectorType != 0 && sectorType != 2) return fail(SCSI_SENSE_ILLEGAL_REQUEST, 0x64, 0x00);  // illegal mode
  if ((flags & 0xF8) != 0x10) return failInvalidField();  // only user data is available
  if (count == 0) return 0;
  return readBlocks(lba, count, buffer, bufsize);
}

// Generic SCSI commands while acting as a plain removable disk.
static int32_t diskScsi(const uint8_t *cdb, uint8_t *b, uint16_t bufsize) {
  switch (cdb[0]) {
    case 0x35:  // SYNCHRONIZE CACHE(10)
      return SdDisk::flush() ? 0 : fail(SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);
    case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
    case 0x2F:  // VERIFY(10)
      return 0;
    case 0x5A: {  // MODE SENSE(10): header only, not write protected
      uint32_t alloc = get16(cdb + 7);
      memset(b, 0, 8);
      put16(b, 6);
      uint32_t n = 8;
      if (n > alloc) n = alloc;
      if (n > bufsize) n = bufsize;
      return n;
    }
    default: return fail(SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
  }
}

static bool diskReady() {
  return SdDisk::active() && !s_diskEjected;
}

}  // namespace CdromUsb

// ===========================================================================
// TinyUSB MSC callbacks
// ===========================================================================
using namespace CdromUsb;

static uint16_t msc_load_descriptor(uint8_t *dst, uint8_t *itf) {
  uint8_t str_index = tinyusb_add_string_descriptor(USB_PRODUCT_STR);
  uint8_t ep_num = tinyusb_get_free_duplex_endpoint();
  TU_VERIFY(ep_num != 0);
  uint8_t descriptor[TUD_MSC_DESC_LEN] = {
    TUD_MSC_DESCRIPTOR(*itf, str_index, ep_num, (uint8_t)(0x80 | ep_num), CFG_TUD_ENDPOINT_SIZE)};
  *itf += 1;
  memcpy(dst, descriptor, TUD_MSC_DESC_LEN);
  return TUD_MSC_DESC_LEN;
}

extern "C" {

uint8_t tud_msc_get_maxlun_cb(void) {
  return 1;
}

static void padCopy(uint8_t *dst, const char *src, size_t len) {
  memset(dst, ' ', len);
  memcpy(dst, src, strnlen(src, len));
}

uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t *resp, uint32_t bufsize) {
  (void)lun;
  (void)bufsize;
  resp->peripheral_qualifier = 0;
  resp->peripheral_device_type = s_diskMode ? 0x00 : 0x05;  // direct-access disk / CD-DVD device
  resp->is_removable = 1;
  resp->version = 0x05;  // SPC-3
  resp->response_data_format = 2;
  resp->additional_length = sizeof(scsi_inquiry_resp_t) - 5;
  padCopy(resp->vendor_id, SCSI_VENDOR, 8);
  padCopy(resp->product_id, s_diskMode ? DISK_PRODUCT : SCSI_PRODUCT, 16);
  padCopy(resp->product_rev, SCSI_REVISION, 4);
  return sizeof(scsi_inquiry_resp_t);
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
  (void)lun;
  padCopy(vendor_id, SCSI_VENDOR, 8);
  padCopy(product_id, s_diskMode ? DISK_PRODUCT : SCSI_PRODUCT, 16);
  padCopy(product_rev, SCSI_REVISION, 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  (void)lun;
  if (s_diskMode) {
    if (diskReady()) return true;
    tud_msc_set_sense(0, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return false;
  }
  if (s_unitAttention) {
    s_unitAttention = false;
    tud_msc_set_sense(0, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);  // medium may have changed
    return false;
  }
  if (!s_present) {
    tud_msc_set_sense(0, SCSI_SENSE_NOT_READY, 0x3A, 0x00);  // medium not present
    return false;
  }
  return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
  (void)lun;
  if (s_diskMode) {
    uint32_t sectors = diskReady() ? SdDisk::sectorCount() : 0;
    *block_count = sectors;
    *block_size = sectors ? 512 : 0;
    return;
  }
  uint32_t n = blocks();
  *block_count = n;
  *block_size = n ? 2048 : 0;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
  (void)lun;
  (void)power_condition;
  if (!load_eject) return true;
  if (s_diskMode) {
    if (!start && !s_diskEjected) {  // "Safely Remove" / eject from the PC
      SdDisk::flush();
      s_diskEjected = true;
    }
    return true;
  }
  if (!start) {
    if (s_prevent) {
      tud_msc_set_sense(0, SCSI_SENSE_ILLEGAL_REQUEST, 0x53, 0x02);  // removal prevented
      return false;
    }
    if (s_present) {
      s_present = false;
      s_mediaEvent = EVT_MEDIA_REMOVAL;
      s_hostEject = true;
    }
  } else if (!s_present) {
    s_hostLoad = true;
  }
  return true;
}

bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal, uint8_t control) {
  (void)lun;
  (void)control;
  s_prevent = prohibit_removal & 0x01;
  return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return s_diskMode;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  (void)lun;
  if (s_diskMode) {
    if (!diskReady()) return failNoMedium();
    int32_t r = SdDisk::read(lba, offset, buffer, bufsize);
    return r < 0 ? fail(SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00) : r;
  }
  if (!s_present) return failNoMedium();
  if (lba >= blocks()) return failLbaRange();
  if (!IsoImage::read((uint64_t)lba * 2048 + offset, buffer, bufsize)) return fail(SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
  return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  (void)lun;
  if (s_diskMode) {
    if (!diskReady()) return failNoMedium();
    int32_t r = SdDisk::write(lba, offset, buffer, bufsize);
    return r < 0 ? fail(SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00) : r;
  }
  return fail(SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);  // write protected
}

// End of a WRITE(10) command: commit the merged data before the next command.
void tud_msc_write10_complete_cb(uint8_t lun) {
  (void)lun;
  if (s_diskMode && !SdDisk::flush()) log_e("write flush failed");
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const cdb[16], void *buffer, uint16_t bufsize) {
  (void)lun;
  uint8_t *b = (uint8_t *)buffer;
  int32_t n;
  uint32_t alloc;
  if (s_diskMode) return diskScsi(cdb, b, bufsize);

  switch (cdb[0]) {
    case 0x43:  // READ TOC/PMA/ATIP
      alloc = get16(cdb + 7);
      n = cmdReadToc(cdb, b);
      break;
    case 0x46:  // GET CONFIGURATION
      alloc = get16(cdb + 7);
      n = cmdGetConfiguration(cdb, b);
      break;
    case 0x4A:  // GET EVENT STATUS NOTIFICATION
      alloc = get16(cdb + 7);
      n = cmdGetEventStatus(cdb, b);
      break;
    case 0x51:  // READ DISC INFORMATION
      alloc = get16(cdb + 7);
      n = cmdReadDiscInfo(cdb, b);
      break;
    case 0x52:  // READ TRACK INFORMATION
      alloc = get16(cdb + 7);
      n = cmdReadTrackInfo(cdb, b);
      break;
    case 0x5A:  // MODE SENSE(10)
      alloc = get16(cdb + 7);
      n = cmdModeSense10(cdb, b);
      break;
    case 0x42:  // READ SUB-CHANNEL
      alloc = get16(cdb + 7);
      n = cmdReadSubChannel(cdb, b);
      break;
    case 0xBD:  // MECHANISM STATUS
      alloc = get16(cdb + 8);
      n = cmdMechanismStatus(b);
      break;
    case 0xAC:  // GET PERFORMANCE
      alloc = 0xFFFF;
      n = cmdGetPerformance(cdb, b);
      break;
    case 0xAD:  // READ DISC STRUCTURE
      alloc = get16(cdb + 8);
      n = cmdReadDiscStructure(cdb, b);
      break;
    case 0xA4:  // REPORT KEY
      alloc = get16(cdb + 8);
      n = cmdReportKey(cdb, b);
      break;
    case 0xBE:  // READ CD
      alloc = 0xFFFF;
      n = cmdReadCd(cdb, b, bufsize);
      break;
    case 0xA8:  // READ(12)
      alloc = 0xFFFF;
      n = readBlocks(get32(cdb + 2), get32(cdb + 6), b, bufsize);
      break;

    case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL (if not handled by the stack)
      s_prevent = cdb[4] & 0x01;
      return 0;

    case 0x15:  // MODE SELECT(6)
    case 0x55:  // MODE SELECT(10)
    case 0xB6:  // SET STREAMING
      return bufsize;  // accept and ignore parameter data

    case 0x2B:  // SEEK(10)
    case 0x2F:  // VERIFY(10)
    case 0x35:  // SYNCHRONIZE CACHE
    case 0xBB:  // SET CD SPEED
    case 0xA7:  // SET READ AHEAD
      if (!s_present && cdb[0] == 0x2B) return failNoMedium();
      return 0;

    default:
      log_d("unsupported SCSI op 0x%02X", cdb[0]);
      return fail(SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);  // invalid command opcode
  }

  if (n < 0) return n;
  uint32_t len = n;
  if (len > alloc) len = alloc;
  if (len > bufsize) len = bufsize;
  return (int32_t)len;
}

}  // extern "C"

// ===========================================================================
// Public API
// ===========================================================================
namespace CdromUsb {

void begin() {
  tinyusb_enable_interface(USB_INTERFACE_MSC, TUD_MSC_DESC_LEN, msc_load_descriptor);
}

void mediaInserted() {
  s_present = true;
  s_unitAttention = true;
  s_mediaEvent = EVT_NEW_MEDIA;
}

void mediaRemoved() {
  if (s_present) s_mediaEvent = EVT_MEDIA_REMOVAL;
  s_present = false;
  s_unitAttention = true;
  s_prevent = false;
}

bool mediaPresent() {
  return s_present;
}

bool locked() {
  return s_prevent;
}

bool takeHostEject() {
  bool v = s_hostEject;
  s_hostEject = false;
  return v;
}

bool takeHostLoad() {
  bool v = s_hostLoad;
  s_hostLoad = false;
  return v;
}

void setDiskMode(bool on) {
  s_diskMode = on;
  s_diskEjected = false;
  s_unitAttention = false;
  s_mediaEvent = EVT_NONE;
  s_prevent = false;
}

bool diskMode() {
  return s_diskMode;
}

bool takeDiskEjected() {
  static bool reported;
  if (!s_diskEjected) {
    reported = false;
    return false;
  }
  if (reported) return false;
  reported = true;
  return true;
}

}  // namespace CdromUsb
