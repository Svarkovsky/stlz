/**
 * ds-stlz.c — STLZ Portable Codec Implementation
 * 
 * Implements the public file-I/O convenience functions declared in ds-stlz.h.
 * Zero dependencies beyond the C standard library and ds-stlz.h.
 * All compression/decompression primitives are static inline in the header.
 * 
 * Copyright (C) 2026 Ivan Svarkovsky <ivansvarkovsky@gmail.com>
 * Licensed under CC BY-NC-SA 4.0
 * Full license: https://creativecommons.org/licenses/by-nc-sa/4.0/
 * 
 * Repository: https://github.com/Svarkovsky
 */

/*
 * THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#include "ds-stlz.h"

/* ========================================================================= */
/* File Inspection                                                           */
/* ========================================================================= */

bool is_stlz_file(const char *filepath) {
    if (!filepath || !filepath[0]) return false;
    
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;
    
    char magic[4];
    size_t r = fread(magic, 1, 4, f);
    fclose(f);
    
    return (r == 4 && memcmp(magic, STLZ_MAGIC, 4) == 0);
}

bool stlz_get_dimensions(const char *filepath, int *width, int *height) {
    uint16_t w = 0, h = 0;
    if (!stlz_get_info(filepath, &w, &h, NULL, NULL))
        return false;
    if (width)  *width  = w;
    if (height) *height = h;
    return true;
}

bool stlz_get_info(const char *filename,
                   uint16_t *width, uint16_t *height,
                   uint8_t *format, uint8_t *stripe_h) {
    FILE *f = fopen(filename, "rb");
    if (!f) return false;
    
    stlz_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return false;
    }
    fclose(f);
    
    if (memcmp(hdr.magic, STLZ_MAGIC, 4) != 0) return false;
    
    if (width)    *width    = hdr.width;
    if (height)   *height   = hdr.height;
    if (format)   *format   = hdr.format;
    if (stripe_h) *stripe_h = hdr.stripe_h;
    return true;
}

/* ========================================================================= */
/* Compression                                                               */
/* ========================================================================= */

bool stlz_compress_file(const void *pixels,
                        uint16_t w, uint16_t h,
                        uint8_t fmt, uint8_t stripe_h,
                        const char *filename) {
    FILE *out = fopen(filename, "wb");
    if (!out) return false;

    int num_stripes = (h + stripe_h - 1) / stripe_h;
    uint8_t bpp = stlz_pixel_size(fmt);
    uint32_t stripe_bytes = w * stripe_h * bpp;

    /* Write header */
    stlz_header_t hdr;
    memcpy(hdr.magic, STLZ_MAGIC, 4);
    hdr.width    = w;
    hdr.height   = h;
    hdr.stripe_h = stripe_h;
    hdr.num_stripes = (uint8_t)num_stripes;
    hdr.format   = fmt;
    memset(hdr.reserved, 0, sizeof(hdr.reserved));

    if (fwrite(&hdr, 1, sizeof(hdr), out) != sizeof(hdr)) {
        fclose(out); return false;
    }

    /* Reserve offset table */
    uint32_t *offsets = (uint32_t *)calloc(num_stripes, sizeof(uint32_t));
    if (!offsets) { fclose(out); return false; }
    if (fwrite(offsets, sizeof(uint32_t), num_stripes, out) != (size_t)num_stripes) {
        free(offsets); fclose(out); return false;
    }

    /* Allocate compression buffers */
    stlz_hash_t *hash = (stlz_hash_t *)malloc(STLZ_HASH_SIZE * sizeof(stlz_hash_t));
    uint8_t *stripe_buf = (uint8_t *)malloc(stripe_bytes);
    uint8_t *comp_buf   = (uint8_t *)malloc(stripe_bytes + 256);

    if (!hash || !stripe_buf || !comp_buf) {
        free(hash); free(stripe_buf); free(comp_buf); free(offsets);
        fclose(out); return false;
    }

    const uint8_t *src = (const uint8_t *)pixels;

    for (int i = 0; i < num_stripes; i++) {
        int y_start = i * stripe_h;
        int y_end   = y_start + stripe_h;
        if (y_end > h) y_end = h;
        int actual_h = y_end - y_start;

        /* Copy stripe rows into contiguous buffer */
        uint8_t *dst = stripe_buf;
        for (int y = y_start; y < y_end; y++) {
            memcpy(dst, src + (size_t)y * w * bpp, w * bpp);
            dst += w * bpp;
        }
        if (actual_h < stripe_h)
            memset(dst, 0, w * bpp * (stripe_h - actual_h));

        /* Delta-XOR → LZ77 compress */
        stlz_delta_encode(stripe_buf, stripe_bytes, w * bpp);
        memset(hash, 0xFF, STLZ_HASH_SIZE * sizeof(stlz_hash_t));

        stlz_mem_writer_t wr = { .buf = comp_buf, .pos = 0,
                                 .max_size = stripe_bytes + 256 };
        stlz_write_lz77(stripe_buf, stripe_bytes, &wr, hash);

        offsets[i] = (uint32_t)ftell(out);
        if (fwrite(comp_buf, 1, wr.pos, out) != wr.pos) {
            free(hash); free(stripe_buf); free(comp_buf); free(offsets);
            fclose(out); return false;
        }
    }

    /* Finalize offset table */
    fseek(out, sizeof(stlz_header_t), SEEK_SET);
    fwrite(offsets, sizeof(uint32_t), num_stripes, out);

    free(stripe_buf); free(comp_buf); free(hash); free(offsets);
    fclose(out);
    return true;
}

/* ========================================================================= */
/* Decompression (stripe-level)                                              */
/* ========================================================================= */

bool stlz_decompress_stripe(void *dst, const char *filename, uint8_t idx) {
    FILE *f = fopen(filename, "rb");
    if (!f) return false;

    /* Read header */
    stlz_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return false; }
    if (memcmp(hdr.magic, STLZ_MAGIC, 4) != 0 || idx >= hdr.num_stripes) {
        fclose(f); return false;
    }

    /* Read offset table */
    uint32_t *offsets = (uint32_t *)malloc(hdr.num_stripes * sizeof(uint32_t));
    if (!offsets) { fclose(f); return false; }
    if (fread(offsets, sizeof(uint32_t), hdr.num_stripes, f) != (size_t)hdr.num_stripes) {
        free(offsets); fclose(f); return false;
    }

    /* Locate compressed stripe */
    uint32_t offset = offsets[idx];
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    uint32_t comp_size = (idx == hdr.num_stripes - 1)
        ? (uint32_t)(file_size - offset)
        : (offsets[idx + 1] - offset);

    fseek(f, offset, SEEK_SET);
    uint8_t *comp_buf = (uint8_t *)malloc(comp_size);
    if (!comp_buf) { free(offsets); fclose(f); return false; }
    if (fread(comp_buf, 1, comp_size, f) != comp_size) {
        free(comp_buf); free(offsets); fclose(f); return false;
    }
    fclose(f);

    /* Decompress */
    uint8_t bpp = stlz_pixel_size(hdr.format);
    uint32_t stripe_bytes = hdr.width * hdr.stripe_h * bpp;

    stlz_mem_reader_t r = { .buf = comp_buf, .pos = 0, .size = comp_size };
    int ok = stlz_read_lz77((unsigned char *)dst, stripe_bytes, &r);
    if (ok) stlz_delta_decode((uint8_t *)dst, stripe_bytes, hdr.width * bpp);

    free(comp_buf);
    free(offsets);
    return ok;
}

/* ========================================================================= */
/* Decompression (full-frame convenience)                                    */
/* ========================================================================= */

bool stlz_decompress_full(void *dst, const char *filename) {
    uint16_t w = 0, h = 0;
    uint8_t fmt = 0, stripe_h = 0;
    if (!stlz_get_info(filename, &w, &h, &fmt, &stripe_h))
        return false;

    uint8_t bpp = stlz_pixel_size(fmt);
    uint32_t stripe_bytes = w * stripe_h * bpp;
    uint8_t *stripe_buf = (uint8_t *)malloc(stripe_bytes);
    if (!stripe_buf) return false;

    uint8_t *dst_bytes = (uint8_t *)dst;

    for (int i = 0; i < ((h + stripe_h - 1) / stripe_h); i++) {
        if (!stlz_decompress_stripe(stripe_buf, filename, (uint8_t)i)) {
            free(stripe_buf); return false;
        }

        int y_start = i * stripe_h;
        int y_end   = y_start + stripe_h;
        if (y_end > h) y_end = h;

        const uint8_t *src = stripe_buf;
        for (int y = y_start; y < y_end; y++) {
            memcpy(dst_bytes + (size_t)y * w * bpp, src, w * bpp);
            src += w * bpp;
        }
    }

    free(stripe_buf);
    return true;
}

/* ========================================================================= */
/* On-the-fly scanline rendering                                             */
/* ========================================================================= */

bool stlz_render_scanlines(const char *filename,
                           int dest_w, int dest_h, bool swap_bytes,
                           void (*callback)(int y, const uint16_t *line,
                                            void *userdata),
                           void *userdata) {
    FILE *f = fopen(filename, "rb");
    if (!f) return false;

    /* Read entire compressed file into memory */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *file_data = (uint8_t *)malloc(file_size);
    if (!file_data) { fclose(f); return false; }
    if (fread(file_data, 1, file_size, f) != (size_t)file_size) {
        free(file_data); fclose(f); return false;
    }
    fclose(f);

    stlz_header_t *hdr = (stlz_header_t *)file_data;
    if (memcmp(hdr->magic, STLZ_MAGIC, 4) != 0) {
        free(file_data); return false;
    }

    uint32_t *offsets = (uint32_t *)(file_data + sizeof(stlz_header_t));
    uint8_t bpp = stlz_pixel_size(hdr->format);
    uint32_t stripe_bytes = hdr->width * hdr->stripe_h * bpp;

    uint8_t  *stripe_buf  = (uint8_t *)malloc(stripe_bytes);
    uint16_t *line_buffer = (uint16_t *)malloc(dest_w * sizeof(uint16_t));

    if (!stripe_buf || !line_buffer) {
        free(stripe_buf); free(line_buffer); free(file_data); return false;
    }

    int current_stripe = -1;

    for (int y_dest = 0; y_dest < dest_h; y_dest++) {
        int y_src = (y_dest * hdr->height) / dest_h;
        if (y_src >= hdr->height) y_src = hdr->height - 1;

        int stripe_idx = y_src / hdr->stripe_h;
        int line_in    = y_src % hdr->stripe_h;

        if (stripe_idx != current_stripe) {
            uint32_t off = offsets[stripe_idx];
            uint32_t comp_size = (stripe_idx == hdr->num_stripes - 1)
                ? (uint32_t)(file_size - off)
                : (offsets[stripe_idx + 1] - off);

            stlz_mem_reader_t r = { .buf  = file_data + off,
                                    .pos  = 0,
                                    .size = comp_size };
            if (!stlz_read_lz77(stripe_buf, stripe_bytes, &r)) {
                free(line_buffer); free(stripe_buf); free(file_data);
                return false;
            }
            stlz_delta_decode(stripe_buf, stripe_bytes, hdr->width * bpp);
            current_stripe = stripe_idx;
        }

        const uint16_t *src_line = (const uint16_t *)(stripe_buf +
                                       (size_t)line_in * hdr->width * bpp);

        /* Nearest-neighbor horizontal scaling */
        for (int x = 0; x < dest_w; x++) {
            int x_src = (x * hdr->width) / dest_w;
            if (x_src >= hdr->width) x_src = hdr->width - 1;
            uint16_t pixel = src_line[x_src];
            if (swap_bytes) pixel = (pixel >> 8) | (pixel << 8);
            line_buffer[x] = pixel;
        }

        if (callback) callback(y_dest, line_buffer, userdata);
    }

    free(line_buffer);
    free(stripe_buf);
    free(file_data);
    return true;
}
