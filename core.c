#include "core.h"

// declaration
void global_init();
void packet_queue_init(PacketQueue *pkt_q);
int packet_queue_put(PacketQueue *pkt_q, AVPacket *pkt);
int packet_queue_get(PacketQueue *pkt_q, AVPacket *pkt, int block);
void stream_open(const char *filename);
static void dump_metadata(void *ctx, AVDictionary *m, const char *indent);


int read_thread_abord = 0;

int no_more_data_in_the_queue = 0; // if there are no more data in the queue, we think the audio is stopping

static AudioState *global_audio_state_ctx = NULL;
static char* loaded_filename = NULL;
AVPacket flush_pkt;

static int g_ffmpeg_global_inited = false;

void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;
    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1)
    {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_free(pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destory(PacketQueue *q)
{
    packet_queue_flush(q);
    if (q->mutex)
        SDL_DestroyMutex(q->mutex);
    if (q->cond)
        SDL_DestroyCond(q->cond);
}

void global_init()
{
    if (g_ffmpeg_global_inited)
        return;

    // Init SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = (unsigned char *)"FLUSH";

    g_ffmpeg_global_inited = true;
}

static int audio_resampling(AVCodecContext *audio_decode_ctx,
                            AVFrame *audio_decode_frame,
                            enum AVSampleFormat out_sample_fmt,
                            int out_channels,
                            int out_sample_rate,
                            uint8_t *out_buf)
{
    SwrContext *swr_ctx = NULL;
    int ret = 0;
    int64_t in_channel_layout = audio_decode_ctx->channel_layout;
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    int out_nb_channels = 0;
    int out_linesize = 0;
    int in_nb_samples = 0;
    int out_nb_samples = 0;
    int max_out_nb_samples = 0;
    uint8_t **resampled_data = NULL;
    int resampled_data_size = 0;

    swr_ctx = swr_alloc();
    if (!swr_ctx)
    {
        av_log(NULL, AV_LOG_ERROR, "swr_alloc error\n");
        return -1;
    }

    in_channel_layout = (audio_decode_ctx->channels ==
                         av_get_channel_layout_nb_channels(audio_decode_ctx->channel_layout))
                            ? audio_decode_ctx->channel_layout
                            : av_get_default_channel_layout(audio_decode_ctx->channels);
    if (in_channel_layout <= 0)
    {
        av_log(NULL, AV_LOG_ERROR, "in_channel_layout error\n");
        return -1;
    }

    if (out_channels == 1)
    {
        out_channel_layout = AV_CH_LAYOUT_MONO;
    }
    else if (out_channels == 2)
    {
        out_channel_layout = AV_CH_LAYOUT_STEREO;
    }
    else
    {
        out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }

    in_nb_samples = audio_decode_frame->nb_samples;
    if (in_nb_samples <= 0)
    {
        av_log(NULL, AV_LOG_ERROR, "in_nb_samples error\n");
        return -1;
    }

    av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", audio_decode_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", audio_decode_ctx->sample_fmt, 0);

    av_opt_set_int(swr_ctx, "out_channel_layout", out_channel_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);

    if ((ret = swr_init(swr_ctx)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to initialize the resampling context\n");
        return -1;
    }

    max_out_nb_samples = out_nb_samples = av_rescale_rnd(in_nb_samples,
                                                         out_sample_rate,
                                                         audio_decode_ctx->sample_rate,
                                                         AV_ROUND_UP);

    if (max_out_nb_samples <= 0)
    {
        av_log(NULL, AV_LOG_ERROR, "av_rescale_rnd error\n");
        return -1;
    }

    out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);

    ret = av_samples_alloc_array_and_samples(&resampled_data, &out_linesize, out_nb_channels, out_nb_samples, out_sample_fmt, 0);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "av_samples_alloc_array_and_samples error\n");
        return -1;
    }

    out_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, audio_decode_ctx->sample_rate) + in_nb_samples,
                                    out_sample_rate, audio_decode_ctx->sample_rate, AV_ROUND_UP);
    if (out_nb_samples <= 0)
    {
        av_log(NULL, AV_LOG_ERROR, "av_rescale_rnd error\n");
        return -1;
    }

    if (out_nb_samples > max_out_nb_samples)
    {
        av_free(resampled_data[0]);
        ret = av_samples_alloc(resampled_data, &out_linesize, out_nb_channels, out_nb_samples, out_sample_fmt, 1);
        max_out_nb_samples = out_nb_samples;
    }

    if (swr_ctx)
    {
        ret = swr_convert(swr_ctx, resampled_data, out_nb_samples,
                          (const uint8_t **)audio_decode_frame->data, audio_decode_frame->nb_samples);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "swr_convert_error error\n");
            return -1;
        }

        resampled_data_size = av_samples_get_buffer_size(&out_linesize, out_nb_channels, ret, out_sample_fmt, 1);
        if (resampled_data_size < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size error\n");
            return -1;
        }
    }
    else
    {
        av_log(NULL, AV_LOG_ERROR, "swr_ctx null error\n");
        return -1;
    }

    memcpy(out_buf, resampled_data[0], resampled_data_size);

    if (resampled_data)
    {
        av_freep(&resampled_data[0]);
    }
    av_freep(&resampled_data);
    resampled_data = NULL;

    if (swr_ctx)
    {
        swr_free(&swr_ctx);
    }
    return resampled_data_size;
}

int audio_decode_frame(AudioState *is, uint8_t *audio_buf, int buf_size)
{
    AVPacket *pkt = &is->audio_pkt;

    int len1, data_size = 0;
    static int pkt_ready = 0;

    for (;;)
    {
        while (pkt_ready)
        {
            int got_frame = 0;

            AVFrame *frame = &is->audio_frame;
            av_frame_unref(frame);

            // get decoded output data from decoder
            int ret = avcodec_receive_frame(is->audio_codec_ctx, frame);

            // check and entire audio frame was decoded
            if (ret == 0)
            {
                got_frame = 1;
            }

            // check the decoder needs more AVPackets to be sent
            if (ret == AVERROR(EAGAIN))
            {
                ret = 0;
            }

            if (ret == 0)
            {
                // give the decoder raw compressed data in an AVPacket
                ret = avcodec_send_packet(is->audio_codec_ctx, pkt);
            }
            // check the decoder needs more AVPackets to be sent
            if (ret == AVERROR(EAGAIN))
            {
                ret = 0;
            }
            else if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "avcodec_receive_frame decoding error\n");
                return -1;
            }
            else
            {
                // mark pkt consumed, reset pkt status
                pkt_ready = 0;
            }
            if (!got_frame)
            {
                continue;
            }
            data_size = 0;

            if (is->audio_codec_ctx->sample_fmt == AV_SAMPLE_FMT_S16)
            {
                data_size = av_samples_get_buffer_size(NULL,
                                                       is->audio_codec_ctx->channels,
                                                       frame->nb_samples,
                                                       is->audio_codec_ctx->sample_fmt,
                                                       1);
                assert(data_size <= buf_size);
                memcpy(is->audio_buf, frame->data[0], data_size);
            }
            else
            {
                // resamplling and copy buf to audio_buf
                data_size = audio_resampling(is->audio_codec_ctx, &is->audio_frame, AV_SAMPLE_FMT_S16, is->audio_frame.channels, is->audio_frame.sample_rate, audio_buf);
                assert(data_size <= buf_size);
            }

            if (data_size <= 0)
            {
                // No data yet, get more frames
                av_log(NULL, AV_LOG_WARNING, "no data yet continue\n");
                continue;
            }
            int n = 2 * is->audio_frame.channels;
            is->audio_clock += (double)data_size / (double)(n * is->audio_frame.sample_rate);
            // We have data, return it and come back for more later
            return data_size;
        }

        if (pkt->data)
        {
            av_packet_unref(pkt);
        }

        if (is->quit)
        {
            return -1;
        }

        if (packet_queue_get(&is->audio_queue, pkt, 1) < 0)
        {
            return -1;
        }

        if (pkt->data == flush_pkt.data)
        {
            avcodec_flush_buffers(is->audio_codec_ctx);
            continue;
        }

        // get new pkt
        pkt_ready = 1;
        // if update, update the audio clock w/pts
        if (pkt->pts != AV_NOPTS_VALUE)
        {
            is->audio_clock = av_q2d(is->audio_codec_ctx->pkt_timebase) * pkt->pts;
        }
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
    AudioState *is = (AudioState *)userdata;
    int len1, audio_size;

    while (len > 0)
    {
        // if audio_buf_index >= is->audio_buf_size, there is no buf left
        // else
        // 如果上次 callback 还有剩余的 packet 未拷贝，则此时 buf_index < buf_size
        // 会先将剩余的 packet 拷贝到 stream（此时 buf_index += (buf_size-buf_index)
        // 如果还未填满 stream，则会再次 decode frame
        // check how much audio is left to writes
        if (is->audio_buf_index >= is->audio_buf_size)
        {
            // We have already sent all our data; get more */
            audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));
            if (audio_size < 0)
            {
                // If error, output silence
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE;
                memset(is->audio_buf, 0, is->audio_buf_size);
            }
            else
            {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_volume == SDL_MIX_MAXVOLUME)
        {
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        }
        else
        {
            memset(stream, is->silence_buf[0], len1);
            if (!is->muted)
                SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

static int audio_open(AudioState *arg)
{
    AudioState *is = arg;
    SDL_AudioSpec wanted_spec, spec;

    // Set audio settings from codec info
    wanted_spec.freq = is->audio_codec_ctx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = is->audio_codec_ctx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = is;

    if (is->audio_opend)
    {
        SDL_CloseAudio();
    }
    if (SDL_OpenAudio(&wanted_spec, &spec) < 0)
    {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }
    is->audio_opend = 1;

    SDL_PauseAudio(0);
    return 0;
}

static int audio_close()
{
    SDL_PauseAudio(1);
    return 0;
}

static int read_thread(void *arg)
{
    AudioState *is = (AudioState *)arg;
    AVPacket packet;

start:
    // prepare clean stuff
    is->audio_buf_index = 0;
    is->audio_buf_size = 0;
    is->audio_pkt_size = 0;
    // clean work
    if (is->audio_codec_ctx_orig)
    {
        avcodec_close(is->audio_codec_ctx_orig);
        is->audio_codec_ctx_orig = NULL;
    }
    if (is->audio_codec_ctx)
    {
        avcodec_close(is->audio_codec_ctx);
        is->audio_codec_ctx = NULL;
    }
    if (is->format_ctx)
    {
        avformat_close_input(&is->format_ctx);
        is->format_ctx = NULL;
    }

    // Open audio file
    if (avformat_open_input(&is->format_ctx, loaded_filename, NULL, NULL) != 0)
    {
        av_log(NULL, AV_LOG_WARNING, "avformat_open_input Failed: %s\n", loaded_filename);
        is->open_error_occurred = 1;
        SDL_LockMutex(is->invalid_file_mux);
        while (!is->new_file_set) {
            av_log(NULL, AV_LOG_WARNING, "avformat_open_input Failed, waiting for cond.\n");
            SDL_CondWait(is->invalid_file_cond, is->invalid_file_mux);
        }
        av_log(NULL, AV_LOG_INFO, "get new file set condition.\n");
        is->new_file_set = 0;
        SDL_UnlockMutex(is->invalid_file_mux);
        if (is->quit) {
            // quit
            return 0;
        }
        goto start;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(is->format_ctx, NULL) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "avformat find_stream Failed\n");
        return -1;
    }

    is->audio_stream_index = -1;
    int i;
    for (i = 0; i < is->format_ctx->nb_streams; i++)
    {
        if (is->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && is->audio_stream_index < 0)
        {
            is->audio_stream_index = i;
        }
    }
    if (is->audio_stream_index == -1)
    {
        av_log(NULL, AV_LOG_ERROR, "audio_stream_index == -1, return -1;\n");
        return -1;
    }

    if (is->format_ctx->duration != AV_NOPTS_VALUE)
    {
        int secs;
        int64_t duration = is->format_ctx->duration + (is->format_ctx->duration <= INT64_MAX - 5000 ? 5000 : 0);
        secs = duration / AV_TIME_BASE;
        is->duration = secs;
    }

    // do not free params or avformat_close_input will panic
    AVCodecParameters *params = is->format_ctx->streams[is->audio_stream_index]->codecpar;
    is->audio_codec_ctx_orig = avcodec_alloc_context3(NULL);
    int ret;
    ret = avcodec_parameters_to_context(is->audio_codec_ctx_orig, params);
    if (ret != 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Couldn't convert params to codec context\n");
        return -1;
    }
    is->audio_codec = avcodec_find_decoder(is->audio_codec_ctx_orig->codec_id);
    if (!is->audio_codec)
    {
        av_log(NULL, AV_LOG_ERROR, "Unsupported codec!\n");
        return -1;
    }

    // Copy context
    is->audio_codec_ctx = avcodec_alloc_context3(is->audio_codec);
    ret = avcodec_parameters_to_context(is->audio_codec_ctx, params);
    if (ret != 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Couldn't copy codec context\n");
        return -1;
    }

    // Open audio device
    audio_open(is);

    if (avcodec_open2(is->audio_codec_ctx, is->audio_codec, NULL) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "avcodec_open2 failed\n");
        return -1;
    }

    // Read frames and put to audio_queue
    for (;;)
    {
        if (is->quit)
        {
            break;
        }
        if (is->read_thread_abord)
        {
            is->read_thread_abord = 0;
            packet_queue_flush(&is->audio_queue);
            goto start;
        }
        // handle seek stuff
        if (is->seek_req)
        {
            int stream_index = -1;
            int64_t seek_target = is->seek_pos;

            if (is->audio_stream_index >= 0)
            {
                stream_index = is->audio_stream_index;
            }
            if (stream_index >= 0)
            {
                seek_target = av_rescale_q(seek_target, AV_TIME_BASE_Q,
                                           is->format_ctx->streams[stream_index]->time_base);
            }
            if (av_seek_frame(is->format_ctx, stream_index, seek_target,
                              is->seek_flags) < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "seek error\n");
            }
            else
            {
                if (is->audio_stream_index >= 0)
                {
                    packet_queue_flush(&is->audio_queue);
                    packet_queue_put(&is->audio_queue, &flush_pkt);
                }
            }
            is->seek_req = 0;
        }

        if (is->audio_queue.size > MAX_AUDIOQ_SIZE)
        {
            SDL_Delay(10);
            continue;
        }

        if (av_read_frame(is->format_ctx, &packet) < 0)
        {
            if (!is->read_thread_abord)
            {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            }
            else
            {
                is->read_thread_abord = 0;
                packet_queue_flush(&is->audio_queue);
                goto start;
            }
        }

        if (packet.stream_index == is->audio_stream_index)
        {
            packet_queue_put(&is->audio_queue, &packet);
        }
        else
        {
            av_packet_unref(&packet); // Free the packet
        }
    }
    return 0;
}

static void notify_read_thread_when_error_occurred(AudioState *is) 
{
    // handle open error
    if (is->open_error_occurred) {
        is->open_error_occurred = 0;
        SDL_LockMutex(is->invalid_file_mux);
        is->new_file_set = 1;
        SDL_CondSignal(is->invalid_file_cond);
        SDL_UnlockMutex(is->invalid_file_mux);
    }
}

static void stream_close(AudioState *is)
{
    is->quit = 1;
    // handle open error
    notify_read_thread_when_error_occurred(is);
    av_log(NULL, AV_LOG_WARNING, "wait for read_tid\n");
    SDL_WaitThread(is->read_tid, NULL);
    audio_close();
    if (is->audio_codec_ctx_orig)
    {
        avcodec_close(is->audio_codec_ctx_orig);
        is->audio_codec_ctx_orig = NULL;
    }
    if (is->audio_codec_ctx)
    {
        avcodec_close(is->audio_codec_ctx);
        is->audio_codec_ctx = NULL;
    }
    if (is->format_ctx)
    {
        avformat_close_input(&is->format_ctx);
        is->format_ctx = NULL;
    }
    if (is->invalid_file_mux)
        SDL_DestroyMutex(is->invalid_file_mux);
    if (is->invalid_file_cond)
        SDL_DestroyCond(is->invalid_file_cond);
    packet_queue_destory(&is->audio_queue);
    av_free(is);
}

void stream_open(const char *filename)
{
    AudioState *is;
    is = (AudioState *)av_mallocz(sizeof(AudioState));
    if (!is)
        return;
    loaded_filename = av_strdup(filename);
    packet_queue_init(&is->audio_queue);
    is->audio_volume = SDL_MIX_MAXVOLUME;
    is->muted = 0;
    is->invalid_file_mux = SDL_CreateMutex();
    is->invalid_file_cond = SDL_CreateCond();
    global_audio_state_ctx = is;

    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
    if (!is->read_tid)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        stream_close(is);
        return;
    }
    return;
}

void stream_seek(AudioState *is, int64_t pos, int flag)
{
    if (!is->seek_req)
    {
        is->seek_pos = pos;
        is->seek_flags = flag < 0 ? AVSEEK_FLAG_BACKWARD : 0;
        is->seek_req = 1;
    }
}

void player_destory(AudioState *is)
{
    if (global_audio_state_ctx)
    {
        stream_close(global_audio_state_ctx);
        av_log(NULL, AV_LOG_WARNING, "destroy_AudioState: force stream_close()\n");
        global_audio_state_ctx = NULL;
    }
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;
    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    no_more_data_in_the_queue = 0;
    // Send signal to queue get function
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

// if block set to True, this func will wait for SDL_CondSignal
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;)
    {

        if (global_audio_state_ctx->quit)
        {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1)
        {
            no_more_data_in_the_queue = 0;
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        }
        else if (!block)
        {
            ret = 0;
            break;
        }
        else
        {
            if (q->nb_packets <= 0)
            {
                no_more_data_in_the_queue = 1;
                ret = -1;
                break;
            }
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

void pause_audio()
{
    if (global_audio_state_ctx == NULL)
        return;
    AudioState *is = global_audio_state_ctx;
    is->paused = !is->paused;
    SDL_PauseAudio(is->paused);
}

void stop_audio()
{
    if (global_audio_state_ctx == NULL)
        return;
    AudioState *is = global_audio_state_ctx;
    if (!is->stopped)
    {
        is->stopped = !is->stopped;
        audio_close();
        packet_queue_flush(&is->audio_queue);
        is->duration = 0;
        is->audio_clock = 0;
    }
}

void set_volume(int volume)
{
    if (global_audio_state_ctx == NULL)
        return;
    if (volume < 0 || volume > 100)
        return;
    AudioState *is = global_audio_state_ctx;
    volume = (SDL_MIX_MAXVOLUME / 100) * volume;
    is->audio_volume = volume;
}

void seek_audio(double percent)
{
    if (global_audio_state_ctx == NULL)
        return;
    if (percent < 0 || percent > 100)
    {
        return;
    }
    int incr = 0;
    AudioState *is = global_audio_state_ctx;
    double seek_target = (double)is->duration * percent / 100.0;
    double current_pos = is->audio_clock;
    incr = seek_target > current_pos ? 1 : -1;
    stream_seek(global_audio_state_ctx, (int64_t)(seek_target * AV_TIME_BASE), incr);
}

void seek_audio_by_sec(int sec)
{
    if (global_audio_state_ctx == NULL)
        return;
    int incr = 0;
    incr = sec > 0 ? 1 : -1;
    AudioState *is = global_audio_state_ctx;
    double pos = is->audio_clock;
    pos += sec;
    int duration = is->duration;
    if (duration < pos || pos < 0)
    {
        return;
    }
    stream_seek(global_audio_state_ctx, (int64_t)(pos * AV_TIME_BASE), incr);
}

// return total audio length
int get_time_length()
{
    if (global_audio_state_ctx == NULL)
        return 0;
    int duration = 0;
    if (global_audio_state_ctx != NULL)
    {
        duration = global_audio_state_ctx->duration;
    }
    return duration;
}

// return current time pos
double get_current_time_pos()
{
    if (global_audio_state_ctx == NULL)
        return 0;
    double pos = 0;
    if (global_audio_state_ctx != NULL)
    {
        pos = global_audio_state_ctx->audio_clock;
    }
    return pos;
}

int is_stopping()
{
    return no_more_data_in_the_queue;
}

void free_player()
{
    if (global_audio_state_ctx != NULL)
    {
        player_destory(global_audio_state_ctx);
    }
}

int load_file(const char *filename)
{
    AudioState *is = NULL;
    if (global_audio_state_ctx == NULL)
    {
        global_init();
        stream_open(filename);
    }
    else
    {
        audio_close();
        no_more_data_in_the_queue = 0;
        AudioState *is = global_audio_state_ctx;
        loaded_filename = av_strdup(filename);
        is->read_thread_abord = 1;
        notify_read_thread_when_error_occurred(is);
    }
    return 0;
}
