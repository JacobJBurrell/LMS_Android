"""Runs a Python script in Windows background mode so long jobs don't make the PC laggy.

Usage: python tools/lowprio.py <script.py> [args...]

PROCESS_MODE_BACKGROUND_BEGIN lowers this process's CPU, disk I/O and memory
priority: it still uses idle CPU at full speed, but yields to foreground apps
and its pages are trimmed first under memory pressure. No-op off Windows.
"""
import ctypes
import os
import runpy
import sys

if os.name == "nt":
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    # HANDLE is pointer-sized: without these, the -1 pseudo-handle is passed as a 32-bit int
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    k32.SetPriorityClass.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    k32.SetPriorityClass.restype = wintypes.BOOL
    PROCESS_MODE_BACKGROUND_BEGIN = 0x00100000
    if not k32.SetPriorityClass(k32.GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN):
        print("lowprio: could not enter background mode (error %d)" % ctypes.get_last_error(), file=sys.stderr)

if len(sys.argv) < 2:
    sys.exit(__doc__)
script = os.path.abspath(sys.argv[1])
sys.argv = sys.argv[1:]
sys.path.insert(0, os.path.dirname(script))
runpy.run_path(script, run_name="__main__")
