; Map the Windows UI language, not the region/format locale. The same macro is
; used at install and as an uninstall fallback. Users can override in the dialog.
!macro DetectInstallerLanguage
  System::Call 'kernel32::GetUserDefaultUILanguage() i.r0'
  ${If} $0 == 1028 ; zh-TW
  ${OrIf} $0 == 3076 ; zh-HK
  ${OrIf} $0 == 5124 ; zh-MO
    StrCpy $LANGUAGE 1028
  ${ElseIf} $0 == 2052 ; zh-CN
  ${OrIf} $0 == 4100 ; zh-SG
    StrCpy $LANGUAGE 2052
  ${Else}
    StrCpy $LANGUAGE 1033
  ${EndIf}
!macroend
