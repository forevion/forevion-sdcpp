# Kandinsky 6

Kandinsky 6.0 Lite generates video together with synchronized audio from text. The distilled checkpoint
(Kandinsky-6.0-Lite-distill-5s) samples in 10 PiFlow steps without classifier-free guidance. This
integration supports text-to-video with Qwen2.5-VL 7B and CLIP-L text encoders, decoded with the
HunyuanVideo VAE or the TAEHV tiny decoder. The audio latent is denoised together with the video, as the model requires, but is not
decoded to sound yet.

## Model files

| Component | Weights and downloads | CLI option | Requirement |
| --- | --- | --- | --- |
| Kandinsky 6.0 Lite distill transformer | `transformer/diffusion_pytorch_model.safetensors` from [kandinskylab/Kandinsky-6.0-Lite-distill-5s-Diffusers](https://huggingface.co/kandinskylab/Kandinsky-6.0-Lite-distill-5s-Diffusers) | `--diffusion-model` | Required |
| Qwen2.5-VL 7B | the same repository's `text_encoder/` folder (sharded safetensors; the vision tower is not loaded), or a GGUF converted from it | `--llm` | Required |
| CLIP-L | the same repository's `text_encoder_2/model.safetensors` | `--clip_l` | Required |
| HunyuanVideo VAE | the same repository's `vae/diffusion_pytorch_model.safetensors` (decoder only) | `--vae` | Use this or TAEHV |
| TAEHV | [taehv.safetensors](https://github.com/madebyollin/taehv/tree/main/safetensors) (HunyuanVideo 1 tiny autoencoder) | `--taesd` | Use this or the VAE |

The VAE gives the sharpest frames; TAEHV decodes about 50 times faster in far less memory.

The transformer and the Qwen2.5-VL folder can be converted once to smaller GGUF files:

```powershell
.\bin\Release\sd-cli.exe -M convert -m ..\models\kandinsky-6.0-lite-distill\transformer\diffusion_pytorch_model.safetensors -o ..\models\diffusion_models\kandinsky6-lite-distill-q8_0.gguf --type q8_0
.\bin\Release\sd-cli.exe -M convert -m ..\models\kandinsky-6.0-lite-distill\text_encoder -o ..\models\text_encoders\kandinsky6-qwen2.5-vl-7b-q8_0.gguf --type q8_0
```

Keep CLIP-L unquantized; its pooled vector is sensitive to quantization.

## Usage

### DGL

| Example | Description |
| --- | --- |
| [Text to video](../dgl/kandinsky6/kandinsky6-lite-distill-txt2video.dgl) | Kandinsky 6.0 Lite distill, 864x480, 49 frames, TAEHV decode. |

#### Run a script

Edit paths, prompts and settings in the script's `var` block, then run from
the build directory. Paths inside the script resolve from the working directory.

**Windows (PowerShell)**

```powershell
.\bin\Release\sd-cli.exe -M dgl --script ..\docs\dgl\kandinsky6\kandinsky6-lite-distill-txt2video.dgl --mmap
```

**Linux/macOS**

```sh
./bin/sd-cli -M dgl --script ../docs/dgl/kandinsky6/kandinsky6-lite-distill-txt2video.dgl --mmap
```

Add `--check` to validate syntax and types without loading weights. See the
[shared run instructions](../dgl/README.md#run-an-example) for build paths
and the server's file-access rules.

### Legacy CLI

CLI examples use the [shared path and shell conventions](README.md#example-conventions).

#### Text-to-video example

**Windows (PowerShell)**

```powershell
.\bin\Release\sd-cli.exe `
  -M vid_gen `
  --diffusion-model ..\models\kandinsky-6.0-lite-distill\transformer\diffusion_pytorch_model.safetensors `
  --llm ..\models\kandinsky-6.0-lite-distill\text_encoder `
  --clip_l ..\models\kandinsky-6.0-lite-distill\text_encoder_2\model.safetensors `
  --taesd ..\models\vae\taehv.safetensors `
  -p "A red fox trotting through a snowy pine forest at sunrise, soft golden light, cinematic." `
  --cfg-scale 1.0 `
  --steps 10 `
  -W 864 `
  -H 480 `
  --video-frames 49 `
  --fps 24 `
  --mmap `
  -v
```

**Linux/macOS**

```sh
./bin/sd-cli \
  -M vid_gen \
  --diffusion-model ../models/kandinsky-6.0-lite-distill/transformer/diffusion_pytorch_model.safetensors \
  --llm ../models/kandinsky-6.0-lite-distill/text_encoder \
  --clip_l ../models/kandinsky-6.0-lite-distill/text_encoder_2/model.safetensors \
  --taesd ../models/vae/taehv.safetensors \
  -p "A red fox trotting through a snowy pine forest at sunrise, soft golden light, cinematic." \
  --cfg-scale 1.0 \
  --steps 10 \
  -W 864 \
  -H 480 \
  --video-frames 49 \
  --fps 24 \
  --mmap \
  -v
```

## Options

- Use `--cfg-scale 1.0` and `--steps 10` with the distilled checkpoint. The model definition selects the
  `euler` sampler and the `piflow` scheduler by default; other samplers are not supported, because
  each step integrates the model's PiFlow policy up to the next sigma.
- `--video-frames` is aligned to 4n+1 (121 frames is the 5-second clip of the reference pipeline at 24 fps).
  `--fps` sets the length of the jointly denoised audio latent.
- 864x480 is the native size; width and height must be multiples of 16.

## Notes and limitations

- Image-to-video and audio decoding (MMAudio VAE and vocoder) are not supported yet. The HunyuanVideo VAE
  is decode-only.
- The VAE decodes in temporal tiles of 4 latent frames that overlap by 2, about 42 dB against an untiled
  decode. In DGL, `tiling(temporal_tiling = true, extra_args = "temporal_tile_overlap=3")` gives about 44 dB
  at twice the decode time. On CPU its 3D convolutions need spatial tiles at almost any size; pass
  `--vae-tiling` to skip the failed untiled attempt.
- The text encoder and transformer weights are bf16; `--mmap` maps them from disk instead of copying them.
  On machines where the automatic placement plan cannot fit both into RAM, add `--auto-fit off` so the mapped
  weights are used directly.
- Transformer weight types, measured on the first step of a 448x256, 17-frame clip against an fp32 PyTorch
  reference (the official bf16 pipeline differs from it by about 1%):

  | Weights | File | Step time | Peak memory | Error |
  | --- | --- | --- | --- | --- |
  | bf16 | 6.4 GB | 9.6 s | 7.6 GB | 0.2% |
  | q8_0 | 3.2 GB | 10.3 s | 5.3 GB | 1.2% |
  | q4_K | 2.4 GB | 5.0 s | 4.8 GB | 6.4% |

- Measured on a 16-core Ryzen 9 9950X, CPU only, bf16 weights, flash attention on, 10 steps:

  | Size | Frames | Transformer step | Total |
  | --- | --- | --- | --- |
  | 448x256 | 17 | 7 s | 82 s |
  | 864x480 | 49 | 102 s | 17.7 min (11.4 GB private memory) |

  The same 864x480, 49-frame clip with q8_0 transformer and text encoder and the full VAE took 70.5 min
  at 4.8 GB private memory: 125 s per step and 49 min of VAE decoding (72 spatial tiles of 128x128).
  Use TAEHV for drafts and the VAE for final renders.
