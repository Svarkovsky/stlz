# STLZ Image Format Specification

**Author:** Ivan Svarkovsky  
**Date:** June 2026  
**License:** CC BY-NC-SA 4.0  
**Repository:** [github.com/Svarkovsky/stlz](https://github.com/Svarkovsky/stlz)  
**Engine:** [github.com/Svarkovsky/dslz](https://github.com/Svarkovsky/dslz)

## 1. Introduction

STLZ (Striped Delta-Stride LZ) is a domain-optimized image codec for memory-constrained embedded systems, particularly retro emulator frontends on ESP32-S3 with limited PSRAM (~656 KB).

The key innovation over DS-LZ and other LZ77 codecs is **frame sectioning into independent horizontal stripes** of fixed height. Each stripe is compressed in isolation, enabling postrоchny rendering without decompressing the entire frame to heap.

**Primary use case:** Save state screenshots for MSX emulator (fMSX/Retro-Go), where:
- Source resolution: 256×192 pixels
- Pixel format: RGB565 (2 bytes/pixel)
- Stripe height: 8 pixels
- Uncompressed stripe size: 256 × 8 × 2 = **4096 bytes (4 KB)**
- Peak render RAM: **~5 KB** (stripe buffer + line buffer)

### 1.1 Terminology

| Term | Definition |
|:-----|:-----------|
| **Stripe** | Horizontal pixel block of fixed height (typical: 8 or 16 lines) |
| **Offset table** | Array of 32-bit file offsets to each compressed stripe |
| **Delta-XOR Stride** | Two-pass differential encoding (XOR) with stride = row length in bytes |
| **Temporary buffer** | Small heap/stack array, immediately freed after operation |

## 2. File Structure

```
┌────────────────────────────────────────────────────────┐
│ Header STLZ (16 bytes)                                 │
├────────────────────────────────────────────────────────┤
│ Offset table (num_stripes × uint32_t)                │
├────────────────────────────────────────────────────────┤
│ Compressed stripe 0 (LZ77 stream)                      │
├────────────────────────────────────────────────────────┤
│ Compressed stripe 1 (LZ77 stream)                      │
├────────────────────────────────────────────────────────┤
│ ...                                                    │
├────────────────────────────────────────────────────────┤
│ Compressed stripe N-1 (LZ77 stream)                    │
└────────────────────────────────────────────────────────┘
```

### 2.1 Header (16 bytes)

Packed structure, no compiler padding:

| Offset | Size | Field | Description |
|:------:|:----:|:------|:------------|
| 0–3 | `char[4]` | `magic` | ASCII `"STLZ"` (0x53 0x54 0x4C 0x5A) |
| 4–5 | `uint16_t` | `width` | Frame width in pixels (Little-Endian) |
| 6–7 | `uint16_t` | `height` | Frame height in pixels (Little-Endian) |
| 8 | `uint8_t` | `stripe_h` | Stripe height in lines (typically 8) |
| 9 | `uint8_t` | `num_stripes` | Total number of stripes in file |
| 10 | `uint8_t` | `format` | Pixel format (1=RGB565_BE, 2=RGB565_LE, 3=RGB888) |
| 11–15 | `uint8_t[5]` | `reserved` | Reserved for future use (zero-filled) |

**Derived values:**
- `num_stripes = ceil(height / stripe_h)`
- `bpp = 2` if format ∈ {1, 2}, else `3` if format = 3
- `stripe_bytes = width × stripe_h × bpp`
- `last_stripe_h = height - (num_stripes - 1) × stripe_h`

### 2.2 Offset Table

Immediately follows header at byte offset 16. Contains `num_stripes` entries of 32-bit little-endian unsigned integers.

| Entry | Description |
|:------|:------------|
| `offsets[i]` | Absolute file offset to start of compressed stripe `i` |

**Compressed stripe size:**
```
size[i] = (i == num_stripes - 1)
    ? (file_size - offsets[i])
    : (offsets[i + 1] - offsets[i])
```

### 2.3 Compressed Stripe Format

Each stripe is an independent DS-LZ compressed stream **without the 7-byte DS-LZ header** (metadata is in the STLZ header).

Stripe preprocessing before compression:
1. Copy `actual_h` rows from source image into stripe buffer
2. Pad remaining rows (if `actual_h < stripe_h`) with zeros
3. Apply `stlz_delta_encode(stripe_buf, stripe_bytes, width × bpp)`
4. Compress with LZ77

## 3. Delta-XOR Preprocessing

### 3.1 Algorithm

Applied per stripe before LZ77 compression, reversed after decompression.

**Stride:** `stride = width × bpp` (full row width in bytes)

**Encoding:**
- **Vertical pass** (top to bottom, vectorized):  
  For `i = size - 4` down to `stride`, step 4:  
  `data[i] ^= data[i - stride]` (32-bit XOR when aligned)
- **Horizontal pass** (right to left):  
  For `i = size - 1` down to `1`:  
  If `i % stride != 0`: `data[i] ^= data[i - 1]`

**Decoding:**
- **Horizontal pass** (left to right):  
  For `i = 1` to `size - 1`:  
  If `i % stride != 0`: `data[i] ^= data[i - 1]`
- **Vertical pass** (bottom to top, vectorized):  
  For `i = stride` to `size - 1`, step 4:  
  `data[i] ^= data[i - stride]`

### 3.2 Effect

After delta encoding, identical rows within a stripe become zero sequences. Horizontal delta further reduces entropy for smooth gradients. The result is highly compressible with LZ77.

### 3.3 32-bit Optimization

When `stride % 4 == 0`, `size % 4 == 0`, and data is 4-byte aligned:
```c
*(uint32_t*)&data[i] ^= *(uint32_t*)&data[i - stride];
```

## 4. Compressor Architecture

### 4.1 Stripe Processing

```
for each stripe i:
    1. Extract rows [i×stripe_h, (i+1)×stripe_h) from source
    2. Pad last stripe if needed
    3. Delta-XOR encode with stride = width × bpp
    4. Reset hash table: memset(hash, 0xFF, STLZ_HASH_SIZE × 8)
    5. LZ77 compress with lazy matching
    6. Record file offset in offset table
    7. Write compressed data
```

### 4.2 Reduced Hash Table (STLZ_HASH_SIZE = 1024)

Unlike DS-LZ (4096 buckets, 32 KB), STLZ uses **1024 buckets (8 KB)**:
- Stripe size is only 4 KB — collisions are negligible with 1024 entries
- Structure: 2-way associative, each bucket holds 2 × 32-bit positions
- Reset per stripe isolates match history within stripe boundaries

**Hash function:**
```c
uint32_t h = (p[0] * 0x9E3779B1u) ^ (p[1] * 0x85EBCA77u) ^ p[2];
return (h ^ (h >> 12)) & (STLZ_HASH_SIZE - 1);  // 10 bits → 1024 buckets
```

### 4.3 Match Finding

Priority order (same as DS-LZ):
1. Hash-based search (2-way associative)
2. Last-offset rep-match
3. Cost-based lazy matching for matches < 16 bytes

### 4.4 Cost Model

| Match Type | Length | Cost (bytes) |
|:-----------|:-------|:-------------|
| Rep short | 3–18 | 1 |
| Rep long | 19–65535 | 4 |
| Near short | 3–66, offset ≤ 255 | 2 |
| Near long | 67–65535, offset ≤ 255 | 5 |
| Far short | 4–35, offset > 255 | 3 |
| Far long | 36–65535, offset > 255 | 6 |

### 4.5 Peak Compression RAM (256×192, stripe_h=8, RGB565)

| Buffer | Size | Purpose |
|:-------|:-----|:--------|
| `hash` | 8 KB | 1024 buckets × 8 bytes |
| `stripe_buf` | 4 KB | 256 × 8 × 2 bytes |
| `comp_buf` | ~4.2 KB | 4096 + 256 bytes |
| `offsets` | 96 bytes | 24 stripes × 4 bytes |
| **Total** | **~16.3 KB** | |

## 5. Decompressor Architecture

### 5.1 Stripe-Level Decompression

```c
stlz_decompress_stripe(dst, filename, idx):
    1. Read header, validate magic
    2. Read offset table
    3. Seek to offsets[idx]
    4. Read compressed data (size from offset table)
    5. LZ77 decompress to dst (stripe_bytes)
    6. Delta-XOR decode with stride = width × bpp
```

### 5.2 Full-Frame Decompression

```c
stlz_decompress_full(dst, filename):
    1. Get dimensions from header
    2. Allocate stripe buffer (stripe_bytes)
    3. For each stripe:
        a. stlz_decompress_stripe(stripe_buf, file, i)
        b. Copy actual rows to dst at correct offset
    4. Free stripe buffer
```

### 5.3 Scanline Render Pipeline

```c
stlz_render_scanlines(filename, dest_w, dest_h, swap, callback, userdata):
    1. Read entire file into memory (file_data)
    2. Parse header, offset table
    3. Allocate stripe_buf (stripe_bytes) + line_buf (dest_w × 2)
    4. For each output scanline y = 0..dest_h-1:
        a. y_src = (y × src_h) / dest_h
        b. stripe_idx = y_src / stripe_h
        c. line_in = y_src % stripe_h
        d. If stripe_idx changed:
            - Decompress stripe to stripe_buf
            - Delta-XOR decode
        e. Extract source row from stripe_buf
        f. NN-scale horizontally to dest_w
        g. Byte-swap if swap == true
        h. callback(y, line_buf, userdata)
    5. Free all buffers
```

**Memory model:**
- `file_data`: Entire compressed file (~15–30 KB for MSX screenshots)
- `stripe_buf`: One decompressed stripe (4 KB)
- `line_buffer`: One output scanline (<2 KB for 640-wide display)
- **Dynamic heap peak: ~5 KB** (stripe_buf + line_buffer)

## 6. LZ77 Bitstream Format (Per Stripe)

STLZ uses the same byte-oriented variable-length encoding as DS-LZ. All multi-byte values are little-endian.

### 6.1 Command Tokens

**Literals (0–127)**  
`0xxxxxxx`  
- Range: `0x00`–`0x7F`  
- Length: `cmd + 1` (1–128 bytes)  
- Payload: `length` raw bytes  

**Near Match (128–191)**  
`10xxxxxx [offset8]`  
- Range: `0x80`–`0xBF`  
- Length: `(cmd & 0x3F) + 3` (3–66 bytes)  
- Offset: 1 byte (1–255)  
- Total: 2 bytes  

**Far Match Short (192–223)**  
`110xxxxx [offset16]`  
- Range: `0xC0`–`0xDF`  
- Length: `(cmd & 0x1F) + 4` (4–35 bytes)  
- Offset: 2 bytes LE (1–65535)  
- Total: 3 bytes  

**Rep Match Short (224–239)**  
`1110xxxx`  
- Range: `0xE0`–`0xEF`  
- Length: `(cmd & 0x0F) + 3` (3–18 bytes)  
- Offset: `last_offset` (implicit)  
- Total: 1 byte  

**Long Literals (240–247)**  
`11110xxx [data...]`  
- Range: `0xF0`–`0xF7`  
- Length: `(cmd & 0x07) + 129` (129–136 bytes)  
- Total: 1 + `length` bytes  

**Escape (248)**  
`11111000 [sub] [payload...]`  
- Value: `0xF8`

| Sub | Name | Payload | Length | Total Size |
|:---:|:-----|:--------|:-------|:-----------|
| 0 | Long literals | `len16` + data | `len16 + 137` | 4 + length |
| 1 | Long far match | `len16` + `off16` | `len16` (1–65535) | 6 bytes |
| 2 | Long rep match | `len16` | `len16` (1–65535) | 4 bytes |
| 3 | Long near match | `len16` + `off8` | `len16` (1–65535) | 5 bytes |

*Reserved sub-commands (4–255) must cause decoder abort.*

### 6.2 End of Stripe

Each stripe stream ends when exactly `stripe_bytes` bytes have been decompressed. No explicit end marker.

## 7. Performance Characteristics

| Metric | Value |
|:-------|:------|
| Maximum image dimensions | 65535 × 65535 pixels |
| Maximum stripes | 255 |
| Maximum stripe height | 255 pixels |
| Maximum match length | 65,535 bytes |
| Sliding window per stripe | 64 KB |
| Minimum match | 3 bytes (4 for far matches with offset > 255) |
| Hash table RAM | 8 KB (1024 buckets) |
| Encoder RAM (256×192, stripe=8) | ~16 KB |
| Decoder RAM (scanline) | ~5 KB dynamic + file_data |
| Encoder speed (ESP32-S3) | ~3.5 MB/s |
| Decoder speed (ESP32-S3, scanline) | ~25 MB/s |

## 8. Security Considerations

The decompressor trusts the input file. Malformed data may cause:
- Buffer overruns if `stripe_bytes` calculation overflows
- Invalid memory access if offset table points outside file
- Incorrect rendering if header dimensions are falsified

**Validation requirements:**
- Verify magic bytes (`"STLZ"`) before processing
- Check `num_stripes == ceil(height / stripe_h)`
- Ensure offset table entries are monotonically increasing
- Validate that compressed stripe sizes are positive
- Verify `format` is in valid range {1, 2, 3}
- Ensure offset + match length does not exceed output position (LZ77 safety)

## 9. Comparison with Alternatives (256×192 RGB565)

| Algorithm | Encode RAM | Decode RAM | Avg File Size | Decode Speed | Scene Dependency |
|:----------|:-----------|:-----------|:--------------|:-------------|:-----------------|
| **STLZ** | **~16 KB** | **~5 KB** | **12–25 KB** | **~25 MB/s** | Low |
| QOI565 | ~0.5 KB | ~0.5 KB | 22–40 KB | ~35 MB/s | High |
| LZ4 | ~16–64 KB | 0 KB | 20–38 KB | ~45 MB/s | Medium |
| RLE | 0 KB | 0 KB | 35–90 KB | ~55 MB/s | Critical |

**STLZ advantages:**
- **1.5–3× smaller files** than QOI565/RLE on real game screens (Delta-XOR exploits tile repetition)
- **120× less decode RAM** than full-frame PNG decoder (~5 KB vs ~630 KB)
- **Random access** to any stripe — impossible with monolithic streams

## 10. Relationship to DS-LZ

STLZ is a container format built on the DS-LZ compression engine:

| Component | Source |
|:----------|:-------|
| LZ77 compressor/decompressor | DS-LZ (adapted, no 7-byte header per stripe) |
| Delta-XOR algorithm | DS-LZ (identical) |
| Hash function | DS-LZ (identical) |
| Cost model | DS-LZ (identical) |
| File container | STLZ (new: header + offset table + stripes) |
| Hash table size | STLZ (reduced: 1024 vs 4096) |
| Pixel format abstraction | STLZ (new) |
| Scanline renderer | STLZ (new) |

## References

- DS-LZ Specification: [github.com/Svarkovsky/dslz](https://github.com/Svarkovsky/dslz)
- Lempel, A., Ziv, J. (1977). "A Universal Algorithm for Sequential Data Compression." *IEEE Transactions on Information Theory*
- Storer, J., Szymanski, T. (1982). "Data Compression via Textual Substitution." *Journal of the ACM*
