// PS4 runtime fixes, based on love-ps4 (src/common/ps4_heap.cpp and ps4.cpp), merged from the copies
// in every port (DolphinPS4, Crema, RPCS3, xemu, HENkaku, Conker, SMW, SSBU, ...). Public switches
// and diagnostics: ps4_kit.h. The --wrap list the build must pass: docs/merge-notes/runtime-core.md.
//
// - The OpenOrbis libc creates its heap on the first malloc by mapping 2.5 GiB of system
//   flexible memory; that fails on retail consoles and malloc then crashes at address 0x38,
//   before main(). The malloc family is redirected here with the linker's --wrap.
// - The heap is dlmalloc (ps4_dlmalloc.c) over a block of memory mapped here. Sony's
//   sceLibcMspace (what love-ps4 uses) stopped returning memory in Dolphin's static
//   initialisation after ~800 small allocations; dlmalloc also checks for corruption and
//   foreign frees, which are logged with the caller.
// - The system libc forwards every malloc of the process (Sony's modules too) here, through the
//   replacement table in ps4_crt1.S.
// - libc++abi runs thread_local destructors through __cxa_thread_atexit_impl, which the PS4
//   libc lacks (and create-fself refuses unresolved imports).
// - mmap, clock_gettime, pthread_once, pthread_create, munmap and mprotect are wrapped too; each
//   wrapper says why.
//
// No loop here waits with sched_yield(): the PS4 scheduler only yields to threads of the same or
// higher priority, so a waiter that yields while a lower-priority thread holds what it waits for
// never lets that thread run again (dlmalloc's spin locks froze Crema 1.61, Conker v00.09/v00.10
// and TOTK that way). Waits spin briefly, then sleep (Backoff below).

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <immintrin.h>

#include <atomic>
#include <memory>
#include <thread>

#include "ps4_kit.h"

// Declared by hand, under ps4k_ names bound to the kernel's symbols: the toolchain headers give
// some of these the wrong (or no) prototypes, and a header that declares them differently can then
// never clash with these.
extern "C" {
int32_t ps4k_sceKernelDebugOutText(int32_t channel, const char* fmt, ...) __asm__("sceKernelDebugOutText");
int32_t ps4k_sceKernelAvailableFlexibleMemorySize(size_t* size) __asm__("sceKernelAvailableFlexibleMemorySize");
int32_t ps4k_sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment)
    __asm__("sceKernelReserveVirtualRange");
int32_t ps4k_sceKernelMunmap(void* addr, size_t len) __asm__("sceKernelMunmap");
int32_t ps4k_sceKernelMprotect(const void* addr, size_t len, int32_t prot) __asm__("sceKernelMprotect");
int32_t ps4k_sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags, const char* name)
    __asm__("sceKernelMapNamedFlexibleMemory");
int32_t ps4k_sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int32_t prot, int32_t flags,
                                                   const char* name) __asm__("sceKernelMapNamedSystemFlexibleMemory");
size_t ps4k_sceKernelGetDirectMemorySize(void) __asm__("sceKernelGetDirectMemorySize");
int32_t ps4k_sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t type,
                                           off_t* physOut) __asm__("sceKernelAllocateDirectMemory");
int32_t ps4k_sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t* startOut,
                                                size_t* sizeOut) __asm__("sceKernelAvailableDirectMemorySize");
int32_t ps4k_sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align)
    __asm__("sceKernelMapDirectMemory");
int32_t ps4k_sceKernelReleaseDirectMemory(off_t start, size_t len) __asm__("sceKernelReleaseDirectMemory");
int32_t ps4k_sceKernelQueryMemoryProtection(void* address, void** start, void** end, uint32_t* protection)
    __asm__("sceKernelQueryMemoryProtection");
int32_t ps4k_sceKernelJitCreateSharedMemory(const char* name, size_t len, int32_t max_prot, int32_t* fd)
    __asm__("sceKernelJitCreateSharedMemory");
int32_t ps4k_sceKernelJitCreateAliasOfSharedMemory(int32_t fd, int32_t max_prot, int32_t* alias_fd)
    __asm__("sceKernelJitCreateAliasOfSharedMemory");
int32_t ps4k_sceKernelClose(int32_t fd) __asm__("sceKernelClose");
struct SceKernelSwVersion {
    size_t size;
    char text[0x1C];
    uint32_t version;
};
int32_t ps4k_sceKernelGetSystemSwVersion(SceKernelSwVersion* version) __asm__("sceKernelGetSystemSwVersion");
uint64_t ps4k_sceKernelGetTscFrequency(void) __asm__("sceKernelGetTscFrequency");
uint64_t ps4k_sceKernelReadTsc(void) __asm__("sceKernelReadTsc");
uint64_t ps4k_sceKernelGetProcessTime(void) __asm__("sceKernelGetProcessTime");
void* ps4k_scePthreadSelf(void) __asm__("scePthreadSelf");
int ps4k_scePthreadSetaffinity(void* thread, uint64_t mask) __asm__("scePthreadSetaffinity");
int ps4k_scePthreadGetaffinity(void* thread, uint64_t* mask) __asm__("scePthreadGetaffinity");
int ps4k_scePthreadSetprio(void* thread, int prio) __asm__("scePthreadSetprio");
int ps4k_scePthreadGetprio(void* thread, int* prio) __asm__("scePthreadGetprio");
int32_t ps4k_sceKernelGetModuleList(int32_t* handles, size_t count, size_t* available)
    __asm__("sceKernelGetModuleList");
int32_t ps4k_sceKernelGetModuleInfo(int32_t handle, void* info) __asm__("sceKernelGetModuleInfo");

// The kernel's (FreeBSD) sigaction, with its own layout: independent of which signal.h the build
// uses (musl's has another layout and Linux signal numbers).
struct Ps4Sigaction {
    void (*handler)(int, void*, void*);
    int flags;
    uint32_t mask[4];
};
int ps4k_sigaction(int sig, const Ps4Sigaction* action, Ps4Sigaction* old) __asm__("sigaction");

// dlmalloc mspace API (ps4_dlmalloc.c).
typedef void* mspace;
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void* mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void* mem);
void* mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
void* mspace_realloc(mspace msp, void* mem, size_t newsize);
void* mspace_memalign(mspace msp, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void* mem);
size_t mspace_footprint(mspace msp);
size_t mspace_max_footprint(mspace msp);
int ps4_heap_ok(void* msp);
size_t ps4_heap_top_free(void* msp);

// Hooks into the other runtime files and the app. Weak no-op definitions, not weak declarations: a
// weak symbol nothing defines becomes an import, and create-fself refuses the eboot ("missing module
// for symbol"). The real ones (strong) win at link time; a weak definition is never inlined.
__attribute__((weak)) void ps4_boot_trace(const char*) {}                    // ps4_crashlog.cpp
__attribute__((weak)) void ps4_install_signal_stack(void) {}                 // ps4_crashlog.cpp
__attribute__((weak)) void ps4_watch_thread(const char*) {}                  // ps4_sampler.cpp
__attribute__((weak)) void ps4_profile_thread_begin(void) {}                 // ps4_profile.cpp
__attribute__((weak)) void ps4_kit_configure_heap(ps4_kit_heap_options*) {}  // the app (ps4_kit.h)

// The real functions behind the wraps (the linker's --wrap binds them).
void* __real_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset);
int __real_munmap(void* addr, size_t len);
int __real_mprotect(void* addr, size_t len, int prot);
int32_t __real_sceKernelMunmap(void* addr, size_t len);
int32_t __real_sceKernelMprotect(const void* addr, size_t len, int prot);
int32_t __real_sceKernelMtypeprotect(const void* addr, size_t len, int type, int prot);
int __real_clock_gettime(clockid_t id, struct timespec* ts);
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);

// OpenOrbis link.x: start of .text (ELF address 0), and the end of .text + .rodata (weak: another
// link script may not define it).
extern char __text_start[];
__attribute__((weak)) extern char __eh_frame_start[];
}

namespace {

const size_t MB = 1024 * 1024;
const size_t PAGE = 16 * 1024;
const int32_t PROT_CPU_RW = 0x3;
const int32_t PROT_CPU_RWX = 0x7;
const int32_t MAP_FIXED_FLAG = 0x10;

// --- Settings ------------------------------------------------------------------------------------

constexpr int MAX_ARENAS_CAP = 64;
constexpr int MAX_BIG_ARENAS_CAP = 32;
constexpr int MAX_OVERFLOW_CAP = 8;

constinit ps4_kit_heap_options g_opt = {
    "ps4",                      // log_tag
    PS4_KIT_HEAP_SYSTEM_FIRST,  // source_order
    256 * MB,                   // system_pool_max
    1024 * MB,                  // direct_max
    320 * MB,                   // regular_reserve
    0,                          // fixed_address
    10 * MB,                    // arena_size
    20,                         // max_arenas
    32 * MB,                    // big_arena_size
    10,                         // max_big_arenas
    6,                          // max_overflow_heaps
    192 * MB,                   // overflow_direct_reserve
    32,                         // auto_arena_waits
    4 * MB,                     // auto_arena_size
    12,                         // max_auto_arenas
    0,                          // regular_only
    64 * MB,                    // regular_max
    512 * MB,                   // overflow_direct_max
};
// The kernel's names for our mappings (shown in its memory listings), from the log tag.
char g_nameHeap[32] = "ps4 heap";
char g_nameMmap[32] = "ps4 mmap";
char g_nameCode[32] = "ps4 code";

int clampInt(int value, int lo, int hi) { return value < lo ? lo : value > hi ? hi : value; }

void loadOptions() {
    ps4_kit_configure_heap(&g_opt);  // (the app's, or the weak no-op above)
    if (!g_opt.log_tag || !g_opt.log_tag[0])
        g_opt.log_tag = "ps4";
    g_opt.max_arenas = clampInt(g_opt.max_arenas, 0, MAX_ARENAS_CAP);
    g_opt.max_big_arenas = clampInt(g_opt.max_big_arenas, 0, MAX_BIG_ARENAS_CAP);
    g_opt.max_overflow_heaps = clampInt(g_opt.max_overflow_heaps, 0, MAX_OVERFLOW_CAP);
    g_opt.arena_size = (g_opt.arena_size + PAGE - 1) & ~(PAGE - 1);
    g_opt.big_arena_size = (g_opt.big_arena_size + PAGE - 1) & ~(PAGE - 1);
    g_opt.max_auto_arenas = clampInt(g_opt.max_auto_arenas, 0, MAX_ARENAS_CAP);
    g_opt.auto_arena_size = (g_opt.auto_arena_size + PAGE - 1) & ~(PAGE - 1);
    g_opt.fixed_address &= ~static_cast<uintptr_t>(PAGE - 1);
    snprintf(g_nameHeap, sizeof(g_nameHeap), "%.20s heap", g_opt.log_tag);
    snprintf(g_nameMmap, sizeof(g_nameMmap), "%.20s mmap", g_opt.log_tag);
    snprintf(g_nameCode, sizeof(g_nameCode), "%.20s code", g_opt.log_tag);
}

// --- Logging -------------------------------------------------------------------------------------

// sceKernelDebugOutText doesn't format its arguments; vsnprintf doesn't allocate.
__attribute__((format(printf, 1, 2))) void heapLog(const char* fmt, ...) {
    char line[288];
    int len = snprintf(line, sizeof(line), "[%s] ", g_opt.log_tag);
    if (len < 0 || len >= static_cast<int>(sizeof(line)))
        len = 0;
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + len, sizeof(line) - len, fmt, args);
    va_end(args);
    ps4k_sceKernelDebugOutText(0, line);
    if (ps4_boot_trace)
        ps4_boot_trace(line);
}

// The first 32 events of a kind, then every 256th: xemu maps its coroutine stacks through mmap, and
// a log line (a file write) for each slowed its disk I/O down further (xemu v00.09).
bool sampledLog(std::atomic<unsigned>& counter, unsigned* number) {
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    *number = n;
    return n < 32 || n % 256 == 0;
}

unsigned long long elf(const void* address) {
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                           reinterpret_cast<uintptr_t>(__text_start));
}

// Where the eboot's code (and read-only data) ends: a stack word in [__text_start, codeEnd) is
// taken for a return address. A fixed 16 MiB missed most of the large emulators' code.
uintptr_t codeEnd() {
    const uintptr_t start = reinterpret_cast<uintptr_t>(__text_start);
    const uintptr_t end = reinterpret_cast<uintptr_t>(__eh_frame_start);
    return end > start ? end : start + 0x8000000;
}

// The end of this thread's stack: stack scans stop there. Reading past it ended threads that sat
// near the top of their stack (Breath of the Wild in Crema Lab 3.32 when the heap filled up; RPCS3's
// "PPU Syscall Usage Thread" in Devil May Cry 4, v01.46).
__attribute__((tls_model("initial-exec"))) thread_local uintptr_t t_stackEnd = 0;

uintptr_t stackEnd() {
    if (!t_stackEnd) {
        void* addr = nullptr;
        size_t size = 0;
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) == 0) {
            if (pthread_attr_get_np(pthread_self(), &attr) == 0)
                pthread_attr_getstack(&attr, &addr, &size);
            pthread_attr_destroy(&attr);
        }
        const uintptr_t frame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        uintptr_t end = size ? reinterpret_cast<uintptr_t>(addr) + size : 0;
        if (end <= frame) {
            // unknown (the main thread may not say): the end of the mapping the frame is in, or
            // else only the current page is surely mapped
            void* mapStart = nullptr;
            void* mapEnd = nullptr;
            uint32_t protection = 0;
            if (ps4k_sceKernelQueryMemoryProtection(reinterpret_cast<void*>(frame), &mapStart, &mapEnd,
                                                    &protection) == 0 &&
                (protection & 1) && reinterpret_cast<uintptr_t>(mapEnd) > frame)
                end = reinterpret_cast<uintptr_t>(mapEnd);
            else
                end = (frame | (PAGE - 1)) + 1;
        }
        t_stackEnd = end;
    }
    return t_stackEnd;
}

// How many words from `stack` up are inside this thread's stack, at most `limit`.
int stackWords(const uintptr_t* stack, int limit) {
    const uintptr_t end = stackEnd(), from = reinterpret_cast<uintptr_t>(stack);
    if (end <= from)
        return 0;
    const uintptr_t words = (end - from) / sizeof(uintptr_t);
    return words < static_cast<uintptr_t>(limit) ? static_cast<int>(words) : limit;
}

// Logs the code addresses on the current stack (no frame pointers in release builds).
void logStack() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(__text_start), end = codeEnd();
    const auto* stack = static_cast<const uintptr_t*>(__builtin_frame_address(0));
    const int words = stackWords(stack, 512);
    int found = 0;
    for (int i = 0; i < words && found < 16; i++) {
        if (stack[i] >= base && stack[i] < end) {
            heapLog("heap:   stack elf 0x%llx\n", static_cast<unsigned long long>(stack[i] - base));
            found++;
        }
    }
}

// --- Waiting ---------------------------------------------------------------------------------------

// A short spin, then 50 us sleeps: a sleeping waiter lets threads of any priority run, so a
// lower-priority holder on the same core gets to finish (see the file comment).
struct Backoff {
    int spins = 0;
    void wait() {
        if (++spins < 64) {
            _mm_pause();
            return;
        }
        struct timespec pause = {0, 50000};
        nanosleep(&pause, nullptr);
    }
};

// The runtime's own locks: critical sections of a few instructions, taken rarely.
struct SleepLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    void lock() {
        Backoff backoff;
        while (flag.test_and_set(std::memory_order_acquire))
            backoff.wait();
    }
    void unlock() { flag.clear(std::memory_order_release); }
};

// --- The main heap ---------------------------------------------------------------------------------

mspace g_heap = nullptr;
const char* g_heapPool = "none";
uintptr_t g_heapBase = 0;
size_t g_heapSize = 0;

mspace createMspace(const char* pool, void* base, size_t size) {
    mspace msp = create_mspace_with_base(base, size, 1);
    if (!msp) {
        heapLog("heap: create_mspace_with_base on %zu MiB of %s failed\n", size / MB, pool);
        return nullptr;
    }
    void* test = mspace_malloc(msp, 4096);
    if (!test) {
        heapLog("heap: %zu MiB of %s at %p: self-test allocation failed\n", size / MB, pool, base);
        return nullptr;
    }
    mspace_free(msp, test);
    g_heapPool = pool;
    g_heapBase = reinterpret_cast<uintptr_t>(base);
    g_heapSize = size;
    heapLog("heap: dlmalloc on %zu MiB of %s at %p\n", size / MB, pool, base);
    return msp;
}

// The PS4 kernel library allocates its own objects (pthread attributes, ...) at 0x200000000,
// where a kernel-placed reservation of ours also lands; those allocations wiped the heap's
// control block. Ask for an address well above that instead.
void* reserveAwayFromKernel(size_t size) {
    const uintptr_t hints[] = {0x1000000000ull, 0x800000000ull, 0x600000000ull};
    for (uintptr_t hint : hints) {
        void* address = reinterpret_cast<void*>(hint);
        if (ps4k_sceKernelReserveVirtualRange(&address, size, 0, PAGE) == 0) {
            if (reinterpret_cast<uintptr_t>(address) >= 0x400000000ull)
                return address;
            heapLog("heap: reservation hint %p gave %p, trying another\n", reinterpret_cast<void*>(hint), address);
            ps4k_sceKernelMunmap(address, size);
        }
    }
    return nullptr;
}

// The heap's range with its guard page in front: at the app's fixed address when it asks for one
// (G-Diffuser resolves the game's 32-bit pointers by their low 32 bits against registered host
// ranges, so the heap's low 32 bits must not overlap the eboot, N64 segmented addresses or
// KSEG0/1), else away from the kernel's objects.
void* reserveHeapRange(size_t sizeWithGuard) {
    if (g_opt.fixed_address > PAGE) {
        void* wanted = reinterpret_cast<void*>(g_opt.fixed_address - PAGE);
        void* address = wanted;
        if (ps4k_sceKernelReserveVirtualRange(&address, sizeWithGuard, MAP_FIXED_FLAG, PAGE) == 0) {
            if (address == wanted)
                return address;
            heapLog("heap: fixed reservation landed at %p, trying hints\n", address);
            ps4k_sceKernelMunmap(address, sizeWithGuard);
        } else {
            heapLog("heap: fixed reservation at %p refused, trying hints\n", reinterpret_cast<void*>(g_opt.fixed_address));
        }
    }
    return reserveAwayFromKernel(sizeWithGuard);
}

mspace mapAndCreate(bool systemPool, size_t size) {
    // Reserve one extra page in front of the heap and leave it inaccessible: a stray write
    // just before the heap's control block then faults where it happens (see crash.log).
    void* guard = reserveHeapRange(size + PAGE);
    if (!guard)
        return nullptr;
    void* base = static_cast<char*>(guard) + PAGE;
    int32_t ret = systemPool
                      ? ps4k_sceKernelMapNamedSystemFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, g_nameHeap)
                      : ps4k_sceKernelMapNamedFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, g_nameHeap);
    if (ret != 0) {
        heapLog("heap: mapping %zu MiB of %s flexible memory failed (0x%x)\n", size / MB,
                systemPool ? "system" : "regular", ret);
        ps4k_sceKernelMunmap(guard, size + PAGE);  // a fixed range must be free for the next size
        return nullptr;
    }
    return createMspace(systemPool ? "system flexible memory" : "flexible memory", base, size);
}

// A heap in direct memory (the 4.5 GiB pool, the GPU's), mapped where the kernel library's objects
// aren't (unplaced, it could land at 0x200000000 like the reservation above). RPCS3 takes its main
// heap from here first: the app's flexible memory is only ~720 MiB in all and the eboot's image
// took ~200 MiB of it; with a 512 MiB heap there, the first run stopped in static init with
// "FMEM 719.7/720.7" in the kernel log.
mspace createDirectHeap(size_t size) {
    off_t phys = 0;
    if (ps4k_sceKernelAllocateDirectMemory(0, ps4k_sceKernelGetDirectMemorySize(), size, 2 * MB, 0, &phys) != 0) {
        heapLog("heap: %zu MiB of direct memory not available\n", size / MB);
        return nullptr;
    }
    void* guard = reserveAwayFromKernel(size + 2 * MB);
    if (!guard) {
        ps4k_sceKernelReleaseDirectMemory(phys, size);
        return nullptr;
    }
    // 2 MiB aligned (large pages) after a guard gap in front of the control block
    void* base = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(guard) + 2 * MB) & ~(uintptr_t)(2 * MB - 1));
    if (base == guard)
        base = static_cast<char*>(guard) + 2 * MB;
    const int32_t ret = ps4k_sceKernelMapDirectMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, phys, 2 * MB);
    if (ret != 0) {
        heapLog("heap: mapping %zu MiB of direct memory failed (0x%x)\n", size / MB, ret);
        ps4k_sceKernelMunmap(guard, size + 2 * MB);
        ps4k_sceKernelReleaseDirectMemory(phys, size);
        return nullptr;
    }
    return createMspace("direct memory", base, size);
}

mspace createHeap() {
    size_t available = 0;
    int32_t ret = ps4k_sceKernelAvailableFlexibleMemorySize(&available);
    heapLog("heap: flexible memory available %zu MiB (0x%x)\n", available / MB, ret);
    // Where does the kernel library put its own objects? (8 bytes reserved even if the
    // headers' pthread_mutexattr_t is the 4-byte musl one.)
    void* attr[2] = {nullptr, nullptr};
    if (pthread_mutexattr_init(reinterpret_cast<pthread_mutexattr_t*>(attr)) == 0) {
        heapLog("heap: kernel pthread objects live around %p\n", attr[0]);
        pthread_mutexattr_destroy(reinterpret_cast<pthread_mutexattr_t*>(attr));
    }
    const size_t candidates[] = {1024 * MB, 768 * MB, 512 * MB, 384 * MB, 256 * MB, 192 * MB, 128 * MB};
    if (g_opt.regular_only) {
        // regular flexible memory only, from regular_max down (see ps4_kit.h)
        for (size_t size = g_opt.regular_max & ~(PAGE - 1); size >= 16 * MB; size = (size / 2) & ~(PAGE - 1)) {
            if (mspace msp = mapAndCreate(false, size))
                return msp;
        }
        heapLog("heap: could not create a heap in regular flexible memory\n");
        return nullptr;
    }
    if (g_opt.source_order == PS4_KIT_HEAP_DIRECT_FIRST) {
        // A direct_max under 256 MiB is a small heap meant to grow by overflow heaps (WinePS4's program
        // processes): that size itself. (The sizes below were never tried under 256 MiB, and WinePS4
        // v00.59's 64 MiB heaps went to the system pool instead: every program took 128 MiB of what the PS4
        // lives on, until "Failed to create the system memory" closed the app.)
        if (g_opt.direct_max >= 16 * MB && g_opt.direct_max < 256 * MB) {
            if (mspace msp = createDirectHeap((g_opt.direct_max + 2 * MB - 1) & ~(2 * MB - 1)))
                return msp;
        }
        for (size_t size : candidates) {
            if (size > g_opt.direct_max || size < 256 * MB)
                continue;
            if (mspace msp = createDirectHeap(size))
                return msp;
        }
    }
    // The system flexible memory pool, leaving regular flexible memory to Piglet. Not all of it:
    // the kernel's own objects ("ScePthread internal memory", std::mutex's kernel objects) and the
    // system libc heap that Sony modules (Piglet's shader compiler) grow on demand come from the
    // same pool. With a 1 GiB heap, compiling Dolphin's larger shaders ran the system out of
    // memory; with 512 MiB, two minutes into SMW a std::mutex could no longer get its kernel object
    // ("mutex lock failed: Out of memory"). Past system_pool_max the overflow heaps take over.
    for (size_t size : candidates) {
        if (size > g_opt.system_pool_max)
            continue;
        if (mspace msp = mapAndCreate(true, size))
            return msp;
    }
    // A process forked by the app reads a nonsense amount (~2^64 bytes): halving from there asked for
    // 512 GiB, 256 GiB, ... and the whole PS4 froze at the 4 GiB request (WinePS4 v00.40/41). The
    // console has 8 GiB in all; more than that is not believed.
    if (ret == 0 && available > 8192 * MB) {
        heapLog("heap: %zu MiB of flexible memory available is not possible: none of it taken\n", available / MB);
        available = 0;
    }
    size_t size = 0;
    if (ret == 0 && available > g_opt.regular_reserve + 32 * MB)
        size = (available - g_opt.regular_reserve) & ~(PAGE - 1);
    for (; size >= 32 * MB; size = (size / 2) & ~(PAGE - 1)) {
        if (mspace msp = mapAndCreate(false, size))
            return msp;
    }
    // Last resort (PSChrome's heap): direct memory.
    const size_t directSizes[] = {256 * MB, 128 * MB};
    for (size_t direct : directSizes) {
        if (mspace msp = createDirectHeap(direct))
            return msp;
    }
    heapLog("heap: could not create a heap, out of memory\n");
    return nullptr;
}

bool g_creatingHeap = false;

// The cores the process started on, read at the first allocation (static init, main thread, before
// the app pins anything): 0x7F when the app was given the 7th core, 0x3F otherwise.
void recordInitialCores() {
    uint64_t mask = 0;
    ps4k_scePthreadGetaffinity(ps4k_scePthreadSelf(), &mask);
    if (__builtin_popcountll(mask & 0x7F) < 6)
        mask = 0x3F;  // pinned already, or unreadable: the six cores every app has
    if (!ps4_new_thread_cores)
        ps4_new_thread_cores = mask & 0x7F;
}

inline mspace getHeap() {
    // The first allocation happens in a static constructor, before any threads exist. While the
    // heap is being created, nested allocations (if a kernel call allocates) just fail.
    if (g_heap == nullptr && !g_creatingHeap) {
        g_creatingHeap = true;
        loadOptions();
        g_heap = createHeap();
        recordInitialCores();
        g_creatingHeap = false;
    }
    return g_heap;
}

// --- Overflow heaps ----------------------------------------------------------------------------------
//
// Direct memory, mapped when the main heap (and the overflow heaps before) can't serve a request.
// Spider-Man 2 fills most of a 512 MiB main heap; a save state then needed one 99 MiB block
// (148 MiB in Mario Kart: Double Dash!!), the allocation failed and the app crashed. With FOOTERS,
// free and realloc find these heaps from the chunk itself.
//
// Several: LLVM compiling Feeding Frenzy 2's 312,000-block executable 7 ways at once filled RPCS3's
// 1 GiB main heap and a 512 MiB overflow heap with over 1 GiB of direct memory still free; booting
// the PS3 system menu took two more heaps in 2 s. A new heap leaves overflow_direct_reserve for the
// app's own direct memory (GPU memory, an emulated machine's RAM).
//
// And given back: the first compile of Devil May Cry 4 made two overflow heaps that then sat
// mostly empty while the game itself ran out of direct memory ("out of direct memory" 20 minutes
// in, then a frozen exit). Each heap counts the bytes its live chunks hold; one that is empty for a
// while is unmapped and its direct memory released (ps4_heap_trim). Allocations and frees pin the
// heap they use, so it is never unmapped under them.
mspace g_overflow[MAX_OVERFLOW_CAP] = {};  // nullptr: slot free (never used, or released)
uintptr_t g_overflowBase[MAX_OVERFLOW_CAP] = {};
size_t g_overflowSize[MAX_OVERFLOW_CAP] = {};
off_t g_overflowPhys[MAX_OVERFLOW_CAP] = {};
// the size of this thread's allocation now being made (an overflow heap of regular flexible memory
// takes it whole)
__attribute__((tls_model("initial-exec"))) thread_local size_t t_want = 0;
constinit std::atomic<int64_t> g_overflowInUse[MAX_OVERFLOW_CAP];
constinit std::atomic<int> g_overflowPins[MAX_OVERFLOW_CAP];
constinit std::atomic<bool> g_overflowClosing[MAX_OVERFLOW_CAP];
int g_overflowEmptyTicks[MAX_OVERFLOW_CAP] = {};
int g_overflowCount = 0;  // slots ever used (atomic accesses)
bool g_overflowExhausted = false;
// Held while a heap is added (~16 ms) or released. Threads that run out meanwhile wait on it and
// then use the new heap: returning nothing failed the allocations of every thread that ran out at
// the same time (Breath of the Wild in Crema 3.32).
constinit SleepLock g_overflowLock;

// The overflow heap holding `p`, or -1
int overflowIndex(const void* p) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    const int count = __atomic_load_n(&g_overflowCount, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        const size_t size = __atomic_load_n(&g_overflowSize[i], __ATOMIC_ACQUIRE);
        if (size && address >= g_overflowBase[i] && address < g_overflowBase[i] + size)
            return i;
    }
    return -1;
}

// Pins the heap holding `p` (if an overflow heap) for an operation on that chunk; -1 if none
int pinChunk(const void* p) {
    const int i = overflowIndex(p);
    if (i >= 0)
        g_overflowPins[i].fetch_add(1, std::memory_order_acq_rel);
    return i;
}
void unpin(int i) {
    if (i >= 0)
        g_overflowPins[i].fetch_sub(1, std::memory_order_acq_rel);
}
// Live bytes of chunk `p` (pinned or just allocated) enter or leave its heap's count
void countChunk(int i, const void* p, int64_t sign) {
    if (i >= 0 && p)
        g_overflowInUse[i].fetch_add(sign * static_cast<int64_t>(mspace_usable_size(p)), std::memory_order_acq_rel);
}

// Maps one more overflow heap; false if none could be (no direct memory to spare, or all in use)
// regular_only: one more heap of regular flexible memory, regular_max big, or as big as the request
// (t_want) that did not fit (see ps4_kit.h); its slot's phys is -1
bool addRegularOverflowHeap(int slot, int count) {
    const size_t want = (t_want + MB + MB - 1) & ~(MB - 1);
    const size_t size = want > g_opt.regular_max ? want : g_opt.regular_max;
    void* base = reserveAwayFromKernel(size);
    if (!base)
        return false;
    if (ps4k_sceKernelMapNamedFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, g_nameHeap) != 0) {
        ps4k_sceKernelMunmap(base, size);
        return false;
    }
    mspace msp = create_mspace_with_base(base, size, 1);
    if (!msp) {
        ps4k_sceKernelMunmap(base, size);
        return false;
    }
    g_overflowBase[slot] = reinterpret_cast<uintptr_t>(base);
    g_overflowPhys[slot] = -1;
    g_overflowInUse[slot].store(0);
    g_overflowEmptyTicks[slot] = 0;
    __atomic_store_n(&g_overflowSize[slot], size, __ATOMIC_RELEASE);
    __atomic_store_n(&g_overflow[slot], msp, __ATOMIC_RELEASE);
    if (slot == count)
        __atomic_store_n(&g_overflowCount, count + 1, __ATOMIC_RELEASE);
    heapLog("heap: heaps full: overflow heap %d of %d, %zu MiB of regular flexible memory at %p\n", slot + 1,
            g_opt.max_overflow_heaps, size / MB, base);
    return true;
}

bool addOverflowHeap(int seen) {
    g_overflowLock.lock();
    const int count = __atomic_load_n(&g_overflowCount, __ATOMIC_ACQUIRE);
    int slot = -1;
    for (int i = 0; i < count && slot < 0; i++)
        if (!__atomic_load_n(&g_overflow[i], __ATOMIC_ACQUIRE))
            slot = i;  // a released heap's place
    if (slot < 0 && count < g_opt.max_overflow_heaps)
        slot = count;
    bool added = count != seen;  // another thread added one meanwhile: try that first
    if (!added && slot >= 0 && !g_overflowExhausted && g_opt.regular_only) {
        if (!(added = addRegularOverflowHeap(slot, count))) {
            g_overflowExhausted = true;
            heapLog("heap: heaps full and no regular flexible memory for another overflow heap\n");
        }
    } else if (!added && slot >= 0 && !g_overflowExhausted) {
        // (overflow_direct_max and 3/4, 1/2, 1/4 of it; 16 MiB at least, and at least what did not fit)
        const size_t top = g_opt.overflow_direct_max < 16 * MB ? 16 * MB : g_opt.overflow_direct_max;
        const size_t need = (t_want + MB + 2 * MB - 1) & ~(2 * MB - 1);
        size_t sizes[] = {top, top / 4 * 3, top / 2, top / 4};
        for (size_t& size : sizes) {
            size = (size + 2 * MB - 1) & ~(2 * MB - 1);
            if (size < 16 * MB) size = 16 * MB;
            if (size < need) size = need;
        }
        size_t largest = 0;
        for (size_t size : sizes) {
            off_t largestStart = 0;
            largest = 0;
            ps4k_sceKernelAvailableDirectMemorySize(0, ps4k_sceKernelGetDirectMemorySize(), 2 * MB, &largestStart,
                                                    &largest);
            if (largest < size + g_opt.overflow_direct_reserve)
                continue;
            off_t phys = 0;
            if (ps4k_sceKernelAllocateDirectMemory(0, ps4k_sceKernelGetDirectMemorySize(), size, 2 * MB, 0, &phys) != 0)
                continue;
            // A hint well above 0x200000000, where the kernel library puts its own objects; 1 GiB apart
            void* base = reinterpret_cast<void*>(0x1800000000ull + static_cast<uint64_t>(slot) * 0x40000000ull);
            const int32_t ret = ps4k_sceKernelMapDirectMemory(&base, size, PROT_CPU_RW, 0, phys, 2 * MB);
            if (ret != 0 || reinterpret_cast<uintptr_t>(base) < 0x400000000ull) {
                heapLog("heap: overflow heap: mapping %zu MiB of direct memory failed (0x%x, %p)\n", size / MB, ret,
                        base);
                if (ret == 0)
                    ps4k_sceKernelMunmap(base, size);
                ps4k_sceKernelReleaseDirectMemory(phys, size);
                continue;
            }
            mspace msp = create_mspace_with_base(base, size, 1);
            if (!msp) {
                ps4k_sceKernelMunmap(base, size);
                ps4k_sceKernelReleaseDirectMemory(phys, size);
                continue;
            }
            g_overflowBase[slot] = reinterpret_cast<uintptr_t>(base);
            g_overflowPhys[slot] = phys;
            g_overflowInUse[slot].store(0);
            g_overflowEmptyTicks[slot] = 0;
            __atomic_store_n(&g_overflowSize[slot], size, __ATOMIC_RELEASE);
            __atomic_store_n(&g_overflow[slot], msp, __ATOMIC_RELEASE);
            if (slot == count)
                __atomic_store_n(&g_overflowCount, count + 1, __ATOMIC_RELEASE);
            heapLog("heap: heaps full: overflow heap %d of %d, %zu MiB of direct memory at %p (largest free "
                    "direct block was %zu MiB)\n",
                    slot + 1, g_opt.max_overflow_heaps, size / MB, base, largest / MB);
            added = true;
            break;
        }
        if (!added) {
            g_overflowExhausted = true;
            heapLog("heap: heaps full and no direct memory to spare for another overflow heap (largest free "
                    "block %zu MiB)\n",
                    largest / MB);
        }
    }
    g_overflowLock.unlock();
    return added;
}

void accountChunk(const void* p, int64_t sign);

// An allocation from the overflow heaps, adding one when they are all full
template <typename F>
void* overflowAlloc(F alloc) {
    if (g_creatingHeap)
        return nullptr;  // a nested allocation while the main heap is made
    for (;;) {
        const int count = __atomic_load_n(&g_overflowCount, __ATOMIC_ACQUIRE);
        for (int i = 0; i < count; i++) {
            if (g_overflowClosing[i].load(std::memory_order_acquire))
                continue;
            g_overflowPins[i].fetch_add(1, std::memory_order_acq_rel);
            void* p = nullptr;
            const mspace msp = __atomic_load_n(&g_overflow[i], __ATOMIC_ACQUIRE);
            if (msp && !g_overflowClosing[i].load(std::memory_order_acquire)) {
                p = alloc(msp);
                countChunk(i, p, +1);
            }
            g_overflowPins[i].fetch_sub(1, std::memory_order_acq_rel);
            if (p)
                return p;
        }
        if (!addOverflowHeap(count))
            return nullptr;
    }
}

bool ownsPointer(const void* p) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    if (address >= g_heapBase && address < g_heapBase + g_heapSize)
        return true;
    return overflowIndex(p) >= 0;
}

// --- Accounting and the heap profiler ------------------------------------------------------------

// Bytes in use in all heaps, for performance lines (a heap that keeps growing is a leak). Each
// thread collects its changes and adds them to the total once they pass 1 MiB either way, so the
// threads don't fight over one counter.
constinit std::atomic<int64_t> g_heapInUse{0};
__attribute__((tls_model("initial-exec"))) thread_local int64_t t_heapDelta = 0;

// Sampling heap profiler (RPCS3), to find what keeps growing (Devil May Cry 4 lost ~70 KiB of heap
// per short-lived thread). About one allocation per 256 KiB allocated - and every larger one - is
// remembered with the code addresses on its stack (its "site") until it is freed; the sites
// holding the most live memory go into the boot trace (symbolize like crash.log).
constexpr int64_t kSampleBytes = 256 * 1024;
constexpr int kSamplePcs = 8;
constexpr int kLiveSlots = 1 << 15;
constexpr int kMaxSites = 32768;   // 2048, then 16384, were both full long before a leak's sites appeared
constexpr int kSiteSlots = 65536;  // hash index into sites
constexpr int kSiteKeyPcs = 4;     // a site is its first 4 addresses (the deeper ones were often stale stack values)
constexpr uintptr_t kTombstone = 1;
struct LiveSample {
    std::atomic<uintptr_t> ptr;
    uint32_t site;
    uint32_t weight_kib;
};
struct Site {
    uint64_t pcs[kSamplePcs];
    std::atomic<int64_t> live_kib;
    std::atomic<uint32_t> samples;
};
// Mapped when the profiler is first turned on (zero-filled by the kernel): ~3.5 MiB that would
// otherwise sit in the eboot's .bss, which comes out of the app's flexible memory.
struct ProfilerTables {
    LiveSample live[kLiveSlots];
    Site sites[kMaxSites];
    int siteIndex[kSiteSlots];  // site + 1, 0 = empty
    int siteCount;
};
constinit std::atomic<ProfilerTables*> g_prof{nullptr};
constinit std::atomic<bool> g_profilerOn{false};
constinit SleepLock g_siteLock;
__attribute__((tls_model("initial-exec"))) thread_local int64_t t_sampleCountdown = kSampleBytes;
__attribute__((tls_model("initial-exec"))) thread_local bool t_inSampler = false;

uint32_t liveHash(uintptr_t p) {
    return static_cast<uint32_t>((p >> 4) * 0x9E3779B97F4A7C15ull >> 49) & (kLiveSlots - 1);
}

void recordSample(ProfilerTables* t, const void* p, uint32_t weight_kib) {
    // the code addresses on this stack, nearest first (no frame pointers: a scan, like logStack)
    uint64_t pcs[kSamplePcs] = {};
    const uintptr_t base = reinterpret_cast<uintptr_t>(__text_start), end = codeEnd();
    const auto* stack = static_cast<const uintptr_t*>(__builtin_frame_address(0));
    const int words = stackWords(stack, 768);
    int found = 0;
    for (int i = 0; i < words && found < kSamplePcs; i++)
        if (stack[i] >= base && stack[i] < end)
            pcs[found++] = stack[i] - base;
    g_siteLock.lock();
    // the site: found through a hash of its addresses (a linear search of 16,384 sites per sample
    // was too slow)
    uint64_t hash = 0xcbf29ce484222325ull;
    for (int k = 0; k < kSiteKeyPcs; k++)
        hash = (hash ^ pcs[k]) * 0x100000001b3ull;
    int site = -1;
    for (uint32_t probe = 0, h = static_cast<uint32_t>(hash >> 40) & (kSiteSlots - 1); probe < kSiteSlots;
         probe++, h = (h + 1) & (kSiteSlots - 1)) {
        const int index = t->siteIndex[h] - 1;
        if (index < 0) {
            if (t->siteCount < kMaxSites) {
                site = t->siteCount;
                memcpy(t->sites[site].pcs, pcs, sizeof(pcs));
                t->siteIndex[h] = site + 1;
                __atomic_store_n(&t->siteCount, site + 1, __ATOMIC_RELEASE);
            }
            break;
        }
        if (memcmp(t->sites[index].pcs, pcs, sizeof(pcs[0]) * kSiteKeyPcs) == 0) {
            site = index;
            break;
        }
    }
    g_siteLock.unlock();
    if (site < 0)
        return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(p);
    for (uint32_t probe = 0, h = liveHash(key); probe < 16; probe++, h = (h + 1) & (kLiveSlots - 1)) {
        uintptr_t expected = t->live[h].ptr.load(std::memory_order_relaxed);
        if ((expected == 0 || expected == kTombstone) &&
            t->live[h].ptr.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
            t->live[h].site = static_cast<uint32_t>(site);
            t->live[h].weight_kib = weight_kib;
            t->sites[site].live_kib.fetch_add(weight_kib, std::memory_order_relaxed);
            t->sites[site].samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void forgetSample(ProfilerTables* t, const void* p) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(p);
    for (uint32_t probe = 0, h = liveHash(key); probe < 16; probe++, h = (h + 1) & (kLiveSlots - 1)) {
        const uintptr_t value = t->live[h].ptr.load(std::memory_order_acquire);
        if (value == 0)
            return;
        if (value == key) {
            const uint32_t site = t->live[h].site, weight = t->live[h].weight_kib;
            uintptr_t expected = key;
            if (t->live[h].ptr.compare_exchange_strong(expected, kTombstone, std::memory_order_acq_rel))
                t->sites[site].live_kib.fetch_sub(weight, std::memory_order_relaxed);
            return;
        }
    }
}

void sampleChunk(const void* p, size_t size, int64_t sign) {
    ProfilerTables* t = g_prof.load(std::memory_order_acquire);
    if (!t || t_inSampler)
        return;
    t_inSampler = true;
    if (sign < 0) {
        forgetSample(t, p);  // also after the profiler is turned off: its sites stay right
    } else if (g_profilerOn.load(std::memory_order_relaxed)) {
        if (static_cast<int64_t>(size) >= kSampleBytes) {
            recordSample(t, p, static_cast<uint32_t>(size >> 10));
        } else if ((t_sampleCountdown -= static_cast<int64_t>(size)) <= 0) {
            t_sampleCountdown += kSampleBytes;
            recordSample(t, p, static_cast<uint32_t>(kSampleBytes >> 10));
        }
    }
    t_inSampler = false;
}

// The sites holding the most sampled live memory
void reportHeapProfile() {
    ProfilerTables* t = g_prof.load(std::memory_order_acquire);
    if (!t)
        return;
    int order[8];
    int n = 0;
    const int count = __atomic_load_n(&t->siteCount, __ATOMIC_ACQUIRE);
    int64_t total = 0;
    for (int i = 0; i < count; i++)
        total += t->sites[i].live_kib.load(std::memory_order_relaxed);
    heapLog("heap profile: %lld MiB live in sampled allocations, %d sites, heap total %lld MiB\n",
            static_cast<long long>(total >> 10), count,
            static_cast<long long>(g_heapInUse.load(std::memory_order_relaxed) >> 20));
    for (int i = 0; i < count; i++) {
        const int64_t live = t->sites[i].live_kib.load(std::memory_order_relaxed);
        if (live < 4096)
            continue;  // under ~4 MiB
        int pos = n < 8 ? n++ : 8;
        while (pos > 0 && t->sites[order[pos - 1]].live_kib.load(std::memory_order_relaxed) < live) {
            if (pos < 8)
                order[pos] = order[pos - 1];
            pos--;
        }
        if (pos < 8)
            order[pos] = i;
    }
    for (int k = 0; k < n; k++) {
        const Site& s = t->sites[order[k]];
        heapLog("heap profile: %lld MiB live (%u samples) at elf 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx "
                "0x%llx\n",
                static_cast<long long>(s.live_kib.load() >> 10), s.samples.load(),
                static_cast<unsigned long long>(s.pcs[0]), static_cast<unsigned long long>(s.pcs[1]),
                static_cast<unsigned long long>(s.pcs[2]), static_cast<unsigned long long>(s.pcs[3]),
                static_cast<unsigned long long>(s.pcs[4]), static_cast<unsigned long long>(s.pcs[5]),
                static_cast<unsigned long long>(s.pcs[6]), static_cast<unsigned long long>(s.pcs[7]));
    }
}

// A thread's last, unreported change goes into the total when it ends. Without this the total
// drifted upwards by up to 1 MiB per ended thread (frees of other threads' memory, never
// reported) - with Devil May Cry 4's thousands of short-lived threads it read ~1 GiB while far
// less was in use.
pthread_key_t g_heapDeltaKey;
constinit std::atomic<int> g_heapDeltaKeyState{0};  // 0 none, 1 creating, 2 ready
__attribute__((tls_model("initial-exec"))) thread_local bool t_heapDeltaRegistered = false;
// Set once the exit flush has run: frees made after it (later thread-exit destructors) go straight
// into the total. Kept in the delta they were lost, and the total climbed ~0.5 MiB/s in DMC4's
// gameplay (hundreds of short-lived sound stream threads) while sampled memory stayed flat.
__attribute__((tls_model("initial-exec"))) thread_local bool t_heapDeltaFlushed = false;

void flushHeapDelta(void*) {
    g_heapInUse.fetch_add(t_heapDelta, std::memory_order_relaxed);
    t_heapDelta = 0;
    t_heapDeltaFlushed = true;
}

void registerHeapDelta() {
    t_heapDeltaRegistered = true;  // first: pthread_setspecific may allocate (and come back here)
    int state = 0;
    if (g_heapDeltaKeyState.compare_exchange_strong(state, 1)) {
        pthread_key_create(&g_heapDeltaKey, flushHeapDelta);
        g_heapDeltaKeyState.store(2, std::memory_order_release);
    }
    if (g_heapDeltaKeyState.load(std::memory_order_acquire) == 2)
        pthread_setspecific(g_heapDeltaKey, reinterpret_cast<void*>(1));
}

void accountChunk(const void* p, int64_t sign) {
    if (!p)
        return;
    if (!t_heapDeltaRegistered && !g_creatingHeap)
        registerHeapDelta();
    const size_t usable = mspace_usable_size(p);
    sampleChunk(p, usable, sign);
    t_heapDelta += sign * static_cast<int64_t>(usable);
    if (t_heapDeltaFlushed || t_heapDelta > static_cast<int64_t>(MB) || t_heapDelta < -static_cast<int64_t>(MB)) {
        g_heapInUse.fetch_add(t_heapDelta, std::memory_order_relaxed);
        t_heapDelta = 0;
    }
}

// Frees a chunk of any heap (FOOTERS finds its heap), keeping the overflow heaps' counts
void freeChunk(void* ptr) {
    accountChunk(ptr, -1);
    const int i = pinChunk(ptr);
    countChunk(i, ptr, -1);
    mspace_free(g_heap, ptr);
    unpin(i);
}

// Releases the overflow heaps that held nothing at the last two checks
void trimOverflowHeaps() {
    const int count = __atomic_load_n(&g_overflowCount, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        if (!__atomic_load_n(&g_overflow[i], __ATOMIC_ACQUIRE))
            continue;
        if (g_overflowInUse[i].load(std::memory_order_acquire) > 0) {
            g_overflowEmptyTicks[i] = 0;
            continue;
        }
        if (++g_overflowEmptyTicks[i] < 2)
            continue;
        g_overflowLock.lock();
        g_overflowClosing[i].store(true, std::memory_order_seq_cst);
        Backoff backoff;
        while (g_overflowPins[i].load(std::memory_order_acquire) != 0)
            backoff.wait();
        if (g_overflowInUse[i].load(std::memory_order_acquire) == 0) {
            const size_t size = g_overflowSize[i];
            __atomic_store_n(&g_overflow[i], static_cast<mspace>(nullptr), __ATOMIC_RELEASE);
            __atomic_store_n(&g_overflowSize[i], static_cast<size_t>(0), __ATOMIC_RELEASE);
            ps4k_sceKernelMunmap(reinterpret_cast<void*>(g_overflowBase[i]), size);
            if (g_overflowPhys[i] != -1)  // (-1: regular flexible memory, gone with the unmap)
                ps4k_sceKernelReleaseDirectMemory(g_overflowPhys[i], size);
            g_overflowExhausted = false;
            heapLog("heap: overflow heap %d empty: its %zu MiB of %s memory given back\n", i + 1, size / MB,
                    g_overflowPhys[i] != -1 ? "direct" : "regular flexible");
        }
        g_overflowEmptyTicks[i] = 0;
        g_overflowClosing[i].store(false, std::memory_order_release);
        g_overflowLock.unlock();
    }
}

// --- Per-thread arenas -------------------------------------------------------------------------------
//
// One locked heap made Dolphin's busy threads (CPU, video, Vulkan submission, shader compilers)
// contend: in Ultimate Spider-Man the video thread spent ~9% (likely more) of its time waiting for
// it, and until Crema 1.51 every Cemu thread shared it and the recompiler (allocation heavy) took
// seconds per function. Each thread the app lists (ps4_kit_set_arena_threads) gets a private
// dlmalloc arena carved out of the main heap; with FOOTERS every chunk records its arena, so
// free/realloc from any thread return memory to the right one. A full arena falls back to the main
// heap. (Main heap only: an arena never leaves its heap, so one in an overflow heap would keep that
// heap from ever being given back.)
//
// Big arenas for compiler threads: RPCS3's LLVM threads (PPU modules, SPU functions) filled 10 MiB
// at once and were back on the one locked heap, 7 cores waiting on each other (the PS3 firmware took
// ~40 min to compile).
constexpr int MAX_PATTERNS = 64;
const char* g_arenaNames[MAX_PATTERNS];
int g_arenaNameCount = 0;
const char* g_bigArenaNames[MAX_PATTERNS];
int g_bigArenaNameCount = 0;

__attribute__((tls_model("initial-exec"))) thread_local mspace t_arena = nullptr;
constinit std::atomic<int> g_arenaCount{0};
// Arenas of threads that have exited, for the next listed thread (Dolphin's audio thread is
// recreated every time the in-game menu closes, the shader compilers when their count changes,
// RPCS3's module compilers are a new thread per module): without reuse each restart took a fresh
// arena until they ran out.
mspace g_freeArenas[MAX_ARENAS_CAP];
int g_freeArenaCount = 0;
mspace g_bigArenas[MAX_BIG_ARENAS_CAP];
constinit std::atomic<int> g_bigArenaCount{0};
mspace g_freeBigArenas[MAX_BIG_ARENAS_CAP];
int g_freeBigArenaCount = 0;
constinit SleepLock g_freeArenaLock;
pthread_key_t g_arenaKey;
constinit std::atomic<int> g_arenaKeyState{0};  // 0 none, 1 creating, 2 ready

bool isBigArena(const void* arena) {
    const int loaded = g_bigArenaCount.load(std::memory_order_acquire);
    const int count = loaded < MAX_BIG_ARENAS_CAP ? loaded : MAX_BIG_ARENAS_CAP;
    for (int i = 0; i < count; i++)
        if (g_bigArenas[i] == arena)
            return true;
    return false;
}

// Thread exit (pthread key destructor): its arena goes back on the free list. Chunks other
// threads still hold stay valid - FOOTERS returns them to this arena when freed.
void releaseArena(void* arena) {
    if (!arena)
        return;
    g_freeArenaLock.lock();
    if (isBigArena(arena)) {
        if (g_freeBigArenaCount < MAX_BIG_ARENAS_CAP)
            g_freeBigArenas[g_freeBigArenaCount++] = static_cast<mspace>(arena);
    } else if (g_freeArenaCount < MAX_ARENAS_CAP) {
        g_freeArenas[g_freeArenaCount++] = static_cast<mspace>(arena);
    }
    g_freeArenaLock.unlock();
}

bool arenaKeyReady() {
    int state = g_arenaKeyState.load(std::memory_order_acquire);
    if (state == 2)
        return true;
    state = 0;
    if (g_arenaKeyState.compare_exchange_strong(state, 1)) {
        const bool ok = pthread_key_create(&g_arenaKey, releaseArena) == 0;
        g_arenaKeyState.store(ok ? 2 : 0, std::memory_order_release);
        return ok;
    }
    Backoff backoff;
    while ((state = g_arenaKeyState.load(std::memory_order_acquire)) == 1)
        backoff.wait();
    return state == 2;
}

bool nameListed(const char* name, const char* const* list, int count) {
    for (int i = 0; i < count; i++) {
        const char* pattern = list[i];
        const size_t n = strlen(pattern);
        if (n && pattern[n - 1] == '*') {
            if (strncmp(name, pattern, n - 1) == 0)
                return true;
        } else if (strcmp(name, pattern) == 0) {
            return true;
        }
    }
    return false;
}

bool newBigArena(const char* name) {
    const int index = g_bigArenaCount.load(std::memory_order_acquire);
    if (index >= g_opt.max_big_arenas)
        return false;
    void* block = mspace_malloc(g_heap, g_opt.big_arena_size);
    if (!block)
        return false;
    int slot = -1;
    g_freeArenaLock.lock();
    slot = g_bigArenaCount.load(std::memory_order_relaxed);
    if (slot < g_opt.max_big_arenas) {
        t_arena = create_mspace_with_base(block, g_opt.big_arena_size, 1);
        if (t_arena) {
            g_bigArenas[slot] = t_arena;
            g_bigArenaCount.store(slot + 1, std::memory_order_release);
        }
    }
    g_freeArenaLock.unlock();
    if (!t_arena) {
        mspace_free(g_heap, block);
        return false;
    }
    heapLog("heap: big arena %d of %d (%zu MiB) for '%s'\n", slot + 1, g_opt.max_big_arenas,
            g_opt.big_arena_size / MB, name);
    return true;
}

void giveArena(const char* name, bool big) {
    if (t_arena || !getHeap() || !arenaKeyReady())
        return;
    if (big) {
        // An exited compiler's arena first (quietly: a new thread per module), else a new one
        // while there are fewer than max_big_arenas; past that, a normal arena
        g_freeArenaLock.lock();
        if (g_freeBigArenaCount > 0)
            t_arena = g_freeBigArenas[--g_freeBigArenaCount];
        g_freeArenaLock.unlock();
        if (t_arena || newBigArena(name)) {
            pthread_setspecific(g_arenaKey, t_arena);
            return;
        }
    }
    // An exited thread's arena first.
    g_freeArenaLock.lock();
    if (g_freeArenaCount > 0)
        t_arena = g_freeArenas[--g_freeArenaCount];
    g_freeArenaLock.unlock();
    if (t_arena) {
        pthread_setspecific(g_arenaKey, t_arena);
        heapLog("heap: reused arena for '%s'\n", name);
        return;
    }
    if (g_arenaCount.fetch_add(1) >= g_opt.max_arenas)
        return;
    void* block = mspace_malloc(g_heap, g_opt.arena_size);
    if (!block)
        return;
    t_arena = create_mspace_with_base(block, g_opt.arena_size, 1);
    if (t_arena)
        pthread_setspecific(g_arenaKey, t_arena);
    else
        mspace_free(g_heap, block);
    heapLog("heap: private %zu MiB arena for '%s'%s\n", g_opt.arena_size / MB, name, t_arena ? "" : " FAILED");
}

// Automatic arenas (ps4_kit_heap_options::auto_arena_waits): ps4_dlmalloc.c counts, per thread, the
// heap locks it had to wait for. __thread, not thread_local: no C++ TLS wrapper, whose weak init
// symbol would become an import create-fself refuses.
extern "C" __thread unsigned ps4_heap_lock_waits __attribute__((tls_model("initial-exec")));
constinit std::atomic<int> g_autoArenaCount{0};
__attribute__((tls_model("initial-exec"))) thread_local bool t_autoTried = false;

void giveAutoArena() {
    t_autoTried = true;  // once per thread, whatever happens
    if (t_arena || !getHeap() || !arenaKeyReady())
        return;
    g_freeArenaLock.lock();
    if (g_freeArenaCount > 0)
        t_arena = g_freeArenas[--g_freeArenaCount];
    g_freeArenaLock.unlock();
    if (!t_arena) {
        const int index = g_autoArenaCount.fetch_add(1);
        if (index >= g_opt.max_auto_arenas)
            return;
        void* block = mspace_malloc(g_heap, g_opt.auto_arena_size);
        if (!block)
            return;
        t_arena = create_mspace_with_base(block, g_opt.auto_arena_size, 1);
        if (!t_arena) {
            mspace_free(g_heap, block);
            return;
        }
        heapLog("heap: automatic %zu MiB arena %d of %d for a thread that waited %u times for a heap lock\n",
                g_opt.auto_arena_size / MB, index + 1, g_opt.max_auto_arenas, ps4_heap_lock_waits);
    }
    pthread_setspecific(g_arenaKey, t_arena);
}

// --- Checks and reports ------------------------------------------------------------------------------

// The operation in progress, for error reports (thread-local: allocations happen on any thread).
// initial-exec: the executable's own TLS, so access never goes through __tls_get_addr.
__attribute__((tls_model("initial-exec"))) thread_local const char* t_op = "?";
__attribute__((tls_model("initial-exec"))) thread_local const void* t_caller = nullptr;
int g_errors = 0;

// Checks the heap's control block before every operation; the first time it is found
// overwritten, logs when (operation number, current and previous caller) and what it now holds.
// (Per thread: shared counters written by every allocation made the cores fight over one cache
// line, Crema 1.50 and before.)
__attribute__((tls_model("initial-exec"))) thread_local unsigned long long t_ops = 0;
__attribute__((tls_model("initial-exec"))) thread_local const char* t_prevOp = "none";
__attribute__((tls_model("initial-exec"))) thread_local const void* t_prevCaller = nullptr;
bool g_heapBroken = false;

void checkHeap(const char* op, const void* caller) {
    t_ops++;
    if (g_heap && !g_heapBroken && !ps4_heap_ok(g_heap)) {
        g_heapBroken = true;
        heapLog("heap: CONTROL BLOCK OVERWRITTEN, noticed at operation #%llu of this thread (%s from elf 0x%llx); "
                "previous operation %s from elf 0x%llx\n",
                t_ops, op, elf(caller), t_prevOp, elf(t_prevCaller));
        const auto* words = reinterpret_cast<const unsigned long long*>(g_heapBase);
        for (int i = 0; i < 16; i += 4)
            heapLog("heap:   +0x%02x: %016llx %016llx %016llx %016llx\n", i * 8, words[i], words[i + 1], words[i + 2],
                    words[i + 3]);
        logStack();
    }
    t_prevOp = op;
    t_prevCaller = caller;
}

void reportFailure(const char* what, size_t size) {
    if (g_errors++ < 8) {
        heapLog("heap: %s(%zu) failed, caller elf 0x%llx\n", what, size, elf(t_caller));
        logStack();
    }
}

// An allocation: this thread's arena, else the main heap, else the overflow heaps.
template <typename F>
void* allocate(F alloc) {
    void* p = t_arena ? alloc(t_arena) : nullptr;
    if (!p) {
        if (!t_arena && !t_autoTried && g_opt.auto_arena_waits > 0 &&
            ps4_heap_lock_waits >= static_cast<unsigned>(g_opt.auto_arena_waits)) {
            giveAutoArena();
            if (t_arena && (p = alloc(t_arena)))
                return p;
        }
        if (mspace heap = getHeap())
            p = alloc(heap);
    }
    if (!p)
        p = overflowAlloc(alloc);
    return p;
}

struct ThreadDtor {
    void (*dtor)(void*);
    void* obj;
    ThreadDtor* next;
};
pthread_key_t g_threadDtorKey;
pthread_once_t g_threadDtorOnce = PTHREAD_ONCE_INIT;

void runThreadDtors(void* p) {
    auto* head = static_cast<ThreadDtor*>(p);
    while (head) {
        ThreadDtor* next = head->next;
        head->dtor(head->obj);
        free(head);
        head = next;
    }
}

void createThreadDtorKey() { pthread_key_create(&g_threadDtorKey, runThreadDtors); }

// Maps zero-filled read/write memory outside the heaps (the profiler's tables): system flexible
// memory, else a plain mmap.
void* mapRuntimeMemory(size_t size) {
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (void* base = g_opt.regular_only ? nullptr : reserveAwayFromKernel(size)) {
        if (ps4k_sceKernelMapNamedSystemFlexibleMemory(&base, size, PROT_CPU_RW, MAP_FIXED_FLAG, g_nameMmap) == 0)
            return base;
        ps4k_sceKernelMunmap(base, size);
    }
    void* p = __real_mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

}  // namespace

extern "C" {

// --- The malloc family -------------------------------------------------------------------------------

// dlmalloc found a corrupted heap (corruption = 1) or was asked to free/realloc something it
// didn't allocate (corruption = 0).
void ps4_heap_error(void* msp, void* chunk, int corruption) {
    (void)msp;
    heapLog("heap: %s during %s from elf 0x%llx (chunk %p)\n", corruption ? "CORRUPTION DETECTED" : "invalid pointer",
            t_op, elf(t_caller), chunk);
    logStack();
    if (corruption)
        abort();
}

// How deep the thread is in the heap's functions (waiting for a lock counts): for thread dumps,
// read on the thread itself from a signal handler (ps4_heap_depth). Freezes where every thread that
// allocates stood still (WinePS4 v00.63) need to know which was inside.
__attribute__((tls_model("initial-exec"))) thread_local int t_heapDepth = 0;
struct HeapDepth {
    HeapDepth() { t_heapDepth++; }
    ~HeapDepth() { t_heapDepth--; }
};

int ps4_heap_depth(void) { return t_heapDepth; }

void* __wrap_malloc(size_t size) {
    HeapDepth depth;
    t_op = "malloc";
    t_caller = __builtin_return_address(0);
    t_want = size;
    checkHeap(t_op, t_caller);
    void* p = allocate([&](mspace m) { return mspace_malloc(m, size); });
    if (!p)
        reportFailure("malloc", size);
    accountChunk(p, +1);
    return p;
}

void __wrap_free(void* ptr) {
    if (!ptr)
        return;
    HeapDepth depth;
    t_op = "free";
    t_caller = __builtin_return_address(0);
    checkHeap(t_op, t_caller);
    if (!ownsPointer(ptr)) {
        if (g_errors++ < 8) {
            heapLog("heap: free(%p) of memory not from this heap, caller elf 0x%llx (ignored)\n", ptr, elf(t_caller));
            logStack();
        }
        return;
    }
    freeChunk(ptr);
}

void* __wrap_calloc(size_t nelem, size_t size) {
    HeapDepth depth;
    t_op = "calloc";
    t_caller = __builtin_return_address(0);
    t_want = nelem * size;
    checkHeap(t_op, t_caller);
    void* p = allocate([&](mspace m) { return mspace_calloc(m, nelem, size); });
    if (!p)
        reportFailure("calloc", nelem * size);
    accountChunk(p, +1);
    return p;
}

void* __wrap_realloc(void* ptr, size_t size) {
    HeapDepth depth;
    t_op = "realloc";
    t_caller = __builtin_return_address(0);
    t_want = size;
    checkHeap(t_op, t_caller);
    if (ptr && !ownsPointer(ptr)) {
        heapLog("heap: realloc(%p) of memory not from this heap, caller elf 0x%llx\n", ptr, elf(t_caller));
        logStack();
        return nullptr;
    }
    if (ptr && size == 0) {
        freeChunk(ptr);
        return nullptr;
    }
    // With FOOTERS this grows the chunk inside the arena it came from (the mspace argument is
    // only used for new allocations); when that arena is full, move it to this thread's. dlmalloc
    // keeps a reallocated chunk in its own heap: an overflow heap's count moves with it.
    void* p = nullptr;
    if (ptr) {
        const int i = pinChunk(ptr);
        accountChunk(ptr, -1);
        countChunk(i, ptr, -1);
        p = mspace_realloc(g_heap, ptr, size);
        countChunk(i, p ? p : ptr, +1);
        accountChunk(p ? p : ptr, +1);
        unpin(i);
    }
    if (!p) {
        p = allocate([&](mspace m) { return mspace_malloc(m, size); });
        accountChunk(p, +1);
        if (p && ptr) {
            const size_t old_size = mspace_usable_size(ptr);
            memcpy(p, ptr, old_size < size ? old_size : size);
            freeChunk(ptr);
        }
    }
    if (!p)
        reportFailure("realloc", size);
    return p;
}

void* __wrap_memalign(size_t alignment, size_t size) {
    HeapDepth depth;
    t_op = "memalign";
    t_caller = __builtin_return_address(0);
    t_want = size + alignment;
    checkHeap(t_op, t_caller);
    void* p = allocate([&](mspace m) { return mspace_memalign(m, alignment, size); });
    if (!p)
        reportFailure("memalign", size);
    accountChunk(p, +1);
    return p;
}

// posix_memalign and aligned_alloc in libc.a are built on __memalign.
void* __wrap___memalign(size_t alignment, size_t size) { return __wrap_memalign(alignment, size); }

// musl's malloc_usable_size would read our chunks as its own (optional wrap: nothing in the ports
// called it, but a library that does would get garbage).
size_t __wrap_malloc_usable_size(void* ptr) { return ptr && ownsPointer(ptr) ? mspace_usable_size(ptr) : 0; }

// --- Arenas --------------------------------------------------------------------------------------------

void ps4_kit_set_arena_threads(const char* const* names, int count) {
    count = clampInt(count, 0, MAX_PATTERNS);
    for (int i = 0; i < count; i++)
        g_arenaNames[i] = names[i];
    __atomic_store_n(&g_arenaNameCount, count, __ATOMIC_RELEASE);
}

void ps4_kit_set_big_arena_threads(const char* const* names, int count) {
    count = clampInt(count, 0, MAX_PATTERNS);
    for (int i = 0; i < count; i++)
        g_bigArenaNames[i] = names[i];
    __atomic_store_n(&g_bigArenaNameCount, count, __ATOMIC_RELEASE);
}

// Gives the calling thread a private arena if the app listed its name (called when a thread names
// itself: SetThreadName -> ps4_watch_thread -> here).
void ps4_heap_private_arena(const char* name) {
    if (t_arena || !name)
        return;
    const bool big = nameListed(name, g_bigArenaNames, __atomic_load_n(&g_bigArenaNameCount, __ATOMIC_ACQUIRE));
    if (big || nameListed(name, g_arenaNames, __atomic_load_n(&g_arenaNameCount, __ATOMIC_ACQUIRE)))
        giveArena(name, big);
}

int ps4_kit_private_arena(int big) {
    giveArena("(requested)", big != 0);
    return t_arena != nullptr;
}

void ps4_kit_set_auto_arena_waits(int waits) {
    __atomic_store_n(&g_opt.auto_arena_waits, waits < 0 ? 0 : waits, __ATOMIC_RELAXED);
}

int ps4_kit_auto_arena_count(void) {
    const int n = g_autoArenaCount.load(std::memory_order_relaxed);
    return n < g_opt.max_auto_arenas ? n : g_opt.max_auto_arenas;
}

// --- System libc malloc replacement --------------------------------------------------------------
//
// ps4_crt1.S, _sceLibcMallocReplace: the system libc forwards every malloc of the process here -
// Sony's system modules too (Piglet, its shader compiler) - instead of serving it from its own
// ~12.7 MiB heap, which they exhausted.
int ps4_replace_malloc_init(void) {
    getHeap();
    return 0;
}
int ps4_replace_malloc_finalize(void) { return 0; }
void* ps4_replace_malloc(size_t size) { return __wrap_malloc(size); }
void ps4_replace_free(void* ptr) { __wrap_free(ptr); }
void* ps4_replace_calloc(size_t nelem, size_t size) { return __wrap_calloc(nelem, size); }
void* ps4_replace_realloc(void* ptr, size_t size) { return __wrap_realloc(ptr, size); }
void* ps4_replace_memalign(size_t boundary, size_t size) { return __wrap_memalign(boundary, size); }
void* ps4_replace_reallocalign(void* ptr, size_t size, size_t boundary) {
    if (!ptr)
        return __wrap_memalign(boundary, size);
    if (size == 0) {
        __wrap_free(ptr);
        return nullptr;
    }
    if (!ownsPointer(ptr))
        return nullptr;
    void* p = __wrap_memalign(boundary, size);
    if (!p)
        return nullptr;
    const size_t old_size = mspace_usable_size(ptr);
    memcpy(p, ptr, old_size < size ? old_size : size);
    __wrap_free(ptr);
    return p;
}
int ps4_replace_posix_memalign(void** out, size_t boundary, size_t size) {
    void* p = __wrap_memalign(boundary, size);
    if (!p)
        return 12;  // ENOMEM
    *out = p;
    return 0;
}
// SceLibcMallocManagedSize: u16 size, u16 version, u32 reserved, then 4 sizes. Report the heaps.
struct MallocManagedSize {
    uint16_t size;
    uint16_t version;
    uint32_t reserved;
    size_t max_system_size, current_system_size, max_inuse_size, current_inuse_size;
};
int ps4_replace_malloc_stats(void* out) {
    auto* stats = static_cast<MallocManagedSize*>(out);
    if (!stats)
        return 0x80020016;  // EINVAL
    mspace heap = getHeap();
    size_t footprint = heap ? mspace_footprint(heap) : 0;
    size_t maxFootprint = heap ? mspace_max_footprint(heap) : 0;
    const int count = __atomic_load_n(&g_overflowCount, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        g_overflowPins[i].fetch_add(1, std::memory_order_acq_rel);
        if (mspace overflow = __atomic_load_n(&g_overflow[i], __ATOMIC_ACQUIRE)) {
            if (!g_overflowClosing[i].load(std::memory_order_acquire)) {
                footprint += mspace_footprint(overflow);
                maxFootprint += mspace_max_footprint(overflow);
            }
        }
        g_overflowPins[i].fetch_sub(1, std::memory_order_acq_rel);
    }
    stats->max_system_size = maxFootprint;
    stats->current_system_size = footprint;
    stats->max_inuse_size = footprint;
    stats->current_inuse_size = footprint;
    return 0;
}
int ps4_replace_malloc_stats_fast(void* out) { return ps4_replace_malloc_stats(out); }
size_t ps4_replace_malloc_usable_size(void* ptr) { return __wrap_malloc_usable_size(ptr); }

// --- Heap diagnostics ----------------------------------------------------------------------------

const char* ps4rt_heap_source(void) {
    getHeap();
    return g_heapPool;
}
size_t ps4rt_heap_size(void) {
    getHeap();
    return g_heapSize;
}
// How much of the heap has been touched: an upper bound of what is in use (freed chunks below the
// top count too).
size_t ps4rt_heap_footprint(void) {
    mspace heap = getHeap();
    return heap ? mspace_footprint(heap) : 0;
}
// The same with the overflow heaps: for a periodic line in the app's boot trace.
size_t ps4_heap_footprint(void) {
    MallocManagedSize stats;
    ps4_replace_malloc_stats(&stats);
    return stats.current_system_size;
}
// The heaps' fill for the periodic report: "heap N of M MiB filled (+ overflow O of P MiB)" -
// how far up each heap has been used (freed chunks below that count: an upper bound).
void ps4rt_heap_report(char* out, size_t size) {
    if (!out || !size)
        return;
    mspace heap = getHeap();
    const size_t filled = heap ? g_heapSize - ps4_heap_top_free(heap) : 0;
    int len = snprintf(out, size, "heap %zu of %zu MiB filled", filled / MB, g_heapSize / MB);
    size_t overflowFilled = 0, overflowSize = 0;
    const int count = __atomic_load_n(&g_overflowCount, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++) {
        g_overflowPins[i].fetch_add(1, std::memory_order_acq_rel);
        mspace overflow = __atomic_load_n(&g_overflow[i], __ATOMIC_ACQUIRE);
        if (overflow && !g_overflowClosing[i].load(std::memory_order_acquire)) {
            overflowFilled += g_overflowSize[i] - ps4_heap_top_free(overflow);
            overflowSize += g_overflowSize[i];
        }
        g_overflowPins[i].fetch_sub(1, std::memory_order_acq_rel);
    }
    if (overflowSize && len > 0 && static_cast<size_t>(len) < size)
        snprintf(out + len, size - len, " (+ overflow %zu of %zu MiB)", overflowFilled / MB, overflowSize / MB);
}

// MiB in use in all heaps (within ~1 MiB per thread) and the overflow heaps mapped now
size_t ps4_heap_in_use_mib(int* overflowHeaps) {
    int mapped = 0;
    const int count = __atomic_load_n(&g_overflowCount, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++)
        mapped += __atomic_load_n(&g_overflow[i], __ATOMIC_ACQUIRE) != nullptr;
    if (overflowHeaps)
        *overflowHeaps = mapped;
    const int64_t bytes = g_heapInUse.load(std::memory_order_relaxed);
    return bytes > 0 ? static_cast<size_t>(bytes) / MB : 0;
}

// Every ~5 s (RPCS3's performance monitor): overflow heaps empty for two calls give their direct
// memory back; every minute, the heap profile (when on).
void ps4_heap_trim(void) {
    trimOverflowHeaps();
    static int s_calls = 0;
    if (++s_calls % 12 == 0 && g_profilerOn.load(std::memory_order_relaxed))
        reportHeapProfile();
}

void ps4_kit_set_heap_profiler(int on) {
    if (on && !g_prof.load(std::memory_order_acquire)) {
        static SleepLock s_mapping;
        s_mapping.lock();
        if (!g_prof.load(std::memory_order_acquire)) {
            auto* tables = static_cast<ProfilerTables*>(mapRuntimeMemory(sizeof(ProfilerTables)));
            if (tables)
                g_prof.store(tables, std::memory_order_release);
            heapLog("heap profile: %s (%zu KiB of tables)\n", tables ? "on" : "no memory for its tables",
                    sizeof(ProfilerTables) / 1024);
        }
        s_mapping.unlock();
    }
    g_profilerOn.store(on && g_prof.load(std::memory_order_acquire), std::memory_order_relaxed);
}

void ps4_heap_profile_report(void) { reportHeapProfile(); }

}  // extern "C"

// --- Executable memory -------------------------------------------------------------------------------
//
// The JIT's / recompiler's code blocks. Jailbreaks differ in what they let a homebrew app map as
// code it writes itself: on GoldHEN 12.00/13.52 consoles system flexible memory mapped RWX runs
// code; on a firmware 11.00 console (a PS4 Pro) that mapping succeeded but its pages did not
// execute - every game crashed at its first recompiled block (SIGSEGV code 2 with rip = the block)
// - and a plain read/write/execute mmap was refused there. So each source is tried once with a
// one-instruction function (ret) before it serves executable mappings; a fault there is caught and
// the next source is tried.

extern "C" int ps4_jit_in_system_pool = 1;  // executable mappings from the system pool when it runs code

namespace {

enum ExecSource { kExecSystemPool, kExecRegular, kExecMprotect, kExecFlexible, kExecDirect, kExecJitShm, kExecNone };
const char* const kExecSourceNames[] = {"system flexible memory", "regular flexible memory (mmap)",
                                        "mmap + mprotect",        "named flexible memory",
                                        "direct memory",          "JIT shared memory",
                                        "nothing"};
constexpr int kSigBusFreeBsd = 10, kSigSegvFreeBsd = 11, kSaSiginfoFreeBsd = 0x40;
constexpr int kMcontextOffset = 0x40, kRipIndex = 20, kRspIndex = 23;  // as ps4_crashlog.cpp
int g_execError = 0;  // why the last mapExec failed (errno or SCE error)

volatile uintptr_t g_probeCode = 0;
volatile int g_probeFaulted = 0;
Ps4Sigaction g_oldSegv, g_oldBus;

void execProbeHandler(int, void*, void* context) {
    auto* regs = reinterpret_cast<uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
    if (g_probeCode && regs[kRipIndex] == g_probeCode) {
        // as if the ret ran: back to the caller
        regs[kRipIndex] = *reinterpret_cast<const uint64_t*>(regs[kRspIndex]);
        regs[kRspIndex] += 8;
        g_probeFaulted = 1;
        return;
    }
    // another fault: the previous handlers get it when the instruction runs again
    ps4k_sigaction(kSigSegvFreeBsd, &g_oldSegv, nullptr);
    ps4k_sigaction(kSigBusFreeBsd, &g_oldBus, nullptr);
}

// Whether code placed at `exec` runs; the ret is written through `write` (the same memory, or a
// writable alias of it).
bool runsCode(void* exec, void* write) {
    static_cast<volatile uint8_t*>(write)[0] = 0xC3;  // ret
    Ps4Sigaction action;
    memset(&action, 0, sizeof(action));
    action.handler = execProbeHandler;
    action.flags = kSaSiginfoFreeBsd;
    g_probeFaulted = 0;
    g_probeCode = reinterpret_cast<uintptr_t>(exec);
    ps4k_sigaction(kSigSegvFreeBsd, &action, &g_oldSegv);
    ps4k_sigaction(kSigBusFreeBsd, &action, &g_oldBus);
    void (*volatile function)() = reinterpret_cast<void (*)()>(exec);
    function();
    ps4k_sigaction(kSigSegvFreeBsd, &g_oldSegv, nullptr);
    ps4k_sigaction(kSigBusFreeBsd, &g_oldBus, nullptr);
    g_probeCode = 0;
    return !g_probeFaulted;
}

// Maps `size` bytes of read/write/execute memory from `source`; *physOut gets direct memory's
// physical offset (to release it after a probe).
void* mapExec(ExecSource source, size_t size, off_t* physOut) {
    g_execError = 0;
    switch (source) {
    case kExecSystemPool:
    case kExecFlexible: {
        void* base = reserveAwayFromKernel(size);
        if (!base) {
            g_execError = -1;
            return nullptr;
        }
        const int32_t ret =
            source == kExecSystemPool
                ? ps4k_sceKernelMapNamedSystemFlexibleMemory(&base, size, PROT_CPU_RWX, MAP_FIXED_FLAG, g_nameCode)
                : ps4k_sceKernelMapNamedFlexibleMemory(&base, size, PROT_CPU_RWX, MAP_FIXED_FLAG, g_nameCode);
        if (ret != 0) {
            g_execError = ret;
            ps4k_sceKernelMunmap(base, size);
            return nullptr;
        }
        return base;
    }
    case kExecRegular:
    case kExecMprotect: {
        void* p = __real_mmap(nullptr, size, source == kExecRegular ? PROT_CPU_RWX : PROT_CPU_RW, MAP_PRIVATE | MAP_ANON,
                              -1, 0);
        if (p == MAP_FAILED) {
            g_execError = errno;
            return nullptr;
        }
        if (source == kExecMprotect) {
            const int32_t ret = ps4k_sceKernelMprotect(p, size, PROT_CPU_RWX);
            if (ret != 0) {
                g_execError = ret;
                ps4k_sceKernelMunmap(p, size);
                return nullptr;
            }
        }
        return p;
    }
    case kExecDirect: {
        // Direct memory isn't given back by munmap: code blocks are kept for the whole run anyway.
        off_t phys = 0;
        int32_t ret = ps4k_sceKernelAllocateDirectMemory(0, ps4k_sceKernelGetDirectMemorySize(), size, PAGE, 0, &phys);
        if (ret != 0) {
            g_execError = ret;
            return nullptr;
        }
        void* base = reserveAwayFromKernel(size);
        ret = base ? ps4k_sceKernelMapDirectMemory(&base, size, PROT_CPU_RWX, MAP_FIXED_FLAG, phys, PAGE) : -1;
        if (ret != 0) {
            g_execError = ret;
            if (base)
                ps4k_sceKernelMunmap(base, size);
            ps4k_sceKernelReleaseDirectMemory(phys, size);
            return nullptr;
        }
        if (physOut)
            *physOut = phys;
        return base;
    }
    case kExecJitShm: {
        int32_t shm = -1;
        const int32_t ret = ps4k_sceKernelJitCreateSharedMemory(nullptr, size, PROT_CPU_RWX, &shm);
        if (ret != 0) {
            g_execError = ret;
            return nullptr;
        }
        void* p = __real_mmap(nullptr, size, PROT_CPU_RWX, MAP_SHARED, shm, 0);
        if (p == MAP_FAILED)
            g_execError = errno;
        ps4k_sceKernelClose(shm);  // the mapping keeps the memory
        return p == MAP_FAILED ? nullptr : p;
    }
    case kExecNone:
        break;
    }
    return nullptr;
}

// Only logged when nothing else runs code: Sony's own way (a read/execute view and a read/write
// alias of the same memory), which a JIT that writes its code where it runs it can't use as is.
void probeJitAlias() {
    int32_t shm = -1, alias = -1;
    int32_t ret = ps4k_sceKernelJitCreateSharedMemory(nullptr, PAGE, PROT_CPU_RWX, &shm);
    if (ret != 0) {
        heapLog("exec: JIT alias: shared memory refused (%#x)\n", static_cast<unsigned>(ret));
        return;
    }
    ret = ps4k_sceKernelJitCreateAliasOfSharedMemory(shm, PROT_CPU_RW, &alias);
    void* exec = ret == 0 ? __real_mmap(nullptr, PAGE, PROT_READ | PROT_EXEC, MAP_SHARED, shm, 0) : MAP_FAILED;
    void* write = ret == 0 ? __real_mmap(nullptr, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, alias, 0) : MAP_FAILED;
    if (ret != 0)
        heapLog("exec: JIT alias: alias refused (%#x)\n", static_cast<unsigned>(ret));
    else if (exec == MAP_FAILED || write == MAP_FAILED)
        heapLog("exec: JIT alias: mapping refused (exec %s, write %s, errno %d)\n", exec == MAP_FAILED ? "no" : "ok",
                write == MAP_FAILED ? "no" : "ok", errno);
    else
        heapLog("exec: JIT alias (separate write and execute views): %s\n", runsCode(exec, write) ? "works" : "faults");
    if (exec != MAP_FAILED)
        ps4k_sceKernelMunmap(exec, PAGE);
    if (write != MAP_FAILED)
        ps4k_sceKernelMunmap(write, PAGE);
    if (alias >= 0)
        ps4k_sceKernelClose(alias);
    ps4k_sceKernelClose(shm);
}

ExecSource chooseExecSource() {
    SceKernelSwVersion version;
    memset(&version, 0, sizeof(version));
    version.size = sizeof(version);
    if (ps4k_sceKernelGetSystemSwVersion(&version) == 0) {
        version.text[sizeof(version.text) - 1] = 0;
        heapLog("firmware %s (0x%08x)\n", version.text, version.version);
    }
    // The system pool first (it keeps regular flexible memory for the GPU driver), then the plain
    // ways, then the ones some jailbreaks allow instead.
    for (int s = ps4_jit_in_system_pool && !g_opt.regular_only ? kExecSystemPool : kExecRegular; s < kExecNone; s++) {
        const ExecSource source = static_cast<ExecSource>(s);
        if (g_opt.regular_only && source != kExecRegular && source != kExecMprotect && source != kExecFlexible)
            continue;
        off_t phys = 0;
        void* code = mapExec(source, PAGE, &phys);
        if (!code) {
            heapLog("executable memory: %s refused (%#x)\n", kExecSourceNames[s], static_cast<unsigned>(g_execError));
            continue;
        }
        const bool ok = runsCode(code, code);
        ps4k_sceKernelMunmap(code, PAGE);
        if (source == kExecDirect)
            ps4k_sceKernelReleaseDirectMemory(phys, PAGE);
        heapLog("executable memory: %s %s\n", kExecSourceNames[s], ok ? "runs code" : "maps, but does not run code");
        if (ok)
            return source;
    }
    probeJitAlias();
    heapLog("executable memory: NONE runs code - this console's jailbreak does not let homebrew run generated code; "
            "a JIT will crash\n");
    return kExecNone;
}

std::atomic<bool> g_execProbed{false};

ExecSource execSource() {
    static const ExecSource source = [] {
        const ExecSource s = chooseExecSource();
        g_execProbed.store(true, std::memory_order_release);
        return s;
    }();
    return source;
}

// --- The program image guard ---------------------------------------------------------------------------
//
// The program's own image (eboot.bin's segments). RPCS3 crashed in Tekken Tag Tournament 2 with the
// program's code no longer executable (fetch faults inside its own threads): something unmapped or
// protected part of it. Every munmap/mprotect is checked against it: unmapping any segment, or
// changing the protection of a read-only one (code, read-only data), is refused and logged with
// its caller. (Writable segments may change protection: guard pages in static buffers.)
struct ImageSegment {
    void* address;
    uint32_t size;
    int32_t prot;
};
struct ImageModuleInfo {
    size_t size;
    char name[256];
    ImageSegment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
};

struct ImageRange {
    uintptr_t lo, hi;
    bool writable;
};
ImageRange g_image[4];
int g_imageCount = 0;
std::atomic<int> g_imageState{0};  // 0 not looked up, 1 looking, 2 known
std::atomic<bool> g_imageGuard{true};

void findImage() {
    int state = 0;
    if (!g_imageState.compare_exchange_strong(state, 1)) {
        Backoff backoff;
        while (g_imageState.load(std::memory_order_acquire) != 2)
            backoff.wait();
        return;
    }
    int found = 0;
    int32_t handles[256];
    size_t count = 0;
    if (ps4k_sceKernelGetModuleList(handles, 256, &count) == 0) {
        for (size_t m = 0; m < count && m < 256 && found == 0; m++) {
            ImageModuleInfo info;
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (ps4k_sceKernelGetModuleInfo(handles[m], &info) != 0 || strcmp(info.name, "eboot.bin") != 0)
                continue;
            for (uint32_t g = 0; g < info.segment_count && g < 4; g++) {
                const uintptr_t a = reinterpret_cast<uintptr_t>(info.segments[g].address);
                g_image[found++] = {a, a + info.segments[g].size, (info.segments[g].prot & 2) != 0};
            }
        }
    }
    if (found == 0) {  // not listed: .text and .rodata (or the next 128 MiB), read-only
        const uintptr_t lo = reinterpret_cast<uintptr_t>(__text_start);
        g_image[found++] = {lo, codeEnd(), false};
    }
    g_imageCount = found;
    g_imageState.store(2, std::memory_order_release);
}

bool hitsImage(const void* address, size_t length, const char* what, int prot, bool readOnlyOnly) {
    if (!g_imageGuard.load(std::memory_order_relaxed))
        return false;
    if (g_imageState.load(std::memory_order_acquire) != 2)
        findImage();
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    for (int i = 0; i < g_imageCount; i++) {
        const ImageRange& r = g_image[i];
        if ((readOnlyOnly && r.writable) || !(start < r.hi && start + length > r.lo))
            continue;
        static std::atomic<int> s_reports{0};
        if (s_reports.fetch_add(1) < 16) {
            heapLog("GUARD: %s(%p, 0x%zx, prot %d) overlaps the program image [%#llx, %#llx) - refused\n", what, address,
                    length, prot, static_cast<unsigned long long>(r.lo), static_cast<unsigned long long>(r.hi));
            logStack();
        }
        return true;
    }
    return false;
}

bool g_reserveProtNone = false;

}  // namespace

extern "C" {

void ps4_kit_set_jit_in_system_pool(int on) { ps4_jit_in_system_pool = on; }

int ps4_exec_probe(void) {
    const ExecSource source = execSource();
    return source == kExecNone ? -1 : static_cast<int>(source);
}

const char* ps4_kit_exec_source(void) {
    return g_execProbed.load(std::memory_order_acquire) ? kExecSourceNames[execSource()] : "not probed yet";
}

void ps4_kit_set_mmap_reserve_prot_none(int on) { g_reserveProtNone = on != 0; }
void ps4_kit_set_image_guard(int on) { g_imageGuard.store(on != 0, std::memory_order_relaxed); }

int __wrap_munmap(void* addr, size_t len) {
    if (hitsImage(addr, len, "munmap", -1, false))
        return -1;
    return __real_munmap(addr, len);
}

int __wrap_mprotect(void* addr, size_t len, int prot) {
    if (hitsImage(addr, len, "mprotect", prot, true))
        return -1;
    return __real_mprotect(addr, len, prot);
}

int32_t __wrap_sceKernelMunmap(void* addr, size_t len) {
    if (hitsImage(addr, len, "sceKernelMunmap", -1, false))
        return static_cast<int32_t>(0x80020016);  // EINVAL
    return __real_sceKernelMunmap(addr, len);
}

int32_t __wrap_sceKernelMprotect(const void* addr, size_t len, int prot) {
    if (hitsImage(addr, len, "sceKernelMprotect", prot, true))
        return static_cast<int32_t>(0x80020016);
    return __real_sceKernelMprotect(addr, len, prot);
}

int32_t __wrap_sceKernelMtypeprotect(const void* addr, size_t len, int type, int prot) {
    if (hitsImage(addr, len, "sceKernelMtypeprotect", prot, true))
        return static_cast<int32_t>(0x80020016);
    return __real_sceKernelMtypeprotect(addr, len, type, prot);
}

// Large anonymous mmaps (--wrap=mmap): JIT code buffers, DSP ARAM, FIFOs, guest RAM ... would
// otherwise come out of the regular flexible memory pool (255 MiB), which Piglet (OpenGL ES)
// needs for all GPU memory - it ran out of it entirely. Take them from the system flexible pool
// (like the heap) instead; fall back to a normal mmap if that fails. Executable mappings come from
// the source the probe above found.
void* __wrap_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    const bool anonymous = fd == -1 && (flags & MAP_ANON) && !(flags & MAP_FIXED);
    if (anonymous && (prot & PROT_EXEC)) {
        static std::atomic<unsigned> s_logged{0};
        unsigned n = 0;
        const ExecSource source = execSource();
        const size_t size = (len + PAGE - 1) & ~(PAGE - 1);
        void* base = source != kExecRegular && source != kExecNone ? mapExec(source, size, nullptr) : nullptr;
        if (base) {
            if (sampledLog(s_logged, &n))
                heapLog("mmap #%u: %zu KiB prot %d from %s at %p, caller elf 0x%llx\n", n, size / 1024, prot,
                        kExecSourceNames[source], base, elf(__builtin_return_address(0)));
            return base;
        }
        void* result = __real_mmap(addr, len, prot, flags, fd, offset);
        if (sampledLog(s_logged, &n))
            heapLog("mmap #%u: %zu KiB prot %d from regular flexible memory = %p, caller elf 0x%llx\n", n, len / 1024,
                    prot, result, elf(__builtin_return_address(0)));
        return result;
    }
    // Optional: inaccessible mappings are address space reservations (QEMU's guest RAM is reserved
    // PROT_NONE, then mapped MAP_FIXED inside): reserve the range without memory behind it.
    if (anonymous && prot == PROT_NONE && len >= MB && g_reserveProtNone) {
        const size_t size = (len + PAGE - 1) & ~(PAGE - 1);
        if (void* base = reserveAwayFromKernel(size)) {
            static std::atomic<unsigned> s_reserved{0};
            unsigned n = 0;
            if (sampledLog(s_reserved, &n))
                heapLog("mmap #%u: %zu KiB reserved (no memory) at %p, caller elf 0x%llx\n", n, size / 1024, base,
                        elf(__builtin_return_address(0)));
            return base;
        }
    }
    static std::atomic<unsigned> s_mapped{0};
    unsigned n = 0;
    if (anonymous && len >= MB && !g_opt.regular_only) {
        const size_t size = (len + PAGE - 1) & ~(PAGE - 1);
        void* base = reserveAwayFromKernel(size);
        if (base) {
            const int32_t ret =
                ps4k_sceKernelMapNamedSystemFlexibleMemory(&base, size, prot & 0x7, MAP_FIXED_FLAG, g_nameMmap);
            if (ret == 0) {
                if (sampledLog(s_mapped, &n))
                    heapLog("mmap #%u: %zu KiB prot %d from system flexible memory at %p, caller elf 0x%llx\n", n,
                            size / 1024, prot, base, elf(__builtin_return_address(0)));
                return base;
            }
            if (sampledLog(s_mapped, &n))
                heapLog("mmap #%u: %zu KiB prot %d not possible from system flexible memory (0x%x)\n", n, size / 1024,
                        prot, ret);
            ps4k_sceKernelMunmap(base, size);
        }
    }
    void* result = __real_mmap(addr, len, prot, flags, fd, offset);
    if (anonymous && len >= MB && sampledLog(s_mapped, &n)) {
        size_t available = 0;
        ps4k_sceKernelAvailableFlexibleMemorySize(&available);
        heapLog("mmap #%u: %zu KiB prot %d from regular flexible memory = %p, caller elf 0x%llx, %zu MiB flexible left\n",
                n, len / 1024, prot, result, elf(__builtin_return_address(0)), available / MB);
    }
    return result;
}

// --- Time ----------------------------------------------------------------------------------------------
//
// clock_gettime for every caller (--wrap): CLOCK_MONOTONIC from the CPU's time stamp counter.
// The kernel's clock_gettime is a system call (FreeBSD 9 has no user-space time page; 777 ns per
// call against 50 ns here, measured on a base PS4), and emulators read the monotonic clock
// thousands of times a second on every busy thread (Dolphin's CoreTiming throttle, std::chrono
// timeouts, wait timers): ~6-9% of each emulation thread in FIFA Street 2. The counter is
// invariant on the PS4's Jaguar cores; it is anchored to the kernel's clock on first use, so
// absolute times from both agree. Other clocks go to the kernel.
unsigned long long ps4_clock_reads = 0;
static bool s_countClockReads = false;

void ps4_kit_set_count_clock_reads(int on) { s_countClockReads = on != 0; }

int __wrap_clock_gettime(clockid_t id, struct timespec* ts) {
    if (id != CLOCK_MONOTONIC)
        return __real_clock_gettime(id, ts);
    struct Anchor {
        uint64_t tsc = 0, ns = 0, mult = 0;  // ns per tick as 32.32 fixed point
        bool ok = false;
    };
    static const Anchor anchor = [] {
        Anchor a;
        const uint64_t freq = ps4k_sceKernelGetTscFrequency();
        struct timespec now;
        if (freq < 1000000 || __real_clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return a;
        a.tsc = __builtin_ia32_rdtsc();
        a.ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
        a.mult = (1000000000ull << 32) / freq;
        a.ok = true;
        return a;
    }();
    if (!anchor.ok)
        return __real_clock_gettime(id, ts);
    if (s_countClockReads)
        ps4_clock_reads++;  // approximate, unsynchronised
    const uint64_t ticks = __builtin_ia32_rdtsc() - anchor.tsc;
    const uint64_t ns = anchor.ns + static_cast<uint64_t>((static_cast<unsigned __int128>(ticks) * anchor.mult) >> 32);
    ts->tv_sec = static_cast<time_t>(ns / 1000000000ull);
    ts->tv_nsec = static_cast<long>(ns % 1000000000ull);
    return 0;
}

// --- Threads -------------------------------------------------------------------------------------------

// pthread_once for every caller (--wrap): the kernel's (FreeBSD) version treats pthread_once_t
// as a 16-byte struct, but the OpenOrbis headers - and the prebuilt libc.a (newlocale,
// call_once, ...) - use a 4-byte int, which it overflowed. This one only uses those 4 bytes:
// 0 = not run (PTHREAD_ONCE_INIT), 1 = running, 2 = done. Waiters back off to sleeping: the
// thread running init may be of lower priority (see the file comment).
int __wrap_pthread_once(pthread_once_t* once, void (*init)(void)) {
    auto* state = reinterpret_cast<int*>(once);
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 2)
        return 0;
    int expected = 0;
    if (__atomic_compare_exchange_n(state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init();
        __atomic_store_n(state, 2, __ATOMIC_RELEASE);
        return 0;
    }
    Backoff backoff;
    while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2)
        backoff.wait();
    return 0;
}

// pthread_create for every caller (--wrap): a PS4 thread inherits its creator's CPU affinity, so a
// worker started from an emulator thread that runs on one core (Cemu's H.264 decoder, started from
// an emulated CPU core's thread; DolphinPS4's shader compilers) would share that busy core and
// starve. New threads start on ps4_new_thread_cores instead (by default the cores the process
// started with: all seven, 0-6, when the app has the 7th core - Crema, SMW and Conker use it); a
// thread that wants others sets them itself.
uint64_t ps4_new_thread_cores = 0;  // 0 until the first allocation reads the process's cores

// The PS4 runs an app's threads by priority alone, with no turns among threads of the same one: a
// thread that never waits keeps its core while threads of its priority that want one wait behind it
// (WinePS4 v00.70: a web helper's six busy threads and a game's loading held all seven cores, and
// the desktop's loop and wineserver stood). A process whose threads must yield to another's, or win
// over them, gives its new threads a priority of their own (0: the system's, inherited).
int ps4_new_thread_priority = 0;

}  // extern "C"

namespace {
// Every thread gets at least this much stack. The system's default is far smaller than a PC's
// (logged at the first thread), and shader translation (glslang) and the driver's shader compiler
// recurse deeply on large shaders; a thread out of stack is ended by the system with error
// CE-34878-0 (suspected when Minecraft's menus appear). SMW and SSBU raised it to 4 MiB for their
// NVN replay thread and RADV's compiles (ps4_kit_set_min_thread_stack).
size_t g_minThreadStack = 2 * 1024 * 1024;
int g_boostPriority = 650;

struct ThreadStart {
    void* (*start)(void*);
    void* arg;
};

uint64_t newThreadCores() {
    const uint64_t cores = ps4_new_thread_cores;
    return cores ? cores : 0x3F;
}
}  // namespace

extern "C" {

// Every thread's outermost frame of app code. Unwinding past it steps into the system library that
// started the thread, which the eboot's unwind tables don't cover: libunwind then searches all of
// .eh_frame (1.9 MB in Conker) on every backtrace. A backtrace stops at this function (Conker
// v00.07/08: save states' wait signatures, taken at every game thread wait, held the game at
// ~0.4 fps). No tail call, so the frame is always there.
__attribute__((noinline, disable_tail_calls)) void* ps4_thread_entry(void* raw) {
    const ThreadStart start = *static_cast<ThreadStart*>(raw);
    free(raw);
    ps4k_scePthreadSetaffinity(ps4k_scePthreadSelf(), newThreadCores());
    if (ps4_new_thread_priority)
        ps4k_scePthreadSetprio(ps4k_scePthreadSelf(), ps4_new_thread_priority);
    if (ps4_install_signal_stack)
        ps4_install_signal_stack();
    // in the stack dumps (crash.log, hang.log) from the start; a thread that names itself later
    // (SetThreadName) updates the name
    if (ps4_watch_thread)
        ps4_watch_thread("(unnamed)");
    if (ps4_profile_thread_begin)
        ps4_profile_thread_begin();
    return start.start(start.arg);
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    // the stack: at least g_minThreadStack
    pthread_attr_t local;
    bool ownAttr = false;
    size_t stack = 0;
    if (attr) {
        pthread_attr_getstacksize(attr, &stack);
        if (stack < g_minThreadStack)
            pthread_attr_setstacksize(const_cast<pthread_attr_t*>(attr), g_minThreadStack);
    } else if (pthread_attr_init(&local) == 0) {
        ownAttr = true;
        pthread_attr_getstacksize(&local, &stack);
        if (stack < g_minThreadStack)
            pthread_attr_setstacksize(&local, g_minThreadStack);
        attr = &local;
    }
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true) && ps4_boot_trace) {
        uint64_t mask = 0;
        ps4k_scePthreadGetaffinity(ps4k_scePthreadSelf(), &mask);
        char line[160];
        snprintf(line, sizeof(line),
                 "threads: first creator's cores %#llx, new threads get %#llx; stack %zu KiB by default, at least %zu KiB",
                 static_cast<unsigned long long>(mask), static_cast<unsigned long long>(newThreadCores()), stack / 1024,
                 g_minThreadStack / 1024);
        ps4_boot_trace(line);
    }
    int result;
    auto* wrapped = static_cast<ThreadStart*>(malloc(sizeof(ThreadStart)));
    if (!wrapped) {
        result = __real_pthread_create(thread, attr, start, arg);
    } else {
        *wrapped = {start, arg};
        result = __real_pthread_create(thread, attr, ps4_thread_entry, wrapped);
        if (result != 0)
            free(wrapped);
    }
    if (ownAttr)
        pthread_attr_destroy(&local);
    return result;
}

void ps4_kit_set_new_thread_cores(uint64_t mask) { ps4_new_thread_cores = mask & 0x7F; }
void ps4_kit_set_min_thread_stack(size_t bytes) { g_minThreadStack = bytes; }
void ps4_kit_set_boost_priority(int priority) { g_boostPriority = clampInt(priority, 256, 767); }
void ps4_kit_set_new_thread_priority(int priority) { ps4_new_thread_priority = priority ? clampInt(priority, 256, 767) : 0; }
void ps4_kit_set_thread_priority(int priority) { ps4k_scePthreadSetprio(ps4k_scePthreadSelf(), clampInt(priority, 256, 767)); }
int ps4_kit_thread_priority(void) {
    int priority = 0;
    return ps4k_scePthreadGetprio(ps4k_scePthreadSelf(), &priority) ? -1 : priority;
}

// The busiest threads that never spin on another thread's locks (RADV's GPU submission thread, the
// sceGnmSubmitCommandBuffers calls at ~0.4 ms each; emulated CPU cores; an NVN replay thread) above
// the default priority (256 highest .. 767 lowest, 700 default), as in Crema: logging, loading and
// shader compiling never take their time. Audio stays higher (400).
void ps4_boost_thread(void) { ps4k_scePthreadSetprio(ps4k_scePthreadSelf(), g_boostPriority); }

}  // extern "C"

// --- Diagnostics: TSC agreement and timing costs ---------------------------------------------------------
//
// Do the cores' time stamp counters agree? Cemu's Wii U clock and other code read the TSC on
// whatever core a thread runs on, and threads move between the app's cores. A thread pinned to core
// 0 and one pinned to core N pass a counter value back and forth; the smallest differences each way
// give the offset (core N minus core 0) and the round trip. In a detached thread, every wait limited
// to 0.2 s: a core the app can't run on (Crema 2.10 waited forever for one, and the game never
// started) is reported as "no answer".
namespace {
struct TscShared {
    std::atomic<uint64_t> value{0};
    std::atomic<int> turn{0};
    std::atomic<bool> abandon{false};
    uint64_t limit = 0;  // TSC ticks a wait may take
};

bool tscWait(TscShared& s, int turn) {
    const uint64_t start = __builtin_ia32_rdtsc();
    while (s.turn.load(std::memory_order_acquire) != turn) {
        if (s.abandon.load(std::memory_order_relaxed))
            return false;
        // the start was read on this core: the limit doesn't depend on other cores' counters
        if (__builtin_ia32_rdtsc() - start > s.limit) {
            s.abandon.store(true);
            return false;
        }
        _mm_pause();
    }
    return true;
}

void tscProbeCore(int core, double ticksPerUs, char* out, size_t size) {
    auto* shared = new TscShared;  // leaked if a side never ends (it would still read it)
    shared->limit = static_cast<uint64_t>(ticksPerUs * 200000);
    auto minAB = std::make_shared<std::atomic<uint64_t>>(~0ull);
    auto minBA = std::make_shared<std::atomic<uint64_t>>(~0ull);
    constexpr int kRounds = 200;
    auto pin = [](int c) {
        ps4k_scePthreadSetaffinity(ps4k_scePthreadSelf(), 1ull << c);
        sched_yield();  // once, to move to that core (not a wait loop)
    };
    std::thread a([shared, minBA, pin] {
        pin(0);
        for (int k = 0; k < kRounds; k++) {
            shared->value.store(__builtin_ia32_rdtsc(), std::memory_order_release);
            shared->turn.store(4 * k + 1, std::memory_order_release);
            if (!tscWait(*shared, 4 * k + 3))
                return;
            const uint64_t d = __builtin_ia32_rdtsc() - shared->value.load(std::memory_order_acquire);
            if ((int64_t)d < (int64_t)minBA->load() || minBA->load() == ~0ull)
                minBA->store(d);
            shared->turn.store(4 * k + 4, std::memory_order_release);
        }
    });
    std::thread b([shared, minAB, core, pin] {
        pin(core);
        for (int k = 0; k < kRounds; k++) {
            if (!tscWait(*shared, 4 * k + 1))
                return;
            const uint64_t d = __builtin_ia32_rdtsc() - shared->value.load(std::memory_order_acquire);
            if ((int64_t)d < (int64_t)minAB->load() || minAB->load() == ~0ull)
                minAB->store(d);
            shared->value.store(__builtin_ia32_rdtsc(), std::memory_order_release);
            shared->turn.store(4 * k + 3, std::memory_order_release);
            if (!tscWait(*shared, 4 * k + 4))
                return;
        }
    });
    a.join();
    if (shared->abandon.load())
        b.detach();  // it may never get to run on that core
    else
        b.join();
    if (shared->abandon.load()) {
        snprintf(out, size, " core%d no answer", core);
        return;
    }
    const double ab = (double)(int64_t)minAB->load(), ba = (double)(int64_t)minBA->load();
    snprintf(out, size, " core%d %+.2f (trip %.2f)", core, (ab - ba) / 2.0 / ticksPerUs, (ab + ba) / 2.0 / ticksPerUs);
}
}  // namespace

extern "C" {

void ps4_tsc_probe(void) {
    std::thread([] {
        char line[400];
        int len = snprintf(line, sizeof(line), "tsc: offsets from core 0 (us):");
        const double ticksPerUs = (double)ps4k_sceKernelGetTscFrequency() / 1e6;
        for (int core = 1; core < 7 && len < (int)sizeof(line) - 40; core++) {
            char part[64];
            tscProbeCore(core, ticksPerUs, part, sizeof(part));
            len += snprintf(line + len, sizeof(line) - len, "%s", part);
        }
        heapLog("%s\n", line);
    }).detach();
}

// The cores' clock, measured once: chains of dependent register additions, one cycle each (its two
// integer pipes keep the loop's own count and branch beside the chain on the Jaguar; additions of
// a constant are not used, as newer x86 cores fold those away when renaming), timed with
// CLOCK_MONOTONIC, the shortest of seven runs of ~1.3 ms. The TSC runs at its own fixed rate
// whatever the cores do, so it cannot say this (MP3 v00.52: does a PS4 Pro's Boost Mode reach a
// homebrew app?).
unsigned ps4_kit_cpu_mhz(void) {
    static std::atomic<unsigned> s_mhz{0};
    if (const unsigned mhz = s_mhz.load(std::memory_order_relaxed))
        return mhz;
    constexpr int kLoops = 1 << 16;  // x 32 additions
    double best = 1e30;
    for (int run = 0; run < 7; run++) {
        uint64_t x = 0;
        const uint64_t one = 1;
        timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < kLoops; i++) {
            __asm__ volatile(
                "add %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\t"
                "add %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\t"
                "add %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\t"
                "add %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\t"
                "add %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\t"
                "add %1, %0\n\tadd %1, %0"
                : "+r"(x)
                : "r"(one));
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns > 0 && ns < best)
            best = ns;
    }
    const unsigned mhz = (unsigned)((double)kLoops * 32.0 / best * 1000.0 + 0.5);
    s_mhz.store(mhz, std::memory_order_relaxed);
    return mhz;
}

// What the time stamp counter, the clocks, a yield, atomics and float conversions cost on this
// console, in ns per call. Measured on a base PS4, 2026-10-21 (Crema): rdtsc 27.7, clock_gettime
// 50, kernel clock 777, sched_yield 8755, lock xadd 10, mutex 60, mulsd 2.6, CVTSD2SS+CVTSS2SD 9.5,
// mulss 1.3.
void ps4_timer_bench(char* out, size_t size) {
    volatile uint64_t sink = 0;
    int len = snprintf(out, size, "bench (ns per call):");
    auto run = [&](const char* name, int count, auto&& body) {
        const uint64_t start = ps4k_sceKernelGetProcessTime();
        uint64_t sum = 0;
        for (int i = 0; i < count; i++)
            sum += body();
        const uint64_t us = ps4k_sceKernelGetProcessTime() - start;
        sink = sink + sum;
        if (len < (int)size - 48)
            len += snprintf(out + len, size - len, " %s %.1f", name, us * 1000.0 / count);
    };
    run("loop", 1000000, [] { return (uint64_t)1; });
    run("rdtsc", 100000, [] { return __builtin_ia32_rdtsc(); });
    run("lfence+rdtsc", 100000, [] {
        _mm_lfence();
        return __builtin_ia32_rdtsc();
    });
    run("rdtscp", 100000, [] {
        unsigned aux;
        return __builtin_ia32_rdtscp(&aux);
    });
    run("sceKernelReadTsc", 100000, [] { return ps4k_sceKernelReadTsc(); });
    run("clock_gettime", 100000, [] {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_nsec;
    });
    run("kernel_clock_gettime", 20000, [] {
        struct timespec ts;
        __real_clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_nsec;
    });
    run("process_time", 20000, [] { return ps4k_sceKernelGetProcessTime(); });
    run("sched_yield", 20000, [] { return (uint64_t)sched_yield(); });
    static std::atomic<uint64_t> counter{0};
    run("lock_xadd", 1000000, [] { return counter.fetch_add(1); });
    run("mfence", 1000000, [] {
        _mm_mfence();
        return (uint64_t)1;
    });
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    run("mutex", 200000, [] {
        pthread_mutex_lock(&mutex);
        pthread_mutex_unlock(&mutex);
        return (uint64_t)1;
    });
    // float latencies as recompiled game code chains them (xmm0 carries the chain from one
    // iteration to the next; 1.0 * 1.0 stays normal): a single-precision op is computed in double
    // and rounded with CVTSD2SS + CVTSS2SD
    // (the registers are set on each test's first pass: the clock calls between tests clobber them)
#define PS4B_FLOAT(name, init, body)                                                       \
    run(name, 1000000, [first = true]() mutable {                                          \
        if (first) {                                                                       \
            asm volatile("pcmpeqw %%xmm1, %%xmm1\n psllq $54, %%xmm1\n psrlq $2, %%xmm1\n" \
                         init "movaps %%xmm1, %%xmm0" ::: "xmm0", "xmm1");                 \
            first = false;                                                                 \
        }                                                                                  \
        asm volatile(body ::: "xmm0", "xmm2");                                             \
        return (uint64_t)1;                                                                \
    })
    PS4B_FLOAT("mulsd", "", "mulsd %%xmm1, %%xmm0");
    PS4B_FLOAT("addsd+subsd", "", "addsd %%xmm1, %%xmm0\n subsd %%xmm1, %%xmm0");
    PS4B_FLOAT("round", "", "cvtsd2ss %%xmm0, %%xmm0\n cvtss2sd %%xmm0, %%xmm0");
    PS4B_FLOAT("mulsd+round", "", "mulsd %%xmm1, %%xmm0\n cvtsd2ss %%xmm0, %%xmm0\n cvtss2sd %%xmm0, %%xmm0");
    PS4B_FLOAT("mulss", "cvtsd2ss %%xmm1, %%xmm1\n", "mulss %%xmm1, %%xmm0");
    PS4B_FLOAT("movsd_merge", "", "movsd %%xmm0, %%xmm2\n movsd %%xmm2, %%xmm0");
    PS4B_FLOAT("movaps", "", "movaps %%xmm0, %%xmm2\n movaps %%xmm2, %%xmm0");
#undef PS4B_FLOAT
}

// --- thread_local destructors ----------------------------------------------------------------------------

int __cxa_thread_atexit_impl(void (*dtor)(void*), void* obj, void* /*dso_symbol*/) {
    pthread_once(&g_threadDtorOnce, createThreadDtorKey);
    auto* entry = static_cast<ThreadDtor*>(malloc(sizeof(ThreadDtor)));
    if (!entry)
        return -1;
    // Destructors run in reverse order of registration, so push to the front.
    entry->dtor = dtor;
    entry->obj = obj;
    entry->next = static_cast<ThreadDtor*>(pthread_getspecific(g_threadDtorKey));
    pthread_setspecific(g_threadDtorKey, entry);
    return 0;
}

}  // extern "C"

// --- Ending the app ----------------------------------------------------------------------------------------
//
// A PS4 app must not end by returning from main or calling exit(): the teardown that follows (static
// destructors and the C library's exit handlers, with RADV's submission thread, VideoOut and the
// GPU still running) crashed kit-test v00.01 on its way out, leaving a system crash report and no
// crash.log of ours. The ports always asked the shell to close them instead -
// sceSystemServiceLoadExec("exit") (HENkaku's quit path) - and the shell then ends the process.
// --wrap=exit (ps4-kit.cmake, app only) routes both a return from main (crt1 calls exit) and an
// explicit exit() here.

extern "C" {
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
// gpu/ps4_vk_wsi.cpp, when the app links Vulkan: hands VideoOut back before the shell ends the process.
__attribute__((weak)) void ps4kit_video_shutdown(void);

__attribute__((noreturn)) void ps4_kit_exit(int status) {
    if (ps4_boot_trace) {
        char line[64];
        snprintf(line, sizeof line, "exit(%d): asking the shell to close the app", status);
        ps4_boot_trace(line);
    }
    if (ps4kit_video_shutdown)
        ps4kit_video_shutdown();  // the screen is ours until we give it back
    fflush(nullptr);  // the logs' last lines
    sceSystemServiceLoadExec("exit", nullptr);
    // The shell ends the process within moments; until it does, nothing else may run teardown.
    for (int i = 0; i < 100; i++) {
        struct timespec ts{0, 100'000'000};
        nanosleep(&ts, nullptr);
    }
    _exit(status);  // the shell never came: end without the teardown
}

void __wrap_exit(int status) {
    ps4_kit_exit(status);
    __builtin_unreachable();
}
}  // extern "C"
