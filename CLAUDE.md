# StreamProof — NvFBC Proxy Overlay for ShadowPlay Bypass

## What this project does
A stream-proof screen overlay that is **invisible to NVIDIA ShadowPlay** (and all other screen capture software). It shows a live screen preview in the top-right corner of the screen that doesn't appear in recordings.

## Architecture

### Two components:

**1. `streamproof.exe` (overlay)**
- Win32 popup window, always-on-top, no taskbar entry
- Captures screen via GDI `BitBlt`, shows scaled preview (384x216)
- Uses `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` — hides from DDA/WGC captures (OBS, Game Bar, Snipping Tool)
- **Does NOT use `WS_EX_LAYERED`** — layered windows break `SetWindowDisplayAffinity`, NVFBC will still capture them
- Sets affinity BEFORE first `ShowWindow` — some NVIDIA drivers ignore it if set after
- Creates shared memory + events for IPC with the proxy DLL
- Responds to hide/show signals from proxy during NvFBC grabs

**2. `nvfbc64.dll` (proxy DLL)**
- Replaces NVIDIA's nvfbc64.dll, loads the real one as `nvfbc64_real.dll`
- Exports `NvFBCCreateInstance` — hooks the function table returned by the real DLL
- Replaces `ToSysGrabFrame` and `ToHwEncGrabFrame` function pointers with hooks
- Hook flow: signal overlay to hide → wait for DwmFlush confirmation → call real grab → signal overlay to show
- Logs to `C:\streamproof_proxy.log`

### IPC mechanism:
- Shared memory: `Local\StreamProofShmem` — overlay position, active state
- Events: `Local\StreamProofHide`, `Local\StreamProofReady`, `Local\StreamProofDone`
- Overlay moves window to (-32000, -32000) during grab, moves back after

## Key technical decisions

### Why not just WDA_EXCLUDEFROMCAPTURE?
- Works against DDA (DXGI Desktop Duplication) and WGC (Windows Graphics Capture)
- Does NOT work against NvFBC — ShadowPlay captures at GPU framebuffer level, ignores display affinity
- Capture mode bytes from ShadowPlay: `0x14>>4=1=NvFBC`, `0x24>>4=2=DDA`, `0x34>>4=3=WGC`
- Forcing ShadowPlay to DDA mode (0x24) causes green screen crash after a few moments

### NvFBC function table layout (x64, SDK v7.x):
```
+0x00: dwVersion (4 bytes + 4 padding)
+0x08: nvFBCCreateHandle
+0x10: nvFBCDestroyHandle
+0x18: nvFBCGetStatus
+0x20: nvFBCCreateCaptureSession
+0x28: nvFBCDestroyCaptureSession
+0x30: nvFBCToSysSetUp
+0x38: nvFBCToSysGrabFrame          ← HOOKED
+0x40: nvFBCToHwEncSetUp
+0x48: nvFBCToHwEncGrabFrame        ← HOOKED
```
These offsets may vary per driver version — check `C:\streamproof_proxy.log` to verify hooks fired.

## Controls
- **F9** — toggle stealth (green border = hidden, red = visible)
- **Drag** — reposition overlay
- **Right-click / ESC** — quit

## Build
Requires MinGW g++:
```
build.bat
```
Or manually:
```
g++ -o streamproof.exe overlay.cpp -mwindows -ldwmapi -lgdi32 -luser32 -lntdll -O2 -s
g++ -shared -o nvfbc64.dll nvfbc_proxy.cpp -mwindows -ldwmapi -lgdi32 -luser32 -O2 -s
```

## Deployment (on machine with NVIDIA GeForce GPU)
1. Copy real `C:\Windows\System32\nvfbc64.dll` → rename to `nvfbc64_real.dll`
2. Place our proxy `nvfbc64.dll` where ShadowPlay loads from
3. Run `streamproof.exe` first
4. Start ShadowPlay recording (Alt+F9)

## What still needs testing
- Verify proxy DLL loads and hooks fire (check proxy log)
- Verify overlay hides during NvFBC grab (no flicker visible to user?)
- Verify overlay is absent from ShadowPlay recording
- May need to adjust function table offsets per driver version
- DwmFlush timing — does 1 flush guarantee the off-screen move is in the framebuffer?
- Test with both Instant Replay and manual recording modes

## Requirements
- Windows 10 2004+ (build 19041)
- NVIDIA GeForce GPU with GeForce Experience / ShadowPlay
- NOT a VM/RDP session (WDA flag fails there)
