file(READ "${SOURCE_DIR}/src/gui.cpp" GUI)

foreach(BAD
    "toInt(base.ac3_bitrate_kbps)"
    "toInt(base.dca_bitrate_kbps)"
)
  string(FIND "${GUI}" "${BAD}" POS)
  if(NOT POS EQUAL -1)
    message(FATAL_ERROR "invalid QVariant::toInt(defaultInteger) use remains: ${BAD}")
  endif()
endforeach()

foreach(NEEDED
    "QVariantMap encoding_map(const EncodingProfile& e)"
    "encoding_from_map"
    "m.value(\"ac3Bitrate\").toInt()"
    "m.value(\"dcaBitrate\").toInt()"
    "RoleHdProfile"
    "RoleUhdProfile"
)
  string(FIND "${GUI}" "${NEEDED}" POS)
  if(POS EQUAL -1)
    message(FATAL_ERROR "missing QVariant dual-profile recovery guard: ${NEEDED}")
  endif()
endforeach()
