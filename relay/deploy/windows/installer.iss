; Inno Setup 6 script for the Reading Companion Voice Relay (Windows x64).
;
; build.ps1 stages everything and invokes:
;   ISCC.exe /DAppVersion=<ver> /DStageDir=<stage> /DOutDir=<out> installer.iss
;
; Stage layout:
;   voice-relay\             PyInstaller onedir (voice-relay.exe + _internal)
;   voice-relay\dict-data\   offline dictionary cards
;   voice-relay-service.exe  WinSW renamed (service wrapper)
;   voice-relay-service.xml  WinSW configuration
;   write-env.ps1            idempotent env-file generator

#ifndef AppVersion
  #define AppVersion "dev"
#endif
#ifndef StageDir
  #error StageDir is not defined (run build.ps1)
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
AppId={{8F3C2D9A-5B71-4E0C-A6D2-3B9E4C1F7A21}
AppName=Reading Companion Voice Relay
AppVersion={#AppVersion}
AppPublisher=Reading Companion
AppPublisherURL=https://github.com/zhou19830318/Reading_Companion
DefaultDirName={autopf}\ReadingCompanionVoiceRelay
DefaultGroupName=Reading Companion Voice Relay
DisableProgramGroupPage=yes
OutputDir={#OutDir}
OutputBaseFilename=voice-relay-setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayIcon={app}\voice-relay\voice-relay.exe

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Setup page / 管理页面"; Filename: "http://127.0.0.1:8080/admin"
Name: "{group}\Uninstall Voice Relay / 卸载"; Filename: "{uninstallexe}"

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\write-env.ps1"" -EnvFile ""{commonappdata}\ReadingCompanion\voice-relay.env"" -DictDataDir ""{app}\voice-relay\dict-data"" -T2IAssetDir ""{commonappdata}\ReadingCompanion\run\t2i"" -Version ""{#AppVersion}"""; Flags: runhidden waituntilterminated; StatusMsg: "Generating config file..."
Filename: "{app}\voice-relay-service.exe"; Parameters: "install"; Flags: runhidden waituntilterminated ignoreerrors; StatusMsg: "Registering service..."
Filename: "{app}\voice-relay-service.exe"; Parameters: "start"; Flags: runhidden waituntilterminated ignoreerrors; StatusMsg: "Starting service..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Reading Companion Voice Relay"""; Flags: runhidden ignoreerrors
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Reading Companion Voice Relay"" dir=in action=allow protocol=TCP localport=8080 remoteip=localsubnet profile=private"; Flags: runhidden
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -Command ""for($i=0; $i -lt 30; $i++){ try { $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 http://127.0.0.1:8080/healthz; if ($r.StatusCode -eq 200) { New-Item -ItemType File -Force -Path '{tmp}\voice-relay-ready.txt' | Out-Null; exit 0 } } catch {} ; Start-Sleep -Seconds 1 }; exit 1"; Flags: runhidden waituntilterminated ignoreerrors; StatusMsg: "Waiting for the relay to become ready..."
Filename: "http://127.0.0.1:8080/admin"; Flags: shellexec nowait postinstall skipifsilent; Check: ServiceIsUp

[UninstallRun]
Filename: "{app}\voice-relay-service.exe"; Parameters: "stop"; Flags: runhidden waituntilterminated ignoreerrors; RunOnceId: "RelayStop"
Filename: "{app}\voice-relay-service.exe"; Parameters: "uninstall"; Flags: runhidden waituntilterminated ignoreerrors; RunOnceId: "RelayUninstall"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Reading Companion Voice Relay"""; Flags: runhidden ignoreerrors; RunOnceId: "RelayFirewall"

[Code]
var
  ExecResultCode: Integer;

function ServiceIsUp(): Boolean;
begin
  Result := FileExists(ExpandConstant('{tmp}\voice-relay-ready.txt'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    // Upgrade path: stop and unregister the old service so [Files] can
    // overwrite the running voice-relay.exe. A first install is a no-op.
    Exec(ExpandConstant('{app}\voice-relay-service.exe'), 'stop', '', SW_HIDE, ewWaitUntilTerminated, ExecResultCode);
    Exec(ExpandConstant('{app}\voice-relay-service.exe'), 'uninstall', '', SW_HIDE, ewWaitUntilTerminated, ExecResultCode);
  end;
end;
