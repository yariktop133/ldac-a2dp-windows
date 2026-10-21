cmake_minimum_required(VERSION 3.20)
project(ldac_a2dp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# --- libldac (AOSP external/libldac, Apache-2.0) ---------------------------
# ldaclib.c / ldacBT.c are "unity" files that #include the rest of src/.
set(LDAC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libldac)

add_library(ldacBT_enc STATIC
    ${LDAC_DIR}/src/ldaclib.c
    ${LDAC_DIR}/src/ldacBT.c)
target_include_directories(ldacBT_enc PUBLIC ${LDAC_DIR}/inc PRIVATE ${LDAC_DIR}/src)
if(MSVC)
    # libldac is written for GCC/Clang: shim <unistd.h> and __attribute__((unused)).
    target_include_directories(ldacBT_enc PRIVATE third_party/compat)
    target_compile_options(ldacBT_enc PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/third_party/compat/msvc_compat.h)
    # Vendor code: silence warnings we are not going to fix upstream.
    target_compile_options(ldacBT_enc PRIVATE /W1 /wd4244 /wd4267)
endif()

add_library(ldacBT_abr STATIC ${LDAC_DIR}/abr/src/ldacBT_abr.c)
target_include_directories(ldacBT_abr PUBLIC ${LDAC_DIR}/abr/inc)
target_link_libraries(ldacBT_abr PUBLIC ldacBT_enc)
if(MSVC)
    target_compile_options(ldacBT_abr PRIVATE /W1)
endif()

# --- AVDTP / A2DP protocol core --------------------------------------------
# Plain C, no heap, no CRT beyond memcpy/memset: the same sources are meant
# to be compiled into the KMDF driver later (stage 4).
add_library(avdtp STATIC
    avdtp/src/avdtp_msg.c
    avdtp/src/a2dp_codec.c
    avdtp/src/avdtp_source.c
    avdtp/src/avdtp_media.c)
target_include_directories(avdtp PUBLIC avdtp/include)
if(MSVC)
    target_compile_options(avdtp PRIVATE /W4 /WX)
else()
    target_compile_options(avdtp PRIVATE -Wall -Wextra -Werror)
endif()

# --- tools ------------------------------------------------------------------
add_executable(ldacenc tools/ldacenc/ldacenc.c tools/ldacenc/wav.c)
target_link_libraries(ldacenc PRIVATE ldacBT_abr ldacBT_enc avdtp)

# --- tests ------------------------------------------------------------------
enable_testing()
add_executable(avdtp_tests
    avdtp/tests/test_main.c
    avdtp/tests/test_msg.c
    avdtp/tests/test_codec.c
    avdtp/tests/test_loopback.c
    avdtp/tests/test_replay.c
    avdtp/tests/sim_sink.c)
target_link_libraries(avdtp_tests PRIVATE avdtp)
add_test(NAME avdtp_tests COMMAND avdtp_tests)
