# Starfield Audio Duck

A minimal SFSE native plugin for Starfield `1.16.244.0`. When an audio session other than Starfield remains active on the Windows default playback device for the configured minimum duration, the plugin applies the configured muted music volume. While external audio remains active, manual changes to Starfield's music slider are left untouched and become the next restore value. When external audio stops, the latest user-selected music volume is applied.

## Features

- Applies only on external-audio state transitions; manual volume changes are not overridden while external audio is active.
- Ignores external audio shorter than the configured activation threshold, avoiding repeated changes from notification sounds.
- Mutes only Starfield's background music; other game volume channels are unchanged.
- Uses Windows Core Audio session events instead of per-frame polling.
- Runs as one SFSE DLL; no separate helper executable is required.
- Ignores Starfield's own audio session by process ID.
- Applies transition changes through one SFSE main-thread task and does not install a permanent per-frame task.

## Compatibility

This release targets Starfield `1.16.244.0`. The tested local combination is SFSE `0.2.21`.

The audio control code uses build-specific RVAs from `Starfield.exe`, including the settings-slider call site used to remember manual music changes, and reads the runtime music bus ID from the game's own music record. The parameters are not tied to a particular computer, Windows user profile, or MO2 mod list when the same unmodified game executable is used. Compatibility is not guaranteed after a game update, with a different executable build, or when another plugin takes over the relevant music functions or bus.

## Configuration

The configuration file is installed at:

```text
Data/SFSE/Plugins/StarfieldAudioDuck.ini
```

For MO2, the archive install root is `SFSE/Plugins`; it does not contain an extra `Data` directory. For manual installation, merge the archive's `SFSE` directory into Starfield's `Data` directory.

```ini
[Settings]
bEnable=1
fMutedMusicVolume=0.0
fActivationDelaySeconds=2.5
fRestoreDelaySeconds=0.5
bIncludeSystemSessions=1
```

- `bEnable`: set to `0` to disable the plugin without removing it.
- `fMutedMusicVolume`: music volume while external audio is active. `0.0` fully mutes music.
- `fActivationDelaySeconds`: minimum continuous duration external audio must remain active before the plugin mutes music. The default is `2.5`; set it to `0` for immediate transitions.
- `fRestoreDelaySeconds`: delay before restoring music after external audio stops.
- `bIncludeSystemSessions`: count Windows audio sessions with process ID `0` as external audio when set to `1`.

## Building

```powershell
$env:XSE_SF_MODS_PATH = Read-Host 'Enter your MO2 mods directory'
xmake f -m releasedbg
xmake
xmake package
```

The install directory is controlled by `XSE_SF_MODS_PATH`; the build does not write directly to the Starfield game directory.

## Verification

After launching through SFSE, inspect:

```powershell
$log = Join-Path $env:USERPROFILE 'Documents\My Games\Starfield\SFSE\Logs\StarfieldAudioDuck.log'
Get-Content $log -Tail 100
```

The log should show the plugin version, a successful Core Audio connection, and the captured initial music volume. Test all of these cases:

- Start external audio for less than `fActivationDelaySeconds`: no music-volume transition occurs.
- Start external audio for longer than `fActivationDelaySeconds`: music changes to `fMutedMusicVolume` once.
- While external audio is active, change Starfield's music slider: the new value is left alone.
- Stop external audio: the latest slider value is applied after the configured delay.
- Change the slider while external audio is inactive, then start and stop external audio.

Version `0.1.4` targets Starfield `1.16.244.0`.

## Source

[GitHub repository](https://github.com/IVseIce/StarfieldAudioDuck)

## License

MIT. See [LICENSE](LICENSE).
