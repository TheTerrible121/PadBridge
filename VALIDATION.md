# Checkpoint 1 validation

Validated in the provided Linux build environment on 2026-08-07:

- Windows-host sources compile as C++20 with GCC warnings promoted to errors.
- Protocol header/config round trips pass.
- H.264 Annex-B parsing passes fragmented-input, SPS/PPS, IDR, and multi-frame
  assertions.
- The same tests pass with UndefinedBehaviorSanitizer enabled.
- The complete host links and its argument parser executes successfully.
- FFmpeg's official documentation confirms `ddagrab` returns D3D11 hardware
  frames directly for NVENC, along with the used `output_idx`, `framerate`, and
  `draw_mouse` options.
- FFmpeg's official documentation confirms `h264_metadata=aud=insert`, which is
  the access-unit delimiter required by the streaming parser.

The iPad target cannot be compiled in this Linux environment because Apple's
iOS SDK and Xcode only run on macOS. The included GitHub Actions macOS workflow
is therefore the compile gate for the native Swift/Metal target; it emits an
unsigned IPA specifically for free local signing with AltStore.

LeakSanitizer was not usable in this managed container because `/proc` thread
inspection is blocked. UndefinedBehaviorSanitizer completed successfully.

