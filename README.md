# rusc~

> **Experimental.** Research prototype. Not production software. Use at own risk.

Real-time zero-shot and few-shot audio classification in Max/MSP using [laion/clap-htsat-fused](https://huggingface.co/laion/clap-htsat-fused).

`rusc~` listens to incoming audio, segments it into fixed-length windows, and classifies each window against a set of class prototypes using CLAP (Contrastive Language-Audio Pretraining). Class prototypes can be text descriptions, audio examples recorded from a `buffer~` or `polybuffer~`, or a mix of both.

---

## Requirements

- macOS, Apple Silicon (arm64)
- Max 8 or later
- Python (conda env) for model export — see below

---

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target rusc_tilde
```

The external is output to `externals/rusc~.mxo`.

Dependencies (expected in `libs/`): ONNX Runtime, r8brain, essentia, FFTW3. See `CMakeLists.txt` for paths.

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
rusc_tilde_audio_1000ms.onnx  — audio encoder, 1000ms context
rusc_tilde_text.onnx          — text encoder
rusc_tilde_meta.json          — model metadata
rusc_tilde_mel_filters.bin    — mel filterbank coefficients
vocab.json / merges.txt       — BPE tokenizer files
```

You can pass `--segment-seconds 0.5` or `--segment-seconds 1.0` to export a different context length. Shorter contexts are more reactive; longer contexts capture more temporal structure.

---

## Real-Time Usage

You can instantiate the object directly in Max such as described below:

```
[rusc~]                              — auto-detects model files via Max's search path
[rusc~ /path/to/model]               — auto-detects the .onnx file in the directory
[rusc~ /path/to/model/rusc_tilde_audio_1000ms.onnx]
[rusc~ /path/to/model mps]           — enable CoreML / Apple Neural Engine
[rusc~ mps]                          — auto-detect + CoreML
```

For auto-detection to work, place all model files in your Max package's `media/` folder or add their directory to Max's search path.

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
| `context` | 500 ms | Audio context window fed to the model. Shorter = more reactive; longer = more temporal context. |
| `confidence` | 0.0 | Minimum winning-class probability to output a result. Below this, all outlets are silent. |
| `sensitivity` | 1.0 | Smoothing on the probability distribution over time. 0 = maximum smoothing, 1 = no smoothing. |
| `sensitivityrange` | 1000 ms | Time constant range for the smoothing. Scales the effect of `sensitivity`. |
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

---

## Known limitations

- macOS arm64 only.
