// RamDay: 使った分だけメモリを確保し、不要になれば解放する RAM ディスク
//
// 仕組み:
//   ディスク本体は ImDisk の署名済みドライバ（imdisk.sys、無改変）が作る。
//   その「プロキシ」型デバイスを共有メモリ経由でこのプロセスにつなぎ、読み書きを
//   ここで受け持つ。中身は 64KB ブロック単位で、書かれたブロックだけメモリを
//   コミットし、TRIM と空きビットマップの回収で、要らなくなったブロックを返す。
//
// レジストリに残さないために:
//   - ImDisk が未インストールなら、起動中だけドライバをサービス登録し、終了時に
//     停止・削除する（インストール済みならそれを使い、登録は触らない）。
//   - TEMP/TMP の元の値は exe の隣の RamDay.ini に控え、解放時に書き戻す。
//     異常終了しても、次回起動時（または -restore）に ini から戻す。
//
// プロセスは 2 つ:
//   設定画面（引数なし、一般権限）: 設定の入力と状態の表示。「開始」でワーカーを管理者として起動する
//   ワーカー（-worker、管理者）     : RAM ディスクを持ち、トレイに常駐する。状態を共有メモリで公開する
//
// コマンドライン（指定すると設定画面を出さずにワーカーを起動する。自動化・検証用）:
//   RamDay.exe -d R -s 4G -fs NTFS [-temp R:\Temp | -notemp] [-scope both|user|system] [-quiet]
//   RamDay.exe -release [-force]   動作中の RamDay を解放して終了させる
//   RamDay.exe -restore            前回の異常終了の後始末（TEMP の復元など）だけ行う
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <dbt.h>
#include <sddl.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "resource.h"

#define APP_TITLE      L"RamDay"
#define WND_CLASS      L"RamDayWnd"
#define WM_TRAY        (WM_APP + 1)
#define WM_RELEASE_REQ (WM_APP + 2)       // wParam: 1 = 使用中でも強制
#define TIMER_TIP      1
#define TIP_MS         2000
#define TIMER_STATUS   3
#define STATUS_MS      500
#define VOLUME_LABEL   L"RamDay"
#define MIN_SIZE       (16ULL << 20)      // フォーマットできる最小限
#define FAT32_MAX      (32ULL << 30)      // Windows の FAT32 フォーマットの上限

// ---- ImDisk の定義（inc/imdisk.h・inc/imdproxy.h より必要な分だけ） ----

#define FILE_DEVICE_IMDISK          0x8372
#define IOCTL_IMDISK_CREATE_DEVICE  CTL_CODE(FILE_DEVICE_IMDISK, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_IMDISK_QUERY_DEVICE   CTL_CODE(FILE_DEVICE_IMDISK, 0x802, METHOD_BUFFERED, 0)
#define IOCTL_IMDISK_REMOVE_DEVICE  CTL_CODE(FILE_DEVICE_IMDISK, 0x806, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IMDISK_DEVICE_TYPE_HD       0x00000010
#define IMDISK_TYPE_PROXY           0x00000300
#define IMDISK_PROXY_TYPE_SHM       0x00003000
#define IMDISK_AUTO_DEVICE_NUMBER   ((ULONG)-1)
#define IMDISK_SERVICE              L"ImDisk"
#define IMDISK_CTL_PATH             L"\\\\?\\GLOBALROOT\\Device\\ImDiskCtl"

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING_;
typedef struct {
    ULONG Length; HANDLE RootDirectory; UNICODE_STRING_ *ObjectName; ULONG Attributes;
    PVOID SecurityDescriptor, SecurityQualityOfService;
} OBJECT_ATTRIBUTES_;

typedef struct {
    ULONG         DeviceNumber;
    DISK_GEOMETRY DiskGeometry;
    LARGE_INTEGER ImageOffset;
    ULONG         Flags;
    WCHAR         DriveLetter;
    USHORT        FileNameLength;
    WCHAR         FileName[1];
} IMDISK_CREATE_DATA;

enum { REQ_NULL, REQ_INFO, REQ_READ, REQ_WRITE, REQ_CONNECT, REQ_CLOSE, REQ_UNMAP, REQ_ZERO };
#define PROXY_FLAG_SUPPORTS_UNMAP 0x02
#define PROXY_FLAG_SUPPORTS_ZERO  0x04
#define PROXY_HEADER_SIZE         4096
#define PROXY_BUFFER_SIZE         (8u << 20)  // 1 往復で運ぶ最大量。超える要求はドライバが分割する

typedef struct { ULONGLONG file_size, req_alignment, flags; } PROXY_INFO_RESP;
typedef struct { ULONGLONG request_code, offset, length; } PROXY_RW_REQ;
typedef struct { ULONGLONG errorno, length; } PROXY_RW_RESP;
typedef struct { ULONGLONG request_code, length; } PROXY_UNMAP_REQ;
typedef struct { ULONGLONG errorno; } PROXY_UNMAP_RESP;
typedef struct { LONGLONG StartingOffset; ULONGLONG LengthInBytes; } DATA_SET_RANGE;

// ---- 設定 ----

enum { FS_NTFS, FS_EXFAT, FS_FAT32, FS_COUNT };
static const wchar_t *FS_NAMES[FS_COUNT] = { L"NTFS", L"exFAT", L"FAT32" };

typedef struct {
    wchar_t   letter;
    ULONGLONG sizeNum;
    BOOL      unitGB;
    int       fs;
    BOOL      temp, tempUser, tempSys;
    wchar_t   tempDir[MAX_PATH];
} Settings;

static Settings  g_cfg;
static HINSTANCE g_inst;
static HWND      g_wnd;
static HICON     g_iconLarge, g_iconSmall;
static NOTIFYICONDATAW g_nid;
static UINT      g_msgTaskbarCreated;
static BOOL      g_quiet;                  // コマンドライン起動: 対話せずログだけ
static BOOL      g_worker;                 // ワーカー（RAM ディスクを管理する管理者プロセス）として動いている
static BOOL      g_ending;                 // サインアウト・シャットダウン中: 時間のかかる全体通知を省く
static wchar_t   g_exeDir[MAX_PATH], g_iniPath[MAX_PATH], g_logPath[MAX_PATH];
static ULONGLONG g_physTotal;

// ---- ログ（exe の隣の RamDay.log） ----

static void applog(const wchar_t *fmt, ...) {
    wchar_t msg[1024], line[1100];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, ARRAYSIZE(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = swprintf(line, ARRAYSIZE(line), L"%04d-%02d-%02d %02d:%02d:%02d.%03d %ls\r\n",
                     t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    char u8[3400];
    int m = WideCharToMultiByte(CP_UTF8, 0, line, n, u8, sizeof u8, NULL, NULL);
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, u8, m, &w, NULL);
    CloseHandle(h);
}

static void last_error_text(DWORD e, wchar_t *buf, size_t n) {
    wchar_t sys[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, sys, ARRAYSIZE(sys), NULL);
    size_t len = wcslen(sys);
    while (len && (sys[len - 1] == L'\n' || sys[len - 1] == L'\r')) sys[--len] = 0;
    swprintf(buf, n, L"%ls（コード %lu）", sys, e);
}

static void fail_msg(HWND owner, const wchar_t *what, DWORD err) {
    wchar_t text[1024], et[600] = L"";
    if (err) last_error_text(err, et, ARRAYSIZE(et));
    swprintf(text, ARRAYSIZE(text), L"%ls%ls%ls", what, err ? L"\n\n" : L"", et);
    applog(L"エラー: %ls", text);
    if (g_worker) {                        // 設定画面が読んで表示する
        WritePrivateProfileStringW(L"Status", L"LastError", text, g_iniPath);
        WritePrivateProfileStringW(NULL, NULL, NULL, g_iniPath);
    }
    if (!g_quiet) MessageBoxW(owner, text, APP_TITLE, MB_ICONERROR);
}

static void format_bytes(ULONGLONG b, wchar_t *buf, size_t n) {
    if (b >= (1ULL << 30))      swprintf(buf, n, L"%.2f GB", b / 1073741824.0);
    else if (b >= (1ULL << 20)) swprintf(buf, n, L"%.1f MB", b / 1048576.0);
    else if (b >= 1024)         swprintf(buf, n, L"%llu KB", b >> 10);
    else                        swprintf(buf, n, L"%llu バイト", b);
}

static ULONGLONG cfg_bytes(const Settings *s) {
    return s->sizeNum << (s->unitGB ? 30 : 20);
}

// ---- ini（UTF-16 で作っておくと Write/GetPrivateProfileStringW が日本語のまま扱う） ----

static void ini_ensure(void) {
    if (GetFileAttributesW(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(g_iniPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    static const BYTE bom[2] = { 0xFF, 0xFE };
    DWORD w;
    WriteFile(h, bom, 2, &w, NULL);
    CloseHandle(h);
}

static void ini_put(const wchar_t *sec, const wchar_t *key, const wchar_t *val) {
    WritePrivateProfileStringW(sec, key, val, g_iniPath);
}

static void ini_put_int(const wchar_t *sec, const wchar_t *key, ULONGLONG v) {
    wchar_t b[32];
    swprintf(b, ARRAYSIZE(b), L"%llu", v);
    ini_put(sec, key, b);
}

static void ini_get(const wchar_t *sec, const wchar_t *key, const wchar_t *def, wchar_t *buf, DWORD n) {
    GetPrivateProfileStringW(sec, key, def, buf, n, g_iniPath);
}

static ULONGLONG ini_get_int(const wchar_t *sec, const wchar_t *key, ULONGLONG def) {
    wchar_t b[32];
    ini_get(sec, key, L"", b, ARRAYSIZE(b));
    return b[0] ? wcstoull(b, NULL, 10) : def;
}

static void ini_flush(void) {
    WritePrivateProfileStringW(NULL, NULL, NULL, g_iniPath);
}

static void settings_load(Settings *s) {
    wchar_t b[MAX_PATH];
    ini_get(L"Settings", L"Drive", L"R", b, ARRAYSIZE(b));
    s->letter = towupper(b[0]);
    if (s->letter < L'D' || s->letter > L'Z') s->letter = L'R';
    s->sizeNum = ini_get_int(L"Settings", L"Size", 4);
    ini_get(L"Settings", L"Unit", L"GB", b, ARRAYSIZE(b));
    s->unitGB = _wcsicmp(b, L"MB") != 0;
    ini_get(L"Settings", L"FileSystem", L"NTFS", b, ARRAYSIZE(b));
    s->fs = FS_NTFS;
    for (int i = 0; i < FS_COUNT; i++) if (!_wcsicmp(b, FS_NAMES[i])) s->fs = i;
    s->temp = (BOOL)ini_get_int(L"Settings", L"Temp", 1);
    s->tempUser = (BOOL)ini_get_int(L"Settings", L"TempUser", 1);
    s->tempSys = (BOOL)ini_get_int(L"Settings", L"TempSystem", 1);
    wchar_t def[16];
    swprintf(def, ARRAYSIZE(def), L"%lc:\\Temp", s->letter);
    ini_get(L"Settings", L"TempDir", def, s->tempDir, ARRAYSIZE(s->tempDir));
}

static void settings_save(const Settings *s) {
    wchar_t b[4] = { s->letter, 0 };
    ini_put(L"Settings", L"Drive", b);
    ini_put_int(L"Settings", L"Size", s->sizeNum);
    ini_put(L"Settings", L"Unit", s->unitGB ? L"GB" : L"MB");
    ini_put(L"Settings", L"FileSystem", FS_NAMES[s->fs]);
    ini_put_int(L"Settings", L"Temp", s->temp);
    ini_put_int(L"Settings", L"TempUser", s->tempUser);
    ini_put_int(L"Settings", L"TempSystem", s->tempSys);
    ini_put(L"Settings", L"TempDir", s->tempDir);
    ini_flush();
}

// ---- 中身の置き場所（64KB ブロック単位でコミット／デコミット） ----
//
// ディスク全体ぶんのアドレス空間を予約だけしておき、オフセットをそのまま
// アドレスに対応させる。書かれたブロックだけ MEM_COMMIT し、中身が要らなく
// なったブロックは MEM_DECOMMIT して物理メモリを返す。要らなくなったと分かる
// 道筋は 2 つ:
//   - TRIM / ZERO 要求（ファイルシステムやドライバが送ってくる）
//   - 回収スレッドがファイルシステムの空きビットマップを読み、全クラスタが
//     空きのブロックを返す（TRIM を送らない FAT 系や、TRIM が来ない場合の備え）
// ブロック表は g_storeLock で守る。プロキシは要求 1 件ごとに排他で取る。

#define BLK_SHIFT 16
#define BLK_SIZE  ((ULONGLONG)1 << BLK_SHIFT)
#define BLK_MASK  (BLK_SIZE - 1)

static BYTE           *g_base;             // 予約したアドレス空間
static BYTE           *g_bmap;             // ブロックごとに 1 = コミット済み
static LONG           *g_blkGen;           // ブロックに最後に書いたときの世代（回収との競合よけ）
static volatile LONG   g_gen;              // 回収の走査ごとに 1 進める
static SRWLOCK         g_storeLock = SRWLOCK_INIT;
static ULONGLONG       g_diskSize, g_nblk;
static volatile LONG64 g_usedBlk;
static ULONGLONG       g_wsMin;            // 今の作業セット下限（ページアウトさせないため）
static BOOL            g_wsWarned;
static volatile LONG64 g_stTrimReq, g_stTrimBytes, g_stTrimFreed, g_stReclaimed;

static BOOL is_zero(const BYTE *p, size_t n) {
    const ULONGLONG *q = (const ULONGLONG *)p;
    size_t w = n / 64 * 8, i = 0;
    for (; i < w; i += 8)
        if (q[i] | q[i + 1] | q[i + 2] | q[i + 3] | q[i + 4] | q[i + 5] | q[i + 6] | q[i + 7]) return FALSE;
    for (i *= 8; i < n; i++) if (p[i]) return FALSE;
    return TRUE;
}

// 使っている分は作業セットの下限（ハード）に含め、ページファイルへ追い出させない。
// 256MB 刻みで上げ下げして、システムコールは境目をまたいだときだけにする。
static void ws_adjust(void) {
    const ULONGLONG step = 256ULL << 20;
    ULONGLONG need = ((ULONGLONG)g_usedBlk << BLK_SHIFT) + (64ULL << 20);
    if (need <= g_wsMin && need + 2 * step > g_wsMin) return;
    ULONGLONG want = (need + step - 1) / step * step;
    if (want == g_wsMin) return;
    if (SetProcessWorkingSetSizeEx(GetCurrentProcess(), want, want + step,
                                   QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE)) {
        g_wsMin = want;
    } else {
        if (!g_wsWarned) applog(L"作業セット下限を %llu MB にできなかった（エラー %lu）", want >> 20, GetLastError());
        g_wsWarned = TRUE;
        g_wsMin = want;                    // 失敗しても毎回呼ばないように境目だけ進める
    }
}

// ページアウトさせないのは作業セットのハード下限（ws_adjust）に任せる。
// VirtualLock でページを先に割り当てる方法も試したが、1GB を 1MB ずつ書く実測で
// 3,853 MB/s → 1,909 MB/s と半分に落ちたのでやめた（普通にページフォルトさせる方が速い）。
static BOOL blk_commit(ULONGLONG b) {
    if (!VirtualAlloc(g_base + (b << BLK_SHIFT), BLK_SIZE, MEM_COMMIT, PAGE_READWRITE)) return FALSE;
    g_bmap[b] = 1;
    InterlockedIncrement64(&g_usedBlk);
    ws_adjust();
    return TRUE;
}

static void blk_free(ULONGLONG b) {
    void *a = g_base + (b << BLK_SHIFT);
    if (!VirtualFree(a, BLK_SIZE, MEM_DECOMMIT)) {
        memset(a, 0, BLK_SIZE);            // 返せないなら 0 で埋めて確保済みのまま置く
        return;
    }
    g_bmap[b] = 0;
    InterlockedDecrement64(&g_usedBlk);
}

static void store_read(ULONGLONG off, BYTE *dst, ULONGLONG len) {
    while (len) {
        ULONGLONG b = off >> BLK_SHIFT, n = BLK_SIZE - (off & BLK_MASK);
        if (n > len) n = len;
        if (g_bmap[b]) memcpy(dst, g_base + off, n);
        else           memset(dst, 0, n);
        off += n; dst += n; len -= n;
    }
}

static ULONGLONG store_write(ULONGLONG off, const BYTE *src, ULONGLONG len) {
    BOOL shrank = FALSE;
    while (len) {
        ULONGLONG b = off >> BLK_SHIFT, n = BLK_SIZE - (off & BLK_MASK);
        if (n > len) n = len;
        if (!g_bmap[b]) {
            if (!is_zero(src, n)) {        // 0 を書くだけなら確保しない（読めば 0 が返る）
                if (!blk_commit(b)) return 28;   // ENOSPC
                g_blkGen[b] = g_gen;
                memcpy(g_base + off, src, n);
            }
        } else if (n == BLK_SIZE && is_zero(src, n)) {
            blk_free(b);                   // ブロックまるごと 0 で上書きされたら返す
            shrank = TRUE;
        } else {
            g_blkGen[b] = g_gen;
            memcpy(g_base + off, src, n);
        }
        off += n; src += n; len -= n;
    }
    if (shrank) ws_adjust();
    return 0;
}

static void store_discard(ULONGLONG off, ULONGLONG len) {
    if (off >= g_diskSize) return;
    if (len > g_diskSize - off) len = g_diskSize - off;
    BOOL shrank = FALSE;
    while (len) {
        ULONGLONG b = off >> BLK_SHIFT, n = BLK_SIZE - (off & BLK_MASK);
        if (n > len) n = len;
        if (g_bmap[b]) {
            if (n == BLK_SIZE) {
                blk_free(b);
                shrank = TRUE;
            } else {
                memset(g_base + off, 0, n);
                if (is_zero(g_base + (b << BLK_SHIFT), BLK_SIZE)) { blk_free(b); shrank = TRUE; }
                else g_blkGen[b] = g_gen;
            }
            if (!g_bmap[b]) InterlockedIncrement64(&g_stTrimFreed);
        }
        off += n; len -= n;
    }
    if (shrank) ws_adjust();
}

static BOOL store_init(ULONGLONG size) {
    g_diskSize = size;
    g_nblk = (size + BLK_MASK) >> BLK_SHIFT;
    g_base = VirtualAlloc(NULL, g_nblk << BLK_SHIFT, MEM_RESERVE, PAGE_READWRITE);
    g_bmap = calloc((size_t)g_nblk, 1);
    g_blkGen = calloc((size_t)g_nblk, sizeof(LONG));
    g_usedBlk = g_stTrimReq = g_stTrimBytes = g_stTrimFreed = g_stReclaimed = 0;
    return g_base && g_bmap && g_blkGen;
}

static void store_free(void) {
    if (g_base) VirtualFree(g_base, 0, MEM_RELEASE);
    free(g_bmap);
    free(g_blkGen);
    g_base = NULL;
    g_bmap = NULL;
    g_blkGen = NULL;
    g_usedBlk = 0;
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
    g_wsMin = 0;
}

// ---- プロキシ（ドライバとの共有メモリ通信） ----
//
// ドライバが共有メモリ先頭に要求ヘッダ、4096 バイト目からデータを置いて
// <名前>_Request を立てる。こちらは応答を同じ場所に書いて <名前>_Response を立てる。

static wchar_t g_objName[64];
static HANDLE  g_shmMap, g_evReq, g_evResp, g_proxyThread;
static BYTE   *g_shm;
static volatile LONG g_proxyStop;

static void proxy_handle(void) {
    BYTE *hdr = g_shm, *data = g_shm + PROXY_HEADER_SIZE;
    ULONGLONG code = *(ULONGLONG *)hdr;
    switch (code) {
    case REQ_INFO: {
        PROXY_INFO_RESP r = { g_diskSize, 1, PROXY_FLAG_SUPPORTS_UNMAP | PROXY_FLAG_SUPPORTS_ZERO };
        memcpy(hdr, &r, sizeof r);
        break;
    }
    case REQ_READ:
    case REQ_WRITE: {
        PROXY_RW_REQ q;
        memcpy(&q, hdr, sizeof q);
        ULONGLONG len = q.length;
        if (len > PROXY_BUFFER_SIZE) len = PROXY_BUFFER_SIZE;
        if (q.offset >= g_diskSize) len = 0;
        else if (len > g_diskSize - q.offset) len = g_diskSize - q.offset;
        PROXY_RW_RESP r = { 0, len };
        if (code == REQ_READ) store_read(q.offset, data, len);
        else if ((r.errorno = store_write(q.offset, data, len)) != 0) r.length = 0;
        memcpy(hdr, &r, sizeof r);
        break;
    }
    case REQ_UNMAP:
    case REQ_ZERO: {
        PROXY_UNMAP_REQ q;
        memcpy(&q, hdr, sizeof q);
        ULONGLONG items = q.length / sizeof(DATA_SET_RANGE);
        if (q.length > PROXY_BUFFER_SIZE) items = 0;
        const DATA_SET_RANGE *rg = (const DATA_SET_RANGE *)data;
        InterlockedIncrement64(&g_stTrimReq);
        for (ULONGLONG i = 0; i < items; i++)
            if (rg[i].StartingOffset >= 0) {
                store_discard((ULONGLONG)rg[i].StartingOffset, rg[i].LengthInBytes);
                InterlockedAdd64(&g_stTrimBytes, (LONG64)rg[i].LengthInBytes);
            }
        PROXY_UNMAP_RESP r = { 0 };
        memcpy(hdr, &r, sizeof r);
        break;
    }
    default: {
        PROXY_RW_RESP r = { 22, 0 };       // EINVAL
        memcpy(hdr, &r, sizeof r);
        break;
    }
    }
}

static DWORD WINAPI proxy_main(LPVOID unused) {
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (WaitForSingleObject(g_evReq, INFINITE) != WAIT_OBJECT_0) return 1;
    for (;;) {
        if (g_proxyStop) break;
        if (*(ULONGLONG *)g_shm == REQ_CLOSE) break;
        AcquireSRWLockExclusive(&g_storeLock);
        proxy_handle();
        ReleaseSRWLockExclusive(&g_storeLock);
        // 応答を立てて次の要求を待つのを 1 回のシステムコールで。
        // 応答後しばらく回って待つ方法も試したが、4K ランダムで 6 万 → 4.8 万 IOPS と遅くなった
        if (SignalObjectAndWait(g_evResp, g_evReq, INFINITE, FALSE) != WAIT_OBJECT_0) break;
    }
    return 0;
}

static void proxy_close(void) {
    if (g_proxyThread) {
        if (WaitForSingleObject(g_proxyThread, 5000) != WAIT_OBJECT_0) {
            g_proxyStop = 1;
            SetEvent(g_evReq);
            WaitForSingleObject(g_proxyThread, 3000);
        }
        CloseHandle(g_proxyThread);
        g_proxyThread = NULL;
    }
    if (g_shm) UnmapViewOfFile(g_shm);
    if (g_shmMap) CloseHandle(g_shmMap);
    if (g_evReq) CloseHandle(g_evReq);
    if (g_evResp) CloseHandle(g_evResp);
    g_shm = NULL;
    g_shmMap = g_evReq = g_evResp = NULL;
    store_free();
}

static BOOL proxy_open(ULONGLONG size) {
    wchar_t name[96];
    g_proxyStop = 0;
    swprintf(g_objName, ARRAYSIZE(g_objName), L"RamDay_%lu", GetCurrentProcessId());
    if (!store_init(size)) return FALSE;
    swprintf(name, ARRAYSIZE(name), L"Global\\%ls", g_objName);
    g_shmMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                  PROXY_HEADER_SIZE + PROXY_BUFFER_SIZE, name);
    if (!g_shmMap || GetLastError() == ERROR_ALREADY_EXISTS) return FALSE;
    g_shm = MapViewOfFile(g_shmMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    swprintf(name, ARRAYSIZE(name), L"Global\\%ls_Request", g_objName);
    g_evReq = CreateEventW(NULL, FALSE, FALSE, name);
    swprintf(name, ARRAYSIZE(name), L"Global\\%ls_Response", g_objName);
    g_evResp = CreateEventW(NULL, FALSE, FALSE, name);
    if (!g_shm || !g_evReq || !g_evResp) return FALSE;
    g_proxyThread = CreateThread(NULL, 0, proxy_main, NULL, 0, NULL);
    return g_proxyThread != NULL;
}

// ---- ドライバ（ImDisk 未インストールなら起動中だけサービス登録する） ----
//
// 登録して開始したら、すぐに登録を「削除予約」にする。読み込まれたドライバは
// そのまま動き続け、停止した時点で（停止できなければ次の再起動時に）Windows が
// 登録を消す。停止はディスクのデバイスが本当に消えたのを確かめてからにする。
// デバイスを誰かが参照したまま停止を頼むと、ドライバが「停止保留」のまま
// 新しい接続を受け付けなくなり、再起動まで使えなくなるため。

#define RAMDAY_DRIVER_TAG L"(RamDay)"      // こちらで登録したサービスの表示名の印

static BOOL g_ownDriver;                   // こちらで読み込んだ → 解放時に止めてよい

typedef struct { UNICODE_STRING_ Name, TypeName; } OBJDIR_INFO;
typedef LONG (NTAPI *NtOpenDirectoryObject_t)(PHANDLE, ACCESS_MASK, OBJECT_ATTRIBUTES_ *);
typedef LONG (NTAPI *NtQueryDirectoryObject_t)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);

// \Device にある ImDisk のディスク（ImDisk<番号>）を数える。num のものがあれば *found = TRUE。
// デバイスを開くと参照が増えてドライバの後始末を遅らせるので、一覧を読むだけにする。
static int imdisk_disks(ULONG num, BOOL *found) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    NtOpenDirectoryObject_t op = (NtOpenDirectoryObject_t)(void *)GetProcAddress(nt, "NtOpenDirectoryObject");
    NtQueryDirectoryObject_t q = (NtQueryDirectoryObject_t)(void *)GetProcAddress(nt, "NtQueryDirectoryObject");
    if (found) *found = FALSE;
    if (!op || !q) return -1;
    wchar_t dir[] = L"\\Device";
    UNICODE_STRING_ us = { sizeof dir - 2, sizeof dir, dir };
    OBJECT_ATTRIBUTES_ oa = { sizeof oa, NULL, &us, 0, NULL, NULL };
    HANDLE h;
    if (op(&h, 0x0001 /* DIRECTORY_QUERY */, &oa) < 0) return -1;
    static BYTE buf[32768];
    ULONG ctx = 0, len;
    BOOLEAN first = TRUE;
    LONG st;
    int n = 0;
    while ((st = q(h, buf, sizeof buf, FALSE, first, &ctx, &len)) >= 0) {
        first = FALSE;
        for (OBJDIR_INFO *o = (OBJDIR_INFO *)buf; o->Name.Buffer; o++) {
            if (o->Name.Length <= 12 || _wcsnicmp(o->Name.Buffer, L"ImDisk", 6) || !iswdigit(o->Name.Buffer[6])) continue;
            n++;
            if (found && wcstoul(o->Name.Buffer + 6, NULL, 10) == num) *found = TRUE;
        }
        if (st != 0x105) break;            // STATUS_MORE_ENTRIES でなければ読み終わり
    }
    CloseHandle(h);
    return n;
}

static HANDLE open_ctl(void) {
    return CreateFileW(IMDISK_CTL_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

// 埋め込みの imdisk.sys を exe の隣（書けなければ Windows の Temp）に用意する
static BOOL driver_file(wchar_t *path, size_t n) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(IDR_IMDISK_SYS), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_inst, r) : NULL;
    const void *bytes = g ? LockResource(g) : NULL;
    DWORD size = r ? SizeofResource(g_inst, r) : 0;
    wchar_t cand[2][MAX_PATH];
    swprintf(cand[0], MAX_PATH, L"%ls\\imdisk.sys", g_exeDir);
    GetWindowsDirectoryW(cand[1], MAX_PATH);
    wcscat(cand[1], L"\\Temp\\RamDay-imdisk.sys");
    for (int i = 0; i < 2; i++) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(cand[i], GetFileExInfoStandard, &fa) && fa.nFileSizeLow == size && !fa.nFileSizeHigh) {
            wcsncpy(path, cand[i], n);
            return TRUE;
        }
        if (!bytes) continue;
        HANDLE h = CreateFileW(cand[i], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD w = 0;
        BOOL ok = WriteFile(h, bytes, size, &w, NULL) && w == size;
        CloseHandle(h);
        if (ok) {
            wcsncpy(path, cand[i], n);
            return TRUE;
        }
        DeleteFileW(cand[i]);
    }
    return FALSE;
}

// ImDisk サービスがこちらで登録したものか（表示名の印で見分ける）
static BOOL service_is_ours(SC_HANDLE svc) {
    static BYTE buf[8192];
    DWORD need;
    QUERY_SERVICE_CONFIGW *qc = (QUERY_SERVICE_CONFIGW *)buf;
    return QueryServiceConfigW(svc, qc, sizeof buf, &need) && qc->lpDisplayName &&
           wcsstr(qc->lpDisplayName, RAMDAY_DRIVER_TAG) != NULL;
}

static BOOL driver_ensure(DWORD *err, BOOL *pending) {
    *pending = FALSE;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) { *err = GetLastError(); return FALSE; }
    SC_HANDLE svc = OpenServiceW(scm, IMDISK_SERVICE, SERVICE_ALL_ACCESS);
    HANDLE h = open_ctl();
    if (h != INVALID_HANDLE_VALUE) {       // もう読み込まれている（前回の RamDay か、インストール済みの ImDisk）
        CloseHandle(h);
        g_ownDriver = svc && service_is_ours(svc);
        applog(L"ImDisk ドライバは読み込み済み（%ls）", g_ownDriver ? L"前回 RamDay が読み込んだもの" : L"インストール済みのもの");
        if (svc) CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return TRUE;
    }
    BOOL created = FALSE;
    if (!svc) {
        wchar_t file[MAX_PATH], bin[MAX_PATH + 8];
        if (!driver_file(file, ARRAYSIZE(file))) {
            *err = GetLastError();
            CloseServiceHandle(scm);
            return FALSE;
        }
        swprintf(bin, ARRAYSIZE(bin), L"\\??\\%ls", file);
        svc = CreateServiceW(scm, IMDISK_SERVICE, L"ImDisk Virtual Disk Driver " RAMDAY_DRIVER_TAG, SERVICE_ALL_ACCESS,
                             SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                             bin, NULL, NULL, NULL, NULL, NULL);
        if (!svc) {
            *err = GetLastError();
            CloseServiceHandle(scm);
            return FALSE;
        }
        created = TRUE;
        applog(L"ドライバを一時的にサービス登録した: %ls", bin);
    }
    BOOL ours = created || service_is_ours(svc);
    if (StartServiceW(svc, 0, NULL)) {
        applog(L"ドライバを開始した");
    } else if (GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        *err = GetLastError();
        // 削除予約済みの登録が残っている = 前回のドライバがまだ停止保留中
        if (*err == ERROR_SERVICE_MARKED_FOR_DELETE || *err == ERROR_SERVICE_DISABLED) *pending = ours;
        if (created) DeleteService(svc);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return FALSE;
    }
    g_ownDriver = ours;
    if (ours) {
        if (DeleteService(svc)) applog(L"サービス登録を削除予約にした（停止時か次の再起動時に消える）");
        else if (GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
            applog(L"サービス登録を削除予約にできなかった（エラー %lu）", GetLastError());
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return TRUE;
}

// こちらで読み込んだドライバを、ディスクが残っていなければ止める（止まれば登録も消える）
static void driver_release_if_idle(void) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, IMDISK_SERVICE, SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | DELETE) : NULL;
    if (svc && service_is_ours(svc)) {
        SERVICE_STATUS st;
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) {
            int disks = imdisk_disks(0, NULL);   // 自分のデバイスが消えたかは disk_remove で待ってある
            if (disks == 0) {
                if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
                    for (int i = 0; i < 30 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; i++)
                        Sleep(100);
                    applog(L"ドライバを停止した（状態 %lu）", st.dwCurrentState);
                } else {
                    applog(L"ドライバを停止できなかった（エラー %lu）", GetLastError());
                }
            } else {
                applog(L"ImDisk のデバイスが %d 個残っているので、ドライバは読み込んだままにする"
                       L"（サービス登録は削除予約済み。再起動時に消える）", disks);
            }
        }
        if (!DeleteService(svc) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
            applog(L"サービス登録を削除できなかった（エラー %lu）", GetLastError());
    }
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
}

static void driver_cleanup(void) {
    if (g_ownDriver) driver_release_if_idle();
    g_ownDriver = FALSE;
    wchar_t tmp[MAX_PATH];
    GetWindowsDirectoryW(tmp, MAX_PATH);
    wcscat(tmp, L"\\Temp\\RamDay-imdisk.sys");
    DeleteFileW(tmp);                      // 置き場所の代替に使っていたら消す（読み込み中なら消えないだけ）
}

// ---- ディスクの作成・フォーマット・削除 ----

static ULONG g_devNum = IMDISK_AUTO_DEVICE_NUMBER;
static BOOL  g_diskUp;

// ---- 全体通知（別スレッドで投げる） ----
//
// HWND_BROADCAST への SendMessageTimeout は、タイムアウトがウィンドウごとにかかる。
// 実測で 15〜97 秒戻らないことがあり、その間 UI スレッド（トレイ・解放の依頼）が
// 止まったので、通知は別スレッドで投げて待たない。終了時だけ少し待つ。

enum { BC_ARRIVAL, BC_REMOVED, BC_REMOVE_PENDING, BC_ENVIRONMENT };
typedef struct { int kind; wchar_t letter; } Broadcast;
static HANDLE g_bcThreads[16];
static int    g_bcCount;

static DWORD WINAPI broadcast_main(LPVOID p) {
    Broadcast b = *(Broadcast *)p;
    free(p);
    DWORD_PTR r;
    if (b.kind == BC_ENVIRONMENT) {
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 2000, &r);
        return 0;
    }
    DEV_BROADCAST_VOLUME dv = { sizeof dv, DBT_DEVTYP_VOLUME, 0, 1u << (b.letter - L'A'), 0 };
    WPARAM ev = b.kind == BC_ARRIVAL ? DBT_DEVICEARRIVAL : b.kind == BC_REMOVED ? DBT_DEVICEREMOVECOMPLETE : DBT_DEVICEREMOVEPENDING;
    SendMessageTimeoutW(HWND_BROADCAST, WM_DEVICECHANGE, ev, (LPARAM)&dv, SMTO_ABORTIFHUNG, 2000, &r);
    if (b.kind != BC_REMOVE_PENDING) {
        wchar_t root[] = L" :\\";
        root[0] = b.letter;
        SHChangeNotify(b.kind == BC_ARRIVAL ? SHCNE_DRIVEADD : SHCNE_DRIVEREMOVED, SHCNF_PATHW, root, NULL);
    }
    return 0;
}

static void broadcast_async(int kind, wchar_t letter) {
    if (g_ending) return;
    for (int i = 0; i < g_bcCount; ) {    // 終わったものを片付ける
        if (WaitForSingleObject(g_bcThreads[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(g_bcThreads[i]);
            g_bcThreads[i] = g_bcThreads[--g_bcCount];
        } else {
            i++;
        }
    }
    if (g_bcCount >= (int)ARRAYSIZE(g_bcThreads)) return;
    Broadcast *b = malloc(sizeof *b);
    b->kind = kind;
    b->letter = letter;
    HANDLE t = CreateThread(NULL, 0, broadcast_main, b, 0, NULL);
    if (t) g_bcThreads[g_bcCount++] = t;
    else free(b);
}

static void broadcast_wait(DWORD ms) {
    if (g_bcCount) WaitForMultipleObjects(g_bcCount, g_bcThreads, TRUE, ms);
}

static void notify_drive(wchar_t letter, BOOL added) {
    broadcast_async(added ? BC_ARRIVAL : BC_REMOVED, letter);
}

static void remove_letter_leftovers(wchar_t letter) {
    wchar_t local[] = L" :", global[] = L"Global\\ :", key[128];
    local[0] = global[7] = letter;
    DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_NO_BROADCAST_SYSTEM, global, NULL);
    DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_NO_BROADCAST_SYSTEM, local, NULL);
    // エクスプローラーがドライブ文字ごとに作る記録も消しておく
    swprintf(key, ARRAYSIZE(key), L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MountPoints2\\%lc", letter);
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
}

static BOOL disk_create(wchar_t letter, ULONGLONG size, DWORD *err) {
    if (!proxy_open(size)) {
        *err = GetLastError();
        proxy_close();
        return FALSE;
    }
    HANDLE ctl = open_ctl();
    if (ctl == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        proxy_close();
        return FALSE;
    }
    wchar_t nt[96];
    swprintf(nt, ARRAYSIZE(nt), L"\\BaseNamedObjects\\%ls", g_objName);
    USHORT nameBytes = (USHORT)(wcslen(nt) * sizeof(wchar_t));
    DWORD cdSize = sizeof(IMDISK_CREATE_DATA) + nameBytes;
    IMDISK_CREATE_DATA *cd = calloc(1, cdSize);
    cd->DeviceNumber = IMDISK_AUTO_DEVICE_NUMBER;
    cd->DiskGeometry.Cylinders.QuadPart = (LONGLONG)size;
    cd->Flags = IMDISK_DEVICE_TYPE_HD | IMDISK_TYPE_PROXY | IMDISK_PROXY_TYPE_SHM;
    cd->DriveLetter = letter;
    cd->FileNameLength = nameBytes;
    memcpy(cd->FileName, nt, nameBytes);
    DWORD dw;
    BOOL ok = DeviceIoControl(ctl, IOCTL_IMDISK_CREATE_DEVICE, cd, cdSize, cd, cdSize, &dw, NULL);
    if (!ok) *err = GetLastError();
    else g_devNum = cd->DeviceNumber;
    free(cd);
    CloseHandle(ctl);
    if (!ok) {
        proxy_close();
        return FALSE;
    }
    g_diskUp = TRUE;
    ini_put_int(L"Backup", L"DeviceNumber", g_devNum);
    ini_flush();
    applog(L"デバイス \\Device\\ImDisk%lu を作成した（%lc:、最大 %llu バイト）", g_devNum, letter, size);
    return TRUE;
}

// fmifs.dll の FormatEx（format.com の中身）。コールバックで結果を受ける
typedef BOOLEAN (__stdcall *FMIFS_CB)(int cmd, DWORD sub, PVOID arg);
typedef VOID (__stdcall *FORMATEX)(PWCHAR root, DWORD media, PWCHAR fs, PWCHAR label, BOOL quick, DWORD cluster, FMIFS_CB cb);
#define FMIFS_DONE     11
#define FMIFS_HARDDISK 0x0C
static BOOL g_formatOk;

static BOOLEAN __stdcall format_cb(int cmd, DWORD sub, PVOID arg) {
    (void)sub;
    if (cmd == FMIFS_DONE) g_formatOk = *(BOOLEAN *)arg;
    return TRUE;
}

static BOOL run_format_com(wchar_t letter, const wchar_t *fs) {
    wchar_t cmd[200], sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\\format.com\" %lc: /FS:%ls /V:%ls /Q /Y", sys, letter, fs, VOLUME_LABEL);
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

static BOOL disk_format(wchar_t letter, int fs) {
    wchar_t root[] = L" :\\", fsName[16], label[] = VOLUME_LABEL;
    root[0] = letter;
    wcscpy(fsName, FS_NAMES[fs]);
    HMODULE m = LoadLibraryW(L"fmifs.dll");
    FORMATEX fx = m ? (FORMATEX)(void *)GetProcAddress(m, "FormatEx") : NULL;
    g_formatOk = FALSE;
    if (fx) fx(root, FMIFS_HARDDISK, fsName, label, TRUE, 0, format_cb);
    if (m) FreeLibrary(m);
    if (g_formatOk) {
        applog(L"%ls でフォーマットした（FormatEx）", fsName);
        return TRUE;
    }
    applog(L"FormatEx が失敗。format.com で再試行する");
    if (run_format_com(letter, fsName)) {
        applog(L"%ls でフォーマットした（format.com）", fsName);
        return TRUE;
    }
    return FALSE;
}

static BOOL remove_by_number(ULONG num) {
    HANDLE ctl = open_ctl();
    if (ctl == INVALID_HANDLE_VALUE) return FALSE;
    DWORD dw;
    BOOL ok = DeviceIoControl(ctl, IOCTL_IMDISK_REMOVE_DEVICE, &num, sizeof num, NULL, 0, &dw, NULL);
    CloseHandle(ctl);
    return ok;
}

// ボリュームを開いてロックを試みる。ロックできなければ *locked = FALSE
static HANDLE volume_open_lock(wchar_t letter, BOOL *locked) {
    wchar_t vol[] = L"\\\\.\\ :";
    vol[4] = letter;
    HANDLE h = CreateFileW(vol, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    *locked = FALSE;
    if (h == INVALID_HANDLE_VALUE) return h;
    FlushFileBuffers(h);
    DWORD dw;
    for (int i = 0; i < (g_ending ? 1 : 6) && !*locked; i++) {
        if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &dw, NULL)) {
            *locked = TRUE;
        } else {
            if (i == 0) broadcast_async(BC_REMOVE_PENDING, letter);   // 開いているウィンドウなどに手放してもらう
            Sleep(200);
        }
    }
    return h;
}

// ロック済み（または強制）のボリュームを外してデバイスを消す。
// ドライバはデバイスを開いているハンドルが 0 になるまで消さず、確かめる間隔が
// 0.1 秒 → 1.6 秒 → 25.6 秒 → 約 7 分と伸びていく。自分のハンドルが参照として
// 残らないよう、ドライブ文字を外してハンドルを閉じてから削除を頼む。
static void disk_remove(wchar_t letter, HANDLE vol, BOOL locked) {
    DWORD dw;
    if (vol != INVALID_HANDLE_VALUE) {
        DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &dw, NULL);
        if (!locked) DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &dw, NULL);
    }
    remove_letter_leftovers(letter);       // これ以降、R: からは開けない
    if (vol != INVALID_HANDLE_VALUE) CloseHandle(vol);
    if (!remove_by_number(g_devNum))
        applog(L"デバイスの削除に失敗（エラー %lu）", GetLastError());
    proxy_close();                         // ドライバから CLOSE が届いてスレッドが抜ける
    BOOL still = TRUE;
    for (int i = 0; i < 30 && still; i++) {
        imdisk_disks(g_devNum, &still);
        if (still) Sleep(100);
    }
    applog(still ? L"\\Device\\ImDisk%lu はまだ参照が残っている（ドライバが後で消す）"
                 : L"\\Device\\ImDisk%lu が消えた", g_devNum);
    notify_drive(letter, FALSE);
    g_diskUp = FALSE;
    applog(L"%lc: を削除し、メモリを解放した", letter);
}

// ---- 回収（ファイルシステムの空きビットマップから、要らなくなったブロックを返す） ----
//
// 走査を始める前に世代を 1 進め、それから空きビットマップを読む。その後で書かれた
// ブロック（世代が新しい）は、ビットマップが古い可能性があるので返さない。
// ファイルシステムはクラスタを割り当ててから書くので、ビットマップで空きなら
// その時点で中身は不要。

static HANDLE  g_reclaimThread, g_reclaimStop;
static wchar_t g_reclaimLetter;
static BOOL    g_verbose;

static ULONGLONG reclaim_once(void) {
    wchar_t vol[] = L"\\\\.\\ :", root[] = L" :\\";
    vol[4] = root[0] = g_reclaimLetter;
    DWORD spc, bps, fc, tc;
    if (!GetDiskFreeSpaceW(root, &spc, &bps, &fc, &tc)) return 0;
    ULONGLONG cl = (ULONGLONG)spc * bps;
    ULONGLONG fsUsed = (ULONGLONG)(tc - fc) * cl;
    ULONGLONG used = (ULONGLONG)g_usedBlk << BLK_SHIFT;
    if (used <= fsUsed + (8ULL << 20)) return 0;      // 余分が小さいうちは読まない

    HANDLE h = CreateFileW(vol, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD dw;
    RETRIEVAL_POINTER_BASE base = { 0 };
    ULONGLONG heap = 0;                    // クラスタ 0 の位置（NTFS は 0、FAT 系はデータ領域の先頭）
    if (DeviceIoControl(h, FSCTL_GET_RETRIEVAL_POINTER_BASE, NULL, 0, &base, sizeof base, &dw, NULL))
        heap = (ULONGLONG)base.FileAreaOffset.QuadPart * bps;
    size_t need = sizeof(VOLUME_BITMAP_BUFFER) + (tc + 7) / 8 + 64;
    VOLUME_BITMAP_BUFFER *bm = malloc(need);
    STARTING_LCN_INPUT_BUFFER in = { { { 0 } } };
    LONG gen0 = InterlockedIncrement(&g_gen);
    BOOL ok = bm && DeviceIoControl(h, FSCTL_GET_VOLUME_BITMAP, &in, sizeof in, bm, (DWORD)need, &dw, NULL);
    CloseHandle(h);
    if (!ok || bm->StartingLcn.QuadPart != 0) {
        free(bm);
        return 0;
    }
    ULONGLONG nclus = (ULONGLONG)bm->BitmapSize.QuadPart;
    const BYTE *bits = bm->Buffer;

    ULONGLONG freed = 0, done = 0;
    AcquireSRWLockExclusive(&g_storeLock);
    for (ULONGLONG b = 0; b < g_nblk; b++) {
        if (!g_bmap[b] || g_blkGen[b] >= gen0) continue;
        ULONGLONG s = b << BLK_SHIFT, e = s + BLK_SIZE;
        if (s < heap) continue;            // FAT 表などデータ領域より前は触らない
        ULONGLONG c0 = (s - heap) / cl, c1 = (e - heap + cl - 1) / cl;
        if (c1 > nclus) continue;          // 末尾（NTFS の予備ブートセクタなど）は触らない
        BOOL allFree = TRUE;
        for (ULONGLONG c = c0; c < c1 && allFree; ) {
            if (!(c & 7) && c + 8 <= c1) {
                if (bits[c >> 3]) allFree = FALSE;
                c += 8;
            } else {
                if (bits[c >> 3] & (1 << (c & 7))) allFree = FALSE;
                c++;
            }
        }
        if (!allFree) continue;
        blk_free(b);
        freed++;
        if (++done % 1024 == 0) {          // 長く握らない（読み書きを待たせない）
            ReleaseSRWLockExclusive(&g_storeLock);
            AcquireSRWLockExclusive(&g_storeLock);
            if (!g_bmap) break;
        }
    }
    if (freed) ws_adjust();
    ReleaseSRWLockExclusive(&g_storeLock);
    free(bm);
    if (freed) InterlockedAdd64(&g_stReclaimed, (LONG64)freed);
    if (freed && g_verbose) applog(L"回収: %llu ブロック（%llu MB）を返した", freed, (freed << BLK_SHIFT) >> 20);
    return freed;
}

static DWORD WINAPI reclaim_main(LPVOID unused) {
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (WaitForSingleObject(g_reclaimStop, 2000) == WAIT_TIMEOUT)
        reclaim_once();
    return 0;
}

static void reclaim_start(wchar_t letter) {
    g_reclaimLetter = letter;
    g_reclaimStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_reclaimThread = CreateThread(NULL, 0, reclaim_main, NULL, 0, NULL);
}

static void reclaim_stop(void) {
    if (g_reclaimThread) {
        SetEvent(g_reclaimStop);
        WaitForSingleObject(g_reclaimThread, 10000);
        CloseHandle(g_reclaimThread);
        g_reclaimThread = NULL;
    }
    if (g_reclaimStop) CloseHandle(g_reclaimStop);
    g_reclaimStop = NULL;
}

static void log_stats(const wchar_t *when) {
    applog(L"%ls: 確保 %llu MB / TRIM・ZERO 要求 %lld 件（%lld MB、返したブロック %lld）/ 回収で返したブロック %lld",
           when, ((ULONGLONG)g_usedBlk << BLK_SHIFT) >> 20, g_stTrimReq, g_stTrimBytes >> 20, g_stTrimFreed, g_stReclaimed);
}

// ---- TEMP / TMP 環境変数 ----

typedef struct { HKEY root; const wchar_t *sub, *name, *key; BOOL sys; } EnvSlot;
static const wchar_t ENV_USER[] = L"Environment";
static const wchar_t ENV_SYS[]  = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
static const EnvSlot SLOTS[4] = {
    { HKEY_CURRENT_USER,  ENV_USER, L"TEMP", L"UserTEMP",   FALSE },
    { HKEY_CURRENT_USER,  ENV_USER, L"TMP",  L"UserTMP",    FALSE },
    { HKEY_LOCAL_MACHINE, ENV_SYS,  L"TEMP", L"SystemTEMP", TRUE  },
    { HKEY_LOCAL_MACHINE, ENV_SYS,  L"TMP",  L"SystemTMP",  TRUE  },
};
static BOOL g_envChanged;

static BOOL env_read(const EnvSlot *s, wchar_t *buf, DWORD cch, DWORD *type) {
    DWORD cb = cch * sizeof(wchar_t);
    *type = 0;
    return RegGetValueW(s->root, s->sub, s->name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                        type, buf, &cb) == ERROR_SUCCESS;
}

static void env_broadcast(void) {
    broadcast_async(BC_ENVIRONMENT, 0);
}

// 元の値を ini に控えてから書き換える。値が無かったものは Type=0 として控える
static BOOL env_apply(const Settings *s, DWORD *err) {
    for (int i = 0; i < 4; i++) {
        const EnvSlot *e = &SLOTS[i];
        if (e->sys ? !s->tempSys : !s->tempUser) continue;
        wchar_t val[2048] = L"", tk[40];
        DWORD type;
        if (!env_read(e, val, ARRAYSIZE(val), &type)) { type = 0; val[0] = 0; }
        swprintf(tk, ARRAYSIZE(tk), L"%lsType", e->key);
        ini_put(L"Backup", e->key, val);
        ini_put_int(L"Backup", tk, type);
    }
    ini_flush();
    for (int i = 0; i < 4; i++) {
        const EnvSlot *e = &SLOTS[i];
        if (e->sys ? !s->tempSys : !s->tempUser) continue;
        LSTATUS r = RegSetKeyValueW(e->root, e->sub, e->name, REG_EXPAND_SZ, s->tempDir,
                                    (DWORD)((wcslen(s->tempDir) + 1) * sizeof(wchar_t)));
        if (r != ERROR_SUCCESS) { *err = (DWORD)r; return FALSE; }
        g_envChanged = TRUE;
    }
    env_broadcast();
    applog(L"TEMP/TMP を %ls に切り替えた（ユーザー: %ls、システム: %ls）", s->tempDir,
         s->tempUser ? L"はい" : L"いいえ", s->tempSys ? L"はい" : L"いいえ");
    return TRUE;
}

// ini の控えから書き戻す。戻せたものから控えを消す
static int env_restore(void) {
    int restored = 0;
    for (int i = 0; i < 4; i++) {
        const EnvSlot *e = &SLOTS[i];
        wchar_t tk[40], tb[16], val[2048];
        swprintf(tk, ARRAYSIZE(tk), L"%lsType", e->key);
        ini_get(L"Backup", tk, L"", tb, ARRAYSIZE(tb));
        if (!tb[0]) continue;
        DWORD type = (DWORD)wcstoul(tb, NULL, 10);
        ini_get(L"Backup", e->key, L"", val, ARRAYSIZE(val));
        LSTATUS r;
        if (type == REG_SZ || type == REG_EXPAND_SZ)
            r = RegSetKeyValueW(e->root, e->sub, e->name, type, val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
        else
            r = RegDeleteKeyValueW(e->root, e->sub, e->name);
        if (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) {
            ini_put(L"Backup", tk, NULL);
            ini_put(L"Backup", e->key, NULL);
            restored++;
            applog(L"%ls\\%ls を元に戻した: %ls", e->sys ? L"システム" : L"ユーザー", e->name, val);
        } else {
            applog(L"%ls\\%ls を戻せなかった（エラー %ld）", e->sys ? L"システム" : L"ユーザー", e->name, r);
        }
    }
    ini_flush();
    if (restored) env_broadcast();
    g_envChanged = FALSE;
    return restored;
}

static void env_describe(BOOL sys, wchar_t *buf, size_t n) {
    wchar_t t[1024] = L"", m[1024] = L"";
    DWORD ty;
    if (!env_read(&SLOTS[sys ? 2 : 0], t, ARRAYSIZE(t), &ty)) wcscpy(t, L"（未設定）");
    if (!env_read(&SLOTS[sys ? 3 : 1], m, ARRAYSIZE(m), &ty)) wcscpy(m, L"（未設定）");
    if (!wcscmp(t, m)) swprintf(buf, n, L"%ls: %ls", sys ? L"システム" : L"ユーザー", t);
    else swprintf(buf, n, L"%ls: TEMP=%ls / TMP=%ls", sys ? L"システム" : L"ユーザー", t, m);
}

// ---- 前回の異常終了の後始末 ----

// そのドライブが RamDay の作った ImDisk デバイスなら、その番号を返す
static BOOL is_our_device(wchar_t letter, ULONG *num) {
    wchar_t vol[] = L"\\\\.\\ :";
    vol[4] = letter;
    HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BYTE buf[4096] = { 0 };
    DWORD dw;
    BOOL ok = DeviceIoControl(h, IOCTL_IMDISK_QUERY_DEVICE, NULL, 0, buf, sizeof buf - 2, &dw, NULL);
    CloseHandle(h);
    if (!ok) return FALSE;
    IMDISK_CREATE_DATA *cd = (IMDISK_CREATE_DATA *)buf;
    *num = cd->DeviceNumber;
    return wcsstr(cd->FileName, L"RamDay_") != NULL;
}

static BOOL recover(void) {
    if (!ini_get_int(L"Backup", L"Active", 0)) return FALSE;
    applog(L"前回の後始末を行う");
    int n = env_restore();
    wchar_t lb[4];
    ini_get(L"Backup", L"Drive", L"", lb, ARRAYSIZE(lb));
    ULONG num;
    if (lb[0] && is_our_device(lb[0], &num)) {
        if (remove_by_number(num)) applog(L"残っていたデバイス %lu を削除した", num);
        remove_letter_leftovers(lb[0]);
        notify_drive(lb[0], FALSE);
    }
    driver_release_if_idle();              // 前回こちらで読み込んだドライバなら、空いていれば止める
    WritePrivateProfileStringW(L"Backup", NULL, NULL, g_iniPath);
    ini_flush();
    applog(L"後始末を終えた（環境変数 %d 件を復元）", n);
    return TRUE;
}

// ---- ワーカーの状態の公開（設定画面が読む） ----
//
// ワーカー（管理者）が名前付き共有メモリに状態を書き、設定画面（一般権限）が読む。
// 一般権限からも読めるよう、認証済みユーザーに読み取りを許す。

enum { ST_STOPPED, ST_STARTING, ST_RUNNING, ST_STOPPING };
typedef struct {
    DWORD     cb;
    LONG      state;
    DWORD     pid;
    ULONGLONG wnd;
    LONG      fs, tempOn;
    WCHAR     letter, pad[3];
    ULONGLONG diskSize, used;
    WCHAR     tempDir[MAX_PATH];
} SharedStatus;
#define STATUS_NAME L"Local\\RamDay_Status"

static HANDLE        g_stMap;
static SharedStatus *g_st;

static void status_open(void) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)",
                                                         SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL);
    g_stMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SharedStatus), STATUS_NAME);
    if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
    g_st = g_stMap ? MapViewOfFile(g_stMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedStatus)) : NULL;
    if (g_st) {
        ZeroMemory(g_st, sizeof *g_st);
        g_st->cb = sizeof *g_st;
        g_st->pid = GetCurrentProcessId();
    }
}

// 状態の公開をやめる（設定画面からは「停止中」に見える）
static void status_close(void) {
    if (g_st) UnmapViewOfFile(g_st);
    if (g_stMap) CloseHandle(g_stMap);
    g_st = NULL;
    g_stMap = NULL;
}

static void status_set(LONG state) {
    if (!g_st) return;
    g_st->wnd = (ULONGLONG)(ULONG_PTR)g_wnd;
    g_st->letter = g_cfg.letter;
    g_st->fs = g_cfg.fs;
    g_st->tempOn = g_envChanged;
    wcsncpy(g_st->tempDir, g_cfg.tempDir, MAX_PATH - 1);
    g_st->diskSize = g_diskSize;
    g_st->used = (ULONGLONG)g_usedBlk << BLK_SHIFT;
    MemoryBarrier();
    g_st->state = state;
}

// 設定画面側: 動いているワーカーの状態を読む。動いていなければ FALSE
static BOOL status_read(SharedStatus *out) {
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, STATUS_NAME);
    if (!m) return FALSE;
    SharedStatus *v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof *v);
    BOOL ok = v && v->cb == sizeof *v;
    if (ok) *out = *v;
    if (v) UnmapViewOfFile(v);
    CloseHandle(m);
    return ok;
}

// ---- 開始・解放 ----

static BOOL g_running;

// ask: 使用中なら強制してよいか本人に訊く（トレイから）。訊かないときは取りやめて FALSE
static BOOL release_all(HWND owner, BOOL force, BOOL ask) {
    if (!g_running) return TRUE;
    wchar_t letter = g_cfg.letter;
    BOOL locked = FALSE;
    HANDLE vol = INVALID_HANDLE_VALUE;
    // 強制（サインアウト・シャットダウンを含む）なら、いちばん大事な TEMP の書き戻しを先に
    if (force && (g_envChanged || ini_get_int(L"Backup", L"Active", 0))) env_restore();
    reclaim_stop();                        // ボリュームを開くので、外す前に止める
    if (g_diskUp) {
        log_stats(L"解放前");
        vol = volume_open_lock(letter, &locked);
        if (vol != INVALID_HANDLE_VALUE && !locked && !force) {
            if (!ask || MessageBoxW(owner,
                    L"RAM ディスク上のファイルを使っているプログラムがあります。\n\n"
                    L"強制的に解放しますか？（保存していない内容は失われます）",
                    APP_TITLE, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
                CloseHandle(vol);
                applog(L"使用中のため解放を取りやめた");
                reclaim_start(letter);
                return FALSE;
            }
        }
        if (!locked) applog(L"ロックできないまま強制的に外す");
    }
    status_set(ST_STOPPING);
    if (g_envChanged || ini_get_int(L"Backup", L"Active", 0)) env_restore();
    if (g_diskUp) disk_remove(letter, vol, locked);
    driver_cleanup();
    WritePrivateProfileStringW(L"Backup", NULL, NULL, g_iniPath);
    ini_flush();
    g_running = FALSE;
    return TRUE;
}

static BOOL start_all(const Settings *s) {
    DWORD err = 0;
    ULONGLONG size = cfg_bytes(s);
    wchar_t sz[32];
    format_bytes(size, sz, ARRAYSIZE(sz));
    applog(L"開始: %lc: 最大 %ls %ls TEMP=%ls", s->letter, sz, FS_NAMES[s->fs], s->temp ? s->tempDir : L"（切り替えない）");
    if (GetLogicalDrives() & (1u << (s->letter - L'A'))) {
        wchar_t m[128];
        swprintf(m, ARRAYSIZE(m), L"%lc: はすでに使われています。別のドライブ文字を選んでください。", s->letter);
        fail_msg(NULL, m, 0);
        return FALSE;
    }
    wchar_t lb[4] = { s->letter, 0 };
    ini_put_int(L"Backup", L"Active", 1);
    ini_put(L"Backup", L"Drive", lb);
    ini_put_int(L"Backup", L"Pid", GetCurrentProcessId());
    ini_flush();
    g_running = TRUE;

    BOOL pending;
    if (!driver_ensure(&err, &pending)) {
        if (pending)
            fail_msg(NULL, L"前回読み込んだ ImDisk ドライバの後片付けが、まだ終わっていません。\n"
                           L"数分待ってからやり直すか、Windows を再起動してください。", 0);
        else
            fail_msg(NULL, L"ImDisk ドライバを読み込めませんでした。", err);
        goto fail;
    }
    if (!disk_create(s->letter, size, &err)) {
        fail_msg(NULL, L"RAM ディスクを作成できませんでした。", err);
        goto fail;
    }
    if (!disk_format(s->letter, s->fs)) {
        fail_msg(NULL, L"フォーマットに失敗しました。容量とファイルシステムの組み合わせを確かめてください。", 0);
        goto fail;
    }
    reclaim_start(s->letter);
    notify_drive(s->letter, TRUE);
    if (s->temp && (s->tempUser || s->tempSys)) {
        int r = SHCreateDirectoryExW(NULL, s->tempDir, NULL);
        if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS) {
            fail_msg(NULL, L"TEMP 用のフォルダーを作れませんでした。", (DWORD)r);
            goto fail;
        }
        if (!env_apply(s, &err)) {
            fail_msg(NULL, L"TEMP 環境変数を書き換えられませんでした。", err);
            goto fail;
        }
    }
    return TRUE;
fail:
    release_all(NULL, TRUE, FALSE);
    return FALSE;
}

// ---- 自分自身を起動する ----

static BOOL is_elevated(void) {
    HANDLE tok;
    TOKEN_ELEVATION e = { 0 };
    DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n);
    CloseHandle(tok);
    return ok && e.TokenIsElevated;
}

// 引数を付けて自分を起動する。elevate なら管理者として（必要なら UAC が出る）。
// 成功すればプロセスのハンドルを返す。UAC で断られたら NULL（GetLastError = ERROR_CANCELLED）
static HANDLE run_self(const wchar_t *args, BOOL elevate) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei = { .cbSize = sizeof sei };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = elevate && !is_elevated() ? L"runas" : L"open";
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return NULL;
    return sei.hProcess;
}

// コマンドラインから exe の部分を除いた残り
static const wchar_t *cmdline_args(void) {
    const wchar_t *p = GetCommandLineW();
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') p++;
        if (*p) p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t') p++;
    }
    while (*p == L' ' || *p == L'\t') p++;
    return p;
}

static void worker_args(const Settings *s, wchar_t *buf, size_t n) {
    wchar_t tmp[MAX_PATH + 64];
    swprintf(buf, n, L"-worker -d %lc -s %llu%ls -fs %ls", s->letter, s->sizeNum, s->unitGB ? L"G" : L"M", FS_NAMES[s->fs]);
    if (s->temp && (s->tempUser || s->tempSys)) {
        swprintf(tmp, ARRAYSIZE(tmp), L" -temp \"%ls\" -scope %ls", s->tempDir,
                 s->tempUser && s->tempSys ? L"both" : s->tempUser ? L"user" : L"system");
    } else {
        wcscpy(tmp, L" -notemp");
    }
    wcsncat(buf, tmp, n - wcslen(buf) - 1);
}

// ---- 設定画面 ----

#define TIMER_UI     2
#define UI_MS        500
#define DLG_TITLE    L"RamDay — RAM ディスクの作成"

static HANDLE  g_uiWorker;                 // 「開始」で起動したワーカー（起動中の見張り用）
static LONG    g_uiShown = -1;             // 前回表示した状態

static void dlg_fill_drives(HWND dlg, wchar_t sel, wchar_t include) {
    HWND cb = GetDlgItem(dlg, IDC_DRIVE);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    DWORD used = GetLogicalDrives();
    int selIdx = -1, lastFree = -1;
    for (wchar_t c = L'D'; c <= L'Z'; c++) {
        if ((used & (1u << (c - L'A'))) && c != include) continue;
        wchar_t t[4] = { c, L':', 0 };
        int i = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
        SendMessageW(cb, CB_SETITEMDATA, i, c);
        if (c == sel) selIdx = i;
        lastFree = i;
    }
    SendMessageW(cb, CB_SETCURSEL, selIdx >= 0 ? selIdx : lastFree, 0);
}

static wchar_t dlg_letter(HWND dlg) {
    HWND cb = GetDlgItem(dlg, IDC_DRIVE);
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : (wchar_t)SendMessageW(cb, CB_GETITEMDATA, i, 0);
}

// 切り替え先が「X:\...」なら、選ばれているドライブ文字に付け替える
static void dlg_sync_tempdir(HWND dlg) {
    wchar_t nl = dlg_letter(dlg), td[MAX_PATH];
    GetDlgItemTextW(dlg, IDC_TEMP_DIR, td, ARRAYSIZE(td));
    if (nl && td[0] && td[1] == L':' && towupper(td[0]) != nl) {
        td[0] = nl;
        SetDlgItemTextW(dlg, IDC_TEMP_DIR, td);
    }
}

// 設定を入力欄に並べる
static void dlg_show_settings(HWND dlg, const Settings *c, wchar_t include) {
    dlg_fill_drives(dlg, c->letter, include);
    SetDlgItemInt(dlg, IDC_SIZE_EDIT, (UINT)c->sizeNum, FALSE);
    SendDlgItemMessageW(dlg, IDC_UNIT, CB_SETCURSEL, c->unitGB ? 1 : 0, 0);
    SendDlgItemMessageW(dlg, IDC_FS, CB_SETCURSEL, c->fs, 0);
    CheckDlgButton(dlg, IDC_TEMP_ON, c->temp ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_TEMP_USER, c->tempUser ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_TEMP_SYS, c->tempSys ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(dlg, IDC_TEMP_DIR, c->tempDir);
    dlg_sync_tempdir(dlg);
}

// 動いているワーカーの設定（共有メモリ）を Settings にする。TEMP の対象（ユーザー/システム）は ini の控えから
static void settings_from_status(const SharedStatus *st, Settings *c) {
    *c = g_cfg;
    c->letter = st->letter;
    c->fs = (st->fs >= 0 && st->fs < FS_COUNT) ? st->fs : FS_NTFS;
    BOOL gb = st->diskSize && !(st->diskSize & ((1ULL << 30) - 1));
    c->unitGB = gb;
    c->sizeNum = st->diskSize >> (gb ? 30 : 20);
    c->temp = st->tempOn;
    if (st->tempOn) {
        wcsncpy(c->tempDir, st->tempDir, MAX_PATH - 1);
        c->tempUser = ini_get_int(L"Backup", L"UserTEMPType", 99) != 99;
        c->tempSys = ini_get_int(L"Backup", L"SystemTEMPType", 99) != 99;
    }
}

// 入力欄の有効・無効。動いている間は設定を変えられない
static void dlg_update_enable(HWND dlg, BOOL editable) {
    static const int fields[] = { IDC_DRIVE, IDC_SIZE_EDIT, IDC_UNIT, IDC_FS, IDC_TEMP_ON };
    for (size_t i = 0; i < ARRAYSIZE(fields); i++) EnableWindow(GetDlgItem(dlg, fields[i]), editable);
    BOOL on = editable && IsDlgButtonChecked(dlg, IDC_TEMP_ON) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, IDC_TEMP_DIR), on);
    EnableWindow(GetDlgItem(dlg, IDC_TEMP_USER), on);
    EnableWindow(GetDlgItem(dlg, IDC_TEMP_SYS), on);
    EnableWindow(GetDlgItem(dlg, IDC_TEMP_LABEL), on);
}

static void set_text_if_changed(HWND dlg, int id, const wchar_t *text) {
    wchar_t cur[2100];
    GetDlgItemTextW(dlg, id, cur, ARRAYSIZE(cur));
    if (wcscmp(cur, text)) SetDlgItemTextW(dlg, id, text);
}

static void ui_refresh(HWND dlg) {
    SharedStatus st;
    BOOL alive = status_read(&st);
    LONG state = alive ? st.state : ST_STOPPED;
    // 起動したワーカーが状態を公開する前、または失敗して終わったとき
    if (g_uiWorker) {
        DWORD code;
        if (GetExitCodeProcess(g_uiWorker, &code) && code != STILL_ACTIVE) {
            CloseHandle(g_uiWorker);
            g_uiWorker = NULL;
            if (code != 0) {
                wchar_t err[1024];
                ini_get(L"Status", L"LastError", L"", err, ARRAYSIZE(err));
                if (!err[0]) swprintf(err, ARRAYSIZE(err), L"RAM ディスクを開始できませんでした（終了コード %lu）。", code);
                MessageBoxW(dlg, err, APP_TITLE, MB_ICONERROR);
            }
        } else if (!alive || state == ST_STARTING) {
            state = ST_STARTING;
        } else {
            CloseHandle(g_uiWorker);       // 動き出したので見張りは終わり
            g_uiWorker = NULL;
        }
    }

    wchar_t text[300], u[32], mx[32];
    switch (state) {
    case ST_STARTING: wcscpy(text, L"開始しています…"); break;
    case ST_STOPPING: wcscpy(text, L"終了しています…"); break;
    case ST_RUNNING:
        format_bytes(st.used, u, ARRAYSIZE(u));
        format_bytes(st.diskSize, mx, ARRAYSIZE(mx));
        swprintf(text, ARRAYSIZE(text), L"動作中 — %lc:（%ls）　メモリ使用 %ls / 最大 %ls",
                 st.letter, (st.fs >= 0 && st.fs < FS_COUNT) ? FS_NAMES[st.fs] : L"?", u, mx);
        break;
    default: wcscpy(text, L"停止中"); break;
    }
    set_text_if_changed(dlg, IDC_STATUS, text);

    wchar_t d[2100];
    env_describe(FALSE, d, ARRAYSIZE(d));
    set_text_if_changed(dlg, IDC_CUR_USER, d);
    env_describe(TRUE, d, ARRAYSIZE(d));
    set_text_if_changed(dlg, IDC_CUR_SYS, d);
    set_text_if_changed(dlg, IDC_CUR_LABEL, state == ST_RUNNING && st.tempOn
                        ? L"現在の値（RamDay が切り替え中。終了すると元の値に戻ります）:"
                        : L"現在の値（開始すると控えて、終了するときにこの値へ戻します）:");

    if (state != g_uiShown) {
        BOOL stopped = state == ST_STOPPED;
        if (state == ST_RUNNING) {         // 動いている RAM ディスクの設定を見せる
            Settings c;
            settings_from_status(&st, &c);
            dlg_show_settings(dlg, &c, c.letter);
        } else if (stopped && g_uiShown != -1) {   // 止まったら次に開始する設定（ini）に戻す
            dlg_show_settings(dlg, &g_cfg, 0);
        }
        dlg_update_enable(dlg, stopped);
        EnableWindow(GetDlgItem(dlg, IDC_START), stopped);
        EnableWindow(GetDlgItem(dlg, IDC_STOP), state == ST_RUNNING);
        SendMessageW(dlg, DM_SETDEFID, stopped ? IDC_START : IDCANCEL, 0);
        g_uiShown = state;
    }
}

static BOOL dlg_collect(HWND dlg, Settings *out) {
    Settings s = g_cfg;
    BOOL ok;
    s.letter = dlg_letter(dlg);
    s.sizeNum = GetDlgItemInt(dlg, IDC_SIZE_EDIT, &ok, FALSE);
    s.unitGB = SendDlgItemMessageW(dlg, IDC_UNIT, CB_GETCURSEL, 0, 0) == 1;
    s.fs = (int)SendDlgItemMessageW(dlg, IDC_FS, CB_GETCURSEL, 0, 0);
    s.temp = IsDlgButtonChecked(dlg, IDC_TEMP_ON) == BST_CHECKED;
    s.tempUser = IsDlgButtonChecked(dlg, IDC_TEMP_USER) == BST_CHECKED;
    s.tempSys = IsDlgButtonChecked(dlg, IDC_TEMP_SYS) == BST_CHECKED;
    GetDlgItemTextW(dlg, IDC_TEMP_DIR, s.tempDir, ARRAYSIZE(s.tempDir));
    ULONGLONG bytes = cfg_bytes(&s);
    const wchar_t *bad = NULL;
    wchar_t m[200];
    if (!s.letter) bad = L"ドライブ文字を選んでください。";
    else if (!ok || bytes < MIN_SIZE) bad = L"最大容量は 16 MB 以上にしてください。";
    else if (bytes > g_physTotal) {
        format_bytes(g_physTotal, m + 100, 100);
        swprintf(m, 100, L"最大容量は実メモリ（%ls）以下にしてください。", m + 100);
        bad = m;
    } else if (s.fs == FS_FAT32 && bytes > FAT32_MAX)
        bad = L"FAT32 は 32 GB までです。exFAT か NTFS を選んでください。";
    else if (s.temp && (s.tempUser || s.tempSys) &&
             (towupper(s.tempDir[0]) != s.letter || s.tempDir[1] != L':' || s.tempDir[2] != L'\\'))
        bad = L"TEMP の切り替え先は、作る RAM ディスク上のフォルダー（例: R:\\Temp）にしてください。";
    if (bad) {
        MessageBoxW(dlg, bad, APP_TITLE, MB_ICONWARNING);
        return FALSE;
    }
    s.tempDir[0] = s.letter;
    *out = s;
    return TRUE;
}

static void ui_start(HWND dlg) {
    Settings s;
    if (!dlg_collect(dlg, &s)) return;
    g_cfg = s;
    settings_save(&g_cfg);
    ini_put(L"Status", L"LastError", NULL);
    ini_flush();
    wchar_t args[MAX_PATH + 128];
    worker_args(&g_cfg, args, ARRAYSIZE(args));
    HANDLE p = run_self(args, TRUE);
    if (!p) {
        DWORD e = GetLastError();
        if (e != ERROR_CANCELLED) fail_msg(dlg, L"RAM ディスクを管理するプロセスを起動できませんでした。", e);
        return;
    }
    g_uiWorker = p;
    ui_refresh(dlg);
}

static void ui_stop(HWND dlg) {
    SharedStatus st;
    if (!status_read(&st) || st.state != ST_RUNNING) return;
    HWND w = (HWND)(ULONG_PTR)st.wnd;
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    DWORD_PTR done = 0;
    BOOL sent = SendMessageTimeoutW(w, WM_RELEASE_REQ, 0, 0, SMTO_ABORTIFHUNG, 60000, &done);
    if (sent && !done &&
        MessageBoxW(dlg, L"RAM ディスク上のファイルを使っているプログラムがあります。\n\n"
                         L"強制的に終了しますか？（保存していない内容は失われます）",
                    APP_TITLE, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES)
        sent = SendMessageTimeoutW(w, WM_RELEASE_REQ, 1, 0, SMTO_ABORTIFHUNG, 60000, &done);
    SetCursor(old);
    if (!sent) fail_msg(dlg, L"RAM ディスクを管理するプロセスが応答しません。", GetLastError());
    ui_refresh(dlg);
}

static INT_PTR CALLBACK setup_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)g_iconLarge);
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);
        SendDlgItemMessageW(dlg, IDC_SIZE_EDIT, EM_LIMITTEXT, 7, 0);
        HWND u = GetDlgItem(dlg, IDC_UNIT);
        SendMessageW(u, CB_ADDSTRING, 0, (LPARAM)L"MB");
        SendMessageW(u, CB_ADDSTRING, 0, (LPARAM)L"GB");
        HWND f = GetDlgItem(dlg, IDC_FS);
        for (int i = 0; i < FS_COUNT; i++) SendMessageW(f, CB_ADDSTRING, 0, (LPARAM)FS_NAMES[i]);
        dlg_show_settings(dlg, &g_cfg, 0);
        wchar_t ram[32], info[64];
        format_bytes(g_physTotal, ram, ARRAYSIZE(ram));
        swprintf(info, ARRAYSIZE(info), L"（実メモリ %ls）", ram);
        SetDlgItemTextW(dlg, IDC_MAXINFO, info);
        g_uiShown = -1;
        ui_refresh(dlg);
        SetTimer(dlg, TIMER_UI, UI_MS, NULL);
        return TRUE;
    }
    case WM_TIMER:
        if (wp == TIMER_UI) ui_refresh(dlg);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TEMP_ON:
            dlg_update_enable(dlg, TRUE);
            return TRUE;
        case IDC_DRIVE:
            if (HIWORD(wp) == CBN_SELCHANGE) dlg_sync_tempdir(dlg);
            return TRUE;
        case IDC_START:
            if (IsWindowEnabled(GetDlgItem(dlg, IDC_START))) ui_start(dlg);
            return TRUE;
        case IDC_STOP:
            ui_stop(dlg);
            return TRUE;
        case IDCANCEL:
            KillTimer(dlg, TIMER_UI);
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// 設定画面を開く（すでに開いていれば前に出す）
static int ui_main(void) {
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\RamDay_UI");
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_ACCESS_DENIED) {
        HWND d = FindWindowW(L"#32770", DLG_TITLE);
        if (d) {
            ShowWindow(d, SW_RESTORE);
            SetForegroundWindow(d);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    // ワーカーが動いていないのに TEMP の控えが残っている = 前回が正しく終わらなかった
    SharedStatus st;
    if (!status_read(&st) && ini_get_int(L"Backup", L"Active", 0) &&
        MessageBoxW(NULL, L"前回 RamDay が正しく終了しなかったため、TEMP 環境変数が RAM ディスクを指したままの"
                          L"可能性があります。\n\n元に戻しますか？（管理者権限の確認が出ます）",
                    APP_TITLE, MB_ICONWARNING | MB_YESNO) == IDYES) {
        HANDLE p = run_self(L"-restore -quiet", TRUE);
        if (p) {
            WaitForSingleObject(p, 60000);
            CloseHandle(p);
        }
    }
    DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_SETUP), NULL, setup_proc);
    if (g_uiWorker) CloseHandle(g_uiWorker);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

// ---- タスクトレイ（ワーカー） ----

static ULONGLONG used_bytes(void) {
    return (ULONGLONG)g_usedBlk << BLK_SHIFT;
}

static void update_tip(void) {
    wchar_t u[32], mx[32];
    format_bytes(used_bytes(), u, ARRAYSIZE(u));
    format_bytes(g_diskSize, mx, ARRAYSIZE(mx));
    swprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"RamDay %lc:  メモリ使用 %ls / 最大 %ls", g_cfg.letter, u, mx);
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void tray_add(void) {
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSmall;
    wcscpy(g_nid.szTip, APP_TITLE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    update_tip();
}

static void tray_balloon(const wchar_t *text) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfo, text, ARRAYSIZE(g_nid.szInfo) - 1);
    wcscpy(g_nid.szInfoTitle, APP_TITLE);
    g_nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    g_nid.hBalloonIcon = g_iconLarge;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void open_drive(void) {
    wchar_t root[] = L" :\\";
    root[0] = g_cfg.letter;
    ShellExecuteW(NULL, L"open", root, NULL, NULL, SW_SHOWNORMAL);
}

static void open_settings(void) {
    HANDLE p = run_self(L"", FALSE);
    if (p) CloseHandle(p);
}

static void show_status(void) {
    wchar_t u[32], mx[32], fu[32] = L"?", text[1200], du[600], ds[600];
    format_bytes(used_bytes(), u, ARRAYSIZE(u));
    format_bytes(g_diskSize, mx, ARRAYSIZE(mx));
    wchar_t root[] = L" :\\";
    root[0] = g_cfg.letter;
    ULARGE_INTEGER avail, total, freeb;
    if (GetDiskFreeSpaceExW(root, &avail, &total, &freeb))
        format_bytes(total.QuadPart - freeb.QuadPart, fu, ARRAYSIZE(fu));
    env_describe(FALSE, du, ARRAYSIZE(du));
    env_describe(TRUE, ds, ARRAYSIZE(ds));
    swprintf(text, ARRAYSIZE(text),
             L"ドライブ: %lc:（%ls）\n最大容量: %ls\n確保しているメモリ: %ls\nファイルシステム上の使用量: %ls\n\n"
             L"TEMP の今の値\n  %ls\n  %ls",
             g_cfg.letter, FS_NAMES[g_cfg.fs], mx, u, fu, du, ds);
    MessageBoxW(g_wnd, text, APP_TITLE, MB_ICONINFORMATION);
}

static void show_menu(void) {
    HMENU m = CreatePopupMenu();
    wchar_t st[128], u[32], mx[32], op[32];
    format_bytes(used_bytes(), u, ARRAYSIZE(u));
    format_bytes(g_diskSize, mx, ARRAYSIZE(mx));
    swprintf(st, ARRAYSIZE(st), L"%lc: メモリ使用 %ls / 最大 %ls ...", g_cfg.letter, u, mx);
    swprintf(op, ARRAYSIZE(op), L"%lc: を開く", g_cfg.letter);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"設定画面を開く");
    AppendMenuW(m, MF_STRING, IDM_STATUS, st);
    AppendMenuW(m, MF_STRING, IDM_OPEN, op);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_RELEASE, L"解放して終了");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, NULL);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static BOOL quit_after_release(BOOL force, BOOL ask) {
    if (!release_all(g_wnd, force, ask)) return FALSE;
    KillTimer(g_wnd, TIMER_TIP);
    KillTimer(g_wnd, TIMER_STATUS);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_wnd);
    return TRUE;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        tray_add();
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_STATUS && g_running) status_set(ST_RUNNING);
        if (wp == TIMER_TIP) {
            update_tip();
            static LONG64 lastUsed = -1, lastTrim = -1;
            if (g_verbose && (g_usedBlk != lastUsed || g_stTrimReq != lastTrim)) {
                lastUsed = g_usedBlk;
                lastTrim = g_stTrimReq;
                log_stats(L"状況");
            }
        }
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK: open_settings(); break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: show_menu(); break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SETTINGS: open_settings(); break;
        case IDM_STATUS: show_status(); break;
        case IDM_OPEN: open_drive(); break;
        case IDM_RELEASE: quit_after_release(FALSE, TRUE); break;
        }
        return 0;
    case WM_RELEASE_REQ:                   // 設定画面・-release から。訊くのは依頼した側
        return quit_after_release(wp != 0, FALSE);   // 1 = 解放した、0 = 使用中で取りやめた
    case WM_CLOSE:
        quit_after_release(FALSE, TRUE);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        // サインアウト・シャットダウン: 対話せず元に戻す（間に合わなければ次回起動時に戻す）
        if (wp) {
            g_ending = TRUE;
            applog(L"セッション終了のため解放する");
            release_all(NULL, TRUE, FALSE);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- 起動 ----

static BOOL parse_size(const wchar_t *s, Settings *c) {
    wchar_t *end;
    ULONGLONG v = wcstoull(s, &end, 10);
    if (!v) return FALSE;
    if (*end == L'G' || *end == L'g') c->unitGB = TRUE;
    else if (*end == L'M' || *end == L'm' || !*end) c->unitGB = FALSE;
    else return FALSE;
    c->sizeNum = v;
    return TRUE;
}

static int release_running(BOOL force) {
    HWND w = FindWindowW(WND_CLASS, NULL);
    if (!w) return 2;                      // 動いていない
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    DWORD_PTR done = 0;
    if (!SendMessageTimeoutW(w, WM_RELEASE_REQ, force, 0, SMTO_ABORTIFHUNG, 180000, &done)) {
        if (p) CloseHandle(p);
        return 3;                          // 返事がない
    }
    if (!done) {
        if (p) CloseHandle(p);
        return 4;                          // 使用中のため取りやめた
    }
    DWORD r = p ? WaitForSingleObject(p, 30000) : WAIT_OBJECT_0;
    if (p) CloseHandle(p);
    return r == WAIT_OBJECT_0 ? 0 : 3;
}

// 管理者が要る作業を、管理者でなければ管理者として起動し直して終わりを待つ
static int run_elevated_and_wait(const wchar_t *args) {
    HANDLE p = run_self(args, TRUE);
    if (!p) return GetLastError() == ERROR_CANCELLED ? 5 : 1;
    WaitForSingleObject(p, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(p, &code);
    CloseHandle(p);
    return (int)code;
}

// RAM ディスクを作って管理する（管理者）
static int worker_main(void) {
    g_worker = TRUE;
    g_quiet = TRUE;                        // 失敗は ini とログに残し、設定画面が表示する
    // 終了したばかりの前のワーカーが後始末をしている間は、少し待つ
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Global\\RamDay_Instance");
    DWORD w = mutex ? WaitForSingleObject(mutex, 10000) : WAIT_FAILED;
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
        fail_msg(NULL, L"RamDay はすでに動いています（タスクトレイのアイコンから操作できます）。", 0);
        return 2;
    }
    recover();                             // 前回が正しく終わっていなければ、先に元へ戻す
    if (g_cfg.letter < L'D' || g_cfg.letter > L'Z' || cfg_bytes(&g_cfg) < MIN_SIZE) {
        fail_msg(NULL, L"ドライブ文字（D〜Z）か容量（16 MB 以上）の指定が正しくありません。", 0);
        ReleaseMutex(mutex);
        return 1;
    }

    WNDCLASSEXW wc = { .cbSize = sizeof wc };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_inst;
    wc.hIcon = g_iconLarge;
    wc.hIconSm = g_iconSmall;
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, WND_CLASS, APP_TITLE, WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_wnd, g_msgTaskbarCreated, MSGFLT_ALLOW, NULL);
    // 一般権限の設定画面・-release からの解放依頼を受け付ける
    ChangeWindowMessageFilterEx(g_wnd, WM_RELEASE_REQ, MSGFLT_ALLOW, NULL);
    status_open();
    status_set(ST_STARTING);

    BOOL ok = start_all(&g_cfg);
    if (!ok) {
        DestroyWindow(g_wnd);
        status_close();
        ReleaseMutex(mutex);
        broadcast_wait(5000);
        return 1;
    }
    status_set(ST_RUNNING);
    tray_add();
    SetTimer(g_wnd, TIMER_TIP, TIP_MS, NULL);
    SetTimer(g_wnd, TIMER_STATUS, STATUS_MS, NULL);
    wchar_t mx[32], text[256];
    format_bytes(g_diskSize, mx, ARRAYSIZE(mx));
    if (g_envChanged)
        swprintf(text, ARRAYSIZE(text), L"%lc: を作りました（最大 %ls・%ls）。\nTEMP と TMP を %ls に切り替えました。",
                 g_cfg.letter, mx, FS_NAMES[g_cfg.fs], g_cfg.tempDir);
    else
        swprintf(text, ARRAYSIZE(text), L"%lc: を作りました（最大 %ls・%ls）。", g_cfg.letter, mx, FS_NAMES[g_cfg.fs]);
    tray_balloon(text);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    release_all(NULL, TRUE, FALSE);        // 念のため（通常はここに来る前に済んでいる）
    // 解放が済んだら、すぐ次のワーカーを起動できるようにする（通知を待つのはその後）
    status_close();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    broadcast_wait(5000);                  // ドライブ削除・環境変数の通知を届けきる
    applog(L"終了");
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int show) {
    (void)prev; (void)cmdLine; (void)show;
    g_inst = inst;
    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    *wcsrchr(g_exeDir, L'\\') = 0;
    swprintf(g_iniPath, MAX_PATH, L"%ls\\RamDay.ini", g_exeDir);
    swprintf(g_logPath, MAX_PATH, L"%ls\\RamDay.log", g_exeDir);
    WIN32_FILE_ATTRIBUTE_DATA la;
    if (GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &la) && la.nFileSizeLow > (512u << 10))
        DeleteFileW(g_logPath);
    ini_ensure();
    MEMORYSTATUSEX ms = { .dwLength = sizeof ms };
    GlobalMemoryStatusEx(&ms);
    g_physTotal = ms.ullTotalPhys;

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    g_iconLarge = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    settings_load(&g_cfg);
    BOOL cli = FALSE, worker = FALSE, doRelease = FALSE, force = FALSE, doRestore = FALSE, tempGiven = FALSE;
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        const wchar_t *a = argv[i];
        if (a[0] == L'-' || a[0] == L'/') a++;
        BOOL has = i + 1 < argc;
        if (!_wcsicmp(a, L"worker")) worker = TRUE;
        else if (!_wcsicmp(a, L"release")) doRelease = TRUE;
        else if (!_wcsicmp(a, L"force")) force = TRUE;
        else if (!_wcsicmp(a, L"restore")) doRestore = TRUE;
        else if (!_wcsicmp(a, L"quiet")) g_quiet = TRUE;
        else if (!_wcsicmp(a, L"verbose")) g_verbose = TRUE;
        else if (!_wcsicmp(a, L"notemp")) { g_cfg.temp = FALSE; cli = TRUE; }
        else if (!_wcsicmp(a, L"d") && has) { g_cfg.letter = towupper(argv[++i][0]); cli = TRUE; }
        else if (!_wcsicmp(a, L"s") && has) {
            if (!parse_size(argv[++i], &g_cfg)) { fail_msg(NULL, L"-s の値が読めません（例: -s 4G、-s 512M）", 0); return 1; }
            cli = TRUE;
        } else if (!_wcsicmp(a, L"fs") && has) {
            const wchar_t *v = argv[++i];
            for (int k = 0; k < FS_COUNT; k++) if (!_wcsicmp(v, FS_NAMES[k])) g_cfg.fs = k;
            cli = TRUE;
        } else if (!_wcsicmp(a, L"temp") && has) {
            wcsncpy(g_cfg.tempDir, argv[++i], MAX_PATH - 1);
            g_cfg.temp = tempGiven = TRUE;
            cli = TRUE;
        } else if (!_wcsicmp(a, L"scope") && has) {
            const wchar_t *v = argv[++i];
            g_cfg.tempUser = _wcsicmp(v, L"system") != 0;
            g_cfg.tempSys = _wcsicmp(v, L"user") != 0;
            cli = TRUE;
        }
    }
    if (argv) LocalFree(argv);
    if (cli && !tempGiven) swprintf(g_cfg.tempDir, MAX_PATH, L"%lc:\\Temp", g_cfg.letter);

    if (doRelease) return release_running(force);   // ワーカーが受け付けるので管理者は要らない

    if (doRestore) {
        if (!is_elevated()) {
            wchar_t args[64];
            swprintf(args, ARRAYSIZE(args), L"-restore%ls", g_quiet ? L" -quiet" : L"");
            return run_elevated_and_wait(args);
        }
        HANDLE mutex = CreateMutexW(NULL, TRUE, L"Global\\RamDay_Instance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 2;   // 動いている間は後始末しない
        BOOL did = recover();
        driver_release_if_idle();          // 読み込んだままのドライバが空いていれば止める
        broadcast_wait(5000);
        ReleaseMutex(mutex);
        if (did && !g_quiet)
            MessageBoxW(NULL, L"TEMP 環境変数などを元に戻しました。", APP_TITLE, MB_ICONINFORMATION);
        return 0;
    }

    if (worker || cli) {                   // ワーカー（コマンドラインからの開始もここ）
        if (!is_elevated()) {
            wchar_t args[2048];
            swprintf(args, ARRAYSIZE(args), L"%ls%ls", worker ? L"" : L"-worker ", cmdline_args());
            HANDLE p = run_self(args, TRUE);
            if (!p) return GetLastError() == ERROR_CANCELLED ? 5 : 1;
            CloseHandle(p);
            return 0;
        }
        return worker_main();
    }
    return ui_main();
}
