cmake_minimum_required(VERSION 3.20)
project(ldac_a2dp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Static CRT: tools and the service run on test machines without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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

# --- SBC encoder (AOSP system/embdrv/sbc/encoder, Apache-2.0) ---------------
set(SBC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sbc_encoder)
add_library(sbc_enc STATIC
    ${SBC_DIR}/srce/sbc_analysis.c
    ${SBC_DIR}/srce/sbc_dct.c
    ${SBC_DIR}/srce/sbc_dct_coeffs.c
    ${SBC_DIR}/srce/sbc_enc_bit_alloc_mono.c
    ${SBC_DIR}/srce/sbc_enc_bit_alloc_ste.c
    ${SBC_DIR}/srce/sbc_enc_coeffs.c
    ${SBC_DIR}/srce/sbc_encoder.c
    ${SBC_DIR}/srce/sbc_packing.c)
target_include_directories(sbc_enc PUBLIC ${SBC_DIR}/include)
if(MSVC)
    target_compile_options(sbc_enc PRIVATE /W1
        /FI${CMAKE_CURRENT_SOURCE_DIR}/third_party/compat/sbc_msvc_compat.h)
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

# --- encoder service (user mode) --------------------------------------------
add_library(ldacsvc_core STATIC
    service/settings.cpp service/capture.cpp service/resampler.cpp
    service/encoder.cpp service/sbc.cpp service/sinks.cpp tools/ldacenc/wav.c)
target_compile_features(ldacsvc_core PUBLIC cxx_std_17)
target_compile_definitions(ldacsvc_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_link_libraries(ldacsvc_core PUBLIC ldacBT_abr ldacBT_enc sbc_enc avdtp ole32 cfgmgr32)
if(MSVC)
    target_compile_options(ldacsvc_core PUBLIC /W4 /utf-8)
endif()

add_executable(ldacsvc service/main.cpp)
target_link_libraries(ldacsvc PRIVATE ldacsvc_core)

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

add_executable(service_tests service/tests/service_tests.cpp)
target_link_libraries(service_tests PRIVATE ldacsvc_core)
add_test(NAME service_tests COMMAND service_tests)
