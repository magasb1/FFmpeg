/*
 * RTP depacketizer for SMPTE ST 336 (KLV) encoded data, RFC 6597
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
 * @brief RTP depacketizer for SMPTE ST 336 encoded data (KLV metadata)
 *
 * Implements the receiver side of RFC 6597. The payload carries no
 * header: each RTP packet holds a complete KLVunit or a fragment of one.
 * All fragments of a KLVunit share the RTP timestamp and the marker bit
 * is set on the packet carrying the last byte of the unit.
 */

#include "libavutil/attributes.h"

#include "avio_internal.h"
#include "rtpdec_formats.h"

/* First four bytes of every SMPTE Universal Label (SMPTE ST 298) */
static const uint8_t klv_ul_prefix[4] = { 0x06, 0x0E, 0x2B, 0x34 };

struct PayloadContext {
    AVIOContext *buf;       ///< KLVunit being reassembled
    uint32_t     timestamp; ///< RTP timestamp of the unit in buf
    uint16_t     prev_seq;  ///< sequence number of the last packet seen
    int          prev_seq_valid;
    /**
     * Set while the packets being received are known to start at a
     * KLVunit boundary. Cleared on packet loss: RFC 6597 section 4.3.1
     * requires the partial unit before a loss and the first unit after a
     * loss to be treated as damaged, so both are dropped and reception
     * resumes after the next marker bit.
     */
    int          in_sync;
};

static av_cold void smpte336m_close_context(PayloadContext *data)
{
    ffio_free_dyn_buf(&data->buf);
}

static int smpte336m_handle_packet(AVFormatContext *ctx, PayloadContext *data,
                                   AVStream *st, AVPacket *pkt,
                                   uint32_t *timestamp, const uint8_t *buf,
                                   int len, uint16_t seq, int flags)
{
    int ret;

    if (len < 1) {
        av_log(ctx, AV_LOG_ERROR, "Empty RTP/SMPTE 336M packet\n");
        return AVERROR_INVALIDDATA;
    }

    if (!data->prev_seq_valid) {
        /* First packet of the session: the RTP header alone cannot tell
         * whether it starts a KLVunit or is the tail of one. Accept it if
         * it begins with the SMPTE Universal Label designator, otherwise
         * wait for the next unit boundary. */
        data->in_sync = len >= sizeof(klv_ul_prefix) &&
                        !memcmp(buf, klv_ul_prefix, sizeof(klv_ul_prefix));
    } else if ((uint16_t)(data->prev_seq + 1) != seq) {
        av_log(ctx, AV_LOG_WARNING,
               "RTP sequence gap (expected %u, got %u), "
               "dropping damaged KLVunit(s)\n",
               (unsigned)(uint16_t)(data->prev_seq + 1), (unsigned)seq);
        ffio_free_dyn_buf(&data->buf);
        data->in_sync = 0;
    }
    data->prev_seq       = seq;
    data->prev_seq_valid = 1;

    if (!data->in_sync) {
        /* Skip up to and including the next marker bit packet, then
         * resume with the first byte of the following KLVunit. */
        if (flags & RTP_FLAG_MARKER)
            data->in_sync = 1;
        return AVERROR(EAGAIN);
    }

    if (data->buf && data->timestamp != *timestamp) {
        /* All packets of a KLVunit share one timestamp; the end of the
         * previous unit must have gone missing undetected. */
        av_log(ctx, AV_LOG_WARNING,
               "RTP timestamp changed inside a KLVunit, "
               "dropping incomplete unit\n");
        ffio_free_dyn_buf(&data->buf);
    }

    if (!data->buf) {
        ret = avio_open_dyn_buf(&data->buf);
        if (ret < 0)
            return ret;
        data->timestamp = *timestamp;
    }

    avio_write(data->buf, buf, len);

    if (!(flags & RTP_FLAG_MARKER))
        return AVERROR(EAGAIN);

    ret = ff_rtp_finalize_packet(pkt, &data->buf, st->index);
    if (ret < 0)
        return ret;

    *timestamp = data->timestamp;

    return 0;
}

const RTPDynamicProtocolHandler ff_smpte336m_dynamic_handler = {
    .enc_name       = "smpte336m",
    .codec_type     = AVMEDIA_TYPE_DATA,
    .codec_id       = AV_CODEC_ID_SMPTE_KLV,
    .need_parsing   = AVSTREAM_PARSE_NONE,
    .priv_data_size = sizeof(PayloadContext),
    .close          = smpte336m_close_context,
    .parse_packet   = smpte336m_handle_packet,
};
