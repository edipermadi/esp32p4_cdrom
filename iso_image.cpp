#include "iso_image.h"
#include "config.h"

#include <vector>
#include <algorithm>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ff.h"
#include "diskio.h"

extern "C" DRESULT ff_disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);

#if FF_MAX_SS != FF_MIN_SS
#define FS_SECTOR_SIZE(fs) ((fs)->ssize)
#else
#define FS_SECTOR_SIZE(fs) FF_MAX_SS
#endif

namespace IsoImage {

static const uint64_t NONE = UINT64_MAX;
static const uint32_t BLOCK = 2048;

struct Extent {
  uint32_t firstCluster;  // index of the first cluster within the file
  uint32_t sector;        // absolute sector on the card
  uint32_t clusters;      // run length
};

struct Buf {
  uint8_t *data;
  uint64_t start;
  uint32_t len;
  uint32_t lastUse;
  bool ready;
};

static std::vector<Extent> s_extents;
static uint8_t s_pdrv;
static uint32_t s_sectorSize;
static uint32_t s_clusterBytes;
static uint64_t s_imageBytes;
static uint32_t s_blocks;
static volatile bool s_open;
static char s_name[256];

static Buf s_buf[2];
static uint32_t s_bufSize;
static uint32_t s_useCounter;
static uint64_t s_prefetchPos = NONE;

static SemaphoreHandle_t s_stateMtx;  // protects s_buf[] metadata, s_prefetchPos
static SemaphoreHandle_t s_ioMtx;     // serialises card reads and open/close
static TaskHandle_t s_prefetchTask;

static uint64_t s_bytesServed;  // statistics only
static uint32_t s_misses;

// ---------------------------------------------------------------------------
// Raw card access through the extent map
// ---------------------------------------------------------------------------
static bool readRaw(uint64_t pos, uint8_t *dst, uint32_t len) {
  while (len) {
    uint32_t clusterIdx = (uint32_t)(pos / s_clusterBytes);
    // last extent whose firstCluster <= clusterIdx
    auto it = std::upper_bound(s_extents.begin(), s_extents.end(), clusterIdx,
                               [](uint32_t v, const Extent &e) { return v < e.firstCluster; });
    if (it == s_extents.begin()) return false;
    const Extent &e = *(it - 1);
    uint64_t extentStart = (uint64_t)e.firstCluster * s_clusterBytes;
    uint64_t extentLen = (uint64_t)e.clusters * s_clusterBytes;
    uint64_t within = pos - extentStart;
    if (within >= extentLen) return false;
    uint32_t n = (uint32_t)std::min<uint64_t>(len, extentLen - within);
    uint32_t sector = e.sector + (uint32_t)(within / s_sectorSize);
    if (ff_disk_read(s_pdrv, dst, sector, n / s_sectorSize) != RES_OK) {
      log_e("card read failed at sector %lu (+%lu)", (unsigned long)sector, (unsigned long)(n / s_sectorSize));
      return false;
    }
    pos += n;
    dst += n;
    len -= n;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------
static bool covered(uint64_t pos) {
  for (auto &b : s_buf)
    if (b.ready && pos >= b.start && pos < b.start + b.len) return true;
  return false;
}

static int lruIndex() {
  return s_buf[0].lastUse <= s_buf[1].lastUse ? 0 : 1;
}

// Copies whatever part of [pos, pos+len) is cached at `pos`. Returns bytes copied.
static uint32_t tryCopy(uint64_t pos, uint8_t *dst, uint32_t len) {
  uint32_t n = 0;
  xSemaphoreTake(s_stateMtx, portMAX_DELAY);
  for (auto &b : s_buf) {
    if (!b.ready || pos < b.start || pos >= b.start + b.len) continue;
    uint32_t off = (uint32_t)(pos - b.start);
    n = std::min(len, b.len - off);
    memcpy(dst, b.data + off, n);
    b.lastUse = ++s_useCounter;

    // Past the middle of this buffer? Ask for the following chunk.
    uint64_t end = b.start + b.len;
    if (off + n >= b.len / 2 && end < s_imageBytes && !covered(end) && s_prefetchPos != end) {
      s_prefetchPos = end;
      xTaskNotifyGive(s_prefetchTask);
    }
    break;
  }
  xSemaphoreGive(s_stateMtx);
  return n;
}

// Fills the least recently used buffer from `start`. Caller holds s_ioMtx.
static bool fill(uint64_t start, uint32_t len) {
  xSemaphoreTake(s_stateMtx, portMAX_DELAY);
  Buf &b = s_buf[lruIndex()];
  b.ready = false;
  xSemaphoreGive(s_stateMtx);

  bool ok = readRaw(start, b.data, len);

  xSemaphoreTake(s_stateMtx, portMAX_DELAY);
  if (ok) {
    b.start = start;
    b.len = len;
    b.ready = true;
    b.lastUse = ++s_useCounter;
  }
  xSemaphoreGive(s_stateMtx);
  return ok;
}

static void prefetchTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreTake(s_ioMtx, portMAX_DELAY);
    xSemaphoreTake(s_stateMtx, portMAX_DELAY);
    uint64_t pos = s_prefetchPos;
    s_prefetchPos = NONE;
    bool skip = pos == NONE || !s_open || pos >= s_imageBytes || covered(pos);
    xSemaphoreGive(s_stateMtx);
    if (!skip) {
      uint32_t len = (uint32_t)std::min<uint64_t>(s_bufSize, s_imageBytes - pos);
      fill(pos, len);
    }
    xSemaphoreGive(s_ioMtx);
  }
}

static void invalidate() {
  xSemaphoreTake(s_stateMtx, portMAX_DELAY);
  for (auto &b : s_buf) {
    b.ready = false;
    b.lastUse = 0;
  }
  s_prefetchPos = NONE;
  xSemaphoreGive(s_stateMtx);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool begin() {
  s_stateMtx = xSemaphoreCreateMutex();
  s_ioMtx = xSemaphoreCreateMutex();

  s_bufSize = CACHE_BUF_SIZE;
  for (;;) {
    s_buf[0].data = (uint8_t *)heap_caps_aligned_alloc(128, s_bufSize, MALLOC_CAP_SPIRAM);
    s_buf[1].data = (uint8_t *)heap_caps_aligned_alloc(128, s_bufSize, MALLOC_CAP_SPIRAM);
    if (s_buf[0].data && s_buf[1].data) break;
    heap_caps_free(s_buf[0].data);
    heap_caps_free(s_buf[1].data);
    s_buf[0].data = s_buf[1].data = nullptr;
    if (s_bufSize <= 64 * 1024) break;
    s_bufSize /= 2;
  }
  if (!s_buf[0].data) {  // no PSRAM: small internal buffers
    s_bufSize = 32 * 1024;
    s_buf[0].data = (uint8_t *)heap_caps_aligned_alloc(64, s_bufSize, MALLOC_CAP_DMA);
    s_buf[1].data = (uint8_t *)heap_caps_aligned_alloc(64, s_bufSize, MALLOC_CAP_DMA);
    if (!s_buf[0].data || !s_buf[1].data) return false;
    log_w("PSRAM not available, using %lu byte internal buffers", (unsigned long)s_bufSize);
  }
  invalidate();
  return xTaskCreatePinnedToCore(prefetchTask, "iso_prefetch", 4096, nullptr, 5, &s_prefetchTask, 0) == pdPASS;
}

bool open(const char *fatPath, const char *displayName) {
  close();

  FIL fil;
  FRESULT fr = f_open(&fil, fatPath, FA_READ);
  if (fr != FR_OK) {
    log_e("f_open(%s) failed: %d", fatPath, fr);
    return false;
  }
  FATFS *fs = fil.obj.fs;
  uint32_t size = fil.obj.objsize;
  uint32_t ss = FS_SECTOR_SIZE(fs);
  uint32_t bcs = (uint32_t)fs->csize * ss;
  if (size < BLOCK || fil.obj.sclust < 2) {
    f_close(&fil);
    log_e("%s is too small", fatPath);
    return false;
  }

  // Walk the cluster chain. Seeking to the end of cluster k leaves fil.clust
  // on cluster k without reading any data sector.
  std::vector<Extent> ext;
  uint32_t nclust = (uint32_t)(((uint64_t)size + bcs - 1) / bcs);
  uint32_t t0 = millis();
  for (uint32_t k = 0; k < nclust; k++) {
    uint32_t ofs = (uint32_t)std::min<uint64_t>((uint64_t)(k + 1) * bcs, size);
    fr = f_lseek(&fil, ofs);
    if (fr != FR_OK || fil.clust < 2) {
      f_close(&fil);
      log_e("cluster walk failed at %lu: %d", (unsigned long)k, fr);
      return false;
    }
    uint32_t sector = (uint32_t)(fs->database + (LBA_t)fs->csize * (fil.clust - 2));
    if (!ext.empty()) {
      Extent &last = ext.back();
      if (last.sector + last.clusters * fs->csize == sector) {
        last.clusters++;
        continue;
      }
    }
    ext.push_back({k, sector, 1});
  }
  BYTE pdrv = fs->pdrv;
  f_close(&fil);

  xSemaphoreTake(s_ioMtx, portMAX_DELAY);
  s_extents.swap(ext);
  s_pdrv = pdrv;
  s_sectorSize = ss;
  s_clusterBytes = bcs;
  s_blocks = size / BLOCK;
  s_imageBytes = (uint64_t)s_blocks * BLOCK;
  strlcpy(s_name, displayName, sizeof(s_name));
  invalidate();
  s_bytesServed = 0;
  s_misses = 0;
  s_open = true;
  xSemaphoreGive(s_ioMtx);

  log_i("%s: %lu blocks, %u extent(s), cluster %lu B, mapped in %lu ms", displayName, (unsigned long)s_blocks,
        (unsigned)s_extents.size(), (unsigned long)bcs, (unsigned long)(millis() - t0));
  return true;
}

void close() {
  if (!s_ioMtx) return;
  xSemaphoreTake(s_ioMtx, portMAX_DELAY);
  s_open = false;
  invalidate();
  std::vector<Extent>().swap(s_extents);
  s_blocks = 0;
  s_imageBytes = 0;
  s_name[0] = 0;
  xSemaphoreGive(s_ioMtx);
}

bool isOpen() {
  return s_open;
}

uint32_t blockCount() {
  return s_open ? s_blocks : 0;
}

const char *name() {
  return s_name;
}

bool read(uint64_t pos, void *dstv, uint32_t len) {
  uint8_t *dst = (uint8_t *)dstv;
  if (!s_open || pos + len > s_imageBytes) return false;
  while (len) {
    uint32_t n = tryCopy(pos, dst, len);
    if (!n) {
      xSemaphoreTake(s_ioMtx, portMAX_DELAY);
      if (!s_open) {
        xSemaphoreGive(s_ioMtx);
        return false;
      }
      n = tryCopy(pos, dst, len);  // the prefetcher may have just delivered it
      if (!n) {
        uint64_t start = pos & ~(uint64_t)(BLOCK - 1);
        uint32_t want = (uint32_t)std::min<uint64_t>(std::min<uint32_t>(CACHE_MISS_FILL, s_bufSize), s_imageBytes - start);
        s_misses++;
        bool ok = fill(start, want);
        xSemaphoreGive(s_ioMtx);
        if (!ok) return false;
        continue;
      }
      xSemaphoreGive(s_ioMtx);
    }
    pos += n;
    dst += n;
    len -= n;
    s_bytesServed += n;
  }
  return true;
}

uint64_t bytesServed() {
  return s_bytesServed;
}

uint32_t cacheMisses() {
  return s_misses;
}

}  // namespace IsoImage
