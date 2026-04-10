// =============================================================
// NvFBC Proxy DLL — StreamProof ShadowPlay Bypass
// =============================================================
//
// HOW IT WORKS:
//   ShadowPlay loads nvfbc64.dll and calls NvFBCCreateInstance()
//   to get a table of function pointers (create session, grab
//   frame, etc). This proxy DLL:
//
//   1. Loads the REAL nvfbc64.dll (renamed to nvfbc64_real.dll)
//   2. Calls the real NvFBCCreateInstance to fill the function table
//   3. Replaces the GrabFrame function pointers with our hooks
//   4. Our hooks: hide overlay → DwmFlush → real grab → show overlay
//
//   Result: NvFBC captures a clean framebuffer without the overlay.
//
// DEPLOYMENT:
//   1. Find nvfbc64.dll in your NVIDIA driver directory
//      (usually C:\Windows\System32\nvfbc64.dll)
//   2. COPY it to the same folder as this proxy, rename to nvfbc64_real.dll
//   3. Place this compiled DLL as nvfbc64.dll where ShadowPlay loads from
//   4. Run overlay.exe first, then start ShadowPlay recording
//
// =============================================================

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include "shared.h"

#pragma comment(lib, "dwmapi.lib")

// -----------------------------------------------------------
// NvFBC type definitions (from NVIDIA Capture SDK)
// -----------------------------------------------------------
typedef int            NVFBCSTATUS;
typedef unsigned int   NvU32;
typedef unsigned long long NVFBC_SESSION_HANDLE;

#define NVFBC_SUCCESS 0

// Generic function pointer type for grab functions
// Signature: NVFBCSTATUS func(NVFBC_SESSION_HANDLE handle, void* params)
typedef NVFBCSTATUS(__cdecl *PFN_GrabFrame)(NVFBC_SESSION_HANDLE, void*);

// The NvFBC function list — filled by NvFBCCreateInstance.
// Layout based on NvFBC SDK v7.x. The version field is followed
// by function pointers. On x64, each pointer is 8 bytes.
//
// If your driver version has a different layout, adjust the
// GRAB_TOSYS_OFFSET and GRAB_HWENC_OFFSET below.
//
// Typical layout (x64):
//   +0x00: dwVersion          (4 bytes + 4 padding)
//   +0x08: nvFBCCreateHandle
//   +0x10: nvFBCDestroyHandle
//   +0x18: nvFBCGetStatus
//   +0x20: nvFBCCreateCaptureSession
//   +0x28: nvFBCDestroyCaptureSession
//   +0x30: nvFBCToSysSetUp
//   +0x38: nvFBCToSysGrabFrame          ← HOOK THIS
//   +0x40: nvFBCToHwEncSetUp
//   +0x48: nvFBCToHwEncGrabFrame        ← HOOK THIS
//   +0x50: nvFBCCudaGrabFrame           (optional)
//   +0x58: nvFBCOGLGrabFrame            (optional)
//   +0x60+: more functions...

#define GRAB_TOSYS_OFFSET   0x38
#define GRAB_HWENC_OFFSET   0x48

// NvFBCCreateInstance params
// +0x00: dwVersion  (4 + 4 padding)
// +0x08: pFunctionList (pointer)
struct NvFBC_CreateInstanceParams {
    NvU32  dwVersion;
    NvU32  _pad;
    void*  pFunctionList;
};

typedef NVFBCSTATUS(__cdecl *PFN_NvFBCCreateInstance)(NvFBC_CreateInstanceParams*);

// -----------------------------------------------------------
// Globals
// -----------------------------------------------------------
static HMODULE                g_thisModule        = nullptr;
static HMODULE                g_realDll           = nullptr;
static PFN_NvFBCCreateInstance g_realCreateInstance = nullptr;

// Original (real) grab functions we replaced
static PFN_GrabFrame          g_realToSysGrab     = nullptr;
static PFN_GrabFrame          g_realToHwEncGrab   = nullptr;

// Shared memory and events for overlay communication
static HANDLE                 g_shmemHandle       = nullptr;
static StreamProofData*       g_shared            = nullptr;
static HANDLE                 g_evtHide           = nullptr;
static HANDLE                 g_evtReady          = nullptr;
static HANDLE                 g_evtDone           = nullptr;

// Debug log file (optional)
static FILE*                  g_log               = nullptr;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// -----------------------------------------------------------
// Open shared memory + events (created by overlay.exe)
// -----------------------------------------------------------
static bool ConnectToOverlay() {
    if (g_shared && g_shared->active)
        return true;  // already connected

    if (!g_shmemHandle) {
        g_shmemHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SP_SHMEM_NAME);
        if (!g_shmemHandle) return false;

        g_shared = (StreamProofData*)MapViewOfFile(
            g_shmemHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StreamProofData));
        if (!g_shared) {
            CloseHandle(g_shmemHandle);
            g_shmemHandle = nullptr;
            return false;
        }
    }

    if (!g_evtHide)  g_evtHide  = OpenEventA(EVENT_ALL_ACCESS, FALSE, SP_EVT_HIDE);
    if (!g_evtReady) g_evtReady = OpenEventA(EVENT_ALL_ACCESS, FALSE, SP_EVT_READY);
    if (!g_evtDone)  g_evtDone  = OpenEventA(EVENT_ALL_ACCESS, FALSE, SP_EVT_DONE);

    return (g_shared && g_evtHide && g_evtReady && g_evtDone);
}

// -----------------------------------------------------------
// Hide overlay before capture, show after
// -----------------------------------------------------------
static void HideOverlay() {
    if (!ConnectToOverlay()) return;
    if (!g_shared->active) return;

    // Signal overlay to hide
    SetEvent(g_evtHide);

    // Wait for overlay to confirm it's hidden + DwmFlush done
    WaitForSingleObject(g_evtReady, 50);  // 50ms timeout
}

static void ShowOverlay() {
    if (!g_shared) return;
    if (!g_shared->active) return;

    // Signal overlay to show again
    SetEvent(g_evtDone);
}

// -----------------------------------------------------------
// Hooked grab functions
// -----------------------------------------------------------
static NVFBCSTATUS __cdecl HookedToSysGrabFrame(NVFBC_SESSION_HANDLE h, void* params) {
    Log("[proxy] ToSysGrabFrame intercepted\n");

    HideOverlay();
    NVFBCSTATUS status = g_realToSysGrab(h, params);
    ShowOverlay();

    return status;
}

static NVFBCSTATUS __cdecl HookedToHwEncGrabFrame(NVFBC_SESSION_HANDLE h, void* params) {
    Log("[proxy] ToHwEncGrabFrame intercepted\n");

    HideOverlay();
    NVFBCSTATUS status = g_realToHwEncGrab(h, params);
    ShowOverlay();

    return status;
}

// -----------------------------------------------------------
// Hook the function table returned by NvFBCCreateInstance
// -----------------------------------------------------------
static void PatchFunctionList(void* pFuncList) {
    if (!pFuncList) return;

    unsigned char* base = (unsigned char*)pFuncList;

    // Read the current function pointers at known offsets
    PFN_GrabFrame* pToSys  = (PFN_GrabFrame*)(base + GRAB_TOSYS_OFFSET);
    PFN_GrabFrame* pHwEnc  = (PFN_GrabFrame*)(base + GRAB_HWENC_OFFSET);

    // Save originals
    g_realToSysGrab  = *pToSys;
    g_realToHwEncGrab = *pHwEnc;

    // Replace with hooks
    DWORD oldProtect;

    VirtualProtect(pToSys, sizeof(void*), PAGE_READWRITE, &oldProtect);
    *pToSys = HookedToSysGrabFrame;
    VirtualProtect(pToSys, sizeof(void*), oldProtect, &oldProtect);

    VirtualProtect(pHwEnc, sizeof(void*), PAGE_READWRITE, &oldProtect);
    *pHwEnc = HookedToHwEncGrabFrame;
    VirtualProtect(pHwEnc, sizeof(void*), oldProtect, &oldProtect);

    Log("[proxy] Patched function list at %p\n", pFuncList);
    Log("[proxy]   ToSysGrab:  real=%p  hook=%p\n", g_realToSysGrab, HookedToSysGrabFrame);
    Log("[proxy]   ToHwEncGrab: real=%p  hook=%p\n", g_realToHwEncGrab, HookedToHwEncGrabFrame);
}

// -----------------------------------------------------------
// Exported: NvFBCCreateInstance (our proxy entry point)
// -----------------------------------------------------------
extern "C" __declspec(dllexport)
NVFBCSTATUS __cdecl NvFBCCreateInstance(NvFBC_CreateInstanceParams* pParams) {
    Log("[proxy] NvFBCCreateInstance called (version=0x%08X)\n",
        pParams ? pParams->dwVersion : 0);

    if (!g_realCreateInstance) {
        Log("[proxy] ERROR: real DLL not loaded\n");
        return -1;
    }

    // Call the real NvFBCCreateInstance — fills in pFunctionList
    NVFBCSTATUS status = g_realCreateInstance(pParams);
    Log("[proxy] Real NvFBCCreateInstance returned %d\n", status);

    if (status == NVFBC_SUCCESS && pParams && pParams->pFunctionList) {
        PatchFunctionList(pParams->pFunctionList);
    }

    return status;
}

// -----------------------------------------------------------
// Load the real nvfbc64.dll
// -----------------------------------------------------------
static bool LoadRealDll() {
    // Try to load from same directory as this proxy (nvfbc64_real.dll)
    wchar_t myPath[MAX_PATH];
    GetModuleFileNameW(g_thisModule, myPath, MAX_PATH);

    // Replace filename with nvfbc64_real.dll
    wchar_t* slash = wcsrchr(myPath, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat(myPath, L"nvfbc64_real.dll");

    g_realDll = LoadLibraryW(myPath);
    if (!g_realDll) {
        // Fallback: try System32
        wchar_t sysPath[MAX_PATH];
        GetSystemDirectoryW(sysPath, MAX_PATH);
        wcscat(sysPath, L"\\nvfbc64_real.dll");
        g_realDll = LoadLibraryW(sysPath);
    }

    if (!g_realDll) {
        Log("[proxy] FATAL: could not load nvfbc64_real.dll\n");
        return false;
    }

    g_realCreateInstance = (PFN_NvFBCCreateInstance)
        GetProcAddress(g_realDll, "NvFBCCreateInstance");

    if (!g_realCreateInstance) {
        Log("[proxy] FATAL: NvFBCCreateInstance not found in real DLL\n");
        return false;
    }

    Log("[proxy] Loaded real DLL at %p, CreateInstance at %p\n",
        g_realDll, g_realCreateInstance);
    return true;
}

// -----------------------------------------------------------
// Forward any other exports from the real DLL.
// NvFBC typically only exports NvFBCCreateInstance, but if
// your version has more, add them here.
// -----------------------------------------------------------

// NvFBCEnable (some driver versions export this)
extern "C" __declspec(dllexport)
NVFBCSTATUS __cdecl NvFBCEnable(unsigned int flag) {
    Log("[proxy] NvFBCEnable(%u) forwarded\n", flag);
    typedef NVFBCSTATUS(__cdecl *Fn)(unsigned int);
    Fn real = (Fn)GetProcAddress(g_realDll, "NvFBCEnable");
    if (real) return real(flag);
    return -1;
}

// -----------------------------------------------------------
// DLL entry point
// -----------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_thisModule = hModule;
        DisableThreadLibraryCalls(hModule);

        // Open debug log
        g_log = fopen("C:\\streamproof_proxy.log", "a");
        Log("\n========== StreamProof Proxy DLL loaded (PID %lu) ==========\n",
            GetCurrentProcessId());

        if (!LoadRealDll()) {
            Log("[proxy] FATAL: failed to initialize\n");
            if (g_log) fclose(g_log);
            return FALSE;
        }

        // Try to connect to overlay (may not be running yet)
        ConnectToOverlay();
        Log("[proxy] Overlay connected: %s\n",
            (g_shared && g_shared->active) ? "yes" : "no (will retry on grab)");
        break;

    case DLL_PROCESS_DETACH:
        Log("[proxy] DLL unloading\n");
        if (g_shared) UnmapViewOfFile(g_shared);
        if (g_shmemHandle) CloseHandle(g_shmemHandle);
        if (g_evtHide) CloseHandle(g_evtHide);
        if (g_evtReady) CloseHandle(g_evtReady);
        if (g_evtDone) CloseHandle(g_evtDone);
        if (g_realDll) FreeLibrary(g_realDll);
        if (g_log) fclose(g_log);
        break;
    }
    return TRUE;
}
