# Rr Sample Pads VST3

JUCE 8 / CMake Windows VST3 16-pad sample player.

- 16 sample pads, double-click to load audio.
- Drag and drop audio files onto pads.
- MIDI note triggering (default notes 36-51).
- Per-pad gain and choke groups in the processor.
- Master volume and Stop All.
- State serialization stores pad metadata and loaded audio.
- GitHub Actions builds a Windows x64 VST3 artifact.

## Build
```powershell
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target RrSamplePads_VST3 --parallel 2
```

This is an initial functional implementation; the CI build and DAW runtime tests are required before calling it production-ready.
