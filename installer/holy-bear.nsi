; HolyBear Edition packaging. Original NitLink and third-party notices are preserved.
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "FileFunc.nsh"

!define PRODUCT "NitLink HolyBear Edition"
!define TITLE "NitLink ${NUMERIC_VERSION} HolyBear Edition"
!define REGKEY "Software\HolyBearTW\NitLink HolyBear Edition"
!define UNINSTALLKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\NitLink-HolyBear-Edition"
Name "${TITLE}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT}"
InstallDirRegKey HKCU "${REGKEY}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetOverwrite on
ShowInstDetails show
ShowUninstDetails show
ManifestDPIAware true
VIProductVersion "${WINDOWS_VERSION}"
VIAddVersionKey "ProductName" "${TITLE}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${TITLE} Setup (x64)"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 NitLink Contributors"

!define MUI_ICON "..\assets\nitlink.ico"
!define MUI_UNICON "..\assets\nitlink.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "..\assets\installer\wizard@3x.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP_STRETCH AspectFitHeight
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "..\assets\installer\wizard@3x.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP_STRETCH AspectFitHeight
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "..\assets\installer\banner@3x.bmp"
!define MUI_HEADERIMAGE_BITMAP_STRETCH AspectFitHeight
!define MUI_HEADERIMAGE_UNBITMAP "..\assets\installer\banner@3x.bmp"
!define MUI_HEADERIMAGE_UNBITMAP_STRETCH AspectFitHeight
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_BGCOLOR "101827"
!define MUI_TEXTCOLOR "EDF2FA"
!define MUI_INSTFILESPAGE_COLORS "EDF2FA 101827"
!define MUI_ABORTWARNING
!define MUI_LANGDLL_ALLLANGUAGES
!define MUI_LANGDLL_WINDOWTITLE "${TITLE}"
!define MUI_LANGDLL_INFO "$(ChooseLanguage)"
!define MUI_WELCOMEPAGE_TITLE "$(WelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(WelcomeText)"
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_HEADER_TEXT "$(LicensePageTitle)"
!define MUI_PAGE_HEADER_SUBTEXT "$(LicensePageSubtitle)"
!define MUI_LICENSEPAGE_TEXT_TOP "$(LicensePageIntro)"
!define MUI_LICENSEPAGE_TEXT_BOTTOM "$(LicensePageConsent)"
!define MUI_LICENSEPAGE_BUTTON "$(LicensePageButton)"
!insertmacro MUI_PAGE_LICENSE "${LICENSE_RTF}"
!define MUI_PAGE_HEADER_TEXT "$(ComponentsPageTitle)"
!define MUI_PAGE_HEADER_SUBTEXT "$(ComponentsPageSubtitle)"
!define MUI_COMPONENTSPAGE_TEXT_TOP "$(ComponentsPageTop)"
!define MUI_COMPONENTSPAGE_TEXT_COMPLIST "$(ComponentsPageList)"
!define MUI_COMPONENTSPAGE_TEXT_DESCRIPTION_TITLE "$(ComponentsDescriptionTitle)"
!define MUI_COMPONENTSPAGE_TEXT_DESCRIPTION_INFO "$(ComponentsDescriptionHint)"
!insertmacro MUI_PAGE_COMPONENTS
!define MUI_PAGE_HEADER_TEXT "$(DirectoryPageTitle)"
!define MUI_PAGE_HEADER_SUBTEXT "$(DirectoryPageSubtitle)"
!insertmacro MUI_PAGE_DIRECTORY
!define MUI_PAGE_HEADER_TEXT "$(InstallPageTitle)"
!define MUI_PAGE_HEADER_SUBTEXT "$(InstallPageSubtitle)"
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchNitLink
!define MUI_FINISHPAGE_RUN_TEXT "$(LaunchText)"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_TITLE "$(FinishTitle)"
!define MUI_FINISHPAGE_TEXT "$(FinishText)"
; Themed Windows checkboxes can ignore foreground colors; use a light finish
; content area so the optional launch label remains readable at every DPI.
!undef MUI_BGCOLOR
!define MUI_BGCOLOR "F4F6FA"
!undef MUI_TEXTCOLOR
!define MUI_TEXTCOLOR "101827"
!insertmacro MUI_PAGE_FINISH
!undef MUI_BGCOLOR
!define MUI_BGCOLOR "101827"
!undef MUI_TEXTCOLOR
!define MUI_TEXTCOLOR "EDF2FA"
!define MUI_WELCOMEPAGE_TITLE "$(UninstallWelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(UninstallWelcome)"
!insertmacro MUI_UNPAGE_WELCOME
!define MUI_PAGE_HEADER_TEXT "$(UninstallConfirmTitle)"
!define MUI_PAGE_HEADER_SUBTEXT "$(UninstallConfirmSubtitle)"
!insertmacro MUI_UNPAGE_CONFIRM
!define MUI_PAGE_HEADER_TEXT "$(UninstallingTitle)"
!define MUI_PAGE_HEADER_SUBTEXT "$(UninstallingSubtitle)"
!insertmacro MUI_UNPAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "$(UninstallFinishTitle)"
!define MUI_FINISHPAGE_TEXT "$(UninstallFinish)"
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "TradChinese"
!insertmacro MUI_LANGUAGE "SimpChinese"
SetFont /LANG=${LANG_ENGLISH} "Segoe UI" 9
SetFont /LANG=${LANG_TRADCHINESE} "Segoe UI" 9
SetFont /LANG=${LANG_SIMPCHINESE} "Segoe UI" 9
!include "strings.nsh"
!include "language.nsh"
!insertmacro MUI_RESERVEFILE_LANGDLL

Function .onInit
  SetShellVarContext current
  SetRegView 64
  !insertmacro DetectInstallerLanguage
  ${GetParameters} $0
  ${GetOptions} $0 "/LANG=" $1
  ${If} $1 == 1033
  ${OrIf} $1 == 1028
  ${OrIf} $1 == 2052
    StrCpy $LANGUAGE $1
  ${EndIf}
  !insertmacro MUI_LANGDLL_DISPLAY
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "$(NeedX64)"
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "$(NeedWindows)"
    Abort
  ${EndIf}
  ReadRegStr $0 HKLM "Software\Microsoft\Windows NT\CurrentVersion" "CurrentBuildNumber"
  ${If} $0 < 17763
    MessageBox MB_OK|MB_ICONSTOP "$(NeedWindows)"
    Abort
  ${EndIf}
FunctionEnd

Function LaunchNitLink
  SetOutPath "$INSTDIR"
  Exec '"$INSTDIR\NitLink.exe"'
FunctionEnd

Section "$(CoreName)" Core
  SectionIn RO
  SetOutPath "$INSTDIR"
  !include "${INSTALL_FILES}"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${REGKEY}" "Language" "$LANGUAGE"
  WriteRegStr HKCU "${UNINSTALLKEY}" "DisplayName" "${TITLE}"
  WriteRegStr HKCU "${UNINSTALLKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALLKEY}" "Publisher" "NitLink Contributors / HolyBearTW"
  WriteRegStr HKCU "${UNINSTALLKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALLKEY}" "DisplayIcon" "$INSTDIR\NitLink.exe"
  WriteRegStr HKCU "${UNINSTALLKEY}" "URLInfoAbout" "https://github.com/HolyBearTW/nitlink-holybear-edition"
  WriteRegStr HKCU "${UNINSTALLKEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKCU "${UNINSTALLKEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegDWORD HKCU "${UNINSTALLKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALLKEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINSTALLKEY}" "EstimatedSize" ${PACKAGE_KB}
  CreateDirectory "$SMPROGRAMS\${PRODUCT}"
  ; Remove only this product's old localized uninstall shortcuts on upgrades.
  Delete "$SMPROGRAMS\${PRODUCT}\Uninstall HolyBear Edition.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\解除安裝 HolyBear Edition.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\卸载 HolyBear Edition.lnk"
  CreateShortcut "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk" "$INSTDIR\NitLink.exe"
  CreateShortcut "$SMPROGRAMS\${PRODUCT}\$(UninstallShortcut).lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section /o "$(DesktopName)" Desktop
  CreateShortcut "$DESKTOP\${PRODUCT}.lnk" "$INSTDIR\NitLink.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${Core} "$(CoreDescription)"
  !insertmacro MUI_DESCRIPTION_TEXT ${Desktop} "$(DesktopDescription)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function un.onInit
  SetShellVarContext current
  SetRegView 64
  ReadRegStr $LANGUAGE HKCU "${REGKEY}" "Language"
  ${If} $LANGUAGE == ""
    !insertmacro DetectInstallerLanguage
  ${EndIf}
FunctionEnd

Section "Uninstall"
  ; Only explicit release files are removed. Generated configuration, screenshots,
  ; VRR_CAP.txt and other user files are always retained, including silent uninstall.
  !include "${UNINSTALL_FILES}"
  Delete "$INSTDIR\Uninstall.exe"
  Delete "$DESKTOP\${PRODUCT}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\$(UninstallShortcut).lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\Uninstall HolyBear Edition.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\解除安裝 HolyBear Edition.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\卸载 HolyBear Edition.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT}"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTALLKEY}"
  DeleteRegKey HKCU "${REGKEY}"
SectionEnd
