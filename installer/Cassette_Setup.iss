; Instalador de Cassette (Inno Setup 6)
;   "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\Cassette_Setup.iss
; Toma la compilación Release de build (cmake --build build --config Release --target CassettePlugin_VST3 CassettePlugin_Standalone)
#ifndef AppVer
  #define AppVer "1.1.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build\CassettePlugin_artefacts\Release"
#endif
#ifndef OutDir
  #define OutDir "..\installer_output"
#endif

[Setup]
AppId={{6C1F4B2A-3D8E-4C51-9A7B-CA55E77E0001}
AppName=Cassette
AppVersion={#AppVer}
AppVerName=Cassette {#AppVer} (Free)
AppPublisher=SVNDALPHON
VersionInfoVersion={#AppVer}
VersionInfoCompany=SVNDALPHON
VersionInfoDescription=Cassette {#AppVer} Setup
VersionInfoCopyright=Copyright (c) 2026 SVNDALPHON
AppCopyright=Copyright (c) 2026 SVNDALPHON
ShowLanguageDialog=yes
LanguageDetectionMethod=uilanguage
UninstallDisplayIcon={app}\Cassette.exe
DefaultDirName={autopf}\SVNDALPHON\Cassette
DisableProgramGroupPage=yes
OutputDir={#OutDir}
OutputBaseFilename=Cassette_{#AppVer}_Setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=Cassette {#AppVer} by SVNDALPHON
CloseApplications=yes

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"; LicenseFile: "LICENSE_es.txt"
Name: "en"; MessagesFile: "compiler:Default.isl"; LicenseFile: "LICENSE.txt"

[Messages]
es.WelcomeLabel2=Cassette {#AppVer} (gratis) by SVNDALPHON%n%nCinta lo-fi y espacio: wow & flutter estéreo, hiss, head bump, dropouts, saturación, bitcrush, crackle de vinilo, ancho estéreo, Room / Hall / Space y un knob de Edad. Incluye buscador y guardado de tus propias cintas.%n%nEl VST3 se instalará en:%n  C:\Program Files\Common Files\VST3\Cassette.vst3%n%nCierra tu DAW antes de continuar.
en.WelcomeLabel2=Cassette {#AppVer} (free) by SVNDALPHON%n%nLo-fi tape and space: stereo wow & flutter, hiss, head bump, dropouts, saturation, bitcrush, vinyl crackle, stereo width, Room / Hall / Space and an Age knob. Includes preset search and saving your own tapes.%n%nThe VST3 will be installed to:%n  C:\Program Files\Common Files\VST3\Cassette.vst3%n%nClose your DAW before continuing.

[CustomMessages]
es.TypeFull=Instalación completa (VST3 + Standalone)
es.TypeVst3=Solo el plugin VST3
es.CompVst3=Cassette VST3 (FL Studio, Ableton Live, Reaper, Studio One…)
es.CompStandalone=Cassette Standalone (sin DAW)
es.UninstallIcon=Desinstalar Cassette
en.TypeFull=Full installation (VST3 + Standalone)
en.TypeVst3=VST3 plugin only
en.CompVst3=Cassette VST3 (FL Studio, Ableton Live, Reaper, Studio One…)
en.CompStandalone=Cassette Standalone (no DAW needed)
en.UninstallIcon=Uninstall Cassette

[Types]
Name: "full"; Description: "{cm:TypeFull}"
Name: "vst3"; Description: "{cm:TypeVst3}"

[Components]
Name: "vst3"; Description: "{cm:CompVst3}"; Types: full vst3; Flags: fixed
Name: "standalone"; Description: "{cm:CompStandalone}"; Types: full

[Files]
Source: "{#BuildDir}\VST3\Cassette.vst3\*"; DestDir: "{commoncf64}\VST3\Cassette.vst3"; Components: vst3; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#BuildDir}\Standalone\Cassette.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "LICENSE_es.txt"; DestDir: "{app}"; DestName: "LICENSE.txt"; Languages: es; Flags: ignoreversion
Source: "LICENSE.txt"; DestDir: "{app}"; DestName: "LICENSE.txt"; Languages: en; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Cassette"; Filename: "{app}\Cassette.exe"; Components: standalone
Name: "{autoprograms}\{cm:UninstallIcon}"; Filename: "{uninstallexe}"

[Code]
// La interfaz usa Microsoft Edge WebView2 Runtime (viene con Windows 11; en Windows 10 puede faltar)
function IsWebView2Installed(): Boolean;
var
  verStr: String;
begin
  Result := RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', verStr) or
            RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', verStr) or
            RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', verStr);
  if verStr = '0.0.0.0' then
    Result := False;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  resCode: Integer;
  psCmd: String;
begin
  if (CurStep = ssPostInstall) and (not IsWebView2Installed()) then
  begin
    // Instalador oficial de Microsoft, silencioso
    psCmd := '-NoProfile -ExecutionPolicy Bypass -Command "$webClient = New-Object System.Net.WebClient; $webClient.DownloadFile(''https://go.microsoft.com/fwlink/p/?LinkId=2124703'', ''$env:TEMP\wv2setup.exe''); Start-Process ''$env:TEMP\wv2setup.exe'' -ArgumentList ''/silent /install'' -Wait"';
    Exec('powershell.exe', psCmd, '', SW_HIDE, ewWaitUntilTerminated, resCode);
  end;
end;
