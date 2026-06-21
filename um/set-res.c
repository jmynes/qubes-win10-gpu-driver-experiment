// QUBES pixel-exact resolution test tool: set-res.exe <width> <height>
// Sends the KMDOD DRIVERPRIVATE escape SetPreferredMode(w,h), waits for the mode
// to be advertised after the synthetic hotplug, then ChangeDisplaySettings(w,h)
// pixel-exact. No Qubes deps — only gdi32 D3DKMT (GetProcAddress'd for robustness).
#include <windows.h>
#include <stdio.h>
#include <d3dkmthk.h>
#include "qb_escape.h"

typedef NTSTATUS (APIENTRY *PFN_OPEN)(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME*);
typedef NTSTATUS (APIENTRY *PFN_ESC)(const D3DKMT_ESCAPE*);
typedef NTSTATUS (APIENTRY *PFN_CLOSE)(const D3DKMT_CLOSEADAPTER*);

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { wprintf(L"usage: set-res <width> <height>\n"); return 1; }
    ULONG w = (ULONG)_wtoi(argv[1]), h = (ULONG)_wtoi(argv[2]);

    DISPLAY_DEVICEW dd; ZeroMemory(&dd, sizeof(dd)); dd.cb = sizeof(dd);
    if (!EnumDisplayDevicesW(NULL, 0, &dd, 0)) { wprintf(L"EnumDisplayDevices failed\n"); return 1; }
    wprintf(L"adapter: %s (%s)\n", dd.DeviceName, dd.DeviceString);

    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    PFN_OPEN  pOpen  = (PFN_OPEN)  GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName");
    PFN_ESC   pEsc   = (PFN_ESC)   GetProcAddress(g, "D3DKMTEscape");
    PFN_CLOSE pClose = (PFN_CLOSE) GetProcAddress(g, "D3DKMTCloseAdapter");
    if (!pOpen || !pEsc || !pClose) { wprintf(L"D3DKMT entry points missing\n"); return 1; }

    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME oa; ZeroMemory(&oa, sizeof(oa));
    wcscpy_s(oa.DeviceName, ARRAYSIZE(oa.DeviceName), dd.DeviceName);
    NTSTATUS st = pOpen(&oa);
    if (st != 0) { wprintf(L"OpenAdapter failed 0x%08x\n", (unsigned)st); return 1; }

    QB_SETPREFERREDMODE_ESCAPE priv; ZeroMemory(&priv, sizeof(priv));
    priv.Magic = QB_ESCAPE_MAGIC; priv.Version = QB_ESCAPE_VERSION;
    priv.Op = QB_ESC_SET_PREFERRED_MODE; priv.Width = w; priv.Height = h;

    D3DKMT_ESCAPE esc; ZeroMemory(&esc, sizeof(esc));
    esc.hAdapter = oa.hAdapter;
    esc.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    esc.Flags.HardwareAccess = 0;             // MUST be 0 under virtualization
    esc.pPrivateDriverData = &priv;
    esc.PrivateDriverDataSize = sizeof(priv);
    st = pEsc(&esc);
    wprintf(L"escape=0x%08x statusOut=0x%08x\n", (unsigned)st, (unsigned)priv.StatusOut);

    D3DKMT_CLOSEADAPTER ca; ca.hAdapter = oa.hAdapter; pClose(&ca);

    // HPD re-enumeration is async on the emulated stack: poll until WxH is advertised.
    BOOL found = FALSE;
    for (int i = 0; i < 25 && !found; i++) {
        DEVMODEW dm;
        for (int m = 0; ; m++) {
            ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsExW(dd.DeviceName, m, &dm, EDS_RAWMODE)) break;
            if (dm.dmPelsWidth == w && dm.dmPelsHeight == h) { found = TRUE; break; }
        }
        if (!found) Sleep(100);
    }
    wprintf(L"mode %ux%u advertised: %s\n", w, h, found ? L"YES" : L"NO");

    DEVMODEW dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG r = ChangeDisplaySettingsExW(dd.DeviceName, &dm, NULL, CDS_RESET, NULL);
    wprintf(L"ChangeDisplaySettings(%ux%u) = %d  (0=ok)\n", w, h, r);
    return 0;
}
