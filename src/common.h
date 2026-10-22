// Shared between scrcpy-vcam.exe (producer) and scrcpy-vcam-source.dll (virtual camera media source).
#pragma once
#include <windows.h>
#include <stdint.h>

// {6B1E2C4A-8F3D-4E5B-9A7C-2D1F0E3B4A5C}
static const GUID CLSID_VcamSource = { 0x6b1e2c4a, 0x8f3d, 0x4e5b, { 0x9a, 0x7c, 0x2d, 0x1f, 0x0e, 0x3b, 0x4a, 0x5c } };
#define VCAM_CLSID_STR L"{6B1E2C4A-8F3D-4E5B-9A7C-2D1F0E3B4A5C}"

#define VCAM_NAME L"scrcpy-vcam"

// Fixed camera output format: NV12, letterboxed phone screen.
#define VCAM_WIDTH  1920
#define VCAM_HEIGHT 1080
#define VCAM_FPS    30
#define VCAM_FRAME_BYTES (VCAM_WIDTH * VCAM_HEIGHT * 3 / 2)

// The media source runs inside the Windows Camera Frame Server service (session 0,
// LOCAL SERVICE), so the shared objects live in the Global namespace. The DLL creates
// them (services hold SeCreateGlobalPrivilege), the exe opens them.
#define VCAM_SHM_NAME   L"Global\\ScrcpyVcamFrames"
#define VCAM_EVENT_NAME L"Global\\ScrcpyVcamFrameReady"
#define VCAM_MAGIC 0x4d414350u // "PCAM"

// Double buffer: the writer fills slot (seq + 1) & 1, then increments seq.
struct VcamShm
{
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
    volatile LONG64 seq;      // number of frames written so far, 0 = no frame yet
    volatile LONG64 readerTick; // GetTickCount64() of last frame delivered by the camera
    volatile LONG64 publishTime[2]; // MFGetSystemTime() when each slot was written
    uint8_t pad[16];
    uint8_t frames[2][VCAM_FRAME_BYTES];
};
