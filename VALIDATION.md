# Validation

Local validation completed in the provided Linux environment on 2026-08-08:

- Complete native host compiles as strict C++20 with warnings as errors.
- Protocol header/config round trips pass.
- H.264 Annex-B fragmented input, SPS/PPS, IDR, and multi-frame tests pass.
- UndefinedBehaviorSanitizer and AddressSanitizer test runs pass where supported.
- Host argument parsing and automatic session-loop binary link successfully.
- The adaptive limiter retains 1/60/120 fps inputs and caps 144/240/1000 fps
  timestamp streams at 120.
- The approved PadBridge icon is a non-alpha 1024 × 1024 iPad app icon and a
  multi-resolution Windows ICO.
- Asset catalog, plist, XAML, JSON, YAML, and installer inputs receive static
  syntax/structure checks before packaging.

The Linux environment does not contain the Windows .NET/WPF SDK or Apple's iOS
SDK. The GitHub Actions workflows are therefore mandatory compile gates for:

- The self-contained Windows x64 WPF controller and Inno Setup installer.
- The Swift/Metal iPad target and unsigned IPA.

The final acceptance gate is one real-device run on the target OMEN/iPad pair:

1. Install the generated Windows setup and refreshed IPA.
2. Confirm one-click USB and Wi-Fi startup.
3. Confirm 2420 × 1668 at 120 Hz, correct aspect ratio, and stable VSync.
4. Confirm mouse/keyboard extension, touch, and Apple Pencil.
5. Confirm idle rate reduction and GPU/CPU utilization.
6. Unplug USB and close/background the iPad app; verify FFmpeg, host, virtual
   monitor, and Windows controller terminate after the three-second grace.

No software-only test can honestly certify cable, driver, GPU, and iPadOS beta
behavior without that final hardware pass.
