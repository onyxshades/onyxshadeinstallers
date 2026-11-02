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

// True if the drive that holds `dir` has at least `needed` bytes free.
static bool HasFreeSpace(const fs::path& dir, unsigned long long needed) {
    ULARGE_INTEGER avail = {};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &avail, NULL, NULL)) return true; // cannot tell, do not block
    return avail.QuadPart >= needed;
}

// Quick sanity check: a ZIP file starts with "PK\x03\x04".
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
        retryable = (status >= 500); // 4xx will not fix itself
        return false;
    }

    DWORD totalBytes = 0;
    size = sizeof(totalBytes);
    if (!HttpQueryInfoW(conn.h, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &totalBytes, &size, NULL))
        totalBytes = 0;

    // archive + extracted copy + staging copy: ask for ~3x the archive size
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
// Run a process and wait for it (cancellable).
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
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished;
}

// Primary method: built-in tar.exe (Windows 10 1803+). Synchronous and reliable.
static bool ExtractWithTar(const fs::path& zip, const fs::path& dest) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    fs::path tar = fs::path(sys) / L"tar.exe";
    if (!fs::exists(tar)) return false;

    std::wstring cmd = L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    DWORD code = 1;
    return RunAndWait(cmd, 120000, code) && code == 0;
}

// Fallback: Shell. CopyHere is asynchronous, so wait for the files to appear.
static bool ExtractWithShell(const fs::path& zip, const fs::path& dest) {
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    IShellDispatch* shell = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_Shell, NULL, CLSCTX_INPROC_SERVER, IID_IShellDispatch, (void**)&shell);
    bool success = false;

    if (SUCCEEDED(hr) && shell) {
        VARIANT vZip, vDest, vOpts;
        VariantInit(&vZip); VariantInit(&vDest); VariantInit(&vOpts);
        vZip.vt = VT_BSTR;  vZip.bstrVal  = SysAllocString(zip.c_str());
        vDest.vt = VT_BSTR; vDest.bstrVal = SysAllocString(dest.c_str());
        vOpts.vt = VT_I4;   vOpts.lVal = 4 | 16;   // no UI, "Yes to all"

        Folder *zipFolder = nullptr, *destFolder = nullptr;
        shell->NameSpace(vZip, &zipFolder);
        shell->NameSpace(vDest, &destFolder);

        if (zipFolder && destFolder) {
            FolderItems* items = nullptr;
            zipFolder->Items(&items);
            if (items) {
                std::vector<std::wstring> names;
                long count = 0;
                items->get_Count(&count);
                for (long i = 0; i < count; i++) {
                    VARIANT vi; VariantInit(&vi); vi.vt = VT_I4; vi.lVal = i;
                    FolderItem* it = nullptr;
                    if (SUCCEEDED(items->Item(vi, &it)) && it) {
                        BSTR n = nullptr;
                        if (SUCCEEDED(it->get_Name(&n)) && n) { names.push_back(n); SysFreeString(n); }
                        it->Release();
                    }
                }

                VARIANT vItems; VariantInit(&vItems);
                vItems.vt = VT_DISPATCH; vItems.pdispVal = items;
                success = SUCCEEDED(destFolder->CopyHere(vItems, vOpts));

                // wait until everything appears on disk (up to 2 minutes)
                for (int t = 0; success && t < 600 && !g_Cancelled; t++) {
                    bool all = true;
                    for (auto& n : names)
                        if (!fs::exists(dest / n)) { all = false; break; }
                    if (all) break;
                    Sleep(200);
                }
                if (success) Sleep(1500); // let the last files finish writing
                items->Release();
            }
        }
        if (zipFolder) zipFolder->Release();
        if (destFolder) destFolder->Release();
        VariantClear(&vZip);
        VariantClear(&vDest);
        shell->Release();
    }

    if (SUCCEEDED(hrInit)) CoUninitialize();
    return success && !g_Cancelled;
}

static bool ExtractZip(const fs::path& zip, const fs::path& dest) {
    return ExtractWithTar(zip, dest) || (!g_Cancelled && ExtractWithShell(zip, dest));
}

// ---------------------------------------------------------------- installation thread
static void Post(HWND hWnd, UINT msg, WPARAM w, const std::wstring* text) {
    std::wstring* copy = text ? new std::wstring(*text) : nullptr;
    if (!PostMessageW(hWnd, msg, w, (LPARAM)copy)) delete copy; // window already closed
}

// Sleep that wakes up early when the user cancels.
static void SleepCancellable(int ms) {
    for (int t = 0; t < ms && !g_Cancelled; t += 100) Sleep(100);
}

// Рекурсивное слияние: добавляет и обновляет файлы из архива, не удаляя сторонние файлы в целевой папке
static bool MergeInstall(const fs::path& staging, const fs::path& dest, std::wstring& error) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        error = L"Failed to create target directory.";
        return false;
    }

    for (auto it = fs::recursive_directory_iterator(staging, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& srcPath = it->path();
        auto relPath = fs::relative(srcPath, staging);
        auto targetPath = dest / relPath;

        if (it->is_directory(ec)) {
            fs::create_directories(targetPath, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(targetPath.parent_path(), ec);
            fs::copy_file(srcPath, targetPath, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = L"Failed to write file (it may be in use): " + targetPath.filename().wstring();
                return false;
            }
        }
    }
    return true;
}

static void InstallationThread(HWND hWnd) {
    auto progress  = [hWnd](float p, const std::wstring& s) { Post(hWnd, WM_APP_PROGRESS, (WPARAM)(p * 100), &s); };
    auto fail      = [hWnd](const std::wstring& s)         { Post(hWnd, WM_APP_ERROR, 0, &s); };
    auto cancelled = [hWnd]()                               { Post(hWnd, WM_APP_CANCELLED, 0, nullptr); };

    fs::path tempZip = fs::temp_directory_path() / L"onyxshade_shade.zip";
    fs::path staging = g_DestPath;
    staging += L".new";
    std::error_code ec;
    auto cleanup = [&]() { fs::remove(tempZip, ec); fs::remove_all(staging, ec); };

    try {
        progress(0.05f, L"Preparing...");
        fs::create_directories(g_DestPath.parent_path());

        // download with up to 3 attempts
        const int MAX_ATTEMPTS = 3;
        std::wstring error;
        bool downloaded = false;
        for (int attempt = 1; attempt <= MAX_ATTEMPTS && !g_Cancelled; attempt++) {
            std::wstring msg = (attempt == 1)
                ? L"Connecting to repository..."
                : L"Retrying download (attempt " + std::to_wstring(attempt) + L"/" + std::to_wstring(MAX_ATTEMPTS) + L")...";
            progress(0.15f, msg);

            bool retryable = true;
            if (DownloadFile(ZIP_URL, tempZip, progress, error, retryable)) { downloaded = true; break; }
            if (g_Cancelled || !retryable || attempt == MAX_ATTEMPTS) break;
            SleepCancellable(2000);
        }
        if (g_Cancelled) { cleanup(); cancelled(); return; }
        if (!downloaded) { cleanup(); fail(error); return; }

        progress(0.78f, L"Verifying archive...");
        if (!IsValidZip(tempZip)) { cleanup(); fail(L"The downloaded file is not a valid ZIP archive."); return; }

        // extract into a staging folder first
        progress(0.80f, L"Extracting shaders...");
        fs::remove_all(staging, ec);
        fs::create_directories(staging);
        bool ok = ExtractZip(tempZip, staging);
        fs::remove(tempZip, ec);

        if (g_Cancelled) { cleanup(); cancelled(); return; }
        if (!ok || fs::is_empty(staging, ec)) { cleanup(); fail(L"Failed to extract files."); return; }

        // If the archive wraps everything in a single top-level folder, use that folder's contents
        fs::path root = staging;
        {
            std::vector<fs::path> entries;
            for (fs::directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->path().filename() == L"__MACOSX") continue; // archive junk
                entries.push_back(it->path());
            }
            std::error_code ec2;
            if (entries.size() == 1 && fs::is_directory(entries[0], ec2)) root = entries[0];
        }

        progress(0.95f, L"Installing...");
        if (!MergeInstall(root, g_DestPath, error)) { cleanup(); fail(error); return; }
        fs::remove_all(staging, ec); // leftover (now empty) staging folder

        progress(1.0f, L"Installation completed successfully!");
        Post(hWnd, WM_APP_DONE, 0, nullptr);
    }
    catch (const std::exception&) {
        cleanup();
        fail(L"File system error. Check access permissions.");
    }
    catch (...) {
        cleanup();
        fail(L"An unexpected error occurred.");
    }
}

// ---------------------------------------------------------------- drawing
static void AddRoundRect(GraphicsPath& p, float x, float y, float w, float h, float r) {
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
}

static void DrawButton(const DRAWITEMSTRUCT* dis) {
    Graphics g(dis->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    const RECT& rc = dis->rcItem;
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool primary  = (dis->CtlID == ID_ACTION);

    // clear background under rounded corners
    SolidBrush clear(Color(255, GetRValue(COLOR_RIGHT_BG), GetGValue(COLOR_RIGHT_BG), GetBValue(COLOR_RIGHT_BG)));
    g.FillRectangle(&clear, (REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));

    Color bg, fg;
    if (primary) {
        bg = disabled ? Color(120, 120, 120) : (pressed ? Color(255, 60, 60, 60) : Color(255, 18, 18, 18));
        fg = Color(255, 255, 255, 255);
    } else {
        bg = pressed ? Color(255, 220, 220, 220) : Color(255, 245, 245, 245);
        fg = Color(255, 60, 60, 60);
    }

    GraphicsPath path;
    AddRoundRect(path, (REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left - 1), (REAL)(rc.bottom - rc.top - 1), primary ? 8.0f : 6.0f);
    SolidBrush bgBrush(bg);
    g.FillPath(&bgBrush, &path);

    wchar_t label[64] = {};
    GetWindowTextW(dis->hwndItem, label, 64);

    FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, primary ? 15.0f : 14.0f, primary ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush text(fg);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    RectF r((REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
    g.DrawString(label, -1, &font, r, &fmt, &text);
}

static void PaintWindow(HWND hWnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc;
    GetClientRect(hWnd, &rc);

    // double buffering to avoid flicker
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(COLOR_BG);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    RECT left = { 0, 0, LEFT_W, rc.bottom };
    if (g_PreviewImage && g_PreviewImage->GetLastStatus() == Ok) {
        Graphics g(mem);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(g_PreviewImage, 0, 0, LEFT_W, (INT)rc.bottom);
    } else {
        HBRUSH b = CreateSolidBrush(COLOR_LEFT_EMPTY);
        FillRect(mem, &left, b);
        DeleteObject(b);
    }

    RECT right = { LEFT_W, 0, rc.right, rc.bottom };
    FillRect(mem, &right, hBrushRight);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hWnd, &ps);
}

// ---------------------------------------------------------------- UI helpers
static HFONT MakeFont(int h, int weight) {
    return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static HWND MakeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font) {
    HWND l = CreateWindowW(L"STATIC", text, WS_VISIBLE | WS_CHILD | SS_LEFT, x, y, w, h, parent, NULL, NULL, NULL);
    SendMessageW(l, WM_SETFONT, (WPARAM)font, TRUE);
    return l;
}

// Button captions depend on whether shaders are already installed.
static void RefreshButtons() {
    bool installed = IsInstalled();
    SetWindowTextW(hActionBtn, installed ? L"Reinstall / Update" : L"Install");
    EnableWindow(hActionBtn, TRUE);
    ShowWindow(hUninstallBtn, installed ? SW_SHOW : SW_HIDE);
    InvalidateRect(hActionBtn, NULL, TRUE);
}

static void ResetUi(const wchar_t* status) {
    g_Installing = false;
    RefreshButtons();
    SetWindowTextW(hStatusLabel, status);
    SendMessageW(hProgressBar, PBM_SETPOS, 0, 0);
    ShowWindow(hProgressBar, SW_HIDE);
}

// ---------------------------------------------------------------- window procedure
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_DestPath = GetTargetDirectory();
        SetWindowRgn(hWnd, CreateRoundRectRgn(0, 0, WIN_W + 1, WIN_H + 1, 16, 16), TRUE);

        hBrushRight = CreateSolidBrush(COLOR_RIGHT_BG);
        hFontTitle  = MakeFont(30, FW_BOLD);
