// Native post-processing for the Windows build (a built-in replacement for the
// ReShade chain SMAA -> AMD CAS -> Vibrance, which can't load into the ARM64 process).
// Runs on the implicit swap chain's back buffer just before the host Present.
// Settings: [postfx] in lb_paths.ini next to the exe; LB_POSTFX=0/1; Scroll Lock toggles.
#pragma once
#ifdef _WIN32
#include <d3d9.h>

// Behaviour flags for the host CreateDevice: strips D3DCREATE_PUREDEVICE while the pass
// is enabled, because a pure device can't return the state the pass saves and restores.
DWORD postfx_create_flags(DWORD flags);
// Call just before the host IDirect3DDevice9::Present.
void postfx_present(IDirect3DDevice9 *dev);
// Call just before the host IDirect3DSwapChain9::Present (acts only for swap chain 0).
void postfx_present_swapchain(IDirect3DSwapChain9 *sc);
// Call before IDirect3DDevice9::Reset: releases every D3DPOOL_DEFAULT resource.
void postfx_device_lost(void);
// Call before the guest's IDirect3DDevice9::Release reaches the host. When only the
// pass's own resources would keep the device alive, they are all released first.
void postfx_device_release(IDirect3DDevice9 *dev);
#endif
