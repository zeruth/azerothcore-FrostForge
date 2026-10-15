#
# This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#

# Protocol Buffers v21.12, the C++ runtime and protoc, for the Battle.net RPC
# (the 3.4.3 client's messages are proto2; it was built with protobuf 2.6.1,
# whose wire format v21 reads and writes unchanged). v21 is the last release
# that does not depend on abseil. Only the C++ sources the upstream cmake/
# lists name are vendored; file_lists.cmake holds those lists.

include(file_lists.cmake)

set(protobuf_definitions
  GOOGLE_PROTOBUF_CMAKE_BUILD)
if(MSVC)
  set(protobuf_options /utf-8 /bigobj)
else()
  list(APPEND protobuf_definitions HAVE_PTHREAD)
  set(protobuf_options)
endif()

add_library(protobuf STATIC
  ${protobuf_libprotobuf_lite_files}
  ${protobuf_libprotobuf_files})

target_include_directories(protobuf
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(protobuf
  PRIVATE
    ${protobuf_definitions})

target_compile_options(protobuf
  PRIVATE
    ${protobuf_options})

target_link_libraries(protobuf
  PRIVATE
    acore-dependency-interface
  PUBLIC
    threads)

add_library(protoc-lib STATIC
  ${protobuf_libprotoc_files})

target_compile_definitions(protoc-lib
  PRIVATE
    ${protobuf_definitions})

target_compile_options(protoc-lib
  PRIVATE
    ${protobuf_options})

target_link_libraries(protoc-lib
  PRIVATE
    acore-dependency-interface
  PUBLIC
    protobuf)

add_executable(protoc
  ${protobuf_protoc_files})

target_link_libraries(protoc
  PRIVATE
    acore-dependency-interface
    protoc-lib)

set_target_properties(protobuf protoc-lib protoc
  PROPERTIES
    FOLDER
      "deps")

# ProtobufGenerate(<out_sources_var> <import_root> <proto>...)
# Runs the vendored protoc over each .proto (paths relative to import_root) and
# sets <out_sources_var> to the generated .pb.cc and .pb.h files, which land in
# ${CMAKE_CURRENT_BINARY_DIR}/proto with the import_root layout. descriptor.proto
# (for custom options) resolves from the vendored runtime.
set(PROTOBUF_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src CACHE INTERNAL "")
function(ProtobufGenerate out_var import_root)
  set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/proto)
  set(generated)
  foreach(proto ${ARGN})
    string(REGEX REPLACE "\\.proto$" "" stem ${proto})
    set(cc ${out_dir}/${stem}.pb.cc)
    set(h ${out_dir}/${stem}.pb.h)
    add_custom_command(
      OUTPUT ${cc} ${h}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
      COMMAND protoc
        --proto_path=${import_root}
        --proto_path=${PROTOBUF_INCLUDE_DIR}
        --cpp_out=${out_dir}
        ${import_root}/${proto}
      DEPENDS protoc ${import_root}/${proto}
      COMMENT "protoc ${proto}"
      VERBATIM)
    list(APPEND generated ${cc} ${h})
  endforeach()
  set(${out_var} ${generated} PARENT_SCOPE)
endfunction()
