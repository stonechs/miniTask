/* Diagnostic executable. The only static import library is kernel32. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

typedef BOOL (WINAPI *NotifyIcon)(DWORD, PNOTIFYICONDATAW);
static LRESULT (WINAPI *default_window_proc)(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK probe_window(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    return default_window_proc(window, message, wp, lp);
}

static HICON probe_icon(HMODULE user, HMODULE gdi, DWORD fill)
{
    HBITMAP (WINAPI *make_dib)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD) =
        (void *)GetProcAddress(gdi, "CreateDIBSection");
    HBITMAP (WINAPI *make_bitmap)(int, int, UINT, UINT, const void *) =
        (void *)GetProcAddress(gdi, "CreateBitmap");
    BOOL (WINAPI *delete_object)(HGDIOBJ) = (void *)GetProcAddress(gdi, "DeleteObject");
    HICON (WINAPI *make_icon)(PICONINFO) = (void *)GetProcAddress(user, "CreateIconIndirect");
    static const BITMAPINFO bitmap = {
        .bmiHeader = {
            .biSize = sizeof(BITMAPINFOHEADER), .biWidth = 16, .biHeight = -16,
            .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB
        }
    };
    static const BYTE mask_bits[32];
    if (!make_dib || !make_bitmap || !delete_object || !make_icon)
        return NULL;
    DWORD *pixels;
    HBITMAP color = make_dib(NULL, &bitmap, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    if (!color)
        return NULL;
    for (unsigned i = 0; i < 256; ++i)
        pixels[i] = fill;
    HBITMAP mask = make_bitmap(16, 16, 1, 1, mask_bits);
    HICON icon = NULL;
    if (mask) {
        ICONINFO info = { TRUE, 0, 0, mask, color };
        icon = make_icon(&info);
        delete_object(mask);
    }
    delete_object(color);
    return icon;
}

void WINAPI memoryProbeStart(void)
{
    const WCHAR *end = GetCommandLineW();
    while (*end)
        ++end;
    int stage = end[-1] - L'0';
    if (stage < 0 || stage > 7)
        ExitProcess(1);

    HMODULE user = NULL, gdi = NULL, shell = NULL;
    HWND window = NULL;
    HICON icon = NULL;
    NotifyIcon notify = NULL;
    BOOL added = FALSE;
    static NOTIFYICONDATAW tray;
    if (stage >= 1 && !(user = LoadLibraryW(L"user32.dll")))
        ExitProcess(11);
    if (stage >= 2 && !(gdi = LoadLibraryW(L"gdi32.dll")))
        ExitProcess(12);
    if (stage >= 3 && !(shell = LoadLibraryW(L"shell32.dll")))
        ExitProcess(13);
    if (stage >= 4) {
        UINT (WINAPI *register_message)(LPCWSTR) =
            (void *)GetProcAddress(user, "RegisterWindowMessageW");
        ATOM (WINAPI *register_class)(const WNDCLASSW *) =
            (void *)GetProcAddress(user, "RegisterClassW");
        HWND (WINAPI *create_window)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                                     HWND, HMENU, HINSTANCE, LPVOID) =
            (void *)GetProcAddress(user, "CreateWindowExW");
        default_window_proc = (void *)GetProcAddress(user, "DefWindowProcW");
        HINSTANCE instance = GetModuleHandleW(NULL);
        WNDCLASSW cls = { 0, probe_window, 0, 0, instance, NULL, NULL, NULL, NULL,
                          L"miniTask.MemoryProbe" };
        if (!register_message || !register_class || !create_window || !default_window_proc ||
            !register_message(L"TaskbarCreated") || !register_class(&cls))
            ExitProcess(14);
        window = create_window(WS_EX_TOOLWINDOW, cls.lpszClassName, L"", WS_POPUP,
                               0, 0, 0, 0, NULL, NULL, instance, NULL);
        if (!window)
            ExitProcess(15);
    }
    if (stage >= 5 && !(icon = probe_icon(user, gdi, 0xff20262bu)))
        ExitProcess(16);
    if (stage >= 6) {
        notify = (void *)GetProcAddress(shell, "Shell_NotifyIconW");
        tray.cbSize = sizeof(tray);
        tray.hWnd = window;
        tray.uID = 1;
        tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        tray.uCallbackMessage = WM_APP + 1;
        tray.hIcon = icon;
        lstrcpyW(tray.szTip, L"miniTask memory probe");
        if (!notify || !notify(NIM_ADD, &tray))
            ExitProcess(17);
        added = TRUE;
        tray.uVersion = NOTIFYICON_VERSION_4;
        if (!notify(NIM_SETVERSION, &tray))
            ExitProcess(18);
    }
    if (stage >= 7) {
        HICON next = probe_icon(user, gdi, 0xff65df87u);
        if (!next)
            ExitProcess(19);
        tray.hIcon = next;
        if (!notify(NIM_MODIFY, &tray))
            ExitProcess(20);
        BOOL (WINAPI *destroy_icon)(HICON) = (void *)GetProcAddress(user, "DestroyIcon");
        if (destroy_icon)
            destroy_icon(icon);
    }
    Sleep(2000);
    if (added)
        notify(NIM_DELETE, &tray);
    ExitProcess(0);
}
