/*
 * Copyright(c) 2019 Intel Corporation
 * Copyright(c) 2026 Alliance for Open Media. All rights reserved.
 * Copyright (c) 2026 Halide Compression, LLC
 *
 * This source code is subject to the terms of the BSD 3-Clause Clear License
 * and the Alliance for Open Media Patent License 1.0. See LICENSE.md and
 * PATENTS.md for details.
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "app_film_grain.h"
#include "../Lib/Codec/grainSynthesis.h"

static bool read_token(FILE* file, char token[32]) {
    if (fscanf(file, " %31s", token) != 1) {
        return false;
    }
    const int next = fgetc(file);
    return (next == EOF || isspace((unsigned char)next)) && !ferror(file);
}

static bool read_label(FILE* file, const char* label) {
    char token[32];
    return read_token(file, token) && !strcmp(token, label);
}

static bool read_integer(FILE* file, int32_t* value) {
    char token[32], *end;
    if (!read_token(file, token)) {
        return false;
    }
    errno                  = 0;
    const long long parsed = strtoll(token, &end, 10);
    if (errno == ERANGE || end == token || *end || parsed < INT32_MIN || parsed > INT32_MAX) {
        return false;
    }
    *value = (int32_t)parsed;
    return true;
}

static bool read_time(FILE* file, uint64_t* time) {
    char token[32], *end;
    if (!read_token(file, token) || !isdigit((unsigned char)token[0])) {
        return false;
    }
    errno = 0;
    *time = strtoull(token, &end, 10);
    return errno != ERANGE && *end == '\0';
}

static bool read_points(FILE* file, const char* label, int32_t* count, int max, int32_t points[][2]) {
    if (!read_label(file, label) || !read_integer(file, count) || *count < 0 || *count > max) {
        return false;
    }
    for (int i = 0; i < *count; ++i) {
        if (!read_integer(file, &points[i][0]) || !read_integer(file, &points[i][1])) {
            return false;
        }
    }
    return true;
}

static bool read_coeffs(FILE* file, const char* label, int count, int32_t* coeffs) {
    if (!read_label(file, label)) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (!read_integer(file, &coeffs[i])) {
            return false;
        }
    }
    return true;
}

static bool read_model(FILE* file, AomFilmGrain* grain) {
    if (!read_label(file, "p")) {
        return false;
    }
    int32_t* fields[] = {&grain->ar_coeff_lag,
                         &grain->ar_coeff_shift,
                         &grain->grain_scale_shift,
                         &grain->scaling_shift,
                         &grain->chroma_scaling_from_luma,
                         &grain->overlap_flag,
                         &grain->cb_mult,
                         &grain->cb_luma_mult,
                         &grain->cb_offset,
                         &grain->cr_mult,
                         &grain->cr_luma_mult,
                         &grain->cr_offset};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (!read_integer(file, fields[i])) {
            return false;
        }
    }
    if ((uint32_t)grain->ar_coeff_lag > 3) {
        return false;
    }
    const int n = 2 * grain->ar_coeff_lag * (grain->ar_coeff_lag + 1);
    return read_points(file, "sY", &grain->num_y_points, 14, grain->scaling_points_y) &&
        read_points(file, "sCb", &grain->num_cb_points, 10, grain->scaling_points_cb) &&
        read_points(file, "sCr", &grain->num_cr_points, 10, grain->scaling_points_cr) &&
        read_coeffs(file, "cY", n, grain->ar_coeffs_y) && read_coeffs(file, "cCb", n + 1, grain->ar_coeffs_cb) &&
        read_coeffs(file, "cCr", n + 1, grain->ar_coeffs_cr);
}

void free_film_grain_table(FilmGrainTable* table) {
    while (table->head) {
        FilmGrainEntry* next = table->head->next;
        free(table->head);
        table->head = next;
    }
    table->current = NULL;
    table->seed    = 0;
}

EbErrorType read_film_grain_table(FILE* file, FilmGrainTable* table) {
    EbErrorType     status     = EB_ErrorBadParameter;
    FilmGrainEntry* tail       = NULL;
    bool            have_model = false;
    char            magic[9];
    if (fread(magic, 1, sizeof(magic), file) != sizeof(magic) || memcmp(magic, "filmgrn1", 8) ||
        !isspace((unsigned char)magic[8])) {
        return status;
    }
    for (;;) {
        FilmGrainEntry entry = {0};
        if (tail) {
            entry.grain = tail->grain;
        }
        char    label[32];
        int32_t seed;
        if (!read_token(file, label)) {
            if (feof(file) && !ferror(file)) {
                break;
            }
            goto fail;
        }
        if (strcmp(label, "E") || !read_time(file, &entry.start_time) || !read_time(file, &entry.end_time) ||
            !read_integer(file, &entry.grain.apply_grain) || !read_integer(file, &seed) ||
            !read_integer(file, &entry.grain.update_parameters) || entry.start_time >= entry.end_time ||
            (tail && entry.start_time < tail->end_time) || seed < 0 || seed > UINT16_MAX ||
            (uint32_t)entry.grain.apply_grain > 1 || (uint32_t)entry.grain.update_parameters > 1 ||
            (entry.grain.apply_grain && !entry.grain.update_parameters && !have_model)) {
            goto fail;
        }
        if (entry.grain.update_parameters) {
            if (!read_model(file, &entry.grain)) {
                goto fail;
            }
            have_model = true;
        }
        entry.grain.random_seed = (uint16_t)seed;
        entry.grain.ignore_ref  = 1;
        entry.grain.bit_depth   = 8;
        if (!svt_aom_film_grain_params_valid(&entry.grain)) {
            goto fail;
        }
        FilmGrainEntry* next = malloc(sizeof(*next));
        if (!next) {
            status = EB_ErrorInsufficientResources;
            goto fail;
        }
        *next = entry;
        if (tail) {
            tail->next = next;
        } else {
            table->head = next;
        }
        tail = next;
    }
    if (!table->head) {
        goto fail;
    }
    table->current = table->head;
    table->seed    = table->head->grain.random_seed;
    return EB_ErrorNone;
fail:
    free_film_grain_table(table);
    return status;
}

void lookup_film_grain_table(FilmGrainTable* table, uint64_t frame, const EbSvtAv1EncConfiguration* config,
                             AomFilmGrain* grain) {
    const long double time = (long double)frame * 10000000 * config->frame_rate_denominator /
        config->frame_rate_numerator;
    while (table->current && time >= table->current->end_time) {
        table->current = table->current->next;
        if (table->current) {
            table->seed = table->current->grain.random_seed;
        }
    }
    memset(grain, 0, sizeof(*grain));
    if (table->current && time >= table->current->start_time) {
        *grain             = table->current->grain;
        grain->random_seed = table->seed;
        table->seed += 3381;
        if (!table->seed) {
            table->seed += 7391;
        }
    }
    grain->bit_depth = config->encoder_bit_depth;
}

/*
 * Designed "dither" film grain (--dither-grain): a white, signal-independent grain model whose
 * synthesized noise has the variance of a triangular (TPDF) dither of the given peak amplitude, in
 * 8-bit code steps. Encode a smooth, undithered 10-bit source and every conforming decoder adds the
 * dither at output, so 8-bit display paths do not band.
 *
 * Calibration (AV1 spec 7.18.3, bit depth B, with ar_coeff_lag = 0 so the grain is white):
 *   grain   = Round2(Gaussian_Sequence[r], 12 - B + grain_scale_shift)
 *             std(Gaussian_Sequence) = 511.53 (the 2048-entry table; 12-bit values)
 *             -> std(grain) = 511.53 * 2^(B - 12 - grain_scale_shift)
 *   noise   = Round2(ScalingLut(x) * grain, ScalingShift), ScalingLut flat = s for all x
 *             -> std(noise) = s * 511.53 * 2^(B - 12 - grain_scale_shift - ScalingShift)    [B-bit codes]
 *   target  = std of TPDF with peak A display steps = A / sqrt(6) 8-bit steps,
 *             one 8-bit step of display RGB is L * 2^(B - 8) codes, L = 219/255 (luma, limited range),
 *             224/255 (chroma, limited range) or 1 (full range)
 *   =>  s = (A / sqrt(6)) * L * 2^(4 + grain_scale_shift + ScalingShift) / 511.53   (independent of B)
 *   e.g. A = 1, limited range, grain_scale_shift = 0, ScalingShift = 11: s = 22.46 -> 22.
 * Luma noise moves R, G and B equally, so luma-only grain dithers all three display channels.
 */
#define DITHER_GAUSS_STD 511.53

static int dither_scaling(double amplitude, double steps_per_lsb, int32_t* scaling_shift) {
    for (int shift = 11; shift >= 8; --shift) {
        const double s = amplitude / 2.449489742783178 * steps_per_lsb * (double)(1 << (4 + shift)) / DITHER_GAUSS_STD;
        if (s <= 255.0) {
            *scaling_shift = shift;
            return (int)(s + 0.5);
        }
    }
    *scaling_shift = 8;
    return 255;
}

EbErrorType make_dither_grain_table(FilmGrainTable* table, double luma_amplitude, double chroma_amplitude,
                                    bool full_range) {
    FilmGrainEntry* e = calloc(1, sizeof(*e));
    if (!e) {
        return EB_ErrorInsufficientResources;
    }
    AomFilmGrain* g      = &e->grain;
    g->apply_grain       = 1;
    g->update_parameters = 1;
    // Identical parameters on every frame: the encoder may then signal update_grain = 0 and load them
    // from a reference frame; the random seed is still sent per frame and advanced in
    // lookup_film_grain_table(), so the noise changes every frame (temporal dither).
    g->ignore_ref        = 0;
    g->random_seed       = 7391;
    g->bit_depth         = 8;
    g->ar_coeff_lag      = 0; // no auto-regression: white grain
    g->ar_coeff_shift    = 6;
    g->grain_scale_shift = 0;
    g->overlap_flag      = 0; // white noise has no block seams to hide
    g->clip_to_restricted_range = 0;
    g->chroma_scaling_from_luma = 0;

    int32_t luma_shift = 11, chroma_shift = 11;
    const int s_y = luma_amplitude > 0 ? dither_scaling(luma_amplitude, full_range ? 1.0 : 219.0 / 255.0, &luma_shift)
                                       : 0;
    const int s_c = chroma_amplitude > 0
        ? dither_scaling(chroma_amplitude, full_range ? 1.0 : 224.0 / 255.0, &chroma_shift)
        : 0;
    // One ScalingShift serves all planes: use the smaller one and rescale the other plane.
    g->scaling_shift = luma_shift < chroma_shift ? luma_shift : chroma_shift;
    const int y = s_y ? (luma_shift > g->scaling_shift ? (s_y + 1) >> (luma_shift - g->scaling_shift) : s_y) : 0;
    const int c = s_c ? (chroma_shift > g->scaling_shift ? (s_c + 1) >> (chroma_shift - g->scaling_shift) : s_c) : 0;
    if (!y && !c) {
        free(e);
        return EB_ErrorBadParameter;
    }
    // A flat scaling function needs two points; num_y_points = 0 would also disable 4:2:0 chroma grain,
    // so keep a (zero) luma function when only chroma grain is requested.
    g->num_y_points          = 2;
    g->scaling_points_y[0][0] = 0;
    g->scaling_points_y[0][1] = y;
    g->scaling_points_y[1][0] = 255;
    g->scaling_points_y[1][1] = y;
    if (c) {
        g->num_cb_points = g->num_cr_points = 2;
        g->scaling_points_cb[0][0] = g->scaling_points_cr[0][0] = 0;
        g->scaling_points_cb[1][0] = g->scaling_points_cr[1][0] = 255;
        g->scaling_points_cb[0][1] = g->scaling_points_cb[1][1] = c;
        g->scaling_points_cr[0][1] = g->scaling_points_cr[1][1] = c;
        // index = cb * mult/64 + luma * (luma_mult - 128)/64 + (offset - 256) * 4: identity (irrelevant
        // for a flat function, but keep it well-formed)
        g->cb_mult = g->cr_mult = 128;
        g->cb_luma_mult = g->cr_luma_mult = 192;
        g->cb_offset = g->cr_offset = 256;
    }
    if (!svt_aom_film_grain_params_valid(g)) {
        free(e);
        return EB_ErrorBadParameter;
    }
    fprintf(stderr,
            "SVT [dither-grain]: luma %.3g -> scaling %d, chroma %.3g -> scaling %d, scaling_shift %d, "
            "grain_scale_shift 0, ar_coeff_lag 0 (white)\n",
            luma_amplitude,
            y,
            chroma_amplitude,
            c,
            g->scaling_shift);
    e->start_time  = 0;
    e->end_time    = UINT64_MAX;
    table->head    = e;
    table->current = e;
    table->seed    = g->random_seed;
    return EB_ErrorNone;
}
