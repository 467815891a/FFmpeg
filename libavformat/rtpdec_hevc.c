/*
 * RTP parser for HEVC/H.265 payload format (draft version 6)
 * Copyright (c) 2014 Thomas Volkert <thomas@homer-conferencing.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"
#include "rtpdec.h"
#include "rtpdec_formats.h"

#define RTP_HEVC_PAYLOAD_HEADER_SIZE       2
#define RTP_HEVC_FU_HEADER_SIZE            1
#define RTP_HEVC_DONL_FIELD_SIZE           2
#define RTP_HEVC_DOND_FIELD_SIZE           1
#define RTP_HEVC_AP_NALU_LENGTH_FIELD_SIZE 2
#define HEVC_SPECIFIED_NAL_UNIT_TYPES      48

/* SDP out-of-band signaling data */
struct PayloadContext {
    int using_donl_field;
    int profile_id;
    uint8_t *sps, *pps, *vps, *sei;
    int sps_size, pps_size, vps_size, sei_size;
    /* In-band VPS/SPS/PPS cache (annex-b, incl. start code) + keyframe
     * tracking. WebRTC/WHEP senders often transmit parameter sets only
     * once per stream (or after the first IDR); we cache them here and
     * re-inject before every IDR so the decoder never loses them, and
     * populate codecpar resolution from the SPS so find_stream_info()
     * finishes without decoding (avoids demux/main thread race on ta). */
    int got_vps, got_sps, got_pps, got_idr, need_pli;
    uint8_t *vps_data, *sps_data, *pps_data;
    int vps_data_size, sps_data_size, pps_data_size;
    int extradata_synced;
};

static const uint8_t start_sequence[] = { 0x00, 0x00, 0x00, 0x01 };

static av_cold int hevc_sdp_parse_fmtp_config(AVFormatContext *s,
                                              AVStream *stream,
                                              PayloadContext *hevc_data,
                                              const char *attr, const char *value)
{
    /* profile-space: 0-3 */
    /* profile-id: 0-31 */
    if (!strcmp(attr, "profile-id")) {
        hevc_data->profile_id = atoi(value);
        av_log(s, AV_LOG_TRACE, "SDP: found profile-id: %d\n", hevc_data->profile_id);
    }

    /* tier-flag: 0-1 */
    /* level-id: 0-255 */
    /* interop-constraints: [base16] */
    /* profile-compatibility-indicator: [base16] */
    /* sprop-sub-layer-id: 0-6, defines highest possible value for TID, default: 6 */
    /* recv-sub-layer-id: 0-6 */
    /* max-recv-level-id: 0-255 */
    /* tx-mode: MSM,SSM */
    /* sprop-vps: [base64] */
    /* sprop-sps: [base64] */
    /* sprop-pps: [base64] */
    /* sprop-sei: [base64] */
    if (!strcmp(attr, "sprop-vps") || !strcmp(attr, "sprop-sps") ||
        !strcmp(attr, "sprop-pps") || !strcmp(attr, "sprop-sei")) {
        uint8_t **data_ptr = NULL;
        int *size_ptr = NULL;
        if (!strcmp(attr, "sprop-vps")) {
            data_ptr = &hevc_data->vps;
            size_ptr = &hevc_data->vps_size;
        } else if (!strcmp(attr, "sprop-sps")) {
            data_ptr = &hevc_data->sps;
            size_ptr = &hevc_data->sps_size;
        } else if (!strcmp(attr, "sprop-pps")) {
            data_ptr = &hevc_data->pps;
            size_ptr = &hevc_data->pps_size;
        } else if (!strcmp(attr, "sprop-sei")) {
            data_ptr = &hevc_data->sei;
            size_ptr = &hevc_data->sei_size;
        } else
            av_assert0(0);

        ff_h264_parse_sprop_parameter_sets(s, data_ptr,
                                           size_ptr, value);
    }

    /* max-lsr, max-lps, max-cpb, max-dpb, max-br, max-tr, max-tc */
    /* max-fps */

    /* sprop-max-don-diff: 0-32767

         When the RTP stream depends on one or more other RTP
         streams (in this case tx-mode MUST be equal to "MSM" and
         MSM is in use), this parameter MUST be present and the
         value MUST be greater than 0.
    */
    if (!strcmp(attr, "sprop-max-don-diff")) {
        if (atoi(value) > 0)
            hevc_data->using_donl_field = 1;
        av_log(s, AV_LOG_TRACE, "Found sprop-max-don-diff in SDP, DON field usage is: %d\n",
                hevc_data->using_donl_field);
    }

    /* sprop-depack-buf-nalus: 0-32767 */
    if (!strcmp(attr, "sprop-depack-buf-nalus")) {
        if (atoi(value) > 0)
            hevc_data->using_donl_field = 1;
        av_log(s, AV_LOG_TRACE, "Found sprop-depack-buf-nalus in SDP, DON field usage is: %d\n",
                hevc_data->using_donl_field);
    }

    /* sprop-depack-buf-bytes: 0-4294967295 */
    /* depack-buf-cap */
    /* sprop-segmentation-id: 0-3 */
    /* sprop-spatial-segmentation-idc: [base16] */
    /* dec-parallel-ca: */
    /* include-dph */

    return 0;
}

static av_cold int hevc_parse_sdp_line(AVFormatContext *ctx, int st_index,
                                       PayloadContext *hevc_data, const char *line)
{
    AVStream *current_stream;
    AVCodecParameters *par;
    const char *sdp_line_ptr = line;

    if (st_index < 0)
        return 0;

    current_stream = ctx->streams[st_index];
    par  = current_stream->codecpar;

    if (av_strstart(sdp_line_ptr, "framesize:", &sdp_line_ptr)) {
        ff_h264_parse_framesize(par, sdp_line_ptr);
    } else if (av_strstart(sdp_line_ptr, "fmtp:", &sdp_line_ptr)) {
        int ret = ff_parse_fmtp(ctx, current_stream, hevc_data, sdp_line_ptr,
                                hevc_sdp_parse_fmtp_config);
        if (hevc_data->vps_size || hevc_data->sps_size ||
            hevc_data->pps_size || hevc_data->sei_size) {
            par->extradata_size = hevc_data->vps_size + hevc_data->sps_size +
                                  hevc_data->pps_size + hevc_data->sei_size;
            if ((ret = ff_alloc_extradata(par, par->extradata_size)) >= 0) {
                int pos = 0;
                memcpy(par->extradata + pos, hevc_data->vps, hevc_data->vps_size);
                pos += hevc_data->vps_size;
                memcpy(par->extradata + pos, hevc_data->sps, hevc_data->sps_size);
                pos += hevc_data->sps_size;
                memcpy(par->extradata + pos, hevc_data->pps, hevc_data->pps_size);
                pos += hevc_data->pps_size;
                memcpy(par->extradata + pos, hevc_data->sei, hevc_data->sei_size);
            }

            av_freep(&hevc_data->vps);
            av_freep(&hevc_data->sps);
            av_freep(&hevc_data->pps);
            av_freep(&hevc_data->sei);
            hevc_data->vps_size = 0;
            hevc_data->sps_size = 0;
            hevc_data->pps_size = 0;
            hevc_data->sei_size = 0;
        }
        return ret;
    }

    return 0;
}

/* ---- HEVC in-band parameter set cache / inject / resolution ---- */

typedef struct HEVCBitReader {
    const uint8_t *buf;
    int size;
    int bitpos;
} HEVCBitReader;

static int hevc_br_bit(HEVCBitReader *br)
{
    if (br->bitpos >= br->size * 8)
        return 0;
    int b = (br->buf[br->bitpos >> 3] >> (7 - (br->bitpos & 7))) & 1;
    br->bitpos++;
    return b;
}

static unsigned hevc_br_bits(HEVCBitReader *br, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | hevc_br_bit(br);
    return v;
}

static int hevc_br_ue(HEVCBitReader *br)
{
    int z = 0;
    while (!hevc_br_bit(br) && br->bitpos < br->size * 8 && z < 30)
        z++;
    if (z >= 30)
        return 0;
    return z ? ((1 << z) - 1 + (int)hevc_br_bits(br, z)) : 0;
}

/* HEVC SPS (NAL type 33): parse coded resolution. Layout after the 2-byte
 * NAL header: sps_video_parameter_set_id(4) sps_max_sub_layers_minus1(3)
 * sps_temporal_id_nesting_flag(1) profile_tier_level(...) sps_seq_parameter_set_id(ue)
 * chroma_format_idc(ue) [sep_colour_plane(1)] pic_width_in_luma_samples(ue)
 * pic_height_in_luma_samples(ue). */
static int hevc_parse_sps_dimensions(const uint8_t *sps, int sps_len,
                                     int *out_w, int *out_h)
{
    HEVCBitReader br;
    int i, max_sl, w, h, chroma;

    if (!sps || sps_len < 6 || ((sps[0] >> 1) & 0x3f) != 33)
        return -1;
    br.buf = sps + 2;                 /* skip 2-byte HEVC NAL header */
    br.size = sps_len - 2;
    br.bitpos = 0;

    hevc_br_bits(&br, 4);             /* sps_video_parameter_set_id */
    max_sl = hevc_br_bits(&br, 3);    /* sps_max_sub_layers_minus1 */
    hevc_br_bit(&br);                 /* sps_temporal_id_nesting_flag */

    /* profile_tier_level */
    hevc_br_bits(&br, 2);             /* general_profile_space */
    hevc_br_bit(&br);                 /* general_tier_flag */
    hevc_br_bits(&br, 5);             /* general_profile_idc */
    hevc_br_bits(&br, 32);            /* general_profile_compatibility_flag */
    hevc_br_bits(&br, 48);            /* general_constraint_indicator_flag */
    hevc_br_bits(&br, 8);             /* general_level_idc */
    {
        int np[7] = {0}, nl[7] = {0};
        for (i = 0; i < max_sl; i++) {
            np[i] = hevc_br_bit(&br); /* sub_layer_profile_present_flag */
            nl[i] = hevc_br_bit(&br); /* sub_layer_level_present_flag */
        }
        for (i = 0; i < max_sl; i++) {
            if (np[i]) {
                hevc_br_bits(&br, 2);  /* profile_space */
                hevc_br_bit(&br);      /* tier */
                hevc_br_bits(&br, 5);  /* profile_idc */
                hevc_br_bits(&br, 32); /* compat */
                hevc_br_bits(&br, 44); /* constraint */
            }
            if (nl[i])
                hevc_br_bits(&br, 8);  /* level_idc */
        }
    }

    hevc_br_ue(&br);                  /* sps_seq_parameter_set_id */
    chroma = hevc_br_ue(&br);
    if (chroma == 3)
        hevc_br_bit(&br);             /* separate_colour_plane_flag */
    w = hevc_br_ue(&br);              /* pic_width_in_luma_samples */
    h = hevc_br_ue(&br);              /* pic_height_in_luma_samples */
    if (hevc_br_bit(&br)) {           /* conformance_window_flag */
        int sx = (chroma == 1 || chroma == 2) ? 2 : 1; /* SubWidthC */
        int sy = (chroma == 1) ? 2 : 1;               /* SubHeightC */
        w -= (hevc_br_ue(&br) + hevc_br_ue(&br)) * sx; /* left + right */
        h -= (hevc_br_ue(&br) + hevc_br_ue(&br)) * sy; /* top + bottom */
    }
    if (w <= 0 || h <= 0)
        return -1;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* Cache a VPS(32)/SPS(33)/PPS(34) NAL unit received in-band. */
static void hevc_cache_parameter_set(PayloadContext *d, const uint8_t *buf,
                                     int len, int nal_type)
{
    uint8_t **store;
    int *store_size;
    uint8_t *ndata;

    if (nal_type == 32) {
        store = &d->vps_data; store_size = &d->vps_data_size;
    } else if (nal_type == 33) {
        store = &d->sps_data; store_size = &d->sps_data_size;
    } else if (nal_type == 34) {
        store = &d->pps_data; store_size = &d->pps_data_size;
    } else {
        return;
    }

    ndata = av_realloc(*store, len + sizeof(start_sequence));
    if (!ndata)
        return;
    *store = ndata;
    memcpy(ndata, start_sequence, sizeof(start_sequence));
    memcpy(ndata + sizeof(start_sequence), buf, len);
    *store_size = len + sizeof(start_sequence);
}

/* Prepend cached VPS/SPS/PPS in front of an IDR packet. */
static int hevc_prepend_parameter_sets(AVPacket *pkt, PayloadContext *d)
{
    int extra = d->vps_data_size + d->sps_data_size + d->pps_data_size;
    int ret, off = 0;

    if (!extra)
        return 0;
    ret = av_grow_packet(pkt, extra);
    if (ret < 0)
        return ret;
    memmove(pkt->data + extra, pkt->data, pkt->size - extra);
    if (d->vps_data_size) {
        memcpy(pkt->data + off, d->vps_data, d->vps_data_size);
        off += d->vps_data_size;
    }
    if (d->sps_data_size) {
        memcpy(pkt->data + off, d->sps_data, d->sps_data_size);
        off += d->sps_data_size;
    }
    if (d->pps_data_size) {
        memcpy(pkt->data + off, d->pps_data, d->pps_data_size);
        off += d->pps_data_size;
    }
    return 0;
}

/* Update keyframe/paramset tracking; mirror h264 need_pli semantics. */
static void hevc_update_nal_info(PayloadContext *d, int nal_type)
{
    if (nal_type == 19 || nal_type == 20 || nal_type == 21) {
        /* Only treat a keyframe as "delivered" once all parameter sets are
         * available. A bare IDR/CRA seen before VPS/SPS/PPS (routine on
         * WebRTC/WHEP) must NOT satisfy need_keyframe, otherwise we would
         * stop requesting a fresh keyframe via PLI and the stream stalls
         * after the bare keyframe is dropped by the discard guard below. */
        if (d->got_vps && d->got_sps && d->got_pps) {
            d->got_idr = 1;
            d->need_pli = 0;          /* fresh keyframe arrived */
        }
    } else if (nal_type == 32) {
        d->got_vps = 1;
    } else if (nal_type == 33) {
        d->got_sps = 1;
    } else if (nal_type == 34) {
        d->got_pps = 1;
    }
    /* A parameter set arriving after a keyframe was already seen means
     * that keyframe (and following P-frames) were undecodable: ask for
     * a fresh keyframe so the cached parameter sets get delivered in-band. */
    if ((nal_type == 32 || nal_type == 33 || nal_type == 34) &&
        d->got_vps && d->got_sps && d->got_pps && d->got_idr)
        d->need_pli = 1;
}

/* Once VPS+SPS+PPS are cached in-band, sync extradata (annex-b) and
 * populate coded resolution so find_stream_info() finishes immediately. */
static void hevc_sync_extradata(AVFormatContext *ctx, AVStream *st,
                                PayloadContext *d)
{
    AVCodecParameters *par = st->codecpar;
    int size = d->vps_data_size + d->sps_data_size + d->pps_data_size;
    uint8_t *extra;
    int off = 0;

    if (d->extradata_synced || par->extradata_size ||
        !d->vps_data_size || !d->sps_data_size || !d->pps_data_size)
        return;

    extra = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!extra)
        return;
    if (d->vps_data_size) {
        memcpy(extra + off, d->vps_data, d->vps_data_size);
        off += d->vps_data_size;
    }
    if (d->sps_data_size) {
        memcpy(extra + off, d->sps_data, d->sps_data_size);
        off += d->sps_data_size;
    }
    if (d->pps_data_size) {
        memcpy(extra + off, d->pps_data, d->pps_data_size);
        off += d->pps_data_size;
    }
    par->extradata = extra;
    par->extradata_size = size;
    d->extradata_synced = 1;
    if ((!par->width || !par->height) && d->sps_data_size > 2) {
        int w = 0, h = 0;
        if (hevc_parse_sps_dimensions(d->sps_data + 4,
                                      d->sps_data_size - 4, &w, &h) == 0) {
            par->width = w;
            par->height = h;
            if (!par->format)
                par->format = AV_PIX_FMT_YUV420P;
            av_log(ctx, AV_LOG_DEBUG,
                   "hevc: SPS resolution %dx%d populated from in-band SPS\n", w, h);
        }
    }
    av_log(ctx, AV_LOG_DEBUG,
           "hevc: cached VPS/SPS/PPS synced to extradata (%d bytes)\n", size);
}

static int hevc_need_keyframe(PayloadContext *ctx)
{
    PayloadContext *d = ctx;
    return !(d->got_vps && d->got_sps && d->got_pps && d->got_idr) ||
           d->need_pli;
}

static int hevc_handle_packet(AVFormatContext *ctx, PayloadContext *rtp_hevc_ctx,
                              AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                              const uint8_t *buf, int len, uint16_t seq,
                              int flags)
{
    const uint8_t *rtp_pl = buf;
    int tid, lid, nal_type;
    int first_fragment, last_fragment, fu_type;
    uint8_t new_nal_header[2];
    int res = 0;

    /* sanity check for size of input packet: 1 byte payload at least */
    if (len < RTP_HEVC_PAYLOAD_HEADER_SIZE + 1) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/HEVC packet, got %d bytes\n", len);
        return AVERROR_INVALIDDATA;
    }

    /*
     * decode the HEVC payload header according to section 4 of draft version 6:
     *
     *    0                   1
     *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
     *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *   |F|   Type    |  LayerId  | TID |
     *   +-------------+-----------------+
     *
     *      Forbidden zero (F): 1 bit
     *      NAL unit type (Type): 6 bits
     *      NUH layer ID (LayerId): 6 bits
     *      NUH temporal ID plus 1 (TID): 3 bits
     */
    nal_type =  (buf[0] >> 1) & 0x3f;
    lid  = ((buf[0] << 5) & 0x20) | ((buf[1] >> 3) & 0x1f);
    tid  =   buf[1] & 0x07;

    /* sanity check for correct layer ID */
    if (lid) {
        /* future scalable or 3D video coding extensions */
        avpriv_report_missing_feature(ctx, "Multi-layer HEVC coding");
        return AVERROR_PATCHWELCOME;
    }

    /* sanity check for correct temporal ID */
    if (!tid) {
        av_log(ctx, AV_LOG_ERROR, "Illegal temporal ID in RTP/HEVC packet\n");
        return AVERROR_INVALIDDATA;
    }

    /* sanity check for correct NAL unit type */
    if (nal_type > 50) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported (HEVC) NAL type (%d)\n", nal_type);
        return AVERROR_INVALIDDATA;
    }

    switch (nal_type) {
    /* video parameter set (VPS) */
    case 32:
    /* sequence parameter set (SPS) */
    case 33:
    /* picture parameter set (PPS) */
    case 34:
    /*  supplemental enhancement information (SEI) */
    case 39:
    /* single NAL unit packet */
    default:
        /* create A/V packet */
        if ((res = av_new_packet(pkt, sizeof(start_sequence) + len)) < 0)
            return res;
        /* A/V packet: copy start sequence */
        memcpy(pkt->data, start_sequence, sizeof(start_sequence));
        /* A/V packet: copy NAL unit data */
        memcpy(pkt->data + sizeof(start_sequence), buf, len);

        /* cache in-band parameter sets */
        if (nal_type == 32 || nal_type == 33 || nal_type == 34)
            hevc_cache_parameter_set(rtp_hevc_ctx, buf, len, nal_type);
        /* inject cached parameter sets before IDR/CRA keyframes */
        if (nal_type == 19 || nal_type == 20 || nal_type == 21)
            res = hevc_prepend_parameter_sets(pkt, rtp_hevc_ctx);
        hevc_update_nal_info(rtp_hevc_ctx, nal_type);
        break;
    /* aggregated packet (AP) - with two or more NAL units */
    case 48: {
        const uint8_t *sub;
        int sub_len, has_idr = 0, has_vps = 0, has_sps = 0, has_pps = 0;
        int total = 0;

        /* pass the HEVC payload header */
        buf += RTP_HEVC_PAYLOAD_HEADER_SIZE;
        len -= RTP_HEVC_PAYLOAD_HEADER_SIZE;

        /* pass the HEVC DONL field */
        if (rtp_hevc_ctx->using_donl_field) {
            buf += RTP_HEVC_DONL_FIELD_SIZE;
            len -= RTP_HEVC_DONL_FIELD_SIZE;
        }

        /* first pass: scan sub-NALs, cache parameter sets, size the packet */
        sub = buf;
        sub_len = len;
        while (sub_len > 2) {
            uint16_t nsz = AV_RB16(sub);
            int nt;
            sub += 2;
            sub_len -= 2;
            if (rtp_hevc_ctx->using_donl_field) {
                if (sub_len < 1)
                    break;
                sub += RTP_HEVC_DOND_FIELD_SIZE;
                sub_len -= RTP_HEVC_DOND_FIELD_SIZE;
            }
            if (nsz > sub_len)
                break;
            nt = (sub[0] >> 1) & 0x3f;
            if (nt == 32) { has_vps = 1; hevc_cache_parameter_set(rtp_hevc_ctx, sub, nsz, nt); }
            else if (nt == 33) { has_sps = 1; hevc_cache_parameter_set(rtp_hevc_ctx, sub, nsz, nt); }
            else if (nt == 34) { has_pps = 1; hevc_cache_parameter_set(rtp_hevc_ctx, sub, nsz, nt); }
            else if (nt == 19 || nt == 20 || nt == 21) has_idr = 1;
            total += sizeof(start_sequence) + nsz;
            sub += nsz;
            sub_len -= nsz;
        }
        if (!total) {
            av_log(ctx, AV_LOG_ERROR, "Invalid HEVC AP packet\n");
            return AVERROR_INVALIDDATA;
        }

        /* second pass: build the packet */
        if ((res = av_new_packet(pkt, total)) < 0)
            return res;
        {
            int pos = 0;
            sub = buf;
            sub_len = len;
            while (sub_len > 2) {
                uint16_t nsz = AV_RB16(sub);
                sub += 2;
                sub_len -= 2;
                if (rtp_hevc_ctx->using_donl_field) {
                    sub += RTP_HEVC_DOND_FIELD_SIZE;
                    sub_len -= RTP_HEVC_DOND_FIELD_SIZE;
                }
                if (nsz > sub_len)
                    break;
                memcpy(pkt->data + pos, start_sequence, sizeof(start_sequence));
                pos += sizeof(start_sequence);
                memcpy(pkt->data + pos, sub, nsz);
                pos += nsz;
                sub += nsz;
                sub_len -= nsz;
            }
        }

        /* inject cached parameter sets unless the aggregate has them all */
        if (has_idr && !(has_vps && has_sps && has_pps))
            res = hevc_prepend_parameter_sets(pkt, rtp_hevc_ctx);
        if (has_idr)
            hevc_update_nal_info(rtp_hevc_ctx, 19);
        if (has_vps) hevc_update_nal_info(rtp_hevc_ctx, 32);
        if (has_sps) hevc_update_nal_info(rtp_hevc_ctx, 33);
        if (has_pps) hevc_update_nal_info(rtp_hevc_ctx, 34);
        break;
    }
    /* fragmentation unit (FU) */
    case 49:
        /* pass the HEVC payload header */
        buf += RTP_HEVC_PAYLOAD_HEADER_SIZE;
        len -= RTP_HEVC_PAYLOAD_HEADER_SIZE;

        /*
         *    decode the FU header
         *
         *     0 1 2 3 4 5 6 7
         *    +-+-+-+-+-+-+-+-+
         *    |S|E|  FuType   |
         *    +---------------+
         *
         *       Start fragment (S): 1 bit
         *       End fragment (E): 1 bit
         *       FuType: 6 bits
         */
        first_fragment = buf[0] & 0x80;
        last_fragment  = buf[0] & 0x40;
        fu_type        = buf[0] & 0x3f;

        /* pass the HEVC FU header */
        buf += RTP_HEVC_FU_HEADER_SIZE;
        len -= RTP_HEVC_FU_HEADER_SIZE;

        /* pass the HEVC DONL field */
        if (rtp_hevc_ctx->using_donl_field) {
            buf += RTP_HEVC_DONL_FIELD_SIZE;
            len -= RTP_HEVC_DONL_FIELD_SIZE;
        }

        av_log(ctx, AV_LOG_TRACE, " FU type %d with %d bytes\n", fu_type, len);

        /* sanity check for size of input packet: 1 byte payload at least */
        if (len <= 0) {
            if (len < 0) {
                av_log(ctx, AV_LOG_ERROR,
                       "Too short RTP/HEVC packet, got %d bytes of NAL unit type %d\n",
                       len, nal_type);
                return AVERROR_INVALIDDATA;
            } else {
                return AVERROR(EAGAIN);
            }
        }

        if (first_fragment && last_fragment) {
            av_log(ctx, AV_LOG_ERROR, "Illegal combination of S and E bit in RTP/HEVC packet\n");
            return AVERROR_INVALIDDATA;
        }

        new_nal_header[0] = (rtp_pl[0] & 0x81) | (fu_type << 1);
        new_nal_header[1] = rtp_pl[1];

        res = ff_h264_handle_frag_packet(pkt, buf, len, first_fragment,
                                         new_nal_header, sizeof(new_nal_header));

        /* inject cached parameter sets before the first fragment of a
         * keyframe; FU fragments of parameter sets are not cached (their
         * data is incomplete) */
        if (res >= 0 && first_fragment &&
            (fu_type == 19 || fu_type == 20 || fu_type == 21))
            res = hevc_prepend_parameter_sets(pkt, rtp_hevc_ctx);
        if (first_fragment)
            hevc_update_nal_info(rtp_hevc_ctx, fu_type);

        break;
    /* PACI packet */
    case 50:
        /* Temporal scalability control information (TSCI) */
        avpriv_report_missing_feature(ctx, "PACI packets for RTP/HEVC");
        res = AVERROR_PATCHWELCOME;
        break;
    }

    pkt->stream_index = st->index;

    /* Sync cached VPS/SPS/PPS into extradata (no-op until all three cached);
     * also covers the FU path (case 49) which doesn't call it itself. */
    hevc_sync_extradata(ctx, st, rtp_hevc_ctx);

    /* Hold packets until VPS+SPS+PPS are ready: a slice without parameter
     * sets makes d3d11va crash. Cached sets are still synced and prepended
     * to the first IDR; hevc_need_keyframe() PLI-requests a fresh IDR so an
     * IDR-first stream recovers. */
    if (res == 0 && pkt->size > 0 &&
        !(rtp_hevc_ctx->got_vps && rtp_hevc_ctx->got_sps &&
          rtp_hevc_ctx->got_pps)) {
        av_packet_unref(pkt);
        return 1;   /* no packet yet: keep demuxing */
    }

    return res;
}

const RTPDynamicProtocolHandler ff_hevc_dynamic_handler = {
    .enc_name         = "H265",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_HEVC,
    .need_parsing     = AVSTREAM_PARSE_FULL,
    .priv_data_size   = sizeof(PayloadContext),
    .parse_sdp_a_line = hevc_parse_sdp_line,
    .parse_packet     = hevc_handle_packet,
    .need_keyframe    = hevc_need_keyframe,
};
