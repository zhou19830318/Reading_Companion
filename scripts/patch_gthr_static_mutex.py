"""
PlatformIO pre-build script: switch libstdc++'s embedded mutexes from dynamic
pthread_mutex_init/destroy to static PTHREAD_MUTEX_INITIALIZER lazy init.

Why (B2: three identical crashes during WiFi connect)
-----------------------------------------------------
All three crashes share one signature: Load access fault, MCAUSE=5,
MEPC inside pthread_mutex_lock_internal at `lw a4,4(a0)` (reading
esp_pthread_mutex_t::type at mux+4), RA inside pthread_mutex_destroy.

Root cause chain:
  1. toolchain `bits/os_defines.h` defines `_GTHREAD_USE_MUTEX_INIT_FUNC`,
     so `bits/gthr-default.h` does `#undef __GTHREAD_MUTEX_INIT`.
  2. Without `__GTHREAD_MUTEX_INIT`, `ext/concurrence.h`'s `__mutex`
     (the mutex embedded in every shared_ptr control block) calls
     `pthread_mutex_init()` in its ctor and `pthread_mutex_destroy()` in
     its (unconditionally declared) dtor, ignoring the init return value.
  3. During WiFi connect the internal heap drops to 2-5 KB
     (j.log: Free 194 B, MaxAlloc 32 B). pthread_mutex_init() then fails
     (malloc of esp_pthread_mutex_t or xSemaphoreCreateMutex fails) and
     the failure paths do NOT write `*mutex`, so the member keeps its
     uninitialized `new` garbage.
  4. Later `delete this` on the control block runs ~__mutex ->
     pthread_mutex_destroy(garbage) -> garbage is neither
     PTHREAD_MUTEX_INITIALIZER (0xFFFFFFFF) nor NULL -> lock_internal()
     dereferences mux+4 -> fault.

Fix (F2)
--------
Comment out `#define _GTHREAD_USE_MUTEX_INIT_FUNC 1` in every
`bits/os_defines.h` copy shipped with the toolchain. With the flag gone:
  * `__GTHREAD_MUTEX_INIT` stays defined (= PTHREAD_MUTEX_INITIALIZER),
  * concurrence.h `__mutex` constant-initialises `_M_mutex` in-class and
    declares NO destructor, so our TUs can never reach
    pthread_mutex_destroy() from a control block,
  * std::mutex's `__mutex_base` gets the constexpr default ctor,
  * lock() -> pthread_mutex_lock() -> pthread_mutex_init_if_static()
    lazy-initialises on first use (IDF pthread.c supports the static
    sentinel), which cannot fail catastrophically.

Side benefit: removes a ~90 B internal-heap malloc + 8 B mutex malloc per
shared_ptr control block (WiFi connect is exactly when the heap is empty).

Mixed-ABI note: prebuilt archive objects (libpthread.a, libstdc++.a) were
compiled with the dynamic path. Worst case for a block created there and
destroyed here is a bounded leak of that block's mutex, never a crash
(destroying a PTHREAD_MUTEX_INITIALIZER value early-returns 0).

This patches files inside the PlatformIO package cache (outside the repo).
It is idempotent and refuses to act if the upstream line text has changed,
so a toolchain update that restructures the header won't be corrupted.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os

OLD_LINE = "#define _GTHREAD_USE_MUTEX_INIT_FUNC 1"
NEW_LINE = (
    "//" + OLD_LINE + "  // crosspoint F2: static mutex init (see scripts/patch_gthr_static_mutex.py)"
)


def _toolchain_dir(env):
    d = env.PioPlatform().get_package_dir("toolchain-riscv32-esp")
    if d and os.path.isdir(d):
        return d
    return None


def _os_defines_headers(tc_dir):
    cxx_root = os.path.join(tc_dir, "riscv32-esp-elf", "include", "c++")
    if not os.path.isdir(cxx_root):
        return []
    found = []
    for root, _dirs, files in os.walk(cxx_root):
        if "os_defines.h" in files:
            found.append(os.path.join(root, "os_defines.h"))
    return found


def patch_os_defines(env):
    tc_dir = _toolchain_dir(env)
    if not tc_dir:
        print("WARN: patch_gthr_static_mutex: toolchain-riscv32-esp not found; skipping")
        return

    headers = _os_defines_headers(tc_dir)
    if not headers:
        print(
            "WARN: patch_gthr_static_mutex: no bits/os_defines.h under %s; "
            "toolchain layout changed - B2 crash guard NOT applied" % tc_dir
        )
        return

    patched = 0
    already = 0
    changed = 0
    for path in headers:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()

        if NEW_LINE in text:
            already += 1
            continue

        if OLD_LINE not in text:
            changed += 1  # upstream line text differs; do not touch this copy
            continue

        text = text.replace(OLD_LINE, NEW_LINE, 1)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        patched += 1

    if patched:
        print(
            "patched_gthr_static_mutex: %d os_defines.h patched (%d already patched)"
            % (patched, already)
        )
    elif already:
        pass  # fully applied already, keep the build log quiet
    if changed:
        print(
            "WARN: patch_gthr_static_mutex: %d os_defines.h copy(ies) no longer "
            "match '%s' (toolchain updated?); those copies keep dynamic mutex init"
            % (changed, OLD_LINE)
        )


patch_os_defines(env)  # noqa: F821
