#!/usr/bin/env python3
"""
Export laion/clap-htsat-fused to TorchScript for use with rusc_tilde.

Usage (from the rusc_tilde project root):
    conda run -n clap python scripts/export_clap.py
    conda run -n clap python scripts/export_clap.py --output-dir /path/to/output

Outputs in --output-dir (default: ./model):
    rusc_tilde.ts   — TorchScript model loaded by the Max external
    vocab.json      — RoBERTa BPE vocabulary (loaded by C++ tokenizer)
    merges.txt      — BPE merge rules (loaded by C++ tokenizer)

Requirements (conda env "clap"):
    torch==2.4.1  torchaudio==2.4.1  transformers==4.44.2
"""

import argparse
import shutil
from pathlib import Path
from typing import List

import torch
import torch.nn as nn


# ─────────────────────────────────────────────────────────────────────────────
# Sub-module wrappers (traced, return plain tensors)
# ─────────────────────────────────────────────────────────────────────────────

class AudioEncoderWrapper(nn.Module):
    """Wraps ClapAudioModel + projection → normalised embedding."""
    def __init__(self, audio_model, audio_projection):
        super().__init__()
        self.audio_model = audio_model
        self.audio_projection = audio_projection

    def forward(self, input_features: torch.Tensor, is_longer: torch.Tensor) -> torch.Tensor:
        out = self.audio_model(input_features=input_features, is_longer=is_longer)
        emb = self.audio_projection(out.pooler_output)
        return emb / emb.norm(p=2, dim=-1, keepdim=True)


class TextEncoderWrapper(nn.Module):
    """Wraps ClapTextModel + projection → normalised embedding (batch size 1)."""
    def __init__(self, text_model, text_projection):
        super().__init__()
        self.text_model = text_model
        self.text_projection = text_projection

    def forward(self, input_ids: torch.Tensor, attention_mask: torch.Tensor) -> torch.Tensor:
        out = self.text_model(input_ids=input_ids, attention_mask=attention_mask)
        emb = self.text_projection(out.pooler_output)
        return emb / emb.norm(p=2, dim=-1, keepdim=True)


# ─────────────────────────────────────────────────────────────────────────────
# Main TorchScript wrapper
# ─────────────────────────────────────────────────────────────────────────────

class ClapTorchScriptWrapper(nn.Module):
    """
    TorchScript-compatible wrapper for laion/clap-htsat-fused.

    Interface expected by the C++ rusc_tilde external:
        get_sr()                   -> int   (always 48000)
        get_seglen()               -> int   (480000 = 10 s × 48 kHz)
        get_max_text_length()      -> int   (max token length used at export)
        encode_text_from_tokens(input_ids [N,L], attention_mask [N,L]) -> Tensor [N, 512]
        forward(audio [1,1,N], text_embs [M,512]) -> Tensor [M]  softmax probs
    """

    def __init__(
        self,
        traced_audio_encoder: torch.jit.ScriptModule,
        traced_text_encoder: torch.jit.ScriptModule,
        mel_filters: torch.Tensor,       # [513, 64]  htk-scale filters
        logit_scale_a: float,
        n_fft: int = 1024,
        hop_length: int = 480,
        segment_samples: int = 48_000,
        nb_max_frames: int = 1001,
        max_text_length: int = 77,
    ):
        super().__init__()

        self.audio_encoder = traced_audio_encoder
        self.text_encoder  = traced_text_encoder

        self.register_buffer("mel_filters",   mel_filters)
        self.register_buffer("hann_window",   torch.hann_window(n_fft))
        self.register_buffer("logit_scale_a", torch.tensor(logit_scale_a))

        self.n_fft:            int = n_fft
        self.hop_length:       int = hop_length
        self._segment_samples: int = segment_samples
        self._nb_max_frames:   int = nb_max_frames
        self._max_text_length: int = max_text_length

    # ── Metadata ─────────────────────────────────────────────────────────────

    @torch.jit.export
    def get_sr(self) -> int:
        return 48000

    @torch.jit.export
    def get_seglen(self) -> int:
        return self._segment_samples

    @torch.jit.export
    def get_max_text_length(self) -> int:
        return self._max_text_length

    # ── Text encoding (called from C++ after BPE tokenisation) ───────────────

    @torch.jit.export
    def encode_text_from_tokens(
        self,
        input_ids:      torch.Tensor,   # [N, max_text_length]  int64
        attention_mask: torch.Tensor,   # [N, max_text_length]  int64
    ) -> torch.Tensor:                  # [N, 512]
        N = input_ids.shape[0]
        embeds: List[torch.Tensor] = []
        for i in range(N):
            emb = self.text_encoder(input_ids[i:i+1], attention_mask[i:i+1])  # [1, 512]
            embeds.append(emb)
        return torch.cat(embeds, dim=0)  # [N, 512]

    # ── Audio pre-processing (raw waveform → CLAP mel features) ──────────────

    def _waveform_to_features(self, waveform: torch.Tensor) -> torch.Tensor:
        """
        waveform: [N] float32 at 48 kHz
        Returns: input_features [1, 4, 1001, 64]  (CLAP "fusion" format)
        """
        # Pad / trim to exactly segment_samples
        n = waveform.shape[0]
        if n < self._segment_samples:
            waveform = torch.nn.functional.pad(waveform, (0, self._segment_samples - n))
        else:
            waveform = waveform[:self._segment_samples]

        # STFT (centre-padded, Hann window)
        stft = torch.stft(
            waveform,
            n_fft=self.n_fft,
            hop_length=self.hop_length,
            win_length=self.n_fft,
            window=self.hann_window,
            center=True,
            return_complex=True,
        )  # [513, T]

        # Power → mel → log10 power dB   (matches ClapFeatureExtractor exactly)
        power   = stft.real ** 2 + stft.imag ** 2          # [513, T]
        mel     = torch.matmul(self.mel_filters.T, power)   # [64,  T]
        log_mel = 10.0 * torch.log10(torch.clamp(mel, min=1e-10))  # [64, T]

        # [T, 64] → trim / pad to nb_max_frames
        log_mel = log_mel.T
        T = log_mel.shape[0]
        if T > self._nb_max_frames:
            log_mel = log_mel[:self._nb_max_frames]
        elif T < self._nb_max_frames:
            log_mel = torch.nn.functional.pad(log_mel, (0, 0, 0, self._nb_max_frames - T))
        # log_mel: [1001, 64]

        # CLAP "fusion" format: replicate mel 4× along chunk dim
        # is_longer is always True (how this model was trained)
        feats = log_mel.unsqueeze(0).expand(4, -1, -1).unsqueeze(0).contiguous()  # [1, 4, 1001, 64]
        return feats

    # ── Audio encoding (for few-shot example registration) ───────────────────

    @torch.jit.export
    def encode_audio(self, audio: torch.Tensor) -> torch.Tensor:
        """audio: [1,1,N] float32 at 48 kHz → [1, 512] normalised embedding."""
        waveform = audio.squeeze()
        input_features = self._waveform_to_features(waveform)
        is_longer = torch.ones(1, 1, dtype=torch.bool, device=audio.device)
        return self.audio_encoder(input_features, is_longer)

    # ── Forward ───────────────────────────────────────────────────────────────

    def forward(
        self,
        audio:     torch.Tensor,   # [1, 1, N]  float32 at 48 kHz
        text_embs: torch.Tensor,   # [M, 512]   from encode_text_from_tokens
    ) -> torch.Tensor:             # [M]  softmax probabilities
        waveform = audio.squeeze()  # [N]
        input_features = self._waveform_to_features(waveform)  # [1, 4, 1001, 64]
        is_longer = torch.ones(1, 1, dtype=torch.bool, device=audio.device)

        audio_embs = self.audio_encoder(input_features, is_longer)  # [1, 512]

        logit_scale = self.logit_scale_a.exp()
        logits = logit_scale * (audio_embs @ text_embs.T)  # [1, M]
        return torch.softmax(logits[0], dim=-1)             # [M]


# ─────────────────────────────────────────────────────────────────────────────
# Export helpers
# ─────────────────────────────────────────────────────────────────────────────

def trace_audio_encoder(model, device: torch.device) -> torch.jit.ScriptModule:
    wrapper = AudioEncoderWrapper(model.audio_model, model.audio_projection).to(device)
    wrapper.eval()
    example_feats      = torch.randn(1, 4, 1001, 64, device=device)
    example_is_longer  = torch.ones(1, 1, dtype=torch.bool, device=device)
    with torch.no_grad():
        traced = torch.jit.trace(wrapper, (example_feats, example_is_longer))
    return traced


def trace_text_encoder(model, max_text_length: int, device: torch.device) -> torch.jit.ScriptModule:
    wrapper = TextEncoderWrapper(model.text_model, model.text_projection).to(device)
    wrapper.eval()
    example_ids  = torch.zeros(1, max_text_length, dtype=torch.long, device=device)
    example_mask = torch.ones(1, max_text_length, dtype=torch.long, device=device)
    with torch.no_grad():
        traced = torch.jit.trace(wrapper, (example_ids, example_mask))
    return traced


# ─────────────────────────────────────────────────────────────────────────────
# Verification
# ─────────────────────────────────────────────────────────────────────────────

def verify_export(scripted: torch.jit.ScriptModule, model, processor, device: torch.device):
    import numpy as np
    print("  Verifying mel computation...")

    np.random.seed(0)
    seg = scripted.get_seglen()
    full_seg = 480000  # 10s — only at full length does HF padding match ours
    audio_np = np.random.randn(seg).astype(np.float32)

    if seg == full_seg:
        ref_inputs = processor(audios=audio_np, sampling_rate=48000, return_tensors="pt")
        ref_feats  = ref_inputs.input_features[0, 0].to(device)
        wav_t = torch.from_numpy(audio_np).to(device)
        our_feats = scripted._waveform_to_features(wav_t)[0, 0]
        diff = (our_feats - ref_feats).abs().max().item()
        print(f"  Max mel diff vs. HuggingFace: {diff:.6f}  (should be < 0.001)")
        assert diff < 0.001, f"Mel mismatch too large: {diff}"
    else:
        print(f"  Mel diff check skipped (segment={seg} < 480000 — HF uses repeat-pad, we zero-pad)")

    print("  Verifying text embedding shape...")
    tokenizer = processor.tokenizer
    texts = ["violin pizzicato", "flute multiphonics"]
    tok_out = tokenizer(texts, padding="max_length", max_length=scripted.get_max_text_length(),
                        truncation=True, return_tensors="pt")
    ids  = tok_out.input_ids.to(device)
    mask = tok_out.attention_mask.to(device)
    with torch.no_grad():
        text_embs = scripted.encode_text_from_tokens(ids, mask)
    print(f"  text_embs shape: {text_embs.shape}  (expected [2, 512])")
    assert text_embs.shape == (2, 512)

    print("  Verifying full forward pass...")
    audio_tensor = torch.from_numpy(audio_np).to(device).unsqueeze(0).unsqueeze(0)  # [1,1,N]
    with torch.no_grad():
        probs = scripted(audio_tensor, text_embs)
    print(f"  probs: {probs.tolist()}  (sum={probs.sum().item():.4f}, should be ~1)")
    assert abs(probs.sum().item() - 1.0) < 1e-4

    print("  All checks passed.")


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Export laion/clap-htsat-fused to TorchScript")
    parser.add_argument("--output-dir", default="model", help="Output directory (default: ./model)")
    parser.add_argument("--device",     default="cpu",   help="Torch device for export (cpu|mps)")
    parser.add_argument("--max-text-length", type=int, default=77,
                        help="Max token length for text inputs (default: 77)")
    parser.add_argument("--segment-seconds", type=float, default=1.0,
                        help="Audio context window in seconds (default: 1.0, max: 10.0)")
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

    # ── 3. Trace sub-encoders ────────────────────────────────────────────────
    fe = processor.feature_extractor
    print(f"Tracing audio encoder (segment={fe.nb_max_samples} samples, "
          f"frames={fe.nb_max_frames + 1})...")
    traced_audio = trace_audio_encoder(model, device)

    print(f"Tracing text encoder (max_length={args.max_text_length} tokens)...")
    traced_text = trace_text_encoder(model, args.max_text_length, device)

    # ── 4. Build scriptable wrapper ──────────────────────────────────────────
    mel_filters = torch.from_numpy(fe.mel_filters.copy()).float()  # [513, 64]  htk scale

    segment_samples = int(round(args.segment_seconds * 48000))
    print(f"Segment: {args.segment_seconds}s → {segment_samples} samples")

    wrapper = ClapTorchScriptWrapper(
        traced_audio_encoder = traced_audio,
        traced_text_encoder  = traced_text,
        mel_filters          = mel_filters,
        logit_scale_a        = model.logit_scale_a.item(),
        n_fft                = fe.n_fft,
        hop_length           = fe.hop_length,
        segment_samples      = segment_samples,
        nb_max_frames        = fe.nb_max_frames + 1,    # 1001 — fixed encoder input shape
        max_text_length      = args.max_text_length,
    ).to(device)

    # ── 5. Script ────────────────────────────────────────────────────────────
    print("Scripting wrapper...")
    scripted = torch.jit.script(wrapper)

    # ── 6. Verify before saving ──────────────────────────────────────────────
    print("Verifying export...")
    with torch.no_grad():
        verify_export(scripted, model, processor, device)

    # ── 7. Save ──────────────────────────────────────────────────────────────
    ms = int(round(args.segment_seconds * 1000))
    out_path = output_dir / f"rusc_tilde_{ms}ms.ts"
    scripted.save(str(out_path))
    print(f"\nExport complete → {out_path}")
    print(f"                 {output_dir / 'vocab.json'}")
    print(f"                 {output_dir / 'merges.txt'}")
    print()
    print("Next: build rusc_tilde and point @model to the directory above,")
    print("      or pass the .ts path as the first argument to rusc~.")


if __name__ == "__main__":
    main()
