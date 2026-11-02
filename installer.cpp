#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <algorithm>
#include <memory>
using std::min;   // GDI+ uses min/max without std::
using std::max;
#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <filesystem>
#include <fstream>
#include <functional>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- constants
static const wchar_t* APP_NAME  = L"OnyxShade v1.0.4.0";
static const wchar_t* ZIP_URL   = L"https://github.com/onyxshades/onyxshadeinstallers/releases/download/v0.0.1/shade.zip";

static const int WIN_W = 670, WIN_H = 380, LEFT_W = 280;

enum : int { ID_ACTION = 101, ID_CLOSE = 102, ID_UNINSTALL = 103 };
enum : UINT {
    WM_APP_PROGRESS  = WM_APP + 1,  // wParam = %, lParam = new std::wstring*
    WM_APP_ERROR     = WM_APP + 2,  // lParam = new std::wstring*
    WM_APP_DONE      = WM_APP + 3,
    WM_APP_CANCELLED = WM_APP + 4,
};

static const COLORREF COLOR_BG         = RGB(0xED, 0xEE, 0xF0);
static const COLORREF COLOR_RIGHT_BG   = RGB(0xFF, 0xFF, 0xFF);
static const COLORREF COLOR_LEFT_EMPTY = RGB(0x35, 0x35, 0x38);
static const COLORREF COLOR_TEXT_DARK  = RGB(0x11, 0x11, 0x11);
static const COLORREF COLOR_TEXT_MUTED = RGB(0x77, 0x77, 0x77);

// ---------------------------------------------------------------- global state
static HWND hActionBtn, hUninstallBtn, hStatusLabel, hProgressBar, hCloseBtn;
static HFONT hFontTitle, hFontText, hFontSmall;
static HBRUSH hBrushRight;

static std::atomic<bool> g_Cancelled(false);
static std::atomic<bool> g_Installing(false);
static fs::path g_DestPath;
static Bitmap* g_PreviewImage = nullptr;
static ULONG_PTR g_GdiplusToken = 0;

// ---------------------------------------------------------------- utilities
static fs::path GetTargetDirectory() {
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path)))
        return fs::path(path) / L"Onyxia" / L"2020";
    return fs::path(L"C:\\Onyxia\\2020");
}

static fs::path GetAppDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return fs::path(path).parent_path();
}

struct InetHandle {
    HINTERNET h;
    explicit InetHandle(HINTERNET handle = nullptr) : h(handle) {}
    ~InetHandle() { if (h) InternetCloseHandle(h); }
    InetHandle(const InetHandle&) = delete;
    InetHandle& operator=(const InetHandle&) = delete;
    operator bool() const { return h != nullptr; }
};

using ProgressFn = std::function<void(float, const std::wstring&)>;

static bool HasFreeSpace(const fs::path& dir, unsigned long long needed) {
    ULARGE_INTEGER avail = {};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &avail, NULL, NULL)) return true;
    return avail.QuadPart >= needed;
}

static bool IsValidZip(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    char sig[4] = {};
    in.read(sig, 4);
    return in.gcount() == 4 && sig[0] == 'P' && sig[1] == 'K' && sig[2] == 3 && sig[3] == 4;
}

static bool IsInstalled() {
    std::error_code ec;
    return fs::is_directory(g_DestPath, ec) && !fs::is_empty(g_DestPath, ec);
}

// ---------------------------------------------------------------- download
static bool DownloadFile(const std::wstring& url, const fs::path& destFile, const ProgressFn& onProgress, std::wstring& error, bool& retryable) {
    retryable = true;
    InetHandle session(InternetOpenW(L"OnyxShadeInstaller/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0));
    if (!session) { error = L"Failed to initialize networking."; return false; }

    DWORD timeout = 15000;
    InternetSetOptionW(session.h, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(session.h, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    InetHandle conn(InternetOpenUrlW(session.h, url.c_str(), NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0));
    if (!conn) { error = L"Failed to connect to the repository."; return false; }

    DWORD status = 0, size = sizeof(status);
    if (HttpQueryInfoW(conn.h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL) && status != 200) {
        error = L"Server returned HTTP error " + std::to_wstring(status) + L".";
        retryable = (status >= 500);
        return false;
    }

    DWORD totalBytes = 0;
    size = sizeof(totalBytes);
    if (!HttpQueryInfoW(conn.h, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &totalBytes, &size, NULL))
        totalBytes = 0;

    unsigned long long needed = totalBytes ? (unsigned long long)totalBytes * 3ull : 100ull * 1024 * 1024;
    if (!HasFreeSpace(g_DestPath.parent_path(), needed)) {
        error = L"Not enough free disk space to install.";
        retryable = false;
        return false;
    }

    std::ofstream out(destFile, std::ios::binary | std::ios::trunc);
    if (!out) { error = L"Failed to create temporary file."; retryable = false; return false; }

    std::vector<char> buffer(64 * 1024);
    DWORD bytesRead = 0;
    unsigned long long downloaded = 0;
    bool ok = true;

    while (true) {
        if (g_Cancelled) { ok = false; break; }
        if (!InternetReadFile(conn.h, buffer.data(), (DWORD)buffer.size(), &bytesRead)) {
            error = L"Connection lost during download.";
            ok = false;
            break;
        }
        if (bytesRead == 0) break;

        out.write(buffer.data(), bytesRead);
        if (!out) { error = L"Disk write error."; ok = false; break; }
        downloaded += bytesRead;

        float frac = totalBytes ? (float)downloaded / totalBytes : 0.5f;
        wchar_t text[128];
        swprintf_s(text, L"Downloading... %.1f MB", downloaded / (1024.0 * 1024.0));
        onProgress(0.15f + 0.60f * frac, text);
    }

    out.close();
    if (ok && downloaded == 0) { error = L"Received an empty file."; ok = false; }
    if (ok && totalBytes && downloaded != totalBytes) { error = L"Download is incomplete."; ok = false; }
    if (!ok) { std::error_code ec; fs::remove(destFile, ec); }
    return ok;
}

// ---------------------------------------------------------------- extraction
static bool RunAndWait(std::wstring cmd, DWORD timeoutMs, DWORD& exitCode) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return false;

    DWORD waited = 0;
    bool finished = false;
    while (waited < timeoutMs) {
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) { finished = true; break; }
        if (g_Cancelled) break;
        waited += 200;
    }
    if (!finished) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    Close
