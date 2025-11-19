/*
 * Copyright © 2024, Lynne
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdint.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

static int decode_frame(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame)
{
    int ret = 0;

    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting a packet for decoding (%s)\n",
                av_err2str(ret));
        return ret;
    }

    ret = avcodec_receive_frame(dec, frame);
    return ret;
}

static enum AVPixelFormat remap_pixfmt(enum AVPixelFormat fmt)
{
    switch (fmt) {
    case AV_PIX_FMT_YUV420P:
        return AV_PIX_FMT_NV12;
    case AV_PIX_FMT_GBRAP16:
        return AV_PIX_FMT_RGBA64;
    case AV_PIX_FMT_RGB48LE:
    case AV_PIX_FMT_RGB48BE:
        return AV_PIX_FMT_GBRP16;
    case AV_PIX_FMT_BGR0:
        return AV_PIX_FMT_RGB0;
    default:
        return fmt;
    };
}

int main(int argc, const char **argv)
{
    int err;
    av_log_set_level(AV_LOG_TRACE);

    AVFormatContext *in_ctx = avformat_alloc_context();
    err = avformat_open_input(&in_ctx, argv[1], NULL, NULL);
    if (err < 0) {
        printf("Error opening input file: %s\n", argv[1]);
        return AVERROR(err);
    }

    const AVCodec *in_dec;
    int sid = err = av_find_best_stream(in_ctx, AVMEDIA_TYPE_VIDEO, -1, -1,
                                        &in_dec, 0);
    if (err < 0) {
        printf("Error finding stream for file: %s\n", argv[1]);
        return AVERROR(err);
    }

    AVCodecContext *in_avctx = avcodec_alloc_context3(in_dec);
    if (err < 0) {
        printf("Error opening decoder: %s\n", av_err2str(err));
        return AVERROR(err);
    }

    err = avcodec_parameters_to_context(in_avctx, in_ctx->streams[sid]->codecpar);
    if (err < 0) {
        printf("Error using codec parameters: %s\n", av_err2str(err));
        return AVERROR(err);
    }

    AVBufferRef *hw_dev_ref;
    err = av_hwdevice_ctx_create(&hw_dev_ref, AV_HWDEVICE_TYPE_VULKAN,
                                 argv[2], NULL, 0);
    if (err < 0) {
        printf("Error creating device: %s\n", av_err2str(err));
        return AVERROR(err);
    }

    if (!strcmp(argv[3], "1"))
        in_avctx->hw_device_ctx = hw_dev_ref;

    err = avcodec_open2(in_avctx, in_dec, NULL);
    if (err < 0) {
        printf("Error opening decoder: %s\n", av_err2str(err));
        return AVERROR(err);
    }

    av_dump_format(in_ctx, 0, argv[1], 0);

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return ENOMEM;

    err = av_read_frame(in_ctx, pkt);
    if (err < 0) {
        printf("Error reading packet: %s\n", av_err2str(err));
        return AVERROR(err);
    }

    /* Probe */
    AVFrame *frame = av_frame_alloc();
    err = decode_frame(in_avctx, pkt, frame);
    if (err < 0) {
        printf("Error decoding frame: %s\n", av_err2str(err));
        return AVERROR(err);
    }
    av_frame_unref(frame);

    /* Frame context */
    AVBufferRef *hwfc_ref = NULL;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(in_avctx->pix_fmt);
    if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        printf("Software decoding\n");
        printf("Creating frame context to upload hardware frames into\n");

        hwfc_ref = av_hwframe_ctx_alloc(hw_dev_ref);
        if (!hwfc_ref)
            return ENOMEM;

        AVHWFramesContext *hwfc = (AVHWFramesContext *)hwfc_ref->data;
        hwfc->format = AV_PIX_FMT_VULKAN;
        hwfc->sw_format = remap_pixfmt(in_avctx->pix_fmt);
        hwfc->width  = in_avctx->width;
        hwfc->height = in_avctx->height;

        err = av_hwframe_ctx_init(hwfc_ref);
        if (err < 0) {
            printf("Error creating frames context: %s\n", av_err2str(err));
            return AVERROR(err);
        }
    } else {
        printf("Hardware decoding\n");
        hwfc_ref = in_avctx->hw_frames_ctx;
    }

    /* Encoder */
    const AVCodec *out_enc = avcodec_find_encoder_by_name("ffv1_vulkan");
    AVCodecContext *out_avctx = avcodec_alloc_context3(out_enc);
    if (err < 0) {
        printf("Error opening encoder\n");
        return AVERROR(err);
    }

    out_avctx->time_base = av_make_q(1, 1);
    out_avctx->width = in_avctx->width;
    out_avctx->height = in_avctx->height;
    out_avctx->sw_pix_fmt = remap_pixfmt(in_avctx->sw_pix_fmt);
    out_avctx->pix_fmt = AV_PIX_FMT_VULKAN;
    out_avctx->hw_frames_ctx = hwfc_ref;
    out_avctx->hw_device_ctx = hw_dev_ref;

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "level", "4", 0);
    av_dict_set(&opts, "strict", "-2", 0);
    av_dict_set(&opts, "async_depth", "4", 0);
    err = avcodec_open2(out_avctx, out_enc, &opts);
    if (err < 0) {
        printf("Error initializing encoder: %s\n", av_err2str(err));
        return AVERROR(err);
    }

    int max_frames = 1000;
    int64_t time_start = av_gettime();

    AVPacket *out_pkt = av_packet_alloc();
    AVFrame *hw_frame = av_frame_alloc();
    AVFrame *temp = av_frame_alloc();

    SwsContext *swc = sws_alloc_context();

    av_log_set_level(AV_LOG_INFO);

    if (argc > 4 && !strcmp(argv[4], "1"))
        printf("Decoding and encoding %i frames\n", max_frames);
    else
        printf("Decoding %i frames\n", max_frames);

    for (int i = 0; i < max_frames; i++) {
        err = decode_frame(in_avctx, pkt, frame);
        if (err < 0) {
            printf("Error decoding frame: %s\n", av_err2str(err));
            return AVERROR(err);
        }

        AVFrame *src = frame;
        if (!frame->hw_frames_ctx &&
            frame->format != remap_pixfmt(in_avctx->sw_pix_fmt)) {
            temp->width = frame->width;
            temp->height = frame->height;
            temp->format = remap_pixfmt(in_avctx->sw_pix_fmt);

            err = av_frame_get_buffer(temp, 0);
            if (err < 0) {
                printf("Error allocating temporary frame: %s\n", av_err2str(err));
                return AVERROR(err);
            }

            err = sws_scale_frame(swc, temp, frame);
            if (err < 0) {
                printf("Error scaling frame: %s\n", av_err2str(err));
                return AVERROR(err);
            }

            src = temp;
        }

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            err = av_hwframe_get_buffer(hwfc_ref, hw_frame, 0);
            if (err < 0) {
                printf("Error allocating hardware frame\n");
                return AVERROR(err);
            }

            err = av_hwframe_transfer_data(hw_frame, src, 0);
            if (err < 0) {
                printf("Error uploading frame: %s\n", av_err2str(err));
                return AVERROR(err);
            }
            src = hw_frame;
        }

        if (argc > 3 && !strcmp(argv[4], "1")) {
            err = avcodec_send_frame(out_avctx, src);
            if (err < 0) {
                printf("Error sending frame for encoding: %s\n", av_err2str(err));
                return AVERROR(err);
            }

            avcodec_receive_packet(out_avctx, out_pkt);
            if (err < 0) {
                printf("Error receiving encoded packet: %s\n", av_err2str(err));
                return AVERROR(err);
            }
        }

        /* Final */
        printf("\rFrames done: %i, fmt: %i", i + 1, in_avctx->pix_fmt);
        fflush(stdout);

        av_frame_unref(temp);
        av_frame_unref(hw_frame);
        av_frame_unref(frame);
        av_packet_unref(out_pkt);
    }
    printf("\n");
    int64_t time = av_gettime() - time_start;
    printf("Time = %f\n", (double)time/(1000.0*1000.0));
}
