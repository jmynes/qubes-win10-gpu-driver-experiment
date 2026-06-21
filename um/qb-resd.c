// QUBES resolution corrector daemon (qb-resd.exe) — runs in the user's interactive
// session. Tails the gui-agent log for its RAW requested work-area size
// ("ResolutionChangeThread: resolution change: WxH", logged before the gui-agent's
// area-Jaccard snap), debounces, and applies that EXACT W x H via the KMDOD escape +
// ChangeDisplaySettings. Because the driver's SetPreferredMode shrinks DispInfo, the
// gui-agent's subsequent snap-UP to a larger grid mode fails (DISP_CHANGE_BADMODE) and
// the exact value persists -> Win7-parity panel-aware maximize. No Qubes deps.
#include <windows.h>
#include <stdio.h>
#include <d3dkmthk.h>
#include "qb_escape.h"

typedef NTSTATUS (APIENTRY *PFN_OPEN)(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME*);
typedef NTSTATUS (APIENTRY *PFN_ESC)(const D3DKMT_ESCAPE*);
typedef NTSTATUS (APIENTRY *PFN_CLOSE)(const D3DKMT_CLOSEADAPTER*);
static PFN_OPEN pOpen; static PFN_ESC pEsc; static PFN_CLOSE pClose;
static int g_lastW = 0, g_lastH = 0;

static void LogLine(const wchar_t* fmt, ...) {
    wchar_t b[512]; va_list a; va_start(a, fmt); _vsnwprintf_s(b, 512, _TRUNCATE, fmt, a); va_end(a);
    wprintf(L"%s\n", b); fflush(stdout);
}

static BOOL SetExact(ULONG w, ULONG h) {
    if ((int)w == g_lastW && (int)h == g_lastH) return TRUE;     // already applied
    DISPLAY_DEVICEW dd; ZeroMemory(&dd, sizeof(dd)); dd.cb = sizeof(dd);
    if (!EnumDisplayDevicesW(NULL, 0, &dd, 0)) return FALSE;

    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME oa; ZeroMemory(&oa, sizeof(oa));
    wcscpy_s(oa.DeviceName, ARRAYSIZE(oa.DeviceName), dd.DeviceName);
    if (pOpen(&oa) != 0) return FALSE;

    QB_SETPREFERREDMODE_ESCAPE priv; ZeroMemory(&priv, sizeof(priv));
    priv.Magic = QB_ESCAPE_MAGIC; priv.Version = QB_ESCAPE_VERSION;
    priv.Op = QB_ESC_SET_PREFERRED_MODE; priv.Width = w; priv.Height = h;
    D3DKMT_ESCAPE esc; ZeroMemory(&esc, sizeof(esc));
    esc.hAdapter = oa.hAdapter; esc.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    esc.Flags.HardwareAccess = 0; esc.pPrivateDriverData = &priv; esc.PrivateDriverDataSize = sizeof(priv);
    NTSTATUS st = pEsc(&esc);
    D3DKMT_CLOSEADAPTER ca; ca.hAdapter = oa.hAdapter; pClose(&ca);
    if (st != 0 || priv.StatusOut != 0) { LogLine(L"escape failed st=0x%x out=0x%x for %ux%u", st, priv.StatusOut, w, h); return FALSE; }

    BOOL found = FALSE;
    for (int i = 0; i < 25 && !found; i++) {
        DEVMODEW dm;
        for (int m = 0; ; m++) { ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsExW(dd.DeviceName, m, &dm, EDS_RAWMODE)) break;
            if (dm.dmPelsWidth == w && dm.dmPelsHeight == h) { found = TRUE; break; } }
        if (!found) Sleep(80);
    }
    DEVMODEW dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG r = ChangeDisplaySettingsExW(dd.DeviceName, &dm, NULL, CDS_RESET, NULL);
    LogLine(L"applied %ux%u advertised=%d CDS=%d", w, h, found, r);
    if (r == DISP_CHANGE_SUCCESSFUL) { g_lastW = (int)w; g_lastH = (int)h; return TRUE; }
    return FALSE;
}

static BOOL NewestLog(wchar_t* out, size_t n) {
    WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW(L"Q:\\Qubes Logs\\gui-agent-*.log", &fd);
    if (hf == INVALID_HANDLE_VALUE) return FALSE;
    FILETIME best; ZeroMemory(&best, sizeof(best)); wchar_t bestName[128]; bestName[0] = 0;
    do { if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; wcscpy_s(bestName, 128, fd.cFileName); } } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    if (!bestName[0]) return FALSE;
    swprintf_s(out, n, L"Q:\\Qubes Logs\\%s", bestName);
    return TRUE;
}

int wmain(void) {
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    pOpen = (PFN_OPEN)GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName");
    pEsc  = (PFN_ESC) GetProcAddress(g, "D3DKMTEscape");
    pClose= (PFN_CLOSE)GetProcAddress(g, "D3DKMTCloseAdapter");
    if (!pOpen || !pEsc || !pClose) { LogLine(L"D3DKMT missing"); return 1; }
    LogLine(L"qb-resd started");

    wchar_t curLog[260]; curLog[0] = 0; LARGE_INTEGER pos; pos.QuadPart = 0;
    char line[1024]; int linelen = 0;
    ULONG pendW = 0, pendH = 0; DWORD pendTick = 0;

    for (;;) {
        wchar_t newest[260];
        if (NewestLog(newest, 260) && wcscmp(newest, curLog) != 0) { wcscpy_s(curLog, 260, newest); pos.QuadPart = 0; linelen = 0; LogLine(L"following %s", curLog); }
        if (curLog[0]) {
            HANDLE h = CreateFileW(curLog, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER size; GetFileSizeEx(h, &size);
                if (size.QuadPart < pos.QuadPart) pos.QuadPart = 0;          // rotated/truncated
                SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
                char buf[8192]; DWORD rd;
                while (ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd > 0) {
                    for (DWORD i = 0; i < rd; i++) {
                        char c = buf[i];
                        if (c == '\n' || linelen >= 1023) {
                            line[linelen] = 0;
                            char* p = strstr(line, "resolution change: ");
                            if (p) { int w, hh; if (sscanf_s(p + 19, "%dx%d", &w, &hh) == 2 && w > 0 && hh > 0) { pendW = (ULONG)w; pendH = (ULONG)hh; pendTick = GetTickCount(); } }
                            linelen = 0;
                        } else if (c != '\r') { line[linelen++] = c; }
                    }
                    pos.QuadPart += rd;
                }
                CloseHandle(h);
            }
        }
        if (pendTick && (GetTickCount() - pendTick) > 400) { SetExact(pendW, pendH); pendTick = 0; }   // debounce
        Sleep(100);
    }
}
