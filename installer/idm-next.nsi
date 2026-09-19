; ============================================================
;  IDM Next 安装脚本 (NSIS)
;  用法见同目录 package.ps1 / package.sh：先用 windeployqt 收集运行时
;  到 installer/dist，再 makensis 本脚本生成安装包。
; ============================================================
!define APPNAME      "IDM Next"
!define APPVERSION   "0.1.0"
!define PUBLISHER    "IDM Next Project"
!define EXECUTABLE   "idm-next.exe"
!define HOSTEXE      "idm-next-host.exe"
!define UNINST_KEY   "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

Name "${APPNAME} ${APPVERSION}"
OutFile "installer\idm-next-setup-x64.exe"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "${UNINST_KEY}" "InstallLocation"
; 安装到 Program Files 需要写权限，请求管理员提权
RequestExecutionLevel admin

; ---------- 界面 ----------
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "SimpChinese"

; ---------- 安装段 ----------
Section "Main" SecMain
  SetOutPath "$INSTDIR"

  ; 打包前由 windeployqt 收集好的全部内容（Qt DLL + 插件 + 主程序 + 宿主）
  File /r "installer\dist\*.*"

  ; 开始菜单
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${EXECUTABLE}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk" "$INSTDIR\uninstall.exe"

  ; 桌面快捷方式
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXECUTABLE}"

  ; 写卸载程序
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; 添加/删除程序（ARP）注册
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName"     "${APPNAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${APPVERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher"      "${PUBLISHER}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon"    "$INSTDIR\${EXECUTABLE},0"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  ; 估计卸载大小（KB，DWORD）
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"

  ; 可选：magnet / 浏览器协议关联（取消下一行注释启用，详见 RegisterMagnetProtocol）
  ; Call RegisterMagnetProtocol
SectionEnd

; ---------- 卸载段 ----------
Section "Uninstall"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk"
  RMDir  "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"

  ; 删除整个安装目录（含 Qt 运行库）
  RMDir /r "$INSTDIR"

  DeleteRegKey HKLM "${UNINST_KEY}"
  ; 若曾注册 magnet 协议，一并清理
  DeleteRegKey HKCR "magnet"
SectionEnd

; ---------- magnet 协议关联（可选，默认未调用）----------
Function RegisterMagnetProtocol
  WriteRegStr HKCR "magnet" "" "URL:magnet protocol"
  WriteRegStr HKCR "magnet" "URL Protocol" ""
  WriteRegStr HKCR "magnet\shell\open\command" "" \
              '"$INSTDIR\${EXECUTABLE}" "%1"'
FunctionEnd
