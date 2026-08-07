# PadBridge

PadBridge is a free, personal Windows-to-iPad extended-display project. Its
target profile is an HP OMEN 15-en1097nr and an 11-inch M4 iPad Pro at the
iPad's native **2420 x 1668 at 120 Hz**, over either the supplied USB-C cable or
local Wi-Fi.

This repository is **checkpoint 1**, not a finished Sidecar replacement. It
already contains the real 120 Hz media path: an NVIDIA NVENC Windows sender, a
native iPad listener, VideoToolbox hardware H.264 decode, a latest-frame Metal
renderer, and touch/Apple Pencil return packets. It can send either a generated
test pattern or a Windows display captured with FFmpeg's GPU-native `ddagrab`.
The automatic virtual-monitor lifecycle, Windows touch injection, audio, and
installer are the next checkpoint.

## What is free, and the one Apple limitation

No paid Apple Developer membership is required. GitHub Actions builds the
unsigned iPad app on a macOS runner; AltStore/AltServer signs it on Windows with
a free Apple ID. Free Apple provisioning expires every seven days. AltServer
normally refreshes it automatically over Wi-Fi or USB, but if both devices miss
each other for over seven days you must refresh once. Apple provides no
permanent, native, no-membership signing route for a standalone iPad app.

The Apple-supplied USB-C charge cable is USB 2 (480 Mb/s). That is ample for the
default 60 Mb/s compressed stream even though it is not a video cable: PadBridge
sends data, not DisplayPort video.

## Repository map

- `windows-host` — C++20 protocol, TCP transport, FFmpeg/NVENC display source,
  unit tests, and USB helper scripts.
- `ipad-client` — SwiftUI shell, Network listener, VideoToolbox decoder, Metal
  NV12 renderer, and touch/Pencil capture.
- `protocol` — versioned byte-level wire specification.
- `.github/workflows` — free unsigned IPA and Windows executable builds.

## Fast first run

### 1. Build the unsigned IPA for free

1. Put this repository in a free GitHub repository.
2. Open **Actions > Build unsigned iPad IPA > Run workflow**.
3. Download the `PadBridge-unsigned-ipa` artifact.
4. Install AltServer on Windows and AltStore Classic on the iPad, then open the
   `.ipa` through AltStore. A separate free Apple ID used only for sideloading
   is reasonable.
5. Launch PadBridge once and allow Local Network access.

The workflow generates the Xcode project with XcodeGen and builds with code
signing disabled. AltStore performs the free local signing; no Mac is needed.

### 2. Build the Windows host

Install current NVIDIA drivers, CMake, Visual Studio 2022 Build Tools with the
C++ workload, and a Windows FFmpeg build that includes `h264_nvenc` and
`ddagrab`. Then run in PowerShell:

```powershell
cd windows-host
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### 3. Connect over the supplied USB-C cable

1. Trust the Windows PC on the iPad when prompted.
2. Obtain the free `iproxy.exe` from a libimobiledevice Windows build and put it
   at `windows-host\tools\iproxy.exe` (or on `PATH`).
3. Open PadBridge on the iPad.
4. In one PowerShell window run:

```powershell
.\windows-host\scripts\start-usb-bridge.ps1
```

5. In another window run the 120 Hz test pattern:

```powershell
.\windows-host\scripts\run-test-pattern.ps1
```

The iPad overlay should change to `Streaming` and report decoded FPS. If NVENC
cannot sustain native 120 Hz during this first diagnostic, retry with `-Fps 60`
to separate setup issues from 120 Hz tuning.

### 4. Send an actual extended Windows display

Install the free, signed Virtual Display Driver from
`VirtualDrivers/Virtual-Display-Driver`, add `2420x1668` at `120` to its options,
and choose **Extend these displays** in Windows Settings. Determine its FFmpeg
DXGI output index, then run (try `0`, `1`, and `2` as needed):

```powershell
.\windows-host\scripts\run-test-pattern.ps1 -Display 1
```

At this checkpoint the virtual display must be enabled and positioned manually.
Laptop keyboard and touchpad control it normally because Windows sees it as a
real extended monitor. iPad touch/Pencil packets already return to the host and
are drained, but Windows pointer/pen injection lands in checkpoint 2.

### Wi-Fi diagnostic

Skip `iproxy`, leave PadBridge open, allow TCP port `52100` on the private
Windows network, find the iPad's local IP in Wi-Fi settings, and run:

```powershell
.\windows-host\build\Release\padbridge_host.exe --host 192.168.1.50 --display 1
```

This first Wi-Fi path uses low-delay TCP. Checkpoint 2 replaces video-over-Wi-Fi
with a loss-tolerant datagram channel and automatic Bonjour discovery.

## 120 Hz troubleshooting order

1. Confirm the virtual display itself is set to 120 Hz in Advanced display.
2. Confirm `ffmpeg -encoders` contains `h264_nvenc` and `ffmpeg -filters`
   contains `ddagrab`.
3. Keep the OMEN plugged in and select its high-performance/NVIDIA mode.
4. Start at 60 Mb/s over USB; lower to 40 Mb/s if the cable path is unstable.
5. Read the on-iPad decoded FPS counter. It measures decoder output, so a steady
   120 verifies capture/encode/transport/decode before display cadence tuning.

## Next implementation checkpoint

- Install/enable/disable the signed virtual monitor from a Windows service and
  restore its placement after reconnect.
- Replace process-based FFmpeg control with an in-process D3D11/NVENC pipeline
  and one-frame queues.
- Convert normalized touch/Pencil packets to Windows synthetic pointer and pen
  input.
- Add audio loopback capture and iPad playback.
- Add discovery, UDP/QUIC media, reconnect, USB/Wi-Fi handoff, and a one-click
  installer plus iPad charger automation instructions.

## Upstream tools

- [AltStore Classic and AltServer](https://altstore.io/)
- [AltServer free-signing/refresh documentation](https://faq.altstore.io/altstore-classic/altserver)
- [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
- [libimobiledevice / iproxy](https://github.com/libimobiledevice/libusbmuxd)
- [FFmpeg](https://ffmpeg.org/)

The code is MIT licensed.
