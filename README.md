# Real-time Universal Sound Classifier: rusc~ 

> **Experimental.** Research prototype. Not production software. Use at own risk.
Part of this code was implemented by Claude Opus 4.8 for faster implementation. The code have been revewied by a human before making it available online.

Real-time zero-shot and few-shot audio classification in Max/MSP using [laion/clap-htsat-fused](https://huggingface.co/laion/clap-htsat-fused).

`rusc~` listens to incoming audio, segments it into fixed-length windows, and classifies each window against a set of class prototypes using CLAP (Contrastive Language-Audio Pretraining). Class prototypes can be text descriptions, audio examples recorded from a `buffer~` or `polybuffer~`, or a mix of both.

---

## Requirements

- macOS 14+ on Apple Silicon (arm64), or Windows 10+ (x64)
- Max 8 or later
- CMake 3.19+, a C++17 compiler (Xcode / Visual Studio 2022 or later)
- Python (conda env) for model export — see below

---

## Build

```bash
git clone --recursive https://github.com/nbrochec/rusc_tilde.git
cd rusc_tilde
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release          # macOS
cmake -S . -B build -A x64                              # Windows (latest Visual Studio)
cmake --build build --config Release --target rusc_tilde
```

The external is written to `externals/rusc~.mxo` (macOS) or `externals/rusc~.mxe64` (Windows).

All dependencies are fetched automatically at configure time: ONNX Runtime (prebuilt release), r8brain (resampler) and pocketfft (FFT). A local copy in `libs/onnxruntime` or `libs/r8brain` is used instead when present.

On Windows, `onnxruntime.dll` is copied next to the external. When installing into a Max package, put the DLL in the package's `support/` folder so Max can find it.

Unit tests for the DSP front-end (no model required):

```bash
cmake --build build --config Release --target rusc_tests
ctest --test-dir build -C Release --output-on-failure
```

Continuous integration builds both platforms on every push (`.github/workflows/build.yml`).

---

## Export the model

Create a conda environment with the required packages, then run the export script from the project root:

```bash
conda create -n clap python=3.12
conda activate clap
pip install torch==2.4.1 torchaudio==2.4.1 transformers==4.44.2 onnx onnxruntime

conda run -n clap python scripts/export_clap_onnx.py
```

This downloads `laion/clap-htsat-fused` from HuggingFace and writes the following files to `./model/`:

```
clap_audio_1000ms.onnx  — audio encoder, 1000ms context
clap_text.onnx          — text encoder
clap_meta.json          — model metadata
clap_mel_filters.bin    — mel filterbank coefficients
vocab.json / merges.txt       — BPE tokenizer files
```

You can pass `--segment-seconds 0.5` or `--segment-seconds 1.0` to export a different context length. Shorter contexts are more reactive; longer contexts capture more temporal structure.

---

## Real-Time Usage

You can instantiate the object directly in Max such as described below:

```
[rusc~]                              — auto-detects model files via Max's search path
[rusc~ clap_audio_1000ms.onnx]       — file name only, found via Max's search path
[rusc~ /path/to/model]               — auto-detects the .onnx file in the directory
[rusc~ /path/to/model/clap_audio_1000ms.onnx]
[rusc~ clap_audio_1000ms.onnx ane]   — CoreML execution provider (experimental, see Performance)
[rusc~ ane]                          — auto-detect + CoreML
```

No full path is needed, but keep the `.onnx` extension since it identifies the model format: if the model files are inside a Max package (for example the package's `media/` folder) or in a folder added to Max's search path, Max finds them by name. The other model files (`clap_text.onnx`, `clap_meta.json`, `clap_mel_filters.bin`, `vocab.json`, `merges.txt`) must sit next to the `.onnx` file.

### Inlets

| Inlet | Description |
|---|---|
| 1 (left) | Audio signal input. Mono, any sample rate (resampled internally to 48 kHz). |
| 2 (right) | Few-shot registration messages (`record`, `record_multi`, `clear_example`, `clear_examples`). |

### Outlets

| Outlet | Description |
|---|---|
| 1 (left) | Index of the winning class (int, 0-based). |
| 2 | Name of the winning class (symbol). |
| 3 | Full probability distribution over all classes (list of floats). |
| 4 (dumpout) | `latency <ms>` after each inference; `classnames <name1> ...` when requested. |

---

## Attributes

| Attribute | Default | Description |
|---|---|---|
| `enabled` | 1 | Turn inference on/off without stopping DSP. |
| `threshold` | −80 dB | Energy gate — audio below this level is ignored. |
| `window` | 20 ms | Look-back window for the energy gate. |
| `context` | 1000 ms | Audio context window fed to the model (minimum 100 ms, capped at the exported model's context). Shorter = more reactive; longer = more temporal context. |
| `confidence` | 0.0 | Minimum winning-class probability to output a result. Below this, all outlets are silent. |
| `sensitivity` | 1.0 | Smoothing on the probability distribution over time. 0 = maximum smoothing, 1 = no smoothing. |
| `sensitivityrange` | 2000 ms | Time constant range for the smoothing. Scales the effect of `sensitivity`. |
| `threads` | 4 | CPU threads used inside the ONNX encoder. Set it in the object box: it is applied when the model loads. See [Performance](#performance). |
| `verbose` | 0 | Print extra information to the Max console. |

---

## Messages

### Inlet 1

**`set_classes <name1> <name2> ...`**
Set the text class prototypes. Each atom is one class name. Use underscores for multi-word names (`kick_drum`, `hi_hat`). Replaces the current class set entirely.

**`add_class <name1> <name2> ...`**
Append class names to the current set without clearing existing ones. Duplicate names are ignored.

**`classnames`**
Output the current active class names to the dumpout as `classnames <name1> <name2> ...`.

### Inlet 2

**`record <label> <buffer_name>`**
Register a single audio example for a class. Reads the named `buffer~`, resamples to 48 kHz, and stores it as the prototype for that label. The label does not need to exist in the text class set — `record` creates it if needed. Multiple `record` calls with the same label overwrite the previous example.

**`record_multi <label> <polybuffer_name>`**
Register multiple audio examples for a class from a `polybuffer~`. Reads all slots (`<name>.1`, `<name>.2`, …), encodes each one, and averages their embeddings into a single prototype for that label. Use this instead of `record` when you have several recordings of the same sound — the averaged prototype is more robust than any single example. As with `record`, the label does not need to exist in the text class set.

**`clear_example <label>`**
Remove the audio example for a single label. The class reverts to its text embedding if one exists, or disappears from the class set entirely if it was audio-only.

**`clear_examples`**
Remove all registered audio examples.

---

## Performance

Latency of one inference (audio encoder, 1 s context, `clap_audio_1000ms.onnx`), measured on an Apple M4 Pro with ONNX Runtime 1.28. Best and mean of 20 runs after warm-up.

| Backend | Best | Mean | Notes |
|---|---|---|---|
| CPU, `@threads 1` | 53 ms | 55 ms | |
| CPU, `@threads 2` | 40 ms | 41 ms | |
| CPU, `@threads 4` (default) | 32 ms | 32 ms | most of the gain, moderate CPU load |
| CPU, `@threads 8` | 28 ms | 28 ms | diminishing returns |
| CoreML `ane` (all units) | 29 ms | 37 ms | 33 s model load, 2.4 s first run |
| CoreML CPU+GPU | 41 ms | 42 ms | 25 s model load |

The mel front-end adds about 1.3 ms and the class scoring is negligible, so total latency is the encoder time plus the `context` window.

**About `ane`.** With the current ONNX export the CoreML provider can only take 863 of the 946 graph nodes and splits the encoder into 33 partitions, because HTSAT's patch embedding and `is_longer` logic produce dynamically-shaped tensors (`NonZero`, unbounded reshapes) that CoreML refuses. The remaining nodes run on the ORT CPU provider and the data crosses back and forth, which cancels the Neural Engine's advantage. Results are numerically identical to the CPU path (max difference 1e-7). Until the export is rewritten with static shapes, `ane` is not recommended: use the CPU with `@threads 4` or more.

The CPU thread count is set once when the ONNX session is created, so put `@threads` in the object box.

---

## Workflows

**Zero-shot (text only)**
```
1. Send to inlet 1: set_classes kick snare hihat
2. Connect audio to inlet 1.
```

**Few-shot with single examples per class**
```
1. Load a recording into buffer~ "kick_buf", then send to inlet 2: record kick kick_buf
2. Do the same for other labels.
```
Labels are created automatically from the recordings.

**Few-shot with multiple examples per class**
```
1. Record several takes into a polybuffer~ named "kicks" (slots: kicks.1, kicks.2, …)
2. Send to inlet 2: record_multi kick kicks
```
The averaged prototype is more representative than a single take.

**Mixed (text + audio overrides)**
```
1. Send to inlet 1: set_classes kick snare hihat
2. Override specific labels with recordings:
   record_multi kick kicks_poly
   record snare my_snare_buf
```
Hihat stays as text prototype.
