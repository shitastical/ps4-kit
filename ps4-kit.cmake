# ps4-kit: the shared PS4 runtime, link setup and packaging steps of our ports, in one place. Include
# from a project configured with toolchain/ps4-kit-toolchain.cmake:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=~/ps4-kit/toolchain/ps4-kit-toolchain.cmake ...
#   include(~/ps4-kit/ps4-kit.cmake)
#   add_executable(app ...)
#   ps4_kit_app(app)                 # runtime + its linker wraps + eboot.bin (+ ps4_kit_link_mesa(app))
#
# Every fix here used to be copied into each port by hand, and the copies drifted (the dlmalloc
# spin-lock freeze fix reached 4 of 12 ports). A fix made here reaches every port that builds with it.

set(PS4_KIT_DIR ${CMAKE_CURRENT_LIST_DIR})
if (NOT DEFINED OPENORBIS)
    set(OPENORBIS /opt/pacbrew/ps4/openorbis)
endif ()

# --- The runtime -------------------------------------------------------------------------------
# An object library, not an archive: its __wrap_ functions must always be linked, and nothing in the
# app refers to most of them by name.
option(PS4_KIT_NETFS "SMB/NFS shares under /net (runtime/ps4_netfs.cpp; needs libsmb2 and libnfs)" ON)
set(PS4_KIT_RUNTIME_SOURCES
    ${PS4_KIT_DIR}/runtime/ps4_runtime.cpp
    ${PS4_KIT_DIR}/runtime/ps4_dlmalloc.c
    ${PS4_KIT_DIR}/runtime/ps4_modules.cpp
    ${PS4_KIT_DIR}/runtime/ps4_crashlog.cpp
    ${PS4_KIT_DIR}/runtime/ps4_sampler.cpp
    ${PS4_KIT_DIR}/runtime/ps4_profile.cpp
    ${PS4_KIT_DIR}/runtime/ps4_string.c
    ${PS4_KIT_DIR}/runtime/ps4_random.c
    ${PS4_KIT_DIR}/runtime/ps4_net.cpp
    ${PS4_KIT_DIR}/runtime/ps4_fileat.cpp
    ${PS4_KIT_DIR}/runtime/ps4_stdio.cpp
    ${PS4_KIT_DIR}/runtime/ps4_bsd.c)
if (PS4_KIT_NETFS)
    list(APPEND PS4_KIT_RUNTIME_SOURCES ${PS4_KIT_DIR}/runtime/ps4_netfs.cpp)
endif ()
add_library(ps4kit_runtime OBJECT ${PS4_KIT_RUNTIME_SOURCES})
target_include_directories(ps4kit_runtime PUBLIC ${PS4_KIT_DIR}/runtime)
target_include_directories(ps4kit_runtime SYSTEM PRIVATE ${PS4_SYSROOT}/include)  # libsmb2, libnfs
target_compile_features(ps4kit_runtime PRIVATE cxx_std_20)
target_compile_options(ps4kit_runtime PRIVATE -O2)

# --- crt1.o ------------------------------------------------------------------------------------
# runtime/ps4_crt1.S: a copy of OpenOrbis' crt1.o whose libc malloc-replacement table sends every
# malloc of the process - system modules included - to our heap (ps4_replace_* in ps4_runtime.cpp).
# The system libc's own heap is ~12.7 MiB by default; Piglet's shader compiles and command buffers
# exhausted it within minutes (Dolphin: "Internal Memory is running out"), and declaring a bigger
# sceLibcHeapSize in crt1.o had no effect on hardware.
set(PS4_CRT1 ${CMAKE_BINARY_DIR}/ps4-crt1-malloc-replace.o)
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -target x86_64-pc-freebsd12-elf -c ${PS4_KIT_DIR}/runtime/ps4_crt1.S -o ${PS4_CRT1}
    RESULT_VARIABLE _ps4_crt1_result)
if (NOT _ps4_crt1_result EQUAL 0)
    message(FATAL_ERROR "assembling runtime/ps4_crt1.S failed: ${_ps4_crt1_result}")
endif ()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PS4_KIT_DIR}/runtime/ps4_crt1.S)
foreach (_lang C CXX)
    string(REPLACE "${OPENORBIS}/lib/crt1.o" "${PS4_CRT1}" CMAKE_${_lang}_LINK_EXECUTABLE
                   "${CMAKE_${_lang}_LINK_EXECUTABLE}")
endforeach ()

# --- Linker script -----------------------------------------------------------------------------
# The toolchain's linker script only collects plain .init_array, so prioritized constructors
# (.init_array.NNN - e.g. libc++'s iostream setup) end up in an orphan section the loader never
# runs. Link with a copy that includes them, in priority order.
set(PS4_LINK_SCRIPT_IN ${OPENORBIS}/link.x)
set(PS4_LINK_SCRIPT ${CMAKE_BINARY_DIR}/ps4-link.x)
file(READ ${PS4_LINK_SCRIPT_IN} PS4_LINK_SCRIPT_TEXT)
string(FIND "${PS4_LINK_SCRIPT_TEXT}" "*(.init_array);" PS4_INIT_ARRAY_POS)
if (PS4_INIT_ARRAY_POS EQUAL -1)
    message(FATAL_ERROR "Unexpected ${PS4_LINK_SCRIPT_IN}: no '*(.init_array);' rule to patch")
endif ()
string(REPLACE "*(.init_array);"
    "KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))); KEEP(*(.init_array));"
    PS4_LINK_SCRIPT_TEXT "${PS4_LINK_SCRIPT_TEXT}")
# It also only collects plain .data/.tdata/.tbss. Per-symbol sections (.data.<name>: C++ template
# statics and inline variables, libc's stdin/stdout/stderr FILEs, the C++ personality pointer, all of
# Mesa with -fdata-sections) became orphans placed past the end of the data segment's file image, so
# they started out as zeros on the console.
foreach (rule ".data" ".tdata" ".tbss")
    string(FIND "${PS4_LINK_SCRIPT_TEXT}" "*(${rule})" PS4_RULE_POS)
    if (PS4_RULE_POS EQUAL -1)
        message(FATAL_ERROR "Unexpected ${PS4_LINK_SCRIPT_IN}: no '*(${rule})' rule to patch")
    endif ()
    string(REPLACE "*(${rule})" "*(${rule} ${rule}.*)" PS4_LINK_SCRIPT_TEXT "${PS4_LINK_SCRIPT_TEXT}")
endforeach ()
file(WRITE ${PS4_LINK_SCRIPT} "${PS4_LINK_SCRIPT_TEXT}")
string(REPLACE "${PS4_LINK_SCRIPT_IN}" "${PS4_LINK_SCRIPT}" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
if (NOT CMAKE_EXE_LINKER_FLAGS MATCHES "ps4-link.x")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --script ${PS4_LINK_SCRIPT}")
endif ()

# --- Linker wraps ------------------------------------------------------------------------------
# Process-wide, as the ports have always had them (CMake's probe programs link with these too):
#  malloc family: the libc heap can't initialize on retail consoles (it maps 2.5 GiB of system
#    flexible memory; malloc then crashes at 0x38 before main) - ps4_runtime.cpp's dlmalloc heap.
#  pthread_once: the kernel's writes 16 bytes into the headers' 4-byte pthread_once_t.
#  mmap: large anonymous mmaps from the system flexible pool, keeping regular flexible memory free.
#  clock_gettime: CLOCK_MONOTONIC from the time stamp counter instead of a system call.
#  pthread_create: new threads on all of the app's cores, not just their creator's.
set(PS4_KIT_WRAPS malloc free calloc realloc memalign __memalign pthread_once mmap clock_gettime
    pthread_create)
foreach (fn ${PS4_KIT_WRAPS})
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=${fn}")
endforeach ()
# App only (ps4_kit_app): a probe program calling one of these would not link without the runtime.
#  munmap/mprotect and libkernel's own: the image guard refuses to unmap or reprotect the eboot's
#    segments (a stray call there killed the app with no crash log), and the heap tracks what it maps.
#  exit: the shell closes the app (ps4_kit_exit) - the process teardown crashed with the GPU live.
#  getaddrinfo: the C library has no DNS on the PS4; ps4_net.cpp asks the PS4's resolver.
#  Paths (ps4_fileat.cpp): the *at functions, getcwd/chdir/fchdir, realpath, readlink and statvfs are
#    ENOSYS stubs or broken in the C library (Boost.Filesystem and libc++'s std::filesystem use them);
#    every path is made absolute with ".." removed by name (a ".." in SSBU's save path lost saves).
#  The reads: /net SMB/NFS shares (ps4_netfs.cpp).
set(PS4_KIT_APP_WRAPS exit munmap mprotect sceKernelMunmap sceKernelMprotect sceKernelMtypeprotect getaddrinfo
    open close stat lstat mkdir rmdir unlink rename access getcwd chdir fchdir
    fstatat openat unlinkat fchmodat utimensat realpath readlink
    statvfs fstatvfs)
if (PS4_KIT_NETFS)
    list(APPEND PS4_KIT_APP_WRAPS read _read readv _readv pread lseek fstat getdents)
endif ()
#  stdio output (ps4_stdio.cpp): one lock around it, as musl never locks its FILEs here (two threads
#    printing at once wrote each other's stacks). Each is also --undefined: the libc function gets
#    linked even when nothing in the app calls it, or its __real_ name is left as an import.
set(PS4_KIT_STDIO_WRAPS vfprintf vprintf fprintf printf fputs puts fputc putc putchar fwrite fflush perror)

# --- Identity ----------------------------------------------------------------------------------
# The plain homebrew identity: Vulkan (RADV on GNM) gets the 4.5 GiB of direct memory under it. Ports
# that draw with the system's Piglet (OpenGL ES) need RetroArch's authority ID 0x3100000000000002
# instead: Piglet only gives a display to processes with a system authority ID (love-ps4).
set(PS4_PAID "0x3800000000000035" CACHE STRING "Program authority ID of eboot.bin")
set(PS4_AUTH_INFO "000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")

# --- Mesa (RADV on GNM) ------------------------------------------------------------------------
# Built by gpu/build-mesa.sh. Whole archives, as Mesa's own shared-library link does: the dispatch
# tables refer to entry points through weak symbols, which don't pull archive members (they'd
# silently be NULL). libvulkan_radeon.a already contains the Vulkan runtime and WSI objects.
if (DEFINED ENV{PS4_MESA_BUILD})
    set(_ps4_mesa_default $ENV{PS4_MESA_BUILD})
else ()
    set(_ps4_mesa_default $ENV{HOME}/ps4kit-build/build/mesa)
endif ()
set(PS4_MESA_BUILD "${_ps4_mesa_default}" CACHE PATH "Mesa PS4 build directory")
get_filename_component(_ps4_mesa_src_default "${PS4_MESA_BUILD}/../../src/mesa" ABSOLUTE)
set(PS4_MESA_SRC "${_ps4_mesa_src_default}" CACHE PATH "Mesa PS4 source tree (its include/ has the Vulkan headers)")
# RADV, plus the kit's presenter (gpu/ps4_vk_wsi.cpp: VK_EXT_headless_surface + VK_KHR_swapchain on
# VideoOut, which the statically linked RADV lacks). The app gets vkGetInstanceProcAddr from
# ps4kit::vk_get_instance_proc_addr() (gpu/ps4_vk_wsi.h).
function(ps4_kit_link_mesa target)
    target_sources(${target} PRIVATE ${PS4_KIT_DIR}/gpu/ps4_vk_wsi.cpp)
    target_include_directories(${target} PRIVATE ${PS4_KIT_DIR}/gpu ${PS4_MESA_SRC}/include)
    set(libs
        src/amd/vulkan/libvulkan_radeon.a src/amd/common/libamd_common.a
        src/amd/compiler/libaco.a src/amd/addrlib/libaddrlib.a src/vulkan/util/libvulkan_util.a
        src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
        src/util/libxmlconfig.a src/util/libmesa_util.a src/util/libmesa_util_simd.a
        src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a src/util/libparson.a
        src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a)
    list(TRANSFORM libs PREPEND "${PS4_MESA_BUILD}/")
    # ps4.cmake links with ld.lld directly: raw linker flags. Keyword signature, as most projects
    # link their app targets with PRIVATE and CMake forbids mixing the two.
    target_link_libraries(${target} --whole-archive ${libs} --no-whole-archive)
    # The presenter's VideoOut and splash screen; zlib for Mesa's shader cache.
    target_link_libraries(${target} ${PS4_SYSROOT}/lib/libz.a -lSceVideoOut -lSceSystemService)
endfunction()

# --- eboot.bin ---------------------------------------------------------------------------------
# ELF -> <binary dir>/<target>_eboot/eboot.bin (fake SELF), then check the result: PacBrew's
# create-fself (Nov 2024) predates OpenOrbis create-fself 626d440, and depending on the size of
# .data.rel.ro it emits the data PT_LOAD twice, which the console rejects at launch ("CE-41839-5",
# LNC 0x80aa001a).
function(ps4_kit_add_eboot target)
    set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/${target}_eboot)
    add_custom_command(
        OUTPUT ${out_dir}/eboot.bin
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${CMAKE_COMMAND} -E env OO_PS4_TOOLCHAIN=${OPENORBIS}
            ${OPENORBIS}/bin/create-fself -in=$<TARGET_FILE:${target}>
            -out=${out_dir}/${target}.oelf --eboot ${out_dir}/eboot.bin
            --paid ${PS4_PAID} --authinfo ${PS4_AUTH_INFO}
        COMMAND ${CMAKE_COMMAND} -DOELF=${out_dir}/${target}.oelf
            -P ${PS4_KIT_DIR}/toolchain/check-oelf.cmake
        DEPENDS ${target}
        VERBATIM)
    add_custom_target(${target}_eboot ALL DEPENDS ${out_dir}/eboot.bin)
endfunction()

# The whole app: the runtime's objects, its app-only wraps, eboot.bin.
function(ps4_kit_app target)
    target_link_libraries(${target} ps4kit_runtime)
    foreach (fn ${PS4_KIT_APP_WRAPS})
        target_link_options(${target} PRIVATE --wrap=${fn})
    endforeach ()
    foreach (fn ${PS4_KIT_STDIO_WRAPS})
        target_link_options(${target} PRIVATE --wrap=${fn} --undefined=${fn})
    endforeach ()
    # The PS4 resolver and module loader (ps4_net.cpp, ps4_modules.cpp); netfs's protocol libraries
    # from the sysroot.
    if (PS4_KIT_NETFS)
        target_link_libraries(${target} ${PS4_SYSROOT}/lib/libsmb2.a ${PS4_SYSROOT}/lib/libnfs.a)
    endif ()
    target_link_libraries(${target} -lSceNet -lSceSysmodule -lSceSystemService)
    ps4_kit_add_eboot(${target})
endfunction()
