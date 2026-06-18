<sub>STLZ is public source — the code is open for inspection, modification, and non-commercial use under CC BY-NC-SA 4.0.</sub>

# STLZ — Striped Delta-Stride LZ Image Codec

[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC_BY--NC--SA_4.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc-sa/4.0/)
[![Language: C99](https://img.shields.io/badge/language-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![Platform: ESP32-S3](https://img.shields.io/badge/platform-ESP32--S3-green.svg)](https://www.espressif.com/)

> *"Render 256×192 MSX screenshots on 640×416 screens with ~5 KB RAM."*

A domain-optimized image codec for memory-constrained embedded systems.  
Splits framebuffers into independent horizontal stripes, applies delta-XOR stride preprocessing per stripe,  
then compresses each with LZ77. Enables **postrоchny rendering without a full-frame buffer** — decode only the stripe you need.

Designed for **retro emulator frontends** (fMSX/Retro-Go on ESP32-S3) where PSRAM is limited to ~656 KB and full-frame buffers cause OOM.

## Why STLZ?

| Problem | Legacy (PNG/LodePNG) | STLZ |
|:--------|:---------------------|:-----|
| Save state screenshot | ~530 KB – 1 MB PSRAM peak | **~16 KB** peak |
| Menu preview render | ~630 KB PSRAM (full decode + resize) | **~5 KB** + file_data (~15–30 KB) |
| Old PNG compatibility | Crash on large images | **~99 KB** peak (single-line resize) |

## Features

- **Stripe-level random access** — decode any stripe independently via offset table
- **Postrоchny rendering without full-frame buffer** — peak heap: ~5 KB (stripe_buf + line_buffer)
- **2D Delta-XOR stride preprocessing** per stripe — turns repeating tiles into zero runs
- **Reduced hash table** — 1024 buckets (8 KB) instead of 4096 (32 KB), zero compression loss on 4 KB stripes
- **Zero external dependencies** — only stdio, stdint, string, stdlib, stdbool
- **Header-only codec core** — `static inline` LZ77 + delta in `ds-stlz.h`
- **File I/O convenience** — `ds-stlz.c` for compression, decompression, and scanline rendering

## Quick Start

```c
#include "ds-stlz.h"   // header-only codec core
// Link with ds-stlz.c for file I/O functions

// Compress raw pixel buffer (256x192, RGB565 LE) to STLZ file
uint8_t pixels[256 * 192 * 2];  // MSX screen buffer
stlz_compress_file(pixels, 256, 192, STLZ_FMT_RGB565_LE, 8, "screenshot.stlz");

// Decompress full image into pre-allocated framebuffer
stlz_decompress_full(framebuffer, "screenshot.stlz");

// Render scanline-by-scanline with nearest-neighbor scaling
// Peak RAM: ~5 KB (stripe_buf 4 KB + line_buffer <2 KB) + file_data
stlz_render_scanlines("screenshot.stlz", 640, 416, true, my_callback, &ctx);
```

### Pixel Formats

| Format | Value | Description | Bytes/Pixel |
|:-------|:-----:|:------------|:-----------:|
| `STLZ_FMT_RGB565_BE` | 1 | RGB565 big-endian | 2 |
| `STLZ_FMT_RGB565_LE` | 2 | RGB565 little-endian | 2 |
| `STLZ_FMT_RGB888` | 3 | RGB24 true color | 3 |

### Stripe Height

Typical value: **8** pixels. For MSX 256×192 RGB565:
- Stripe bytes uncompressed: `256 × 8 × 2 = 4096` bytes (4 KB)
- Number of stripes: `ceil(192 / 8) = 24`
- Hash table: 1024 buckets × 8 bytes = 8 KB — optimal for 4 KB stripes

## File Format

```
[Header: 16 bytes]      magic, width, height, stripe_h, num_stripes, format, reserved
[Offset table]          num_stripes × uint32_t (absolute file offsets)
[Compressed stripe 0]   independent LZ77 stream (with per-stripe delta-XOR)
[Compressed stripe 1]   independent LZ77 stream
...
[Compressed stripe N-1] independent LZ77 stream
```

See [SPECIFICATION.md](SPECIFICATION.md) for full binary format details.

## API Reference

### File Inspection

| Function | Description |
|----------|-------------|
| `is_stlz_file(path)` | Check if file starts with "STLZ" magic |
| `stlz_get_dimensions(path, &w, &h)` | Read image dimensions |
| `stlz_get_info(path, &w, &h, &fmt, &stripe_h)` | Read full metadata |

### Compression

| Function | Description |
|----------|-------------|
| `stlz_compress_file(pixels, w, h, fmt, stripe_h, path)` | Compress raw pixels to STLZ file |

**Peak RAM during compression** (256×192, stripe_h=8, RGB565):
- `hash`: 1024 × 8 = **8 KB**
- `stripe_buf`: 256 × 8 × 2 = **4 KB**
- `comp_buf`: 4096 + 256 = **~4.2 KB**
- `offsets`: 24 × 4 = **96 bytes**
- **Total: ~16.3 KB**

### Decompression

| Function | Description |
|----------|-------------|
| `stlz_decompress_stripe(dst, path, idx)` | Decompress single stripe into pre-allocated buffer |
| `stlz_decompress_full(dst, path)` | Decompress entire image into pre-allocated buffer |

### Scanline Render

| Function | Description |
|----------|-------------|
| `stlz_render_scanlines(path, dw, dh, swap, callback, userdata)` | Decode scanline-by-scanline with NN-scaling |

**Peak RAM during scanline render**:
- `file_data`: compressed file size = **~15–30 KB** (read entirely)
- `stripe_buf`: width × stripe_h × bpp = **4 KB**
- `line_buffer`: dest_w × 2 = **<2 KB** (e.g., 640 × 2 = 1280 bytes)
- **Dynamic heap total: ~5 KB** (stripe + line), plus file_data

**Callback signature:**
```c
void callback(int y, const uint16_t *line, void *userdata);
```

## Performance (ESP32-S3, 240 MHz)

### Memory Footprint

| Operation | Peak RAM | Formula |
|:----------|:---------|:--------|
| Compression | **~16 KB** | hash(8K) + stripe_buf(4K) + comp_buf(~4.2K) + offsets |
| Full decompress | **~4 KB** | stripe_buf (reused per stripe) |
| Scanline render | **~5 KB** + file_data | stripe_buf(4K) + line_buffer(<2K) |
| Legacy PNG resize | **~630 KB** | full decode + scaled buffer |

### Compression Ratio vs Alternatives (256×192 RGB565, real games)

| Algorithm | Encode RAM | Decode RAM | Avg File Size | Decode Speed | Scene Dependency |
|:----------|:-----------|:-----------|:--------------|:-------------|:-----------------|
| **STLZ** | **~16 KB** | **~5 KB** | **12–25 KB** | **~25 MB/s** | Low (Delta-XOR) |
| QOI565 | ~0.5 KB | ~0.5 KB | 22–40 KB | ~35 MB/s | High (no 2D) |
| LZ4 | ~16–64 KB | 0 KB | 20–38 KB | ~45 MB/s | Medium |
| RLE | 0 KB | 0 KB | 35–90 KB | ~55 MB/s | Critical |

**Key insight:** STLZ achieves **1.5–3× smaller files** than QOI565/RLE on real game screens because 2D Delta-XOR turns repeating tile rows into zero runs that LZ77 compresses near-perfectly.

### Speed

| Operation | Speed (ESP32-S3) |
|:----------|:-----------------|
| Encode | ~3.5 MB/s |
| Full decode | ~25 MB/s |
| Scanline render (with scaling) | ~15–20 MB/s |

## Architecture

### Stripe Independence

Each stripe is a self-contained LZ77 stream:
- Own delta-XOR preprocessing with `stride = width × bpp`
- Own hash table state (reset per stripe via `memset(hash, 0xFF, ...)`)
- Own offset in file (via offset table)

This enables:
- **Random access** — jump to any stripe without decoding previous
- **Resilient storage** — corruption in one stripe doesn't affect others
- **Bounded memory** — decoder never needs more than one stripe buffer

### Delta-XOR Per Stripe

```
Original stripe (8 rows × 256 pixels × 2 bytes = 4096 bytes):
┌─────────────────────────────────────────┐
│ Row 0: [R0G0][R0G1]...[R0G255]        │
│ Row 1: [R1G0][R1G1]...[R1G255]        │
│ ...                                     │
│ Row 7: [R7G0][R7G1]...[R7G255]        │
└─────────────────────────────────────────┘
                    ↓ Delta-XOR encode
┌─────────────────────────────────────────┐
│ Row 0: [R0G0][R0G1]...[R0G255]        │  (unchanged)
│ Row 1: [ΔV0][ΔV1]...[ΔV255]            │  (vertical XOR: R1^R0)
│ ...                                     │
│ Row 7: [ΔV0][ΔV1]...[ΔV255]            │
└─────────────────────────────────────────┘
                    ↓ Horizontal XOR within each row
Identical tiles → long zero runs → LZ77 compresses efficiently
```

### Scanline Render Pipeline

```
[STLZ file on SD] (~15–30 KB)
         │
         ▼ (read entire file to heap)
[file_data buffer]
         │
    ┌────┴────┐
    ▼         ▼
[Offsets]  [Compressed stripe N]
                │
                ▼ (LZ77 decompress)
         [stripe_buf] (4 KB)
                │
                ▼ (Delta-XOR decode)
         [stripe_buf] (decoded pixels)
                │
                ▼ (NN-scale + byte-swap)
         [line_buffer] (<2 KB)
                │
                ▼ (copy to screen)
         rg_gui_copy_buffer()
```

**Memory freed immediately** after rendering completes.

## Use Cases

**Designed for:**
- Retro emulator save state screenshots (fMSX, etc.)
- Embedded UI graphics on microcontrollers with <1 MB PSRAM
- LCD/OLED displays where source image exceeds available GRAM
- Menu preview systems requiring fast thumbnail rendering

**Not suitable for:**
- Natural photographs (use JPEG)
- Video streams (no temporal compression)
- General text compression (use DS-LZ or gzip)

## Requirements

- **C99 compiler** (GCC, Clang, xtensa-esp32-elf)
- Standard library: `<stdio.h>`, `<stdint.h>`, `<string.h>`, `<stdlib.h>`, `<stdbool.h>`
- **Target RAM for rendering:** ~5 KB dynamic heap + file size

## Relationship to DS-LZ

STLZ builds upon the DS-LZ compression engine:

| Component | Source | Modification |
|:----------|:-------|:-------------|
| LZ77 compressor/decompressor | DS-LZ | Identical token format |
| Delta-XOR algorithm | DS-LZ | Identical, but `stride = width × bpp` per stripe |
| Hash function | DS-LZ | Identical |
| Cost model | DS-LZ | Identical |
| File container | **STLZ** | Header + offset table + independent stripes |
| Hash table size | **STLZ** | Reduced from 4096 to 1024 (8 KB vs 32 KB) |
| Pixel format abstraction | **STLZ** | RGB565 BE/LE, RGB888 |
| Scanline renderer | **STLZ** | Built-in NN-scaling + byte-swap |

## Acknowledgments

STLZ builds upon DS-LZ and the work of many engineers. See [DS-LZ acknowledgments](https://github.com/Svarkovsky/dslz) for full list.

### Retro Emulator & Embedded Graphics
- **Marat Fayzullin** — fMSX, the portable MSX emulator
- **Ducalex** — Retro-Go, ESP32 retro gaming framework
- **LVGL** — Lightweight embedded graphics library

## License

**CC BY-NC-SA 4.0**

- ✅ Share — copy and redistribute
- ✅ Adapt — remix and build upon
- ❌ **Commercial use is prohibited without written permission**
- ⚠️ ShareAlike — derivatives must use the same license

**Attribution required:** name, link to repository, indication of changes.

Full license: [LICENSE.md](LICENSE.md) · [NOTICE.md](NOTICE.md)

Commercial licensing: ivansvarkovsky@gmail.com

Based on DS-LZ compression engine and public-domain LZ77 (Lempel-Ziv, 1977). All patents expired. Independent implementation.

## Author

**Ivan Svarkovsky** — [GitHub](https://github.com/Svarkovsky) — ivansvarkovsky@gmail.com

## Repositories

**STLZ:** [github.com/Svarkovsky/stlz](https://github.com/Svarkovsky/stlz)  
**DS-LZ (engine):** [github.com/Svarkovsky/dslz](https://github.com/Svarkovsky/dslz)
