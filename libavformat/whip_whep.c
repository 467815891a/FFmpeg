/*
 * WHIP/WHEP shared functions
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

#include <rtc/rtc.h>

#include "libavutil/avstring.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "whip_whep.h"

// Convert libdatachannel log level to equivalent ffmpeg log level.
static int libdatachannel_to_ffmpeg_log_level(int libdatachannel_log_level)
{
    if      (libdatachannel_log_level >= RTC_LOG_VERBOSE) return AV_LOG_TRACE;
    else if (libdatachannel_log_level >= RTC_LOG_DEBUG)   return AV_LOG_DEBUG;
    else if (libdatachannel_log_level >= RTC_LOG_INFO)    return AV_LOG_VERBOSE;
    else if (libdatachannel_log_level >= RTC_LOG_WARNING) return AV_LOG_WARNING;
    else if (libdatachannel_log_level >= RTC_LOG_ERROR)   return AV_LOG_ERROR;
    else if (libdatachannel_log_level >= RTC_LOG_FATAL)   return AV_LOG_FATAL;
    else                                                  return AV_LOG_QUIET;
}

static void libdatachannel_log(rtcLogLevel rtc_level, const char *message)
{
    av_log(NULL, libdatachannel_to_ffmpeg_log_level(rtc_level), "[libdatachannel] %s\n", message);
}

void ff_whip_whep_init_rtc_logger(void)
{
    rtcLogLevel rtc_level = RTC_LOG_FATAL;
    int ff_level = av_log_get_level();

    if      (ff_level >= AV_LOG_TRACE)   rtc_level = RTC_LOG_VERBOSE;
    else if (ff_level >= AV_LOG_DEBUG)   rtc_level = RTC_LOG_DEBUG;
    else if (ff_level >= AV_LOG_INFO)    rtc_level = RTC_LOG_INFO;
    else if (ff_level >= AV_LOG_WARNING) rtc_level = RTC_LOG_WARNING;
    else if (ff_level >= AV_LOG_ERROR)   rtc_level = RTC_LOG_ERROR;

    rtcInitLogger(rtc_level, libdatachannel_log);
}

/* Convert a "whep://" URL to the equivalent "http://" URL used for the
 * WHEP resource. Returns NULL if the scheme is not supported. */
static char *whep_url_to_http(const char *url)
{
    if (av_strstart(url, "whep://", NULL))
        return av_asprintf("http://%s", url + strlen("whep://"));
    if (av_strstart(url, "http", NULL))
        return av_strdup(url);
    return NULL;
}

int ff_whip_whep_exchange_and_set_sdp(AVFormatContext *s, int pc, const char *token, char **session_url)
{
    char offer[SDP_MAX_SIZE], offer_hex[2 * SDP_MAX_SIZE], answer[SDP_MAX_SIZE];
    AVDictionary *options = NULL;
    AVIOContext *io_ctx = NULL;
    char *http_url;
    int ret;

    http_url = whep_url_to_http(s->url);
    if (!http_url) {
        av_log(s, AV_LOG_ERROR, "Unsupported URL scheme\n");
        return AVERROR(EINVAL);
    }

    if (rtcCreateOffer(pc, offer, sizeof(offer)) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create offer\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(s, AV_LOG_DEBUG, "Generated offer: %s\n", offer);

    if (rtcSetLocalDescription(pc, "offer") < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set local description\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    av_dict_set(&options, "content_type", "application/sdp", 0);
    ff_data_to_hex(offer_hex, offer, strlen(offer), 0);
    av_dict_set(&options, "post_data", offer_hex, 0);
    if (token) {
        char* headers = av_asprintf("Authorization: Bearer %s\r\n", token);
        if (!headers) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate headers\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        av_dict_set(&options, "headers", headers, 0);
        av_free(headers);
    }

    ret = avio_open2(&io_ctx, http_url, AVIO_FLAG_READ, NULL, &options);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to send offer to endpoint: %s\n", av_err2str(ret));
        goto fail;
    }

    ret = avio_read(io_ctx, answer, sizeof(answer) - 1);
    if (ret <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to read answer: %s\n",
               av_err2str(ret));
        ret = AVERROR(EIO);
        goto fail;
    }
    answer[ret] = 0;
    av_log(s, AV_LOG_DEBUG, "Received answer: %s\n", answer);

    if (rtcSetRemoteDescription(pc, answer, "answer") < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set remote description: %s\n", answer);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (session_url)
        av_opt_get(io_ctx, "new_location", AV_OPT_SEARCH_CHILDREN, (uint8_t **)session_url);

    ret = 0;
fail:
    av_free(http_url);
    avio_closep(&io_ctx);
    av_dict_free(&options);
    return ret;
}

int ff_whip_whep_delete_session(AVFormatContext *s, const char *token, const char *session_url)
{
    AVDictionary *options = NULL;
    AVIOContext *io_ctx = NULL;
    char *http_url;
    int ret;

    if (!session_url) {
        av_log(s, AV_LOG_ERROR, "No session URL provided\n");
        return AVERROR(EINVAL);
    }
    http_url = whep_url_to_http(session_url);
    if (!http_url) {
        av_log(s, AV_LOG_ERROR, "Unsupported URL scheme\n");
        return AVERROR(EINVAL);
    }

    av_dict_set(&options, "method", "DELETE", 0);
    if (token) {
        char* headers = av_asprintf("Authorization: Bearer %s\r\n", token);
        if (!headers) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate headers\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        av_dict_set(&options, "headers", headers, 0);
        av_free(headers);
    }
    ret = avio_open2(&io_ctx, http_url, AVIO_FLAG_READ, NULL, &options);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to delete session: %s\n", av_err2str(ret));
    }

fail:
    av_free(http_url);
    avio_closep(&io_ctx);
    av_dict_free(&options);
    return ret;
}
