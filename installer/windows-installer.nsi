; ============================================================
; R GUI 2 – Windows NSIS Installer Script
; Build with:
;   makensis /DAPP_VERSION=1.2.3 windows-installer.nsi
; ============================================================

!ifndef APP_VERSION
  !define APP_VERSION "1.0.0"
!endif

!define APP_NAME        "R GUI 2"
!define APP_EXE         "rgui2.exe"
!define APP_PUBLISHER   "R GUI 2"
!define REG_UNINST_KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\R-GUI-2"
!define REG_APP_KEY     "Software\R-GUI-2"

Name          "${APP_NAME} ${APP_VERSION}"
OutFile       "R-GUI-2-${APP_VERSION}-Setup.exe"
InstallDir    "$PROGRAMFILES64\R GUI 2"
InstallDirRegKey HKLM "${REG_APP_KEY}" "InstallLocation"
RequestExecutionLevel admin
Unicode True

; -- Modern UI ----------------------------------------------------------------
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"
!include "WinMessages.nsh"

Var Rscript

!define MUI_ABORTWARNING
!define MUI_ICON   "r-gui-2.ico"
!define MUI_UNICON "r-gui-2.ico"
!define MUI_WELCOMEPAGE_TEXT \
  "This wizard will install ${APP_NAME} ${APP_VERSION} on your computer.$\r$\n$\r$\n\
   R GUI 2 is a lightweight Qt-based IDE for the R programming language.$\r$\n$\r$\n\
   This installer bundles the rgui2 R package. An existing R installation is required.$\r$\n$\r$\n\
   Click Next to continue."
!define MUI_FINISHPAGE_RUN         "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT    "Launch R GUI 2"

; -- Installer pages -----------------------------------------------------------
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; -- Uninstaller pages ---------------------------------------------------------
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; -- Version metadata embedded in the EXE --------------------------------------
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName"     "${APP_NAME}"
VIAddVersionKey "ProductVersion"  "${APP_VERSION}"
VIAddVersionKey "CompanyName"     "${APP_PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_NAME} Installer"
VIAddVersionKey "FileVersion"     "${APP_VERSION}"
VIAddVersionKey "LegalCopyright"  "(c) ${APP_PUBLISHER}"

; ===============================================================================
; Main install section
; ===============================================================================
Section "R GUI 2 (required)" SecMain
  SectionIn RO   ; cannot be deselected

  ; ── Visual C++ Redistributable (VCRUNTIME140 / MSVCP140) ───────────────────
  DetailPrint "Installing Visual C++ Redistributable..."
  SetOutPath "$TEMP"
  File "/oname=vc_redist.exe" "staging\vc_redist.x64.exe"
  ExecWait '"$TEMP\vc_redist.exe" /install /quiet /norestart'
  Delete "$TEMP\vc_redist.exe"

  ; ── Application files (exe + Qt6 DLLs + MinGW runtime) ────────────────
  SetOutPath "$INSTDIR"
  File /r "staging\*.*"

  ; README (for MUI finish page)
  File "..\README.md"

  ; -- Gogh colour themes ---------------------------------------------------
  SetOutPath "$INSTDIR\gogh-themes"
  File /r "staging\gogh-themes\*.*"

  ; -- Bundled fonts --------------------------------------------------------
  SetOutPath "$INSTDIR\fonts"
  File /r "staging\fonts\*.*"

  ; -- rgui2 R companion package source (for post-install R setup) ----------
  SetOutPath "$INSTDIR\rgui2pkg"
  File /r "staging\rgui2pkg\*.*"

  ; -- rgui2 pre-built package tarball ---------------------------------------
  SetOutPath "$INSTDIR"
  File "staging\rgui2pkg.tar.gz"

  ; -- Shortcuts ------------------------------------------------------------
  CreateDirectory "$SMPROGRAMS\R GUI 2"
  CreateShortcut  "$SMPROGRAMS\R GUI 2\R GUI 2.lnk"           "$INSTDIR\${APP_EXE}"
  CreateShortcut  "$SMPROGRAMS\R GUI 2\Uninstall R GUI 2.lnk" "$INSTDIR\uninstall.exe"
  CreateShortcut  "$DESKTOP\R GUI 2.lnk"                      "$INSTDIR\${APP_EXE}"

  ; -- Registry (Add/Remove Programs) --------------------------------------
  WriteRegStr   HKLM "${REG_APP_KEY}"    "InstallLocation"   "$INSTDIR"
  WriteRegStr   HKLM "${REG_UNINST_KEY}" "DisplayName"       "${APP_NAME}"
  WriteRegStr   HKLM "${REG_UNINST_KEY}" "DisplayVersion"    "${APP_VERSION}"
  WriteRegStr   HKLM "${REG_UNINST_KEY}" "Publisher"         "${APP_PUBLISHER}"
  WriteRegStr   HKLM "${REG_UNINST_KEY}" "InstallLocation"   "$INSTDIR"
  WriteRegStr   HKLM "${REG_UNINST_KEY}" "DisplayIcon"       "$INSTDIR\${APP_EXE}"
  WriteRegStr   HKLM "${REG_UNINST_KEY}" "UninstallString"   '"$INSTDIR\uninstall.exe"'
  WriteRegStr   HKLM "${REG_UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoModify"          1
  WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoRepair"          1

  ; -- Write uninstaller ----------------------------------------------------
  WriteUninstaller "$INSTDIR\uninstall.exe"


SectionEnd

; ===============================================================================
; Pre-built R packages (jsonlite + rgui2) — requires an existing R installation
; ===============================================================================
Section "rgui2 R package (requires existing R installation)" SecRPkg

  ; Detect an existing R installation via the registry (set by the official
  ; R-core installer). We do not bundle/install R itself anymore.
  ReadRegStr $0 HKLM "SOFTWARE\R-core\R64" "InstallPath"
  ${If} $0 == ""
    ReadRegStr $0 HKCU "SOFTWARE\R-core\R64" "InstallPath"
  ${EndIf}

  ${If} $0 == ""
    MessageBox MB_ICONEXCLAMATION|MB_OK \
      "No existing R installation was found. Please install R from https://cran.r-project.org, then re-run this installer to copy the bundled R packages."
  ${Else}
    DetailPrint "Found R installation at $0"
    DetailPrint "Copying R packages..."
    SetOutPath "$0\library"
    File /r "staging\R-library\*"
  ${EndIf}

SectionEnd

; ===============================================================================
; Uninstaller
; ===============================================================================
Section "Uninstall"

  ; Remove application directory
  RMDir /r "$INSTDIR"

  ; Remove shortcuts
  Delete    "$DESKTOP\R GUI 2.lnk"
  RMDir /r  "$SMPROGRAMS\R GUI 2"

  ; Remove registry entries
  DeleteRegKey HKLM "${REG_UNINST_KEY}"
  DeleteRegKey HKLM "${REG_APP_KEY}"

SectionEnd

; -- Section descriptions (shown on the components page) -----------------------
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain}    "R GUI 2 application files (required)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecRPkg}    "Copies jsonlite and rgui2 R packages into your existing R installation."
!insertmacro MUI_FUNCTION_DESCRIPTION_END
