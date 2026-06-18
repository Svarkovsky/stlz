# STLZ Image Format Specification

**Author:** Ivan Svarkovsky  
**Date:** June 2026  
**License:** CC BY-NC-SA 4.0  
**Repository:** [github.com/Svarkovsky/stlz](https://github.com/Svarkovsky/stlz)  
**Engine:** [github.com/Svarkovsky/dslz](https://github.com/Svarkovsky/dslz)

## 1. Introduction

STLZ (Striped Delta-Stride LZ) is a domain-optimized image codec for memory-constrained embedded systems. It combines the DS-LZ compression engine with a structured container format that enables stripe-level random access and zero-heap scanline rendering.

Unlike general-purpose image formats (PNG, BMP, GIF), STLZ is designed for scenarios where:
- The full decoded image does not fit in RAM
- Random access to image regions is required
- The display resolution differs from the source image resolution
- The target platform has 4–20 KB of available RAM

### 1.1 Terminology

| Term | Definition |
|------|------------|
| Stripe | A horizontal band of the image, `stripe_h` pixels tall |
| Stripe buffer | Decompressed pixel data for one stripe |
| Offset table | Array of absolute file offsets to each compressed stripe |
| Pixel format | Color encoding (RGB565 BE/LE, RGB888) |
| BPP | Bytes per pixel (2 for RGB565, 3 for RGB888) |
| Stride | Row width in bytes for delta preprocessing (`width × bpp`) |
| Literal | Raw byte copied to output |
| Match | Back-reference to previously decoded data |
| Rep-match | Match reusing the most recently used offset |

## 2. File Format

### 2.1 Overview

```
┌─────────────────────────────────────────────────────────────┐
│ Header (24 bytes)                                           │
├─────────────────────────────────────────────────────────────┤
│ Offset Table (num_stripes × 4 bytes)                        │
├─────────────────────────────────────────────────────────────┤
│ Compressed Stripe 0 (variable size)                         │
│ Compressed Stripe 1 (variable size)                         │
│ ...                                                         │
│ Compressed Stripe N-1 (variable size)                       │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 Header Structure

The header is 24 bytes, packed (no padding):

| Offset | Size | Field | Description |
|:------:|:----:|:------|:------------|
| 0 | 4 | `magic` | ASCII "STLZ" (0x53 0x54 0x4C 0x5A) |
| 4 | 2 | `width` | Image width in pixels (1–65535) |
| 6 | 2 | `height` | Image height in pixels (1–65535) |
| 8 | 1 | `stripe_h` | Stripe height in pixels (1–255) |
| 9 | 1 | `num_stripes` | Number of stripes (1–255) |
| 10 | 1 | `format` | Pixel format (1=RGB565_BE, 2=RGB565_LE, 3=RGB888) |
| 11–23 | 13 | `reserved` | Reserved for future use (must be zero) |

**Derived values:**
- `num_stripes = ceil(height / stripe_h)`
- `bpp = 2` if format ∈ {1, 2}, else `3` if format = 3
- `stripe_bytes = width × stripe_h × bpp` (uncompressed stripe size)
- `last_stripe_h = height - (num_stripes - 1) × stripe_h` (actual height of last stripe)

### 2.3 Offset Table

Immediately follows the header. Contains `num_stripes` entries of 32-bit little-endian unsigned integers.

| Entry | Description |
|:------|:------------|
| `offsets[0]` | Absolute file offset to start of Compressed Stripe 0 |
| `offsets[1]` | Absolute file offset to start of Compressed Stripe 1 |
| ... | ... |
| `offsets[N-1]` | Absolute file offset to start of Compressed Stripe N-1 |

**Compressed stripe size calculation:**
```
size[i] = (i == num_stripes - 1)
    ? (file_size - offsets[i])
    : (offsets[i + 1] - offsets[i])
```

### 2.4 Compressed Stripe Format

Each stripe is an independent DS-LZ compressed stream without the 7-byte DS-LZ header (metadata is in the STLZ header instead).

The stripe data consists of:
1. **Delta-XOR encoded pixel data** — preprocessed with stride = `width × bpp`
2. **LZ77 compressed stream** — using the same token format as DS-LZ (see Section 3)

**Stripe preprocessing before compression:**
1. Copy `actual_h` rows from source image into stripe buffer
2. Pad remaining rows (if `actual_h < stripe_h`) with zeros
3. Apply `stlz_delta_encode(stripe_buf, stripe_bytes, width × bpp)`
4. Compress with LZ77

## 3. LZ77 Bitstream Format (Per Stripe)

STLZ uses the same byte-oriented variable-length encoding as DS-LZ. All multi-byte values are little-endian.

### 3.1 Command Tokens

**Literals (0–127)**  
`0xxxxxxx`  
- Range: `0x00`–`0x7F` (0–127)  
- Length: `cmd + 1` (1–128 bytes)  
- Payload: `length` raw bytes  

**Near Match (128–191)**  
`10xxxxxx [offset8]`  
- Range: `0x80`–`0xBF` (128–191)  
- Length: `(cmd & 0x3F) + 3` (3–66 bytes)  
- Offset: 1 byte (1–255)  
- Total: 2 bytes  

**Far Match Short (192–223)**  
`110xxxxx [offset16]`  
- Range: `0xC0`–`0xDF` (192–223)  
- Length: `(cmd & 0x1F) + 4` (4–35 bytes)  
- Offset: 2 bytes LE (1–65535)  
- Total: 3 bytes  

**Rep Match Short (224–239)**  
`1110xxxx`  
- Range: `0xE0`–`0xEF` (224–239)  
- Length: `(cmd & 0x0F) + 3` (3–18 bytes)  
- Offset: `last_offset` (implicit)  
- Total: 1 byte  

**Long Literals (240–247)**  
`11110xxx [data...]`  
- Range: `0xF0`–`0xF7` (240–247)  
- Length: `(cmd & 0x07) + 129` (129–136 bytes)  
- Total: 1 + `length` bytes  

**Escape (248)**  
`11111000 [sub] [payload...]`  
- Value: `0xF8` (248)  

| Sub | Name | Payload | Length | Total Size |
|:---:|:-----|:--------|:-------|:-----------|
| 0 | Long literals | `len16` + data | `len16 + 137` | 4 + length |
| 1 | Long far match | `len16` + `off16` | `len16` (1–65535) | 6 bytes |
| 2 | Long rep match | `len16` | `len16` (1–65535) | 4 bytes |
| 3 | Long near match | `len16` + `off8` | `len16` (1–65535) | 5 bytes |

*Reserved sub-commands (4–255) must cause decoder abort.*

### 3.2 End of Stripe

Each stripe stream ends when exactly `stripe_bytes` bytes have been decompressed. No explicit end marker.

## 4. Delta-XOR Preprocessing

### 4.1 Algorithm

Applied per stripe before LZ77 compression, reversed after decompression.

**Stride value:** `stride = width × bpp` (full row width in bytes)

**Encoding:**
- Pass 1 (vertical): For `i = size-1` down to `stride`: `data[i] ^= data[i - stride]`
- Pass 2 (horizontal): For `i = size-1` down to `1`: If `i % stride != 0`: `data[i] ^= data[i - 1]`

**Decoding:**
- Pass 1 (horizontal): For `i = 1` to `size-1`: If `i % stride != 0`: `data[i] ^= data[i - 1]`
- Pass 2 (vertical): For `i = stride` to `size-1`: `data[i] ^= data[i - stride]`

### 4.2 Effect

After delta encoding, identical rows within a stripe become sequences of zeros. Horizontal delta further reduces entropy for smooth gradients. The result is highly compressible with LZ77.

### 4.3 Optimization

When `stride % 4 == 0`, `size % 4 == 0`, and data is 4-byte aligned, the implementation uses 32-bit XOR operations for speed:

```c
*(uint32_t*)&data[i] ^= *(uint32_t*)&data[i - stride];
```

## 5. Compressor Architecture

### 5.1 Stripe Processing

```
for each stripe i:
    1. Extract rows [i×stripe_h, (i+1)×stripe_h) from source image
    2. Pad last stripe if needed
    3. Apply delta-XOR with stride = width × bpp
    4. Reset hash table (all entries = 0xFFFFFFFF)
    5. LZ77 compress with lazy matching
    6. Record file offset in offset table
    7. Write compressed data
```

### 5.2 Hash Table

- Size: `STLZ_HASH_SIZE` buckets (default 1024)
- Structure: 2-way associative, each bucket holds 2 × 32-bit positions
- Total RAM: `STLZ_HASH_SIZE × 8` bytes (8 KB default)
- Hash function: `h = (p[0] × 0x9E3779B1) ^ (p[1] × 0x85EBCA77) ^ p[2]; return (h ^ (h >> 12)) & (STLZ_HASH_SIZE - 1)`

### 5.3 Match Finding

Priority order (same as DS-LZ):
1. Hash-based search (2-way associative)
2. Last-offset rep-match
3. Cost-based lazy matching for matches < 16 bytes

### 5.4 Cost Model

| Match Type | Length Range | Cost (bytes) |
|:-----------|:-------------|:-------------|
| Rep short | 3–18 | 1 |
| Rep long | 19–65535 | 4 |
| Near short | 3–66, offset ≤ 255 | 2 |
| Near long | 67–65535, offset ≤ 255 | 5 |
| Far short | 4–35, offset > 255 | 3 |
| Far long | 36–65535, offset > 255 | 6 |

## 6. Decompressor Architecture

### 6.1 Stripe-Level Decompression

```c
stlz_decompress_stripe(dst, filename, idx):
    1. Read header, validate magic
    2. Read offset table
    3. Seek to offsets[idx]
    4. Read compressed data (size from offset table)
    5. LZ77 decompress to dst (stripe_bytes)
    6. Apply delta-XOR decode with stride = width × bpp
```

### 6.2 Full-Frame Decompression

```c
stlz_decompress_full(dst, filename):
    1. Get image dimensions from header
    2. Allocate stripe buffer (stripe_bytes)
    3. For each stripe:
        a. stlz_decompress_stripe(stripe_buf, file, i)
        b. Copy actual rows to dst at correct offset
    4. Free stripe buffer
```

### 6.3 Scanline Render (Zero Heap)

```c
stlz_render_scanlines(filename, dest_w, dest_h, swap, callback, userdata):
    1. Read entire file into memory (or mmap)
    2. Parse header, offset table
    3. Allocate stripe_buf (stripe_bytes) + line_buf (dest_w × 2)
    4. For each output scanline y = 0..dest_h-1:
        a. y_src = (y × src_h) / dest_h
        b. stripe_idx = y_src / stripe_h
        c. line_in = y_src % stripe_h
        d. If stripe_idx changed:
            - Decompress stripe to stripe_buf
            - Delta-XOR decode
        e. Scale source row to dest_w using nearest-neighbor
        f. Byte-swap if swap == true
        g. callback(y, line_buf, userdata)
    5. Free buffers
```

**Memory model:**
- Input: File data (can be streamed or memory-mapped)
- Output: Callback-driven (no full-frame buffer)
- Working: `stripe_buf` + `line_buf` + 3 stack variables

## 7. Performance Characteristics

| Metric | Value |
|:-------|:------|
| Maximum image dimensions | 65535 × 65535 pixels |
| Maximum stripes | 255 |
| Maximum stripe height | 255 pixels |
| Maximum match length | 65,535 bytes |
| Sliding window per stripe | 64 KB |
| Minimum match | 3 bytes (4 for far matches with offset > 255) |
| Hash table RAM (default) | 8 KB (1024 buckets) |
| Hash table RAM (max) | 32 KB (4096 buckets) |
| Encoder RAM | ~stripe_bytes + hash_table + I/O buffers |
| Decoder RAM (full) | ~stripe_bytes |
| Decoder RAM (scanline) | ~stripe_bytes + dest_w × 2 |
| Encoder speed (ESP32-S3) | ~3.5 MB/s |
| Decoder speed (ESP32-S3, full) | ~25 MB/s |
| Decoder speed (ESP32-S3, scanline) | ~15 MB/s |

## 8. Security Considerations

The decompressor trusts the input file. Malformed data may cause:
- Buffer overruns if `stripe_bytes` calculation overflows
- Invalid memory access if offset table points outside file
- Incorrect rendering if header dimensions are falsified

**Validation requirements:**
- Verify magic bytes before processing
- Check `num_stripes == ceil(height / stripe_h)`
- Ensure offset table entries are monotonically increasing
- Validate that compressed stripe sizes are positive
- Verify `format` is in valid range {1, 2, 3}
- Ensure offset + match length does not exceed output position (LZ77 safety)

## 9. Reference Implementation

- **Repository:** [github.com/Svarkovsky/stlz](https://github.com/Svarkovsky/stlz)
- **Files:** `ds-stlz.h` (header-only codec), `ds-stlz.c` (file I/O)
- **API:** See README.md for function reference

## 10. Relationship to DS-LZ

STLZ is a container format built on the DS-LZ compression engine:

| Component | Source |
|:----------|:-------|
| LZ77 compressor/decompressor | DS-LZ (adapted) |
| Delta-XOR algorithm | DS-LZ (identical) |
| Hash function | DS-LZ (identical) |
| Cost model | DS-LZ (identical) |
| File container | STLZ (new) |
| Pixel format abstraction | STLZ (new) |
| Stripe architecture | STLZ (new) |
| Scanline renderer | STLZ (new) |

## References

- DS-LZ Specification: [github.com/Svarkovsky/dslz/SPECIFICATION.md](https://github.com/Svarkovsky/dslz)
- Lempel, A., Ziv, J. (1977). "A Universal Algorithm for Sequential Data Compression." *IEEE Transactions on Information Theory*
- Storer, J., Szymanski, T. (1982). "Data Compression via Textual Substitution." *Journal of the ACM*
- W3C. (2003). PNG Specification (RFC 2083). [w3.org/TR/PNG](https://www.w3.org/TR/PNG/)
