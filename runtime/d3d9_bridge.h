// Shared declarations for the guest<->host Direct3D 9 bridge (C++ side).
#pragma once
#include <d3d9.h>
#ifdef GPTR
#undef GPTR  // <winbase.h> GlobalAlloc flag; rt.h's GPTR() is the guest-pointer helper
#endif
#include <cstdint>
#include <cstring>

extern "C" {
#include "rt.h"
}
#include "d3d9_ifaces.h"

// Guest COM objects: { vtable, host id }, vtables are arrays of thunks.
void com_register_iface(int iface, const char *name, const ShimFn *fns, int n);
void *com_host(uint32_t guest);
uint32_t com_wrap(void *host, int iface);
void com_QueryInterface(CPU *c);
void com_AddRef(CPU *c);
void com_Release(CPU *c);
uint32_t d3d9_wrap(void *host, int iface);
int d3d9_iface_from_iid(const GUID &iid);

template <class T, class R, class... P, class... A>
static inline R bridge_call(T *self, R (STDMETHODCALLTYPE T::*m)(P...), A... a) {
    return (self->*m)(((P)(a))...);
}

extern int g_d3d_trace, g_bisect;
bool d3d_skip_draw(void);
void d3d_trace(CPU *c, const char *name, int nargs);

extern "C" {
void *platform_window(void);  // the SDL_Window*
void platform_window_pixels(int *w, int *h);  // window size in pixels (platform_sdl.c)
void platform_present_hook(void);
#ifdef _WIN32
void *platform_hwnd(void);    // real HWND for the system d3d9.dll (runtime/win/win_platform.c)
#endif
}
// What D3D9 calls the device window: dxvk-native takes the SDL_Window* itself.
static inline void *platform_d3d_window(void) {
#ifdef _WIN32
    return platform_hwnd();
#else
    return platform_window();
#endif
}
