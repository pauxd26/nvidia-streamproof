#pragma once
#include <windows.h>

// IPC names (shared between overlay.exe and nvfbc proxy DLL)
#define SP_SHMEM_NAME   "Local\\StreamProofShmem"
#define SP_EVT_HIDE     "Local\\StreamProofHide"
#define SP_EVT_READY    "Local\\StreamProofReady"
#define SP_EVT_DONE     "Local\\StreamProofDone"

// Shared memory layout
struct StreamProofData {
    LONG  active;       // 1 = overlay running, 0 = not
    LONG  hidden;       // 1 = overlay is currently hidden
    HWND  hwnd;         // overlay window handle
    LONG  posX;         // overlay screen X
    LONG  posY;         // overlay screen Y
    LONG  width;        // overlay width
    LONG  height;       // overlay height
};
