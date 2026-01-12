#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
}

// 错误处理辅助函数
static void log_error(const std::string& msg, int averror) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(averror, err_buf, sizeof(err_buf));
    throw std::runtime_error(msg + ": " + err_buf);
}

void convert_to_opus(const std::string& input_filename, const std::string& output_filename) {
    // --- 1. 定义目标参数 ---
    const int64_t  TARGET_CH_LAYOUT = AV_CH_LAYOUT_MONO;
    const int      TARGET_SAMPLE_RATE = 16000;
    const AVSampleFormat TARGET_SAMPLE_FMT = AV_SAMPLE_FMT_S16;
    int ret;

    // --- 2. 打开输入并初始化解码器 ---
    AVFormatContext* input_format_ctx = nullptr;
    if ((ret = avformat_open_input(&input_format_ctx, input_filename.c_str(), nullptr, nullptr)) < 0) log_error("avformat_open_input", ret);
    if ((ret = avformat_find_stream_info(input_format_ctx, nullptr)) < 0) log_error("avformat_find_stream_info", ret);
    int audio_stream_index = av_find_best_stream(input_format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_index < 0) throw std::runtime_error("Could not find audio stream");
    AVStream* audio_stream = input_format_ctx->streams[audio_stream_index];
    const AVCodec* decoder = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    AVCodecContext* decoder_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(decoder_ctx, audio_stream->codecpar);
    if ((ret = avcodec_open2(decoder_ctx, decoder, nullptr)) < 0) log_error("avcodec_open2(decoder)", ret);

    // --- 3. 初始化编码器 ---
    const AVCodec* encoder = avcodec_find_encoder_by_name("libopus");
    AVCodecContext* encoder_ctx = avcodec_alloc_context3(encoder);
    encoder_ctx->channels = av_get_channel_layout_nb_channels(TARGET_CH_LAYOUT);
    encoder_ctx->channel_layout = TARGET_CH_LAYOUT;
    encoder_ctx->sample_rate = TARGET_SAMPLE_RATE;
    encoder_ctx->sample_fmt = TARGET_SAMPLE_FMT;
    encoder_ctx->bit_rate = 32000;
    av_opt_set_int(encoder_ctx->priv_data, "frame_duration", 10, 0);
    if ((ret = avcodec_open2(encoder_ctx, encoder, nullptr)) < 0) log_error("avcodec_open2(encoder)", ret);
    
    const int ENCODER_FRAME_SIZE = encoder_ctx->frame_size;

    // --- 4. 初始化重采样器 ---
    SwrContext* swr_ctx = swr_alloc_set_opts(nullptr, TARGET_CH_LAYOUT, TARGET_SAMPLE_FMT, TARGET_SAMPLE_RATE, decoder_ctx->channel_layout, decoder_ctx->sample_fmt, decoder_ctx->sample_rate, 0, nullptr);
    swr_init(swr_ctx);

    // --- 5. 打开输出文件 ---
    FILE* outfile = fopen(output_filename.c_str(), "wb");
    if (!outfile) throw std::runtime_error("Could not open output file");
    
    // --- 6. 初始化帧、包和FIFO蓄水池 ---
    AVPacket* input_packet = av_packet_alloc();
    AVFrame* decoded_frame = av_frame_alloc();
    AVFrame* output_frame = av_frame_alloc();
    AVPacket* output_packet = av_packet_alloc();
    
    output_frame->nb_samples = ENCODER_FRAME_SIZE;
    output_frame->format = TARGET_SAMPLE_FMT;
    output_frame->channel_layout = TARGET_CH_LAYOUT;
    av_frame_get_buffer(output_frame, 0);

    AVAudioFifo* fifo = av_audio_fifo_alloc(TARGET_SAMPLE_FMT, av_get_channel_layout_nb_channels(TARGET_CH_LAYOUT), 1);

    auto write_output_frame = [&](AVFrame* frame) {
        ret = avcodec_send_frame(encoder_ctx, frame);
        if (ret < 0) return;
        while (ret >= 0) {
            ret = avcodec_receive_packet(encoder_ctx, output_packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) log_error("avcodec_receive_packet", ret);
            uint16_t len = static_cast<uint16_t>(output_packet->size);
            uint8_t len_be[2] = { (uint8_t)((len >> 8) & 0xFF), (uint8_t)(len & 0xFF) };
            fwrite(len_be, 1, 2, outfile);
            fwrite(output_packet->data, 1, output_packet->size, outfile);
            av_packet_unref(output_packet);
        }
    };

    auto add_samples_to_fifo = [&](uint8_t** samples, int frame_size) {
        if (av_audio_fifo_write(fifo, (void**)samples, frame_size) < frame_size) {
            throw std::runtime_error("Failed to write to FIFO");
        }
    };
    
    auto process_fifo = [&]() {
        while (av_audio_fifo_size(fifo) >= ENCODER_FRAME_SIZE) {
            if (av_audio_fifo_read(fifo, (void**)output_frame->data, ENCODER_FRAME_SIZE) < ENCODER_FRAME_SIZE) {
                throw std::runtime_error("Failed to read from FIFO");
            }
            write_output_frame(output_frame);
        }
    };

    auto resample_and_store = [&](AVFrame* input_frame) {
        uint8_t** dst_data = nullptr;
        int max_dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, decoder_ctx->sample_rate) + (input_frame ? input_frame->nb_samples : 0), TARGET_SAMPLE_RATE, decoder_ctx->sample_rate, AV_ROUND_UP);
        av_samples_alloc_array_and_samples(&dst_data, NULL, av_get_channel_layout_nb_channels(TARGET_CH_LAYOUT), max_dst_nb_samples, TARGET_SAMPLE_FMT, 0);

        int dst_nb_samples = swr_convert(swr_ctx, dst_data, max_dst_nb_samples, input_frame ? (const uint8_t**)input_frame->extended_data : NULL, input_frame ? input_frame->nb_samples : 0);
        
        if (dst_nb_samples > 0) {
            add_samples_to_fifo(dst_data, dst_nb_samples);
        }
        av_freep(&dst_data[0]);
        av_freep(&dst_data);
    };
    
    // --- 7. 主处理循环 ---
    while (av_read_frame(input_format_ctx, input_packet) == 0) {
        if (input_packet->stream_index == audio_stream_index) {
            ret = avcodec_send_packet(decoder_ctx, input_packet);
            if (ret < 0) log_error("avcodec_send_packet", ret);
            
            while (true) {
                ret = avcodec_receive_frame(decoder_ctx, decoded_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    log_error("avcodec_receive_frame", ret);
                }
                resample_and_store(decoded_frame);
                process_fifo();
            }
        }
        av_packet_unref(input_packet);
    }

    // --- 8. 刷新(Flush)所有组件的缓冲区 ---
    
    // 刷新解码器
    avcodec_send_packet(decoder_ctx, NULL);
    while (true) {
        ret = avcodec_receive_frame(decoder_ctx, decoded_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            log_error("avcodec_receive_frame (flushing)", ret);
        }
        resample_and_store(decoded_frame);
        process_fifo();
    }
    
    // 刷新重采样器
    resample_and_store(nullptr);
    process_fifo();

    // 处理FIFO中最后的、可能不足一帧的数据
    if (av_audio_fifo_size(fifo) > 0) {
        int remaining = av_audio_fifo_size(fifo);
        av_audio_fifo_read(fifo, (void**)output_frame->data, remaining);
        // 用静音填充到满一帧
        av_samples_set_silence(&output_frame->data[0], remaining, ENCODER_FRAME_SIZE - remaining, av_get_channel_layout_nb_channels(TARGET_CH_LAYOUT), TARGET_SAMPLE_FMT);
        output_frame->nb_samples = ENCODER_FRAME_SIZE;
        write_output_frame(output_frame);
    }

    // 刷新编码器
    write_output_frame(nullptr);

    // --- 9. 清理 ---
    fclose(outfile);
    av_audio_fifo_free(fifo);
    swr_free(&swr_ctx);
    av_frame_free(&decoded_frame);
    av_frame_free(&output_frame);
    av_packet_free(&input_packet);
    av_packet_free(&output_packet);
    avcodec_free_context(&decoder_ctx);
    avcodec_free_context(&encoder_ctx);
    avformat_close_input(&input_format_ctx);

    std::cout << "Robust conversion with loop fix finished successfully!" << std::endl;
}

int main(int argc, char* argv[]) {
    // ... main函数与之前相同 ...
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <output_file.opus>" << std::endl;
        return 1;
    }
    try {
        convert_to_opus(argv[1], argv[2]);
    } catch (const std::runtime_error& e) {
        std::cerr << "An error occurred: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
