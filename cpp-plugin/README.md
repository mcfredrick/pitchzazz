# Pitchzazz (C++/JUCE)

A JUCE audio plugin (AU/VST3/Standalone) for macOS — the C++ counterpart
to the Rust `pitch-cli` implementation in the parent repo. Same algorithm
(McLeod pitch detection, phase-vocoder pitch shift, scale quantization),
ported deliberately so the two implementations can be compared on real
data. See `../docs/ROADMAP.md` Phase 2/3 for why this exists.

## Build

```
./scripts/generate_and_open_xcode.sh
```

or directly:

```
./scripts/build.sh vst3 debug
./scripts/build.sh au debug
./scripts/build.sh standalone debug
```

## Links

- Built with [JUCE-Plugin-Starter](https://github.com/danielraffel/JUCE-Plugin-Starter)
- Parent project: `../README.md`, `../docs/ARCHITECTURE.md`
