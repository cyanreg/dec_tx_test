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

static int decode_frame(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame)
{
    int ret = 0;

    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting a packet for decoding (%s)\n", av_err2str(ret));
        return ret;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret < 0) {
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                return 0;

            fprintf(stderr, "Error during decoding (%s)\n", av_err2str(ret));
            return ret;
        }
    }

    return ret;
}

static enum AVPixelFormat remap_pixfmt(enum AVPixelFormat fmt)
{
    switch (fmt) {
    case AV_PIX_FMT_GBRAP16:
        return AV_PIX_FMT_RGBA64;
    case AV_PIX_FMT_RGB48LE:
    case AV_PIX_FMT_RGB48BE:
        return AV_PIX_FMT_GBRP16;
    case AV_PIX_FMT_GBRP10:
        return AV_PIX_FMT_X2BGR10;
    case AV_PIX_FMT_BGR0:
        return AV_PIX_FMT_RGB0;
    default:
        return fmt;
    };
}

int main(int argc, const char **argv)
{
    int err;

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
        printf("Error opening decoder\n");
        return AVERROR(err);
    }

    err = avcodec_parameters_to_context(in_avctx, in_ctx->streams[sid]->codecpar);
    if (err < 0) {
        printf("Error using codec parameters\n");
        return AVERROR(err);
    }

    AVBufferRef *hw_dev_ref;
    err = av_hwdevice_ctx_create(&hw_dev_ref, AV_HWDEVICE_TYPE_VULKAN,
                                 argv[2], NULL, 0);
    if (err < 0) {
        printf("Error creating device\n");
        return AVERROR(err);
    }

    if (!strcmp(argv[3], "1"))
        in_avctx->hw_device_ctx = hw_dev_ref;

    err = avcodec_open2(in_avctx, in_dec, NULL);
    if (err < 0) {
        printf("Error opening decoder\n");
        return AVERROR(err);
    }

    av_dump_format(in_ctx, 0, argv[1], 0);

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return ENOMEM;

    err = av_read_frame(in_ctx, pkt);
    if (err < 0) {
        printf("Error reading packet\n");
        return AVERROR(err);
    }

    AVFrame *frame = av_frame_alloc();
    if (!pkt)
        return ENOMEM;

    AVFrame *hw_frame = av_frame_alloc();
    if (!pkt)
        return ENOMEM;

    /* Probe */
    err = decode_frame(in_avctx, pkt, frame);
    if (err < 0) {
        printf("Error decoding frame\n");
        return AVERROR(err);
    }
    av_frame_unref(frame);

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
            printf("Error creating frames context\n");
            return AVERROR(err);
        }

        err = av_hwframe_get_buffer(hwfc_ref, hw_frame, 0);
        if (err < 0) {
            printf("Error allocating hardware frame\n");
            return AVERROR(err);
        }
    } else {
        printf("Hardware decoding\n");
    }

    int max_frames = 1000;
    int64_t time_start = av_gettime();

    printf("Decoding %i frames\n", max_frames);
    for (int i = 0; i < max_frames; i++) {
        err = decode_frame(in_avctx, pkt, frame);
        if (err < 0) {
            printf("Error decoding frame\n");
            return AVERROR(err);
        }

        if (hwfc_ref)
            av_hwframe_transfer_data(hw_frame, frame, 0);

        printf("\rFrame decoded: %i, fmt: %i", i + 1, in_avctx->pix_fmt);
        fflush(stdout);

        av_frame_unref(frame);
    }
    printf("\n");
    int64_t time = av_gettime() - time_start;
    printf("Time = %f\n", (double)time/(1000.0*1000.0));
}
