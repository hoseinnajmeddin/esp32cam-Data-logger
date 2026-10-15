#include <Arduino.h>
#include <SD_MMC.h>
#include <driver/uart.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/stream_buffer.h>
#include <atomic>
#include <algorithm>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "config.h"
#include "logic.h"
#include "file_open.h"

// One producer (RX task), one consumer (Arduino loop). SD calls only in consumer.
struct Block { uint64_t ms; int64_t epoch; uint16_t len; uint8_t bytes[256]; };
Block *pool = nullptr;
uint32_t slots = 0;
uint32_t readSlot = 0; // Consumer-owned index, independent of sequence-counter wrap.
std::atomic<uint32_t> head{0}, tail{0};
std::atomic<uint32_t> softwareDropped{0}, uartOverflow{0}, uartErrors{0}, mirrorDropped{0};
std::atomic<bool> stopped{false};
QueueHandle_t uartEvents = nullptr;
StreamBufferHandle_t mirrorStream = nullptr;
constexpr uart_port_t PORT = UART_NUM_0;
constexpr const char *INFO = "/sdcard/log-file-infirmation.txt";
constexpr const char *TMP = "/sdcard/log-file-infirmation.tmp";
constexpr const char *BACKUP = "/sdcard/log-file-infirmation.bak";
const char *loggerState = "WAITING";
int lastIoErrno = 0;
const char *lastFileState = "NONE";
char lastFile[160] = "none";
uint32_t session = 0, fileNumber = 0, maxDiskSession = 0;
uint64_t completedFiles = 0, logBytes = 0, incompleteFiles = 0;
uint64_t lastRx = 0, anchor = 0, startMs = 0, lastFlush = 0;
int64_t fileEpoch = 0;
bool sessionStarted = false;
FILE *logFile = nullptr;
uint64_t currentBytes = 0;
char currentPath[192] = {};
char ioBuffer[4096]; // Internal RAM; stdio batches small RX blocks into SD writes.

uint64_t nowMs() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }
void report(const char *msg) {
    uart_write_bytes(PORT, msg, strlen(msg));
    uart_write_bytes(PORT, "\r\n", 2);
}
bool exists(const char *p) { struct stat s {}; return stat(p, &s) == 0; }
bool syncFile(FILE *f) { return fflush(f) == 0 && fsync(fileno(f)) == 0 && !ferror(f); }
uint64_t freeBytes() {
    uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
    return total > used ? total - used : 0;
}

// Rebuild counts from disk on every boot, or if the information file disappears.
// .open.txt marks an unconfirmed/incomplete file and is never overwritten.
bool scanLogs() {
    File dir = SD_MMC.open("/logs");
    if (!dir || !dir.isDirectory()) return false;
    completedFiles = logBytes = incompleteFiles = 0;
    maxDiskSession = 0;
    uint32_t latestSession=0, latestNumber=0;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            const char *name = strrchr(f.name(), '/');
            name = name ? name + 1 : f.name();
            unsigned s=0, n=0; int pos=0;
            if (sscanf(name, "%u-%u-time_%n", &s, &n, &pos)==2 && pos>0 && s>0 && n>0) {
                size_t len = strlen(name);
                if (len >= 4 && strcmp(name + len - 4, ".txt") == 0) {
                    maxDiskSession = std::max(maxDiskSession, static_cast<uint32_t>(s));
                    logBytes += f.size();
                    if(s>latestSession || (s==latestSession && n>latestNumber)) {
                        latestSession=s; latestNumber=n;
                        snprintf(lastFile,sizeof(lastFile),"/logs/%s",name);
                        lastFileState=strstr(name,".open.txt")?"INCOMPLETE":"COMPLETE";
                    }
                    if (strstr(name, ".open.txt")) ++incompleteFiles;
                    else ++completedFiles;
                }
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    return true;
}
bool saveInfo() {
    char body[1800];
    uint64_t total=SD_MMC.totalBytes(), used=SD_MMC.usedBytes();
    int n=snprintf(body,sizeof(body),
        "format_version=2\r\n"
        "last_io_errno=%d\r\n"
        "completed_files=%llu\r\nincomplete_files=%llu\r\nlog_bytes=%llu\r\n"
        "card_total_bytes=%llu\r\ncard_used_bytes=%llu\r\ncard_free_bytes=%llu\r\n"
        "last_session=%lu\r\nlast_file_number=%lu\r\nlast_file=%s\r\n"
        "last_file_state=%s\r\nlogger_state=%s\r\n"
        "uart_overflow_events_since_boot=%lu\r\nuart_error_events_since_boot=%lu\r\n"
        "software_dropped_bytes_since_boot=%lu\r\nmonitor_dropped_bytes_since_boot=%lu\r\n"
        "calendar=Solar_Hijri\r\ntimezone=UTC+03:30\r\n",
        lastIoErrno,(unsigned long long)completedFiles,(unsigned long long)incompleteFiles,
        (unsigned long long)logBytes,(unsigned long long)total,(unsigned long long)used,
        (unsigned long long)(total>used?total-used:0),(unsigned long)session,
        (unsigned long)fileNumber,lastFile,lastFileState,loggerState,
        (unsigned long)uartOverflow.load(),(unsigned long)uartErrors.load(),
        (unsigned long)softwareDropped.load(),(unsigned long)mirrorDropped.load());
    if(n<=0 || n>=static_cast<int>(sizeof(body))) return false;
    FILE *f=fopen(TMP,"wb");
    if(!f) return false;
    bool ok=fwrite(body,1,n,f)==static_cast<size_t>(n) && syncFile(f);
    if(fclose(f)!=0) ok=false;
    if(!ok) return false;
    if(exists(BACKUP) && unlink(BACKUP)!=0) return false;
    if(exists(INFO) && rename(INFO,BACKUP)!=0) return false;
    return rename(TMP,INFO)==0;
}
void fail(const char *reason, int ioError = 0) {
    lastIoErrno=ioError;
    stopped.store(true);
    loggerState=reason;
    // Never label a failed write COMPLETE. Preserve bytes already on disk.
    if(logFile) { fclose(logFile); logFile=nullptr; lastFileState="INCOMPLETE"; }
    saveInfo(); // Best effort: a failed/removed SD may also reject this update.
    char msg[384]; snprintf(msg,sizeof(msg),"[LOGGER][STOP] %s errno=%d (%s) path=%s; restart after correcting fault",
        reason,lastIoErrno,lastIoErrno?strerror(lastIoErrno):"not recorded",currentPath);
    report(msg);
}
void makePath(char *out,size_t size,bool open,int64_t epoch) {
    char label[48]; timeLabel(epoch,label,sizeof(label));
    snprintf(out,size,"/sdcard/logs/%lu-%04lu-time_%s%s.txt",
             (unsigned long)session,(unsigned long)fileNumber,label,open?".open":"");
}
bool finishFile() {
    if(!logFile) return true;
    bool ok=syncFile(logFile);
    if(fclose(logFile)!=0) ok=false;
    logFile=nullptr;
    if(!ok) { fail("SD_WRITE_ERROR"); return false; }
    char finalPath[192]; makePath(finalPath,sizeof(finalPath),false,fileEpoch);
    if(exists(finalPath) || rename(currentPath,finalPath)!=0) {
        fail("SD_RENAME_ERROR"); return false;
    }
    snprintf(lastFile,sizeof(lastFile),"%s",finalPath+7); // Strip /sdcard mount prefix.
    lastFileState="COMPLETE";
    ++completedFiles;
    if(incompleteFiles) --incompleteFiles;
    if(!saveInfo()) { fail("INFO_WRITE_ERROR"); return false; }
    return true;
}
bool openFile(const Block &b) {
    if(!exists(INFO)) {
        if(!scanLogs()) { fail("SD_SCAN_ERROR"); return false; }
        if(!saveInfo()) { fail("INFO_WRITE_ERROR"); return false; }
    }
    // Include all queued payload as a conservative additional reservation.
    uint64_t queued=static_cast<uint32_t>(head.load()-tail.load())*256ULL;
    if(freeBytes() < cfg::FILE_BUDGET + cfg::CARD_RESERVE + queued) {
        fail("SD_FULL"); return false;
    }
    if(fileNumber==UINT32_MAX) { fail("NUMBER_LIMIT"); return false; }
    ++fileNumber;
    startMs=windowStart(anchor,b.ms,cfg::WINDOW_MS);
    fileEpoch=b.epoch ? b.epoch-static_cast<int64_t>((b.ms-startMs)/1000) : 0;
    makePath(currentPath,sizeof(currentPath),true,fileEpoch);
    // Exclusive fopen bypasses fdopen/F_GETFL, unsupported by this FAT VFS.
    errno=0;
    logFile=openExclusiveLog(currentPath);
    if(!logFile) { const int e=errno; fail("SD_OPEN_ERROR",e); return false; }
    setvbuf(logFile,ioBuffer,_IOFBF,sizeof(ioBuffer));
    currentBytes=0;
    ++incompleteFiles;
    snprintf(lastFile,sizeof(lastFile),"%s",currentPath+7);
    lastFileState="OPEN"; loggerState="RUNNING";
    if(!saveInfo()) { fail("INFO_WRITE_ERROR"); return false; }
    return true;
}
size_t consume(const Block &b) {
    bool fresh=!sessionStarted || newSession(lastRx,b.ms,cfg::SESSION_GAP_MS);
    if(fresh) {
        if(!finishFile()) return 0;
        if(session==UINT32_MAX) { fail("NUMBER_LIMIT"); return 0; }
        ++session; fileNumber=0; anchor=b.ms; sessionStarted=true;
    }
    lastRx=b.ms;
    if(logFile && b.ms>=startMs+cfg::WINDOW_MS && !finishFile()) return 0;
    if(!logFile && !openFile(b)) return 0;
    if(!fileEpoch && b.epoch) fileEpoch=b.epoch-static_cast<int64_t>((b.ms-startMs)/1000);
    if(currentBytes + b.len > cfg::FILE_BUDGET) { fail("FILE_BUDGET_EXCEEDED"); return 0; }
    size_t n=fwrite(b.bytes,1,b.len,logFile);
    currentBytes+=n; logBytes+=n;
    if(n!=b.len) fail("SD_WRITE_ERROR");
    return n;
}

void monitorTask(void *) {
    uint8_t b[512];
    for(;;) {
        size_t n=xStreamBufferReceive(mirrorStream,b,sizeof(b),portMAX_DELAY);
        if(n) uart_write_bytes(PORT,reinterpret_cast<const char *>(b),n);
    }
}
void rxTask(void *) {
    char line[384]; size_t lineLen=0; bool longLine=false;
    int64_t epochAnchor=0, lastObserved=0; uint64_t monoAnchor=0;
    Block b {}; uint32_t writeSlot=0;
    for(;;) {
        uart_event_t e;
        // Count hardware events; do not flush the RX ring (that discards more data).
        while(xQueueReceive(uartEvents,&e,0)==pdTRUE) {
            if(e.type==UART_FIFO_OVF || e.type==UART_BUFFER_FULL) uartOverflow.fetch_add(1);
            if(e.type==UART_FRAME_ERR || e.type==UART_PARITY_ERR) uartErrors.fetch_add(1);
        }
        int n=uart_read_bytes(PORT,b.bytes,sizeof(b.bytes),pdMS_TO_TICKS(10));
        if(n<=0) continue;
        b.ms=nowMs(); b.len=n;
        for(int i=0;i<n;i++) {
            char c=static_cast<char>(b.bytes[i]);
            if(c=='\n') {
                if(!longLine) {
                    line[lineLen]='\0'; int64_t epoch=0;
                    if(gpsEpoch(line,epoch) && epoch!=lastObserved) {
                        epochAnchor=epoch; lastObserved=epoch; monoAnchor=b.ms;
                    }
                }
                lineLen=0; longLine=false;
            } else if(!longLine) {
                if(lineLen<sizeof(line)-1) line[lineLen++]=c;
                else longLine=true;
            }
        }
        b.epoch=epochAnchor ? epochAnchor+(b.ms-monoAnchor)/1000 : 0;
        if(mirrorStream) {
            size_t mirrored=xStreamBufferSend(mirrorStream,b.bytes,n,0);
            mirrorDropped.fetch_add(n-mirrored);
        }
        if(stopped.load()) { softwareDropped.fetch_add(n); continue; }
        uint32_t h=head.load(std::memory_order_relaxed);
        if(static_cast<uint32_t>(h-tail.load(std::memory_order_acquire))>=slots) {
            softwareDropped.fetch_add(n); // Preserve old blocks; stop permanently until reset.
            stopped.store(true); continue;
        }
        pool[writeSlot]=b;
        writeSlot=(writeSlot+1)%slots;
        head.store(h+1,std::memory_order_release);
    }
}
void fatalBoot(const char *reason) {
    report(reason);
    for(;;) delay(1000);
}
void setup() {
    // Use IDF UART0 directly: RX/TX are independent directions, no Serial.begin().
    uart_config_t u {};
    u.baud_rate=cfg::BAUD; u.data_bits=UART_DATA_8_BITS; u.parity=UART_PARITY_DISABLE;
    u.stop_bits=UART_STOP_BITS_1; u.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;
    u.source_clk=UART_SCLK_APB;
    if(uart_param_config(PORT,&u)!=ESP_OK ||
       uart_set_pin(PORT,cfg::TX_PIN,cfg::RX_PIN,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE)!=ESP_OK ||
       uart_driver_install(PORT,cfg::UART_RX_BYTES,8192,64,&uartEvents,0)!=ESP_OK) {
        for(;;) delay(1000);
    }
    report("\r\n[LOGGER] v0.2.1: UART0 RX=3 TX=1 / SD 1-bit / 60s files / 5s flush");
    pinMode(4,OUTPUT); digitalWrite(4,LOW);
    if(!psramFound()) fatalBoot("[LOGGER][FATAL] PSRAM not found. No capture started.");
    size_t free=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest=heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t wanted=free>cfg::PSRAM_RESERVE?free-cfg::PSRAM_RESERVE:0;
    wanted=std::min(wanted,std::min(cfg::MAX_POOL_BYTES,largest));
    while(wanted>=cfg::MIN_POOL_BYTES) {
        slots=wanted/sizeof(Block);
        pool=static_cast<Block *>(heap_caps_malloc(slots*sizeof(Block),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(pool) break;
        wanted-=64*1024;
    }
    if(!pool) fatalBoot("[LOGGER][FATAL] Insufficient PSRAM for safe capture.");
    char msg[200];
    snprintf(msg,sizeof(msg),"[LOGGER] PSRAM total=%u free_before=%u pool=%u payload_max=%u",
             ESP.getPsramSize(),static_cast<unsigned>(free),static_cast<unsigned>(slots*sizeof(Block)),slots*256);
    report(msg);
    if(!SD_MMC.begin("/sdcard",true,false,cfg::SD_FREQ_KHZ,5))
        fatalBoot("[LOGGER][FATAL] SD mount failed. FAT32 / power / contacts; NO automatic format.");
    if(!SD_MMC.exists("/logs") && !SD_MMC.mkdir("/logs")) fatalBoot("[LOGGER][FATAL] mkdir failed.");
    if(!scanLogs()) fatalBoot("[LOGGER][FATAL] Cannot scan logs.");
    session=maxDiskSession; // New boot always starts a new session on first data.
    snprintf(msg,sizeof(msg),"[LOGGER] SD type=%u card=%llu filesystem=%llu free=%llu incomplete=%llu",
             SD_MMC.cardType(),(unsigned long long)SD_MMC.cardSize(),
             (unsigned long long)SD_MMC.totalBytes(),(unsigned long long)freeBytes(),
             (unsigned long long)incompleteFiles);
    report(msg);
    if(cfg::MIRROR_TO_PC) {
        mirrorStream=xStreamBufferCreate(16384,1);
        if(!mirrorStream || xTaskCreatePinnedToCore(monitorTask,"monitor",3072,nullptr,1,nullptr,1)!=pdPASS)
            fatalBoot("[LOGGER][FATAL] Monitor task allocation failed.");
    }
    // Start tracker only AFTER READY: mount/scan is deliberately outside capture.
    uart_flush_input(PORT); xQueueReset(uartEvents);
    report("[LOGGER] READY - start/restart tracker now.");
    if(xTaskCreatePinnedToCore(rxTask,"uart_rx",4096,nullptr,3,nullptr,0)!=pdPASS)
        fatalBoot("[LOGGER][FATAL] RX task allocation failed.");
    lastFlush=nowMs();
}
void loop() {
    static bool faultHandled=false;
    if(stopped.load()) {
        if(!faultHandled) {
            uint32_t end=head.load(std::memory_order_acquire);
            uint32_t t=tail.load();
            while(t!=end) {
                softwareDropped.fetch_add(pool[readSlot].len);
                readSlot=(readSlot+1)%slots; ++t;
            }
            tail.store(t,std::memory_order_release);
            // A ring overflow sets stopped in RX; other faults already set loggerState.
            if(strcmp(loggerState,"RUNNING")==0 || strcmp(loggerState,"WAITING")==0) fail("BUFFER_FULL");
            else saveInfo();
            faultHandled=true;
        }
        delay(100); return;
    }
    uint64_t now=nowMs();
    uint32_t pending=head.load(std::memory_order_acquire)-tail.load();
    // File creation occurs promptly on first input, subsequent batches every 5s.
    bool due=now-lastFlush>=cfg::FLUSH_MS;
    bool end=logFile && now>=startMs+cfg::WINDOW_MS;
    if(pending && (!logFile || due || end || pending>=slots/2)) {
        // Snapshot keeps a continuous stream from preventing flush/rotation.
        uint32_t limit=head.load(std::memory_order_acquire);
        while(tail.load()!=limit && !stopped.load()) {
            uint32_t t=tail.load(std::memory_order_relaxed);
            size_t saved=consume(pool[readSlot]);
            softwareDropped.fetch_add(pool[readSlot].len-saved);
            readSlot=(readSlot+1)%slots;
            tail.store(t+1,std::memory_order_release);
        }
        if(!stopped.load() && logFile && !syncFile(logFile)) fail("SD_SYNC_ERROR");
        lastFlush=nowMs();
    }
    // Close nonempty windows even if the tracker stops transmitting.
    if(!stopped.load() && logFile && nowMs()>=startMs+cfg::WINDOW_MS && head.load()==tail.load())
        finishFile();
    delay(2);
}
