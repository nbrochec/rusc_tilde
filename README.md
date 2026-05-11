# clap~

> **Experimental / vibe-coded.** Research prototype developed at IRCAM (RepMus / REACH team). Not production software — expect rough edges and breaking changes.

Real-time zero-shot audio classification for Max/MSP using [laion/clap-htsat-fused](https://huggingface.co/laion/clap-htsat-fused).

`clap~` listens to incoming audio, segments it into fixed-length windows, and classifies each window against a set of class prototypes using CLAP (Contrastive Language-Audio Pretraining). Class prototypes can be text descriptions, audio examples recorded from a `buffer~`, or a mix of both.

**Contributors:** Nicolas Brochec, Claude Sonnet (Anthropic)

---

## Requirements

- macOS, Apple Silicon (arm64)
- Max 8 or later
- Python (conda env) for model export — see below

---

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target clap_tilde
```

The external is output to `externals/clap~.mxo`.

Dependencies (expected in `libs/`): LibTorch, ONNX Runtime. See `CMakeLists.txt` for paths.

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
clap_tilde_audio_333ms.onnx   — audio encoder, 333 ms context
clap_tilde_text.onnx          — text encoder
clap_tilde_meta.json          — model metadata
clap_tilde_mel_filters.bin    — mel filterbank coefficients
vocab.json / merges.txt       — BPE tokenizer files
```

You can pass `--segment-seconds 0.5` or `--segment-seconds 1.0` to export a different context length. Shorter contexts are more reactive; longer contexts capture more temporal structure.

---

## Usage

```
[clap~ /path/to/model/clap_tilde_audio_333ms.onnx]
[clap~ /path/to/model]            — auto-detects the .onnx file in the directory
[clap~ /path/to/model mps]        — enable CoreML / Apple Neural Engine
```

### Inlets

| Inlet | Description |
|---|---|
| 1 (left) | Audio signal input. Mono, any sample rate (resampled internally to 48 kHz). |
| 2 (right) | Few-shot registration messages (`record`). |

### Outlets

| Outlet | Description |
|---|---|
| 1 (left) | Index of the winning class (int, 0-based). |
| 2 | Name of the winning class (symbol). |
| 3 | Full probability distribution over all classes (list of floats). |
| 4 (dumpout) | `latency <ms>` after each inference. |

---

## Attributes

| Attribute | Default | Description |
|---|---|---|
| `enabled` | 1 | Turn inference on/off without stopping DSP. |
| `threshold` | −120 dB | Energy gate — audio below this level is ignored. |
| `window` | 20 ms | Look-back window for the energy gate. |
| `confidence` | 0.0 | Minimum winning-class probability to output a result. Below this, all outlets are silent. |
| `sensitivity` | 1.0 | Smoothing on the probability distribution over time. 0 = maximum smoothing, 1 = no smoothing. |
| `sensitivityrange` | 2000 ms | Time constant range for the smoothing. Scales the effect of `sensitivity`. |
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
Register an audio example for a class. Reads the named `buffer~`, resamples to 48 kHz, and encodes it as an audio embedding. On the next inference cycle, this embedding replaces the text embedding for that label (or adds a new class if the label was not in the text set). Multiple `record` calls with the same label overwrite the previous example.

**`clear_example <label>`**
Remove the audio example for a single label. The class reverts to its text embedding.

**`clear_examples`**
Remove all registered audio examples.

---

## Workflows

**Zero-shot (text only)**
```
1. Load: [clap~ /path/to/model]
2. Send: set_classes dog_bark cat_meow rain thunder
3. Connect audio to inlet 1.
```

**Few-shot (audio examples override text)**
```
1. Load: [clap~ /path/to/model]
2. Send to inlet 1: set_classes kick snare hihat
3. Load audio into buffer~, then send to inlet 2: record kick my_kick_buffer
4. Repeat for other labels.
```

**Audio-only (no text)**
```
1. Load: [clap~ /path/to/model]
2. Skip set_classes entirely.
3. Send to inlet 2: record label1 buf1
4. Send to inlet 2: record label2 buf2
```

---

## Known limitations

- macOS arm64 only.
- Single audio channel — only channel 0 is used.
- One audio example per label — last `record` call wins, no averaging.
