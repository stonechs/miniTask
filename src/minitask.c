#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <shellapi.h>

enum {
    SAMPLE_TIMER = 1,
    SAMPLE_MS = 500,
    TRAY_EVENT = WM_APP + 1,
    EXIT_COMMAND = 1,
    HISTORY_COMMAND,
    STARTUP_COMMAND,
    HISTORY_TIMER = 2,
    ICON_SIZE = 16
};

enum { HISTORY_CAPACITY = 128, HISTORY_SECONDS = 60 };

typedef struct {
    ULONGLONG tick;
    unsigned value;
    BOOL valid;
} Sample;

static const WCHAR class_name[] = L"miniTask.Tray";
static NOTIFYICONDATAW tray;
static HICON icon;
static BOOL tray_added;
static UINT taskbar_created;
static unsigned shown_level = 11;
static ULONGLONG previous_total, previous_idle;
static BOOL has_baseline;
static HWND history_window;
static Sample history[HISTORY_CAPACITY];
static unsigned history_head, history_count;
static unsigned low_load_count;
static DWORD sample_period = SAMPLE_MS;
static BOOL startup_enabled = TRUE;
static const WCHAR history_class[] = L"miniTask.History";
static const WCHAR startup_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR startup_value[] = L"miniTask";
static const WCHAR settings_key[] = L"Software\\miniTask";

static BOOL read_setting(const WCHAR *name, DWORD *value)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, settings_key, 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS)
        return FALSE;
    DWORD type, size = sizeof(*value);
    LONG result = RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(*value);
}

static BOOL write_setting(const WCHAR *name, DWORD value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, settings_key, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return FALSE;
    LONG result = RegSetValueExW(key, name, 0, REG_DWORD,
                                 (const BYTE *)&value, sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

#ifdef MINITASK_DIAGNOSTICS
/* Read-only messages used by the external measurement script. */
static ULONGLONG last_tick, sample_count;
static DWORD interval_ms;
static unsigned cpu_percent;
#endif

static ULONGLONG file_time(FILETIME value)
{
    return ((ULONGLONG)value.dwHighDateTime << 32) | value.dwLowDateTime;
}

static HICON make_icon(unsigned level)
{
    static const BITMAPINFO bitmap_info = {
        .bmiHeader = {
            .biSize = sizeof(BITMAPINFOHEADER),
            .biWidth = ICON_SIZE,
            .biHeight = -ICON_SIZE,
            .biPlanes = 1,
            .biBitCount = 32,
            .biCompression = BI_RGB
        }
    };
    static const BYTE opaque_mask[ICON_SIZE * 2];
    DWORD *pixels;
    HBITMAP color = CreateDIBSection(NULL, &bitmap_info, DIB_RGB_COLORS,
                                    (void **)&pixels, NULL, 0);
    if (!color)
        return NULL;

    for (unsigned y = 0; y < ICON_SIZE; ++y) {
        for (unsigned x = 0; x < ICON_SIZE; ++x) {
            DWORD fill = 0xff20262bu;
            if (x >= 4 && x < 12 && y >= 3 && y < 13)
                fill = (y >= 13 - level) ? 0xff65df87u : 0xff48535bu;
            pixels[y * ICON_SIZE + x] = fill;
        }
    }

    HBITMAP mask = CreateBitmap(ICON_SIZE, ICON_SIZE, 1, 1, opaque_mask);
    HICON result = NULL;
    if (mask) {
        ICONINFO info = { TRUE, 0, 0, mask, color };
        result = CreateIconIndirect(&info);
        DeleteObject(mask);
    }
    DeleteObject(color);
    return result;
}

static void update_tray(unsigned level)
{
    if (tray_added && level == shown_level)
        return;

    HICON next = icon;
    if (!icon || level != shown_level) {
        next = make_icon(level);
        if (!next)
            return;
    }

    tray.hIcon = next;
    if (!Shell_NotifyIconW(tray_added ? NIM_MODIFY : NIM_ADD, &tray)) {
        if (next != icon)
            DestroyIcon(next);
        tray.hIcon = icon;
        tray_added = FALSE;
        return;
    }

    if (!tray_added) {
        tray.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray);
    }
    tray_added = TRUE;
    if (icon && icon != next)
        DestroyIcon(icon);
    icon = next;
    shown_level = level;
}

static void add_history(unsigned percent, BOOL valid)
{
    Sample *sample = &history[history_head];
    sample->tick = GetTickCount64();
    sample->value = percent;
    sample->valid = valid;
    history_head = (history_head + 1) % HISTORY_CAPACITY;
    if (history_count < HISTORY_CAPACITY)
        ++history_count;
    if (history_window)
        InvalidateRect(history_window, NULL, FALSE);
}

static BOOL set_startup(BOOL enabled)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, startup_key, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return FALSE;
    BOOL success;
    if (enabled) {
        WCHAR path[MAX_PATH], command[MAX_PATH + 3];
        DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
        if (!length || length >= MAX_PATH) {
            RegCloseKey(key);
            return FALSE;
        }
        wsprintfW(command, L"\"%s\"", path);
        success = RegSetValueExW(key, startup_value, 0, REG_SZ,
                                 (const BYTE *)command,
                                 (lstrlenW(command) + 1) * sizeof(WCHAR)) == ERROR_SUCCESS;
    } else {
        LONG result = RegDeleteValueW(key, startup_value);
        success = result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    return success;
}

static void load_startup(void)
{
    DWORD value;
    startup_enabled = !read_setting(L"Startup", &value) || value != 0;
    set_startup(startup_enabled);
}

static LRESULT CALLBACK history_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT client;
        GetClientRect(window, &client);
        FillRect(dc, &client, (HBRUSH)(COLOR_WINDOW + 1));
        const int left = 38, top = 12, right = 12, bottom = 26;
        int width = client.right - left - right;
        int height = client.bottom - top - bottom;
        HPEN grid = CreatePen(PS_SOLID, 1, RGB(220, 224, 228));
        HGDIOBJ old_pen = SelectObject(dc, grid);
        for (int line = 0; line <= 4; ++line) {
            int y = top + height * line / 4;
            MoveToEx(dc, left, y, NULL);
            LineTo(dc, client.right - right, y);
            WCHAR label[8];
            wsprintfW(label, L"%d%%", 100 - line * 25);
            TextOutW(dc, 2, y - 7, label, lstrlenW(label));
        }
        DeleteObject(SelectObject(dc, old_pen));
        HPEN trace = CreatePen(PS_SOLID, 2, RGB(32, 135, 76));
        old_pen = SelectObject(dc, trace);
        POINT points[HISTORY_CAPACITY];
        int count = 0;
        ULONGLONG now = GetTickCount64();
        unsigned first = (history_head + HISTORY_CAPACITY - history_count) % HISTORY_CAPACITY;
        for (unsigned i = 0; i < history_count; ++i) {
            Sample *sample = &history[(first + i) % HISTORY_CAPACITY];
            if (!sample->valid || now - sample->tick > HISTORY_SECONDS * 1000ULL) {
                if (count > 1)
                    Polyline(dc, points, count);
                count = 0;
                continue;
            }
            points[count].x = client.right - right -
                (int)((now - sample->tick) * (ULONGLONG)width /
                      (HISTORY_SECONDS * 1000ULL));
            points[count].y = top + height - (int)(sample->value * (ULONGLONG)height / 100);
            ++count;
        }
        if (count > 1)
            Polyline(dc, points, count);
        DeleteObject(SelectObject(dc, old_pen));
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_CLOSE) {
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static void toggle_history(void)
{
    if (history_window) {
        ShowWindow(history_window, IsWindowVisible(history_window) ? SW_HIDE : SW_SHOW);
        if (IsWindowVisible(history_window))
            SetForegroundWindow(history_window);
        return;
    }
    history_window = CreateWindowExW(WS_EX_TOOLWINDOW, history_class,
        L"miniTask - 最近 60 秒 CPU", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 380, 190, NULL, NULL,
        GetModuleHandleW(NULL), NULL);
    if (history_window)
        ShowWindow(history_window, SW_SHOW);
}

static void sample_cpu(void)
{
    FILETIME idle_time, kernel_time, user_time;
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        has_baseline = FALSE;
        return;
    }

    ULONGLONG idle = file_time(idle_time);
    ULONGLONG total = file_time(kernel_time) + file_time(user_time);
    ULONGLONG elapsed = total - previous_total;
    ULONGLONG idle_elapsed = idle - previous_idle;
    BOOL valid = has_baseline && total > previous_total &&
                 idle >= previous_idle && idle_elapsed <= elapsed;
    has_baseline = TRUE;
    previous_total = total;
    previous_idle = idle;
    if (!valid) {
        add_history(0, FALSE);
        return;
    }

    ULONGLONG busy = elapsed - idle_elapsed;
    unsigned percent = (unsigned)((100 * busy + elapsed / 2) / elapsed);
#ifdef MINITASK_DIAGNOSTICS
    ULONGLONG now = GetTickCount64();
    interval_ms = last_tick ? (DWORD)(now - last_tick) : 0;
    last_tick = now;
    cpu_percent = percent;
    ++sample_count;
#endif
    add_history(percent, TRUE);
    update_tray((percent + 9) / 10);

    if (percent < 3) {
        if (low_load_count < 20)
            ++low_load_count;
        if (sample_period == SAMPLE_MS && low_load_count >= 20) {
            sample_period = 2000;
            SetCoalescableTimer(tray.hWnd, SAMPLE_TIMER, sample_period, NULL, 100);
        }
    } else {
        low_load_count = 0;
        if (sample_period != SAMPLE_MS && percent >= 8) {
            sample_period = SAMPLE_MS;
            SetCoalescableTimer(tray.hWnd, SAMPLE_TIMER, sample_period, NULL, 20);
        }
    }
}

static void show_menu(HWND window, POINT position)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING | (history_window && IsWindowVisible(history_window)
                                   ? MF_CHECKED : 0), HISTORY_COMMAND, L"\x5386\x53f2\x6ce2\x5f62");
    AppendMenuW(menu, MF_STRING | (startup_enabled ? MF_CHECKED : 0),
                STARTUP_COMMAND, L"\x767b\x5f55\x65f6\x542f\x52a8");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (!AppendMenuW(menu, MF_STRING, EXIT_COMMAND, L"\x9000\x51fa")) {
        DestroyMenu(menu);
        return;
    }

    SetForegroundWindow(window);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                  position.x, position.y, 0, window, NULL);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (command == EXIT_COMMAND)
        DestroyWindow(window);
    else if (command == HISTORY_COMMAND)
        toggle_history();
    else if (command == STARTUP_COMMAND) {
        BOOL next = !startup_enabled;
        if (set_startup(next) && write_setting(L"Startup", next))
            startup_enabled = next;
    }
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == taskbar_created && taskbar_created) {
        tray_added = FALSE;
        update_tray(icon ? shown_level : 0);
        return 0;
    }
    switch (message) {
    case WM_TIMER:
        if (wparam == SAMPLE_TIMER)
            sample_cpu();
        return 0;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND || wparam == PBT_APMRESUMEAUTOMATIC) {
            has_baseline = FALSE;
#ifdef MINITASK_DIAGNOSTICS
            last_tick = 0;
#endif
        }
        return TRUE;
    case TRAY_EVENT:
        if (LOWORD(lparam) == WM_CONTEXTMENU) {
            POINT position = { (SHORT)LOWORD(wparam), (SHORT)HIWORD(wparam) };
            if (position.x == -1 && position.y == -1 && !GetCursorPos(&position))
                return 0;
            show_menu(window, position);
        } else if (LOWORD(lparam) == WM_RBUTTONUP && HIWORD(lparam) == 0) {
            /* Legacy shell callback when NIM_SETVERSION is unavailable. */
            POINT position;
            if (GetCursorPos(&position))
                show_menu(window, position);
        }
        return 0;
#ifdef MINITASK_DIAGNOSTICS
    case WM_APP + 2:
        return (LRESULT)sample_count;
    case WM_APP + 3:
        return (LRESULT)((interval_ms << 8) | cpu_percent);
    case WM_APP + 4:
        return tray_added;
#endif
    case WM_DESTROY:
        KillTimer(window, SAMPLE_TIMER);
        if (tray_added)
            Shell_NotifyIconW(NIM_DELETE, &tray);
        if (icon)
            DestroyIcon(icon);
        if (history_window)
            DestroyWindow(history_window);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static DWORD run(void)
{
    HANDLE instance_lock = CreateMutexW(NULL, FALSE, L"Local\\miniTask.Tray");
    if (!instance_lock)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instance_lock);
        return 0;
    }

    DWORD result = 1;
    const WCHAR *error = L"Cannot initialize the tray window.";
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW cls = { 0, window_proc, 0, 0, instance, NULL, NULL, NULL, NULL, class_name };
    WNDCLASSW history_cls = { 0, history_proc, 0, 0, instance, NULL, NULL,
                              (HBRUSH)(COLOR_WINDOW + 1), NULL, history_class };

    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        error = L"Cannot set high process priority.";
        goto done;
    }
    if (GetActiveProcessorGroupCount() != 1) {
        error = L"This version supports a single processor group.";
        goto done;
    }

    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (!RegisterClassW(&cls))
        goto done;
    if (!RegisterClassW(&history_cls))
        goto done;
    load_startup();
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"miniTask",
                                  WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!window)
        goto done;

    tray.cbSize = sizeof(tray);
    tray.hWnd = window;
    tray.uID = 1; /* Nonzero to distinguish version 4 callbacks. */
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    tray.uCallbackMessage = TRAY_EVENT;
    lstrcpyW(tray.szTip, L"miniTask - CPU");
    update_tray(0);
    sample_cpu();
    if (!SetCoalescableTimer(window, SAMPLE_TIMER, SAMPLE_MS, NULL, 20)) {
        DestroyWindow(window);
        error = L"Cannot start the CPU sample timer.";
        goto done;
    }

    MSG message;
    BOOL status;
    while ((status = GetMessageW(&message, NULL, 0, 0)) > 0)
        DispatchMessageW(&message);
    if (status < 0)
        DestroyWindow(window);
    error = NULL;
    result = status < 0 ? 1 : (DWORD)message.wParam;

done:
    if (error)
        MessageBoxW(NULL, error, L"miniTask", MB_OK | MB_ICONERROR);
    CloseHandle(instance_lock);
    return result;
}

void WINAPI miniTaskStart(void)
{
    ExitProcess(run());
}
