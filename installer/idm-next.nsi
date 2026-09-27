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
!define NM_HOST      "com.tencent.idm_next"
!define NM_MANIFEST  "$INSTDIR\browser-extension\native-messaging-host\com.tencent.idm_next.json"

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
  ; makensis 可能从任意工作目录启动，必须以本 .nsi 文件所在目录定位 dist。
  File /r "${__FILEDIR__}\dist\*.*"

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

  ; 安装器本身是 machine-wide / 管理员权限，因此 Native Messaging Host 必须写 HKLM。
  ; 若标准用户输入管理员凭据，写 HKCU 会落到管理员账户而不是实际浏览器用户。
  IfFileExists "$INSTDIR\browser-extension\native-messaging-host\install_host.ps1" 0 nmhost_done
    ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\browser-extension\native-messaging-host\install_host.ps1" -Scope Machine' $0
    DetailPrint "Native Messaging Host (Machine) 注册返回码: $0"
  nmhost_done:
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk"
  RMDir  "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"

  ; 新安装版注册在 HKLM，所有用户可发现。卸载只删除本项目 host。
  DeleteRegKey HKLM "Software\Google\Chrome\NativeMessagingHosts\${NM_HOST}"
  DeleteRegKey HKLM "Software\Microsoft\Edge\NativeMessagingHosts\${NM_HOST}"

  ; 兼容早期安装器曾错误写入当前用户 HKCU 的版本：仅当值仍指向本次安装目录时清理，
  ; 避免误删用户后来为便携版重新注册的同名 host。
  ReadRegStr $0 HKCU "Software\Google\Chrome\NativeMessagingHosts\${NM_HOST}" ""
  StrCmp $0 "${NM_MANIFEST}" 0 +2
    DeleteRegKey HKCU "Software\Google\Chrome\NativeMessagingHosts\${NM_HOST}"
  ReadRegStr $0 HKCU "Software\Microsoft\Edge\NativeMessagingHosts\${NM_HOST}" ""
  StrCmp $0 "${NM_MANIFEST}" 0 +2
    DeleteRegKey HKCU "Software\Microsoft\Edge\NativeMessagingHosts\${NM_HOST}"

  DeleteRegKey HKLM "${UNINST_KEY}"

  RMDir /r "$INSTDIR"
SectionEnd
