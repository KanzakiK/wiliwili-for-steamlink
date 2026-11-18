/*
 * 最小 FFmpeg 解封装/解码验证
 *
 * 用法: ./ffmpeg_demux_test <file.flv|file.mp4>
 *
 * 验证链路:
 *   FLV/MOV 解封装 -> H.264 AVCC 转 Annex-B -> AAC 解码 -> 48kHz/2ch S16 PCM
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

static int count_start_codes(const uint8_t *data, int size)
{
    int count = 0;
    for (int i = 0; i + 2 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            count++;
    }
    return count;
}

static int first_nal_type(const uint8_t *data, int size)
{
    for (int i = 0; i + 3 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            return data[i + 3] & 0x1f;
    }
    return -1;
}

static void print_error(const char *what, int err)
{
    char buffer[128];
    av_strerror(err, buffer, sizeof(buffer));
    fprintf(stderr, "%s: %s\n", what, buffer);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.flv|file.mp4>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    AVFormatContext *format = NULL;
    int ret = avformat_open_input(&format, path, NULL, NULL);
    if (ret < 0) {
        print_error("avformat_open_input 失败", ret);
        return 1;
    }

    ret = avformat_find_stream_info(format, NULL);
    if (ret < 0) {
        print_error("avformat_find_stream_info 失败", ret);
        avformat_close_input(&format);
        return 1;
    }

    av_dump_format(format, 0, path, 0);

    int video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    printf("流索引: video=%d audio=%d, 容器时长=%lld ms\n",
           video_index, audio_index,
           (long long)(format->duration / (AV_TIME_BASE / 1000)));

    if (video_index < 0) {
        fprintf(stderr, "没有找到视频流\n");
        avformat_close_input(&format);
        return 1;
    }

    AVStream *video_stream = format->streams[video_index];
    printf("视频: %s %dx%d, extradata=%d 字节\n",
           avcodec_get_name(video_stream->codecpar->codec_id),
           video_stream->codecpar->width,
           video_stream->codecpar->height,
           video_stream->codecpar->extradata_size);

    const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filter) {
        fprintf(stderr, "找不到 h264_mp4toannexb\n");
        avformat_close_input(&format);
        return 1;
    }

    AVBSFContext *bsf = NULL;
    ret = av_bsf_alloc(filter, &bsf);
    if (ret < 0) {
        print_error("av_bsf_alloc 失败", ret);
        avformat_close_input(&format);
        return 1;
    }

    avcodec_parameters_copy(bsf->par_in, video_stream->codecpar);
    bsf->time_base_in = video_stream->time_base;
    ret = av_bsf_init(bsf);
    if (ret < 0) {
        print_error("av_bsf_init 失败", ret);
        av_bsf_free(&bsf);
        avformat_close_input(&format);
        return 1;
    }

    AVCodecContext *audio_decoder = NULL;
    SwrContext *resampler = NULL;

    if (audio_index >= 0) {
        AVStream *audio_stream = format->streams[audio_index];
        const AVCodec *codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        printf("音频: %s %d Hz, %d 声道\n",
               avcodec_get_name(audio_stream->codecpar->codec_id),
               audio_stream->codecpar->sample_rate,
               audio_stream->codecpar->ch_layout.nb_channels);

        if (codec) {
            audio_decoder = avcodec_alloc_context3(codec);
            if (audio_decoder) {
                avcodec_parameters_to_context(audio_decoder, audio_stream->codecpar);
                if (avcodec_open2(audio_decoder, codec, NULL) < 0)
                    avcodec_free_context(&audio_decoder);
            }
        }
        if (!audio_decoder)
            fprintf(stderr, "音频解码器打开失败，将跳过音频\n");
    }

    AVPacket *packet = av_packet_alloc();
    AVPacket *output = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!packet || !output || !frame) {
        fprintf(stderr, "内存不足\n");
        return 1;
    }

    int video_packets = 0;
    int audio_packets = 0;
    int printed_annexb = 0;
    long video_bytes = 0;
    long annexb_bytes = 0;
    long decoded_samples = 0;
    long pcm_bytes = 0;

    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video_index) {
            video_packets++;
            video_bytes += packet->size;

            ret = av_bsf_send_packet(bsf, packet);
            if (ret < 0) {
                print_error("av_bsf_send_packet 失败", ret);
            } else {
                while (av_bsf_receive_packet(bsf, output) == 0) {
                    annexb_bytes += output->size;
                    if (printed_annexb < 5) {
                        printf("Annex-B 包 %d: %d 字节, %d 个 NAL, 首 NAL 类型 %d\n",
                               printed_annexb, output->size,
                               count_start_codes(output->data, output->size),
                               first_nal_type(output->data, output->size));
                        printed_annexb++;
                    }
                    av_packet_unref(output);
                }
            }
        } else if (packet->stream_index == audio_index && audio_decoder) {
            audio_packets++;
            ret = avcodec_send_packet(audio_decoder, packet);
            if (ret >= 0) {
                while (avcodec_receive_frame(audio_decoder, frame) >= 0) {
                    decoded_samples += frame->nb_samples;

                    if (!resampler) {
                        AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
                        ret = swr_alloc_set_opts2(&resampler,
                                                  &output_layout, AV_SAMPLE_FMT_S16, 48000,
                                                  &frame->ch_layout,
                                                  (enum AVSampleFormat)frame->format,
                                                  frame->sample_rate, 0, NULL);
                        if (ret < 0 || swr_init(resampler) < 0) {
                            fprintf(stderr, "重采样器初始化失败\n");
                            swr_free(&resampler);
                        }
                    }

                    if (resampler) {
                        int max_output = (int)av_rescale_rnd(
                            swr_get_delay(resampler, frame->sample_rate) + frame->nb_samples,
                            48000, frame->sample_rate, AV_ROUND_UP);
                        uint8_t *pcm = malloc((size_t)max_output * 2 * sizeof(int16_t));
                        if (pcm) {
                            int converted = swr_convert(resampler, &pcm, max_output,
                                                        (const uint8_t **)frame->data,
                                                        frame->nb_samples);
                            if (converted > 0)
                                pcm_bytes += (long)converted * 2 * sizeof(int16_t);
                            free(pcm);
                        }
                    }

                    av_frame_unref(frame);
                }
            }
        }

        av_packet_unref(packet);
    }

    /* 冲掉 BSF 里剩余的数据 */
    av_bsf_send_packet(bsf, NULL);
    while (av_bsf_receive_packet(bsf, output) == 0) {
        annexb_bytes += output->size;
        av_packet_unref(output);
    }

    printf("汇总: 视频包=%d (%ld 字节) -> Annex-B %ld 字节; 音频包=%d, 解码 %ld 样本, PCM %ld 字节\n",
           video_packets, video_bytes, annexb_bytes,
           audio_packets, decoded_samples, pcm_bytes);

    av_packet_free(&packet);
    av_packet_free(&output);
    av_frame_free(&frame);
    if (resampler)
        swr_free(&resampler);
    if (audio_decoder)
        avcodec_free_context(&audio_decoder);
    av_bsf_free(&bsf);
    avformat_close_input(&format);
    return 0;
}
