cmake_minimum_required(VERSION 3.16)
project(webtransportd LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Binaries land in the source root so tests can find ./webtransportd,
# ./examples/echo, and thirdparty/picoquic/certs/ via relative paths.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Platform flags
# ---------------------------------------------------------------------------
if(WIN32)
    add_compile_definitions(_WINDOWS _GNU_SOURCE)
    set(PLATFORM_LIBS ws2_32 iphlpapi bcrypt)
    set(PLATFORM_LDEXTRA -Wl,--allow-multiple-definition)
else()
    add_compile_definitions(_GNU_SOURCE _DARWIN_C_SOURCE)
    set(PLATFORM_LIBS "")
    set(PLATFORM_LDEXTRA "")
endif()

# ---------------------------------------------------------------------------
# Sanitizers  (disable with -DNO_SANITIZER=ON)
# ---------------------------------------------------------------------------
if(NOT NO_SANITIZER AND NOT WIN32)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Warnings (our code only — vendored code uses -w)
# ---------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Werror)

# ---------------------------------------------------------------------------
# Vendored definitions shared by vendor + daemon + tests
# ---------------------------------------------------------------------------
set(PICOQUIC_DEFS
    PICOQUIC_WITH_MBEDTLS=1
    PTLS_WITHOUT_OPENSSL=1
    PTLS_WITHOUT_FUSION=1
    PICOQUIC_WITHOUT_SSLKEYLOG=1
    DISABLE_DEBUG_PRINTF=1
)
if(WIN32)
    list(APPEND PICOQUIC_DEFS _WINDOWS)
endif()

# ---------------------------------------------------------------------------
# Third-party include paths (SYSTEM so -Werror is not applied)
# ---------------------------------------------------------------------------
set(VENDOR_INCLUDES
    thirdparty/picoquic/picoquic
    thirdparty/picoquic/picohttp
    thirdparty/picoquic/picoquic_mbedtls
    thirdparty/picoquic/loglib
    thirdparty/picotls/include
    thirdparty/picotls/deps/cifra/src
    thirdparty/picotls/deps/cifra/src/ext
    thirdparty/picotls/deps/micro-ecc
    thirdparty/mbedtls/include
)
# picotls.c on Windows uses wincompat.h from the MSVC subdir; it must come
# first so it wins over picoquic's own (incomplete) wincompat.h.
if(WIN32)
    list(INSERT VENDOR_INCLUDES 0 thirdparty/picotls/picotlsvs/picotls)
endif()

# ---------------------------------------------------------------------------
# Vendored sources
# ---------------------------------------------------------------------------

# picoquic/picoquic — exclude winsockloop.c on non-Windows
file(GLOB PICOQUIC_CORE_SRCS "thirdparty/picoquic/picoquic/*.c")
if(NOT WIN32)
    list(REMOVE_ITEM PICOQUIC_CORE_SRCS
        "${CMAKE_SOURCE_DIR}/thirdparty/picoquic/picoquic/winsockloop.c")
endif()

# picohttp — all
file(GLOB PICOHTTP_SRCS "thirdparty/picoquic/picohttp/*.c")

# picotls/lib — exclude upstream-only backends not in our vendor set
file(GLOB PICOTLS_SRCS "thirdparty/picotls/lib/*.c")
foreach(_ex
        certificate_compression.c fusion.c mbedtls.c mbedtls_sign.c openssl.c)
    list(REMOVE_ITEM PICOTLS_SRCS
        "${CMAKE_SOURCE_DIR}/thirdparty/picotls/lib/${_ex}")
endforeach()

# picotls/lib/cifra — exclude libaegis (needs external aegis.h)
file(GLOB PICOTLS_CIFRA_SRCS "thirdparty/picotls/lib/cifra/*.c")
list(REMOVE_ITEM PICOTLS_CIFRA_SRCS
    "${CMAKE_SOURCE_DIR}/thirdparty/picotls/lib/cifra/libaegis.c")

# cifra internals — exclude test drivers and duplicate curve25519 variants
file(GLOB CIFRA_SRCS "thirdparty/picotls/deps/cifra/src/*.c")
file(GLOB CIFRA_TEST_SRCS "thirdparty/picotls/deps/cifra/src/test*.c")
list(REMOVE_ITEM CIFRA_SRCS ${CIFRA_TEST_SRCS})
foreach(_ex donna naclref tweetnacl)
    list(REMOVE_ITEM CIFRA_SRCS
        "${CMAKE_SOURCE_DIR}/thirdparty/picotls/deps/cifra/src/curve25519.${_ex}.c")
endforeach()

# micro-ecc
file(GLOB MICROECC_SRCS "thirdparty/picotls/deps/micro-ecc/*.c")

# picoquic_mbedtls bridge
file(GLOB PICOQUIC_MBEDTLS_SRCS "thirdparty/picoquic/picoquic_mbedtls/*.c")

# mbedtls/library — exclude Godot's platform shim
file(GLOB MBEDTLS_SRCS "thirdparty/mbedtls/library/*.c")
list(REMOVE_ITEM MBEDTLS_SRCS
    "${CMAKE_SOURCE_DIR}/thirdparty/mbedtls/library/godot_core_mbedtls_platform.c")

# loglib
file(GLOB LOGLIB_SRCS "thirdparty/picoquic/loglib/*.c")

# ---------------------------------------------------------------------------
# vendored static library
# ---------------------------------------------------------------------------
add_library(vendored STATIC
    ${PICOQUIC_CORE_SRCS}
    ${PICOHTTP_SRCS}
    ${PICOTLS_SRCS}
    ${PICOTLS_CIFRA_SRCS}
    ${CIFRA_SRCS}
    ${MICROECC_SRCS}
    ${PICOQUIC_MBEDTLS_SRCS}
    ${MBEDTLS_SRCS}
    ${LOGLIB_SRCS}
)
target_compile_options(vendored PRIVATE -w)
if(WIN32)
    target_compile_options(vendored PRIVATE
        -Wno-error=incompatible-pointer-types
        -Wno-error=implicit-function-declaration
        -Wno-error=int-conversion)
endif()
target_compile_definitions(vendored PRIVATE ${PICOQUIC_DEFS})
target_include_directories(vendored SYSTEM PRIVATE ${VENDOR_INCLUDES})
target_link_libraries(vendored PUBLIC Threads::Threads ${PLATFORM_LIBS})

# ---------------------------------------------------------------------------
# Windows resource file (UTF-8 manifest)
# ---------------------------------------------------------------------------
if(WIN32)
    set(RC_OBJ "${CMAKE_CURRENT_BINARY_DIR}/webtransportd_rc.o")
    add_custom_command(
        OUTPUT  "${RC_OBJ}"
        COMMAND windres -O coff
                -o "${RC_OBJ}"
                "${CMAKE_SOURCE_DIR}/src/core/windows/webtransportd.rc"
        DEPENDS src/core/windows/webtransportd.rc
                src/core/windows/webtransportd.exe.manifest
    )
    add_custom_target(webtransportd_rc DEPENDS "${RC_OBJ}")
    set(WINRES_EXTRA "${RC_OBJ}")
else()
    set(WINRES_EXTRA "")
endif()

# ---------------------------------------------------------------------------
# Our source files
# ---------------------------------------------------------------------------
set(DAEMON_SRCS
    src/core/webtransportd.c
    src/core/autocert.c
    src/core/child_process.c
    src/core/cmdline.c
    src/core/peer_session.c
    src/core/frame.c
    src/core/log.c
)
if(WIN32)
    list(APPEND DAEMON_SRCS src/core/wtd_wincompat.c)
endif()

# ---------------------------------------------------------------------------
# webtransportd binary
# ---------------------------------------------------------------------------
add_executable(webtransportd ${DAEMON_SRCS} ${WINRES_EXTRA})
target_include_directories(webtransportd PRIVATE src/include)
target_include_directories(webtransportd SYSTEM PRIVATE ${VENDOR_INCLUDES})
target_compile_definitions(webtransportd PRIVATE ${PICOQUIC_DEFS})
target_link_libraries(webtransportd vendored ${PLATFORM_LDEXTRA})
if(WIN32)
    add_dependencies(webtransportd webtransportd_rc)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
add_executable(echo examples/echo.c src/core/frame.c)
target_include_directories(echo PRIVATE src/include)
set_target_properties(echo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/examples")

add_executable(frame_hi examples/frame_hi.c)
target_include_directories(frame_hi PRIVATE src/include)
set_target_properties(frame_hi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/examples")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

# Helper: link a test against the full vendored stack
function(add_vendor_test name)
    add_executable(${name} tests/unit/${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE src/include)
    target_include_directories(${name} SYSTEM PRIVATE ${VENDOR_INCLUDES})
    target_compile_definitions(${name} PRIVATE ${PICOQUIC_DEFS})
    target_link_libraries(${name} vendored ${PLATFORM_LDEXTRA})
    add_test(NAME ${name} COMMAND ${name}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

# Helper: link a test against only our code (no vendored stack)
function(add_unit_test name)
    add_executable(${name} tests/unit/${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE src/include)
    target_link_libraries(${name} Threads::Threads ${PLATFORM_LIBS})
    add_test(NAME ${name} COMMAND ${name}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

# Unit tests — our code only
add_unit_test(frame_test         src/core/frame.c)
add_unit_test(frame_fuzz_test    src/core/frame.c)
add_unit_test(frame_helper_test  src/core/frame.c)
add_unit_test(log_test           src/core/log.c)
add_unit_test(env_test           src/core/env.c)
add_unit_test(cmdline_test       src/core/cmdline.c)
add_unit_test(peer_session_test  src/core/peer_session.c src/core/frame.c)
add_unit_test(child_process_test src/core/child_process.c src/core/cmdline.c)

# autocert needs mbedtls so use the vendor helper
add_vendor_test(autocert_test src/core/autocert.c)

# Exec tests — self-contained, just exec ./webtransportd
foreach(_t version_test selftest_test help_test)
    add_executable(${_t} tests/unit/${_t}.c)
    target_include_directories(${_t} PRIVATE src/include)
    target_link_libraries(${_t} Threads::Threads ${PLATFORM_LIBS})
    add_test(NAME ${_t} COMMAND ${_t}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_dependencies(${_t} webtransportd)
endforeach()

# Windows manifest test
if(WIN32)
    add_executable(windows_manifest_test
        tests/unit/windows_manifest_test.c "${RC_OBJ}")
    target_include_directories(windows_manifest_test PRIVATE src/include)
    target_link_libraries(windows_manifest_test Threads::Threads ${PLATFORM_LIBS})
    add_dependencies(windows_manifest_test webtransportd_rc)
    add_test(NAME windows_manifest_test COMMAND windows_manifest_test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
else()
    add_executable(windows_manifest_test tests/unit/windows_manifest_test.c)
    target_include_directories(windows_manifest_test PRIVATE src/include)
    add_test(NAME windows_manifest_test COMMAND windows_manifest_test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# picoquic vendor tests
add_vendor_test(picoquic_link_test)
add_vendor_test(picoquic_create_test)
add_vendor_test(handshake_test)

# End-to-end tests that fork/exec webtransportd + examples/echo
foreach(_t handshake_echo_test handshake_concurrent_test handshake_static_test)
    add_vendor_test(${_t})
    add_dependencies(${_t} webtransportd echo)
endforeach()

# LSAN suppressions
set_tests_properties(
    frame_test frame_fuzz_test frame_helper_test log_test env_test
    cmdline_test peer_session_test child_process_test autocert_test
    version_test selftest_test help_test windows_manifest_test
    picoquic_link_test picoquic_create_test handshake_test
    handshake_echo_test handshake_concurrent_test handshake_static_test
    PROPERTIES ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/.asanignore:leak_check_at_exit=1"
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS webtransportd echo frame_hi DESTINATION bin)
