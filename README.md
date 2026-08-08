# PadBridge

PadBridge turns an iPad into a real extended Windows display. This build is
optimized for an HP OMEN 15-en1097nr and an 11-inch M4 iPad Pro at its native
**2420 × 1668, 120 Hz** resolution.

It supports:

- A true extended Windows desktop, not screen mirroring.
- USB-C through Apple Mobile Device Service, with no `iproxy` or Python.
- Automatic local Wi-Fi discovery through Bonjour.
- NVIDIA NVENC H.264 capture with a GPU-resident zero-copy path.
- VideoToolbox hardware decode and Metal presentation on iPad.
- Adaptive 120 Hz: full rate during activity and approximately 1 Hz when idle.
- Windows touch and Apple Pencil input from the iPad.
- Correct aspect ratio, VSync presentation, and latest-frame rendering.
- Optional placement of newly opened apps on the display under the cursor.
- One-click connect/disconnect, tray controls, saved settings, and an installer.
- Automatic cleanup when the cable is removed or the iPad app closes.

## Everyday use

No terminal or PowerShell is used in normal operation.

1. Open **PadBridge** on the iPad.
2. Connect the USB-C cable, or keep both devices on the same local Wi-Fi.
3. Open **PadBridge** on Windows. Auto-connect is enabled by default.
4. Move the Windows cursor beyond the laptop screen onto the iPad.

USB is preferred automatically. The controller enables the extended display,
restores 2420 × 1668 at 120 Hz, starts the native host and bundled FFmpeg, and
routes touch/Pencil input back to the correct Windows monitor.

When an established connection disappears for three seconds, PadBridge stops
capture and NVENC, removes the iPad display when it is safe to do so, releases
the USB/Wi-Fi connection, and exits the Windows controller. Backgrounding the
iPad app stops its listener and decoder; a disconnected foreground iPad drops
to a 1 Hz waiting view and allows normal screen sleep.

## One-time setup

The current test devices already have these prerequisites:

1. **Apple Devices for Windows**, with the iPad paired and trusted.
2. **Virtual Display Driver** by VirtualDrivers, with a PadBridge display
   available. The controller automatically selects 2420 × 1668 at 120 Hz.
3. The PadBridge iPad IPA installed through SideStore or AltStore.
4. `PadBridge-Setup-1.0.0.exe` installed on Windows.

The Windows release bundles its controller, native host, and a compatible
FFmpeg executable. It does not require CMake, Visual Studio, Python, `iproxy`,
or a separate FFmpeg installation.

## Free Apple signing limitation

A paid Apple Developer membership is not required. GitHub Actions builds an
unsigned IPA, and SideStore/AltStore signs it locally with a free Apple ID.
Apple's free provisioning expires after seven days, so the IPA must continue to
be refreshed. A custom app cannot legally bypass that iPadOS restriction.

## Release artifacts

Run the two GitHub Actions workflows after uploading the complete repository:

- **Build complete Windows app** produces `PadBridge-Setup-1.0.0.exe` and a
  portable ZIP. The setup executable is the normal installation choice.
- **Build unsigned iPad IPA** produces `PadBridge-unsigned.ipa` for SideStore.

The Windows app is locally built and unsigned, so Windows SmartScreen may show
an unrecognized-publisher warning. Code-signing certificates are paid and are
not required for this personal build.

## Connection modes

| Mode | Behavior | Default bitrate |
|---|---|---:|
| Auto | Uses trusted USB first, then Bonjour Wi-Fi | 60/35 Mb/s |
| USB-C | Native Apple usbmux forwarding on loopback | 60 Mb/s |
| Wi-Fi | Bonjour discovery or a saved iPad address | 35 Mb/s |

Internet speed is irrelevant to Wi-Fi mode; both devices communicate directly
inside the local network.

## Resource policy

- Desktop Duplication emits no duplicate frames when the desktop is static.
- A timestamp limiter prevents cursor-only updates exceeding 120 fps.
- NVENC uses its low-latency hardware path and no B-frames.
- Decode/render queues keep only the newest frame.
- The iPad renderer follows VSync and falls to 1 Hz while idle.
- Disconnect tears down every media process instead of polling indefinitely.

Audio redirection is not enabled in this display-focused 1.0 build. Windows
audio remains on the selected Windows output device; this keeps the mandatory
120 Hz display path lean and avoids adding an always-on audio capture session.

## Repository map

- `windows-controller` — one-click WPF app, native USB forwarding, Bonjour,
  display lifecycle, settings, tray, and app placement.
- `windows-host` — capture, NVENC process control, protocol, reconnect grace,
  and Windows synthetic touch/Pencil injection.
- `ipad-client` — native SwiftUI, Network.framework, VideoToolbox, Metal, and
  UIKit touch/Pencil client.
- `protocol` — versioned wire format.
- `installer` — per-user Inno Setup package.
- `.github/workflows` — complete Windows release and unsigned IPA builds.

Developer-only PowerShell scripts remain in `windows-host/scripts` for deep
diagnostics; end users do not run them.

## Upstream components

- [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
- [FFmpeg](https://ffmpeg.org/)
- [libusbmuxd protocol reference](https://github.com/libimobiledevice/libusbmuxd)
- [SideStore](https://sidestore.io/)

PadBridge source is MIT licensed. See `THIRD_PARTY_NOTICES.md` for bundled and
interoperated components.
