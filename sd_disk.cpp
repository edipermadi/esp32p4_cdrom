#include "sd_disk.h"
#include "config.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ff.h"
#include "diskio.h"

extern "C" DRESULT ff_disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);
extern "C" DRESULT ff_disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);

namespace SdDisk {

static const uint32_t SECTOR = 512;

static SemaphoreHandle_t s_mtx;
static volatile bool s_active;
static uint8_t s_pdrv;
static uint32_t s_sectors;

// Write-behind buffer: consecutive writes are merged and sent to the card as
// one multi-block transfer (flushed at the end of every WRITE command).
static uint8_t *s_wbuf;
static uint32_t s_wbufSize;
static uint32_t s_wStart;  // first sector
static uint32_t s_wLen;    // bytes buffered

// Read-ahead buffer
static uint8_t *s_rbuf;
static uint32_t s_rbufSize;
static uint32_t s_rStart;
static uint32_t s_rLen;  // bytes valid (0 = empty)

static uint64_t s_bytesRead, s_bytesWritten;

static uint8_t *allocBuf(uint32_t &size) {
  for (; size >= 8 * 1024; size /= 2) {
    uint8_t *p = (uint8_t *)heap_caps_aligned_alloc(128, size, MALLOC_CAP_SPIRAM);
    if (!p) p = (uint8_t *)heap_caps_aligned_alloc(64, size, MALLOC_CAP_DMA);
    if (p) return p;
  }
  return nullptr;
}

static bool flushLocked() {
  if (!s_wLen) return true;
  bool ok = ff_disk_write(s_pdrv, s_wbuf, s_wStart, s_wLen / SECTOR) == RES_OK;
  if (!ok) log_e("card write failed at sector %lu (+%lu)", (unsigned long)s_wStart, (unsigned long)(s_wLen / SECTOR));
  s_wLen = 0;
  return ok;
}

bool begin(uint8_t pdrv, uint32_t sectorCount) {
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
  if (!s_wbuf) {
    s_wbufSize = DISK_WRITE_BUF;
    s_wbuf = allocBuf(s_wbufSize);
  }
  if (!s_rbuf) {
    s_rbufSize = DISK_READ_AHEAD;
    s_rbuf = allocBuf(s_rbufSize);
  }
  if (!s_wbuf || !s_rbuf || !sectorCount) return false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_pdrv = pdrv;
  s_sectors = sectorCount;
  s_wLen = 0;
  s_rLen = 0;
  s_bytesRead = s_bytesWritten = 0;
  s_active = true;
  xSemaphoreGive(s_mtx);
  return true;
}

void end() {
  if (!s_mtx) return;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  flushLocked();
  s_active = false;
  s_rLen = 0;
  xSemaphoreGive(s_mtx);
}

bool active() {
  return s_active;
}

uint32_t sectorCount() {
  return s_active ? s_sectors : 0;
}

bool flush() {
  if (!s_mtx) return true;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  bool ok = flushLocked();
  xSemaphoreGive(s_mtx);
  return ok;
}

int32_t read(uint32_t lba, uint32_t offset, void *bufv, uint32_t len) {
  uint8_t *buf = (uint8_t *)bufv;
  uint64_t addr = (uint64_t)lba * SECTOR + offset;
  if (!s_active || addr % SECTOR || len % SECTOR || addr + len > (uint64_t)s_sectors * SECTOR) return -1;

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  uint32_t sec = addr / SECTOR;
  uint32_t n = len / SECTOR;
  // Pending writes overlapping this range must reach the card first.
  if (s_wLen && sec < s_wStart + s_wLen / SECTOR && s_wStart < sec + n) flushLocked();

  bool ok = true;
  while (n && ok) {
    if (s_rLen && sec >= s_rStart && sec < s_rStart + s_rLen / SECTOR) {
      uint32_t avail = s_rStart + s_rLen / SECTOR - sec;
      uint32_t k = n < avail ? n : avail;
      memcpy(buf, s_rbuf + (sec - s_rStart) * SECTOR, k * SECTOR);
      buf += k * SECTOR;
      sec += k;
      n -= k;
      continue;
    }
    uint32_t fill = s_rbufSize / SECTOR;
    if (fill > s_sectors - sec) fill = s_sectors - sec;
    ok = ff_disk_read(s_pdrv, s_rbuf, sec, fill) == RES_OK;
    if (ok) {
      s_rStart = sec;
      s_rLen = fill * SECTOR;
    } else {
      s_rLen = 0;
      log_e("card read failed at sector %lu", (unsigned long)sec);
    }
  }
  if (ok) s_bytesRead += len;
  xSemaphoreGive(s_mtx);
  return ok ? (int32_t)len : -1;
}

int32_t write(uint32_t lba, uint32_t offset, const uint8_t *buf, uint32_t len) {
  uint64_t addr = (uint64_t)lba * SECTOR + offset;
  if (!s_active || addr % SECTOR || len % SECTOR || addr + len > (uint64_t)s_sectors * SECTOR) return -1;

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  uint32_t sec = addr / SECTOR;
  const uint32_t total = len;
  s_rLen = 0;  // drop read-ahead, it may now be stale
  bool ok = true;
  while (len && ok) {
    bool contiguous = s_wLen && sec == s_wStart + s_wLen / SECTOR;
    if (!contiguous || s_wLen == s_wbufSize) {
      ok = flushLocked();
      s_wStart = sec;
    }
    uint32_t room = s_wbufSize - s_wLen;
    uint32_t k = len < room ? len : room;
    memcpy(s_wbuf + s_wLen, buf, k);
    s_wLen += k;
    buf += k;
    len -= k;
    sec += k / SECTOR;
    s_bytesWritten += k;
  }
  xSemaphoreGive(s_mtx);
  return ok ? (int32_t)total : -1;
}

uint64_t bytesRead() {
  return s_bytesRead;
}

uint64_t bytesWritten() {
  return s_bytesWritten;
}

}  // namespace SdDisk
