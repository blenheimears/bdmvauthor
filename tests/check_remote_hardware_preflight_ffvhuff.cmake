if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()
file(READ "${SOURCE_DIR}/include/bdmvauthor/author.hpp" H)
file(READ "${SOURCE_DIR}/src/author.cpp" A)
file(READ "${SOURCE_DIR}/src/gui.cpp" G)
file(READ "${SOURCE_DIR}/src/cli.cpp" C)

foreach(needle
  "std::string hardware_device;"
  "std::string remote_hardware_device;")
  string(FIND "${H}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing remote hardware device API marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "verify_remote_hardware_encoder_device"
  "remote hardware preflight failed for "
  "child_device="
  "device creation failed"
  "failed to set value 'qsv="
  "remote_hardware_device_spec"
  "lossless FFVHUFF menu/composition relay"
  "-c:v ffvhuff -pred median -f matroska - | "
  "remote_ffmpeg_ffvhuff_input"
  "prefer_ffvhuff_remote_relay")
  string(FIND "${A}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing remote hardware/FFVHUFF implementation marker: ${needle}")
  endif()
endforeach()

# Deterministic hardware initialization failures must be classified before
# secondary Broken pipe diagnostics from the local feeder.
string(FIND "${A}" "device creation failed" device_pos)
string(FIND "${A}" "broken pipe" pipe_pos)
if(device_pos EQUAL -1 OR pipe_pos EQUAL -1 OR NOT device_pos LESS pipe_pos)
  message(FATAL_ERROR "hardware device failure classification does not precede Broken pipe")
endif()

foreach(needle
  "hardwareDevice"
  "Hardware device / render node"
  "/dev/dri/renderD128"
  "server.hardware_device=r.hardware_device.toStdString()")
  string(FIND "${G}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing remote render-node GUI marker: ${needle}")
  endif()
endforeach()

foreach(needle
  "--remote-hardware-device PATH"
  "tools.remote_hardware_device=value()")
  string(FIND "${C}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing remote render-node CLI marker: ${needle}")
  endif()
endforeach()

message(STATUS "remote hardware device preflight/classification and FFVHUFF menu relay source checks ok")
