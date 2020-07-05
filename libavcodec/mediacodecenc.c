#include <stdint.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixfmt.h"
#include "libavutil/internal.h"

#include "internal.h"
#include "avcodec.h"
#include "decode.h"
#include "mediacodec_wrapper.h"
#include "mediacodecdec_common.h"
#include "../build/output-armv7a/include/libavutil/frame.h"

typedef struct MediaCodecEncContext {
    AVClass *avclass;

    MediaCodecDecContext *ctx;

    int delay_flush;
    int fr;
    int i_frame_interval;
    int bit_rate;
    int profile;
    int level;

    int bitrate_mode;
#define BITRATE_MODE_CQ       0
#define BITRATE_MODE_VBR      1
#define BITRATE_MODE_CBR      2

    int draining;
    AVPacket buffered_pkt;
} MediaCodecEncContext;

static av_cold int mediacodec_encode_close(AVCodecContext *avctx) {
    return 0;
}

static av_cold int mediacodec_encode_init(AVCodecContext *avctx) {
    int ret;

    const char *codec_mime = NULL;

    FFAMediaFormat *format = NULL;
    MediaCodecEncContext *s = avctx->priv_data;

    if (avctx->bit_rate <= 0) {
        av_log(avctx, AV_LOG_ERROR, "bit rate is not set\n");
        /*ret = AVERROR_EXIT;
        goto done;*/
        avctx->bit_rate = 1 * 1000 * 1000;
    }
    s->bit_rate = avctx->bit_rate;

    format = ff_AMediaFormat_new();
    if (!format) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create media format\n");
        ret = AVERROR_EXTERNAL;
        goto done;
    }

    codec_mime = "video/hevc";

    // todo proccess extradata
    /*ret = set_extradata(avctx, format);
    if (ret < 0)
        goto done;*/

    ff_AMediaFormat_setString(format, "mime", codec_mime);
    ff_AMediaFormat_setInt32(format, "width", avctx->width);
    ff_AMediaFormat_setInt32(format, "height", avctx->height);
    if (s->fr <= 0) {
        s->fr = 30;
    }
    ff_AMediaFormat_setInt32(format, "frame-rate", s->fr);
    ff_AMediaFormat_setInt32(format, "i-frame-interval", s->i_frame_interval);
    ff_AMediaFormat_setInt32(format, "bitrate-mode", s->bitrate_mode);
    ff_AMediaFormat_setInt32(format, "bitrate", s->bit_rate);
    // ff_AMediaFormat_setInt32(format, "profile", s->profile);
    ff_AMediaFormat_setInt32(format, "max-input-size", 1920 * 1080 * 4);
    ff_AMediaFormat_setInt32(format, "color-format", 0x7F420888); // COLOR_FormatYUV420Flexible


    s->ctx = av_mallocz(sizeof(*s->ctx));
    if (!s->ctx) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate MediaCodecDecContext\n");
        ret = AVERROR(ENOMEM);
        goto done;
    }

    s->ctx->delay_flush = s->delay_flush;
    av_init_packet(&s->buffered_pkt);
    s->buffered_pkt.data = NULL;
    s->buffered_pkt.size = 0;

    if ((ret = ff_mediacodec_enc_init(avctx, s->ctx, codec_mime, format, s->profile)) < 0) {
        s->ctx = NULL;
        goto done;
    }

    av_log(avctx, AV_LOG_INFO,
           "MediaCodec started successfully: codec = %s, ret = %d\n",
           s->ctx->codec_name, ret);
    done:
    if (format) {
        ff_AMediaFormat_delete(format);
    }

    if (ret < 0) {
        mediacodec_encode_close(avctx);
    }

    return ret;
}

static av_cold int mediacodec_send_frame(AVCodecContext *avctx, const AVFrame *frame) {
    MediaCodecEncContext *s = avctx->priv_data;
    int ret;
    /* In delay_flush mode, wait until the user has released or rendered
       all retained frames. */
    if (s->delay_flush && ff_mediacodec_dec_is_flushing(avctx, s->ctx)) {
        if (!ff_mediacodec_dec_flush(avctx, s->ctx)) {
            return AVERROR(EAGAIN);
        }
    }
    ret = ff_mediacodec_enc_send(avctx, s->ctx, frame, &s->buffered_pkt);
    av_log(avctx, AV_LOG_ERROR, "YYYYYYYYYY mediacodec_send_frame return: %d", ret);
    return ret;
}

static av_cold int mediacodec_receive_packet(AVCodecContext *avctx, AVPacket *avpkt) {
    MediaCodecEncContext *s = avctx->priv_data;
    if (s->buffered_pkt.size > 0) {
        av_packet_move_ref(avpkt, &s->buffered_pkt);
        return 0;
    }
    return ff_mediacodec_enc_receive(avctx, s->ctx, avpkt);
}

static void mediacodec_encode_flush(AVCodecContext *avctx) {
    MediaCodecEncContext *s = avctx->priv_data;

    // av_packet_unref(&s->buffered_pkt);

    ff_mediacodec_dec_flush(avctx, s->ctx);
}

#define OFFSET(x) offsetof(MediaCodecEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption ff_mediacodec_venc_options[] = {
        {"delay_flush",      "Delay flush until hw output buffers are returned to the encoder",
                                                             OFFSET(delay_flush),      AV_OPT_TYPE_BOOL,  {.i64 = 0},                0,       1,
                VE},
        {"fr",               "set frame rate",               OFFSET(fr),               AV_OPT_TYPE_INT,   {.i64 = 30},               INT_MIN, INT_MAX,
                VE},
        {"i_frame_interval", "set I frame interval",
                                                             OFFSET(i_frame_interval), AV_OPT_TYPE_INT,   {.i64 = 1},                INT_MIN, INT_MAX,
                VE},
        {"bitrate_mode",     "set bit rate mode cq/cbr/vbr", OFFSET(bitrate_mode),     AV_OPT_TYPE_FLAGS, {.i64 = BITRATE_MODE_CBR}, INT_MIN, INT_MAX,
                VE, "bitrate_mode"},
        {"cq",               "do not control",                          0,             AV_OPT_TYPE_CONST, {.i64 = BITRATE_MODE_CQ},  INT_MIN, INT_MAX,
                VE, "bitrate_mode"},
        {"vbr",              "depends on the picture",                  0,             AV_OPT_TYPE_CONST, {.i64 = BITRATE_MODE_VBR}, INT_MIN, INT_MAX,
                VE, "bitrate_mode"},
        {"cbr",              "try best to keep bitrate as set bitrate", 0,             AV_OPT_TYPE_CONST, {.i64 = BITRATE_MODE_CBR}, INT_MIN, INT_MAX,
                VE, "bitrate_mode"},
        {"profile",          "set mediacodec profile",
                                                             OFFSET(profile),          AV_OPT_TYPE_INT,   {.i64 = 1},                INT_MIN, INT_MAX,
                VE},
        {NULL}
};


static const AVClass ff_hevc_mediacodec_enc_class = {\
    .class_name = "hevc_mediacodec_enc", \
    .item_name  = av_default_item_name, \
    .option     = ff_mediacodec_venc_options, \
    .version    = LIBAVUTIL_VERSION_INT, \
};

AVCodec ff_hevc_mediacodec_encoder = {
        .name = "hevc_mediacodec_enc",
        .long_name = NULL_IF_CONFIG_SMALL("MediaCodec HEVC Encoder"),
        .type = AVMEDIA_TYPE_VIDEO,
        .id = AV_CODEC_ID_HEVC,
        .init = mediacodec_encode_init,
        // .encode2 = mediacodec_encode_frame,
        .send_frame = mediacodec_send_frame,
        .receive_packet = mediacodec_receive_packet,
        .close = mediacodec_encode_close,
        .flush = mediacodec_encode_flush,
        .priv_data_size = sizeof(MediaCodecEncContext),
        .priv_class = &ff_hevc_mediacodec_enc_class,
        .capabilities = AV_CODEC_CAP_DELAY,
        .caps_internal = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
        .wrapper_name = "mediacodec",
};
