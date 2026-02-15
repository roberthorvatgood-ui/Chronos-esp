
/*****
 * export_fs.cpp
 * Chronos – Export filesystem engine (CSV, list, delete, stats, purge, ZIP)
 * [Updated: 2026-01-19 22:55 CET]
 * [Updated: 2026-01-24 14:45 CET] FIX: shrink /api/list JSON payload (drop href/mode) to avoid parse failures
 *  - CS guard around all SD/FS touches (CH422G-based CS)
 *  - Exact signatures preserved (links from GUI)
 *****/
#include "export_fs.h"
#include "app_log.h"
#include <Arduino.h>
#include <FS.h>
#include <SD.h>      // SD.cardSize()
#include <time.h>
#include "chronos_sd.h"

namespace chronos {

static FS* g_fs = nullptr;

/* ---------- FS registration ---------------------------------------------- */
void exportfs_set_fs(FS* fs) { g_fs = fs; }
FS*  exportfs_get_fs()       { return g_fs; }
bool exportfs_begin()        { return g_fs != nullptr; }

/* ---------- time + path helpers ------------------------------------------ */
static String two(int v){ char b[8];  snprintf(b, sizeof b, "%02d", v); return b; }
static String four(int v){ char b[8]; snprintf(b, sizeof b, "%04d", v); return b; }

static void now_ymdhms(int& Y,int& M,int& D,int& h,int& m,int& s){
  struct tm t{};
  if (getLocalTime(&t)) {
    Y=t.tm_year+1900; M=t.tm_mon+1; D=t.tm_mday; h=t.tm_hour; m=t.tm_min; s=t.tm_sec;
    return;
  }
  // Fallback if RTC/LocalTime not ready
  unsigned long sec = millis()/1000UL;
  Y=2000; M=1; D=1; h=(sec/3600)%24; m=(sec/60)%60; s=sec%60;
}

/***** [Updated: 2026-01-19 22:55 CET]
 * [Updated: 2026-01-24 14:45 CET] FIX: shrink /api/list JSON payload (drop href/mode) to avoid parse failures Guard CS around exists/mkdir *****/
String exportfs_make_path(const char* mode){
  if (!g_fs) return "";
  int Y,M,D,h,mi,s; now_ymdhms(Y,M,D,h,mi,s);
  String date = four(Y) + "-" + two(M) + "-" + two(D);
  String dir  = "/exp/" + date;

  { // touch FS safely
    ChronosSdSelectGuard _sd;
    if (!g_fs->exists(dir)) g_fs->mkdir(dir);
  }

  String safe = mode ? String(mode) : String("Experiment");
  safe.replace("/", "_"); safe.replace(" ", "");
  String time = two(h) + "-" + two(mi) + "-" + two(s);
  return dir + "/Chronos_" + safe + "_" + date + "_" + time + ".csv";
}

/***** [Updated: 2026-01-19 22:55 CET]
 * [Updated: 2026-01-24 14:45 CET] FIX: shrink /api/list JSON payload (drop href/mode) to avoid parse failures CS guard around open/write/close *****/
/***** [Updated: 2026-01-19 23:59 CET] Self-healing save: ensure mount, robust open *****/
/***** [Updated: 2026-01-19 24:05 CET] Self-healing save; flush() is void on ESP32 core 3.3.5 *****/
String exportfs_save_csv(const char* mode, void (*emit_csv)(Print& out)){
  if (!emit_csv) {
    Serial.println("[Export] save aborted: emit_csv callback is null");
    return "";
  }

  // Mount-on-demand: if SD not ready yet, try once right now.
  if (!chronos_sd_is_ready()) {
    Serial.println("[Export] SD not mounted -> attempting mount now...");
    if (!chronos_sd_begin()) {
      Serial.println("[Export] mount failed -> save aborted");
      return "";
    }
    // chronos_sd_begin() already calls exportfs_set_fs(&SD); guard anyway:
    if (!g_fs) chronos::exportfs_set_fs(&SD);
  }

  // Build the target path and ensure /exp/<date> exists (done inside helper)
  String path = exportfs_make_path(mode);
  if (path.isEmpty()) {
    Serial.println("[Export] save aborted: make_path() returned empty");
    return "";
  }

  LOG_I("SD", "Saving CSV: %s", path.c_str());

  // Actual write guarded by CS (CH422G) for the entire open/write/close
  ChronosSdSelectGuard _sd;
  File f = g_fs->open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[Export] save failed: open('%s', FILE_WRITE) returned null\n", path.c_str());
    LOG_E("SD", "Write failed: %s", path.c_str());
    return "";
  }

  // Emit and close. Note: on ESP32, File::flush() returns void.
  emit_csv(f);
  f.flush();                      // no return value on this core
  size_t written = f.size();      // capture size before closing for diagnostics
  f.close();

  if (written == 0) {
    Serial.printf("[Export] warning: zero-length file after write -> %s (removing)\n", path.c_str());
    // Best-effort cleanup
    ChronosSdSelectGuard _sd2;
    (void)g_fs->remove(path);
    return "";
  }

  Serial.printf("[Export] saved CSV -> %s (%u bytes)\n", path.c_str(), (unsigned)written);
  LOG_I("SD", "CSV saved: %s", path.c_str());
  return path;
}



/* ---------- pretty size (used by JSON builder) --------------------------- */
String pretty_size(size_t bytes){
  const char* u[] = {"B","KB","MB"};
  float v = bytes; int i = 0;
  while (v >= 1024.f && i < 2) { v /= 1024.f; ++i; }
  char b[32];
  if (i == 0) snprintf(b, sizeof b, "%u %s", (unsigned)bytes, u[i]);
  else        snprintf(b, sizeof b, "%.1f %s", v, u[i]);
  return b;
}

/* ---------- safe listing builder (no STL allocations in hot paths) ------- */
#define CHR_MAX_DATES 24
#define CHR_MAX_FILES 256

struct FileEnt { String name; size_t size; };

static String  s_dates[CHR_MAX_DATES];
static FileEnt s_files[CHR_MAX_FILES];

static void sort_desc(String* arr, int n){
  for (int i = 1; i < n; ++i){
    String k=arr[i]; int j=i-1;
    while (j>=0 && arr[j] < k){ arr[j+1]=arr[j]; --j; }
    arr[j+1]=k;
  }
}

static void sort_files_desc(FileEnt* a, int n){
  for (int i = 1; i < n; ++i){
    FileEnt k=a[i]; int j=i-1;
    while (j>=0 && a[j].name < k.name){ a[j+1]=a[j]; --j; }
    a[j+1]=k;
  }
}

static int list_dates(FS* fs, String* out, int maxOut){
  int count=0;
  ChronosSdSelectGuard _sd;
  if (!fs || !fs->exists("/exp")) return 0;
  File root = fs->open("/exp");
  if (!root || !root.isDirectory()) return 0;
  for (File d = root.openNextFile(); d; d = root.openNextFile()){
    if (!d.isDirectory()) continue;
    String date = d.name();
    if (date.startsWith("/exp/")) date.remove(0,5);
    if (date.length()==0) continue;
    if (count<maxOut) out[count++]=date;
    yield();
  }
  return count;
}

static int list_files_for_date(FS* fs, const String& date, FileEnt* out, int maxOut){
  int count=0;
  ChronosSdSelectGuard _sd;
  File dir = fs->open("/exp/" + date);
  if (!dir || !dir.isDirectory()) return 0;
  for (File f=dir.openNextFile(); f; f=dir.openNextFile()){
    if (f.isDirectory()) continue;
    if (count<maxOut){
      out[count].name = f.name();
      out[count].size = (size_t)f.size();
      ++count;
    }
    if ((count & 15)==0) delay(0);
  }
  return count;
}

/***** [Updated: 2026-01-19 22:55 CET]
 * [Updated: 2026-01-24 14:45 CET] FIX: shrink /api/list JSON payload (drop href/mode) to avoid parse failures Guard CS while building JSON *****/
void exportfs_build_grouped_json(String& outJson){
  outJson = "{\"dates\":[";
  outJson.reserve(8192);

  if (!g_fs) { outJson += "]}"; return; }

  int nDates = list_dates(g_fs, s_dates, CHR_MAX_DATES);
  if (nDates > 1) sort_desc(s_dates, nDates);

  bool firstDate = true;
  for (int di=0; di<nDates; ++di){
    if (!firstDate) outJson += ","; firstDate = false;

    const String& date = s_dates[di];
    outJson += "{\"date\":\""; outJson += date; outJson += "\",\"files\":[";
    const int nFiles = list_files_for_date(g_fs, date, s_files, CHR_MAX_FILES);
    if (nFiles > 1) sort_files_desc(s_files, nFiles);

    bool firstFile = true;
    const String dirPath = "/exp/" + date;
    for (int i=0;i<nFiles;++i){
      if (!firstFile) outJson += ","; firstFile = false;

      String mode="Experiment";
      int p1 = s_files[i].name.indexOf("Chronos_");
      if (p1>=0){ int p2=s_files[i].name.indexOf('_',p1+8); if (p2>p1) mode = s_files[i].name.substring(p1+8,p2); }

      outJson += "{\"name\":\"";  outJson += s_files[i].name;
      outJson += "\",\"size\":\""; outJson += pretty_size(s_files[i].size);
      outJson += "\",\"bytes\":"; outJson += String((unsigned)s_files[i].size);
      outJson += ",\"mode\":\"";  outJson += mode;
      outJson += "\",\"href\":\"/dl?f="; outJson += dirPath; outJson += "/"; outJson += s_files[i].name; outJson += "\"}";
      if ((i & 15)==0) delay(0);
    }
    outJson += "]}";
    delay(0);
  }
  outJson += "]}";
}

void exportfs_emit_grouped_json(Print& out){
  String body; exportfs_build_grouped_json(body); out.print(body);
}

/* ---------- Delete helpers ------------------------------------------------ */
static bool is_under_exp(const String& p){
  if (!p.startsWith("/exp/")) return false;
  if (p.indexOf("..") >= 0)   return false;
  return true;
}

bool exportfs_delete_file(const String& absPath){
  if (!g_fs || !is_under_exp(absPath)) return false;
  ChronosSdSelectGuard _sd;
  if (!g_fs->exists(absPath)) return false;
  bool ok = g_fs->remove(absPath);
  LOG_I("SD", "Delete file: %s -> %s", absPath.c_str(), ok ? "OK" : "FAIL");
  return ok;
}

bool exportfs_delete_date(const String& date){
  if (!g_fs) return false;
  if (date.length()!=10) return false;
  String dirPath = "/exp/" + date;

  ChronosSdSelectGuard _sd;
  if (!g_fs->exists(dirPath)) return true;

  File dir = g_fs->open(dirPath);
  if (!dir || !dir.isDirectory()) return false;

  for (File f=dir.openNextFile(); f; f=dir.openNextFile()){
    if (f.isDirectory()) continue;
    String fp = dirPath + "/" + String(f.name());
    if (g_fs->exists(fp)) g_fs->remove(fp);
    delay(0);
  }
  bool ok = g_fs->rmdir(dirPath);
  LOG_I("SD", "Delete date: %s -> %s", date.c_str(), ok ? "OK" : "FAIL");
  return ok;
}

/* ---------- Status + purge ------------------------------------------------ */
bool exportfs_fs_stats(uint64_t& total, uint64_t& used)
{
  total = 0; used = 0;
  if (!g_fs) return false;

  ChronosSdSelectGuard _sd;
  uint64_t sectors = SD.cardSize();
  if (sectors == 0) return false;
  total = sectors * 512ULL;

  if (g_fs->exists("/exp")) {
    File root = g_fs->open("/exp");
    if (root && root.isDirectory()){
      for (File d = root.openNextFile(); d; d = root.openNextFile()){
        if (!d.isDirectory()) continue;
        String dayPath = String("/exp/") + d.name();
        File day = g_fs->open(dayPath);
        if (!day || !day.isDirectory()) continue;
        for (File f = day.openNextFile(); f; f = day.openNextFile()){
          if (!f.isDirectory()) used += f.size();
        }
      }
    }
  }
  return true;
}

bool exportfs_purge_oldest_until_free(uint64_t minFreeBytes, uint64_t* outFreed){
  if (outFreed) *outFreed = 0;
  if (!g_fs) return false;

  auto freeBytes = [&]()->uint64_t{
    uint64_t t=0,u=0; exportfs_fs_stats(t,u); return (t>u)? (t-u): 0ULL;
  };

  if (freeBytes() >= minFreeBytes) return true;

  int nDates = list_dates(g_fs, s_dates, CHR_MAX_DATES);
  if (nDates <= 0) return true;
  if (nDates > 1) sort_desc(s_dates, nDates); // newest-first

  for (int di = nDates-1; di >= 0; --di){      // oldest date first
    const String& date = s_dates[di];
    int nFiles = list_files_for_date(g_fs, date, s_files, CHR_MAX_FILES);
    if (nFiles > 1) sort_files_desc(s_files, nFiles); // newest-first

    for (int i = nFiles-1; i >= 0; --i){       // oldest file first
      String path = "/exp/" + date + "/" + s_files[i].name;
      size_t sz = s_files[i].size;
      ChronosSdSelectGuard _sd;
      if (g_fs->exists(path) && g_fs->remove(path)){
        if (outFreed) *outFreed += sz;
      }
      if (freeBytes() >= minFreeBytes) return true;
      if ((i & 7)==0) delay(0);
    }
    {
      ChronosSdSelectGuard _sd;
      g_fs->rmdir("/exp/" + date);
    }
    if (freeBytes() >= minFreeBytes) return true;
    delay(0);
  }
  uint64_t final_freed = outFreed ? *outFreed : 0;
  LOG_W("SD", "Purge: freed %llu bytes", (unsigned long long)final_freed);
  return (freeBytes() >= minFreeBytes);
}

/* ---------- CRC + ZIP helpers -------------------------------------------- */
#define CHR_MAX_ZIP_FILES CHR_MAX_FILES

static uint32_t crc_table[256]; static bool crc_inited=false;
static void crc32_init(){
  if (crc_inited) return;
  for (uint32_t i=0;i<256;++i){
    uint32_t c=i; for(int k=0;k<8;++k) c=(c&1)?(0xEDB88320U^(c>>1)):(c>>1);
    crc_table[i]=c;
  }
  crc_inited=true;
}
static uint32_t crc32_update(uint32_t crc, const uint8_t* buf, size_t len){
  crc^=0xFFFFFFFFU;
  for(size_t i=0;i<len;++i) crc=crc_table[(crc^buf[i])&0xFFU]^(crc>>8);
  return crc^0xFFFFFFFFU;
}
static uint16_t dos_time(){ struct tm t{}; if (getLocalTime(&t))
  return ((t.tm_hour&31)<<11)|((t.tm_min&63)<<5)|((t.tm_sec/2)&31); return 0; }
static uint16_t dos_date(){ struct tm t{}; if (getLocalTime(&t))
  return (((t.tm_year-80)&127)<<9)|(((t.tm_mon+1)&15)<<5)|(t.tm_mday&31); return 0; }
static void w16(File& f, uint16_t v){ uint8_t b[2]={(uint8_t)(v&0xFF),(uint8_t)(v>>8)}; f.write(b,2); }
static void w32(File& f, uint32_t v){ uint8_t b[4]={(uint8_t)(v&0xFF),(uint8_t)((v>>8)&0xFF),(uint8_t)((v>>16)&0xFF),(uint8_t)((v>>24)&0xFF)}; f.write(b,4); }
static String base_name(const String& n){ int p=n.lastIndexOf('/'); return (p>=0)? n.substring(p+1): n; }

struct FileEntZip { String name; size_t size; };
struct ZipEnt     { String name; size_t size; uint32_t crc; uint32_t lho; };

static FileEntZip s_zip_files[CHR_MAX_ZIP_FILES];
static ZipEnt     s_zip_ents [CHR_MAX_ZIP_FILES];
static uint8_t    s_zip_buf  [1024];

/* ---------- Stored ZIP in /exp/<date> (kept for compatibility) ----------- */
String exportfs_zip_date(const String& date, const char* zipName){
  if (!g_fs || date.length()!=10) return "";

  int nFiles=0;
  {
    ChronosSdSelectGuard _sd;
    File dir = g_fs->open("/exp/" + date);
    if (!dir || !dir.isDirectory()) return "";
    for (File f=dir.openNextFile(); f; f=dir.openNextFile()){
      if (f.isDirectory()) continue;
      if (nFiles<CHR_MAX_ZIP_FILES){
        s_zip_files[nFiles].name=base_name(String(f.name()));
        s_zip_files[nFiles].size=(size_t)f.size();
        ++nFiles;
      }
      if ((nFiles & 7)==0) delay(0);
    }
  }
  if (nFiles<=0) return "";

  // pass 1: CRC + sizes
  crc32_init();
  for (int i=0;i<nFiles;++i){
    String abs = "/exp/" + date + "/" + s_zip_files[i].name;
    ChronosSdSelectGuard _sd;
    File f = g_fs->open(abs, FILE_READ); if (!f) return "";
    uint32_t crc=0; size_t total=0;
    while (true){ size_t rd = f.read(s_zip_buf, sizeof s_zip_buf); if (!rd) break;
      crc=crc32_update(crc,s_zip_buf,rd); total+=rd; if ((total&0x7FFF)==0) delay(0); }
    f.close();
    s_zip_ents[i].name = s_zip_files[i].name;
    s_zip_ents[i].size = total;
    s_zip_ents[i].crc  = crc;
    s_zip_ents[i].lho  = 0;
  }

  // output name
  String zname;
  if (zipName && *zipName) zname = zipName;
  else {
    int Y,M,D,h,mi,s; now_ymdhms(Y,M,D,h,mi,s);
    zname = "Chronos_Zip_" + date + "_" + two(h)+"-"+two(mi)+"-"+two(s) + ".zip";
  }
  zname.replace("/", "_"); zname.replace("\\", "_");
  String zipPath = "/exp/" + date + "/" + zname;

  ChronosSdSelectGuard _sd;
  File z = g_fs->open(zipPath, FILE_WRITE); if (!z) return "";
  const uint16_t dt=dos_time(), dd=dos_date();
  uint32_t cd_start=0;

  // pass 2: local headers + data
  for (int i=0;i<nFiles;++i){
    const String& nm = s_zip_ents[i].name;
    const String  abs = "/exp/" + date + "/" + nm;
    s_zip_ents[i].lho = (uint32_t)z.position();
    w32(z,0x04034B50); w16(z,20); w16(z,0); w16(z,0);
    w16(z,dt); w16(z,dd);
    w32(z,s_zip_ents[i].crc);
    w32(z,(uint32_t)s_zip_ents[i].size);
    w32(z,(uint32_t)s_zip_ents[i].size);
    w16(z,(uint16_t)nm.length()); w16(z,0);
    z.write((const uint8_t*)nm.c_str(), nm.length());

    File f = g_fs->open(abs, FILE_READ); if (!f){ z.close(); g_fs->remove(zipPath); return ""; }
    while (true){ size_t rd = f.read(s_zip_buf, sizeof s_zip_buf); if (!rd) break;
      z.write(s_zip_buf, rd); if ((z.position() & 0x7FFF)==0) delay(0); }
    f.close();
  }

  // central directory
  cd_start = (uint32_t)z.position();
  for (int i=0;i<nFiles;++i){
    const String& nm = s_zip_ents[i].name;
    w32(z,0x02014B50); w16(z,20); w16(z,20); w16(z,0); w16(z,0);
    w16(z,dt); w16(z,dd);
    w32(z,s_zip_ents[i].crc);
    w32(z,(uint32_t)s_zip_ents[i].size);
    w32(z,(uint32_t)s_zip_ents[i].size);
    w16(z,(uint16_t)nm.length()); w16(z,0); w16(z,0);
    w16(z,0); w16(z,0); w32(z,0);
    w32(z,s_zip_ents[i].lho);
    z.write((const uint8_t*)nm.c_str(), nm.length());
    if ((i & 15)==0) delay(0);
  }
  uint32_t cd_end = (uint32_t)z.position();
  // EOCD
  w32(z,0x06054B50);
  w16(z,0); w16(z,0);
  w16(z,(uint16_t)nFiles); w16(z,(uint16_t)nFiles);
  w32(z, cd_end - cd_start); w32(z, cd_start);
  w16(z,0);
  z.flush(); z.close();

  return zipPath;
}

/* ---------- Temporary ZIP under /tmp (preferred for download) ------------- */
String exportfs_zip_temp_date(const String& date, String* outZipName)
{
  if (!g_fs || date.length()!=10) return "";

  // ensure /tmp exists; cull stale temp zips
  {
    ChronosSdSelectGuard _sd;
    if (!g_fs->exists("/tmp")) g_fs->mkdir("/tmp");
    File tmp = g_fs->open("/tmp");
    if (tmp && tmp.isDirectory()){
      for (File f = tmp.openNextFile(); f; f = tmp.openNextFile()){
        String n = f.name();
        if (!f.isDirectory() && n.endsWith(".zip"))
          g_fs->remove(String("/tmp/") + base_name(n));
      }
    }
  }

  // list files for the date
  int nFiles=0;
  {
    ChronosSdSelectGuard _sd;
    File dir = g_fs->open("/exp/" + date);
    if (!dir || !dir.isDirectory()) return "";
    for (File f=dir.openNextFile(); f; f=dir.openNextFile()){
      if (f.isDirectory()) continue;
      if (nFiles<CHR_MAX_ZIP_FILES){
        s_zip_files[nFiles].name=base_name(String(f.name()));
        s_zip_files[nFiles].size=(size_t)f.size();
        ++nFiles;
      }
      if ((nFiles & 7)==0) delay(0);
    }
  }
  if (nFiles<=0) return "";

  // pass 1: CRC + sizes
  crc32_init();
  for (int i=0;i<nFiles;++i){
    String abs = "/exp/" + date + "/" + s_zip_files[i].name;
    ChronosSdSelectGuard _sd;
    File f = g_fs->open(abs, FILE_READ); if (!f) return "";
    uint32_t crc=0; size_t total=0;
    while (true){ size_t rd = f.read(s_zip_buf, sizeof s_zip_buf); if (!rd) break;
      crc=crc32_update(crc,s_zip_buf,rd); total+=rd; if ((total&0x7FFF)==0) delay(0); }
    f.close();
    s_zip_ents[i].name = s_zip_files[i].name;
    s_zip_ents[i].size = total;
    s_zip_ents[i].crc  = crc;
    s_zip_ents[i].lho  = 0;
  }

  // temp zip name
  int Y,M,D,h,mi,s; now_ymdhms(Y,M,D,h,mi,s);
  String zname = "Chronos_TempZip_" + date + "_" + two(h)+"-"+two(mi)+"-"+two(s) + ".zip";
  if (outZipName) *outZipName = zname;
  String zipPath = "/tmp/" + zname;

  ChronosSdSelectGuard _sd;
  File z = g_fs->open(zipPath, FILE_WRITE); if (!z) return "";
  const uint16_t dt=dos_time(), dd=dos_date();
  uint32_t cd_start=0;

  // pass 2: local headers + data
  for (int i=0;i<nFiles;++i){
    const String& nm = s_zip_ents[i].name;
    const String  abs = "/exp/" + date + "/" + nm;
    s_zip_ents[i].lho = (uint32_t)z.position();
    w32(z,0x04034B50); w16(z,20); w16(z,0); w16(z,0);
    w16(z,dt); w16(z,dd);
    w32(z,s_zip_ents[i].crc);
    w32(z,(uint32_t)s_zip_ents[i].size);
    w32(z,(uint32_t)s_zip_ents[i].size);
    w16(z,(uint16_t)nm.length()); w16(z,0);
    z.write((const uint8_t*)nm.c_str(), nm.length());

    File f = g_fs->open(abs, FILE_READ); if (!f){ z.close(); g_fs->remove(zipPath); return ""; }
    while (true){ size_t rd = f.read(s_zip_buf, sizeof s_zip_buf); if (!rd) break;
      z.write(s_zip_buf, rd); if ((z.position() & 0x7FFF)==0) delay(0); }
    f.close();
  }

  // central directory
  cd_start = (uint32_t)z.position();
  for (int i=0;i<nFiles;++i){
    const String& nm = s_zip_ents[i].name;
    w32(z,0x02014B50); w16(z,20); w16(z,20); w16(z,0); w16(z,0);
    w16(z,dt); w16(z,dd);
    w32(z,s_zip_ents[i].crc);
    w32(z,(uint32_t)s_zip_ents[i].size);
    w32(z,(uint32_t)s_zip_ents[i].size);
    w16(z,(uint16_t)nm.length()); w16(z,0); w16(z,0);
    w16(z,0); w16(z,0); w32(z,0);
    w32(z,s_zip_ents[i].lho);
    z.write((const uint8_t*)nm.c_str(), nm.length());
    if ((i & 15)==0) delay(0);
  }
  uint32_t cd_end = (uint32_t)z.position();
  // EOCD
  w32(z,0x06054B50);
  w16(z,0); w16(z,0);
  w16(z,(uint16_t)nFiles); w16(z,(uint16_t)nFiles);
  w32(z, cd_end - cd_start); w32(z, cd_start);
  w16(z,0);
  z.flush(); z.close();

  return zipPath; // caller can stream + remove
}

} // namespace chronos
