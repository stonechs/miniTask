#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

enum {
    SAMPLE_TIMER = 1,
    SAMPLE_MS = 500,
    TRAY_EVENT = WM_APP + 1,
    EXIT_COMMAND = 1,
    HISTORY_COMMAND,
    STARTUP_COMMAND,
    RANK_COMMAND,
    LOG_COMMAND,
    LOG_FOLDER_COMMAND,
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
static BOOL rank_enabled, log_enabled;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static char *log_buffer;
static DWORD log_length;
static ULONGLONG last_log_flush;
static WCHAR log_path[MAX_PATH];
static unsigned peak_percent;
static SYSTEMTIME peak_time;
static WCHAR peak_name[64];
static WCHAR top_name[64];
static unsigned top_percent;

typedef struct {
    DWORD pid;
    ULONGLONG created, cpu;
} ProcessTime;
static ProcessTime *process_times[2];
static unsigned process_counts[2], process_slot;

static BOOL set_rank(BOOL enabled)
{
    if (enabled && !process_times[0]) {
        void *memory = VirtualAlloc(NULL, 2 * 512 * sizeof(ProcessTime),
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!memory)
            return FALSE;
        process_times[0] = memory;
        process_times[1] = process_times[0] + 512;
    }
    if (!enabled && process_times[0]) {
        VirtualFree(process_times[0], 0, MEM_RELEASE);
        process_times[0] = process_times[1] = NULL;
    }
    process_counts[0] = process_counts[1] = 0;
    rank_enabled = enabled;
    return TRUE;
}

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

static void flush_log(void)
{
    if (log_file == INVALID_HANDLE_VALUE || !log_buffer || !log_length)
        return;
    DWORD written;
    if (WriteFile(log_file, log_buffer, log_length, &written, NULL) &&
        written == log_length) {
        log_length = 0;
        last_log_flush = GetTickCount64();
        LARGE_INTEGER size;
        if (GetFileSizeEx(log_file, &size) && size.QuadPart >= 2 * 1024 * 1024) {
            CloseHandle(log_file);
            log_file = INVALID_HANDLE_VALUE;
            WCHAR old_path[MAX_PATH];
            lstrcpyW(old_path, log_path);
            lstrcatW(old_path, L".old");
            BOOL moved = MoveFileExW(log_path, old_path, MOVEFILE_REPLACE_EXISTING);
            log_file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ,
                                   NULL, moved ? CREATE_ALWAYS : OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (moved && log_file != INVALID_HANDLE_VALUE) {
                static const char header[] =
                    "time,cpu_percent,top_process,top_percent,peak_percent,peak_time,peak_process\r\n";
                WriteFile(log_file, header, sizeof(header) - 1, &written, NULL);
            }
        }
    }
}

static void close_log(void)
{
    flush_log();
    if (log_file != INVALID_HANDLE_VALUE)
        CloseHandle(log_file);
    log_file = INVALID_HANDLE_VALUE;
    if (log_buffer) {
        VirtualFree(log_buffer, 0, MEM_RELEASE);
        log_buffer = NULL;
    }
}

static BOOL open_log(void)
{
    if (log_file != INVALID_HANDLE_VALUE)
        return TRUE;
    if (!log_buffer)
        log_buffer = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!log_buffer)
        return FALSE;
    WCHAR folder[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
    if (!length || length >= MAX_PATH || length + 22 >= MAX_PATH) {
        close_log();
        return FALSE;
    }
    lstrcatW(folder, L"\\miniTask");
    CreateDirectoryW(folder, NULL);
    lstrcpyW(log_path, folder);
    lstrcatW(log_path, L"\\cpu-v2.csv");
    log_file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) {
        close_log();
        return FALSE;
    }
    LARGE_INTEGER size;
    if (GetFileSizeEx(log_file, &size) && size.QuadPart == 0) {
        static const char header[] =
            "time,cpu_percent,top_process,top_percent,peak_percent,peak_time,peak_process\r\n";
        DWORD written;
        WriteFile(log_file, header, sizeof(header) - 1, &written, NULL);
    }
    return TRUE;
}

static void append_log(unsigned percent)
{
    if (!log_enabled || !open_log())
        return;
    SYSTEMTIME time;
    GetLocalTime(&time);
    WCHAR line[320];
    int chars = wsprintfW(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u,%u,%s,%u,%u,%04u-%02u-%02u %02u:%02u:%02u,%s\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, percent, top_name, top_percent,
        peak_percent, peak_time.wYear, peak_time.wMonth, peak_time.wDay,
        peak_time.wHour, peak_time.wMinute, peak_time.wSecond, peak_name);
    char encoded[640];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, chars, encoded,
                                    sizeof(encoded), NULL, NULL);
    if (bytes <= 0 || bytes > 4096)
        return;
    if (log_length + bytes > 4096)
        flush_log();
    if (log_length + bytes <= 4096) {
        for (int i = 0; i < bytes; ++i)
            log_buffer[log_length + i] = encoded[i];
        log_length += bytes;
    }
    if (GetTickCount64() - last_log_flush >= 2000)
        flush_log();
}

static void sample_processes(ULONGLONG system_elapsed)
{
    top_name[0] = 0;
    top_percent = 0;
    if (!rank_enabled || !process_times[0])
        return;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    unsigned next = process_slot ^ 1;
    process_counts[next] = 0;
    ULONGLONG highest = 0;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!entry.th32ProcessID || process_counts[next] >= 512)
                continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ProcessID);
            if (!process)
                continue;
            FILETIME created, exited, kernel, user;
            BOOL read = GetProcessTimes(process, &created, &exited, &kernel, &user);
            CloseHandle(process);
            if (!read)
                continue;
            ProcessTime *current = &process_times[next][process_counts[next]++];
            current->pid = entry.th32ProcessID;
            current->created = file_time(created);
            current->cpu = file_time(kernel) + file_time(user);
            for (unsigned i = 0; i < process_counts[process_slot]; ++i) {
                ProcessTime *previous = &process_times[process_slot][i];
                if (previous->pid == current->pid &&
                    previous->created == current->created &&
                    current->cpu >= previous->cpu) {
                    ULONGLONG delta = current->cpu - previous->cpu;
                    if (delta > highest) {
                        highest = delta;
                        lstrcpynW(top_name, entry.szExeFile, 64);
                    }
                    break;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    process_slot = next;
    if (system_elapsed && highest)
        top_percent = (unsigned)((highest * 100 + system_elapsed / 2) /
                                 system_elapsed);
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
    sample_processes(elapsed);
    if (percent > peak_percent) {
        peak_percent = percent;
        GetLocalTime(&peak_time);
        lstrcpynW(peak_name, top_name, 64);
    }
    append_log(percent);
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
    AppendMenuW(menu, MF_STRING | (rank_enabled ? MF_CHECKED : 0),
                RANK_COMMAND, L"\x8bb0\x5f55\x6700\x9ad8\x8d1f\x8377\x8fdb\x7a0b");
    AppendMenuW(menu, MF_STRING | (log_enabled ? MF_CHECKED : 0),
                LOG_COMMAND, L"\x8bb0\x5f55\x65e5\x5fd7");
    AppendMenuW(menu, MF_STRING, LOG_FOLDER_COMMAND,
                L"\x6253\x5f00\x65e5\x5fd7\x76ee\x5f55");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (!AppendMenuW(menu, MF_STRING, EXIT_COMMAND, L"\x9000\x51fa")) {
        DestroyMenu(menu);
        return;
    }

    SetForegroundWindow(window);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY |
                                  TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                                  position.x, position.y, 0, window, NULL);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (command)
        SendMessageW(window, WM_COMMAND, command, 0);
}

static void execute_command(HWND window, UINT command)
{
    if (command == EXIT_COMMAND)
        DestroyWindow(window);
    else if (command == HISTORY_COMMAND)
        toggle_history();
    else if (command == STARTUP_COMMAND) {
        BOOL next = !startup_enabled;
        if (set_startup(next) && write_setting(L"Startup", next))
            startup_enabled = next;
    } else if (command == RANK_COMMAND) {
        if (set_rank(!rank_enabled))
            write_setting(L"Rank", rank_enabled);
    } else if (command == LOG_COMMAND) {
        if (log_enabled) {
            log_enabled = FALSE;
            close_log();
            write_setting(L"Log", 0);
        } else if (open_log()) {
            log_enabled = TRUE;
            write_setting(L"Log", 1);
        }
    } else if (command == LOG_FOLDER_COMMAND) {
        WCHAR folder[MAX_PATH];
        DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
        if (length && length < MAX_PATH && length + 10 < MAX_PATH) {
            lstrcatW(folder, L"\\miniTask");
            CreateDirectoryW(folder, NULL);
            ShellExecuteW(window, L"open", folder, NULL, NULL, SW_SHOWNORMAL);
        }
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
    case WM_COMMAND:
        execute_command(window, LOWORD(wparam));
        return 0;
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
            POINT position;
            if (GetCursorPos(&position))
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
        close_log();
        set_rank(FALSE);
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
    DWORD setting;
    set_rank(read_setting(L"Rank", &setting) && setting != 0);
    if (!read_setting(L"LogDefaultV03", &setting)) {
        write_setting(L"Log", 1);
        write_setting(L"LogDefaultV03", 1);
    }
    log_enabled = read_setting(L"Log", &setting) && setting != 0 && open_log();
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
