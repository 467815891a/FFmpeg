/*
 * RTP H.264 Protocol (RFC3984)
 * Copyright (c) 2006 Ryan Martell
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

/**
 * @file
 * @brief H.264 / RTP Code (RFC3984)
 * @author Ryan Martell <rdm4@martellventures.com>
 *
 * @note Notes:
 * Notes:
 * This currently supports packetization mode:
 * Single Nal Unit Mode (0), or
 * Non-Interleaved Mode (1).  It currently does not support
 * Interleaved Mode (2). (This requires implementing STAP-B, MTAP16, MTAP24,
 *                        FU-B packet types)
 */

#include "libavutil/attributes.h"
#include "libavutil/base64.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "avformat.h"

#include "rtpdec.h"
#include "rtpdec_formats.h"

struct PayloadContext {
    // sdp setup parameters
    uint8_t profile_idc;
    uint8_t profile_iop;
    uint8_t level_idc;
    int packetization_mode;
#ifdef DEBUG
    int packet_types_received[32];
#endif
    int got_sps;
    int got_pps;
    int got_idr;
    /* Cached in-band SPS/PPS NAL units (annex-b, incl. start code).
     * WebRTC/WHEP senders often transmit them only once per stream;
     * we cache them and inject before every IDR frame so the decoder
     * (and hwaccel init) always has valid parameter sets. */
    uint8_t *sps_data;
    int sps_size;
    uint8_t *pps_data;
    int pps_size;
    int extradata_synced;
    /* Set when parameter sets arrive after a keyframe was already decoded
     * without them (SPS/PPS often arrive late on WebRTC, after the first
     * IDR). The demuxer then requests a new keyframe (PLI); the next IDR
     * gets the cached parameter sets prepended and the decoder recovers. */
    int need_pli;
};

#ifdef DEBUG
#define COUNT_NAL_TYPE(data, nal) data->packet_types_received[(nal) & 0x1f]++
#define NAL_COUNTERS data->packet_types_received
#else
#define COUNT_NAL_TYPE(data, nal) do { } while (0)
#define NAL_COUNTERS NULL
#endif
#define NAL_MASK 0x1f

static const uint8_t start_sequence[] = { 0, 0, 0, 1 };

static void parse_profile_level_id(AVFormatContext *s,
                                   PayloadContext *h264_data,
                                   const char *value)
{
    char buffer[3];
    // 6 characters=3 bytes, in hex.
    uint8_t profile_idc;
    uint8_t profile_iop;
    uint8_t level_idc;

    buffer[0]   = value[0];
    buffer[1]   = value[1];
    buffer[2]   = '\0';
    profile_idc = strtol(buffer, NULL, 16);
    buffer[0]   = value[2];
    buffer[1]   = value[3];
    profile_iop = strtol(buffer, NULL, 16);
    buffer[0]   = value[4];
    buffer[1]   = value[5];
    level_idc   = strtol(buffer, NULL, 16);

    av_log(s, AV_LOG_DEBUG,
           "RTP Profile IDC: %x Profile IOP: %x Level: %x\n",
           profile_idc, profile_iop, level_idc);
    h264_data->profile_idc = profile_idc;
    h264_data->profile_iop = profile_iop;
    h264_data->level_idc   = level_idc;
}

int ff_h264_parse_sprop_parameter_sets(AVFormatContext *s,
                                       uint8_t **data_ptr, int *size_ptr,
                                       const char *value)
{
    char base64packet[1024];
    uint8_t decoded_packet[1024];
    int packet_size;

    while (*value) {
        char *dst = base64packet;

        while (*value && *value != ','
               && (dst - base64packet) < sizeof(base64packet) - 1) {
            *dst++ = *value++;
        }
        *dst++ = '\0';

        if (*value == ',')
            value++;

        packet_size = av_base64_decode(decoded_packet, base64packet,
                                       sizeof(decoded_packet));
        if (packet_size > 0) {
            uint8_t *dest = av_realloc(*data_ptr,
                                       packet_size + sizeof(start_sequence) +
                                       *size_ptr +
                                       AV_INPUT_BUFFER_PADDING_SIZE);
            if (!dest) {
                av_log(s, AV_LOG_ERROR,
                       "Unable to allocate memory for extradata!\n");
                return AVERROR(ENOMEM);
            }
            *data_ptr = dest;

            memcpy(dest + *size_ptr, start_sequence,
                   sizeof(start_sequence));
            memcpy(dest + *size_ptr + sizeof(start_sequence),
                   decoded_packet, packet_size);
            memset(dest + *size_ptr + sizeof(start_sequence) +
                   packet_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

            *size_ptr += sizeof(start_sequence) + packet_size;
        }
    }

    return 0;
}

static int sdp_parse_fmtp_config_h264(AVFormatContext *s,
                                      AVStream *stream,
                                      PayloadContext *h264_data,
                                      const char *attr, const char *value)
{
    AVCodecParameters *par = stream->codecpar;

    if (!strcmp(attr, "packetization-mode")) {
        av_log(s, AV_LOG_DEBUG, "RTP Packetization Mode: %d\n", atoi(value));
        h264_data->packetization_mode = atoi(value);
        /*
         * Packetization Mode:
         * 0 or not present: Single NAL mode (Only nals from 1-23 are allowed)
         * 1: Non-interleaved Mode: 1-23, 24 (STAP-A), 28 (FU-A) are allowed.
         * 2: Interleaved Mode: 25 (STAP-B), 26 (MTAP16), 27 (MTAP24), 28 (FU-A),
         *                      and 29 (FU-B) are allowed.
         */
        if (h264_data->packetization_mode > 1)
            av_log(s, AV_LOG_ERROR,
                   "Interleaved RTP mode is not supported yet.\n");
    } else if (!strcmp(attr, "profile-level-id")) {
        if (strlen(value) == 6)
            parse_profile_level_id(s, h264_data, value);
    } else if (!strcmp(attr, "sprop-parameter-sets")) {
        int ret;
        if (*value == 0 || value[strlen(value) - 1] == ',') {
            av_log(s, AV_LOG_WARNING, "Missing PPS in sprop-parameter-sets, ignoring\n");
            return 0;
        }
        par->extradata_size = 0;
        av_freep(&par->extradata);
        ret = ff_h264_parse_sprop_parameter_sets(s, &par->extradata,
                                                 &par->extradata_size, value);
        av_log(s, AV_LOG_DEBUG, "Extradata set to %p (size: %d)\n",
               par->extradata, par->extradata_size);
        if (ret == 0) {
            h264_data->got_sps = 1;
            h264_data->got_pps = 1;
        }
        return ret;
    }
    return 0;
}

void ff_h264_parse_framesize(AVCodecParameters *par, const char *p)
{
    char buf1[50];
    char *dst = buf1;

    // remove the protocol identifier
    while (*p && *p == ' ')
        p++;                     // strip spaces.
    while (*p && *p != ' ')
        p++;                     // eat protocol identifier
    while (*p && *p == ' ')
        p++;                     // strip trailing spaces.
    while (*p && *p != '-' && (dst - buf1) < sizeof(buf1) - 1)
        *dst++ = *p++;
    *dst = '\0';

    // a='framesize:96 320-240'
    // set our parameters
    par->width   = atoi(buf1);
    par->height  = atoi(p + 1); // skip the -
}

static void update_sps_pps_idr_info(PayloadContext *data, uint8_t nal_type) {
    switch (nal_type) {
    case 5:
        /* Only treat a keyframe as "delivered" once the parameter sets are
         * available. An IDR seen before SPS/PPS (routine on WebRTC/WHEP)
         * must NOT satisfy need_keyframe, otherwise we would stop requesting
         * a fresh IDR via PLI and the stream would stall after the bare IDR
         * is dropped below. */
        if (data->got_sps && data->got_pps) {
            data->got_idr = 1;
            data->need_pli = 0;   /* new keyframe arrived: PLI demand satisfied */
        }
        break;
    case 7:
        data->got_sps = 1;
        break;
    case 8:
        data->got_pps = 1;
        break;
    }
    /* Only a *parameter set* arriving after a keyframe was already seen
     * warrants a PLI: the decoder opened without params and that keyframe
     * could not be decoded. A complete STAP-A (SPS+PPS+IDR) must not
     * trigger it. */
    if ((nal_type == 7 || nal_type == 8) &&
        data->got_sps && data->got_pps && data->got_idr)
        data->need_pli = 1;
}

/* Cache a SPS (7) or PPS (8) NAL unit received in-band. The payload is
 * stored in annex-b form (start code + NAL). A newer parameter set of the
 * same type replaces the older one. */
static void h264_cache_parameter_set(PayloadContext *data,
                                     const uint8_t *buf, int len)
{
    uint8_t nal_type = buf[0] & 0x1f;
    uint8_t **store;
    int *store_size;
    uint8_t *ndata;

    if (nal_type == 7) {
        store      = &data->sps_data;
        store_size = &data->sps_size;
    } else if (nal_type == 8) {
        store      = &data->pps_data;
        store_size = &data->pps_size;
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

/* Prepend the cached SPS/PPS (if any) in front of an IDR packet so the
 * decoder always receives valid parameter sets with every keyframe. */
static int h264_prepend_parameter_sets(AVPacket *pkt, PayloadContext *data)
{
    int extra = data->sps_size + data->pps_size;
    int ret;

    if (!extra)
        return 0;

    ret = av_grow_packet(pkt, extra);
    if (ret < 0)
        return ret;

    memmove(pkt->data + extra, pkt->data, pkt->size - extra);
    if (data->sps_size) {
        memcpy(pkt->data, data->sps_data, data->sps_size);
        if (data->pps_size)
            memcpy(pkt->data + data->sps_size, data->pps_data, data->pps_size);
    } else if (data->pps_size) {
        memcpy(pkt->data, data->pps_data, data->pps_size);
    }
    return 0;
}

/* Minimal H.264 SPS bit-parser: extracts coded resolution (cropped).
 * Used to populate codecpar width/height so avformat_find_stream_info()
 * can complete without decoding a first frame (which may be undecodable
 * on WebRTC/WHEP when parameter sets arrive late, stalling the demuxer
 * and racing the player thread's decoder init -> ta corruption). */
typedef struct H264BitReader {
    const uint8_t *buf;
    int size;
    int bitpos;
} H264BitReader;

static int h264_br_bit(H264BitReader *br)
{
    if (br->bitpos >= br->size * 8)
        return 0;
    int b = (br->buf[br->bitpos >> 3] >> (7 - (br->bitpos & 7))) & 1;
    br->bitpos++;
    return b;
}

static unsigned h264_br_bits(H264BitReader *br, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | h264_br_bit(br);
    return v;
}

static int h264_br_ue(H264BitReader *br)
{
    int z = 0;
    while (!h264_br_bit(br) && br->bitpos < br->size * 8 && z < 30)
        z++;
    if (z >= 30)
        return 0;
    return z ? ((1 << z) - 1 + (int)h264_br_bits(br, z)) : 0;
}

static int h264_br_se(H264BitReader *br)
{
    int k = h264_br_ue(br);
    return (k & 1) ? (k + 1) / 2 : -(k / 2);
}

static int h264_parse_sps_dimensions(const uint8_t *sps, int sps_len,
                                     int *out_w, int *out_h)
{
    uint8_t ebuf[256];
    const uint8_t *psps = sps;
    int plen = sps_len;
    H264BitReader br;
    int profile_idc, chroma_format_idc = 1;
    int i, n, poc_type;
    int width_mbs, height_mbs, frame_mbs_only, crop;
    int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    int crop_x, crop_y, cw, ch;

    if (!sps || sps_len < 4 || (sps[0] & 0x1f) != 7)
        return -1;
    if (sps_len > 4) {
        /* strip emulation prevention bytes (00 00 03 -> 00 00) */
        int o = 0, zeros = 0;
        for (i = 1; i < sps_len && o < (int)sizeof(ebuf); i++) {
            if (zeros >= 2 && sps[i] == 3) {
                zeros = 0;
                continue;
            }
            ebuf[o++] = sps[i];
            zeros = (sps[i] == 0) ? zeros + 1 : 0;
        }
        psps = ebuf;
        plen = o;
    } else {
        psps = sps + 1;
        plen = sps_len - 1;
    }
    br.buf = psps;
    br.size = plen;
    br.bitpos = 0;

    profile_idc = h264_br_bits(&br, 8);
    h264_br_bits(&br, 8);                       /* constraint flags */
    h264_br_bits(&br, 8);                       /* level_idc */
    h264_br_ue(&br);                            /* seq_parameter_set_id */

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
        profile_idc == 86  || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135) {
        chroma_format_idc = h264_br_ue(&br);
        if (chroma_format_idc == 3)
            h264_br_bit(&br);                   /* separate_colour_plane */
        h264_br_ue(&br);                        /* bit_depth_luma_minus8 */
        h264_br_ue(&br);                        /* bit_depth_chroma_minus8 */
        h264_br_bit(&br);                       /* qpprime_y_zero_transform_bypass */
        if (h264_br_bit(&br)) {                 /* seq_scaling_matrix_present */
            int n2 = (chroma_format_idc != 3) ? 8 : 12;
            for (i = 0; i < n2; i++) {
                if (h264_br_bit(&br)) {
                    int sz = (i < 6) ? 16 : 64;
                    for (int j = 0; j < sz; j++)
                        h264_br_se(&br);
                }
            }
        }
    }

    h264_br_ue(&br);                            /* log2_max_frame_num_minus4 */
    poc_type = h264_br_ue(&br);
    if (poc_type == 0) {
        h264_br_ue(&br);                        /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (poc_type == 1) {
        h264_br_bit(&br);                       /* delta_pic_order_always_zero */
        h264_br_se(&br);                        /* offset_for_non_ref_pic */
        h264_br_se(&br);                        /* offset_for_top_to_bottom */
        n = h264_br_ue(&br);                    /* num_ref_frames_in_pic_order_cnt_cycle */
        for (i = 0; i < n; i++)
            h264_br_se(&br);
    }

    h264_br_ue(&br);                            /* max_num_ref_frames */
    h264_br_bit(&br);                           /* gaps_in_frame_num_value_allowed */
    width_mbs  = h264_br_ue(&br) + 1;
    height_mbs = h264_br_ue(&br) + 1;
    frame_mbs_only = h264_br_bit(&br);
    if (!frame_mbs_only)
        h264_br_bit(&br);                       /* mb_adaptive_frame_field */
    h264_br_bit(&br);                           /* direct_8x8_inference */
    crop = h264_br_bit(&br);
    if (crop) {
        crop_l = h264_br_ue(&br);
        crop_r = h264_br_ue(&br);
        crop_t = h264_br_ue(&br);
        crop_b = h264_br_ue(&br);
    }

    cw = width_mbs * 16;
    ch = height_mbs * 16 * (2 - frame_mbs_only);
    crop_x = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
    crop_y = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
    crop_y *= (2 - frame_mbs_only);
    cw -= (crop_l + crop_r) * crop_x;
    ch -= (crop_t + crop_b) * crop_y;
    if (cw <= 0 || ch <= 0)
        return -1;
    *out_w = cw;
    *out_h = ch;
    return 0;
}

/* Once both SPS and PPS have been cached in-band, sync them into the
 * stream extradata (only if the SDP did not provide sprop-parameter-sets).
 * This allows hwaccel init (e.g. d3d11va) to parse the parameter sets. */
static int h264_sync_extradata(AVFormatContext *ctx, AVStream *st,
                               PayloadContext *data)
{
    AVCodecParameters *par = st->codecpar;
    int size = data->sps_size + data->pps_size;
    uint8_t *extra;
    int off = 0;

    /* only sync once BOTH parameter sets have been cached in-band:
     * WebRTC senders often deliver SPS and PPS as separate single-NAL
     * RTP packets; syncing after just the SPS would leave the decoder
     * with a SPS-only extradata and a missing PPS. */
    if (data->extradata_synced || par->extradata_size ||
        !data->sps_size || !data->pps_size)
        return 0;

    extra = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!extra)
        return AVERROR(ENOMEM);

    if (data->sps_size) {
        memcpy(extra + off, data->sps_data, data->sps_size);
        off += data->sps_size;
    }
    if (data->pps_size) {
        memcpy(extra + off, data->pps_data, data->pps_size);
        off += data->pps_size;
    }
    par->extradata      = extra;
    par->extradata_size = size;
    data->extradata_synced = 1;
    /* Populate coded resolution from the SPS so find_stream_info() can
     * finish without decoding a frame (WebRTC first IDR often arrives
     * before the parameter sets -> undecodable -> demuxer stalls and
     * races the player's decoder init -> ta corruption/crash). */
    if ((!par->width || !par->height) && data->sps_size > 4) {
        int w = 0, h = 0;
        if (h264_parse_sps_dimensions(data->sps_data + 4,
                                      data->sps_size - 4, &w, &h) == 0) {
            par->width  = w;
            par->height = h;
            if (!par->format)
                par->format = AV_PIX_FMT_YUV420P;
            av_log(ctx, AV_LOG_DEBUG,
                   "h264: SPS resolution %dx%d populated from in-band SPS\n", w, h);
        }
    }
    av_log(ctx, AV_LOG_DEBUG,
           "h264: cached SPS/PPS synced to extradata (%d bytes, sps=%d pps=%d, sps_nal=%d pps_nal=%d)\n",
           size, data->sps_size, data->pps_size,
           data->sps_size ? data->sps_size - 4 : 0,
           data->pps_size ? data->pps_size - 4 : 0);
    return 0;
}

int ff_h264_handle_aggregated_packet(AVFormatContext *ctx, PayloadContext *data, AVPacket *pkt,
                                     const uint8_t *buf, int len,
                                     int skip_between, int *nal_counters,
                                     int nal_mask)
{
    int pass         = 0;
    int total_length = 0;
    uint8_t *dst     = NULL;
    int ret;

    // first we are going to figure out the total size
    for (pass = 0; pass < 2; pass++) {
        const uint8_t *src = buf;
        int src_len        = len;

        while (src_len > 2) {
            uint16_t nal_size = AV_RB16(src);

            // consume the length of the aggregate
            src     += 2;
            src_len -= 2;

            if (nal_size <= src_len) {
                if (pass == 0) {
                    // counting
                    total_length += sizeof(start_sequence) + nal_size;
                } else {
                    // copying
                    memcpy(dst, start_sequence, sizeof(start_sequence));
                    dst += sizeof(start_sequence);
                    memcpy(dst, src, nal_size);
                    if (nal_counters)
                        nal_counters[(*src) & nal_mask]++;
                    update_sps_pps_idr_info(data, (*src) & nal_mask);
                    dst += nal_size;
                }
            } else {
                av_log(ctx, AV_LOG_ERROR,
                       "nal size exceeds length: %d %d\n", nal_size, src_len);
                return AVERROR_INVALIDDATA;
            }

            // eat what we handled
            src     += nal_size + skip_between;
            src_len -= nal_size + skip_between;
        }

        if (pass == 0) {
            /* now we know the total size of the packet (with the
             * start sequences added) */
            if ((ret = av_new_packet(pkt, total_length)) < 0)
                return ret;
            dst = pkt->data;
        }
    }

    return 0;
}

int ff_h264_handle_frag_packet(AVPacket *pkt, const uint8_t *buf, int len,
                               int start_bit, const uint8_t *nal_header,
                               int nal_header_len)
{
    int ret;
    int tot_len = len;
    int pos = 0;
    if (start_bit)
        tot_len += sizeof(start_sequence) + nal_header_len;
    if ((ret = av_new_packet(pkt, tot_len)) < 0)
        return ret;
    if (start_bit) {
        memcpy(pkt->data + pos, start_sequence, sizeof(start_sequence));
        pos += sizeof(start_sequence);
        memcpy(pkt->data + pos, nal_header, nal_header_len);
        pos += nal_header_len;
    }
    memcpy(pkt->data + pos, buf, len);
    return 0;
}

static int h264_handle_packet_fu_a(AVFormatContext *ctx, PayloadContext *data, AVPacket *pkt,
                                   const uint8_t *buf, int len,
                                   int *nal_counters, int nal_mask)
{
    uint8_t fu_indicator, fu_header, start_bit, nal_type, nal;
    int ret;

    if (len < 3) {
        av_log(ctx, AV_LOG_ERROR, "Too short data for FU-A H.264 RTP packet\n");
        return AVERROR_INVALIDDATA;
    }

    fu_indicator = buf[0];
    fu_header    = buf[1];
    start_bit    = fu_header >> 7;
    nal_type     = fu_header & 0x1f;
    nal          = fu_indicator & 0xe0 | nal_type;

    // skip the fu_indicator and fu_header
    buf += 2;
    len -= 2;

    if (start_bit) {
        if (nal_counters)
            nal_counters[nal_type & nal_mask]++;
        update_sps_pps_idr_info(data, nal_type & nal_mask);
    }
    ret = ff_h264_handle_frag_packet(pkt, buf, len, start_bit, &nal, 1);
    /* first fragment of an IDR: prepend cached SPS/PPS */
    if (ret >= 0 && start_bit && (nal_type & nal_mask) == 5)
        ret = h264_prepend_parameter_sets(pkt, data);
    return ret;
}

// return 0 on packet, no more left, 1 on packet, 1 on partial packet
static int h264_handle_packet(AVFormatContext *ctx, PayloadContext *data,
                              AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                              const uint8_t *buf, int len, uint16_t seq,
                              int flags)
{
    uint8_t nal;
    uint8_t type;
    int result = 0;

    if (!len) {
        av_log(ctx, AV_LOG_ERROR, "Empty H.264 RTP packet\n");
        return AVERROR_INVALIDDATA;
    }
    nal  = buf[0];
    type = nal & 0x1f;

    /* Simplify the case (these are all the NAL types used internally by
     * the H.264 codec). */
    if (type >= 1 && type <= 23)
        type = 1;
    switch (type) {
    case 0:                    // undefined, but pass them through
    case 1:
        if ((result = av_new_packet(pkt, len + sizeof(start_sequence))) < 0)
            return result;
        memcpy(pkt->data, start_sequence, sizeof(start_sequence));
        memcpy(pkt->data + sizeof(start_sequence), buf, len);
        COUNT_NAL_TYPE(data, nal);
        /* cache SPS/PPS seen in-band (single NAL mode) */
        if ((nal & 0x1f) == 7 || (nal & 0x1f) == 8)
            h264_cache_parameter_set(data, buf, len);
        update_sps_pps_idr_info(data, nal & 0x1f);
        /* inject cached SPS/PPS before an IDR */
        if ((nal & 0x1f) == 5)
            result = h264_prepend_parameter_sets(pkt, data);
        break;

    case 24:                   // STAP-A (one packet, multiple nals)
        // consume the STAP-A NAL
        buf++;
        len--;
        {
            const uint8_t *sub = buf;
            int sub_len = len;
            int has_idr = 0;
            int has_sps = 0;
            int has_pps = 0;

            /* First pass: cache SPS/PPS carried in-band, detect IDR and
             * whether the aggregate already carries parameter sets. */
            while (sub_len > 2) {
                uint16_t nal_size = AV_RB16(sub);
                sub     += 2;
                sub_len -= 2;
                if (nal_size > sub_len)
                    break;
                switch (sub[0] & 0x1f) {
                case 7:
                    has_sps = 1;
                    h264_cache_parameter_set(data, sub, nal_size);
                    break;
                case 8:
                    has_pps = 1;
                    h264_cache_parameter_set(data, sub, nal_size);
                    break;
                case 5:
                    has_idr = 1;
                    break;
                }
                sub     += nal_size;
                sub_len -= nal_size;
            }
            result = ff_h264_handle_aggregated_packet(ctx, data, pkt, buf, len, 0,
                                                      NAL_COUNTERS, NAL_MASK);
            /* inject cached SPS/PPS before an IDR unless the aggregate
             * already contains both parameter sets */
            if (result == 0 && has_idr && !(has_sps && has_pps))
                result = h264_prepend_parameter_sets(pkt, data);
        }
        break;

    case 25:                   // STAP-B
    case 26:                   // MTAP-16
    case 27:                   // MTAP-24
    case 29:                   // FU-B
        avpriv_report_missing_feature(ctx, "RTP H.264 NAL unit type %d", type);
        result = AVERROR_PATCHWELCOME;
        break;

    case 28:                   // FU-A (fragmented nal)
        result = h264_handle_packet_fu_a(ctx, data, pkt, buf, len,
                                         NAL_COUNTERS, NAL_MASK);
        break;

    case 30:                   // undefined
    case 31:                   // undefined
    default:
        av_log(ctx, AV_LOG_ERROR, "Undefined type (%d)\n", type);
        result = AVERROR_INVALIDDATA;
        break;
    }

    pkt->stream_index = st->index;

    /* Sync cached SPS/PPS into extradata (no-op until both cached); also
     * covers the FU-A path (case 28) which doesn't call it itself. */
    h264_sync_extradata(ctx, st, data);

    /* Hold packets until SPS+PPS are ready: a slice without parameter sets
     * makes d3d11va crash. Cached SPS/PPS are still synced to extradata and
     * prepended to the first IDR; need_keyframe() keeps PLI-requesting a
     * fresh IDR so an IDR-first stream recovers. */
    if (result == 0 && pkt->size > 0 && !(data->got_sps && data->got_pps)) {
        av_packet_unref(pkt);
        return 1;   /* no packet yet: keep demuxing */
    }

    return result;
}

static void h264_close_context(PayloadContext *data)
{
#ifdef DEBUG
    int ii;

    for (ii = 0; ii < 32; ii++) {
        if (data->packet_types_received[ii])
            av_log(NULL, AV_LOG_DEBUG, "Received %d packets of type %d\n",
                   data->packet_types_received[ii], ii);
    }
#endif
    av_free(data->sps_data);
    av_free(data->pps_data);
}

static int parse_h264_sdp_line(AVFormatContext *s, int st_index,
                               PayloadContext *h264_data, const char *line)
{
    AVStream *stream;
    const char *p = line;

    if (st_index < 0)
        return 0;

    stream = s->streams[st_index];

    if (av_strstart(p, "framesize:", &p)) {
        ff_h264_parse_framesize(stream->codecpar, p);
    } else if (av_strstart(p, "fmtp:", &p)) {
        return ff_parse_fmtp(s, stream, h264_data, p, sdp_parse_fmtp_config_h264);
    } else if (av_strstart(p, "cliprect:", &p)) {
        // could use this if we wanted.
    }

    return 0;
}

static int h264_need_keyframe(PayloadContext *h264) {
    return !(h264->got_sps && h264->got_pps && h264->got_idr) ||
           h264->need_pli;
}

const RTPDynamicProtocolHandler ff_h264_dynamic_handler = {
    .enc_name         = "H264",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_H264,
    .need_parsing     = AVSTREAM_PARSE_FULL,
    .priv_data_size   = sizeof(PayloadContext),
    .parse_sdp_a_line = parse_h264_sdp_line,
    .close            = h264_close_context,
    .parse_packet     = h264_handle_packet,
    .need_keyframe    = h264_need_keyframe,
};
