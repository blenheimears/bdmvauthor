if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/model.hpp" MODEL)
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" AUTHOR_H)
file(READ "${SOURCE_DIR}/src/author.cpp" AUTHOR)
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/cli.cpp" CLI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)

foreach(needle
  "H264Hardware=4"
  "HevcHardware=5"
  "video_codec_is_hardware"
  "h264_hardware_advanced_options"
  "hevc_hardware_advanced_options")
  string(FIND "${MODEL}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "hardware-video model regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "std::string h264_hardware_encoder"
  "std::string hevc_hardware_encoder"
  "std::string hardware_decoder"
  "DisableForHardwareEncodes")
  string(FIND "${AUTHOR_H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "hardware-video runtime settings regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "ffmpeg_hardware_decode_works"
  "-hwaccel "
  "unsupported profiles, unavailable devices"
  "video_input_hardware_plan"
  "ffmpeg_hardware_video_options"
  "backend == \"vaapi\" || backend == \"qsv\" || backend == \"vulkan\""
  "h264_metadata=aud=insert"
  "hevc_metadata=aud=insert"
  "storage_.encode_cache_policy == EncodeCachePolicy::DisableForHardwareEncodes"
  "application_cache_enabled"
  "compliance_cache_root = (p.use_compliance_cache && application_cache_enabled)"
  "video_encode_cache_root")
  string(FIND "${AUTHOR}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "hardware-video authoring regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "h.264 / AVC (hardware)"
  "h.265 / HEVC (hardware)"
  "hardware/encodingBackend"
  "hardware/decodingBackend"
  "-encoders"
  "h264_"
  "hevc_"
  "-hwaccels"
  "NVIDIA CUDA / NVENC"
  "AMD AMF"
  "Intel Quick Sync Video (QSV)"
  "VAAPI"
  "VDPAU"
  "Vulkan Video"
  "Apple VideoToolbox"
  "Direct3D 11 Video Acceleration"
  "Direct3D 12 Video Acceleration"
  "DirectX Video Acceleration 2 (DXVA2)"
  "Windows Media Foundation"
  "cache/encodedMediaPolicy"
  "hardware-bypass"
  "Disable cache for hardware video encodes only"
  "Disable all caches entirely")
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "hardware-video GUI/settings regression: missing ${needle}")
  endif()
endforeach()

foreach(needle
  "h264-hardware"
  "hevc-hardware"
  "h264HardwareAdvancedOptions"
  "hevcHardwareAdvancedOptions"
  "kProjectFormatVersion = 29")
  string(FIND "${PROJECT}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "hardware-video project persistence regression: missing ${needle}")
  endif()
endforeach()

# Backend/API and application cache policy choices must remain application state,
# not project state.
foreach(forbidden
  "hardware/encodingBackend"
  "hardware/decodingBackend"
  "cache/encodedMediaPolicy")
  string(FIND "${PROJECT}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "application-only hardware/cache setting leaked into project persistence: ${forbidden}")
  endif()
endforeach()

foreach(needle
  "--h264-hardware-encoder"
  "--hevc-hardware-encoder"
  "--hardware-decoder"
  "h264-hardware"
  "hevc-hardware")
  string(FIND "${CLI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "hardware-video CLI regression: missing ${needle}")
  endif()
endforeach()

message(STATUS "FFmpeg hardware H.264/H.265 encode/decode and cache-policy source checks ok")
