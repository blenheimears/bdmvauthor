if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not supplied")
endif()

file(READ "${SOURCE_DIR}/windows/build-msys2-ucrt64.sh" BUILD)
file(READ "${SOURCE_DIR}/windows/installer.nsi" NSIS)
file(READ "${SOURCE_DIR}/windows/installer-license-header.txt" LICENSE_HEADER)

foreach(needle
    "mingw-w64-ucrt-x86_64-nsis"
    "makensis.exe"
    [=[MSYS2_ARG_CONV_EXCL='*' "$ucrt_prefix/bin/makensis.exe"]=]
    "BDMVAUTHOR_INSTALLER_OUT"
    [=[bdmvauthor-${version}-windows-ucrt64-setup.exe]=]
    [=[uninstall_files_nsh="$build_dir/bdmvauthor-uninstall-files.nsh"]=]
    [=[find "$dist_dir" -type f -print0 | sort -z]=]
    [=[Delete "$INSTDIR\\%s"]=]
    [=[/DVERSION=$version]=]
    [=[/DVERSION4=${version}.0]=]
    [=[/DSTAGE_DIR=$(cygpath -w "$dist_dir")]=]
    [=[/DSOURCE_DIR=$(cygpath -w "$root")]=]
    [=[/DOUT_FILE=$(cygpath -w "$installer_out")]=]
    [=[/DUNINSTALL_FILES_NSH=$(cygpath -w "$uninstall_files_nsh")]=]
    [=[installer_license="$build_dir/bdmvauthor-installer-license.txt"]=]
    [=[cat "$root/windows/installer-license-header.txt" "$root/LICENSE" > "$installer_license"]=]
    [=[/DINSTALLER_LICENSE=$(cygpath -w "$installer_license")]=]
    [=[cp -f "$root/LICENSE" "$dist_dir/LICENSE-BDMVAUTHOR.txt"]=]
    [=[cp -f "$root/THIRD_PARTY_NOTICES.md" "$dist_dir/THIRD_PARTY_NOTICES.md"]=]
    [=[PATH="$ffmpeg_source_dir:$PATH" "$ffmpeg_source_dir/ffmpeg.exe" -L > "$dist_dir/licenses/FFmpeg-LICENSE.txt" 2>&1]=])
  string(FIND "${BUILD}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows installer build regression: missing build marker: ${needle}")
  endif()
endforeach()

foreach(needle
    [=[InstallDir "$PROGRAMFILES64\\${PRODUCT_NAME}"]=]
    "RequestExecutionLevel admin"
    "SetRegView 64"
    [=[!include "WordFunc.nsh"]=]
    [=[!insertmacro VersionCompare]=]
    [=[File /r "${STAGE_DIR}\\*"]=]
    [=[CreateShortCut "$SMPROGRAMS\\${PRODUCT_NAME}\\${PRODUCT_NAME}.lnk"]=]
    [=[CreateShortCut "$SMPROGRAMS\\${PRODUCT_NAME}\\Uninstall ${PRODUCT_NAME}.lnk"]=]
    [=[WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName"]=]
    [=[WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion"]=]
    [=[WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString"]=]
    [=[WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString"]=]
    [=[WriteRegStr HKLM "${APP_PATH_KEY}" "" "$INSTDIR\\${PRODUCT_EXE}"]=]
    [=[WriteUninstaller "$INSTDIR\\Uninstall.exe"]=]
    [=[ReadRegStr $0 HKLM "${UNINSTALL_KEY}" "InstallLocation"]=]
    [=[ReadRegStr $3 HKLM "${UNINSTALL_KEY}" "DisplayVersion"]=]
    [=[${VersionCompare} "${VERSION}" "$3" $4]=]
    [=[InitPluginsDir]=]
    [=[CopyFiles /SILENT "$0\\Uninstall.exe" "$PLUGINSDIR\\bdmvauthor-uninstall.exe"]=]
    [=[ExecWait '"$PLUGINSDIR\\bdmvauthor-uninstall.exe" /S _?=$0' $2]=]
    [=[IfFileExists "$0\\${PRODUCT_EXE}" 0 old_install_removed]=]
    [=[StrCpy $INSTDIR $0]=]
    [=[!include "${UNINSTALL_FILES_NSH}"]=]
    [=[SetErrorLevel 1]=]
    [=[Delete "$INSTDIR\\Uninstall.exe"]=]
    [=[RMDir "$INSTDIR"]=]
    [=[!define MUI_ICON "${SOURCE_DIR}\\resources\\bdmvauthor.ico"]=]
    [=[!define MUI_UNICON "${SOURCE_DIR}\\resources\\bdmvauthor.ico"]=]
    [=[!insertmacro MUI_PAGE_LICENSE "${INSTALLER_LICENSE}"]=]
    "Apache License 2.0 displayed here applies to BDMV Author itself only")
  string(FIND "${NSIS}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows installer source regression: missing NSIS marker: ${needle}")
  endif()
endforeach()


# Native makensis options begin with /D.  Under MSYS2 those are path-converted
# unless the native invocation disables argument conversion.  Require the guard
# to be attached directly to makensis rather than merely exported elsewhere.
string(FIND "${BUILD}" [=[MSYS2_ARG_CONV_EXCL='*' "$ucrt_prefix/bin/makensis.exe"]=] argconv_pos)
string(FIND "${BUILD}" [=[/DVERSION=$version]=] define_pos)
if(argconv_pos EQUAL -1 OR define_pos EQUAL -1 OR argconv_pos GREATER define_pos)
  message(FATAL_ERROR "Windows installer build must suppress MSYS2 path conversion before passing /D options to native makensis.exe")
endif()

# The install location must follow Windows' 64-bit Program Files variable, not a
# drive-letter path or an independently hard-coded Program Files location.
string(FIND "${NSIS}" [=[InstallDir "$PROGRAMFILES64\\${PRODUCT_NAME}"]=] progfiles_pos)
if(progfiles_pos EQUAL -1)
  message(FATAL_ERROR "Windows installer default location must use the NSIS $PROGRAMFILES64 variable")
endif()

foreach(forbidden
    "C:\\Program Files"
    "C:/Program Files"
    [=[RMDir /r "$INSTDIR"]=]
    "third_party-source")
  string(FIND "${NSIS}" "${forbidden}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "Windows installer contains forbidden packaging/uninstall marker: ${forbidden}")
  endif()
endforeach()

# Failed removals must leave the registered uninstaller available for retry;
# registry deletion therefore belongs after the generated staged-file deletion
# include and its failure branch.
string(FIND "${NSIS}" [=[!include "${UNINSTALL_FILES_NSH}"]=] include_pos REVERSE)
string(FIND "${NSIS}" [=[DeleteRegKey HKLM "${UNINSTALL_KEY}"]=] regdelete_pos REVERSE)
if(include_pos EQUAL -1 OR regdelete_pos EQUAL -1 OR regdelete_pos LESS include_pos)
  message(FATAL_ERROR "Windows uninstaller must delete registered metadata only after staged files are removed successfully")
endif()


foreach(needle
    "The Apache License 2.0 text below applies to BDMV Author itself only."
    "third-party tools and runtime components"
    "THIRD_PARTY_NOTICES.md"
    "licenses directory")
  string(FIND "${LICENSE_HEADER}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Windows installer license notice regression: missing marker: ${needle}")
  endif()
endforeach()

message(STATUS "NSIS installer source/build/upgrade checks ok")
