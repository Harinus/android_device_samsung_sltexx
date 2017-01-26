/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef WOLFSON_AUDIO_HW_H
#define WOLFSON_AUDIO_HW_H

#include <cutils/list.h>
#include <hardware/audio.h>

#include <tinyalsa/asoundlib.h>
#include <tinycompress/tinycompress.h>

#include <audio_utils/resampler.h>
#include <audio_route/audio_route.h>

#define MIXER_CARD 0
#define SOUND_CARD 0

#define PCM_CARD 0
#define PCM_CARD_SPDIF 1
#define PCM_TOTAL 2

#define PCM_DEVICE_PLAYBACK 0     /* Playback link */
#define PCM_DEVICE_CAPTURE 0      /* Capture link */
#define PCM_DEVICE_VOICE 1        /* Baseband link */
#define PCM_DEVICE_SCO 2          /* Bluetooth link */
#define PCM_DEVICE_DEEP_BUFFER 3  /* Deep buffer */

#define PLAYBACK_PERIOD_SIZE 240
#define PLAYBACK_PERIOD_SIZE_DEEP_BUFFER 960
#define PLAYBACK_PERIOD_COUNT 2
#define PLAYBACK_PERIOD_COUNT_DEEP_BUFFER 2
#define PLAYBACK_DEFAULT_CHANNEL_COUNT 2
#define PLAYBACK_DEFAULT_SAMPLING_RATE 48000
#define PLAYBACK_START_THRESHOLD(size, count) (((size) * (count)) - 1)
#define PLAYBACK_STOP_THRESHOLD(size, count) ((size) * ((count) + 2))
#define PLAYBACK_AVAILABLE_MIN 1

#define DEEP_BUFFER_PERIOD_SIZE 960
#define DEEP_BUFFER_PERIOD_COUNT 2
#define DEEP_BUFFER_CHANNEL_COUNT 2
#define DEEP_BUFFER_SAMPLING_RATE 48000

#define SCO_PERIOD_SIZE 240
#define SCO_PERIOD_COUNT 2
#define SCO_DEFAULT_CHANNEL_COUNT 2
#define SCO_DEFAULT_SAMPLING_RATE 8000
#define SCO_START_THRESHOLD 335
#define SCO_STOP_THRESHOLD 336
#define SCO_AVAILABLE_MIN 1

#define CAPTURE_PERIOD_SIZE 320
#define CAPTURE_PERIOD_SIZE_LOW_LATENCY 240
#define CAPTURE_PERIOD_COUNT 2
#define CAPTURE_PERIOD_COUNT_LOW_LATENCY 2
#define CAPTURE_DEFAULT_CHANNEL_COUNT 2
#define CAPTURE_DEFAULT_SAMPLING_RATE 48000
#define CAPTURE_START_THRESHOLD 1

#define VOICE_DEFAULT_PERIOD_SIZE 320
#define VOICE_DEFAULT_PERIOD_COUNT 2
#define VOICE_DEFAULT_CHANNEL_COUNT 2
#define VOICE_SAMPLING_RATE 8000
#define VOICE_SAMPLING_RATE_WIDEBAND 16000
#define VOICE_START_THRESHOLD 1

#define HDMI_PERIOD_SIZE 336
#define HDMI_PERIOD_COUNT 8
#define HDMI_DEFAULT_CHANNEL_COUNT 6
#define HDMI_DEFAULT_SAMPLING_RATE 48000
#define HDMI_START_THRESHOLD 1

#define HDMI_MAX_SUPPORTED_CHANNEL_MASKS 2

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef int snd_device_t;

typedef enum {
    USECASE_INVALID = -1,

    /* Playback usecases */
    USECASE_AUDIO_PLAYBACK = 0,
    USECASE_AUDIO_PLAYBACK_MULTI_CH,

    USECASE_AUDIO_HFP_SCO,

    /* Capture usecases */
    USECASE_AUDIO_CAPTURE,
    USECASE_AUDIO_CAPTURE_LOW_LATENCY,

    USECASE_VOICE_CALL,

    AUDIO_USECASE_MAX
} audio_usecase_t;

typedef enum {
    PCM_PLAYBACK,
    PCM_CAPTURE,
    VOICE_CALL,
    PCM_HPF_CALL,
} usecase_type_t;

struct pcm_device_profile {
    struct pcm_config config;
    int               card;
    int               id;
    usecase_type_t    type;
    audio_devices_t   devices;
};

struct stream_out {
    struct audio_stream_out     stream;

    pthread_mutex_t             lock; /* see note below on mutex acquisition order */
    pthread_mutex_t             pre_lock; /* acquire before lock to avoid DOS by playback thread */
    pthread_cond_t              cond;

    struct pcm_config           config;
    struct listnode             pcm_dev_list;
    bool                        standby;
    unsigned int                sample_rate;
    audio_channel_mask_t        channel_mask;
    audio_format_t              format;
    audio_devices_t             devices;
    audio_output_flags_t        flags;
    audio_usecase_t             usecase;
    /* Array of supported channel mask configurations. +1 so that the last entry is always 0 */
    audio_channel_mask_t        supported_channel_masks[MAX_SUPPORTED_CHANNEL_MASKS + 1];
    bool                        muted;
    /* total frames written, not cleared when entering standby */
    uint64_t                    written;
    int64_t                     last_write_time_us;
    audio_io_handle_t           handle;

    struct audio_device         *dev;
}

struct stream_in {
    struct audio_stream_in              stream;

    pthread_mutex_t                     lock; /* see note below on mutex acquisition order */
    pthread_mutex_t                     pre_lock; /* acquire before lock to avoid DOS by
                                                     capture thread */
    struct pcm_config                   config;
    struct listnode                     pcm_dev_list;
    int                                 standby;
    audio_source_t                      source;
    audio_devices_t                     devices;
    uint32_t                            main_channels;
    audio_usecase_t                     usecase;
    usecase_type_t                      usecase_type;
    bool                                enable_aec;
    audio_input_flags_t                 input_flags;

    /* TODO: remove resampler if possible when AudioFlinger supports downsampling from 48 to 8 */
    unsigned int                        requested_rate;
    struct resampler_itfe*              resampler;
    struct resampler_buffer_provider    buf_provider;
    int                                 read_status;
    int16_t*                            read_buf;
    size_t                              read_buf_size;
    size_t                              read_buf_frames;

    struct audio_device*                dev;
};

struct audio_usecase {
    struct listnode         adev_list_node;
    audio_usecase_t         id;
    usecase_type_t          type;
    audio_devices_t         devices;
    snd_device_t            out_snd_device;
    snd_device_t            in_snd_device;
    struct audio_stream*    stream;
    struct listnode         mixer_list;
};

struct audio_device {
    struct audio_hw_device  hw_device;
    pthread_mutex_t         lock; /* see note below on mutex acquisition order */

    struct {
        struct audio_route *audio_route;
        struct timespec shutdown_time;
    } mixer;

    audio_mode_t            mode;
    struct stream_in        *active_input;
    struct stream_out       *primary_output;

    struct {
        bool                    in_call;
        float                   volume;
        bool                    mic_mute;
        bool                    bluetooth_nrec;
        bool                    wb_amr;
        bool                    two_mic_config;
    } voice;

    int                     *snd_dev_ref_cnt;
    struct listnode         usecase_list;

    /* RIL */
    struct ril_handle ril;

    pthread_mutex_t         lock_inputs; /* see note below on mutex acquisition order */
    pthread_mutex_t         lock_outputs; /* see note below on mutex acquisition order */
};

#endif /* WOLFSON_AUDIO_HW_H */
