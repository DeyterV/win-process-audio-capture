# win-process-audio-capture

OBS Studio plugin that adds an audio capture source which records audio from a specific Windows process, selected by executable name and PID — without requiring a visible window.

## Features

- Captures audio from any process via Windows process loopback (WASAPI)
- Select process by exe name + PID from a dropdown list of processes that currently have an audio session
- Refresh button to update the process list
- Automatically reconnects if the process restarts
- No virtual audio cable required

## Requirements

- Windows 10 2004 (build 19041) or later
- OBS Studio 31.0 or later

## Installation

1. Download the latest `.zip` from [Releases](https://github.com/DeyterV/win-process-audio-capture/releases)
2. Extract the zip — it produces a `win-process-audio-capture/` folder
3. Place that folder into one of:
   - `%APPDATA%\obs-studio\plugins\` — per-user install, no admin rights required
   - `<obs-dir>\obs-plugins\64bit\` for the `.dll` and `<obs-dir>\data\obs-plugins\win-process-audio-capture\` for the `data\` folder — system-wide install
4. Restart OBS Studio
5. Add a new source: **Audio Capture (Process)** → select the target process from the list

## Building from source

Requires Visual Studio 2022 and CMake 3.28+.

```
git clone https://github.com/DeyterV/win-process-audio-capture.git
cd win-process-audio-capture
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Dependencies (OBS headers) are downloaded automatically by the build system.

## License

[GPL-2.0-or-later](LICENSE)
