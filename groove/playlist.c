/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "file.h"
#include "queue.h"
#include "buffer.h"

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <pthread.h>

struct GrooveSinkPrivate {
    struct GrooveSink externals;
    struct GrooveQueue *audioq;
    int audioq_size; // in bytes
    int min_audioq_size; // in bytes
};

struct SinkStack {
    struct GrooveSink *sink;
    struct SinkStack *next;
};

struct SinkMap {
    struct SinkStack *stack_head;
    AVFilterContext *aformat_ctx;
    AVFilterContext *abuffersink_ctx;
    struct SinkMap *next;
};

struct GroovePlaylistPrivate {
    struct GroovePlaylist externals;
    pthread_t thread_id;
    int abort_request;

    AVPacket audio_pkt_temp;
    AVFrame *in_frame;
    int paused;
    int last_paused;

    int in_sample_rate;
    uint64_t in_channel_layout;
    enum AVSampleFormat in_sample_fmt;
    AVRational in_time_base;

    char strbuf[512];
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer_ctx;
    AVFilterContext *volume_ctx;
    AVFilterContext *asplit_ctx;

    // this mutex applies to the variables in this block
    pthread_mutex_t decode_head_mutex;
    char decode_head_mutex_inited;
    // decode_thread waits on this cond when the decode_head is NULL
    pthread_cond_t decode_head_cond;
    char decode_head_cond_inited;
    // decode_thread waits on this cond when every sink is full
    // should also signal when the first sink is attached.
    pthread_cond_t sink_drain_cond;
    char sink_drain_cond_inited;
    // pointer to current playlist item being decoded
    struct GroovePlaylistItem *decode_head;
    // desired volume for the volume filter
    double volume;
    // set to 1 to trigger a rebuild
    int rebuild_filter_graph_flag;
    // map audio format to list of sinks
    // for each map entry, use the first sink in the stack as the example
    // of the audio format in that stack
    struct SinkMap *sink_map;
    int sink_map_count;

    // the value for volume that was used to construct the filter graph
    double filter_volume;

    // only touched by decode_thread, tells whether we have sent the end_of_q_sentinel
    int sent_end_of_q;

    struct GroovePlaylistItem *purge_item; // set temporarily
};

// this is used to tell the difference between a buffer underrun
// and the end of the playlist.
static struct GrooveBuffer *end_of_q_sentinel = NULL;

static int frame_size(const AVFrame *frame) {
    return av_get_channel_layout_nb_channels(frame->channel_layout) *
        av_get_bytes_per_sample(frame->format) *
        frame->nb_samples;
}

static struct GrooveBuffer * frame_to_groove_buffer(struct GroovePlaylist *playlist,
        struct GrooveSink *sink, AVFrame *frame)
{
    struct GrooveBufferPrivate *b = av_mallocz(sizeof(struct GrooveBufferPrivate));

    if (!b) {
        av_log(NULL, AV_LOG_ERROR, "unable to allocate buffer\n");
        return NULL;
    }

    struct GrooveBuffer *buffer = &b->externals;

    if (pthread_mutex_init(&b->mutex, NULL) != 0) {
        av_free(b);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex\n");
        return NULL;
    }

    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    struct GrooveFile *file = p->decode_head->file;

    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    buffer->item = p->decode_head;
    buffer->pos = f->audio_clock;

    buffer->data = frame->extended_data;
    buffer->frame_count = frame->nb_samples;
    buffer->format.channel_layout = frame->channel_layout;
    buffer->format.sample_fmt = frame->format;
    buffer->format.sample_rate = frame->sample_rate;
    buffer->size = frame_size(frame);

    b->frame = frame;

    return buffer;
}


// decode one audio packet and return its uncompressed size
static int audio_decode_frame(struct GroovePlaylist *playlist, struct GrooveFile *file) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    AVPacket *pkt = &f->audio_pkt;
    AVCodecContext *dec = f->audio_st->codec;

    AVPacket *pkt_temp = &p->audio_pkt_temp;
    *pkt_temp = *pkt;

    // update the audio clock with the pts if we can
    if (pkt->pts != AV_NOPTS_VALUE)
        f->audio_clock = av_q2d(f->audio_st->time_base) * pkt->pts;

    int max_data_size = 0;
    int len1, got_frame;
    int new_packet = 1;
    AVFrame *in_frame = p->in_frame;

    // NOTE: the audio packet can contain several frames
    while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
        new_packet = 0;

        len1 = avcodec_decode_audio4(dec, in_frame, &got_frame, pkt_temp);
        if (len1 < 0) {
            // if error, we skip the frame
            pkt_temp->size = 0;
            return -1;
        }

        pkt_temp->data += len1;
        pkt_temp->size -= len1;

        if (!got_frame) {
            // stop sending empty packets if the decoder is finished
            if (!pkt_temp->data && dec->codec->capabilities & CODEC_CAP_DELAY)
                return 0;
            continue;
        }

        // push the audio data from decoded frame into the filtergraph
        int err = av_buffersrc_write_frame(p->abuffer_ctx, in_frame);
        if (err < 0) {
            av_strerror(err, p->strbuf, sizeof(p->strbuf));
            av_log(NULL, AV_LOG_ERROR, "error writing frame to buffersrc: %s\n",
                    p->strbuf);
            return -1;
        }

        // for each data format in the sink map, pull filtered audio from its
        // buffersink, turn it into a GrooveBuffer and then increment the ref
        // count for each sink in that stack.
        struct SinkMap *map_item = p->sink_map;
        double clock_adjustment = 0;
        while (map_item) {
            struct GrooveSink *example_sink = map_item->stack_head->sink;
            int data_size = 0;
            for (;;) {
                AVFrame *oframe = av_frame_alloc();
                int err = example_sink->buffer_sample_count == 0 ?
                    av_buffersink_get_frame(map_item->abuffersink_ctx, oframe) :
                    av_buffersink_get_samples(map_item->abuffersink_ctx, oframe, example_sink->buffer_sample_count);
                if (err == AVERROR_EOF || err == AVERROR(EAGAIN)) {
                    av_frame_free(&oframe);
                    break;
                }
                if (err < 0) {
                    av_frame_free(&oframe);
                    av_log(NULL, AV_LOG_ERROR, "error reading buffer from buffersink\n");
                    return -1;
                }
                struct GrooveBuffer *buffer = frame_to_groove_buffer(playlist, example_sink, oframe);
                if (!buffer) {
                    av_frame_free(&oframe);
                    return -1;
                }
                data_size += buffer->size;
                struct SinkStack *stack_item = map_item->stack_head;
                // we hold this reference to avoid cleanups until at least this loop
                // is done and we call unref after it.
                groove_buffer_ref(buffer);
                while (stack_item) {
                    struct GrooveSink *sink = stack_item->sink;
                    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;
                    // as soon as we call groove_queue_put, this buffer could be unref'd.
                    // so we ref before putting it in the queue, and unref if it failed.
                    groove_buffer_ref(buffer);
                    if (groove_queue_put(s->audioq, buffer) < 0) {
                        av_log(NULL, AV_LOG_ERROR, "unable to put buffer in queue\n");
                        groove_buffer_unref(buffer);
                    }
                    stack_item = stack_item->next;
                }
                groove_buffer_unref(buffer);
            }
            if (data_size > max_data_size) {
                max_data_size = data_size;
                clock_adjustment = data_size / (double)example_sink->bytes_per_sec;
            }
            map_item = map_item->next;
        }

        // if no pts, then estimate it
        if (pkt->pts == AV_NOPTS_VALUE)
            f->audio_clock += clock_adjustment;
        return max_data_size;
    }
    return max_data_size;
}

// abuffer -> volume -> asplit for each audio format
//                     -> aformat -> abuffersink
static int init_filter_graph(struct GroovePlaylist *playlist, struct GrooveFile *file) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    // destruct old graph
    avfilter_graph_free(&p->filter_graph);

    // create new graph
    p->filter_graph = avfilter_graph_alloc();
    if (!p->filter_graph) {
        av_log(NULL, AV_LOG_ERROR, "unable to create filter graph: out of memory\n");
        return -1;
    }

    AVFilter *abuffer = avfilter_get_by_name("abuffer");
    AVFilter *volume = avfilter_get_by_name("volume");
    AVFilter *asplit = avfilter_get_by_name("asplit");
    AVFilter *aformat = avfilter_get_by_name("aformat");
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    int err;
    // create abuffer filter
    AVCodecContext *avctx = f->audio_st->codec;
    AVRational time_base = f->audio_st->time_base;
    snprintf(p->strbuf, sizeof(p->strbuf),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64, 
            time_base.num, time_base.den, avctx->sample_rate,
            av_get_sample_fmt_name(avctx->sample_fmt),
            avctx->channel_layout);
    av_log(NULL, AV_LOG_INFO, "abuffer: %s\n", p->strbuf);
    // save these values so we can compare later and check
    // whether we have to reconstruct the graph
    p->in_sample_rate = avctx->sample_rate;
    p->in_channel_layout = avctx->channel_layout;
    p->in_sample_fmt = avctx->sample_fmt;
    p->in_time_base = time_base;
    err = avfilter_graph_create_filter(&p->abuffer_ctx, abuffer,
            NULL, p->strbuf, NULL, p->filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error initializing abuffer filter\n");
        return err;
    }
    // as we create filters, this points the next source to link to
    AVFilterContext *audio_src_ctx = p->abuffer_ctx;

    // save the volume value so we can compare later and check
    // whether we have to reconstruct the graph
    p->filter_volume = p->volume;
    // if volume is not equal to 1.0, create volume filter
    double vol = p->volume;
    if (vol > 1.0) vol = 1.0;
    if (vol < 0.0) vol = 0.0;
    if (vol == 1.0) {
        p->volume_ctx = NULL;
    } else {
        snprintf(p->strbuf, sizeof(p->strbuf), "volume=%f", vol);
        av_log(NULL, AV_LOG_INFO, "volume: %s\n", p->strbuf);
        err = avfilter_graph_create_filter(&p->volume_ctx, volume, NULL,
                p->strbuf, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "error initializing volume filter\n");
            return err;
        }
        err = avfilter_link(audio_src_ctx, 0, p->volume_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link filters\n");
            return err;
        }
        audio_src_ctx = p->volume_ctx;
    }

    // if only one sink, no need for asplit
    if (p->sink_map_count < 2) {
        p->asplit_ctx = NULL;
    } else {
        snprintf(p->strbuf, sizeof(p->strbuf), "%d", p->sink_map_count);
        av_log(NULL, AV_LOG_INFO, "asplit: %s\n", p->strbuf);
        err = avfilter_graph_create_filter(&p->asplit_ctx, asplit,
                NULL, p->strbuf, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to create asplit filter\n");
            return err;
        }
        err = avfilter_link(audio_src_ctx, 0, p->asplit_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link to asplit\n");
            return err;
        }
        audio_src_ctx = p->asplit_ctx;
    }

    // for each audio format, create aformat and abuffersink filters
    struct SinkMap *map_item = p->sink_map;
    int pad_index = 0;
    while (map_item) {
        struct GrooveSink *example_sink = map_item->stack_head->sink;
        struct GrooveAudioFormat *audio_format = &example_sink->audio_format;

        AVFilterContext *inner_audio_src_ctx = audio_src_ctx;
        if (example_sink->disable_resample) {
            map_item->aformat_ctx = NULL;
        } else {
            // create aformat filter
            snprintf(p->strbuf, sizeof(p->strbuf),
                    "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%"PRIx64,
                    av_get_sample_fmt_name((enum AVSampleFormat)audio_format->sample_fmt),
                    audio_format->sample_rate, audio_format->channel_layout);
            av_log(NULL, AV_LOG_INFO, "aformat: %s\n", p->strbuf);
            err = avfilter_graph_create_filter(&map_item->aformat_ctx, aformat,
                    NULL, p->strbuf, NULL, p->filter_graph);
            if (err < 0) {
                av_strerror(err, p->strbuf, sizeof(p->strbuf));
                av_log(NULL, AV_LOG_ERROR, "unable to create aformat filter: %s\n",
                        p->strbuf);
                return err;
            }
            err = avfilter_link(audio_src_ctx, pad_index, map_item->aformat_ctx, 0);
            if (err < 0) {
                av_log(NULL, AV_LOG_ERROR, "unable to link filters\n");
                return err;
            }
            inner_audio_src_ctx = map_item->aformat_ctx;
        }

        // create abuffersink filter
        err = avfilter_graph_create_filter(&map_item->abuffersink_ctx, abuffersink,
                NULL, NULL, NULL, p->filter_graph);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to create abuffersink filter\n");
            return err;
        }
        err = avfilter_link(inner_audio_src_ctx, 0, map_item->abuffersink_ctx, 0);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "unable to link filters\n");
            return err;
        }

        pad_index += 1;
        map_item = map_item->next;
    }

    err = avfilter_graph_config(p->filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error configuring the filter graph\n");
        return err;
    }

    p->rebuild_filter_graph_flag = 0;

    return 0;
}

static int maybe_init_filter_graph(struct GroovePlaylist *playlist, struct GrooveFile *file) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    AVCodecContext *avctx = f->audio_st->codec;
    AVRational time_base = f->audio_st->time_base;

    // if the input format stuff has changed, then we need to re-build the graph
    if (!p->filter_graph || p->rebuild_filter_graph_flag ||
        p->in_sample_rate != avctx->sample_rate ||
        p->in_channel_layout != avctx->channel_layout ||
        p->in_sample_fmt != avctx->sample_fmt ||
        p->in_time_base.num != time_base.num ||
        p->in_time_base.den != time_base.den ||
        p->volume != p->filter_volume)
    {
        return init_filter_graph(playlist, file);
    }

    return 0;
}

static int every_sink(struct GroovePlaylist *playlist, int (*func)(struct GrooveSink *), int default_value) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    struct SinkMap *map_item = p->sink_map;
    while (map_item) {
        struct SinkStack *stack_item = map_item->stack_head;
        while (stack_item) {
            struct GrooveSink *sink = stack_item->sink;
            int value = func(sink);
            if (value != default_value)
                return value;
            stack_item = stack_item->next;
        }
        map_item = map_item->next;
    }
    return default_value;
}

static int sink_is_full(struct GrooveSink *sink) {
    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;
    return s->audioq_size >= s->min_audioq_size;
}

static int every_sink_full(struct GroovePlaylist *playlist) {
    return every_sink(playlist, sink_is_full, 1);
}

static int sink_signal_end(struct GrooveSink *sink) {
    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;
    groove_queue_put(s->audioq, end_of_q_sentinel);
    return 0;
}

static void every_sink_signal_end(struct GroovePlaylist *playlist) {
    every_sink(playlist, sink_signal_end, 0);
}

static int sink_flush(struct GrooveSink *sink) {
    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;

    groove_queue_flush(s->audioq);
    if (sink->flush)
        sink->flush(sink);

    return 0;
}

static void every_sink_flush(struct GroovePlaylist *playlist) {
    every_sink(playlist, sink_flush, 0);
}

static int decode_one_frame(struct GroovePlaylist *playlist, struct GrooveFile *file) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
    AVPacket *pkt = &f->audio_pkt;

    // might need to rebuild the filter graph if certain things changed
    if (maybe_init_filter_graph(playlist, file) < 0)
        return -1;

    // abort_request is set if we are destroying the file
    if (f->abort_request)
        return -1;

    // handle pause requests
    // only read p->paused once so that we don't need a mutex
    int paused = p->paused;
    if (paused != p->last_paused) {
        p->last_paused = paused;
        if (paused) {
            av_read_pause(f->ic);
        } else {
            av_read_play(f->ic);
        }
    }

    // handle seek requests
    pthread_mutex_lock(&f->seek_mutex);
    if (f->seek_pos >= 0) {
        if (av_seek_frame(f->ic, f->audio_stream_index, f->seek_pos, 0) < 0) {
            av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", f->ic->filename);
        } else if (f->seek_flush) {
            every_sink_flush(playlist);
        }
        avcodec_flush_buffers(f->audio_st->codec);
        f->seek_pos = -1;
        f->eof = 0;
    }
    pthread_mutex_unlock(&f->seek_mutex);

    if (f->eof) {
        if (f->audio_st->codec->codec->capabilities & CODEC_CAP_DELAY) {
            av_init_packet(pkt);
            pkt->data = NULL;
            pkt->size = 0;
            pkt->stream_index = f->audio_stream_index;
            if (audio_decode_frame(playlist, file) > 0) {
                // keep flushing
                return 0;
            }
        }
        // this file is complete. move on
        return -1;
    }
    int err = av_read_frame(f->ic, pkt);
    if (err < 0) {
        // treat all errors as EOF, but log non-EOF errors.
        if (err != AVERROR_EOF) {
            av_log(NULL, AV_LOG_WARNING, "error reading frames\n");
        }
        f->eof = 1;
        return 0;
    }
    if (pkt->stream_index != f->audio_stream_index) {
        // we're only interested in the One True Audio Stream
        av_free_packet(pkt);
        return 0;
    }
    audio_decode_frame(playlist, file);
    av_free_packet(pkt);
    return 0;
}

static void audioq_put(struct GrooveQueue *queue, void *obj) {
    struct GrooveBuffer *buffer = obj;
    if (buffer == end_of_q_sentinel)
        return;
    struct GrooveSinkPrivate *s = queue->context;
    s->audioq_size += buffer->size;
}

static void audioq_get(struct GrooveQueue *queue, void *obj) {
    struct GrooveBuffer *buffer = obj;
    if (buffer == end_of_q_sentinel)
        return;
    struct GrooveSink *sink = queue->context;
    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;
    s->audioq_size -= buffer->size;

    struct GroovePlaylist *playlist = sink->playlist;
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    if (s->audioq_size < s->min_audioq_size)
        pthread_cond_signal(&p->sink_drain_cond);
}

static void audioq_cleanup(struct GrooveQueue *queue, void *obj) {
    struct GrooveBuffer *buffer = obj;
    if (buffer == end_of_q_sentinel)
        return;
    struct GrooveSink *sink = queue->context;
    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;
    s->audioq_size -= buffer->size;
    groove_buffer_unref(buffer);
}

static int audioq_purge(struct GrooveQueue *queue, void *obj) {
    struct GrooveBuffer *buffer = obj;
    if (buffer == end_of_q_sentinel)
        return 0;
    struct GrooveSink *sink = queue->context;
    struct GroovePlaylist *playlist = sink->playlist;
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    struct GroovePlaylistItem *item = p->purge_item;
    return buffer->item == item;
}

// this thread is responsible for decoding and inserting buffers of decoded
// audio into each sink
static void *decode_thread(void *arg) {
    struct GroovePlaylistPrivate *p = arg;
    struct GroovePlaylist *playlist = &p->externals;

    while (!p->abort_request) {
        pthread_mutex_lock(&p->decode_head_mutex);

        // if we don't have anything to decode, wait until we do
        if (!p->decode_head) {
            if (!p->sent_end_of_q) {
                every_sink_signal_end(playlist);
                p->sent_end_of_q = 1;
            }
            pthread_cond_wait(&p->decode_head_cond, &p->decode_head_mutex);
            pthread_mutex_unlock(&p->decode_head_mutex);
            continue;
        }
        p->sent_end_of_q = 0;

        // if all sinks are filled up, no need to read more
        if (every_sink_full(playlist)) {
            pthread_cond_wait(&p->sink_drain_cond, &p->decode_head_mutex);
            pthread_mutex_unlock(&p->decode_head_mutex);
            continue;
        }

        struct GrooveFile *file = p->decode_head->file;

        p->volume = p->decode_head->gain * playlist->volume;

        if (decode_one_frame(playlist, file) < 0) {
            p->decode_head = p->decode_head->next;
            // seek to beginning of next song
            if (p->decode_head) {
                struct GrooveFile *next_file = p->decode_head->file;
                struct GrooveFilePrivate *next_f = (struct GrooveFilePrivate *) next_file;
                pthread_mutex_lock(&next_f->seek_mutex);
                next_f->seek_pos = 0;
                next_f->seek_flush = 0;
                pthread_mutex_unlock(&next_f->seek_mutex);
            }
        }

        pthread_mutex_unlock(&p->decode_head_mutex);
    }

    return NULL;
}

static int sink_formats_equal(const struct GrooveSink *a, const struct GrooveSink *b) {
    if (a->buffer_sample_count != b->buffer_sample_count)
        return 0;
    if (a->disable_resample) {
        return b->disable_resample;
    } else {
        return b->disable_resample ? 0 :
            (a->audio_format.sample_rate == b->audio_format.sample_rate &&
            a->audio_format.channel_layout == b->audio_format.channel_layout &&
            a->audio_format.sample_fmt == b->audio_format.sample_fmt);
    }
}

static int remove_sink_from_map(struct GrooveSink *sink) {
    struct GroovePlaylist *playlist = sink->playlist;
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    struct SinkMap *map_item = p->sink_map;
    struct SinkMap *prev_map_item = NULL;
    while (map_item) {
        struct SinkMap *next_map_item = map_item->next;
        struct SinkStack *stack_item = map_item->stack_head;
        struct SinkStack *prev_stack_item = NULL;
        while (stack_item) {
            struct SinkStack *next_stack_item = stack_item->next;
            struct GrooveSink *item_sink = stack_item->sink;
            if (item_sink == sink) {
                av_free(stack_item);
                if (prev_stack_item) {
                    prev_stack_item->next = next_stack_item;
                } else if (next_stack_item) {
                    map_item->stack_head = next_stack_item;
                } else {
                    // the stack is empty; delete the map item
                    av_free(map_item);
                    p->sink_map_count -= 1;
                    if (prev_map_item) {
                        prev_map_item->next = next_map_item;
                    } else {
                        p->sink_map = next_map_item;
                    }
                }
                return 0;
            }

            prev_stack_item = stack_item;
            stack_item = next_stack_item;
        }
        prev_map_item = map_item;
        map_item = next_map_item;
    }

    return -1;
}

static int add_sink_to_map(struct GroovePlaylist *playlist, struct GrooveSink *sink) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    struct SinkStack *stack_entry = av_mallocz(sizeof(struct SinkStack));
    stack_entry->sink = sink;

    if (!stack_entry)
        return -1;

    struct SinkMap *map_item = p->sink_map;
    while (map_item) {
        // if our sink matches the example sink from this map entry,
        // push our sink onto the stack and we're done
        struct GrooveSink *example_sink = map_item->stack_head->sink;
        if (sink_formats_equal(example_sink, sink)) {
            stack_entry->next = map_item->stack_head;
            map_item->stack_head = stack_entry;
            return 0;
        }
        map_item = map_item->next;
    }
    // we did not find somewhere to put it, so push it onto the stack.
    struct SinkMap *map_entry = av_mallocz(sizeof(struct SinkMap));
    map_entry->stack_head = stack_entry;
    if (!map_entry) {
        av_free(stack_entry);
        return -1;
    }
    if (p->sink_map) {
        map_entry->next = p->sink_map;
        p->sink_map = map_entry;
    } else {
        p->sink_map = map_entry;
    }
    p->sink_map_count += 1;
    return 0;
}

int groove_sink_detach(struct GrooveSink *sink) {
    struct GroovePlaylist *playlist = sink->playlist;

    if (!playlist)
        return -1;

    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;

    if (s->audioq) {
        groove_queue_abort(s->audioq);
        groove_queue_flush(s->audioq);
    }

    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    pthread_mutex_lock(&p->decode_head_mutex);
    int err = remove_sink_from_map(sink);
    pthread_mutex_unlock(&p->decode_head_mutex);

    sink->playlist = NULL;

    return err;
}

int groove_sink_attach(struct GrooveSink *sink, struct GroovePlaylist *playlist) {
    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;

    // cache computed audio format stuff
    int channel_count = av_get_channel_layout_nb_channels(sink->audio_format.channel_layout);
    int bytes_per_frame = channel_count *
        av_get_bytes_per_sample((enum AVSampleFormat)sink->audio_format.sample_fmt);
    sink->bytes_per_sec = bytes_per_frame * sink->audio_format.sample_rate;

    s->min_audioq_size = sink->buffer_size * bytes_per_frame;
    av_log(NULL, AV_LOG_INFO, "audio queue size: %d\n", s->min_audioq_size);

    // add the sink to the entry that matches its audio format
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    pthread_mutex_lock(&p->decode_head_mutex);
    int err = add_sink_to_map(playlist, sink);
    pthread_cond_signal(&p->sink_drain_cond);
    pthread_mutex_unlock(&p->decode_head_mutex);

    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to attach device: out of memory\n");
        return err;
    }

    // in case we've called abort on the queue, reset
    groove_queue_reset(s->audioq);

    sink->playlist = playlist;

    return 0;
}

int groove_sink_buffer_get(struct GrooveSink *sink, struct GrooveBuffer **buffer, int block) {
    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;

    if (groove_queue_get(s->audioq, (void**)buffer, block) == 1) {
        if (*buffer == end_of_q_sentinel) {
            *buffer = NULL;
            return GROOVE_BUFFER_END;
        } else {
            return GROOVE_BUFFER_YES;
        }
    } else {
        *buffer = NULL;
        return GROOVE_BUFFER_NO;
    }
}

int groove_sink_buffer_peek(struct GrooveSink *sink, int block) {
    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;
    return groove_queue_peek(s->audioq, block);
}

struct GroovePlaylist * groove_playlist_create(void) {
    struct GroovePlaylistPrivate *p = av_mallocz(sizeof(struct GroovePlaylistPrivate));
    if (!p) {
        av_log(NULL, AV_LOG_ERROR, "unable to allocate playlist\n");
        return NULL;
    }
    struct GroovePlaylist *playlist = &p->externals;

    // the one that the playlist can read
    playlist->volume = 1.0;
    // the other volume multiplied by the playlist item's gain
    p->volume = 1.0;

    // set this flag to true so that a race condition does not send the end of
    // queue sentinel early.
    p->sent_end_of_q = 1;

    if (pthread_mutex_init(&p->decode_head_mutex, NULL) != 0) {
        groove_playlist_destroy(playlist);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate mutex\n");
        return NULL;
    }
    p->decode_head_mutex_inited = 1;

    if (pthread_cond_init(&p->decode_head_cond, NULL) != 0) {
        groove_playlist_destroy(playlist);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate decode head mutex condition\n");
        return NULL;
    }
    p->decode_head_cond_inited = 1;

    if (pthread_cond_init(&p->sink_drain_cond, NULL) != 0) {
        groove_playlist_destroy(playlist);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate sink drain mutex condition\n");
        return NULL;
    }
    p->sink_drain_cond_inited = 1;

    p->in_frame = av_frame_alloc();

    if (!p->in_frame) {
        groove_playlist_destroy(playlist);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate frame\n");
        return NULL;
    }

    if (pthread_create(&p->thread_id, NULL, decode_thread, playlist) != 0) {
        groove_playlist_destroy(playlist);
        av_log(NULL, AV_LOG_ERROR, "unable to create playlist thread\n");
        return NULL;
    }

    return playlist;
}

void groove_playlist_destroy(struct GroovePlaylist *playlist) {
    groove_playlist_clear(playlist);

    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    // wait for decode thread to finish
    p->abort_request = 1;
    pthread_cond_signal(&p->decode_head_cond);
    pthread_cond_signal(&p->sink_drain_cond);
    pthread_join(p->thread_id, NULL);

    every_sink(playlist, groove_sink_detach, 0);

    avfilter_graph_free(&p->filter_graph);
    av_frame_free(&p->in_frame);

    if (p->decode_head_mutex_inited)
        pthread_mutex_destroy(&p->decode_head_mutex);

    if (p->decode_head_cond_inited)
        pthread_cond_destroy(&p->decode_head_cond);

    if (p->sink_drain_cond_inited)
        pthread_cond_destroy(&p->sink_drain_cond);

    av_free(p);
}

void groove_playlist_play(struct GroovePlaylist *playlist) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    // no mutex needed for this boolean flag
    p->paused = 0;
}

void groove_playlist_pause(struct GroovePlaylist *playlist) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    // no mutex needed for this boolean flag
    p->paused = 1;
}

void groove_playlist_seek(struct GroovePlaylist *playlist, struct GroovePlaylistItem *item, double seconds) {
    struct GrooveFile * file = item->file;
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    int64_t ts = seconds * f->audio_st->time_base.den / f->audio_st->time_base.num;
    if (f->ic->start_time != AV_NOPTS_VALUE)
        ts += f->ic->start_time;

    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    pthread_mutex_lock(&p->decode_head_mutex);
    pthread_mutex_lock(&f->seek_mutex);

    f->seek_pos = ts;
    f->seek_flush = 1;

    pthread_mutex_unlock(&f->seek_mutex);

    p->decode_head = item;
    pthread_cond_signal(&p->decode_head_cond);
    pthread_mutex_unlock(&p->decode_head_mutex);
}

struct GroovePlaylistItem * groove_playlist_insert(struct GroovePlaylist *playlist, struct GrooveFile *file,
        double gain, struct GroovePlaylistItem *next)
{
    struct GroovePlaylistItem * item = av_mallocz(sizeof(struct GroovePlaylistItem));
    if (!item)
        return NULL;

    item->file = file;
    item->next = next;
    item->gain = gain;

    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;

    // lock decode_head_mutex so that decode_head cannot point to a new item
    // while we're screwing around with the queue
    pthread_mutex_lock(&p->decode_head_mutex);

    if (next) {
        if (next->prev) {
            item->prev = next->prev;
            item->prev->next = item;
            next->prev = item;
        } else {
            playlist->head = item;
        }
    } else if (!playlist->head) {
        playlist->head = item;
        playlist->tail = item;

        pthread_mutex_lock(&f->seek_mutex);
        f->seek_pos = 0;
        f->seek_flush = 0;
        pthread_mutex_unlock(&f->seek_mutex);

        p->decode_head = playlist->head;
        pthread_cond_signal(&p->decode_head_cond);
    } else {
        item->prev = playlist->tail;
        playlist->tail->next = item;
        playlist->tail = item;
    }

    pthread_mutex_unlock(&p->decode_head_mutex);
    return item;
}

static int purge_sink(struct GrooveSink *sink) {
    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;

    groove_queue_purge(s->audioq);

    struct GroovePlaylist *playlist = sink->playlist;
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    struct GroovePlaylistItem *item = p->purge_item;

    if (sink->purge)
        sink->purge(sink, item);

    return 0;
}

void groove_playlist_remove(struct GroovePlaylist *playlist, struct GroovePlaylistItem *item) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    pthread_mutex_lock(&p->decode_head_mutex);

    // if it's currently being played, seek to the next item
    if (item == p->decode_head) {
        p->decode_head = item->next;
    }

    if (item->prev) {
        item->prev->next = item->next;
    } else {
        playlist->head = item->next;
    }
    if (item->next) {
        item->next->prev = item->prev;
    } else {
        playlist->tail = item->prev;
    }

    // in each sink,
    // we must be absolutely sure to purge the audio buffer queue
    // of references to item before freeing it at the bottom of this method
    p->purge_item = item;
    every_sink(playlist, purge_sink, 0);
    p->purge_item = NULL;

    pthread_cond_signal(&p->sink_drain_cond);
    pthread_mutex_unlock(&p->decode_head_mutex);

    av_free(item);
}

void groove_playlist_clear(struct GroovePlaylist *playlist) {
    struct GroovePlaylistItem * node = playlist->head;
    if (!node) return;
    while (node) {
        struct GroovePlaylistItem *next = node->next;
        groove_playlist_remove(playlist, node);
        node = next;
    }
}

int groove_playlist_count(struct GroovePlaylist *playlist) {
    struct GroovePlaylistItem * node = playlist->head;
    int count = 0;
    while (node) {
        count += 1;
        node = node->next;
    }
    return count;
}

void groove_playlist_set_gain(struct GroovePlaylist *playlist, struct GroovePlaylistItem *item,
        double gain)
{
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    pthread_mutex_lock(&p->decode_head_mutex);
    item->gain = gain;
    if (item == p->decode_head) {
        p->volume = playlist->volume * p->decode_head->gain;
    }
    pthread_mutex_unlock(&p->decode_head_mutex);
}

void groove_playlist_position(struct GroovePlaylist *playlist, struct GroovePlaylistItem **item,
        double *seconds)
{
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    pthread_mutex_lock(&p->decode_head_mutex);
    if (item)
        *item = p->decode_head;

    if (seconds && p->decode_head) {
        struct GrooveFile *file = p->decode_head->file;
        struct GrooveFilePrivate *f = (struct GrooveFilePrivate *) file;
        *seconds = f->audio_clock;
    }
    pthread_mutex_unlock(&p->decode_head_mutex);
}

void groove_playlist_set_volume(struct GroovePlaylist *playlist, double volume) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;

    pthread_mutex_lock(&p->decode_head_mutex);
    playlist->volume = volume;
    p->volume = p->decode_head ? volume * p->decode_head->gain : volume;
    pthread_mutex_unlock(&p->decode_head_mutex);
}

int groove_playlist_playing(struct GroovePlaylist *playlist) {
    struct GroovePlaylistPrivate *p = (struct GroovePlaylistPrivate *) playlist;
    return !p->paused;
}

struct GrooveSink * groove_sink_create(void) {
    struct GrooveSinkPrivate *s = av_mallocz(sizeof(struct GrooveSinkPrivate));

    if (!s) {
        av_log(NULL, AV_LOG_ERROR, "could not create sink: out of memory\n");
        return NULL;
    }

    struct GrooveSink *sink = &s->externals;

    sink->buffer_size = 8192;

    s->audioq = groove_queue_create();

    if (!s->audioq) {
        groove_sink_destroy(sink);
        av_log(NULL, AV_LOG_ERROR, "could not create audio buffer: out of memory\n");
        return NULL;
    }

    s->audioq->context = sink;
    s->audioq->cleanup = audioq_cleanup;
    s->audioq->put = audioq_put;
    s->audioq->get = audioq_get;
    s->audioq->purge = audioq_purge;

    return sink;
}

void groove_sink_destroy(struct GrooveSink *sink) {
    if (!sink)
        return;

    struct GrooveSinkPrivate *s = (struct GrooveSinkPrivate *) sink;

    if (s->audioq)
        groove_queue_destroy(s->audioq);

    av_free(s);
}
