# Starfield Audio Duck

A minimal SFSE native plugin for Starfield `1.16.244.0`. When an audio session other than Starfield is active on the Windows default playback device, the plugin temporarily changes Starfield's background music volume to the configured muted value. When external audio stops, it restores the in-game music volume captured before muting.

## Features

- Mutes only Starfield's background music; other game volume channels are unchanged.
- Uses Windows Core Audio session events instead of per-frame polling.
- Runs as one SFSE DLL; no separate helper executable is required.
- Ignores Starfield's own audio session by process ID.
- Applies state changes through one SFSE main-thread task and does not install a permanent per-frame task.

## Compatibility

This release targets Starfield `1.16.244.0`. The tested local combination is SFSE `0.2.21`.

The audio control code uses build-specific RVAs from `Starfield.exe` and reads the runtime music bus ID from the game's own music record. The parameters are not tied to a particular computer, Windows user profile, or MO2 mod list when the same unmodified game executable is used. Compatibility is not guaranteed after a game update, with a different executable build, or when another plugin takes over the relevant music functions or bus.

## Configuration

The configuration file is installed at:

```text
Data/SFSE/Plugins/StarfieldAudioDuck.ini
```

```ini
[Settings]
bEnable=1
fMutedMusicVolume=0.0
fRestoreDelaySeconds=0.5
bIncludeSystemSessions=1
```

- `bEnable`: set to `0` to disable the plugin without removing it.
- `fMutedMusicVolume`: music volume while external audio is active. `0.0` fully mutes music.
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

The log should show the plugin version, a successful Core Audio connection, and the captured initial music volume. Play or pause audio in a browser or media player and verify that only Starfield's music is muted and restored. Version `0.1.1` was validated in-game on Starfield `1.16.244.0`.

## Source

[GitHub repository](https://github.com/IVseIce/StarfieldAudioDuck)

## License

MIT. See [LICENSE](LICENSE).
