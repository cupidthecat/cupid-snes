include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

function(snes_configure_archive)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  # Build the same in-process readers on each platform without external helpers.
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  set(ENABLE_WERROR OFF)
  set(ZLIB_BUILD_TESTING OFF)
  set(ZLIB_BUILD_SHARED OFF)
  set(ZLIB_INSTALL OFF)
  FetchContent_Declare(snes_zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16)
  FetchContent_MakeAvailable(snes_zlib)

  set(XZ_NLS OFF)
  set(XZ_SANDBOX no)
  set(XZ_TOOL_XZ OFF)
  set(XZ_TOOL_XZDEC OFF)
  set(XZ_TOOL_LZMADEC OFF)
  set(XZ_TOOL_LZMAINFO OFF)
  set(XZ_TOOL_SCRIPTS OFF)
  set(XZ_DOC OFF)
  FetchContent_Declare(snes_xz
    URL https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz
    URL_HASH SHA256=0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9)
  FetchContent_MakeAvailable(snes_xz)

  FetchContent_Declare(snes_bzip2
    URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
    URL_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269)
  FetchContent_MakeAvailable(snes_bzip2)
  add_library(snes_archive_bzip2 STATIC
    ${snes_bzip2_SOURCE_DIR}/blocksort.c ${snes_bzip2_SOURCE_DIR}/huffman.c
    ${snes_bzip2_SOURCE_DIR}/crctable.c ${snes_bzip2_SOURCE_DIR}/randtable.c
    ${snes_bzip2_SOURCE_DIR}/compress.c ${snes_bzip2_SOURCE_DIR}/decompress.c
    ${snes_bzip2_SOURCE_DIR}/bzlib.c)
  target_include_directories(snes_archive_bzip2 PUBLIC ${snes_bzip2_SOURCE_DIR})

  set(LZ4_BUILD_CLI OFF)
  set(LZ4_BUILD_LEGACY_LZ4C OFF)
  FetchContent_Declare(snes_lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
    URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
    SOURCE_SUBDIR build/cmake)
  FetchContent_MakeAvailable(snes_lz4)

  set(ZSTD_BUILD_SHARED OFF)
  set(ZSTD_BUILD_STATIC ON)
  set(ZSTD_BUILD_PROGRAMS OFF)
  set(ZSTD_BUILD_TESTS OFF)
  FetchContent_Declare(snes_zstd
    URL https://github.com/facebook/zstd/archive/refs/tags/v1.5.7.tar.gz
    URL_HASH SHA256=37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3
    SOURCE_SUBDIR build/cmake)
  FetchContent_MakeAvailable(snes_zstd)

  # Codec targets are built in this tree. Do not search for another installed copy.
  foreach(feature ZLIB BZip2 LZMA LZ4 ZSTD OPENSSL NETTLE MBEDTLS LIBB2 LIBXML2
                  BSDXML EXPAT WIN32_XMLLITE PCREPOSIX PCRE2POSIX TEST
                  TAR CPIO CAT UNZIP INSTALL)
    set(ENABLE_${feature} OFF)
  endforeach()
  if(WIN32)
    set(ENABLE_ICONV OFF)
  else()
    set(ENABLE_ICONV ON)
  endif()
  foreach(feature LIBZ ZLIB_H LIBBZ2 BZLIB_H LIBLZMA LZMA_H LIBLZ4 LZ4_H LZ4HC_H
                  LIBZSTD ZSTD_H ZSTD_compressStream ZSTD_minCLevel)
    set(HAVE_${feature} 1)
  endforeach()
  FetchContent_Declare(snes_libarchive
    URL https://github.com/libarchive/libarchive/releases/download/v3.8.10/libarchive-3.8.10.tar.gz
    URL_HASH SHA256=a1bffbe5152cfe869751179e7b16648c87c461cb8c46eee75062efbe164cc57c)
  FetchContent_MakeAvailable(snes_libarchive)
  target_link_libraries(archive_static zlibstatic liblzma snes_archive_bzip2
    lz4_static libzstd_static)
  target_include_directories(archive_static PRIVATE ${snes_xz_SOURCE_DIR}/src/liblzma/api)
  target_compile_definitions(archive_static PUBLIC LIBARCHIVE_STATIC)
  add_library(snes_archive_library ALIAS archive_static)

  foreach(item "${snes_libarchive_SOURCE_DIR}/COPYING|libarchive.txt"
               "${snes_zlib_SOURCE_DIR}/LICENSE|zlib.txt"
               "${snes_xz_SOURCE_DIR}/COPYING|XZ.txt"
               "${snes_xz_SOURCE_DIR}/COPYING.0BSD|XZ-0BSD.txt"
               "${snes_bzip2_SOURCE_DIR}/LICENSE|bzip2.txt"
               "${snes_lz4_SOURCE_DIR}/lib/LICENSE|LZ4.txt"
               "${snes_zstd_SOURCE_DIR}/LICENSE|Zstandard.txt")
    string(REPLACE "|" ";" parts "${item}")
    list(GET parts 0 source)
    list(GET parts 1 name)
    configure_file("${source}" "${CMAKE_BINARY_DIR}/archive-licenses/${name}" COPYONLY)
  endforeach()
endfunction()
snes_configure_archive()
