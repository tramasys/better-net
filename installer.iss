#if VER != 0x07010000
    #error Use Inno Setup 7.1.0
#endif

[Setup]
AppId=tramasys.better-net
AppName=better-net
AppVersion={#AppVersion}
AppVerName=better-net
AppPublisher=tramasys
AppPublisherURL=https://github.com/tramasys/better-net
AppSupportURL=https://github.com/tramasys/better-net/issues
DefaultDirName={autopf}\better-net
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupArchitecture=x64
MinVersion=10.0.14393
DisableProgramGroupPage=yes
DisableWelcomePage=yes
DisableReadyPage=yes
WizardStyle=modern dark
SetupIconFile=src\assets\better-net.ico
WizardSmallImageFile=src\assets\better-net.png
UninstallDisplayIcon={app}\better-net.exe
UninstallDisplayName=better-net
CloseApplications=yes
RestartApplications=no
OutputBaseFilename=better-net-{#AppVersion}-setup-x64
Compression=lzma2
SolidCompression=yes

[Tasks]
Name: "desktopicon"; Description: "Desktop shortcut"; Flags: unchecked

[Files]
Source: "{#AppBinary}"; DestDir: "{app}"; Flags: ignoreversion

[Run]
Filename: "{app}\better-net.exe"; Parameters: "--show"; Description: "Open better-net"; Flags: nowait postinstall skipifsilent

[Messages]
FinishedLabel=better-net is installed.

[Code]
function ReadShortcut(Path: String): Variant;
var
    Shell: Variant;
begin
    Shell := CreateOleObject('WScript.Shell');
    Result := Shell.CreateShortcut(Path);
end;

procedure RegisterExtraCloseApplicationsResources;
var
    Path: String;
    Link: Variant;
begin
    Path := ExpandConstant('{userprograms}\better-net.lnk');

    if FileExists(Path) then begin
        Link := ReadShortcut(Path);

        if CompareText(ExtractFileName(Link.TargetPath), 'better-net.exe') = 0 then begin
            RegisterExtraCloseApplicationsResource(Link.TargetPath);
        end;
    end;
end;

procedure InstallShortcut(Path: String);
var
    Link: Variant;
    Executable, StartupCommand: String;
begin
    Path := ExpandConstant(Path);
    Link := ReadShortcut(Path);

    if FileExists(Path) and (CompareText(ExtractFileName(Link.TargetPath), 'better-net.exe') <> 0) then begin
        Exit;
    end;

    Executable := ExpandConstant('{app}\better-net.exe');

    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'better-net', StartupCommand) and
       (StartupCommand = '"' + Link.TargetPath + '" --background') then begin
        if not RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'better-net',
                                   '"' + Executable + '" --background') then begin
            RaiseException('Could not update the better-net startup entry.');
        end;
    end;

    Link.TargetPath := Executable;
    Link.Arguments := '--show';
    Link.WorkingDirectory := ExpandConstant('{app}');
    Link.IconLocation := ExpandConstant('{app}\better-net.exe') + ',0';
    Link.Description := 'better-net';
    Link.Save;
end;

procedure CurStepChanged(Step: TSetupStep);
begin
    if Step = ssPostInstall then begin
        InstallShortcut('{userprograms}\better-net.lnk');

        if WizardIsTaskSelected('desktopicon') then begin
            InstallShortcut('{userdesktop}\better-net.lnk');
        end;
    end;
end;

procedure RemoveOwnedShortcut(Path: String);
var
    Link: Variant;
begin
    Path := ExpandConstant(Path);

    if FileExists(Path) then begin
        Link := ReadShortcut(Path);

        if CompareText(Link.TargetPath, ExpandConstant('{app}\better-net.exe')) = 0 then begin
            Link := Unassigned;

            if not DeleteFile(Path) then begin
                RaiseException('Could not remove the better-net shortcut.');
            end;
        end;
    end;
end;

procedure CurUninstallStepChanged(Step: TUninstallStep);
var
    ExitCode: Integer;
    Executable, StartupCommand: String;
begin
    if Step = usUninstall then begin
        Executable := ExpandConstant('{app}\better-net.exe');

        if FileExists(Executable) then begin
            if Exec(Executable, '--quit', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ExitCode) then begin
                if ExitCode <> 0 then begin
                    RaiseException('Exit better-net from the tray and try again.');
                end;
            end else if CheckForMutexes('Local\BetterNet.SingleInstance.1') then begin
                RaiseException('Could not close better-net. Exit it from the tray and try again.');
            end;
        end;

        RemoveOwnedShortcut('{userprograms}\better-net.lnk');
        RemoveOwnedShortcut('{userdesktop}\better-net.lnk');

        if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'better-net', StartupCommand) and
           (StartupCommand = '"' + Executable + '" --background') then begin
            if not RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'better-net') then begin
                RaiseException('Could not remove the better-net startup entry.');
            end;
        end;
    end;
end;
