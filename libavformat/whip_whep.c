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

/* Register WHEP protocol handler */
// WHEP protocol is now registered in whep.c instead of here

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
    rtcInitLogger(RTC_LOG_VERBOSE, libdatachannel_log);
}

int ff_whip_whep_exchange_and_set_sdp(AVFormatContext *s, int pc, const char *token, char **session_url)
{
    char offer[SDP_MAX_SIZE], offer_hex[2 * SDP_MAX_SIZE], answer[SDP_MAX_SIZE];
    AVDictionary *options = NULL;
    AVIOContext *io_ctx = NULL;
    int ret;
    char *http_url = NULL;

    av_log(s, AV_LOG_INFO, "Starting WHEP SDP exchange with URL: %s\n", s->url);
    
    // Convert whep:// to http:// for the actual HTTP request
    if (av_strstart(s->url, "whep://", NULL)) {
        http_url = av_malloc(strlen(s->url) + 1);
        if (!http_url) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate memory for HTTP URL\n");
            return AVERROR(ENOMEM);
        }
        strcpy(http_url, "http://");
        strcat(http_url, s->url + 7); // Skip "whep://"
        av_log(s, AV_LOG_INFO, "Converted WHEP URL to HTTP: %s\n", http_url);
    } else if (av_strstart(s->url, "http", NULL)) {
        http_url = av_strdup(s->url);
    } else {
        av_log(s, AV_LOG_ERROR, "Unsupported URL scheme\n");
        return AVERROR(EINVAL);
    }

    if (rtcCreateOffer(pc, offer, sizeof(offer)) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create offer\n");
        return AVERROR_EXTERNAL;
    }
    av_log(s, AV_LOG_DEBUG, "Generated offer: %s\n", offer);

    if (rtcSetLocalDescription(pc, "offer") < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set local description\n");
        return AVERROR_EXTERNAL;
    }

    av_dict_set(&options, "content_type", "application/sdp", 0);
    ff_data_to_hex(offer_hex, offer, strlen(offer), 0);
    av_dict_set(&options, "post_data", offer_hex, 0);
    if (token) {
        char* headers = av_asprintf("Authorization: Bearer %s\r\n", token);
        if (!headers) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate headers\n");
            return AVERROR(ENOMEM);
        }
        av_dict_set(&options, "headers", headers, 0);
        av_free(headers);
    }

    av_log(s, AV_LOG_INFO, "Sending SDP offer to URL: %s\n", http_url);
    ret = avio_open2(&io_ctx, http_url, AVIO_FLAG_READ, NULL, &options);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to send offer to endpoint: %s (%s)\n", http_url, av_err2str(ret));
        goto fail;
    }

    ret = avio_read(io_ctx, answer, sizeof(answer) - 1);
    if (ret <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to read answer: %s\n",
               av_err2str(ret));
        if (ret == 0) {
            av_log(s, AV_LOG_ERROR, "No data received from server\n");
            ret = AVERROR(EIO);
        }
        goto fail;
    }
    answer[ret] = 0;
    av_log(s, AV_LOG_INFO, "Received answer: %s\n", answer);

    if (rtcSetRemoteDescription(pc, answer, "answer") < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set remote description: %s\n", answer);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (session_url) {
        ret = av_opt_get(io_ctx, "new_location", AV_OPT_SEARCH_CHILDREN, (uint8_t **)session_url);
        if (ret < 0) {
            av_log(s, AV_LOG_WARNING, "Failed to get session URL: %s\n", av_err2str(ret));
            *session_url = NULL;
        } else {
            av_log(s, AV_LOG_INFO, "Session URL: %s\n", *session_url ? *session_url : "none");
        }
    }

    ret = 0;
fail:
    avio_closep(&io_ctx);
    av_dict_free(&options);
    av_free(http_url);
    return ret;
}

int ff_whip_whep_delete_session(AVFormatContext *s, const char *token, const char *session_url)
{
    AVDictionary *options = NULL;
    AVIOContext *io_ctx = NULL;
    int ret;
    char *http_url = NULL;

    if (!session_url) {
        av_log(s, AV_LOG_ERROR, "No session URL provided\n");
        return AVERROR(EINVAL);
    }
    
    // Convert whep:// to http:// for the actual HTTP request
    if (av_strstart(session_url, "whep://", NULL)) {
        http_url = av_malloc(strlen(session_url) + 1);
        if (!http_url) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate memory for HTTP URL\n");
            return AVERROR(ENOMEM);
        }
        strcpy(http_url, "http://");
        strcat(http_url, session_url + 7); // Skip "whep://"
        av_log(s, AV_LOG_INFO, "Converted WHEP URL to HTTP for DELETE: %s\n", http_url);
    } else if (av_strstart(session_url, "http", NULL)) {
        http_url = av_strdup(session_url);
    } else {
        av_log(s, AV_LOG_ERROR, "Unsupported URL scheme: %s\n", session_url);
        return AVERROR(EINVAL);
    }

    av_dict_set(&options, "method", "DELETE", 0);
    if (token) {
        char* headers = av_asprintf("Authorization: Bearer %s\r\n", token);
        if (!headers) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate headers\n");
            av_free(http_url);
            return AVERROR(ENOMEM);
        }
        av_dict_set(&options, "headers", headers, 0);
        av_free(headers);
    }
    ret = avio_open2(&io_ctx, http_url, AVIO_FLAG_READ, NULL, &options);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to delete session: %s\n", av_err2str(ret));
    }

    avio_closep(&io_ctx);
    av_dict_free(&options);
    av_free(http_url);
    return ret;
}

