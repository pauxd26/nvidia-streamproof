#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <dwmapi.h>
#include "shared.h"

#pragma comment(lib, "dwmapi.lib")

// --- Config ---
static constexpr int PREVIEW_W     = 384;
static constexpr int PREVIEW_H     = 216;
static constexpr int MARGIN        = 20;
static constexpr int BORDER        = 2;
static constexpr int TARGET_FPS    = 30;

static constexpr COLORREF COLOR_HIDDEN  = RGB(0, 200, 0);   // Green = invisible to capture
static constexpr COLORREF COLOR_VISIBLE = RGB(200, 0, 0);   // Red   = visible to capture

#define HOTKEY_TOGGLE 1

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#define WDA_NONE 0x00000000

// --- Globals ---
static HDC      g_memDC       = nullptr;
static HBITMAP  g_bitmap      = nullptr;
static int      g_screenW     = 0;
static int      g_screenH     = 0;
static bool     g_running     = true;
static bool     g_hidden      = true;    // starts invisible to capture
static bool     g_dragging    = false;
static POINT    g_dragOffset  = {};

// Shared memory for proxy DLL communication
static HANDLE            g_shmemHandle = nullptr;
static StreamProofData*  g_shared      = nullptr;
static HANDLE            g_evtHide     = nullptr;
static HANDLE            g_evtReady    = nullptr;
static HANDLE            g_evtDone     = nullptr;

// --- Screen capture via GDI ---
static void CaptureScreen(HDC hScreenDC) {
    BitBlt(g_memDC, 0, 0, g_screenW, g_screenH, hScreenDC, 0, 0, SRCCOPY);
}

// --- Toggle capture invisibility ---
static void ToggleStealth(HWND hWnd) {
    g_hidden = !g_hidden;
    SetWindowDisplayAffinity(hWnd, g_hidden ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    InvalidateRect(hWnd, nullptr, TRUE);
}

// --- Update shared memory with current window position ---
static void UpdateSharedPosition(HWND hWnd) {
    if (!g_shared) return;
    RECT rc;
    GetWindowRect(hWnd, &rc);
    InterlockedExchange(&g_shared->posX, rc.left);
    InterlockedExchange(&g_shared->posY, rc.top);
    InterlockedExchange(&g_shared->width, rc.right - rc.left);
    InterlockedExchange(&g_shared->height, rc.bottom - rc.top);
    g_shared->hwnd = hWnd;
}

// --- Create shared memory and events for proxy communication ---
static bool InitSharedMemory(HWND hWnd) {
    g_shmemHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(StreamProofData), SP_SHMEM_NAME);
    if (!g_shmemHandle) return false;

    g_shared = (StreamProofData*)MapViewOfFile(
        g_shmemHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StreamProofData));
    if (!g_shared) return false;

    ZeroMemory(g_shared, sizeof(StreamProofData));
    g_shared->hwnd = hWnd;
    InterlockedExchange(&g_shared->active, 1);
    UpdateSharedPosition(hWnd);

    g_evtHide  = CreateEventA(nullptr, FALSE, FALSE, SP_EVT_HIDE);
    g_evtReady = CreateEventA(nullptr, FALSE, FALSE, SP_EVT_READY);
    g_evtDone  = CreateEventA(nullptr, FALSE, FALSE, SP_EVT_DONE);

    return (g_evtHide && g_evtReady && g_evtDone);
}

static void CleanupSharedMemory() {
    if (g_shared) {
        InterlockedExchange(&g_shared->active, 0);
        UnmapViewOfFile(g_shared);
        g_shared = nullptr;
    }
    if (g_shmemHandle) { CloseHandle(g_shmemHandle); g_shmemHandle = nullptr; }
    if (g_evtHide)     { CloseHandle(g_evtHide);     g_evtHide = nullptr; }
    if (g_evtReady)    { CloseHandle(g_evtReady);     g_evtReady = nullptr; }
    if (g_evtDone)     { CloseHandle(g_evtDone);      g_evtDone = nullptr; }
}

// --- Handle hide/show requests from the proxy DLL ---
// Called from the main loop. Non-blocking check.
static void HandleProxyRequests(HWND hWnd) {
    if (!g_evtHide) return;

    // Check if proxy is asking us to hide
    DWORD result = WaitForSingleObject(g_evtHide, 0);
    if (result != WAIT_OBJECT_0) return;

    // --- HIDE: move window off-screen so NvFBC doesn't see it ---
    RECT rc;
    GetWindowRect(hWnd, &rc);
    int savedX = rc.left;
    int savedY = rc.top;

    // Move far off-screen
    SetWindowPos(hWnd, nullptr, -32000, -32000, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);

    // Flush DWM so the compositor renders a frame without the overlay
    DwmFlush();

    InterlockedExchange(&g_shared->hidden, 1);

    // Signal proxy: "I'm hidden, you can capture now"
    SetEvent(g_evtReady);

    // Wait for proxy to finish the grab (up to 100ms)
    WaitForSingleObject(g_evtDone, 100);

    // --- SHOW: move back to original position ---
    SetWindowPos(hWnd, nullptr, savedX, savedY, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);

    InterlockedExchange(&g_shared->hidden, 0);
}

// --- Window procedure ---
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        if (g_memDC) {
            RECT rc;
            GetClientRect(hWnd, &rc);

            COLORREF borderCol = g_hidden ? COLOR_HIDDEN : COLOR_VISIBLE;
            HBRUSH br = CreateSolidBrush(borderCol);
            FrameRect(hdc, &rc, br);
            RECT inner = { BORDER, BORDER, rc.right - BORDER, rc.bottom - BORDER };
            FrameRect(hdc, &inner, br);
            DeleteObject(br);

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, BORDER, BORDER, PREVIEW_W, PREVIEW_H,
                       g_memDC, 0, 0, g_screenW, g_screenH, SRCCOPY);

            // Status label
            const wchar_t* label = g_hidden ? L" HIDDEN " : L" VISIBLE ";
            SetBkColor(hdc, borderCol);
            SetTextColor(hdc, RGB(255, 255, 255));
            HFONT font = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(hdc, font);
            TextOutW(hdc, BORDER + 4, BORDER + 4, label, lstrlenW(label));
            SelectObject(hdc, oldFont);
            DeleteObject(font);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_HOTKEY:
        if (wParam == HOTKEY_TOGGLE)
            ToggleStealth(hWnd);
        return 0;

    case WM_LBUTTONDOWN:
        g_dragging = true;
        SetCapture(hWnd);
        g_dragOffset.x = (SHORT)LOWORD(lParam);
        g_dragOffset.y = (SHORT)HIWORD(lParam);
        return 0;

    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT cursor;
            GetCursorPos(&cursor);
            SetWindowPos(hWnd, nullptr,
                         cursor.x - g_dragOffset.x,
                         cursor.y - g_dragOffset.y,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER);
            UpdateSharedPosition(hWnd);
        }
        return 0;

    case WM_LBUTTONUP:
        g_dragging = false;
        ReleaseCapture();
        return 0;

    case WM_RBUTTONUP:
        DestroyWindow(hWnd);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(hWnd, HOTKEY_TOGGLE);
        CleanupSharedMemory();
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- Entry point ---
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {

    // Check Windows build
    OSVERSIONINFOW ovi = { sizeof(ovi) };
    typedef LONG(WINAPI* RtlGetVersion_t)(OSVERSIONINFOW*);
    auto RtlGetVer = (RtlGetVersion_t)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (RtlGetVer) RtlGetVer(&ovi);
    if (ovi.dwBuildNumber < 19041) {
        wchar_t verbuf[128];
        wsprintfW(verbuf,
            L"Windows 10 build 19041+ required.\nYour build: %lu",
            ovi.dwBuildNumber);
        MessageBoxW(nullptr, verbuf, L"StreamProof", MB_OK | MB_ICONWARNING);
        return 1;
    }

    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInst;
    wc.hCursor        = LoadCursor(nullptr, IDC_SIZEALL);
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName  = L"StreamProofOverlay";
    RegisterClassExW(&wc);

    int winW = PREVIEW_W + BORDER * 2;
    int winH = PREVIEW_H + BORDER * 2;
    int posX = g_screenW - winW - MARGIN;
    int posY = MARGIN;

    HWND hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"StreamProofOverlay", L"StreamProof",
        WS_POPUP,
        posX, posY, winW, winH,
        nullptr, nullptr, hInst, nullptr);

    if (!hWnd) {
        MessageBoxW(nullptr, L"Failed to create overlay window.", L"Error", MB_OK);
        return 1;
    }

    // Set invisible to capture (works for DDA/WGC, proxy handles NvFBC)
    SetWindowDisplayAffinity(hWnd, WDA_EXCLUDEFROMCAPTURE);

    // Register F9 hotkey for manual toggle
    RegisterHotKey(hWnd, HOTKEY_TOGGLE, 0, VK_F9);

    // Init shared memory for proxy DLL communication
    if (!InitSharedMemory(hWnd)) {
        // Non-fatal: overlay still works without the proxy
    }

    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hWnd);

    HDC hScreenDC = GetDC(nullptr);
    g_memDC  = CreateCompatibleDC(hScreenDC);
    g_bitmap = CreateCompatibleBitmap(hScreenDC, g_screenW, g_screenH);
    SelectObject(g_memDC, g_bitmap);

    MSG msg;
    DWORD frameMs   = 1000 / TARGET_FPS;
    DWORD lastFrame = 0;

    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Check for hide/show requests from proxy DLL
        HandleProxyRequests(hWnd);

        DWORD now = GetTickCount();
        if (now - lastFrame >= frameMs) {
            CaptureScreen(hScreenDC);
            InvalidateRect(hWnd, nullptr, FALSE);
            lastFrame = now;
        }
        Sleep(1);
    }

    DeleteObject(g_bitmap);
    DeleteDC(g_memDC);
    ReleaseDC(nullptr, hScreenDC);
    return 0;
}
