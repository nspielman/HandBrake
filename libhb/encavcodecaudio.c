/* encavcodecaudio.c

   Copyright (c) 2003-2017 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "hb.h"
#include "hbffmpeg.h"

struct hb_work_private_s
{
    hb_job_t       * job;
    AVCodecContext * context;

    int              out_discrete_channels;
    int              samples_per_frame;
    unsigned long    max_output_bytes;
    unsigned long    input_samples;
    uint8_t        * output_buf;
    uint8_t        * input_buf;
    hb_list_t      * list;

    AVAudioResampleContext *avresample;

    int64_t          last_pts;
};

static int  encavcodecaInit( hb_work_object_t *, hb_job_t * );
static int  encavcodecaWork( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
static void encavcodecaClose( hb_work_object_t * );

hb_work_object_t hb_encavcodeca =
{
    WORK_ENCAVCODEC_AUDIO,
    "AVCodec Audio encoder (libavcodec)",
    encavcodecaInit,
    encavcodecaWork,
    encavcodecaClose
};

static int encavcodecaInit(hb_work_object_t *w, hb_job_t *job)
{
    AVCodec *codec;
    AVCodecContext *context;
    hb_audio_t *audio = w->audio;

    hb_work_private_t *pv = calloc(1, sizeof(hb_work_private_t));
    w->private_data       = pv;
    pv->job               = job;
    pv->list              = hb_list_init();
    pv->last_pts          = AV_NOPTS_VALUE;

    // channel count, layout and matrix encoding
    int matrix_encoding;
    uint64_t channel_layout   = hb_ff_mixdown_xlat(audio->config.out.mixdown,
                                                   &matrix_encoding);
    pv->out_discrete_channels =
        hb_mixdown_get_discrete_channel_count(audio->config.out.mixdown);

    // default settings and options
    AVDictionary *av_opts          = NULL;
    const char *codec_name         = NULL;
    enum AVCodecID codec_id        = AV_CODEC_ID_NONE;
    enum AVSampleFormat sample_fmt = AV_SAMPLE_FMT_FLTP;
    int bits_per_raw_sample        = 0;
    int profile                    = FF_PROFILE_UNKNOWN;

    // override with encoder-specific values
    switch (audio->config.out.codec)
    {
        case HB_ACODEC_AC3:
            codec_id = AV_CODEC_ID_AC3;
            if (matrix_encoding != AV_MATRIX_ENCODING_NONE)
                av_dict_set(&av_opts, "dsur_mode", "on", 0);
            break;

        case HB_ACODEC_FFEAC3:
            codec_id = AV_CODEC_ID_EAC3;
            if (matrix_encoding != AV_MATRIX_ENCODING_NONE)
                av_dict_set(&av_opts, "dsur_mode", "on", 0);
            break;

        case HB_ACODEC_FDK_AAC:
        case HB_ACODEC_FDK_HAAC:
            codec_name          = "libfdk_aac";
            sample_fmt          = AV_SAMPLE_FMT_S16;
            bits_per_raw_sample = 16;
            switch (audio->config.out.codec)
            {
                case HB_ACODEC_FDK_HAAC:
                    profile = FF_PROFILE_AAC_HE;
                    break;
                default:
                    profile = FF_PROFILE_AAC_LOW;
                    break;
            }
            // Libav's libfdk-aac wrapper expects back channels for 5.1
            // audio, and will error out unless we translate the layout
            if (channel_layout == AV_CH_LAYOUT_5POINT1)
                channel_layout  = AV_CH_LAYOUT_5POINT1_BACK;
            break;

        case HB_ACODEC_FFAAC:
            codec_name = "aac";
            av_dict_set(&av_opts, "stereo_mode", "ms_off", 0);
            break;

        case HB_ACODEC_FFFLAC:
        case HB_ACODEC_FFFLAC24:
            codec_id = AV_CODEC_ID_FLAC;
            switch (audio->config.out.codec)
            {
                case HB_ACODEC_FFFLAC24:
                    sample_fmt          = AV_SAMPLE_FMT_S32;
                    bits_per_raw_sample = 24;
                    break;
                default:
                    sample_fmt          = AV_SAMPLE_FMT_S16;
                    bits_per_raw_sample = 16;
                    break;
            }
            break;

        case HB_ACODEC_LAME:
            codec_name = "libmp3lame";
            break;

        case HB_ACODEC_OPUS:
            codec_name = "libopus";
            // Libav's libopus wrapper expects back channels for 5.1
            // audio, and will error out unless we translate the layout
            if (channel_layout == AV_CH_LAYOUT_5POINT1)
                channel_layout  = AV_CH_LAYOUT_5POINT1_BACK;
            break;

        default:
            hb_error("encavcodecaInit: unsupported codec (0x%x)",
                     audio->config.out.codec);
            return 1;
    }
    if (codec_name != NULL)
    {
        codec = avcodec_find_encoder_by_name(codec_name);
        if (codec == NULL)
        {
            hb_error("encavcodecaInit: avcodec_find_encoder_by_name(%s) failed",
                     codec_name);
            return 1;
        }
    }
    else
    {
        codec = avcodec_find_encoder(codec_id);
        if (codec == NULL)
        {
            hb_error("encavcodecaInit: avcodec_find_encoder(%d) failed",
                     codec_id);
            return 1;
        }
    }
    // allocate the context and apply the settings
    context                      = avcodec_alloc_context3(codec);
    hb_ff_set_sample_fmt(context, codec, sample_fmt);
    context->bits_per_raw_sample = bits_per_raw_sample;
    context->profile             = profile;
    context->channel_layout      = channel_layout;
    context->channels            = pv->out_discrete_channels;
    context->sample_rate         = audio->config.out.samplerate;
    context->time_base           = (AVRational){1, 90000};

    if (audio->config.out.bitrate > 0)
    {
        context->bit_rate = audio->config.out.bitrate * 1000;
    }
    else if (audio->config.out.quality >= 0)
    {
        context->global_quality = audio->config.out.quality * FF_QP2LAMBDA;
        context->flags |= CODEC_FLAG_QSCALE;
        if (audio->config.out.codec == HB_ACODEC_FDK_AAC ||
            audio->config.out.codec == HB_ACODEC_FDK_HAAC)
        {
            char vbr[2];
            snprintf(vbr, 2, "%.1g", audio->config.out.quality);
            av_dict_set(&av_opts, "vbr", vbr, 0);
        }
    }

    if (audio->config.out.compression_level >= 0)
    {
        context->compression_level = audio->config.out.compression_level;
    }

    // For some codecs, libav requires the following flag to be set
    // so that it fills extradata with global header information.
    // If this flag is not set, it inserts the data into each
    // packet instead.
    context->flags |= CODEC_FLAG_GLOBAL_HEADER;

    if (hb_avcodec_open(context, codec, &av_opts, 0))
    {
        hb_error("encavcodecaInit: hb_avcodec_open() failed");
        return 1;
    }
    w->config->init_delay = av_rescale(context->initial_padding,
                                       90000, context->sample_rate);

    // avcodec_open populates the opts dictionary with the
    // things it didn't recognize.
    AVDictionaryEntry *t = NULL;
    while ((t = av_dict_get(av_opts, "", t, AV_DICT_IGNORE_SUFFIX)))
    {
        hb_log("encavcodecaInit: Unknown avcodec option %s", t->key);
    }
    av_dict_free(&av_opts);

    pv->context           = context;
    audio->config.out.samples_per_frame =
    pv->samples_per_frame = context->frame_size;
    pv->input_samples     = context->frame_size * context->channels;
    pv->input_buf         = malloc(pv->input_samples * sizeof(float));
    // Some encoders in libav (e.g. fdk-aac) fail if the output buffer
    // size is not some minumum value.  8K seems to be enough :(
    pv->max_output_bytes  = MAX(FF_MIN_BUFFER_SIZE,
                                (pv->input_samples *
                                 av_get_bytes_per_sample(context->sample_fmt)));

    // sample_fmt conversion
    if (context->sample_fmt != AV_SAMPLE_FMT_FLT)
    {
        pv->output_buf = malloc(pv->max_output_bytes);
        pv->avresample = avresample_alloc_context();
        if (pv->avresample == NULL)
        {
            hb_error("encavcodecaInit: avresample_alloc_context() failed");
            return 1;
        }
        av_opt_set_int(pv->avresample, "in_sample_fmt",
                       AV_SAMPLE_FMT_FLT, 0);
        av_opt_set_int(pv->avresample, "out_sample_fmt",
                       context->sample_fmt, 0);
        av_opt_set_int(pv->avresample, "in_channel_layout",
                       context->channel_layout, 0);
        av_opt_set_int(pv->avresample, "out_channel_layout",
                       context->channel_layout, 0);
        if (hb_audio_dither_is_supported(audio->config.out.codec))
        {
            // dithering needs the sample rate
            av_opt_set_int(pv->avresample, "in_sample_rate",
                           context->sample_rate, 0);
            av_opt_set_int(pv->avresample, "out_sample_rate",
                           context->sample_rate, 0);
            av_opt_set_int(pv->avresample, "dither_method",
                           audio->config.out.dither_method, 0);
        }
        if (avresample_open(pv->avresample))
        {
            hb_error("encavcodecaInit: avresample_open() failed");
            avresample_free(&pv->avresample);
            return 1;
        }
    }
    else
    {
        pv->avresample = NULL;
        pv->output_buf = pv->input_buf;
    }

    if (context->extradata != NULL)
    {
        memcpy(w->config->extradata.bytes, context->extradata,
               context->extradata_size);
        w->config->extradata.length = context->extradata_size;
    }

    return 0;
}

/***********************************************************************
 * Close
 ***********************************************************************
 *
 **********************************************************************/
// Some encoders (e.g. flac) require a final NULL encode in order to
// finalize things.
static void Finalize(hb_work_object_t *w)
{
    hb_work_private_t *pv = w->private_data;

    // Then we need to recopy the header since it was modified
    if (pv->context->extradata != NULL)
    {
        memcpy(w->config->extradata.bytes, pv->context->extradata,
               pv->context->extradata_size);
        w->config->extradata.length = pv->context->extradata_size;
    }
}

static void encavcodecaClose(hb_work_object_t * w)
{
    hb_work_private_t * pv = w->private_data;

    if (pv != NULL)
    {
        if (pv->context != NULL)
        {
            Finalize(w);
            hb_deep_log(2, "encavcodecaudio: closing libavcodec");
            if (pv->context->codec != NULL)
                avcodec_flush_buffers(pv->context);
            hb_avcodec_close(pv->context);
            av_free( pv->context );
        }

        if (pv->output_buf != NULL)
        {
            free(pv->output_buf);
        }
        if (pv->input_buf != NULL && pv->input_buf != pv->output_buf)
        {
            free(pv->input_buf);
        }
        pv->output_buf = pv->input_buf = NULL;

        if (pv->list != NULL)
        {
            hb_list_empty(&pv->list);
        }

        if (pv->avresample != NULL)
        {
            avresample_free(&pv->avresample);
        }

        free(pv);
        w->private_data = NULL;
    }
}

static void get_packets( hb_work_object_t * w, hb_buffer_list_t * list )
{
    hb_work_private_t * pv = w->private_data;
    hb_audio_t        * audio = w->audio;

    while (1)
    {
        // Prepare output packet
        int           ret;
        AVPacket      pkt;
        hb_buffer_t * out;

        av_init_packet(&pkt);
        ret = avcodec_receive_packet(pv->context, &pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        if (ret < 0)
        {
            hb_log("encavcodecaudio: avcodec_receive_packet failed");
            break;
        }

        out = hb_buffer_init(pkt.size);
        memcpy(out->data, pkt.data, out->size);

        // FIXME: On windows builds, there is an upstream bug in the lame
        // encoder that causes an extra output packet that has the same
        // timestamp as the second to last packet.  This causes an error
        // during muxing. Work around it by dropping such packets here.
        // See: https://github.com/HandBrake/HandBrake/issues/726
        if (pkt.pts > pv->last_pts)
        {
            // The output pts from libav is in context->time_base. Convert it
            // back to our timebase.
            out->s.start = av_rescale_q(pkt.pts, pv->context->time_base,
                                        (AVRational){1, 90000});
            out->s.duration  = (double)90000 * pv->samples_per_frame /
                                               audio->config.out.samplerate;
            out->s.stop      = out->s.start + out->s.duration;
            out->s.type      = AUDIO_BUF;
            out->s.frametype = HB_FRAME_AUDIO;

            hb_buffer_list_append(list, out);
            pv->last_pts = pkt.pts;
        }
        av_packet_unref(&pkt);
    }
}

static void Encode(hb_work_object_t *w, hb_buffer_list_t *list)
{
    hb_work_private_t * pv = w->private_data;
    hb_audio_t        * audio = w->audio;
    uint64_t            pts, pos;

    while (hb_list_bytes(pv->list) >= pv->input_samples * sizeof(float))
    {
        int ret;

        hb_list_getbytes(pv->list, pv->input_buf,
                         pv->input_samples * sizeof(float), &pts, &pos);

        // Prepare input frame
        int     out_linesize, out_size;
        AVFrame frame = { .nb_samples = pv->samples_per_frame, };

        out_size = av_samples_get_buffer_size(&out_linesize,
                                              pv->context->channels,
                                              pv->samples_per_frame,
                                              pv->context->sample_fmt, 1);
        avcodec_fill_audio_frame(&frame,
                                 pv->context->channels, pv->context->sample_fmt,
                                 pv->output_buf, out_size, 1);
        if (pv->avresample != NULL)
        {
            int in_linesize, out_samples;

            av_samples_get_buffer_size(&in_linesize, pv->context->channels,
                                       frame.nb_samples, AV_SAMPLE_FMT_FLT, 1);
            out_samples = avresample_convert(pv->avresample,
                                            frame.extended_data, out_linesize,
                                            frame.nb_samples, &pv->input_buf,
                                            in_linesize, frame.nb_samples);
            if (out_samples != pv->samples_per_frame)
            {
                // we're not doing sample rate conversion,
                // so this shouldn't happen
                hb_log("encavcodecaWork: avresample_convert() failed");
                continue;
            }
        }

        frame.pts = pts + (90000LL * pos / (sizeof(float) *
                                          pv->out_discrete_channels *
                                          audio->config.out.samplerate));

        frame.pts = av_rescale_q(frame.pts, (AVRational){1, 90000},
                                 pv->context->time_base);

        // Encode
        ret = avcodec_send_frame(pv->context, &frame);
        if (ret < 0)
        {
            hb_log("encavcodecaudio: avcodec_send_frame failed");
            return;
        }
        get_packets(w, list);
    }
}

static void Flush( hb_work_object_t * w, hb_buffer_list_t * list )
{
    hb_work_private_t * pv = w->private_data;

    avcodec_send_frame(pv->context, NULL);
    get_packets(w, list);
}

/***********************************************************************
 * Work
 ***********************************************************************
 *
 **********************************************************************/
static int encavcodecaWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
                    hb_buffer_t ** buf_out )
{
    hb_work_private_t * pv = w->private_data;
    hb_buffer_t       * in = *buf_in;
    hb_buffer_list_t    list;

    if (pv->context == NULL || pv->context->codec == NULL)
    {
        hb_error("encavcodecaudio: codec context is uninitialized");
        return HB_WORK_DONE;
    }

    hb_buffer_list_clear(&list);
    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        /* EOF on input - send it downstream & say we're done */
        Flush(w, &list);
        hb_buffer_list_append(&list, hb_buffer_eof_init());
        *buf_out = hb_buffer_list_clear(&list);
        return HB_WORK_DONE;
    }

    hb_list_add( pv->list, in );
    *buf_in = NULL;

    Encode(w, &list);
    *buf_out = hb_buffer_list_clear(&list);

    return HB_WORK_OK;
}


