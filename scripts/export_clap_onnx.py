#!/usr/bin/env python3
"""
Export laion/clap-htsat-fused to ONNX for use with rusc_tilde.

Usage (from the rusc_tilde project root):
    conda run -n clap python scripts/export_clap_onnx.py
    conda run -n clap python scripts/export_clap_onnx.py --default-context-ms 500

Outputs in --output-dir (default: ./model):
    rusc_tilde_audio_<N>ms.onnx  — audio encoder (fixed frame count baked from context)
    rusc_tilde_text.onnx          — text encoder
    rusc_tilde_meta.json          — sr, default_context_ms, seglen, nb_max_frames, ...
    rusc_tilde_mel_filters.bin    — float32 [513 × 64] mel filterbank
    vocab.json                    — RoBERTa BPE vocabulary
    merges.txt                    — BPE merge rules

The ONNX audio model input is [1, 4, nb_max_frames, 64] where:
    nb_max_frames = seglen // hop_length + 1
    seglen        = round(default_context_ms * sr / 1000)

At runtime in Max, @context <ms> captures shorter audio and zero-pads the
mel spectrogram to nb_max_frames before feeding the ONNX model.

Requirements (conda env "clap"):
    torch==2.4.1  torchaudio==2.4.1  transformers==4.44.2  onnx  onnxruntime
"""

import argparse
import json
import shutil
from pathlib import Path

import torch
import torch.nn as nn


# ─────────────────────────────────────────────────────────────────────────────
# Wrappers
# ─────────────────────────────────────────────────────────────────────────────

class AudioEncoderWrapper(nn.Module):
    """Wraps the raw audio encoder + projection into a single normalised embedding."""
    def __init__(self, audio_model, audio_projection):
        super().__init__()
        self.audio_model      = audio_model
        self.audio_projection = audio_projection

    def forward(self, input_features: torch.Tensor, is_longer: torch.Tensor) -> torch.Tensor:
        out = self.audio_model(input_features=input_features, is_longer=is_longer)
        emb = self.audio_projection(out.pooler_output)
        return emb / emb.norm(p=2, dim=-1, keepdim=True)


class AudioONNXWrapper(nn.Module):
    """
    Audio encoder for ONNX export — neural network only, mel preprocessing done in C++.

    Input:  input_features [1, 4, nb_max_frames, 64]  float32
    Output: audio_embedding [1, 512]  float32 (L2-normalised)
    """
    def __init__(self, audio_encoder):
        super().__init__()
        self.audio_encoder = audio_encoder
        self.register_buffer("is_longer", torch.ones(1, 1, dtype=torch.bool))

    def forward(self, input_features: torch.Tensor) -> torch.Tensor:
        return self.audio_encoder(input_features, self.is_longer)


class TextONNXWrapper(nn.Module):
    """
    Text encoder for ONNX export.
    Inputs:  input_ids [N, L] int64,  attention_mask [N, L] int64
    Output:  text_embedding [N, 512]  float32 (L2-normalised)
    """
    def __init__(self, text_model, text_projection):
        super().__init__()
        self.text_model      = text_model
        self.text_projection = text_projection

    def forward(self, input_ids: torch.Tensor, attention_mask: torch.Tensor) -> torch.Tensor:
        out = self.text_model(input_ids=input_ids, attention_mask=attention_mask)
        emb = self.text_projection(out.pooler_output)
        return emb / emb.norm(p=2, dim=-1, keepdim=True)


# ─────────────────────────────────────────────────────────────────────────────
# Export helpers
# ─────────────────────────────────────────────────────────────────────────────

def export_audio_encoder(wrapper: AudioONNXWrapper, device: torch.device,
                         out_path: Path, nb_max_frames: int) -> None:
    """Export audio encoder with fixed nb_max_frames baked into the ONNX graph."""
    wrapper.eval()
    dummy = torch.zeros(1, 4, nb_max_frames, 64, device=device)
    with torch.no_grad():
        torch.onnx.export(
            wrapper,
            dummy,
            str(out_path),
            input_names=["input_features"],
            output_names=["audio_embedding"],
            opset_version=14,
            do_constant_folding=True,
        )


def export_text_encoder(wrapper: TextONNXWrapper, max_text_length: int,
                        device: torch.device, out_path: Path) -> None:
    wrapper.eval()
    dummy_ids  = torch.zeros(1, max_text_length, dtype=torch.long, device=device)
    dummy_mask = torch.ones(1, max_text_length, dtype=torch.long, device=device)
    with torch.no_grad():
        torch.onnx.export(
            wrapper,
            (dummy_ids, dummy_mask),
            str(out_path),
            input_names=["input_ids", "attention_mask"],
            output_names=["text_embedding"],
            dynamic_axes={
                "input_ids":      {0: "batch_size"},
                "attention_mask": {0: "batch_size"},
                "text_embedding": {0: "batch_size"},
            },
            opset_version=17,
            do_constant_folding=True,
        )


# ─────────────────────────────────────────────────────────────────────────────
# Verification
# ─────────────────────────────────────────────────────────────────────────────

def verify_exports(audio_onnx: Path, text_onnx: Path,
                   max_text_length: int, nb_max_frames: int) -> None:
    import numpy as np
    import onnxruntime as ort

    print("  Loading ONNX sessions for verification...")
    audio_sess = ort.InferenceSession(str(audio_onnx))
    text_sess  = ort.InferenceSession(str(text_onnx))

    np.random.seed(42)
    dummy_mel = np.random.randn(1, 4, nb_max_frames, 64).astype(np.float32)
    emb = audio_sess.run(["audio_embedding"], {"input_features": dummy_mel})[0]
    assert emb.shape == (1, 512), f"Unexpected audio embedding shape: {emb.shape}"
    print(f"  Audio embedding shape at nb_frames={nb_max_frames}: {emb.shape}  ✓")

    dummy_ids  = np.zeros((2, max_text_length), dtype=np.int64)
    dummy_mask = np.ones((2, max_text_length),  dtype=np.int64)
    text_emb = text_sess.run(
        ["text_embedding"],
        {"input_ids": dummy_ids, "attention_mask": dummy_mask}
    )[0]
    assert text_emb.shape == (2, 512), f"Unexpected text embedding shape: {text_emb.shape}"
    print(f"  Text embedding shape: {text_emb.shape}  ✓")

    print("  All checks passed.")


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Export laion/clap-htsat-fused to ONNX")
    parser.add_argument("--output-dir",          default="model",
                        help="Output directory (default: ./model)")
    parser.add_argument("--device",              default="cpu",
                        help="Torch device for export (cpu|mps)")
    parser.add_argument("--max-text-length",     type=int, default=77)
    parser.add_argument("--default-context-ms",  type=int, default=1000,
                        help="Max audio context window in ms (default: 1000). "
                             "Determines the ONNX model input size. "
                             "Shorter contexts are zero-padded at runtime.")
    parser.add_argument("--skip-verify",         action="store_true",
                        help="Skip onnxruntime verification step")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    device = torch.device(args.device)

    # ── 1. Download model ────────────────────────────────────────────────────
    print("Downloading laion/clap-htsat-fused from HuggingFace...")
    from transformers import ClapModel, ClapProcessor
    processor = ClapProcessor.from_pretrained("laion/clap-htsat-fused")
    model     = ClapModel.from_pretrained("laion/clap-htsat-fused")
    model.eval().to(device)
    print("  Done.")

    # ── 2. Save tokenizer files ──────────────────────────────────────────────
    print("Saving tokenizer files...")
    tmp_tok_dir = output_dir / "_tokenizer_tmp"
    processor.tokenizer.save_pretrained(str(tmp_tok_dir))
    shutil.copy(tmp_tok_dir / "vocab.json",  output_dir / "vocab.json")
    shutil.copy(tmp_tok_dir / "merges.txt",  output_dir / "merges.txt")
    shutil.rmtree(tmp_tok_dir)
    print(f"  vocab.json + merges.txt → {output_dir}")

    # ── 3. Build wrappers ────────────────────────────────────────────────────
    fe = processor.feature_extractor

    sr         = 48000
    hop_length = fe.hop_length           # 480
    seglen     = round(args.default_context_ms * sr / 1000)   # max audio captured in C++ (e.g. 48000)
    # HTSAT is trained on 10-second spectrograms → always export with 1001 frames.
    # Shorter contexts are zero-padded in C++ before inference, not by shrinking the model.
    nb_max_frames = 10 * sr // hop_length + 1   # 480000 // 480 + 1 = 1001

    print(f"Context: {args.default_context_ms} ms  →  seglen={seglen}, nb_max_frames={nb_max_frames} (HTSAT fixed)")

    raw_audio_encoder = AudioEncoderWrapper(
        model.audio_model, model.audio_projection).to(device)
    raw_audio_encoder.eval()

    audio_wrapper = AudioONNXWrapper(audio_encoder=raw_audio_encoder).to(device)
    text_wrapper  = TextONNXWrapper(
        text_model      = model.text_model,
        text_projection = model.text_projection,
    ).to(device)

    # ── 4. Export ────────────────────────────────────────────────────────────
    audio_onnx_path = output_dir / f"rusc_tilde_audio_{args.default_context_ms}ms.onnx"
    text_onnx_path  = output_dir / "rusc_tilde_text.onnx"

    print(f"Exporting audio encoder → {audio_onnx_path}")
    export_audio_encoder(audio_wrapper, device, audio_onnx_path, nb_max_frames)

    print(f"Exporting text encoder  → {text_onnx_path}")
    export_text_encoder(text_wrapper, args.max_text_length, device, text_onnx_path)

    # ── 5. Write metadata sidecar ────────────────────────────────────────────
    meta = {
        "sr":                 sr,
        "default_context_ms": args.default_context_ms,
        "seglen":             seglen,
        "nb_max_frames":      nb_max_frames,
        "n_fft":              fe.n_fft,
        "hop_length":         hop_length,
        "max_text_length":    args.max_text_length,
        "logit_scale_a":      float(model.logit_scale_a.item()),
    }
    meta_path = output_dir / "rusc_tilde_meta.json"
    meta_path.write_text(json.dumps(meta, indent=2))
    print(f"Metadata sidecar        → {meta_path}")

    # ── 6. Save mel filterbank ───────────────────────────────────────────────
    import numpy as np
    mel_path = output_dir / "rusc_tilde_mel_filters.bin"
    mel_np   = fe.mel_filters.copy().astype(np.float32)   # [513, 64] row-major
    mel_np.tofile(str(mel_path))
    print(f"Mel filters             → {mel_path}  ({mel_np.shape}, float32)")

    # ── 7. Verify ────────────────────────────────────────────────────────────
    if not args.skip_verify:
        print("Verifying exports...")
        verify_exports(audio_onnx_path, text_onnx_path, args.max_text_length, nb_max_frames)
    else:
        print("Verification skipped.")

    # ── 8. Summary ───────────────────────────────────────────────────────────
    print(f"\nExport complete:")
    print(f"  {audio_onnx_path}  (nb_max_frames={nb_max_frames})")
    print(f"  {text_onnx_path}")
    print(f"  {meta_path}")
    print(f"  {mel_path}")
    print(f"  {output_dir / 'vocab.json'}")
    print(f"  {output_dir / 'merges.txt'}")
    print()
    print("Load in Max:")
    print(f"  rusc~                     (auto-detect via Max search path)")
    print(f"  rusc~ {output_dir}        (explicit directory)")
    print(f"  rusc~ {output_dir} mps    (CoreML / Apple Silicon)")
    print(f"Use @context <ms> to set shorter context at runtime (zero-padded to {nb_max_frames} frames).")


if __name__ == "__main__":
    main()
