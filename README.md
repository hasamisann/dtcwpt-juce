# DT-CWPT

Dual-Tree Complex Wavelet Packet Transform library for JUCE 8.x

English | [日本語](README-ja.md)

---

## Overview

This library implements the Dual-Tree Complex Wavelet Packet Transform (Bayram & Selesnick, 2008) as a JUCE module. It performs subband decomposition of audio signals via dual-tree complex wavelet filter banks with configurable tree topology, and reconstructs the output with perfect reconstruction. A callback interface allows user-defined processing on the complex subbands between analysis and synthesis.

---

## Requirements

- JUCE: tested with 8.0.12 (may work with 6.x or later, but not guaranteed)
- C++: C++20 or later (tested with C++20)

---

## Features

- Configurable tree topology: full packet, wavelet tree, or arbitrary mixed-depth binary trees
- Perfect reconstruction for any valid topology
- Pluggable subband processing via the `BandProcessor` interface (per-band or cross-band)
- Re/Im to Magnitude/Phase conversion with in-place support
- Sidechain input for parallel analysis
- Sample-accurate stateful filtering with automatic inter-band delay compensation

---

## Project Structure

```
dtcwpt/                  <- JUCE module root
├── dtcwpt.h             <- Master header + module declaration
├── dtcwpt.cpp           <- Unity build file
├── dtcwpt_processor.h/.ipp
├── dtcwpt_analysis_node.h/.ipp
├── dtcwpt_synthesis_node.h/.ipp
├── dtcwpt_stateful_filter.h/.ipp
├── dtcwpt_delay_buffer.h/.ipp
├── dtcwpt_topology_planner.h/.ipp
├── dtcwpt_band_processor.h/.ipp
├── dtcwpt_complex_utils.h/.ipp
├── dtcwpt_filter_structs.h
└── dtcwpt_filter_coeffs.h
tests/                   <- Unit tests (optional)
```

---

## Basic Usage

### Minimal (Passthrough)

```cpp
#include <dtcwpt/dtcwpt.h>

// Create processor
dtcwpt::DTCWPTProcessor processor;

// Configure topology
dtcwpt::TopologyConfig config;
config.destinations = {"LLL", "LLH", "LHL", "LHH", "HLL", "HLH", "HHL", "HHH"};
config.maxDepth = 12; // Optional: defaults to the supported limit

// Initialize
processor.prepareToPlay(sampleRate, maxBlockSize, config, numChannels);

// Get latency for host compensation
int latency = processor.getLatency();

// Process audio block (analysis -> synthesis passthrough)
processor.processBlock(audioBuffer);
```

### With Band Processing

```cpp
// Register a per-band processor (e.g., attenuate high-frequency bands)
processor.setBandProcessor(dtcwpt::makeLambdaProcessor(
    [](int bandIndex, double* re, double* im, size_t numSamples) {
        if (bandIndex >= 4) {
            for (size_t i = 0; i < numSamples; ++i) {
                re[i] *= 0.5;
                im[i] *= 0.5;
            }
        }
    }
));

// processBlock() now applies the band processor between analysis and synthesis
processor.processBlock(audioBuffer);
```

### With Magnitude/Phase Processing

```cpp
processor.setBandProcessor(dtcwpt::makeLambdaProcessor(
    [](int bandIndex, double* re, double* im, size_t n) {
        // Convert to magnitude/phase (in-place: re becomes mag, im becomes phase)
        dtcwpt::complexToMagPhase(re, im, re, im, n);

        // Modify magnitude (e.g., spectral gating)
        for (size_t i = 0; i < n; ++i) {
            if (re[i] < 0.01) re[i] = 0.0;  // re[i] is magnitude here
        }

        // Convert back to Re/Im (in-place: mag becomes re, phase becomes im)
        dtcwpt::magPhaseToComplex(re, im, re, im, n);
    }
));
```

---

## Integration

### Option 1: Projucer

Drag the `dtcwpt/` folder into your Projucer project's "Modules" section. The module will be automatically configured.

### Option 2: CMake

Add the library as a JUCE module in your `CMakeLists.txt`:

```cmake
# Register the dtcwpt module
juce_add_module("path/to/dtcwpt")

# Link your target against it
target_link_libraries(MyTarget PRIVATE dtcwpt)
```

### Include Header

```cpp
#include <dtcwpt/dtcwpt.h>
```

---

## Tests

The `tests/` directory contains unit tests.

### Building Tests

Prerequisites: JUCE must be available in your environment.

Build commands:
```bash
cmake -B build -S tests
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The build system will automatically locate JUCE in the following order:
1. System-installed JUCE (via CMake's `find_package`)
2. Local `../JUCE` directory (e.g., git submodule)
3. Download automatically via CMake FetchContent (if neither above is found)

---

## Important Constraints

### Depth Limitation

- Maximum supported tree depth: 12 (maximum 4096 leaf bands at full depth)
- `TopologyConfig::maxDepth` defaults to that supported limit
- `prepareToPlay()` rejects configurations where the deepest destination path (`actualDepth`) exceeds `maxDepth`

### Topology (Destinations) Rules

`README.md` is the normative specification for valid `TopologyConfig::destinations` inputs.
`TopologyConfig::destinations` defines the leaf nodes of a binary wavelet packet tree.
Each destination is a path string of `'L'` (low-pass) and `'H'` (high-pass) characters, where the string length equals the node depth.

The destinations must form the complete leaf set of a valid binary tree where every node has either 0 or 2 children.
`prepareToPlay()` computes `actualDepth` as the deepest destination path length and requires `actualDepth <= maxDepth`.

In other words:

- If a node is split (has children), both its L-child and H-child must exist in the tree
- Leaf nodes (destinations) have no children
- Destination lists must be non-empty
- Every destination path must be non-empty and contain only `L` or `H`
- Destination paths must be unique
- No destination may be an ancestor of another destination
- No path may be longer than `maxDepth` characters at prepare time

#### Path String Encoding

| Path | Meaning | Depth | Node Index |
|------|---------|-------|------------|
| `"L"` | Root -> Low | 1 | 2 |
| `"H"` | Root -> High | 1 | 3 |
| `"LL"` | Root -> Low -> Low | 2 | 4 |
| `"LH"` | Root -> Low -> High | 2 | 5 |
| `"HLL"` | Root -> High -> Low -> Low | 3 | 12 |

#### Valid Topology Examples

Example 1: Full packet at depth 2 -- All leaves at the same depth.

```
         (root)
        /      \
       L        H           depth 1
      / \      / \
    LL   LH  HL   HH       depth 2 (all leaves)

destinations = {"LL", "LH", "HL", "HH"}    Valid
```

Every internal node (root, L, H) has exactly 2 children. All 4 leaves are listed.

Example 2: Mixed-depth tree -- Leaves at different depths.

```
         (root)
        /      \
       L        H           "L" is a leaf (depth 1)
               / \
             HL   HH        "HL","HH" are leaves (depth 2)

destinations = {"L", "HL", "HH"}    Valid
```

`L` is a leaf (no children). `H` is an internal node with both children `HL` and `HH`.

Example 3: Wavelet tree at depth 3 -- Only the L-branch is fully decomposed.

```
              (root)
             /      \
            L        H         "H" is a leaf (depth 1)
           / \
         LL   LH               "LH" is a leaf (depth 2)
        / \
      LLL  LLH                 "LLL","LLH" are leaves (depth 3)

destinations = {"LLL", "LLH", "LH", "H"}    Valid
```

#### Invalid Topology Examples

Invalid 1: Missing sibling -- Node H is split, but only HL exists (HH is missing).

```
         (root)
        /      \
       L        H
               /
             HL        <-- Where is HH?

destinations = {"L", "HL"}    INVALID -- H has only 1 child
```

Invalid 2: Overlapping paths -- `"L"` is listed as a leaf, but `"LL"` implies `L` is an internal node.

```
destinations = {"L", "LL", "LH", "H"}    INVALID -- L cannot be both a leaf and a parent
```

Invalid 3: Incomplete leaves -- Internal node L has children LL and LH, but only LL is listed.

```
destinations = {"LL", "H"}    INVALID -- LH is missing (L must have both children)
```

`prepareToPlay()` rejects empty destination lists, empty paths, invalid characters, duplicates, ancestor overlap, incomplete binary tree shapes, `maxDepth` values outside `1..12`, and any topology whose deepest destination path exceeds the configured `maxDepth`. Malformed tree shapes are rejected before topology planning and processor state construction, so invalid README-defined topologies do not proceed into undefined behavior.

### Double Precision

This library uses `juce::AudioBuffer<double>` throughout. JUCE's standard plugin API uses `float` buffers. You must convert between `float` and `double` in your plugin's `processBlock()`:

```cpp
// float -> double (before processing)
for (int ch = 0; ch < numChannels; ++ch) {
    const float* src = floatBuffer.getReadPointer(ch);
    double* dst = doubleBuffer.getWritePointer(ch);
    for (int i = 0; i < numSamples; ++i)
        dst[i] = static_cast<double>(src[i]);
}

processor.processBlock(doubleBuffer);

// double -> float (after processing)
for (int ch = 0; ch < numChannels; ++ch) {
    const double* src = doubleBuffer.getReadPointer(ch);
    float* dst = floatBuffer.getWritePointer(ch);
    for (int i = 0; i < numSamples; ++i)
        dst[i] = static_cast<float>(src[i]);
}
```

Pre-allocate the `doubleBuffer` in `prepareToPlay()` to avoid heap allocations on the audio thread.

---

## Band Processing

### Pipeline Overview

The DT-CWPT processor follows a fixed 4-stage pipeline:

```
Input -> [Analysis] -> [Delay Compensation] -> [Band Processing] -> [Synthesis] -> Output
```

| Stage               | Description                                                                        |
| ------------------- | ---------------------------------------------------------------------------------- |
| Analysis            | Decomposes input through dual-tree filter banks into sub-bands at each destination |
| Delay Compensation  | Aligns sub-band phases before cross-band processing (automatic)                    |
| Band Processing | User-defined processing on aligned sub-bands via `BandProcessor` (optional)        |
| Synthesis       | Reconstructs the output signal from processed sub-bands via inverse transform      |

Without a registered `BandProcessor`, the pipeline acts as a passthrough (perfect reconstruction).

### BandProcessor Interface

Implement the `BandProcessor` interface to process sub-bands between analysis and synthesis:

```cpp
class BandProcessor {
public:
    virtual void prepare(double sampleRate, int maxSamplesPerBand,
                         int numBands, int numChannels) = 0;
    virtual void processAllBands(BandData& data);   // Override for cross-band algorithms
    virtual void processBand(int bandIndex, double* re, double* im,
                             size_t numSamples);     // Override for per-band processing
    virtual void reset() = 0;
};
```

- `prepare()` is called when `DTCWPTProcessor::prepareToPlay()` runs, or when `setBandProcessor()` is called after prepare.
- `processAllBands()` default implementation iterates over all channels and bands, calling `processBand()` for each. Override this for cross-band or cross-channel algorithms (e.g., spectral morphing).
- `processBand()` default implementation is a no-op (passthrough). Override for simple per-band processing.
- `reset()` is called when playback restarts.

All methods are called on the audio thread and must be real-time safe (no allocations, no locks, no syscalls).

Register a band processor with:

```cpp
processor.setBandProcessor(std::move(myBandProcessor));
```

### Using makeLambdaProcessor

For simple per-band processing, use the convenience factory instead of subclassing:

```cpp
// BandProcessorFunc = std::function<void(int bandIndex, double* re, double* im, size_t numSamples)>
auto bp = dtcwpt::makeLambdaProcessor(
    [](int bandIndex, double* re, double* im, size_t numSamples) {
        // Your per-band processing here
    }
);
processor.setBandProcessor(std::move(bp));
```

### Complex Utilities (Re/Im <-> Mag/Phase)

The library provides utility functions for converting between rectangular (Re/Im) and polar (Magnitude/Phase) representations. These are useful for magnitude-based spectral processing within band processors.

```cpp
// Re/Im -> Magnitude/Phase
//   mag[i] = sqrt(re[i]^2 + im[i]^2)
//   phase[i] = atan2(im[i], re[i])
dtcwpt::complexToMagPhase(re, im, mag, phase, n);

// Magnitude/Phase -> Re/Im
//   re[i] = mag[i] * cos(phase[i])
//   im[i] = mag[i] * sin(phase[i])
dtcwpt::magPhaseToComplex(mag, phase, re, im, n);
```

In-place operation is supported. You can pass the same pointers for input and output (e.g., `re == mag` and `im == phase`), which avoids the need for temporary buffers on the audio thread:

```cpp
// In-place: re becomes magnitude, im becomes phase
dtcwpt::complexToMagPhase(re, im, re, im, n);
// ... modify magnitude/phase ...
// In-place: magnitude becomes re, phase becomes im
dtcwpt::magPhaseToComplex(re, im, re, im, n);
```

### Sidechain Support

Feed a sidechain signal for parallel CWPT analysis (e.g., for spectral morphing between two signals):

```cpp
// Must be called BEFORE processBlock() for the same block
processor.processSidechain(sidechainBuffer);
processor.processBlock(mainBuffer);
```

In your `BandProcessor::processAllBands()`, access sidechain data via `BandData`:

```cpp
void processAllBands(BandData& data) override {
    if (data.hasSidechain) {
        for (int ch = 0; ch < data.numChannels; ++ch) {
            for (int b = 0; b < data.numBands; ++b) {
                auto& main = data.bands[ch][b];
                auto& sc   = data.sidechainBands[ch][b];
                // Use sc.re, sc.im alongside main.re, main.im
            }
        }
    }
}
```

The sidechain channel count is automatically adjusted to match the main input (duplicating the last channel or truncating as needed).

---

## API Reference

### DTCWPTProcessor

```cpp
void prepareToPlay(double sampleRate, int maxBlockSize,
                   const TopologyConfig& config, int channelNum);
```

Initializes the processor with given parameters. Must be called before processing.

---

```cpp
void processBlock(juce::AudioBuffer<double>& buffer);
```

Processes an audio block. Accepts any buffer size; an internal FIFO handles block-size adaptation automatically.

---

```cpp
int getLatency() const;
```

Returns the processing latency in samples. Use this for host latency compensation.

---

```cpp
void setBandProcessor(std::unique_ptr<BandProcessor> processor);
```

Registers a band processor (takes ownership). If `prepareToPlay()` was already called, immediately calls `prepare()` on the processor. Must be called when audio is NOT running.

---

```cpp
void processSidechain(juce::AudioBuffer<double>& sidechainBuffer);
```

Feeds sidechain audio for parallel CWPT analysis. Must be called with the same sample count as the main `processBlock()` call, and must be called before `processBlock()` for the same block.

---

### TopologyConfig

```cpp
struct TopologyConfig {
    std::vector<std::string> destinations;  // Path strings (e.g., "LLL", "LH")
    int maxDepth = 12;                      // Supported maximum tree depth by default (1-12)
};
```

Defines the wavelet packet tree structure. `maxDepth` defaults to the supported limit, and `prepareToPlay()` requires `actualDepth <= maxDepth`. See [Topology (Destinations) Rules](#topology-destinations-rules) for validity constraints.

---

### BandProcessor

```cpp
class BandProcessor {
public:
    virtual void prepare(double sampleRate, int maxSamplesPerBand,
                         int numBands, int numChannels) = 0;
    virtual void processAllBands(BandData& data);
    virtual void processBand(int bandIndex, double* re, double* im,
                             size_t numSamples);
    virtual void reset() = 0;
};
```

Abstract interface for user-defined subband processing. See [Band Processing](#band-processing) for usage details.

---

### BandData

```cpp
struct BandData {
    std::vector<std::vector<ChannelBandView>> bands;          // bands[ch][bandIndex]
    std::vector<std::vector<ChannelBandView>> sidechainBands; // sidechain (empty if inactive)
    int numChannels;
    int numBands;
    bool hasSidechain;
    const std::vector<int>* destinationIds;  // Node IDs in band order
};
```

All-bands, all-channels data container passed to `processAllBands()`. Band indices correspond to `TopologyConfig::destinations` order. Uses zero-copy pointers into the processor's internal buffers.

---

### ChannelBandView

```cpp
struct ChannelBandView {
    double* re;           // Pointer to real part of subband data (mutable)
    double* im;           // Pointer to imaginary part of subband data (mutable)
    size_t numSamples;    // Number of subband samples in this band
};
```

Per-band data view for a single channel. Provides mutable access to the subband's Re/Im data.

---

### makeLambdaProcessor

```cpp
using BandProcessorFunc = std::function<void(int bandIndex, double* re,
                                             double* im, size_t numSamples)>;

std::unique_ptr<BandProcessor> makeLambdaProcessor(BandProcessorFunc func);
```

Convenience factory that creates a `BandProcessor` from a lambda or function object. The returned processor delegates `processBand()` to the provided function; `prepare()` and `reset()` are no-ops.

---

### Complex Utilities

```cpp
void complexToMagPhase(const double* re, const double* im,
                       double* mag, double* phase, size_t n);

void magPhaseToComplex(const double* mag, const double* phase,
                       double* re, double* im, size_t n);
```

Convert between rectangular (Re/Im) and polar (Magnitude/Phase) representations. In-place operation is supported (`re == mag` and `im == phase` is valid).

---

## Technical Details

### Background

The standard Discrete Wavelet Transform (DWT) decomposes a signal through iterated 2-channel filter banks (low-pass/high-pass analysis, downsampling by 2, and corresponding synthesis). The DWT applies this only to the low-pass branch, yielding logarithmic frequency tiling. However, the decimated DWT is shift-variant: small time shifts in the input produce large changes in coefficients.

The Dual-Tree Complex Wavelet Transform (DT-CWT) [1] mitigates this by running two parallel filter banks whose wavelets form an approximate Hilbert pair. The resulting complex coefficients have near shift-invariant magnitude and cleanly separated phase. The wavelet packet extension (DT-CWPT) [2] generalises the tree structure so that both the low-pass and high-pass branches can be further decomposed, allowing arbitrary frequency tilings beyond the fixed logarithmic layout of the DT-CWT.

### Filter coefficients

This implementation uses two sets of filters:

| Level | Filters | Taps (lo / hi) |
|-------|---------|-----------------|
| 1 | CDF 9/7 biorthogonal | 10 / 8 |
| >= 2 | Kingsbury Q-shift | 14 / 14 |

The half-sample delay between the two trees' filter sets produces the approximate 90-degree phase shift required for the Hilbert pair relationship.

### Perfect reconstruction

Perfect reconstruction holds for any valid binary tree topology (every internal node has exactly two children). The PR filter bank conditions are:

$$H_0(z)G_0(z) + H_1(z)G_1(z) = 2z^{-l}$$

$$H_0(-z)G_0(z) + H_1(-z)G_1(z) = 0$$

### References

1. Selesnick, I. W., Baraniuk, R. G., & Kingsbury, N. G. (2005). The dual-tree complex wavelet transform. *IEEE Signal Processing Magazine*, 22(6), 123-151. https://doi.org/10.1109/MSP.2005.1550194

2. Bayram, I., & Selesnick, I. W. (2008). On the dual-tree complex wavelet packet and M-band transforms. *IEEE Transactions on Signal Processing*, 56(6), 2298-2310. https://doi.org/10.1109/TSP.2007.916129

---

## License

MIT License - see [LICENSE](LICENSE) file for details.
