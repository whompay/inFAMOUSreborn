cmake_minimum_required(VERSION 3.20)
project(inFamousRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "inFamous (BCES00220) static recompilation project"
)

# ---------------------------------------------------------------------------
# PORT_SCRUB_PRIVATE (player installs only, set by installer/infamous.toml): removes obsolete files
# left behind by older setups. Never runs in a source tree (.git or github/ at the root, or no .installer/).
# ---------------------------------------------------------------------------
option(PORT_SCRUB_PRIVATE "Remove obsolete files left by older setups" OFF)
get_filename_component(_scrub_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(PORT_SCRUB_PRIVATE AND EXISTS "${_scrub_root}/.installer"
   AND NOT EXISTS "${_scrub_root}/.git" AND NOT EXISTS "${_scrub_root}/github")
    set(_scrub_removed 0)
    # Obsolete folders.
    file(GLOB _scrub_dirs LIST_DIRECTORIES true
         "${_scrub_root}/_xfer*" "${_scrub_root}/_audit_*" "${_scrub_root}/_to_delete"
         "${_scrub_root}/infamous-recomp/_xfer*" "${_scrub_root}/infamous-recomp/_bench"
         "${_scrub_root}/ps3recomp/_xfer*" "${_scrub_root}/ps3recomp/scratch"
         "${_scrub_root}/infamous-recomp/tools/bench")
    foreach(_d IN LISTS _scrub_dirs)
        if(IS_DIRECTORY "${_d}")
            file(REMOVE_RECURSE "${_d}")
            math(EXPR _scrub_removed "${_scrub_removed} + 1")
        endif()
    endforeach()
    # Obsolete documents (never code, game data, saves or build output).
    file(GLOB_RECURSE _scrub_docs
         "${_scrub_root}/*.md" "${_scrub_root}/*.txt" "${_scrub_root}/*.rst"
         "${_scrub_root}/*.docx" "${_scrub_root}/*.pdf" "${_scrub_root}/*.html")
    foreach(_f IN LISTS _scrub_docs)
        string(TOLOWER "${_f}" _fl)
        file(RELATIVE_PATH _rel "${_scrub_root}" "${_fl}")
        if(_rel MATCHES "^(infamous \\(europe\\)|sdl3/|fsr-sdk/|dlss-sdk/)" OR
           _rel MATCHES "(^|/)(gamedata[^/]*|savedata[^/]*|dev_hdd0|build|build-[^/]*|out|target|shadercache)/")
            continue()
        endif()
        get_filename_component(_name "${_fl}" NAME)
        if(_name MATCHES "handoff|agent|coord|thread_perf|^review_|^infamous_.*\\.md$|^ps3_porting_playbook|provenance")
            file(REMOVE "${_f}")
            math(EXPR _scrub_removed "${_scrub_removed} + 1")
        endif()
    endforeach()
    if(_scrub_removed GREATER 0)
        message(STATUS "Removed ${_scrub_removed} obsolete file(s) from older setups")
    endif()
endif()

# ---------------------------------------------------------------------------
# v189 PORT_PGO: profile-guided optimisation for EVERYTHING (lifted PPU + SPU, runtime, renderer).
#   OFF (default) = as before.
#   GEN = instrumented build (-fprofile-instr-generate); the exe writes LLVM_PROFILE_FILE at exit and every
#         30 s (main.cpp, PORT_PGO_GEN). Slower while it records.
#   USE = optimise with PORT_PGO_PROFILE (a .profdata from llvm-profdata merge of the GEN run's .profraw).
# Semantics are unchanged by PGO (code layout, inlining, register allocation, branch order only).
# Driven by pgo_all.cmd (build-pgo-gen\ and build-pgo\; build-rc\ is not touched).
# ---------------------------------------------------------------------------
set(PORT_PGO "OFF" CACHE STRING "PGO mode: OFF, GEN or USE (v189)")
set(PORT_PGO_PROFILE "" CACHE FILEPATH "llvm-profdata output for PORT_PGO=USE")
if(PORT_PGO STREQUAL "GEN")
    # v189b: IR-level instrumentation (-fprofile-generate: MST edge counters, loop counter promotion) without
    # value profiling (-disable-vp: no runtime call per indirect call / variable-size memcpy). v189's front-end
    # -fprofile-instr-generate made the w2 SPU jobs ~10x slower (3.1 ms per busy run vs 0.33) -> 4.5 fps.
    add_compile_options(-fprofile-generate "SHELL:-mllvm -disable-vp=true")
    add_compile_definitions(PORT_PGO_GEN=1)
    get_filename_component(_pgo_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB _pgo_rt "${_pgo_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
    if(NOT _pgo_rt)
        message(FATAL_ERROR "PORT_PGO=GEN: clang_rt.profile-x86_64.lib not found next to ${CMAKE_CXX_COMPILER}")
    endif()
    list(GET _pgo_rt 0 _pgo_rt)
    get_filename_component(_pgo_rt_dir "${_pgo_rt}" DIRECTORY)
    add_link_options("${_pgo_rt}" "/LIBPATH:${_pgo_rt_dir}")
    message(STATUS "PGO:             GEN (instrumented), runtime ${_pgo_rt}")
elseif(PORT_PGO STREQUAL "USE")
    file(TO_CMAKE_PATH "${PORT_PGO_PROFILE}" PORT_PGO_PROFILE)
    if(NOT EXISTS "${PORT_PGO_PROFILE}")
        message(FATAL_ERROR "PORT_PGO=USE needs PORT_PGO_PROFILE=<file.profdata> (got '${PORT_PGO_PROFILE}')")
    endif()
    # Indexed instrumentation profiles are not forward-compatible across LLVM releases. Visual
    # Studio can therefore provide a clang-cl older than the one that wrote a packaged profile.
    # Probe the actual compiler during configure so that this optional optimisation can never make
    # an otherwise supported installation fail hundreds of objects into the first build.
    set(_pgo_probe_src "${CMAKE_BINARY_DIR}/CMakeFiles/port_pgo_probe.c")
    set(_pgo_probe_obj "${CMAKE_BINARY_DIR}/CMakeFiles/port_pgo_probe.obj")
    file(WRITE "${_pgo_probe_src}" "int port_pgo_probe(void) { return 0; }\n")
    if(MSVC)
        set(_pgo_probe_args /nologo /c "${_pgo_probe_src}" "/Fo${_pgo_probe_obj}")
    else()
        set(_pgo_probe_args -c "${_pgo_probe_src}" -o "${_pgo_probe_obj}")
    endif()
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" ${_pgo_probe_args} "-fprofile-use=${PORT_PGO_PROFILE}"
                -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
                -Wno-profile-instr-missing -Wno-backend-plugin
        RESULT_VARIABLE _pgo_probe_result
        OUTPUT_VARIABLE _pgo_probe_stdout
        ERROR_VARIABLE _pgo_probe_stderr)
    file(REMOVE "${_pgo_probe_src}" "${_pgo_probe_obj}")
    if(_pgo_probe_result EQUAL 0)
        add_compile_options("-fprofile-use=${PORT_PGO_PROFILE}" -Wno-profile-instr-unprofiled
                            -Wno-profile-instr-out-of-date -Wno-profile-instr-missing -Wno-backend-plugin)
        message(STATUS "PGO:             USE ${PORT_PGO_PROFILE}")
    else()
        message(WARNING "PGO profile is incompatible with ${CMAKE_C_COMPILER}; building without PGO.\n${_pgo_probe_stderr}")
    endif()
endif()

# v190 PORT_LTO (default OFF): ThinLTO across everything -- lets the optimiser inline hot runtime paths (SPU
# channel/MFC ops in spu_channels.c, ps3_indirect_call / HLE helpers, guest-memory slow paths) into the lifted
# SPU/PPU code, which lives in other translation units. Meant together with PORT_PGO=USE. Objects become LLVM
# bitcode; lld-link does the code generation (parallel, cached in <build>/lto-cache for incremental relinks).
option(PORT_LTO "ThinLTO for the whole program (v190)" OFF)
if(PORT_LTO)
    add_compile_options(-flto=thin)
    add_link_options("/lldltocache:${CMAKE_BINARY_DIR}/lto-cache")
    message(STATUS "LTO:             ThinLTO ON (cache ${CMAKE_BINARY_DIR}/lto-cache)")
endif()

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# v103 PORT_PPU_REGCACHE (default OFF -- opt-in perf experiment): compile a register-cached copy
# of every lifted PPU TU instead of the original. ps3recomp/tools/ppu_regcache.py keeps the guest
# GPRs/FPRs/CR/XER a function uses often in locals and syncs them with *ctx only around calls
# (func_X/indirect/syscall/HLE/trampoline/taskwait hooks) and returns, so the lifted stores into
# vm_base (char-typed, may alias ctx) no longer force every register through memory. The copies
# are generated at BUILD time from recompiled/*.cpp (hand patches there carry over; never edit the
# copies) into <build>/recompiled_rc/ and compiled with the same flags. Differentially tested
# (old vs new from identical random state, ~1,400 functions x 40 seeds, 0 diffs; planted bugs
# caught). Offline speed check inconclusive (+-3% on short cold paths, faster in loops) -> measure
# in a real run: -DPORT_PPU_REGCACHE=ON in build.cmd; ON<->OFF recompiles all five TUs.
option(PORT_PPU_REGCACHE "Compile register-cached copies of the lifted PPU TUs (v103)" OFF)
# v156 PORT_PPU_VMB_LOCAL (default OFF, only with PORT_PPU_REGCACHE): ppu_regcache.py --vmb gives every
# lifted function a local copy of vm_base (rc_vmb) that the inline guest-memory helpers use (PPU_VMB in
# recompiled/ppu_recomp.h), so the global is not re-loaded after every guest store. Exact: vm_base is
# set before guest code runs and never changes. OFF by default: in clang 18 test compiles it saved ~1.7%
# instructions but the extra live local raised stack traffic (+15-20% stack references), so it is an
# A/B candidate, not a clear win. Flipping it recompiles all five TUs.
option(PORT_PPU_VMB_LOCAL "Per-function local guest base in the register-cached PPU TUs (v156)" OFF)
# v241B PORT_PPU_VR (default OFF; only with PORT_PPU_REGCACHE): VMX register caching + vector intrinsics for the
# lifter's VMX templates (ps3recomp/tools/ppu_regcache_v241.py = ppu_regcache.py + --vr / --vr-ab; its output without
# those flags is byte-identical to ppu_regcache.py). OFF = the v103 script exactly as before (no TU changes).
#   ALL = every lifted function gets the new code (--vr: VR cache + VMX intrinsics + one-test scalar FP helpers;
#         --nodrain: no trampoline drain after direct calls to functions that never set g_trampoline_fn).
#   AB  = in-run A/B twins: the functions listed in PORT_PPU_VR_LIST get an old (v103) and a new (--vr) copy behind a
#         dispatcher (env PPU_VR=0 old / PPU_VR_PHASE=0|1 new only in that PS3_AB parity / default new); everything
#         else is the v103 code. Any change of this option recompiles all five lifted TUs.
set(PORT_PPU_VR "OFF" CACHE STRING "Lifted PPU VMX register caching (v241B): OFF, ALL or AB")
# v241: --nodrain is opt-in (PORT_PPU_VR_NODRAIN=ON). The 17:50 differential harness rerun showed 0 diffs
# for --vr alone (1041 fns x 20 seeds) but 1 unexplained diff with --nodrain (func_002B9E80 seed 16, budget-cut run).
option(PORT_PPU_VR_NODRAIN "v241B --nodrain trampoline-drain elision (unverified)" OFF)
set(PORT_PPU_VR_LIST "${CMAKE_CURRENT_SOURCE_DIR}/tools/v241B_vr_hot.txt" CACHE FILEPATH "Function list for PORT_PPU_VR=AB (v241B)")
# v243A/v245 PORT_PPU_MEMLEAN (default OFF; only with PORT_PPU_REGCACHE): lean guest-memory accessors. AB = after the regcache
# script, ps3recomp/tools/ppu_memacc_v243.py gives every function in PORT_PPU_MEMLEAN_LIST a twin whose loads/stores use
# ps3recomp/runtime/ppu/ppu_memory_v243.h (no bounds compare on 8/16/64-bit loads, one byte-table test per store
# instead of bounds + reservation-bitmap bit extraction) behind a dispatcher (g_v243a_sel, spu_coherency.c: lean only
# once the runtime is verified to be in the standard fast mode; env V243A_MEM=0 old / V243A_MEM_PHASE=0|1 lean only in
# that PS3_AB parity / default lean). ALL = v245 production form: rewrite each listed function in place, retaining its
# original symbol/PGO profile and removing the old twin plus per-call dispatcher. ALL requires the standard fast memory
# model and fails closed before guest execution if diagnostics or incompatible limits disable it. OFF = no TU changes.
set(PORT_PPU_MEMLEAN "OFF" CACHE STRING "Lifted PPU lean guest accessors (v243A/v245): OFF, AB or ALL")
set_property(CACHE PORT_PPU_MEMLEAN PROPERTY STRINGS OFF AB ALL)
set(PORT_PPU_MEMLEAN_LIST "${CMAKE_CURRENT_SOURCE_DIR}/tools/v243A_mem_hot.txt" CACHE FILEPATH "Function list for PORT_PPU_MEMLEAN=AB/ALL (v243A/v245)")
if(PORT_PPU_REGCACHE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_rc_flags --stats)
    set(_rc_script "${PS3RECOMP_DIR}/tools/ppu_regcache.py")
    set(_rc_deps "")
    # --nodrain scans ALL five lifted TUs (which functions set g_trampoline_fn), so every copy depends on all of them
    set(_rc_all_ppu "")
    foreach(_s IN LISTS RECOMP_SOURCES)
        get_filename_component(_n "${_s}" NAME)
        if(_n MATCHES "^ppu_recomp_[0-9]+\\.(c|cpp)$")
            list(APPEND _rc_all_ppu "${_s}")
        endif()
    endforeach()
    if(PORT_PPU_VR STREQUAL "ALL")
        set(_rc_script "${PS3RECOMP_DIR}/tools/ppu_regcache_v241.py")
        list(APPEND _rc_flags --vr)
        if(PORT_PPU_VR_NODRAIN)
            list(APPEND _rc_flags --nodrain)
        endif()
        set(_rc_deps ${_rc_all_ppu})
        message(STATUS "Lifted PPU:      VMX register caching in every function (PORT_PPU_VR=ALL, v241B)")
    elseif(PORT_PPU_VR STREQUAL "AB")
        set(_rc_script "${PS3RECOMP_DIR}/tools/ppu_regcache_v241.py")
        list(APPEND _rc_flags "--vr-ab=${PORT_PPU_VR_LIST}")
        if(PORT_PPU_VR_NODRAIN)
            list(APPEND _rc_flags --nodrain)
        endif()
        set(_rc_deps "${PORT_PPU_VR_LIST}" ${_rc_all_ppu})
        message(STATUS "Lifted PPU:      VMX register caching A/B twins for ${PORT_PPU_VR_LIST} (PORT_PPU_VR=AB, v241B)")
    endif()
    if(PORT_PPU_VMB_LOCAL)
        list(APPEND _rc_flags --vmb)
        message(STATUS "Lifted PPU:      per-function local guest base (PORT_PPU_VMB_LOCAL=ON, v156)")
    endif()
    set(_rc_dir "${CMAKE_CURRENT_BINARY_DIR}/recompiled_rc")
    file(MAKE_DIRECTORY "${_rc_dir}")
    set(_rc_sources "")
    foreach(_src IN LISTS RECOMP_SOURCES)
        get_filename_component(_name "${_src}" NAME)
        if(_name MATCHES "^ppu_recomp_[0-9]+\\.(c|cpp)$")
            set(_out "${_rc_dir}/${_name}")
            set(_ma_cmd "")   # v243A PORT_PPU_MEMLEAN: empty when OFF -> identical command
            set(_ma_deps "")
            if(PORT_PPU_MEMLEAN STREQUAL "AB" OR PORT_PPU_MEMLEAN STREQUAL "ALL")
                string(TOLOWER "${PORT_PPU_MEMLEAN}" _ma_mode)
                set(_ma_cmd COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/ppu_memacc_v243.py" "${_out}" "${_out}"
                    "--list=${PORT_PPU_MEMLEAN_LIST}" "--hdr=${PS3RECOMP_DIR}/runtime/ppu/ppu_memory_v243.h" "--mode=${_ma_mode}" --stats)
                set(_ma_deps "${PS3RECOMP_DIR}/tools/ppu_memacc_v243.py" "${PS3RECOMP_DIR}/runtime/ppu/ppu_memory_v243.h" "${PORT_PPU_MEMLEAN_LIST}")
            endif()
            add_custom_command(
                OUTPUT "${_out}"
                COMMAND "${Python3_EXECUTABLE}" "${_rc_script}" "${_src}" "${_out}" ${_rc_flags}
                ${_ma_cmd}
                DEPENDS "${_src}" "${_rc_script}" ${_rc_deps} ${_ma_deps}
                COMMENT "PPU register caching (v103): ${_name}")
            list(APPEND _rc_sources "${_out}")
        else()
            list(APPEND _rc_sources "${_src}")
        endif()
    endforeach()
    set(RECOMP_SOURCES ${_rc_sources})
    message(STATUS "Lifted PPU:      register-cached copies in ${_rc_dir} (PORT_PPU_REGCACHE=ON, v103)")
    if(PORT_PPU_MEMLEAN STREQUAL "AB")
        message(STATUS "Lifted PPU:      lean guest-accessor A/B twins for ${PORT_PPU_MEMLEAN_LIST} (PORT_PPU_MEMLEAN=AB, v243A)")
    elseif(PORT_PPU_MEMLEAN STREQUAL "ALL")
        # Only the startup scaffold needs the gate. A directory-wide definition
        # would needlessly rebuild every SPU image and runtime source.
        set_property(SOURCE "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp" APPEND
                     PROPERTY COMPILE_DEFINITIONS PORT_PPU_MEMLEAN_ALL=1)
        message(STATUS "Lifted PPU:      direct lean guest accessors for ${PORT_PPU_MEMLEAN_LIST} (PORT_PPU_MEMLEAN=ALL, v245; no twins/dispatch)")
    endif()
else()
    message(STATUS "Lifted PPU:      original TUs (PORT_PPU_REGCACHE=OFF)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)
# v242B PORT_CPU_COMPAT (default OFF): the compatibility exe for CPUs without AVX2/FMA/BMI2 (pre-Haswell Intel,
# AVX-less Pentium/Celeron, AMD before Excavator): forces PORT_SPU_AVX2=OFF and PORT_PPU_AVX2=OFF (floor = SSE4.1,
# like the runtime's own -msse4.1), builds inFamousRecomp_sse4.exe and defines PORT_CPU_COMPAT (main.cpp entry check
# asks for SSE4.1 only). The AVX2-only intrinsic paths have SSE4.1 twins under #if __AVX2__ (rsx_d3d12_backend.c,
# cellGcmSys.c, spu_xfk.h); fused multiply-adds use fma()/fmaf(), never a*b+c, so the results are bit-identical.
# Build: build_pgo.cmd USE build-sse4 <profile> -DPORT_CPU_COMPAT=ON
# v242 PORT_CPU_AUTO (default ON): Setup and the updater build the game on the PC that runs it, so the first configure
# of a build folder probes this CPU (tools/v242/cpu_probe.c, same test as the exe's startup check). Without
# AVX2/FMA/BMI2 it selects PORT_CPU_COMPAT=ON by itself and keeps the normal exe name inFamousRecomp.exe, so the
# installer and the launcher need no change. On AVX2 CPUs, for cross builds, or with an explicit -DPORT_CPU_COMPAT=...
# nothing changes. PORT_CPU_AUTO_SIMULATE=ON (test only) acts as if the CPU lacked AVX2.
option(PORT_CPU_AUTO "Select the compatibility build automatically on CPUs without AVX2/FMA/BMI2 (v242)" ON)
option(PORT_CPU_AUTO_SIMULATE "Test only: configure as if this CPU lacked AVX2/FMA/BMI2 (v242)" OFF)
if(PORT_CPU_AUTO AND WIN32 AND NOT CMAKE_CROSSCOMPILING AND NOT DEFINED CACHE{PORT_CPU_COMPAT})
    try_run(_v242_cpu_run _v242_cpu_built "${CMAKE_BINARY_DIR}/v242_cpu_probe"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/v242/cpu_probe.c")
    if(_v242_cpu_built AND PORT_CPU_AUTO_SIMULATE)
        set(_v242_cpu_run 1)
    endif()
    if(_v242_cpu_built AND "${_v242_cpu_run}" STREQUAL "1")
        set(PORT_CPU_COMPAT ON CACHE BOOL "Compatibility build without AVX2/FMA/BMI2 (v242B; auto-selected by PORT_CPU_AUTO)")
        set(PORT_CPU_COMPAT_AUTO ON CACHE INTERNAL "PORT_CPU_COMPAT was selected by the v242 CPU probe")
        message(STATUS "CPU compat:      v242 this CPU has no AVX2/FMA/BMI2 -> compatibility build (PORT_CPU_AUTO)")
    elseif(_v242_cpu_built AND "${_v242_cpu_run}" STREQUAL "2")
        message(STATUS "CPU compat:      v242 CPU probe: AVX2/FMA/BMI2 present -> default build")
    else()
        message(STATUS "CPU compat:      v242 CPU probe gave no answer (built=${_v242_cpu_built} run=${_v242_cpu_run}) -> default build")
    endif()
endif()
option(PORT_CPU_COMPAT "Compatibility build without AVX2/FMA/BMI2 (inFamousRecomp_sse4.exe, v242B)" OFF)
if(PORT_CPU_COMPAT)
    set(PORT_SPU_AVX2 OFF)
    set(PORT_PPU_AVX2 OFF)
    message(STATUS "CPU compat:      v242B PORT_CPU_COMPAT=ON -> no AVX2/FMA/BMI2 anywhere, exe inFamousRecomp_sse4")
endif()
option(PORT_SPU_AVX2 "Compile lifted SPU code with AVX2/FMA/BMI2 (Haswell+)" ON)
# v30 (FPS): the runtime (SPU dispatcher/channels/MFC, RSX vertex conversion, cellSpurs)
# gets the same AVX2/FMA/BMI2 codegen as the lifted SPU code when PORT_SPU_AVX2 is on.
# -ffp-contract=off stays in effect for the runtime, so no FP result changes.
if(PORT_SPU_AVX2 AND TARGET ps3recomp_runtime AND (NOT MSVC OR CMAKE_C_COMPILER_ID MATCHES "Clang"))
    target_compile_options(ps3recomp_runtime PRIVATE -mavx2 -mfma -mbmi2)
endif()

# v181 PORT_C_NO_TLS_GUARDS (default ON): clang-cl in MSVC >= 19.25 compatibility puts an on-demand TLS-init guard
# (load __tls_guard, test, call __dyn_tls_on_demand_init) in front of EVERY access to an extern thread-local -- ~75k
# sites in the lifted SPU code (trampoline pointer, drain checks, per-hop ring) plus the C runtime. The guard only
# serves C++ thread_local objects with dynamic initializers (C has none) and DLLs loaded after threads exist (this is
# an EXE), so C translation units drop it: /Zc:tlsGuards- when this clang-cl knows it, else MSVC compatibility 19.24
# (the last version without guards) for C sources only -- C++ (MSVC STL) is never touched. The lifted PPU code (C++)
# gets the same effect from PPU_THREAD_LOCAL = require_constant_initialization (ppu_recomp.h / ppu_context.h).
option(PORT_C_NO_TLS_GUARDS "C sources (lifted SPU + runtime C): no on-demand TLS-init guards (v181)" ON)
set(_c_no_tlsg "")
if(PORT_C_NO_TLS_GUARDS AND MSVC AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    # v182: v181 picked /Zc:tlsGuards- because check_c_compiler_flag passed -- but clang-cl only WARNS
    # "argument unused during compilation" for it (clang 18: the flag is in the ignored list, no cc1 option),
    # so the build kept all ~75k guards in C code (perf182 exe: spu_tlc still had two guard checks per hop).
    # Always use the MSVC-compat-19.24 route for C sources (verified: removes the guard for C, clang 18).
    set(_c_no_tlsg "/clang:-fms-compatibility-version=19.24")
    message(STATUS "C sources:       no TLS-init guards via ${_c_no_tlsg} (PORT_C_NO_TLS_GUARDS=ON, v181)")
    if(TARGET ps3recomp_runtime)
        target_compile_options(ps3recomp_runtime PRIVATE "$<$<COMPILE_LANGUAGE:C>:${_c_no_tlsg}>")
    endif()
endif()

# v183 PORT_NO_GS (default ON): /GS- (no stack-cookie store + __security_check_cookie per call) for the runtime and
# the lifted SPU code, as v156 did for the lifted PPU code. perf184: ~9k exe functions still carried the cookie,
# among them the per-dispatch SPU hot paths (spu_wws_job_refresh_ovl: 8 pushes + cookie for a header compare).
# Code generation only: the cookie detects stack smashing after the fact, it changes no results. OFF = old.
option(PORT_NO_GS "No /GS stack cookies in the runtime and lifted SPU code (v183)" ON)
if(MSVC AND PORT_NO_GS AND TARGET ps3recomp_runtime)
    target_compile_options(ps3recomp_runtime PRIVATE /GS-)
    message(STATUS "Runtime + lifted SPU: /GS- (PORT_NO_GS=ON, v183)")
endif()

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    # v95 PORT_SPU_REGCACHE (default ON): ALSO compile spu_recomp_rc.c where present
    # (ps3recomp/tools/spu_regcache.py: SPU registers held in host locals per lifted function,
    # spilled/reloaded at every runtime call, transfer and return; symbols <img>_rc_spu_*).
    # Both copies are linked; the runtime serves the rc copy unless SPU_REGCACHE=0
    # (SPU_REGCACHE=ab + PS3_AB: rc in even A/B phases). OFF = original code only.
    option(PORT_SPU_REGCACHE "Also build the register-cached lifted SPU sources (spu_recomp_rc.c)" ON)
    if(PORT_SPU_REGCACHE)
        set(_spu_rc "")
        foreach(_f IN LISTS SPU_LIFTED)
            get_filename_component(_d "${_f}" DIRECTORY)
            if(EXISTS "${_d}/spu_recomp_rc.c")
                list(APPEND _spu_rc "${_d}/spu_recomp_rc.c")
            endif()
        endforeach()
        list(LENGTH _spu_rc _spu_rc_n)
        list(APPEND SPU_LIFTED ${_spu_rc})
        message(STATUS "Lifted SPU:      + ${_spu_rc_n} register-cached twin(s) (PORT_SPU_REGCACHE=ON)")
    endif()
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# The F10 trainer calls the title's lifted functions directly, so a tree
# without the game's real lift (the toolkit smoke check, CI) can leave it out.
option(PORT_BUILD_TRAINER "Build the F10 in-game trainer (needs the game's lifted functions)" ON)
set(PORT_GAME_SOURCES
    main.cpp
    stubs.cpp
)
if(PORT_BUILD_TRAINER)
    list(APPEND PORT_GAME_SOURCES trainer.cpp)   # F10 in-game trainer (trainer.h)
endif()

add_executable(${PROJECT_NAME}
    ${PORT_GAME_SOURCES}
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)
if(NOT PORT_BUILD_TRAINER)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PORT_NO_TRAINER=1)
endif()

# The title-specific host glue used to be the last sizeable body of hot code
# left at clang-cl's baseline x86-64 target.  In particular stubs.cpp contains
# frequently-called HLE shims, while main.cpp owns the FIFO/vblank pump.  The
# lifted PPU/SPU and ps3recomp runtime already use this exact feature set.
# Keep it coupled to PORT_SPU_AVX2 so the v242 compatibility build remains
# SSE4.1-only, and disable implicit FP contraction so enabling host SIMD cannot
# change an existing a*b+c into a fused operation.  Explicit fma() calls retain
# their defined single-rounding behaviour.
if(PORT_SPU_AVX2 AND (NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set(_host_glue_fp "-ffp-contract=off")
    if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_host_glue_fp "/clang:-ffp-contract=off")
    endif()
    set_property(SOURCE ${PORT_GAME_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
        -mavx2 -mfma -mbmi2 "${_host_glue_fp}")
    message(STATUS "Host game glue:   AVX2/FMA/BMI2 codegen (FP contraction off)")
endif()

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
#   -msse4.1            -- spu_helpers.h only uses pshufb/blendv for spu_shufb (the
#                          hottest SPU op) when __SSE4_1__ is defined; without it the
#                          lifted SPU code fell back to a 16-iteration byte loop.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # 2026-10-04: + AVX2/FMA/BMI2 (user CPU: i7-14700F). SPU fma/fms/fnms called
        # libm fmaf() per lane (~2% of every SPU thread); with -mfma it is one vfmadd,
        # bit-identical (both single-rounding). PORT_SPU_AVX2=OFF for older CPUs.
        if(PORT_SPU_AVX2)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-msse4.1;-mavx2;-mfma;-mbmi2")
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-msse4.1")
        endif()
        if(MSVC AND PORT_NO_GS)   # v183: no /GS stack cookies in the lifted SPU code (see PORT_NO_GS)
            foreach(_f IN LISTS SPU_SOURCES)
                get_source_file_property(_o "${_f}" COMPILE_OPTIONS)
                set_source_files_properties("${_f}" PROPERTIES COMPILE_OPTIONS "${_o};/GS-")
            endforeach()
        endif()
        if(_c_no_tlsg)   # v181: lifted SPU code is C -- no TLS-init guards (see PORT_C_NO_TLS_GUARDS above)
            foreach(_f IN LISTS SPU_SOURCES)
                get_source_file_property(_o "${_f}" COMPILE_OPTIONS)
                set_source_files_properties("${_f}" PROPERTIES COMPILE_OPTIONS "${_o};${_c_no_tlsg}")
            endforeach()
        endif()
        # v177: the lifted WWS job modules (post-processed by ps3recomp/tools/spu_ibfast.py) build with -w --
        # clang-cl's -Wall is -Weverything and the generated code's warnings were ~800 MB of build_rc.log.
        foreach(_f IN LISTS SPU_SOURCES)
            if(_f MATCHES "/jobmod_[^/]+/spu_recomp_rc\\.c$")
                get_source_file_property(_o "${_f}" COMPILE_OPTIONS)
                set_source_files_properties("${_f}" PROPERTIES COMPILE_OPTIONS "${_o};-w")
            endif()
        endforeach()
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
        dxguid
    )
endif()
# 2026-10-04: link map (build-fast\inFamousRecomp.map) so PS3_SAMPLE's host
# `[samp] rva 0x...` buckets can be resolved to runtime/backend functions.
# v242B: the compat build is inFamousRecomp_sse4.exe (+ .map of the same stem, bench_drive copies <exe>.map).
set(_v242b_exe "inFamousRecomp")
if(PORT_CPU_COMPAT)
    if(NOT PORT_CPU_COMPAT_AUTO)   # v242: an auto-selected compat build keeps the normal exe name
        set(_v242b_exe "inFamousRecomp_sse4")
        set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${_v242b_exe}")
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE PORT_CPU_COMPAT=1)
    # Two SPU-header spots differ between FMA and no-FMA codegen (kept unchanged for the default build): spu_xfk.h's
    # spu_*_k macros use _mm_fmadd_ps unconditionally, and spu_helpers.h spu_fi's `base - step * y` is FUSED in the
    # AVX2 build (lifted SPU TUs compile with clang's default -ffp-contract=on + -mfma). Configure-time copies with the
    # equivalent explicit fmaf() forms go into an include dir searched FIRST, for the compat build only (lifted SPU TUs
    # reach them as "spu_xfk.h" / "../../runtime/spu/spu_helpers.h"). Already-patched sources are copied unchanged.
    set(_v242b_g "${CMAKE_BINARY_DIR}/v242b_compat_inc")
    get_filename_component(_v242b_spu "${PS3RECOMP_DIR}/runtime/spu" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(READ "${_v242b_spu}/spu_helpers.h" _v242b_h)
    string(REPLACE "\r\n" "\n" _v242b_h "${_v242b_h}")   # v242: CRLF copies (Setup/updater) match too
    set(_v242b_old "        r._f32[i] = base - step * y;")
    string(FIND "${_v242b_h}" "${_v242b_old}" _v242b_at)
    if(_v242b_at GREATER -1)
        string(REPLACE "${_v242b_old}" "        r._f32[i] = fmaf(-step, y, base);   /* v242B compat copy: = the fused AVX2 result */" _v242b_h "${_v242b_h}")
    elseif(NOT _v242b_h MATCHES "fmaf\\(-step, y, base\\)")
        message(FATAL_ERROR "PORT_CPU_COMPAT: spu_helpers.h spu_fi changed -- re-check its FP contraction (v242B)")
    endif()
    # spu_dfma family (unused by the current lifts, IR-checked): `x*y+t` etc. are contracted in the AVX2 build too
    string(REPLACE "spu__dget(a,i)*spu__dget(b,i)+spu__dget(t,i)" "fma(spu__dget(a,i),spu__dget(b,i),spu__dget(t,i))" _v242b_h "${_v242b_h}")
    string(REPLACE "spu__dget(a,i)*spu__dget(b,i)-spu__dget(t,i)" "fma(spu__dget(a,i),spu__dget(b,i),-spu__dget(t,i))" _v242b_h "${_v242b_h}")
    string(REPLACE "spu__dget(t,i)-spu__dget(a,i)*spu__dget(b,i)" "fma(-spu__dget(a,i),spu__dget(b,i),spu__dget(t,i))" _v242b_h "${_v242b_h}")
    file(WRITE "${_v242b_g}/runtime/spu/spu_helpers.h" "${_v242b_h}")
    file(READ "${_v242b_spu}/spu_xfk.h" _v242b_x)
    string(REPLACE "\r\n" "\n" _v242b_x "${_v242b_x}")   # v242: CRLF copies (Setup/updater) match too
    if(NOT _v242b_x MATCHES "__FMA__")
        string(FIND "${_v242b_x}" "#define spu_fma_k(a, b, c, m)" _v242b_at)
        if(_v242b_at LESS 0)
            message(FATAL_ERROR "PORT_CPU_COMPAT: spu_xfk.h layout changed (v242B)")
        endif()
        string(REPLACE "#define spu_fma_k(a, b, c, m)  spu__fst(_mm_fmadd_ps " "#define spu_fma_k(a, b, c, m)  spu__fst(spu__xfk_fmadd " _v242b_x "${_v242b_x}")
        string(REPLACE "#define spu_fms_k(a, b, c, m)  spu__fst(_mm_fmsub_ps " "#define spu_fms_k(a, b, c, m)  spu__fst(spu__xfk_fmsub " _v242b_x "${_v242b_x}")
        string(REPLACE "#define spu_fnms_k(a, b, c, m) spu__fst(_mm_fnmadd_ps(" "#define spu_fnms_k(a, b, c, m) spu__fst(spu__xfk_fnmadd(" _v242b_x "${_v242b_x}")
        string(REPLACE "#ifndef SPU_XFK_H\n#define SPU_XFK_H\n" "#ifndef SPU_XFK_H\n#define SPU_XFK_H\n/* v242B compat copy: per-lane fused fmaf instead of the FMA intrinsics (fms = fma(a,b,-c), fnms = fma(-a,b,c)) */\nstatic inline __m128 spu__xfk_f3(__m128 a, __m128 b, __m128 c, int k) { float A[4], B[4], C[4], R[4]; _mm_storeu_ps(A, a); _mm_storeu_ps(B, b); _mm_storeu_ps(C, c); for (int i = 0; i < 4; i++) R[i] = k == 0 ? fmaf(A[i], B[i], C[i]) : k == 1 ? fmaf(A[i], B[i], -C[i]) : fmaf(-A[i], B[i], C[i]); return _mm_loadu_ps(R); }\n#define spu__xfk_fmadd(a, b, c)  spu__xfk_f3((a), (b), (c), 0)\n#define spu__xfk_fmsub(a, b, c)  spu__xfk_f3((a), (b), (c), 1)\n#define spu__xfk_fnmadd(a, b, c) spu__xfk_f3((a), (b), (c), 2)\n" _v242b_x "${_v242b_x}")
        if(_v242b_x MATCHES "_mm_fn?m(add|sub)_ps")
            message(FATAL_ERROR "PORT_CPU_COMPAT: spu_xfk.h still has FMA intrinsics after the v242B patch")
        endif()
    endif()
    file(WRITE "${_v242b_g}/spu_xfk.h" "${_v242b_x}")
    # "../../runtime/spu/spu_helpers.h" is looked up under every -I dir: <g>/a/b/../../runtime/spu = <g>/runtime/spu.
    # Source-level include dirs come BEFORE the target's (checked with CMake 3.28/Ninja), so the SPU sources' own
    # INCLUDE_DIRECTORIES property (set above to runtime/spu) gets the copies prepended.
    file(MAKE_DIRECTORY "${_v242b_g}/a/b")
    if(SPU_SOURCES)
        set_source_files_properties(${SPU_SOURCES} PROPERTIES
            INCLUDE_DIRECTORIES "${_v242b_g};${_v242b_g}/a/b;${PS3RECOMP_DIR}/runtime/spu")
    endif()
    message(STATUS "CPU compat:      v242B SPU header copies for no-FMA codegen in ${_v242b_g}")
    # fma()/fmaf() -> the verified software versions in every TU (runtime + lifted PPU/SPU + game sources)
    get_filename_component(_v242b_fi "${PS3RECOMP_DIR}/runtime/ppu/v242b_compat_fma.h" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE "/FI${_v242b_fi}")
        if(TARGET ps3recomp_runtime)
            target_compile_options(ps3recomp_runtime PRIVATE "/FI${_v242b_fi}")
        endif()
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -include "${_v242b_fi}")
        if(TARGET ps3recomp_runtime)
            target_compile_options(ps3recomp_runtime PRIVATE -include "${_v242b_fi}")
        endif()
    endif()
endif()
if(WIN32)
    target_link_options(${PROJECT_NAME} PRIVATE "/MAP:${CMAKE_BINARY_DIR}/${_v242b_exe}.map")
endif()
# v242B PORT_CPU_CHECK (default ON): entry point v242b_entry (main.cpp) checks the CPU features BEFORE the C runtime
# and static constructors run (AVX2 code there would crash an older CPU first) and starts inFamousRecomp_sse4.exe or
# shows a message; then it calls the normal mainCRTStartup. OFF = the default CRT entry as before.
option(PORT_CPU_CHECK "Startup CPU feature check / compat relaunch (v242B)" ON)
if(WIN32 AND MSVC AND PORT_CPU_CHECK)
    target_link_options(${PROJECT_NAME} PRIVATE "/ENTRY:v242b_entry")
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
# v102 PORT_PPU_AVX2 (default ON): the lifted PPU TUs were the only big code built for baseline
# x86-64 (SSE2). With AVX2/FMA/BMI/LZCNT/MOVBE: fma() (the fmadd family), trunc/nearbyint (fctiw*),
# and the per-lane byte swaps of every VMX float/int op (ppu_vldf4/vstf4: 15k sites per TU) become
# single instructions (pshufb/vfmadd/roundsd/movbe) instead of libm calls / scalar loops; ~2.5%
# fewer instructions overall, 20-35% in VMX-heavy functions (clang 18, static count). Results are
# bit-identical: fma/round are exactly rounded either way, and -ffp-contract=off keeps the compiler
# from fusing a*b+c on its own. Same CPU floor as PORT_SPU_AVX2 (Haswell+). OFF = old flags.
# Changing it recompiles all five ppu_recomp_00*.cpp.
option(PORT_PPU_AVX2 "Compile the lifted PPU code with AVX2/FMA/BMI2/LZCNT/MOVBE (Haswell+)" ON)
if(MSVC)
    # v156: /GS- -- no stack-cookie check in the machine-generated lifted code (clang-cl enables /GS
    # by default; functions with local arrays, e.g. VMX temporaries, paid a cookie store + a
    # __security_check_cookie call per invocation). Code generation only; no behaviour change.
    set(_ppu_recomp_opts "/w;/bigobj;/GS-")
    if(PORT_PPU_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND _ppu_recomp_opts -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -mmovbe -mpopcnt "/clang:-ffp-contract=off")
        message(STATUS "Lifted PPU:      AVX2/FMA/BMI2/LZCNT/MOVBE codegen (PORT_PPU_AVX2=ON, v102)")
    endif()
    # v241: the PORT_PPU_VR helpers need SSE4.1 (pshufb/blendv/ptest); the compat floor is SSE4.1 anyway.
    if(NOT PORT_PPU_AVX2 AND NOT PORT_PPU_VR STREQUAL "OFF" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND _ppu_recomp_opts -msse4.1)
        message(STATUS "Lifted PPU:      -msse4.1 for PORT_PPU_VR without AVX2 (v241)")
    endif()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${_ppu_recomp_opts}")
    # v30c: inline fast guest loads in the lifted PPU code (recompiled/ppu_recomp.h)
    # v156 PORT_PPU_PRESERVE_MOST (default ON): the inline guest accessors fall back to the
    # preserve_most wrappers ppu_slow_* (ppu_loader.cpp) instead of plain calls to vm_read*/vm_write*,
    # so the rare slow-path call stops clobbering rcx/rdx/r8-r10 in the hot lifted code. OFF = plain
    # calls (exact v155 call sites; also gives the vm_* diagnostics the real caller address back).
    # Flipping it recompiles the five lifted TUs only.
    option(PORT_PPU_PRESERVE_MOST "Lifted PPU: preserve_most slow-path wrappers for guest accesses (v156)" ON)
    if(PORT_PPU_PRESERVE_MOST)
        # v157: really switched on again (PPU_USE_PRESERVE_MOST). The v156 boot crash blamed on it was the
        # SPU fast halt pad (bisect156: only SPU_FAST_HALT=0 booted); the Win64 wrapper/caller codegen was checked.
        set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_DEFINITIONS "PPU_INLINE_LOADS;PPU_USE_PRESERVE_MOST")
        message(STATUS "Lifted PPU:      preserve_most slow-path wrappers (PORT_PPU_PRESERVE_MOST=ON, v157 active)")
    else()
        set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_DEFINITIONS "PPU_INLINE_LOADS;PPU_NO_PRESERVE_MOST")
    endif()
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
