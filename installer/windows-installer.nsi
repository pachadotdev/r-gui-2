; ============================================================
; Q R IDE – Windows NSIS Installer Script
; Build with:
;   makensis /DAPP_VERSION=1.2.3 windows-installer.nsi
; ============================================================

!ifndef APP_VERSION
  !define APP_VERSION "1.0.0"
!endif

!define APP_NAME        "Q R IDE"
!define APP_EXE         "q.exe"
!define APP_PUBLISHER   "Q R IDE Project"
!define REG_UNINST_KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\Q-R-IDE"
!define REG_APP_KEY     "Software\Q-R-IDE"

Name          "${APP_NAME} ${APP_VERSION}"
OutFile       "Q-R-IDE-${APP_VERSION}-Setup.exe"
InstallDir    "$PROGRAMFILES64\Q R IDE"
InstallDirRegKey HKLM "${REG_APP_KEY}" "InstallLocation"
RequestExecutionLevel admin
Unicode True

; ── Modern UI ────────────────────────────────────────────────────────────────
!include "MUI2.nsh"
!include "LogicLib.nsh"

!define MUI_ABORTWARNING
!define MUI_ICON   "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_WELCOMEPAGE_TEXT \
  "This wizard will install ${APP_NAME} ${APP_VERSION} on your computer.$\r$\n$\r$\n\
   Q is a lightweight Qt-based IDE for the R programming language.$\r$\n$\r$\n\
   R must already be installed. Download it from https://www.r-project.org before using Q.$\r$\n$\r$\n\
   Click Next to continue."
!define MUI_FINISHPAGE_RUN         "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT    "Launch Q R IDE"
!define MUI_FINISHPAGE_SHOWREADME  "$INSTDIR\README.md"

; ── Installer pages ───────────────────────────────────────────────────────────
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; ── Uninstaller pages ─────────────────────────────────────────────────────────
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ── Version metadata embedded in the EXE ──────────────────────────────────────
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName"     "${APP_NAME}"
VIAddVersionKey "ProductVersion"  "${APP_VERSION}"
VIAddVersionKey "CompanyName"     "${APP_PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_NAME} Installer"
VIAddVersionKey "FileVersion"     "${APP_VERSION}"
VIAddVersionKey "LegalCopyright"  "(c) ${APP_PUBLISHER}"

; ═══════════════════════════════════════════════════════════════════════════════
; Helper: check that R is installed (warns, does NOT block install)
; ═══════════════════════════════════════════════════════════════════════════════
Function CheckRInstalled
  nsExec::ExecToStack 'where Rscript'
  Pop $0  ; exit code
  Pop $1  ; stdout
  ${If} $0 != 0
    MessageBox MB_ICONINFORMATION|MB_OK \
      "R was not found on your PATH.$\r$\n$\r$\n\
       Please install R from https://www.r-project.org and re-run this installer, \
       or install R after this installer completes and then run:$\r$\n$\r$\n\
       Rscript -e ""install.packages('$INSTDIR\qiderpkg', repos=NULL, type='source')"""
  ${EndIf}
FunctionEnd

; ═══════════════════════════════════════════════════════════════════════════════
; Main install section
; ═══════════════════════════════════════════════════════════════════════════════
Section "Q R IDE (required)" SecMain
  SectionIn RO   ; cannot be deselected

  ; ── Application files (exe + Qt6 DLLs + MinGW runtime) ──────────────────
  SetOutPath "$INSTDIR"
  File /r "staging\*.*"

  ; README (for MUI finish page)
  File "..\README.md"

  ; ── Gogh colour themes ───────────────────────────────────────────────────
  SetOutPath "$INSTDIR\gogh-themes"
  File /r "staging\gogh-themes\*.*"

  ; ── Bundled fonts ────────────────────────────────────────────────────────
  SetOutPath "$INSTDIR\fonts"
  File /r "staging\fonts\*.*"

  ; ── qide R companion package source (for post-install R setup) ──────────
  SetOutPath "$INSTDIR\qiderpkg"
  File /r "staging\qiderpkg\*.*"

  SetOutPath "$INSTDIR"

  ; ── Shortcuts ────────────────────────────────────────────────────────────
  CreateDirectory "$SMPROGRAMS\Q R IDE"
  CreateShortcut  "$SMPROGRAMS\Q R IDE\Q R IDE.lnk"           "$INSTDIR\${APP_EXE}"
  CreateShortcut  "$SMPROGRAMS\Q R IDE\Uninstall Q R IDE.lnk" "$INSTDIR\uninstall.exe"
  CreateShortcut  "$DESKTOP\Q R IDE.lnk"                      "$INSTDIR\${APP_EXE}"

  ; ── Registry (Add/Remove Programs) ──────────────────────────────────────
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

  ; ── Write uninstaller ────────────────────────────────────────────────────
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; ── Install qide R package if R is present ───────────────────────────────
  Call CheckRInstalled
  nsExec::ExecToLog \
    'Rscript -e "install.packages(\"$INSTDIR\\qiderpkg\", repos=NULL, type=\"source\")"'

SectionEnd

; ═══════════════════════════════════════════════════════════════════════════════
; Uninstaller
; ═══════════════════════════════════════════════════════════════════════════════
Section "Uninstall"

  ; Remove application directory
  RMDir /r "$INSTDIR"

  ; Remove shortcuts
  Delete    "$DESKTOP\Q R IDE.lnk"
  RMDir /r  "$SMPROGRAMS\Q R IDE"

  ; Remove registry entries
  DeleteRegKey HKLM "${REG_UNINST_KEY}"
  DeleteRegKey HKLM "${REG_APP_KEY}"

SectionEnd
