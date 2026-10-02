// RamDayLaunch: 前回「開始」した設定で RamDay を開始して、すぐ終わる（スタートアップ登録用）
//
// 中身は同じフォルダーの RamDay.exe を -start 付きで起動するだけ。設定の読み込み、
// 管理者としてのワーカーの起動、失敗時の表示は RamDay.exe 側が行う。
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int show) {
    (void)inst; (void)prev; (void)cmdLine; (void)show;
    wchar_t dir[MAX_PATH], exe[MAX_PATH + 16];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    *wcsrchr(dir, L'\\') = 0;
    swprintf(exe, MAX_PATH + 16, L"%ls\\RamDay.exe", dir);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"同じフォルダーに RamDay.exe がありません。\n"
                          L"RamDayLaunch.exe は RamDay.exe と同じ場所に置いてください。",
                    L"RamDay", MB_ICONERROR);
        return 1;
    }
    HINSTANCE r = ShellExecuteW(NULL, L"open", exe, L"-start", dir, SW_SHOWNORMAL);
    return (INT_PTR)r > 32 ? 0 : 1;
}
