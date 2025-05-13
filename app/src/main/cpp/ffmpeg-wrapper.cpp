extern "C" {
#include <jni.h>
#include <android/log.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>

#define LOG_TAG "FFmpegRotation"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

typedef struct FilterContext {
    AVFilterGraph *graph;
    AVFilterContext *src;
    AVFilterContext *sink;
} FilterContext;

int init_rotation_filter(FilterContext *fctx, AVCodecParameters *codecpar, int degrees) {
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    int ret;
    char args[512];

    fctx->graph = avfilter_graph_alloc();

    // Tạo filter chain dựa trên góc xoay
    const char *filter_spec;
    switch (degrees) {
        case 90:
            filter_spec = "transpose=1";
            break;
        case 180:
            filter_spec = "transpose=1,transpose=1";
            break;
        case 270:
            filter_spec = "transpose=2";
            break;
        default:
            return AVERROR(EINVAL);
    }

    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/25",
             codecpar->width, codecpar->height, codecpar->format);

    // Tạo buffer source
    ret = avfilter_graph_create_filter(&fctx->src, buffersrc, "in", args, NULL, fctx->graph);
    if (ret < 0) return ret;

    // Tạo buffer sink
    ret = avfilter_graph_create_filter(&fctx->sink, buffersink, "out", NULL, NULL, fctx->graph);
    if (ret < 0) return ret;

    // Kết nối các filter
    AVFilterContext *transpose_ctx;
    ret = avfilter_graph_create_filter(&transpose_ctx,
                                       avfilter_get_by_name("transpose"), "transpose", filter_spec,
                                       NULL, fctx->graph);
    if (ret < 0) return ret;

    ret = avfilter_link(fctx->src, 0, transpose_ctx, 0);
    ret |= avfilter_link(transpose_ctx, 0, fctx->sink, 0);
    if (ret < 0) return ret;

    ret = avfilter_graph_config(fctx->graph, NULL);
    return ret;
}

JNIEXPORT jint JNICALL
Java_com_nhatnguyenba_ffmpeg_MainActivityKt_rotateVideo(
        JNIEnv *env, jclass thiz,
        jstring input_jstr, jstring output_jstr, jint degrees) {

    const char *input = env->GetStringUTFChars(input_jstr, NULL);
    const char *output = env->GetStringUTFChars(output_jstr, NULL);
    AVFormatContext *in_ctx = NULL, *out_ctx = NULL;
    FilterContext fctx = {0};
    int ret = 0;
    int video_stream_idx = -1;

    // Khởi tạo FFmpeg
//    av_register_all();
//    avfilter_register_all();

    // Mở input file
    if ((ret = avformat_open_input(&in_ctx, input, NULL, NULL))) {
        LOGD("Could not open input file");
        return -1;
    }

    if ((ret = avformat_find_stream_info(in_ctx, NULL))) {
        LOGD("Failed to find stream info");
        return -1;
    }

    // Tìm video stream
    video_stream_idx = av_find_best_stream(in_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream_idx < 0) {
        LOGD("No video stream found");
        ret = AVERROR(EINVAL);
        return -1;
    }

    // Chuẩn bị output file
    avformat_alloc_output_context2(&out_ctx, NULL, NULL, output);
    AVStream *out_stream = avformat_new_stream(out_ctx, NULL);

    // Copy và điều chỉnh thông số codec
    AVCodecParameters *in_par = in_ctx->streams[video_stream_idx]->codecpar;
    AVCodecParameters *out_par = out_stream->codecpar;
    avcodec_parameters_copy(out_par, in_par);

    // Đảo chiều video nếu xoay 90/270 độ
    if (degrees == 90 || degrees == 270) {
        out_par->width = in_par->height;
        out_par->height = in_par->width;
    }

    // Mở output file
    if ((ret = avio_open(&out_ctx->pb, output, AVIO_FLAG_WRITE))) {
        LOGD("Could not open output file");
        return -1;
    }

    // Khởi tạo filter graph
    if ((ret = init_rotation_filter(&fctx, in_par, degrees))) {
        LOGD("Failed to initialize filter graph");
        return -1;
    }

    // Xử lý từng frame
    AVPacket pkt;
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();

    avformat_write_header(out_ctx, NULL);

    while (av_read_frame(in_ctx, &pkt) >= 0) {
        if (pkt.stream_index != video_stream_idx) continue;

        // Giải mã frame
        AVCodecContext *codec_ctx = avcodec_alloc_context3(NULL);
        avcodec_parameters_to_context(codec_ctx, in_par);
        avcodec_open2(codec_ctx, avcodec_find_decoder(in_par->codec_id), NULL);

        ret = avcodec_send_packet(codec_ctx, &pkt);
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;

            // Áp dụng filter
            if (av_buffersrc_add_frame(fctx.src, frame) < 0) break;

            while (av_buffersink_get_frame(fctx.sink, filt_frame) >= 0) {
                // Mã hóa và ghi frame
                AVPacket out_pkt;
                av_init_packet(&out_pkt);

                AVCodecContext *enc_ctx = avcodec_alloc_context3(
                        avcodec_find_encoder(out_par->codec_id));
                avcodec_parameters_to_context(enc_ctx, out_par);
                avcodec_open2(enc_ctx, NULL, NULL);

                avcodec_send_frame(enc_ctx, filt_frame);
                avcodec_receive_packet(enc_ctx, &out_pkt);

                av_interleaved_write_frame(out_ctx, &out_pkt);
                av_packet_unref(&out_pkt);
                av_frame_unref(filt_frame);
            }
        }
        av_packet_unref(&pkt);
    }

    av_write_trailer(out_ctx);

    cleanup:
    avformat_close_input(&in_ctx);
    if (out_ctx) avio_closep(&out_ctx->pb);
    avformat_free_context(out_ctx);
    avfilter_graph_free(&fctx.graph);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    env->ReleaseStringUTFChars(input_jstr, input);
    env->ReleaseStringUTFChars(output_jstr, output);
    return ret;
}

static int init_filter(FilterContext *fctx, AVCodecParameters *codecpar, const char *filter_desc) {
    int ret;
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();

    fctx->graph = avfilter_graph_alloc();
    if (!fctx->graph || !outputs || !inputs) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    char args[512];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/25",
             codecpar->width, codecpar->height, codecpar->format);

    ret = avfilter_graph_create_filter(&fctx->src, buffersrc, "in", args, NULL, fctx->graph);
    if (ret < 0) goto end;

    ret = avfilter_graph_create_filter(&fctx->sink, buffersink, "out", NULL, NULL, fctx->graph);
    if (ret < 0) goto end;

    outputs->name = av_strdup("in");
    outputs->filter_ctx = fctx->src;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = fctx->sink;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    if ((ret = avfilter_graph_parse_ptr(fctx->graph, filter_desc, &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(fctx->graph, NULL)) < 0)
        goto end;

    end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

static int process_video(const char *input, const char *output, const char *filter_desc) {
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    FilterContext fctx = {0};
    int ret;

    if ((ret = avformat_open_input(&ifmt_ctx, input, NULL, NULL))) {
        LOGD("Could not open input file");
        return -1;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL))) {
        LOGD("Failed to retrieve input stream information");
        return -1;
    }

    AVStream *in_stream = ifmt_ctx->streams[0];
    if ((ret = init_filter(&fctx, in_stream->codecpar, filter_desc))) {
        LOGD("Failed to initialize filter");
        return -1;
    }

    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, output);
    AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
    avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);

    if ((ret = avio_open(&ofmt_ctx->pb, output, AVIO_FLAG_WRITE))) {
        LOGD("Could not open output file");
        return -1;
    }

    if ((ret = avformat_write_header(ofmt_ctx, NULL))) {
        LOGD("Error writing header");
        return -1;
    }

    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();
    AVPacket pkt;

    while (av_read_frame(ifmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index != 0) continue;

        if (av_buffersrc_add_frame(fctx.src, frame) < 0) {
            LOGD("Error feeding filter");
            break;
        }

        while (av_buffersink_get_frame(fctx.sink, filt_frame) >= 0) {
            av_packet_rescale_ts(&pkt, in_stream->time_base, out_stream->time_base);
            pkt.dts = pkt.pts = filt_frame->pts;

            if ((ret = av_interleaved_write_frame(ofmt_ctx, &pkt))) {
                LOGD("Error writing frame");
                break;
            }
            av_frame_unref(filt_frame);
        }
        av_packet_unref(&pkt);
    }

    av_write_trailer(ofmt_ctx);

    end:
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx) avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    avfilter_graph_free(&fctx.graph);
    return ret;
}

//extern "C" JNIEXPORT jint JNICALL
//Java_com_nhatnguyenba_ffmpeg_MainActivityKt_rotateVideo(
//        JNIEnv* env, jobject thiz,
//        jstring input_jstr, jstring output_jstr, jint degrees) {
//
//    const char* input = env->GetStringUTFChars(input_jstr, NULL);
//    const char* output = env->GetStringUTFChars(output_jstr, NULL);
//    char filter_desc[50];
//
//    snprintf(filter_desc, sizeof(filter_desc), "rotate=%d*PI/180", degrees);
//    int ret = process_video(input, output, filter_desc);
//
//    env->ReleaseStringUTFChars(input_jstr, input);
//    env->ReleaseStringUTFChars(output_jstr, output);
//    return ret;
//}

JNIEXPORT jint JNICALL
Java_com_nhatnguyenba_ffmpeg_MainActivityKt_applyWatermark(
        JNIEnv *env, jclass thiz,
        jstring input_jstr, jstring output_jstr, jstring watermark_jstr) {

    const char *input = env->GetStringUTFChars(input_jstr, NULL);
    const char *output = env->GetStringUTFChars(output_jstr, NULL);
    const char *watermark = env->GetStringUTFChars(watermark_jstr, NULL);

    char filter_desc[256];
    snprintf(filter_desc, sizeof(filter_desc),
             "[1]scale=100:100[wm];[0][wm]overlay=10:10");

    // Thêm logic xử lý 2 input (video + watermark)
    // ... (Cần mở rộng process_video để xử lý multiple inputs)

    env->ReleaseStringUTFChars(input_jstr, input);
    env->ReleaseStringUTFChars(output_jstr, output);
    env->ReleaseStringUTFChars(watermark_jstr, watermark);
    return 0;
}
}