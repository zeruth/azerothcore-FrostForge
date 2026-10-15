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

# The Battle.net RPC messages, as the 3.4.3.54261 client defines them. The
# bgs/ tree is extracted from the client exe, not written by hand; regenerate it with
#   python apps/bnetproto/protoextract.py <WowClassic.exe> src/server/proto --filter bgs/
# protoc runs at build time (ProtobufGenerate, deps/protobuf).

file(GLOB_RECURSE bnetproto_protos
  RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/bgs/*.proto)

ProtobufGenerate(bnetproto_sources ${CMAKE_CURRENT_SOURCE_DIR} ${bnetproto_protos})

add_library(bnetproto STATIC
  ${bnetproto_sources})

target_include_directories(bnetproto
  PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}/proto)

target_link_libraries(bnetproto
  PRIVATE
    acore-dependency-interface
  PUBLIC
    protobuf)

set_target_properties(bnetproto
  PROPERTIES
    FOLDER
      "server")
