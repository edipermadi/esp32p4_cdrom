#include "sd_format.h"
#include "crlf_out.h"
#include "config.h"

#include "esp_heap_caps.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp32-hal-ldo.h"
#endif

namespace SdFormat {

static const uint32_t WORK_SIZE = 256 * 1024;

static sdmmc_card_t s_card;
static sdmmc_host_t s_host;
#if SOC_SDMMC_IO_POWER_EXTERNAL
static sd_pwr_ctrl_handle_t s_pwr;
#endif

// ---------------------------------------------------------------------------
// Low-level card bring-up (same configuration SD_MMC uses on the P4)
// ---------------------------------------------------------------------------
static bool cardOpen() {
  s_host = SDMMC_HOST_DEFAULT();
  s_host.slot = SDMMC_HOST_SLOT_0;
  s_host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  s_host.flags = SD_1BIT_MODE ? SDMMC_HOST_FLAG_1BIT : SDMMC_HOST_FLAG_4BIT;

  // Slot 0 uses the dedicated IOMUX pins; 0 = "not set" for the driver.
  sdmmc_slot_config_t slot = {};
  slot.cd = SDMMC_SLOT_NO_CD;
  slot.wp = SDMMC_SLOT_NO_WP;
  slot.width = SD_1BIT_MODE ? 1 : 4;

#if SOC_SDMMC_IO_POWER_EXTERNAL && SD_LDO_CHANNEL >= 0
  sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = SD_LDO_CHANNEL};
  ldoSdmmcPrepareAcquire(SD_LDO_CHANNEL);
  if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr) != ESP_OK) {
    ldoSdmmcDriverCreateFailed(SD_LDO_CHANNEL);
    s_pwr = nullptr;
    return false;
  }
  ldoSdmmcDriverAttached(SD_LDO_CHANNEL);
  s_host.pwr_ctrl_handle = s_pwr;
#endif

  if (sdmmc_host_init() != ESP_OK) goto fail_pwr;
  if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot) != ESP_OK) goto fail_host;
  if (sdmmc_card_init(&s_host, &s_card) != ESP_OK) goto fail_host;
  return true;

fail_host:
  sdmmc_host_deinit();
fail_pwr:
#if SOC_SDMMC_IO_POWER_EXTERNAL && SD_LDO_CHANNEL >= 0
  sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
  s_pwr = nullptr;
  ldoSdmmcDriverDetached(SD_LDO_CHANNEL);
#endif
  return false;
}

static void cardClose() {
  sdmmc_host_deinit();
#if SOC_SDMMC_IO_POWER_EXTERNAL && SD_LDO_CHANNEL >= 0
  if (s_pwr) {
    sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
    s_pwr = nullptr;
    ldoSdmmcDriverDetached(SD_LDO_CHANNEL);
  }
#endif
}

static uint32_t le32(const uint8_t *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
static void setLe32(uint8_t *p, uint32_t v) {
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

// Is there anything on the card that looks like data (partition table or a
// non-FAT boot sector)? `buf` is a scratch buffer of at least 512 bytes.
static bool hasForeignContent(uint8_t *buf) {
  if (sdmmc_read_sectors(&s_card, buf, 0, 1) != ESP_OK) return true;  // be safe
  if (!memcmp(buf + 3, "EXFAT   ", 8) || !memcmp(buf + 3, "NTFS    ", 8)) {
    Term.printf("SD: card holds an %.5s filesystem\n", buf + 3);
    return true;
  }
  if (buf[510] == 0x55 && buf[511] == 0xAA) {
    for (int i = 0; i < 4; i++) {
      const uint8_t *e = buf + 446 + 16 * i;
      if (e[4] != 0 && le32(e + 12) != 0) {
        Term.printf("SD: card has a partition table (partition %d type 0x%02X%s)\n", i + 1, e[4], e[4] == 0xEE ? " = GPT" : "");
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Partition + format
// ---------------------------------------------------------------------------
static bool partitionAndFormat(uint8_t *work, uint8_t pdrv) {
  const uint32_t total = s_card.csd.capacity;  // 512-byte sectors
  if (s_card.csd.sector_size != 512 || total < 8192) {
    Term.println("SD: unsupported card geometry");
    return false;
  }

  // Partition start: 4 MB boundary (like the SD Association formatter), smaller for tiny cards.
  const uint32_t start = total >= 131072 ? 8192 : 2048;
  const uint32_t count = total - start;

  // Cluster size: 32 KB if FAT32 allows it, else 4 KB, else let FatFS pick FAT12/16.
  BYTE fmt = FM_FAT32;
  DWORD au = 32768;
  uint8_t type = 0x0C;  // FAT32 LBA
  if ((uint64_t)count * 512 / 32768 < 66000) {
    au = 4096;
    if ((uint64_t)count * 512 / 4096 < 66000) {
      fmt = FM_FAT;
      au = 0;
      type = 0x06;  // FAT16
    }
  }
  Term.printf("SD: creating primary partition %lu MB at sector %lu, %s, %lu B clusters\n",
                (unsigned long)((uint64_t)count * 512 / (1024 * 1024)), (unsigned long)start, fmt == FM_FAT32 ? "FAT32" : "FAT16",
                (unsigned long)au);

  // 1) Drop every old partition: zero the first 1 MB (MBR, GPT header and
  //    entries, old boot sectors) and the last 64 sectors (backup GPT).
  memset(work, 0, WORK_SIZE);
  for (uint32_t s = 0; s < 2048; s += WORK_SIZE / 512) {
    if (sdmmc_write_sectors(&s_card, work, s, WORK_SIZE / 512) != ESP_OK) return false;
  }
  if (sdmmc_write_sectors(&s_card, work, total - 64, 64) != ESP_OK) return false;

  // 2) New MBR with a single primary partition spanning the card.
  uint8_t *mbr = work;
  memset(mbr, 0, 512);
  uint8_t *e = mbr + 446;
  e[0] = 0x00;                       // not bootable
  e[1] = 0xFE, e[2] = 0xFF, e[3] = 0xFF;  // CHS start: use LBA
  e[4] = type;
  e[5] = 0xFE, e[6] = 0xFF, e[7] = 0xFF;  // CHS end: use LBA
  setLe32(e + 8, start);
  setLe32(e + 12, count);
  setLe32(mbr + 440, esp_random());  // disk signature
  mbr[510] = 0x55;
  mbr[511] = 0xAA;
  if (sdmmc_write_sectors(&s_card, mbr, 0, 1) != ESP_OK) return false;

  // 3) Format partition 1. Temporarily bind the logical volume to it.
  char drv[4];
  snprintf(drv, sizeof(drv), "%u:", pdrv);
  PARTITION saved = VolToPart[pdrv];
  VolToPart[pdrv].pd = pdrv;
  VolToPart[pdrv].pt = 1;
  MKFS_PARM opt = {};
  opt.fmt = fmt;
  opt.n_fat = 2;
  opt.align = start >= 8192 ? 8192 : 64;  // align FAT/data area to the erase block
  opt.n_root = 0;
  opt.au_size = au;
  uint32_t t0 = millis();
  FRESULT fr = f_mkfs(drv, &opt, work, WORK_SIZE);
  if (fr == FR_OK) {
    FATFS *fs = (FATFS *)heap_caps_calloc(1, sizeof(FATFS), MALLOC_CAP_DEFAULT);
    if (fs && f_mount(fs, drv, 1) == FR_OK) {
      char label[16];
      snprintf(label, sizeof(label), "%s%s", drv, SD_FORMAT_LABEL);
      f_setlabel(label);
      f_mount(nullptr, drv, 0);
    }
    free(fs);
  }
  VolToPart[pdrv] = saved;
  if (fr != FR_OK) {
    Term.printf("SD: f_mkfs failed (%d)\n", fr);
    return false;
  }
  Term.printf("SD: format done in %lu ms\n", (unsigned long)(millis() - t0));
  return true;
}

Result run(bool force) {
  if (!cardOpen()) return CARD_ERROR;

  Result result = CARD_ERROR;
  BYTE pdrv = 0xFF;
  uint8_t *work = (uint8_t *)heap_caps_aligned_alloc(128, WORK_SIZE, MALLOC_CAP_SPIRAM);
  if (!work) work = (uint8_t *)heap_caps_aligned_alloc(64, WORK_SIZE, MALLOC_CAP_DMA);

  if (work && ff_diskio_get_drive(&pdrv) == ESP_OK && pdrv != 0xFF) {
    ff_diskio_register_sdmmc(pdrv, &s_card);
    char drv[4];
    snprintf(drv, sizeof(drv), "%u:", pdrv);

    bool doFormat = force;
    if (!force) {
      FATFS *fs = (FATFS *)heap_caps_calloc(1, sizeof(FATFS), MALLOC_CAP_DEFAULT);
      FRESULT fr = fs ? f_mount(fs, drv, 1) : FR_NOT_ENOUGH_CORE;
      if (fr == FR_OK) {
        f_mount(nullptr, drv, 0);
        result = HAS_FAT;
      } else if (fr == FR_NO_FILESYSTEM) {
        if (!AUTO_FORMAT_FOREIGN && hasForeignContent(work))
          result = FOREIGN;
        else
          doFormat = true;
      } else {
        Term.printf("SD: card read error (%d), not formatting\n", fr);
      }
      free(fs);
    }
    if (doFormat) result = partitionAndFormat(work, pdrv) ? FORMATTED : FORMAT_FAILED;

    ff_diskio_register(pdrv, nullptr);
  }
  heap_caps_free(work);
  cardClose();
  return result;
}

const char *describe(Result r) {
  switch (r) {
    case CARD_ERROR: return "card not responding";
    case HAS_FAT: return "card already FAT formatted";
    case FOREIGN: return "card has another filesystem/partitions - use 'format YES' to wipe it";
    case FORMATTED: return "card partitioned and formatted FAT32";
    case FORMAT_FAILED: return "formatting failed";
  }
  return "?";
}

}  // namespace SdFormat
