# Kandinsky 6 VSR

Kandinsky 6.0 VSR upscales an existing video 2x, 2.25x or 4x in two distilled PiFlow steps. It
takes no prompt and no text encoder. The pipeline follows `Kandinsky6SRPipeline` in diffusers:

1. The whole clip is encoded by the causal K-VAE (64 latent channels, 16x spatial and 4x temporal
   compression).
2. The clip is cut into overlapping tiles. Each tile is refined at one of the trained output
   sizes (512x512, 512x768 or 768x512, the one closest to the clip's aspect ratio).
3. Each latent tile is enlarged by the 2x or 4x latent upscaler and mixed with noise
   (`sqrt(1 - 0.7^2) * latent + 0.7 * noise`).
4. Each tile is denoised in two PiFlow steps by a 1.4B text-free transformer.
5. Each tile is decoded and the tiles are blended with Hann windows.

2.25x first enlarges the pixels 1.125x (bilinear, rounded to 16) and then takes the 2x route.

## Checkpoints

| Checkpoint | Transformer | Flow shift | Family |
| --- | --- | --- | --- |
| [VSR 2.0](https://huggingface.co/kandinskylab/Kandinsky-6.0-VSR-2.0-distilled2steps-Diffusers) | 1.4B, 3.2 GB bf16 | 5.0 | `kandinsky6_vsr` |
| [VSR 5s](https://huggingface.co/kandinskylab/Kandinsky-6.0-VSR-distilled2steps-5s-Diffusers) | 1.4B, 3.2 GB bf16 | 3.5 | `kandinsky6_vsr_5s` |

Both checkpoints share the K-VAE and the latent upscaler bank, and their transformers have the
same layout. The flow shift is stored only in each repository's scheduler config, so pick the
5s checkpoint by family in DGL, or with the context `model_args` `variant=5s` in the
generation API and sd-server.

## Model files

| Component | Weights | CLI option / DGL loader | Notes |
| --- | --- | --- | --- |
| Transformer | `transformer/diffusion_pytorch_model.safetensors` | `--diffusion-model` / `load_dit` | Keep it bf16; Q8_0 adds 0.9% error over the two steps |
| K-VAE | `vae/diffusion_pytorch_model.safetensors` (7.0 GB fp32) | `--vae` / `load_vae` | Its convolutions run in f16, so an f16 GGUF (3.5 GB) gives the same result |
| Latent upscaler bank | `latent_upscaler/diffusion_pytorch_model.safetensors` (7.3 GB bf16) | `--latent-upscaler` / `load_latent_upscaler` | Holds the 2x and 4x models; `model_args = "scale=2"` or `"scale=4"` loads one |

Convert the K-VAE once:

```powershell
.\bin\Release\sd-cli.exe -M convert -m ..\models\kandinsky-6.0-vsr\vae\diffusion_pytorch_model.safetensors -o ..\models\vae\kandinsky6-vsr-kvae-f16.gguf --type f16
```

The 2x entry of the upscaler file also stores a backbone that its forward pass never reads; that
part is not loaded.

## Usage

### DGL

| Example | Description |
| --- | --- |
| [Upscale a video](../dgl/kandinsky6/kandinsky6-vsr-upscale.dgl) | Tile, refine and blend a clip; keeps the source audio |

```powershell
.\bin\Release\sd-cli.exe -M dgl --script ..\docs\dgl\kandinsky6\kandinsky6-vsr-upscale.dgl
```

The script uses two built-ins:

- `kandinsky6_vsr::plan(width, height, scale, min_overlap)` returns the resized input size, the
  latent upscale factor, the input-space tile size and the tile positions.
- `kandinsky6_vsr::blend(tiles, plan)` blends the decoded tiles into U8 video frames.

All tiles are refined before any is decoded, so the transformer and the upscaler can be released
before the decoder runs.

### Generation API and sd-server

Send the clip as `control_frames` at its own size and set `width` to 2x, 2.25x or 4x its width.
The prompt is ignored. The built-in `kandinsky6-vsr` recipe has the `diffusion_model`, `vae` and
`latent_upscaler` slots. In Forevion Studio, pick the **Upscale video** task on Create, or press
**Upscale** on a video in the Gallery.

## Notes and limitations

- The checkpoints were trained with NABLA sparse attention. This port runs full attention. The two
  are identical while a tile fits in the sliding-tile prior: at most 8 token blocks of 8x8 latents
  per side and at most 6 latent frames (21 video frames). Longer clips and 768-pixel tiles attend
  to more blocks than NABLA would; that case was not compared against the reference.
- Frame counts are `4n+1`; extra trailing frames are dropped.
- At 2x the clip must cover one input tile: 256x256, 384x256 or 256x384 pixels. At 4x it must cover
  128x128, 192x128 or 128x192.
- The K-VAE decodes one latent frame per graph and carries the causal padding between graphs. This
  is exact and bounds memory to four output frames per tile.
- The encoder returns the posterior mean, so `posterior = "sample"` also gives the mean.

## Speed and memory

Measured on a 16-core Ryzen 9 9950X without a GPU, 2x, a 448x256 clip with 9 frames (two
512x768 output tiles), bf16 transformer, f16 GGUF K-VAE, `scale=2` upscaler:

| Stage | Time |
| --- | --- |
| K-VAE encode | 5.5 s |
| Upscale and 2 PiFlow steps, per tile | about 21 s |
| K-VAE decode, both tiles | 663 s |
| Total through sd-server | 12 min 12 s, peak 13.5 GB RAM |

On that machine, with about 13 GB of commit free, a full 512x768 decode graph did not fit. The
VAE decode then retries with spatial tiling on its own (`Reducing VAE decode tiles`), which is
slower and not bit-exact at the seams. A 512x512 tile with two latent frames decodes in about
16 s when it fits. Machines with more free memory skip the fallback.

## Accuracy

Each stage was compared with the diffusers pipeline (fp32 activations) on a 5-frame 256x256 clip
at 2x, feeding every stage the reference output of the previous one:

| Stage | Relative L2 |
| --- | --- |
| K-VAE encode (posterior mean, scaled) | 0.14% |
| 2x latent upscaler | 0.02% |
| 4x latent upscaler (8x8 latent crop) | 0.03% |
| Transformer, first PiFlow step | 0.06% |
| After both PiFlow steps | 0.16% |
| K-VAE decode | 0.14% |

The f16 GGUF K-VAE gives the same figures (0.14% encode, 0.04% decode on a reconstruction).

Model reference: [kandinskylab/kandinsky-6](https://huggingface.co/kandinskylab).
