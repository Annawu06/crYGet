!ifndef APP_EXE
  !error "APP_EXE must point to the built Windows executable"
!endif
!ifndef SETUP_EXE
  !error "SETUP_EXE must point to the installer output"
!endif
!ifndef LICENSE_DIR
  !error "LICENSE_DIR must point to bundled third-party notices"
!endif
!ifndef APP_ICON
  !error "APP_ICON must point to the Windows icon"
!endif
!ifndef RUNTIME_DIR
  !error "RUNTIME_DIR must point to the application runtime dependency directory"
!endif

Unicode true
Name "crYGet"
OutFile "${SETUP_EXE}"
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
InstallDir "$LOCALAPPDATA\Programs\crYGet"
InstallDirRegKey HKCU "Software\crYGet" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Install"
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  File /oname=cryget-desktop.exe "${APP_EXE}"
  File "${RUNTIME_DIR}/*.dll"
  File /nonfatal "${RUNTIME_DIR}/icudtl.dat"
  SetOutPath "$INSTDIR\licenses"
  File "${LICENSE_DIR}/*"

  Delete "$SMPROGRAMS\crYGet\crYGet.lnk"
  Delete "$SMPROGRAMS\crYGet\Uninstall crYGet.lnk"
  RMDir "$SMPROGRAMS\crYGet"
  ClearErrors
  CreateShortcut "$SMPROGRAMS\crYGet.lnk" "$INSTDIR\cryget-desktop.exe"
  IfErrors 0 +2
    MessageBox MB_ICONEXCLAMATION "crYGet was installed, but its Start menu shortcut could not be created. Open $INSTDIR\cryget-desktop.exe directly."
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "Software\crYGet" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\crYGet" "DisplayName" "crYGet"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\crYGet" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\crYGet" "DisplayIcon" "$INSTDIR\cryget-desktop.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\crYGet" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\crYGet" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\crYGet" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  Delete "$SMPROGRAMS\crYGet.lnk"
  Delete "$SMPROGRAMS\crYGet\crYGet.lnk"
  Delete "$SMPROGRAMS\crYGet\Uninstall crYGet.lnk"
  RMDir "$SMPROGRAMS\crYGet"

  Delete "$INSTDIR\cryget-desktop.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\icudtl.dat"
  Delete "$INSTDIR\licenses\*"
  RMDir "$INSTDIR\licenses"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\crYGet"
  DeleteRegKey HKCU "Software\crYGet"
SectionEnd
