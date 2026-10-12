if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)
file(READ "${SOURCE_DIR}/src/project_file.cpp" PROJECT)

foreach(needle
  "QString hardware_encode_backend=QStringLiteral(\"auto\")"
  "QString hardware_decode_backend=QStringLiteral(\"software\")"
  "hardwareEncodingBackend"
  "hardwareDecodingBackend"
  "probe_remote_hardware_capabilities"
  "run_remote_server_capture"
  "Detect / refresh APIs"
  "Hardware encoding API"
  "Hardware decoding API"
  "Advanced overrides"
  "Automatic — choose an available FFmpeg hardware encoder"
  "Automatic hardware decoding (FFmpeg -hwaccel auto)"
  "h264_encoders"
  "hevc_encoders"
  "hwaccels"
  "preferred_remote_hardware_encoder"
  "Unsupported streams automatically fall back"
  "QString::number(c.port)"
  "remote_server_command_line"
)
  string(FIND "${GUI}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "remote hardware configuration regression: missing ${needle}")
  endif()
endforeach()

# The old raw fields may exist only in the Advanced overrides dialog, not as the
# normal remote-server configuration labels.
foreach(forbidden
  "form->addRow(\"H.264 hardware encoder\""
  "form->addRow(\"H.265 hardware encoder\""
  "form->addRow(\"Hardware decoding API\",hwdecode_"
)
  string(FIND "${GUI}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "raw remote hardware field is still exposed as a primary setting: ${forbidden}")
  endif()
endforeach()

foreach(forbidden
  "hardwareEncodingBackend"
  "hardwareDecodingBackend"
  "h264HardwareEncoder"
  "hevcHardwareEncoder"
  "hardwareDecoder"
  "hardwareDevice"
)
  string(FIND "${PROJECT}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "remote hardware application setting leaked into project serialization: ${forbidden}")
  endif()
endforeach()

message(STATUS "remote hardware configuration source checks ok")
