cmake_minimum_required(VERSION 3.16)

set(LIBNAME arcdav3a)
project(${LIBNAME} C)
set(CMAKE_C_STANDARD 11)
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
file(APPEND "${pc_file}" "Version: 0.1\n")
file(APPEND "${pc_file}" "Libs: -L\${libdir} -l${LIBNAME} -lm\n")
file(APPEND "${pc_file}" "Cflags: -I\${includedir}\n")
add_library(${LIBNAME} STATIC ${SRC})
install(TARGETS ${LIBNAME} LIBRARY DESTINATION ${CMAKE_INSTALL_LIB_DIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIB_DIR})
install(FILES
    include/decoder.h
    include/avs3_cnst_com.h
    include/avs3_options.h
    include/avs3_stat_com.h
    include/avs3_stat_dec.h
    include/avs3_stat_meta.h
    DESTINATION ${CMAKE_INSTALL_INCLUDE_DIR}
)
install(FILES ${pc_file} DESTINATION ${CMAKE_INSTALL_PKGCONFIG_DIR})
