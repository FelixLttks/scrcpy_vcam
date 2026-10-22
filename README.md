# scrcpy-vcam

Use your Android phone's screen and camera as a webcam on Windows 11, over USB.

scrcpy-vcam creates one virtual camera, "scrcpy-vcam", that any app can select (PowerPoint, Teams,
Zoom, OBS, browsers). It was made for PowerPoint Cameo, which can only use one camera per presentation:
scrcpy-vcam switches between the phone's screen and camera depending on the current slide.

It uses the server of [scrcpy](https://github.com/Genymobile/scrcpy) to capture the phone. It is not
affiliated with the scrcpy project.

## Requirements

- Windows 11
- [scrcpy](https://github.com/Genymobile/scrcpy) 4.x on `PATH` (`winget install Genymobile.scrcpy`).
  scrcpy-vcam uses its `scrcpy-server` file and `adb` from `PATH` (falls back to the `adb.exe` next to scrcpy).
  scrcpy is not bundled.
- Android phone with USB debugging enabled. Android 12 or newer for the camera.
- To build: Visual Studio 2019+ or Build Tools with "Desktop development with C++", and the Windows 11 SDK.

## Build and install

```
build.cmd
install.cmd
```

`install.cmd` asks for admin rights. It copies `scrcpy-vcam-source.dll` to `%ProgramData%\scrcpy-vcam`
and registers it. This is needed because the Windows Camera Frame Server service loads the camera source
and only looks in `HKEY_LOCAL_MACHINE`. Run `install.cmd` again after rebuilding the DLL.
`uninstall.cmd` removes it.

## Usage

1. Connect the phone and run `bin\scrcpy-vcam.exe`. The camera exists while it runs.
2. In PowerPoint, insert a Cameo and select "scrcpy-vcam" under Camera Format > Preview.
3. To pick the source per slide, rename the cameo in the Selection Pane (Alt+F10) so that its name
   contains `camera` or `screen`. scrcpy-vcam checks the current slide (slide show or editor) every
   100 ms. Slides without such a name keep the current source.
   You can also press `C` (camera) or `S` (screen) in the scrcpy-vcam window.

The output is always 1920x1080 at 30 fps. The screen is centered with black bars; crop the cameo to fit.
The camera is captured in 16:9. The phone's display stays on while scrcpy-vcam runs.

```
scrcpy-vcam [--front] [--no-camera] [--start-camera] [--max-fps N] [--bit-rate N]

  --front         use the front camera instead of the back camera
  --no-camera     stream the screen only
  --start-camera  start with the camera instead of the screen
  --max-fps N     screen frame rate limit (default 60)
  --bit-rate N    bits per second (default 16000000)
```

## How it works

```
phone: scrcpy-server (screen) ─┐
phone: scrcpy-server (camera) ─┴─ adb ─> scrcpy-vcam.exe ── shared memory ──> scrcpy-vcam-source.dll ──> app
                                         (H.264 decode)                         (in Frame Server)
```

- `scrcpy-vcam.exe` starts two scrcpy-server instances over adb, decodes both H.264 streams with the
  Windows H.264 decoder and writes the active one into shared memory. It also registers the virtual
  camera (`MFCreateVirtualCamera`) for as long as it runs.
- `scrcpy-vcam-source.dll` is the camera's media source. It runs inside the Frame Server service and
  delivers the newest frame at a fixed 30 fps, like a real webcam.
- Both streams run all the time, so switching is instant.

## Troubleshooting

- `bin\testcap.exe` opens the camera like an app, prints the frame rate and saves `testcap.bmp`.
- The camera source logs to `%ProgramData%\scrcpy-vcam\source.log`.
- Close other tools that use the phone camera through scrcpy (scrcpy itself, Plugcam, ...).
  Android only lets one process use the camera at a time.
- The screen stream only sends frames when the screen content changes, so 0 fps in the status line
  is normal for a still screen.

## License

MIT, see [LICENSE](LICENSE). scrcpy is licensed under the Apache License 2.0.
