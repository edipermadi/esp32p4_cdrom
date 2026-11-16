// ESP32-P4 USB CD-ROM emulator
// Board: Guition JC-ESP32P4-M3 (ESP32-P4, 16MB flash, 32MB PSRAM)
//
// Presents an .iso file from the microSD card (FAT32) as a read-only USB
// CD-ROM / DVD-ROM drive on the P4's high-speed USB OTG port.
//
// Arduino IDE settings (Tools menu):
//   Board            : ESP32P4 Dev Module
//   Chip Variant     : "Before v3.00" for rev 1.x chips (most JC-ESP32P4-M3 boards)
//   USB Mode         : USB-OTG (TinyUSB)
//   USB CDC On Boot  : Disabled   (the log/console stays on the UART USB-C port)
//   USB MSC On Boot  : Disabled   (this sketch provides its own MSC device)
//   Flash Size       : 16MB, PSRAM: Enabled
//
// Serial console (115200 baud, UART port): type "help". Line editing
// (backspace, arrow keys, history) uses linenoise and needs a VT100 terminal
// such as picocom, minicom, screen or idf.py monitor.
//
// USB disk mode: type "set-mode msc" and the board re-connects as a
// read/write USB disk of the whole microSD card, so ISOs can be copied from
// the PC. Use "Eject"/"Safely Remove" on the PC, then type "set-mode cdrom":
// the board re-connects as the CD-ROM with the updated ISO list. The mode is
// saved in NVS, so the board boots into whichever mode was set last.

#include <USB.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include "ff.h"
#include "tusb.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "linenoise/linenoise.h"

#include "config.h"
#include "iso_image.h"
#include "cdrom_usb.h"
#include "sd_disk.h"
#include "sd_format.h"

static std::vector<String> g_isos;  // file names inside ISO_DIR
static int g_current = -1;          // index into g_isos of the selected image
static char g_drive[4] = "0:";      // FatFS logical drive of the SD card
static uint8_t g_pdrv = 0;          // physical drive (diskio) of the SD card
static bool g_sdMounted = false;
static Preferences g_prefs;

// Lines typed on the console are handed from the console task to loop(), so
// every command runs on the same task as the USB/button handling.
static QueueHandle_t g_cmdQueue;      // char* (malloc'd by linenoise)
static SemaphoreHandle_t g_cmdDone;   // given by loop() after a command ran

// ---------------------------------------------------------------------------
// SD card
// ---------------------------------------------------------------------------
static bool mountSdOnce() {
  if (SD_1BIT_MODE)
    SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0);
  else
    SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0, SD_PIN_D1, SD_PIN_D2, SD_PIN_D3);
#if SD_LDO_CHANNEL >= 0
  SD_MMC.setPowerChannel(SD_LDO_CHANNEL);
#endif
  if (!SD_MMC.begin("/sdcard", SD_1BIT_MODE, false, SD_FREQ_KHZ, 2)) {
    Serial.println("SD: high-speed mount failed, retrying at 20 MHz");
    SD_MMC.end();
    if (!SD_MMC.begin("/sdcard", SD_1BIT_MODE, false, SDMMC_FREQ_DEFAULT, 2)) {
      SD_MMC.end();
      return false;
    }
  }
  // Find the FatFS logical drive the card was registered as.
  for (int i = 0; i < FF_VOLUMES; i++) {
    char root[8];
    snprintf(root, sizeof(root), "%d:/", i);
    FF_DIR dir;
    if (f_opendir(&dir, root) == FR_OK) {
      g_pdrv = dir.obj.fs->pdrv;
      f_closedir(&dir);
      snprintf(g_drive, sizeof(g_drive), "%d:", i);
      break;
    }
  }
  Serial.printf("SD: %llu MB card, FatFS drive %s\n", SD_MMC.cardSize() / (1024 * 1024), g_drive);
  g_sdMounted = true;
  return true;
}

// Mounts the card; if it has no FAT filesystem and AUTO_FORMAT is on, wipes
// the partition table, creates one full-size partition and formats it FAT32.
static bool mountSd() {
  if (mountSdOnce()) return true;
#if AUTO_FORMAT
  Serial.println("SD: mount failed, checking card ...");
  SdFormat::Result r = SdFormat::run(false);
  Serial.printf("SD: %s\n", SdFormat::describe(r));
  if (r == SdFormat::FORMATTED || r == SdFormat::HAS_FAT) return mountSdOnce();
#endif
  return false;
}

static void unmountSd() {
  SD_MMC.end();
  g_sdMounted = false;
}

static String fatPath(const String &file) {
  String p = String(g_drive) + ISO_DIR;
  if (!p.endsWith("/")) p += "/";
  return p + file;
}

static void scanIsos() {
  g_isos.clear();
  String dirPath = String(g_drive) + ISO_DIR;
  FF_DIR dir;
  FILINFO fi;
  if (f_opendir(&dir, dirPath.c_str()) != FR_OK) {
    Serial.printf("Cannot open folder %s\n", ISO_DIR);
    return;
  }
  while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
    if (fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
    String n = fi.fname;
    String lower = n;
    lower.toLowerCase();
    if (lower.endsWith(".iso") && !n.startsWith("._")) g_isos.push_back(n);
  }
  f_closedir(&dir);
  std::sort(g_isos.begin(), g_isos.end(), [](const String &a, const String &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
}

// ---------------------------------------------------------------------------
// Disc handling
// ---------------------------------------------------------------------------
static void ejectDisc() {
  CdromUsb::mediaRemoved();
  IsoImage::close();
}

static bool insertDisc(int index) {
  if (index < 0 || index >= (int)g_isos.size()) return false;
  bool hadDisc = CdromUsb::mediaPresent();
  if (hadDisc && CdromUsb::locked()) Serial.println("Note: host had locked the tray, forcing disc change");
  ejectDisc();
  // Leave the tray empty briefly so the host notices the change.
  if (hadDisc) delay(1500);

  const String &file = g_isos[index];
  Serial.printf("Loading %s ...\n", file.c_str());
  if (!IsoImage::open(fatPath(file).c_str(), file.c_str())) {
    Serial.println("  failed to open image");
    return false;
  }
  g_current = index;
  g_prefs.putString("iso", file);
  CdromUsb::mediaInserted();
  uint32_t blocks = IsoImage::blockCount();
  Serial.printf("  inserted: %lu MB, presented as %s\n", (unsigned long)((uint64_t)blocks * 2048 / (1024 * 1024)),
                blocks > CD_MAX_BLOCKS ? "DVD-ROM" : "CD-ROM");
  return true;
}

static int findIso(const String &name) {
  for (size_t i = 0; i < g_isos.size(); i++)
    if (g_isos[i].equalsIgnoreCase(name)) return i;
  return -1;
}

// ---------------------------------------------------------------------------
// Mode switching (CD-ROM <-> USB disk)
// ---------------------------------------------------------------------------
static void selectStartupDisc() {
  scanIsos();
  printList();
  int idx = findIso(g_prefs.getString("iso", ""));
  if (idx < 0 && !g_isos.empty()) idx = 0;
  if (idx >= 0) insertDisc(idx);
}

// Configures disk mode; USB must be disconnected (or not started yet).
static bool startDiskMode() {
  if (!g_sdMounted && !mountSd()) {
    Serial.println("Disk mode: microSD not available");
  } else if (!SdDisk::begin(g_pdrv, SD_MMC.numSectors())) {
    Serial.println("Disk mode: cannot start (out of memory?)");
  }
  CdromUsb::setDiskMode(true);
  if (!SdDisk::active()) return false;
  Serial.printf("USB disk mode: whole microSD (%lu MB) exposed to the PC\n",
                (unsigned long)((uint64_t)SdDisk::sectorCount() * 512 / (1024 * 1024)));
  return true;
}

static void enterDiskMode() {
  Serial.println("Switching to USB disk mode");
  g_prefs.putString("mode", "msc");  // survives reset / power cycle
  ejectDisc();
  tud_disconnect();
  delay(500);
  startDiskMode();
  tud_connect();
}

static void leaveDiskMode() {
  Serial.println("Switching to CD-ROM mode");
  g_prefs.putString("mode", "cdrom");
  tud_disconnect();
  delay(500);
  SdDisk::end();  // commits anything still buffered
  CdromUsb::setDiskMode(false);
  // Remount so FatFS sees what the PC changed.
  unmountSd();
  if (mountSd())
    selectStartupDisc();
  else
    Serial.println("ERROR: microSD mount failed (card must be FAT32)");
  tud_connect();
}

// ---------------------------------------------------------------------------
// Serial console
// ---------------------------------------------------------------------------
static void printList() {
  if (g_isos.empty()) {
    Serial.printf("No .iso files in %s\n", ISO_DIR);
    return;
  }
  uint64_t total = 0;
  for (size_t i = 0; i < g_isos.size(); i++) {
    FILINFO fi;
    uint64_t size = f_stat(fatPath(g_isos[i]).c_str(), &fi) == FR_OK ? fi.fsize : 0;
    total += size;
    Serial.printf("%c %2u  %8.1f MB  %s\n", (int)i == g_current && IsoImage::isOpen() ? '*' : ' ', (unsigned)i, size / 1048576.0,
                  g_isos[i].c_str());
  }
  Serial.printf("%u image(s), %.1f MB", (unsigned)g_isos.size(), total / 1048576.0);
  FATFS *fs;
  DWORD freeClust;
  if (f_getfree(g_drive, &freeClust, &fs) == FR_OK)
    Serial.printf(", %.1f MB free on card", (double)freeClust * fs->csize * 512 / 1048576.0);
  Serial.println(IsoImage::isOpen() ? "  (* = inserted)" : "");
}

// Re-reads the ISO folder, keeping g_current on the same file.
static void rescanKeepCurrent() {
  String cur = g_current >= 0 ? g_isos[g_current] : "";
  scanIsos();
  g_current = cur.length() ? findIso(cur) : -1;
}

// Splits "a b" or "\"a b\" c" style arguments; quotes allow spaces in names.
static std::vector<String> splitArgs(const String &s) {
  std::vector<String> out;
  size_t i = 0, n = s.length();
  while (i < n) {
    while (i < n && s[i] == ' ') i++;
    if (i >= n) break;
    String tok;
    if (s[i] == '"') {
      i++;
      while (i < n && s[i] != '"') tok += s[i++];
      i++;  // closing quote
    } else {
      while (i < n && s[i] != ' ') tok += s[i++];
    }
    out.push_back(tok);
  }
  return out;
}

// Image by index ("3") or file name (case-insensitive), -1 if not found.
static int resolveIso(const String &arg) {
  if (arg.length() && isDigit(arg[0]) && String(arg.toInt()) == arg) return arg.toInt() < (int)g_isos.size() ? arg.toInt() : -1;
  return findIso(arg);
}

static const char *fatError(FRESULT fr) {
  switch (fr) {
    case FR_NO_FILE: return "file not found";
    case FR_EXIST: return "a file with that name already exists";
    case FR_INVALID_NAME: return "invalid file name";
    case FR_DENIED: return "access denied (read-only or directory full)";
    case FR_WRITE_PROTECTED: return "card is write protected";
    case FR_DISK_ERR: return "card I/O error";
    case FR_NOT_READY: return "card not ready";
    default: return "FatFS error";
  }
}

static void removeIso(const String &arg) {
  int idx = resolveIso(arg);
  if (idx < 0) {
    Serial.println("No such image (use 'ls')");
    return;
  }
  String file = g_isos[idx];
  // The image's cluster list is cached while inserted, so it must be ejected
  // before its clusters are freed.
  if (idx == g_current) {
    ejectDisc();
    g_current = -1;
    Serial.println("Ejected the inserted image");
  }
  FRESULT fr = f_unlink(fatPath(file).c_str());
  if (fr != FR_OK) {
    Serial.printf("rm %s: %s (%d)\n", file.c_str(), fatError(fr), fr);
    return;
  }
  if (g_prefs.getString("iso", "") == file) g_prefs.remove("iso");
  Serial.printf("Deleted %s\n", file.c_str());
  rescanKeepCurrent();
}

static void renameIso(const String &from, String to) {
  int idx = resolveIso(from);
  if (idx < 0) {
    Serial.println("No such image (use 'ls')");
    return;
  }
  to.trim();
  if (!to.length() || to.indexOf('/') >= 0 || to.indexOf('\\') >= 0) {
    Serial.println("New name must be a plain file name (no folders)");
    return;
  }
  String lower = to;
  lower.toLowerCase();
  if (!lower.endsWith(".iso")) to += ".iso";
  String file = g_isos[idx];
  if (file == to) return;
  bool wasInserted = idx == g_current && IsoImage::isOpen();
  FRESULT fr = f_rename(fatPath(file).c_str(), fatPath(to).c_str());
  if (fr != FR_OK) {
    Serial.printf("mv %s: %s (%d)\n", file.c_str(), fatError(fr), fr);
    return;
  }
  Serial.printf("Renamed %s -> %s\n", file.c_str(), to.c_str());
  if (g_prefs.getString("iso", "") == file) g_prefs.putString("iso", to);
  String cur = g_current >= 0 ? (idx == g_current ? to : g_isos[g_current]) : "";
  scanIsos();
  g_current = cur.length() ? findIso(cur) : -1;
  // Re-insert so the host and 'info' see the new name.
  if (wasInserted && g_current >= 0) insertDisc(g_current);
}

static void printHelp(String topic) {
  topic.toLowerCase();
  if (topic == "mv" || topic == "rename") {
    Serial.println("mv <n|name> <new name>   (alias: rename)\n"
                   "  Renames an image. \".iso\" is added if missing. Use quotes for\n"
                   "  names with spaces:  mv 2 \"Ubuntu 24.04.iso\"\n"
                   "  An inserted image is re-inserted under its new name.");
  } else if (topic == "rm" || topic == "remove") {
    Serial.println("rm <n|name>   (alias: remove)\n"
                   "  Deletes an image from the card (cannot be undone). The inserted\n"
                   "  image is ejected first.  Example: rm 3   or   rm old.iso");
  } else if (topic == "set-mode" || topic == "msc" || topic == "mode") {
    Serial.println("set-mode msc | set-mode cdrom\n"
                   "  msc  : the PC sees the whole microSD as a USB disk - copy ISOs\n"
                   "         onto it (FAT32, into " ISO_DIR ").\n"
                   "  cdrom: back to the virtual CD-ROM. 'Eject' / 'Safely Remove' the\n"
                   "         disk on the PC first so all writes are finished.\n"
                   "  The mode is saved and restored after a reset / power cycle.");
  } else if (topic == "format") {
    Serial.println("format YES\n"
                   "  Wipes ALL partitions on the microSD and creates one FAT32\n"
                   "  partition labelled " SD_FORMAT_LABEL ". All data is lost.");
  } else {
    Serial.println(
        "ESP32-P4 virtual CD-ROM - copy .iso files to the microSD, then pick one\n"
        "to present to the PC as a CD/DVD drive.\n"
        "\n"
        "Images (cdrom mode)\n"
        "  ls                 list ISO images with size and index (alias: list)\n"
        "  mount <n|name>     insert an image (by index from 'ls' or file name)\n"
        "  next               insert the next image (also: BOOT button)\n"
        "  eject              remove the disc\n"
        "  load               re-insert the last image\n"
        "  rm <n|name>        delete an image (alias: remove)\n"
        "  mv <n|name> <new>  rename an image (alias: rename)\n"
        "\n"
        "Mode\n"
        "  get-mode           show the current mode (cdrom / msc)\n"
        "  set-mode msc       expose the microSD as a USB disk to copy files\n"
        "  set-mode cdrom     back to the virtual CD-ROM\n"
        "\n"
        "Other\n"
        "  info               status and statistics\n"
        "  format YES         erase the card and format it FAT32\n"
        "  help <command>     more about mv, rm, set-mode, format\n"
        "\n"
        "Typical use: set-mode msc -> copy ISOs on the PC -> eject the disk on\n"
        "the PC -> set-mode cdrom -> ls -> mount <n>");
  }
}

static void printInfo() {
  Serial.printf("USB      : %s\n", USB ? "connected" : "not connected");
  if (CdromUsb::diskMode()) {
    Serial.println("Mode     : USB disk (msc)");
    Serial.printf("Card     : %lu MB%s\n", (unsigned long)((uint64_t)SdDisk::sectorCount() * 512 / (1024 * 1024)),
                  SdDisk::active() ? "" : " (not available)");
    Serial.printf("Traffic  : %llu MB read, %llu MB written\n", SdDisk::bytesRead() / (1024 * 1024), SdDisk::bytesWritten() / (1024 * 1024));
    return;
  }
  Serial.println("Mode     : CD-ROM");
  if (IsoImage::isOpen()) {
    Serial.printf("Disc     : %s (%lu blocks)\n", IsoImage::name(), (unsigned long)IsoImage::blockCount());
  } else {
    Serial.println("Disc     : none");
  }
  Serial.printf("Tray lock: %s\n", CdromUsb::locked() ? "locked by host" : "unlocked");
  Serial.printf("Served   : %llu MB, %lu cache misses\n", IsoImage::bytesServed() / (1024 * 1024), (unsigned long)IsoImage::cacheMisses());
  Serial.printf("PSRAM    : %u KB free\n", (unsigned)(ESP.getFreePsram() / 1024));
}

static void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();

  bool discCmd = cmd == "format" || cmd == "ls" || cmd == "list" || cmd == "mount" || cmd == "insert" || cmd == "next" || cmd == "eject" || cmd == "load"
                 || cmd == "rescan" || cmd == "rm" || cmd == "mv"
                 || cmd == "remove" || cmd == "rename";
  if (discCmd && CdromUsb::diskMode()) {
    Serial.println("Not available in USB disk mode (use 'set-mode cdrom' first)");
    return;
  }
  bool needCard = cmd == "ls" || cmd == "list" || cmd == "mount" || cmd == "insert" || cmd == "rescan" || cmd == "rm" || cmd == "mv"
                 || cmd == "remove" || cmd == "rename";
  if (needCard && !g_sdMounted) {
    Serial.println("microSD not mounted (card inserted? try 'format YES')");
    return;
  }

  if (cmd == "help" || cmd == "?") {
    printHelp(arg);
  } else if (cmd == "ls" || cmd == "list") {
    rescanKeepCurrent();
    printList();
  } else if (cmd == "rm" || cmd == "remove") {
    std::vector<String> a = splitArgs(arg);
    if (a.size() != 1)
      Serial.println("Usage: rm <n|name>   (see 'help rm')");
    else
      removeIso(a[0]);
  } else if (cmd == "mv" || cmd == "rename") {
    std::vector<String> a = splitArgs(arg);
    if (a.size() != 2)
      Serial.println("Usage: mv <n|name> <new name>   (quote names with spaces, see 'help mv')");
    else
      renameIso(a[0], a[1]);
  } else if (cmd == "mount" || cmd == "insert") {
    if (arg.startsWith("\"") && arg.endsWith("\"") && arg.length() >= 2) arg = arg.substring(1, arg.length() - 1);
    int idx = resolveIso(arg);
    if (idx < 0)
      Serial.println("No such image (use 'ls')");
    else
      insertDisc(idx);
  } else if (cmd == "next") {
    if (!g_isos.empty()) insertDisc((g_current + 1) % g_isos.size());
  } else if (cmd == "eject") {
    ejectDisc();
    Serial.println("Disc ejected");
  } else if (cmd == "load") {
    if (g_current >= 0)
      insertDisc(g_current);
    else
      Serial.println("No previous image");
  } else if (cmd == "rescan") {
    rescanKeepCurrent();
    printList();
  } else if (cmd == "format") {
    if (arg != "YES") {
      Serial.println("This ERASES the whole microSD (all partitions) and formats it FAT32.\nType: format YES");
      return;
    }
    ejectDisc();
    g_isos.clear();
    g_current = -1;
    unmountSd();
    SdFormat::Result r = SdFormat::run(true);
    Serial.printf("SD: %s\n", SdFormat::describe(r));
    if (mountSdOnce()) {
      scanIsos();
      Serial.println("Card ready - use 'set-mode msc' to copy ISOs");
    } else {
      Serial.println("ERROR: card mount failed after format");
    }
  } else if (cmd == "set-mode") {
    arg.toLowerCase();
    if (arg == "msc") {
      if (CdromUsb::diskMode())
        Serial.println("Already in msc mode");
      else
        enterDiskMode();
    } else if (arg == "cdrom") {
      if (!CdromUsb::diskMode())
        Serial.println("Already in cdrom mode");
      else
        leaveDiskMode();
    } else {
      Serial.println("Usage: set-mode msc|cdrom");
    }
  } else if (cmd == "get-mode") {
    Serial.println(CdromUsb::diskMode() ? "msc" : "cdrom");
  } else if (cmd == "info" || cmd == "status") {
    printInfo();
  } else {
    Serial.println("Unknown command, type 'help'");
  }
}

// Blocking linenoise reader on its own task. stdin/stdout are routed through
// the UART driver that Serial installed, so both share one RX/TX path.
static void consoleTask(void *) {
  for (;;) {
    // Re-probe every prompt: a terminal attached after boot gets full line
    // editing, a plain serial monitor falls back to dumb mode (backspace only).
    linenoiseSetDumbMode(linenoiseProbe() != 0);
    char *line = linenoise(linenoiseIsDumbMode() ? "> " : "\x1b[1;32mcdrom>\x1b[0m ");
    if (!line) continue;
    if (line[0]) linenoiseHistoryAdd(line);
    xQueueSend(g_cmdQueue, &line, portMAX_DELAY);
    xSemaphoreTake(g_cmdDone, portMAX_DELAY);  // keep the prompt after the output
  }
}

static void startConsole() {
  Serial.flush();
  setvbuf(stdin, NULL, _IONBF, 0);
  uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CR);
  uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
  uart_vfs_dev_use_driver(UART_NUM_0);
  linenoiseSetMultiLine(1);
  linenoiseHistorySetMaxLen(20);
  linenoiseAllowEmpty(false);
  g_cmdQueue = xQueueCreate(1, sizeof(char *));
  g_cmdDone = xSemaphoreCreateBinary();
  xTaskCreate(consoleTask, "console", 6144, nullptr, 2, nullptr);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-P4 USB CD-ROM emulator ===");

#if BUTTON_PIN >= 0
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif
  g_prefs.begin("cdrom", false);

  if (!IsoImage::begin()) Serial.println("ERROR: cannot allocate read cache");

  bool sdOk = mountSd();
  if (!sdOk) Serial.println("ERROR: microSD mount failed (card inserted? FAT32 formatted?)");

  // USB comes up even without a card so the host sees an empty drive.
  CdromUsb::begin();
  // Start in the mode saved by the last 'set-mode' (default: cdrom).
  if (g_prefs.getString("mode", "cdrom") == "msc") {
    Serial.println("Saved mode: msc -> USB disk mode ('set-mode cdrom' to leave)");
    startDiskMode();
  } else if (sdOk) {
    selectStartupDisc();
  }
  USB.manufacturerName(USB_VENDOR_STR);
  USB.productName(USB_PRODUCT_STR);
  USB.begin();
  Serial.println("Type 'help' for commands.");
  startConsole();
}

void loop() {
  char *line;
  if (xQueueReceive(g_cmdQueue, &line, 0) == pdTRUE) {
    handleCommand(String(line));
    linenoiseFree(line);
    xSemaphoreGive(g_cmdDone);
  }

  if (CdromUsb::diskMode()) {
    if (CdromUsb::takeDiskEjected()) Serial.println("PC ejected the disk - all data written, safe to 'set-mode cdrom'");
    delay(5);
    return;
  }

  // Host pressed "Eject" / closed the tray.
  if (CdromUsb::takeHostEject()) {
    IsoImage::close();
    Serial.println("Host ejected the disc ('load' or BOOT button to re-insert)");
  }
  if (CdromUsb::takeHostLoad() && g_current >= 0) insertDisc(g_current);

#if BUTTON_PIN >= 0
  // BOOT button: cycle to the next image (re-insert if the tray is empty).
  static bool lastBtn = true;
  static uint32_t lastChange = 0;
  bool btn = digitalRead(BUTTON_PIN);
  if (btn != lastBtn && millis() - lastChange > 50) {
    lastChange = millis();
    lastBtn = btn;
    if (!btn && !g_isos.empty()) {
      if (!IsoImage::isOpen() && g_current >= 0)
        insertDisc(g_current);
      else
        insertDisc((g_current + 1) % g_isos.size());
    }
  }
#endif
  delay(5);
}
