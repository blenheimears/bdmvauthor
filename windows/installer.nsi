Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WordFunc.nsh"
!insertmacro VersionCompare

!ifndef VERSION
  !error "VERSION must be supplied by the build script"
!endif
!ifndef VERSION4
  !error "VERSION4 must be supplied by the build script"
!endif
!ifndef STAGE_DIR
  !error "STAGE_DIR must be supplied by the build script"
!endif
!ifndef SOURCE_DIR
  !error "SOURCE_DIR must be supplied by the build script"
!endif
!ifndef OUT_FILE
  !error "OUT_FILE must be supplied by the build script"
!endif
!ifndef UNINSTALL_FILES_NSH
  !error "UNINSTALL_FILES_NSH must be supplied by the build script"
!endif
!ifndef INSTALLER_LICENSE
  !error "INSTALLER_LICENSE must be supplied by the build script"
!endif

!define PRODUCT_NAME "BDMV Author"
!define PRODUCT_EXE "bdmvauthor.exe"
!define PRODUCT_CLI_EXE "bdmvauthor-cli.exe"
!define PRODUCT_REG_KEY "Software\\BDMV Author"
!define UNINSTALL_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BDMV Author"
!define APP_PATH_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\bdmvauthor.exe"

Name "${PRODUCT_NAME} ${VERSION}"
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\\${PRODUCT_NAME}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${PRODUCT_NAME} ${VERSION}"
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION4}"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "${PRODUCT_NAME} installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "BDMV Author contributors"

!define MUI_ABORTWARNING
!define MUI_ICON "${SOURCE_DIR}\\resources\\bdmvauthor.ico"
!define MUI_UNICON "${SOURCE_DIR}\\resources\\bdmvauthor.ico"
!define MUI_LICENSEPAGE_TEXT_TOP "The Apache License 2.0 displayed here applies to BDMV Author itself only. Bundled third-party tools and runtime components retain their own licenses; see the notice below and the installed THIRD_PARTY_NOTICES.md / licenses directory."
!define MUI_FINISHPAGE_RUN "$INSTDIR\\${PRODUCT_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Run ${PRODUCT_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${INSTALLER_LICENSE}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP|MB_OK "${PRODUCT_NAME} requires 64-bit Windows."
    Abort
  ${EndIf}

  SetShellVarContext all
  SetRegView 64

  ; Installer-based releases share one uninstall key.  If one is present, remove
  ; it before copying any new files so an upgrade cannot retain stale DLLs or
  ; plugins.  Keep the previous installation directory for the replacement.
  ReadRegStr $0 HKLM "${UNINSTALL_KEY}" "InstallLocation"
  ReadRegStr $3 HKLM "${UNINSTALL_KEY}" "DisplayVersion"
  ${If} $0 != ""
    ; Do not let an older setup silently replace a newer installed release.
    ${If} $3 != ""
      ${VersionCompare} "${VERSION}" "$3" $4
      ${If} $4 == 2
        MessageBox MB_ICONSTOP|MB_OK "A newer ${PRODUCT_NAME} version ($3) is already installed. This ${VERSION} installer will not downgrade it."
        Abort
      ${EndIf}
    ${EndIf}

    IfFileExists "$0\\Uninstall.exe" old_uninstaller_found old_uninstaller_missing

old_uninstaller_found:
    ; NSIS uninstallers normally copy themselves to %TEMP%.  Copy it ourselves
    ; and use _?= so ExecWait receives the real uninstaller error level; this
    ; lets setup fail safely if locked files prevent a clean upgrade.
    InitPluginsDir
    ClearErrors
    CopyFiles /SILENT "$0\\Uninstall.exe" "$PLUGINSDIR\\bdmvauthor-uninstall.exe"
    ${If} ${Errors}
      MessageBox MB_ICONSTOP|MB_OK "The installed ${PRODUCT_NAME} uninstaller could not be prepared. Setup will stop without overwriting the current installation."
      Abort
    ${EndIf}
    ExecWait '"$PLUGINSDIR\\bdmvauthor-uninstall.exe" /S _?=$0' $2
    ${If} $2 != 0
      MessageBox MB_ICONSTOP|MB_OK "The installed copy of ${PRODUCT_NAME} could not be removed cleanly (uninstaller exit code $2). Setup will stop without overwriting it."
      Abort
    ${EndIf}
    IfFileExists "$0\\${PRODUCT_EXE}" 0 old_install_removed
    MessageBox MB_ICONSTOP|MB_OK "The previous ${PRODUCT_NAME} installation could not be fully removed. Close ${PRODUCT_NAME} and any tools using its files, then run setup again."
    Abort

old_uninstaller_missing:
    ; A stale registry key alone is harmless, but an installed executable with no
    ; matching uninstaller is not safe to overwrite automatically.
    IfFileExists "$0\\${PRODUCT_EXE}" 0 stale_install_metadata
    MessageBox MB_ICONSTOP|MB_OK "An older ${PRODUCT_NAME} installation was found at $0, but its uninstaller is missing. Remove that installation manually before continuing."
    Abort

stale_install_metadata:
    DeleteRegKey HKLM "${APP_PATH_KEY}"
    DeleteRegKey HKLM "${UNINSTALL_KEY}"
    DeleteRegKey HKLM "${PRODUCT_REG_KEY}"

old_install_removed:
    StrCpy $INSTDIR $0
  ${Else}
    ; Retain the old location if a legacy/partial product key exists but there is
    ; no installer registration.  Fresh installs use $PROGRAMFILES64 above.
    ReadRegStr $0 HKLM "${PRODUCT_REG_KEY}" "InstallDir"
    ${If} $0 != ""
      StrCpy $INSTDIR $0
    ${EndIf}
  ${EndIf}
FunctionEnd

Section "${PRODUCT_NAME}" SEC_MAIN
  SectionIn RO
  SetShellVarContext all
  SetRegView 64

  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\\*"

  WriteUninstaller "$INSTDIR\\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\\${PRODUCT_NAME}"
  CreateShortCut "$SMPROGRAMS\\${PRODUCT_NAME}\\${PRODUCT_NAME}.lnk" "$INSTDIR\\${PRODUCT_EXE}"
  CreateShortCut "$SMPROGRAMS\\${PRODUCT_NAME}\\Uninstall ${PRODUCT_NAME}.lnk" "$INSTDIR\\Uninstall.exe"

  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "InstallDir" "$INSTDIR"

  WriteRegStr HKLM "${APP_PATH_KEY}" "" "$INSTDIR\\${PRODUCT_EXE}"
  WriteRegStr HKLM "${APP_PATH_KEY}" "Path" "$INSTDIR"

  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "BDMV Author"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\\${PRODUCT_EXE},0"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '$\"$INSTDIR\\Uninstall.exe$\"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '$\"$INSTDIR\\Uninstall.exe$\" /S'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64
  StrCpy $0 0

  ; Generated from the exact staged runtime at installer-build time.  It deletes
  ; only files installed by BDMV Author and removes subdirectories only when
  ; empty, so user-added files in a custom destination are never recursively
  ; erased.  The generated list leaves Uninstall.exe in place until every staged
  ; file has been removed successfully.
  !include "${UNINSTALL_FILES_NSH}"

  ${If} $0 != 0
    ; Keep the uninstall registration, shortcuts, and uninstaller so a failed
    ; removal can be retried after locked files are released.  During an upgrade
    ; this nonzero status is propagated to the new setup through ExecWait/_?=.
    SetErrorLevel 1
    IfSilent uninstall_done
    MessageBox MB_ICONEXCLAMATION|MB_OK "Some installed ${PRODUCT_NAME} files could not be removed. Close programs using the installation and run the uninstaller again."
    Goto uninstall_done
  ${EndIf}

  Delete "$SMPROGRAMS\\${PRODUCT_NAME}\\${PRODUCT_NAME}.lnk"
  Delete "$SMPROGRAMS\\${PRODUCT_NAME}\\Uninstall ${PRODUCT_NAME}.lnk"
  RMDir "$SMPROGRAMS\\${PRODUCT_NAME}"

  DeleteRegKey HKLM "${APP_PATH_KEY}"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "${PRODUCT_REG_KEY}"

  Delete "$INSTDIR\\Uninstall.exe"
  ; This removes the product directory only if it is empty. User-added files are
  ; intentionally preserved.
  RMDir "$INSTDIR"

uninstall_done:
SectionEnd
