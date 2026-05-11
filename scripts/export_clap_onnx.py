#!/usr/bin/env python3
"""
Export laion/clap-htsat-fused to ONNX for use with clap_tilde.

Usage (from the clap_tilde project root):
    conda run -n clap python scripts/export_clap_onnx.py
    conda run -n clap python scripts/export_clap_onnx.py --segment-seconds 0.333

Outputs in --output-dir (default: ./model):
    clap_tilde_audio_<N>ms.onnx  — audio encoder (mel preprocessing baked in)
    clap_tilde_text.onnx          — text encoder
    clap_tilde_meta.json          — sr, seglen, max_text_length, logit_scale_a
    vocab.json                    — RoBERTa BPE vocabulary
    merges.txt                    — BPE merge rules

Requirements (conda env "clap"):
    torch==2.4.1  torchaudio==2.4.1  transformers==4.44.2  onnx  onnxruntime
"""

import argparse
import json
import shutil
from pathlib import Path

import torch
import torch.nn as nn
import torch.nn.functional as F


# ─────────────────────────────────────────────────────────────────────────────
# Wrappers
# ─────────────────────────────────────────────────────────────────────────────

class AudioONNXWrapper(nn.Module):
    """
    Audio encoder for ONNX export — neural network only, no mel preprocessing.
    Mel preprocessing (STFT → filterbank → log10 dB → tile 4×) is done in C++
    using LibTorch before calling this model.

    Input:  input_features [1, 4, nb_max_frames, 64]  float32  (pre-computed mel)
    Output: audio_embedding [1, 512]  float32 (L2-normalized)
    """
    def __init__(self, audio_encoder):
        super().__init__()
        self.audio_encoder = audio_encoder
        self.register_buffer("is_longer", torch.ones(1, 1, dtype=torch.bool))

    def forward(self, input_features: torch.Tensor) -> torch.Tensor:
        emb = self.audio_encoder(input_features, self.is_longer)  # [1, 512]
        return emb


class TextONNXWrapper(nn.Module):
    """
    Text encoder for ONNX export.
    Inputs:  input_ids [N, L] int64,  attention_mask [N, L] int64
    Output:  text_embedding [N, 512]  float32 (L2-normalized)
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
                         out_path: Path, dummy_nb_frames: int = 1001) -> None:
    """Export audio encoder with a dynamic time axis (nb_frames)."""
    wrapper.eval()
    dummy = torch.zeros(1, 4, dummy_nb_frames, 64, device=device)
    with torch.no_grad():
        torch.onnx.export(
            wrapper,
            dummy,
            str(out_path),
            input_names=["input_features"],
            output_names=["audio_embedding"],
            dynamic_axes={
                "input_features": {2: "nb_frames"},
            },
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

def verify_exports(audio_onnx: Path, text_onnx: Path, max_text_length: int) -> None:
    """Verify ONNX output shapes and that dynamic time axis works at two different lengths."""
    import numpy as np
    import onnxruntime as ort

    print("  Loading ONNX sessions for verification...")
    audio_sess = ort.InferenceSession(str(audio_onnx))
    text_sess  = ort.InferenceSession(str(text_onnx))

    np.random.seed(42)
    # Check two different nb_frames values to confirm dynamic axis works
    for nb_frames in [101, 501]:
        dummy_mel = np.random.randn(1, 4, nb_frames, 64).astype(np.float32)
        emb = audio_sess.run(["audio_embedding"], {"input_features": dummy_mel})[0]
        assert emb.shape == (1, 512), f"Unexpected audio embedding shape: {emb.shape}"
        print(f"  Audio embedding shape at nb_frames={nb_frames}: {emb.shape}  ✓")

    # Verify text encoder output shape with 2 classes
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
    parser.add_argument("--output-dir",       default="model", help="Output directory (default: ./model)")
    parser.add_argument("--device",           default="cpu",   help="Torch device for export (cpu|mps)")
    parser.add_argument("--max-text-length",  type=int, default=77)
    parser.add_argument("--default-context-ms", type=int, default=1000,
                        help="Default audio context window in ms used by clap~ on load (default: 1000)")
    parser.add_argument("--skip-verify",      action="store_true",
                        help="Skip verification step (useful if onnxruntime is not installed)")
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
    mel_filters = torch.from_numpy(fe.mel_filters.copy()).float()  # [513, 64]

    print(f"Default context: {args.default_context_ms} ms (adjustable at runtime in Max)")

    # Use raw nn.Module wrappers (not pre-traced TorchScript) so ONNX can trace the full graph
    from export_clap import AudioEncoderWrapper
    raw_audio_encoder = AudioEncoderWrapper(model.audio_model, model.audio_projection).to(device)
    raw_audio_encoder.eval()

    # Mel preprocessing is done in C++ — ONNX model takes pre-computed features [1,4,F,64]
    audio_wrapper = AudioONNXWrapper(audio_encoder=raw_audio_encoder).to(device)

    text_wrapper = TextONNXWrapper(
        text_model      = model.text_model,
        text_projection = model.text_projection,
    ).to(device)

    # ── 4. Export ────────────────────────────────────────────────────────────
    audio_onnx_path = output_dir / "clap_tilde_audio.onnx"
    text_onnx_path  = output_dir / "clap_tilde_text.onnx"

    print(f"Exporting audio encoder → {audio_onnx_path}")
    export_audio_encoder(audio_wrapper, device, audio_onnx_path)

    print(f"Exporting text encoder  → {text_onnx_path}")
    export_text_encoder(text_wrapper, args.max_text_length, device, text_onnx_path)

    # ── 5. Write metadata sidecar ────────────────────────────────────────────
    meta = {
        "sr":                  48000,
        "default_context_ms":  args.default_context_ms,
        "n_fft":               fe.n_fft,
        "hop_length":          fe.hop_length,
        "max_text_length":     args.max_text_length,
        "logit_scale_a":       float(model.logit_scale_a.item()),
        # mel_filters saved separately as binary for C++ to load
    }
    meta_path = output_dir / "clap_tilde_meta.json"
    meta_path.write_text(json.dumps(meta, indent=2))
    print(f"Metadata sidecar        → {meta_path}")

    # Save mel filters as raw float32 binary [513 × 64], row-major (C order)
    mel_path = output_dir / "clap_tilde_mel_filters.bin"
    import numpy as np
    mel_np = fe.mel_filters.copy().astype(np.float32)  # [513, 64]
    mel_np.tofile(str(mel_path))
    print(f"Mel filters             → {mel_path}  ({mel_np.shape}, float32)")

    # ── 6. Verify ────────────────────────────────────────────────────────────
    if not args.skip_verify:
        print("Verifying exports...")
        verify_exports(audio_onnx_path, text_onnx_path, args.max_text_length)
    else:
        print("Verification skipped.")

    # ── 7. Summary ───────────────────────────────────────────────────────────
    print(f"\nExport complete:")
    print(f"  {audio_onnx_path}  (dynamic nb_frames axis)")
    print(f"  {text_onnx_path}")
    print(f"  {meta_path}")
    print(f"  {mel_path}")
    print(f"  {output_dir / 'vocab.json'}")
    print(f"  {output_dir / 'merges.txt'}")
    print()
    print("Load in Max with:")
    print(f"  clap~ {output_dir}         (auto-detect)")
    print(f"  clap~ {output_dir} mps     (CoreML / Apple Silicon)")
    print(f"Set context length at runtime with the 'context' attribute (ms).")


if __name__ == "__main__":
    main()
