; IDM Next NSIS 安装脚本
; 版本由 package.ps1 / CI 通过 /DAPPVERSION=x.y.z 注入。

!ifndef APPVERSION
  !define APPVERSION "0.1.0"
!endif
!ifndef OUTDIR
  !define OUTDIR "installer\out"
!endif

!define APPNAME      "IDM Next"
!define PUBLISHER    "IDM Next Project"
!define EXECUTABLE   "idm-next.exe"
!define HOSTEXE      "idm-next-host.exe"
!define UNINST_KEY   "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

Unicode true
Name "${APPNAME} ${APPVERSION}"
OutFile "${OUTDIR}\idm-next-${APPVERSION}-setup-x64.exe"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"

Section "Main" SecMain
  SetOutPath "$INSTDIR"
  File /r "installer\dist\*.*"

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${EXECUTABLE}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk" "$INSTDIR\uninstall.exe"
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXECUTABLE}"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName"      "${APPNAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion"  "${APPVERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon"     "$INSTDIR\${EXECUTABLE},0"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk"
  RMDir  "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"

  ; 先清理注册表，再删除安装目录。
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKCR "magnet"
  RMDir /r "$INSTDIR"
SectionEnd

Function RegisterMagnetProtocol
  WriteRegStr HKCR "magnet" "" "URL:magnet protocol"
  WriteRegStr HKCR "magnet" "URL Protocol" ""
  WriteRegStr HKCR "magnet\shell\open\command" "" '"$INSTDIR\${EXECUTABLE}" "%1"'
FunctionEnd
