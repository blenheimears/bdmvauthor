if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not set")
endif()
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)

foreach(needle
  "class UnavailableEncodingCapability"
  "verify_encoding_worker_capability"
  "encoding_capability_requirement"
  "remote_program(tools.remote_hdmaenc,\"hdmaenc\")"
  "remote_program(tools.remote_vc1enc,\"vc1enc\")"
  "remote_program(tools.remote_x264,\"x264\")"
  "remote_program(tools.remote_x265,\"x265\")"
  "remote_program(tools.remote_ffmpeg,\"ffmpeg\")"
  "FFmpeg/libx264"
  "FFmpeg/libx265"
  "mpeg2video"
  "FFmpeg AC-3 encoder"
  "FFmpeg DCA encoder"
  "FFmpeg THD/AC-3 encoders"
  "process_exited_with_code(rc,42)"
  "process_exited_with_code(rc,43)"
  "catch(const UnavailableEncodingCapability& e)"
  "required encoder/tool unavailable"
  "encoder/tool unavailable; trying another worker"
  "mark_codec_unavailable(i,codec)"
  "encodingLocation=local"
  "hdmaLocation=local")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "worker capability/cache regression: missing ${needle}")
  endif()
endforeach()

foreach(forbidden
  [=[<<"\\nencodingLocation="<<(tools.remote_encoding]=]
  [=[<< "\\nencodingLocation=" << (tools.remote_encoding]=]
  [=[hdmaLocation="<<((tools.remote_encoding]=])
  string(FIND "${AUTHOR}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "cache identity still depends on worker location: ${forbidden}")
  endif()
endforeach()

# Capability misses must be handled before the generic transient classifier so
# a missing remote executable/encoder can never be reinterpreted as broken pipe.
string(FIND "${AUTHOR}" "catch(const UnavailableEncodingCapability& e)" CAP_CATCH)
string(FIND "${AUTHOR}" "catch(const std::exception& e)" GENERIC_CATCH)
if(CAP_CATCH EQUAL -1 OR GENERIC_CATCH EQUAL -1 OR NOT CAP_CATCH LESS GENERIC_CATCH)
  message(FATAL_ERROR "hard capability catch must precede generic transient classification")
endif()

message(STATUS "worker tool capability and worker-independent cache source checks ok")
