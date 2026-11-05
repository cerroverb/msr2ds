/* Windows: whether the display showing the window is in HDR mode ("Use HDR" in the display settings), its peak
 * brightness and the SDR content brightness Windows uses there, for video.c's HDR output. Vulkan offers HDR swapchain
 * formats whether or not the display is in HDR mode; DXGI and the display configuration know. */
#define COBJMACROS
#include <windows.h>
#include <dxgi1_6.h>
#include <string.h>
#include <wchar.h>

/* not in every MinGW import library */
static const GUID iid_output6 = { 0x068346e8, 0xaaec, 0x4b84, { 0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1 } };

/* the white level Windows gives SDR content on that display (nits), 0 if unknown */
static float sdr_white(const wchar_t *gdi_name) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return 0;
    DISPLAYCONFIG_PATH_INFO paths[64]; DISPLAYCONFIG_MODE_INFO modes[128];
    if (np > 64) np = 64; if (nm > 128) nm = 128;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) != ERROR_SUCCESS) return 0;
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = { .header = { .type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, .size = sizeof src,
            .adapterId = paths[i].sourceInfo.adapterId, .id = paths[i].sourceInfo.id } };
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, gdi_name)) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl = { .header = { .type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, .size = sizeof wl,
            .adapterId = paths[i].targetInfo.adapterId, .id = paths[i].targetInfo.id } };
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS) return (float)wl.SDRWhiteLevel / 1000.f * 80.f;
    }
    return 0;
}

/* 1 when the window's display is in HDR mode (peak and paper white filled in, nits), 0 otherwise */
int win_hdr_info(void *hwnd, float *peak, float *paper) {
    *peak = 1000.f; *paper = 200.f;
    HMONITOR mon = MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi = { .cbSize = sizeof mi };
    if (!mon || !GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return 0;
    IDXGIFactory1 *f = NULL;   /* a new factory each time: one caches the outputs' state from when it was made */
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) return 0;
    int hdr = 0;
    IDXGIAdapter1 *a = NULL;
    for (UINT ai = 0; !hdr && IDXGIFactory1_EnumAdapters1(f, ai, &a) == S_OK; ai++) {
        IDXGIOutput *o = NULL;
        for (UINT oi = 0; !hdr && IDXGIAdapter1_EnumOutputs(a, oi, &o) == S_OK; oi++) {
            DXGI_OUTPUT_DESC d;
            IDXGIOutput6 *o6 = NULL;
            if (SUCCEEDED(IDXGIOutput_GetDesc(o, &d)) && d.Monitor == mon &&
                SUCCEEDED(IDXGIOutput_QueryInterface(o, &iid_output6, (void **)&o6))) {
                DXGI_OUTPUT_DESC1 d1;
                if (SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &d1))) {
                    hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    if (d1.MaxLuminance > 100.f) *peak = d1.MaxLuminance;
                }
                IDXGIOutput6_Release(o6);
            }
            IDXGIOutput_Release(o);
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    float w = sdr_white(mi.szDevice);
    if (w >= 80.f) *paper = w;
    return hdr;
}
