; Inno Setup script template for build.yml's "Package Windows Installer"
; step (cpp-plugin's own README doesn't document this — it's CI-only).
;
; {{PROJECT_NAME}}, {{VERSION}}, {{ARTIFACTS}}, {{COMPANY_NAME}}, and
; {{PROJECT_BUNDLE_ID}} are plain-text placeholders substituted by that
; step's PowerShell (-replace) before ISCC ever sees this file — by the
; time Inno Setup compiles it, none of the double-brace tokens remain, so
; they never collide with Inno's own single-brace constant syntax
; ({app}, {pf}, {autopf}, ...).
;
; {{ARTIFACTS}} is the build's *_artefacts/<config> directory — it
; contains VST3/, CLAP/, and Standalone/ subfolders, whichever the build
; actually produced (AU is macOS-only, so it's never in this list).

[Setup]
AppId={{PROJECT_BUNDLE_ID}}
AppName={{PROJECT_NAME}}
AppVersion={{VERSION}}
AppPublisher={{COMPANY_NAME}}
DefaultDirName={autopf}\{{COMPANY_NAME}}\{{PROJECT_NAME}}
DisableProgramGroupPage=yes
OutputDir={#SourcePath}Output
OutputBaseFilename={{PROJECT_NAME}}_Setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
DisableWelcomePage=no

[Files]
; VST3 — standard machine-wide location every VST3 host scans.
Source: "{{ARTIFACTS}}\VST3\{{PROJECT_NAME}}.vst3\*"; \
  DestDir: "{commoncf64}\VST3\{{PROJECT_NAME}}.vst3"; \
  Flags: ignoreversion recursesubdirs createallsubdirs; \
  Check: DirExists(ExpandConstant('{{ARTIFACTS}}\VST3\{{PROJECT_NAME}}.vst3'))

; CLAP — standard machine-wide location.
Source: "{{ARTIFACTS}}\CLAP\{{PROJECT_NAME}}.clap"; \
  DestDir: "{commoncf64}\CLAP"; \
  Flags: ignoreversion; \
  Check: FileExists(ExpandConstant('{{ARTIFACTS}}\CLAP\{{PROJECT_NAME}}.clap'))

; Standalone app.
Source: "{{ARTIFACTS}}\Standalone\{{PROJECT_NAME}}.exe"; \
  DestDir: "{app}"; \
  Flags: ignoreversion; \
  Check: FileExists(ExpandConstant('{{ARTIFACTS}}\Standalone\{{PROJECT_NAME}}.exe'))

[Icons]
Name: "{autoprograms}\{{PROJECT_NAME}}"; Filename: "{app}\{{PROJECT_NAME}}.exe"; \
  Check: FileExists(ExpandConstant('{app}\{{PROJECT_NAME}}.exe'))
