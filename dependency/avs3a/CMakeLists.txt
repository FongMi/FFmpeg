cmake_minimum_required(VERSION 3.16)

set(LIBNAME arcdav3a)
project(${LIBNAME} C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The speaker gain calculator is built into the native dependency set. Keep
# source revisions fixed; callers may supply FetchContent source overrides for
# offline builds. No renderer code or assets are fetched at playback time.
if(NOT TARGET ear)
    include(FetchContent)
    FetchContent_Declare(arcdav3a_boost
        URL https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.bz2
        URL_HASH SHA256=af57be25cb4c4f4b413ed692fe378affb4352ea50fbe294a11ef548f4d527d89)
    FetchContent_GetProperties(arcdav3a_boost)
    if(NOT arcdav3a_boost_POPULATED)
        FetchContent_Populate(arcdav3a_boost)
    endif()
    set(Boost_INCLUDE_DIR "${arcdav3a_boost_SOURCE_DIR}" CACHE PATH "Boost headers" FORCE)
    set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "Use Boost headers" FORCE)
    set(EAR_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(EAR_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(EAR_SIMD OFF CACHE BOOL "" FORCE)
    set(EAR_PACKAGE_AND_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(arcdav3a_ear
        GIT_REPOSITORY https://github.com/ebu/libear.git
        GIT_TAG 2db69f8fcea0bc5db8a78e14a9c2ae6ed4283c15
        GIT_SUBMODULES submodules/eigen submodules/xsimd)
    FetchContent_MakeAvailable(arcdav3a_ear)
endif()
get_target_property(ear_type ear TYPE)
if(NOT ear_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "arcdav3a requires a static libear; use -DBUILD_SHARED_LIBS=OFF")
endif()
include_directories(.)
include_directories(include)
include_directories(src)
set(SRC
    src/activation_func.c
    src/avs3_bitstream_dec.c
    src/avs3_com.c
    src/avs3_dec.c
    src/avs3_hoa_com.c
    src/avs3_hoa_dec.c
    src/avs3_init_dec.c
    src/avs3_mc_com.c
    src/avs3_mc_dec.c
    src/avs3_metadata_dec.c
    src/avs3_mix_dec.c
    src/avs3_mono_dec.c
    src/avs3_rom_com.c
    src/avs3_stereo_com.c
    src/avs3_stereo_dec.c
    src/bwe_com.c
    src/bwe_dec.c
    src/cnn_layer.c
    src/decoder.c
    src/fd_spectrum_shaping_com.c
    src/fd_spectrum_shaping_dec.c
    src/fft.c
    src/latent_quant.c
    src/load_free_model.c
    src/lsf_dec.c
    src/mcr_com.c
    src/mdct.c
    src/mdct_dequant_decode.c
    src/mdct_quant_encode.c
    src/noise_filling.c
    src/range_coder.c
    src/speaker_renderer.cpp
    src/tns.c
    src/tools.c
)
set(prefix "${CMAKE_INSTALL_PREFIX}")
set(includedir "include")
set(libdir "lib")
set(CMAKE_INSTALL_INCLUDE_DIR "${CMAKE_INSTALL_PREFIX}/${includedir}")
set(CMAKE_INSTALL_LIB_DIR "${CMAKE_INSTALL_PREFIX}/${libdir}")
set(CMAKE_INSTALL_PKGCONFIG_DIR "${CMAKE_INSTALL_LIB_DIR}/pkgconfig")
set(pc_file "${CMAKE_CURRENT_BINARY_DIR}/${LIBNAME}.pc")
file(WRITE "${pc_file}" "prefix=${prefix}\n")
file(APPEND "${pc_file}" "exec_prefix=\${prefix}\n")
file(APPEND "${pc_file}" "includedir=\${prefix}/${includedir}\n")
file(APPEND "${pc_file}" "libdir=\${exec_prefix}/${libdir}\n\n")
file(APPEND "${pc_file}" "Name: ${LIBNAME}\n")
file(APPEND "${pc_file}" "Description: AVS3-A decoder\n")
file(APPEND "${pc_file}" "Version: 0.2\n")
if(ANDROID)
    set(cxx_runtime "-lc++_shared")
elseif(APPLE)
    set(cxx_runtime "-lc++")
else()
    set(cxx_runtime "-lstdc++")
endif()
file(APPEND "${pc_file}" "Libs: -L\${libdir} -l${LIBNAME} -lear ${cxx_runtime} -lm\n")
file(APPEND "${pc_file}" "Cflags: -I\${includedir}\n")
add_library(${LIBNAME} STATIC ${SRC})
target_link_libraries(${LIBNAME} PRIVATE ear)
install(TARGETS ${LIBNAME} LIBRARY DESTINATION ${CMAKE_INSTALL_LIB_DIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIB_DIR})
install(TARGETS ear ARCHIVE DESTINATION ${CMAKE_INSTALL_LIB_DIR})
get_target_property(ear_source_dir ear SOURCE_DIR)
install(FILES "${ear_source_dir}/../LICENSE"
    DESTINATION "share/licenses/arcdav3a/ear")
install(FILES "${ear_source_dir}/../submodules/eigen/COPYING.MPL2"
    DESTINATION "share/licenses/arcdav3a/eigen")
install(FILES "${ear_source_dir}/../submodules/xsimd/LICENSE"
    DESTINATION "share/licenses/arcdav3a/xsimd")
install(FILES "${Boost_INCLUDE_DIR}/LICENSE_1_0.txt"
    DESTINATION "share/licenses/arcdav3a/boost")
install(FILES
    include/decoder.h
    include/speaker_renderer.h
    include/avs3_cnst_com.h
    include/avs3_options.h
    include/avs3_stat_com.h
    include/avs3_stat_dec.h
    include/avs3_stat_meta.h
    DESTINATION ${CMAKE_INSTALL_INCLUDE_DIR}
)
install(FILES ${pc_file} DESTINATION ${CMAKE_INSTALL_PKGCONFIG_DIR})

option(ARCDAV3A_TESTS "Build speaker-renderer regression tests" OFF)
if(ARCDAV3A_TESTS)
    enable_testing()
    add_executable(speaker_renderer_test tests/speaker_renderer_test.cpp)
    target_link_libraries(speaker_renderer_test PRIVATE ${LIBNAME})
    target_compile_options(speaker_renderer_test PRIVATE -UNDEBUG)
    add_test(NAME speaker_renderer COMMAND speaker_renderer_test)
endif()
