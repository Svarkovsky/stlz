<sub>STLZ is public source — the code is open for inspection, modification, and non-commercial use under CC BY-NC-SA 4.0.</sub>

# STLZ — Striped Delta-Stride LZ Image Codec

[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC_BY--NC--SA_4.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc-sa/4.0/)
[![Language: C99](https://img.shields.io/badge/language-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![Platform: ESP32-S3](https://img.shields.io/badge/platform-ESP32--S3-green.svg)](https://www.espressif.com/)
[![Header-Only Core](https://img.shields.io/badge/header--only%20core-yes-orange.svg)](#quick-start)
[![Zero Heap Render](https://img.shields.io/badge/zero%20heap%20render-yes-purple.svg)](#performance)

> *"Render 320×240 images on 128×64 screens with 4 KB RAM."*

A domain-optimized image codec for memory-constrained embedded systems.  
Splits framebuffers into independent horizontal stripes, applies delta-XOR stride preprocessing,  
then compresses each stripe with LZ77. Enables **zero-heap scanline rendering** — decode only what you need, when you need it.

## Features

- **Stripe-level random access** — decode any stripe independently via offset table
- **Zero-heap scanline rendering** — `stlz_render_scanlines()` uses only ~1 stripe + 1 output line
- **Delta-XOR stride preprocessing** per stripe for maximum compression
- **Portable pixel formats** — RGB565 BE/LE, RGB888 (platform-agnostic)
- **Nearest-neighbor scaling** — built-in resize to any target resolution
- **Header-only codec core** — `static inline` LZ77 + delta in `ds-stlz.h`
- **Zero external dependencies** — only stdio, stdint, string, stdlib
- **Configurable hash table** — `STLZ_HASH_SIZE` (default 1024, 8 KB RAM)

## Quick Start

```c
#include "ds-stlz.h"   // header-only codec core
// Link with ds-stlz.c for file I/O convenience functions

// Compress raw pixel buffer to STLZ file
stlz_compress_file(pixels, 320, 240, STLZ_FMT_RGB565_LE, 8, "image.stlz");

// Decompress full image
stlz_decompress_full(framebuffer, "image.stlz");

// Render scanline-by-scanline with scaling (zero full-frame buffer)
stlz_render_scanlines("image.stlz", 128, 64, false, my_callback, &ctx);
```

### Pixel Formats

| Format | Value | Description | Bytes/Pixel |
|:-------|:-----:|:------------|:-----------:|
| `STLZ_FMT_RGB565_BE` | 1 | RGB565 big-endian | 2 |
| `STLZ_FMT_RGB565_LE` | 2 | RGB565 little-endian | 2 |
| `STLZ_FMT_RGB888` | 3 | RGB24 true color | 3 |

### Stripe Height

Typical values: **8** (default), 16, 32. Smaller stripes = better random access granularity.  
Larger stripes = better compression ratio. Must divide evenly or last stripe is padded.

## File Format

```
[Header: 24 bytes]     magic, width, height, stripe_h, num_stripes, format, reserved
[Offset table]         num_stripes × uint32_t (absolute file offsets)
[Compressed stripes]   independent LZ77 streams, each with own delta-XOR preprocessing
```

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

### Decompression

| Function | Description |
|----------|-------------|
| `stlz_decompress_stripe(dst, path, idx)` | Decompress single stripe into pre-allocated buffer |
| `stlz_decompress_full(dst, path)` | Decompress entire image into pre-allocated buffer |

### Streaming Render

| Function | Description |
|----------|-------------|
| `stlz_render_scanlines(path, dw, dh, swap, callback, userdata)` | Decode scanline-by-scanline with scaling |

**Callback signature:**
```c
void callback(int y, const uint16_t *line, void *userdata);
```

## Performance

### Memory Footprint (ESP32-S3, 240 MHz)

| Operation | Peak RAM | Notes |
|:----------|:---------|:------|
| Full decompress | 1 stripe buffer | `width × stripe_h × bpp` |
| Scanline render | 1 stripe + 1 line | ~`width × stripe_h × bpp + dest_w × 2` |
| Compress | 1 stripe + hash table + I/O | ~`width × stripe_h × bpp + STLZ_HASH_SIZE × 8` |

### Compression Ratio vs DS-LZ (Same Source Data)

| Image | Resolution | Format | DS-LZ (raw) | STLZ (stripe=8) | STLZ (stripe=16) |
|:------|:-----------|:-------|:------------|:----------------|:-----------------|
| MSX Title Screen | 256×192 | RGB565 | 822× | 780× | 810× |
| UI Panel | 320×240 | RGB565 | 45× | 42× | 44× |
| Sprite Sheet | 128×128 | RGB888 | 12× | 11× | 11.5× |

> STLZ adds ~4–6% overhead vs DS-LZ due to stripe headers and offset table,  
> but enables random access and streaming render impossible with monolithic DS-LZ streams.

### Speed (ESP32-S3, 240 MHz)

| Operation | Speed |
|:----------|:------|
| Encode | ~3.5 MB/s |
| Full decode | ~25 MB/s |
| Scanline render (no scaling) | ~20 MB/s |
| Scanline render (with scaling) | ~15 MB/s |

## Architecture

### Stripe Independence

Each stripe is a self-contained LZ77 stream:
- Own delta-XOR preprocessing with stride = `width × bpp`
- Own hash table state (reset per stripe)
- Own offset in file (via offset table)

This enables:
- **Random access** — jump to any stripe without decoding previous
- **Parallel decode** — multiple stripes on multi-core (future)
- **Resilient storage** — corruption in one stripe doesn't affect others

### Delta-XOR Per Stripe

```
Original image:          Stripe 0 (rows 0-7):     After delta-XOR:
┌─────────────┐         ┌─────────────┐          ┌─────────────┐
│ Row 0       │         │ R0 R1 R2... │          │ R0 Δ1 Δ2... │
│ Row 1       │    →    │ R7 (padded) │    →     │ ...         │
│ ...         │         └─────────────┘          └─────────────┘
│ Row 7       │         stride = width × bpp
└─────────────┘
```

Vertical delta uses stride = `width × bpp` (row-to-row correlation).  
Horizontal delta uses stride = 1 (pixel-to-pixel correlation within row).

### Scanline Render Pipeline

```
For each output scanline y:
    1. Map y to source row: y_src = (y × src_h) / dest_h
    2. Determine stripe_idx = y_src / stripe_h
    3. If stripe not cached:
        a. Read compressed stripe from file
        b. LZ77 decompress to stripe buffer
        c. Delta-XOR decode
    4. Extract source row from stripe buffer
    5. Scale horizontally: nearest-neighbor
    6. Byte-swap if requested (BE ↔ LE)
    7. Invoke callback with output line
```

Peak RAM: `stripe_bytes + dest_w × 2` bytes. No full-frame buffer ever allocated.

## Use Cases

**Designed for:**
- Embedded UI graphics (buttons, panels, backgrounds)
- Sprite atlases with random access
- Framebuffer images larger than available RAM
- Retro emulator frontends on microcontrollers
- LCD/OLED displays with limited GRAM

**Not suitable for:**
- Natural photographs (use JPEG/PNG)
- Video streams (no temporal compression)
- General text compression (use DS-LZ or gzip)

## Requirements

- **C99 compiler** (GCC, Clang, xtensa-esp32-elf)
- Standard library: `<stdio.h>`, `<stdint.h>`, `<string.h>`, `<stdlib.h>`, `<stdbool.h>`
- **Target RAM:** 4–20 KB for scanline rendering

## Comparison with DS-LZ

| Feature | DS-LZ | STLZ |
|:--------|:------|:-----|
| **Data type** | Any binary data | Structured images |
| **Access pattern** | Sequential only | Random access (stripe-level) |
| **Container** | Raw stream | File with header + offset table |
| **Pixel formats** | None (raw bytes) | RGB565 BE/LE, RGB888 |
| **Rendering** | Manual | Built-in scanline + scaling |
| **Delta stride** | Configurable (128/256/...) | Auto (`width × bpp`) |
| **Decoder RAM** | 0 bytes | ~1 stripe + 1 line |
| **Hash table** | 4096 buckets (32 KB) | 1024 buckets (8 KB, configurable) |
| **Best for** | Save states, VRAM dumps | UI graphics, sprites, backgrounds |

## Relationship to DS-LZ

STLZ builds upon the DS-LZ compression engine:
- Same LZ77 token format (near/far/rep matches, escape codes)
- Same cost-based lazy matching
- Same 32-bit accelerated match extension
- Same 2-way associative hash

Key differences:
- STLZ wraps DS-LZ into a structured image container
- STLZ resets the compressor per stripe (independent streams)
- STLZ adds pixel format abstraction and scaling
- STLZ trades ~4% compression for random access capability

## Acknowledgments

STLZ builds upon DS-LZ and the work of many engineers. See [DS-LZ README](https://github.com/Svarkovsky/dslz) for full acknowledgments.

### Image Format Inspiration
- **PNG** — W3C, Adam7 interlacing and delta filters [RFC 2083](https://www.w3.org/TR/PNG/)
- **GIF** — Compuserve, LZW-based animation format
- **BMP** — Microsoft, simple DIB structure
- **PCX** — ZSoft, early tile-based raster format
- **TGA** — Truevision, scanline-oriented with RLE

### Embedded Graphics
- **LVGL** — Lightweight graphics library for embedded [lvgl.io](https://lvgl.io/)
- **TinyPNG** — PNG optimization for web/mobile
- **ImageMagick** — Reference for pixel format handling [imagemagick.org](https://imagemagick.org/)

## License

**CC BY-NC-SA 4.0**

- ✅ Share — copy and redistribute
- ✅ Adapt — remix and build upon
- ❌ **Commercial use is prohibited without written permission**
- ⚠️ ShareAlike — derivatives must use the same license

**Attribution required:** name, link to repository, indication of changes.

Full license: [LICENSE.md](LICENSE.md) · [NOTICE.md](NOTICE.md)

Commercial licensing: ivansvarkovsky@gmail.com

Based on public-domain LZ77. All patents expired. Independent implementation.

## Author

**Ivan Svarkovsky** — [GitHub](https://github.com/Svarkovsky) — ivansvarkovsky@gmail.com

## Repository

**STLZ:** [github.com/Svarkovsky/stlz](https://github.com/Svarkovsky/stlz)  
**DS-LZ (engine):** [github.com/Svarkovsky/dslz](https://github.com/Svarkovsky/dslz)
