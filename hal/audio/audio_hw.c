/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2013-2015 The CyanogenMod Project
 *               Daniel Hillenbrand <codeworkx@cyanogenmod.com>
 *               Guillaume "XpLoDWilD" Lesniak <xplodgui@gmail.com>
 * Copyright (c) 2015-2017 Andreas Schneider <asn@cryptomilk.org>
 * Copyright (c) 2015-2017 Christopher N. Hesse <raymanfx@gmail.org>
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

#define LOG_TAG "audio_hw_primary"
#define LOG_NDEBUG 0
//#define ALOG_TRACE 1

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <fcntl.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>

#include <linux/videodev2.h>
#include <linux/videodev2_exynos_media.h>

#include <system/audio.h>

#include "audio_hw.h"
#include "routing.h"
#include "ril_interface.h"

#ifdef ALOG_TRACE
#define ALOGT ALOGV
#else
#define ALOGT(a...) do { } while(0)
#endif

/* duration in ms of volume ramp applied when starting capture to remove plop */
#define CAPTURE_START_RAMP_MS 100

#define DAPM_SHUTDOWN_TIME 10000 /* 10 ms */

static struct pcm_device_profile pcm_device_playback = {
    .config = {
        .channels = PLAYBACK_DEFAULT_CHANNEL_COUNT,
        .rate = PLAYBACK_DEFAULT_SAMPLING_RATE,
        .period_size = PLAYBACK_PERIOD_SIZE,
        .period_count = PLAYBACK_PERIOD_COUNT,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = PLAYBACK_START_THRESHOLD(PLAYBACK_PERIOD_SIZE,
                                                    PLAYBACK_PERIOD_COUNT),
        .stop_threshold = PLAYBACK_STOP_THRESHOLD(PLAYBACK_PERIOD_SIZE,
                                                  PLAYBACK_PERIOD_COUNT),
        .silence_threshold = 0,
        .silence_size = UINT_MAX,
        .avail_min = PLAYBACK_AVAILABLE_MIN,
    },
    .card = SOUND_CARD,
    .id = PCM_DEVICE_PLAYBACK,
    .type = PCM_PLAYBACK,
    .devices = AUDIO_DEVICE_OUT_WIRED_HEADSET|
               AUDIO_DEVICE_OUT_WIRED_HEADPHONE|
               AUDIO_DEVICE_OUT_SPEAKER,
};

static struct pcm_device_profile pcm_device_capture = {
    .config = {
        .channels = CAPTURE_DEFAULT_CHANNEL_COUNT,
        .rate = CAPTURE_DEFAULT_SAMPLING_RATE,
        .period_size = CAPTURE_PERIOD_SIZE,
        .period_count = CAPTURE_PERIOD_COUNT,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = CAPTURE_START_THRESHOLD,
        .stop_threshold = 0,
        .silence_threshold = 0,
        .avail_min = 0,
    },
    .card = SOUND_CARD,
    .id = PCM_DEVICE_CAPTURE,
    .type = PCM_CAPTURE,
    .devices = AUDIO_DEVICE_IN_BUILTIN_MIC|
               AUDIO_DEVICE_IN_WIRED_HEADSET|
               AUDIO_DEVICE_IN_BACK_MIC,
};

static struct pcm_device_profile pcm_device_capture_low_latency = {
    .config = {
        .channels = CAPTURE_DEFAULT_CHANNEL_COUNT,
        .rate = CAPTURE_DEFAULT_SAMPLING_RATE,
        .period_size = CAPTURE_PERIOD_SIZE_LOW_LATENCY,
        .period_count = CAPTURE_PERIOD_COUNT_LOW_LATENCY,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = CAPTURE_START_THRESHOLD,
        .stop_threshold = 0,
        .silence_threshold = 0,
        .avail_min = 0,
    },
    .card = SOUND_CARD,
    .id = PCM_DEVICE_CAPTURE,
    .type = PCM_CAPTURE_LOW_LATENCY,
    .devices = AUDIO_DEVICE_IN_BUILTIN_MIC |
               AUDIO_DEVICE_IN_WIRED_HEADSET |
               AUDIO_DEVICE_IN_BACK_MIC,
};

static struct pcm_device_profile pcm_device_playback_sco = {
    .config = {
        .channels = SCO_DEFAULT_CHANNEL_COUNT,
        .rate = SCO_DEFAULT_SAMPLING_RATE,
        .period_size = SCO_PERIOD_SIZE,
        .period_count = SCO_PERIOD_COUNT,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = SCO_START_THRESHOLD,
        .stop_threshold = SCO_STOP_THRESHOLD,
        .silence_threshold = 0,
        .avail_min = SCO_AVAILABLE_MIN,
    },
    .card = SOUND_CARD,
    .id = PCM_DEVICE_SCO,
    .type = PCM_PLAYBACK,
    .devices = AUDIO_DEVICE_OUT_BLUETOOTH_SCO |
               AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET |
               AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT,
};

static struct pcm_device_profile pcm_device_capture_sco = {
    .config = {
        .channels = SCO_DEFAULT_CHANNEL_COUNT,
        .rate = SCO_DEFAULT_SAMPLING_RATE,
        .period_size = SCO_PERIOD_SIZE,
        .period_count = SCO_PERIOD_COUNT,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = CAPTURE_START_THRESHOLD,
        .stop_threshold = 0,
        .silence_threshold = 0,
        .avail_min = 0,
    },
    .card = SOUND_CARD,
    .id = PCM_DEVICE_SCO,
    .type = PCM_CAPTURE,
    .devices = AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET,
};

static struct pcm_device_profile pcm_device_voice = {
    .config = {
        .channels = VOICE_DEFAULT_CHANNEL_COUNT,
        .rate = VOICE_SAMPLING_RATE,
        .period_size = VOICE_DEFAULT_PERIOD_SIZE,
        .period_count = VOICE_DEAULT_PERIOD_COUNT,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = CAPTURE_START_THRESHOLD,
        .stop_threshold = 0,
        .silence_threshold = 0,
        .avail_min = 0,
    },
    .card = SOUND_CARD,
    .id = PCM_DEVICE_VOICE,
    .type = VOICE_CALL,
    .devices = AUDIO_DEVICE_IN_BUILTIN_MIC |
               AUDIO_DEVICE_IN_WIRED_HEADSET |
               AUDIO_DEVICE_IN_BACK_MIC,
};

static struct pcm_device_profile pcm_device_voice_wideband = {
    .config = {
        .channels = VOICE_DEFAULT_CHANNEL_COUNT,
        .rate = VOICE_SAMPLING_RATE_WIDEBAND,
        .period_size = VOICE_DEFAULT_PERIOD_SIZE,
        .period_count = VOICE_DEAULT_PERIOD_COUNT,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = CAPTURE_START_THRESHOLD,
        .stop_threshold = 0,
        .silence_threshold = 0,
        .avail_min = 0,
    },
    .card = SOUND_CARD,
    .id = PCM_DEVICE_VOICE,
    .type = VOICE_CALL,
    .devices = AUDIO_DEVICE_IN_BUILTIN_MIC |
               AUDIO_DEVICE_IN_WIRED_HEADSET |
               AUDIO_DEVICE_IN_BACK_MIC,
};

static struct pcm_config pcm_config_deep_buffer = {
    .channels = DEEP_BUFFER_CHANNEL_COUNT,
    .rate = DEEP_BUFFER_OUTPUT_SAMPLING_RATE,
    .period_size = DEEP_BUFFER_OUTPUT_PERIOD_SIZE,
    .period_count = DEEP_BUFFER_OUTPUT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = DEEP_BUFFER_OUTPUT_PERIOD_SIZE / 4,
    .stop_threshold = INT_MAX,
    .avail_min = DEEP_BUFFER_OUTPUT_PERIOD_SIZE / 4,
};

static const char * const use_case_table[AUDIO_USECASE_MAX] = {
    [USECASE_AUDIO_PLAYBACK] = "playback",
    [USECASE_AUDIO_PLAYBACK_MULTI_CH] = "playback multi-channel",
    [USECASE_AUDIO_HFP_SCO] = "hfp-sco",
    [USECASE_AUDIO_CAPTURE] = "capture",
    [USECASE_AUDIO_CAPTURE_LOW_LATENCY] = "capture low-latency",
    [USECASE_VOICE_CALL] = "voice-call",
};

#if 0
enum output_type {
    OUTPUT_DEEP_BUF,      // deep PCM buffers output stream
    OUTPUT_LOW_LATENCY,   // low latency output stream
    OUTPUT_HDMI,          // HDMI multi channel
    OUTPUT_TOTAL
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    audio_devices_t out_device; /* "or" of stream_out.device for all active output streams */
    audio_devices_t in_device;
    bool mic_mute;
    audio_source_t input_source;
    int cur_route_id;     /* current route ID: combination of input source
                           * and output device IDs */
    audio_mode_t mode;

    struct audio_route *audio_route;
    struct {
        const char *device;
        const char *route;
        int dev_id;
    } active_output;
    struct {
        const char *device;
        const char *route;
    } active_input;

    /* Call audio */
    struct pcm *pcm_voice_rx;
    struct pcm *pcm_voice_tx;

    /* SCO audio */
    struct pcm *pcm_sco_rx;
    struct pcm *pcm_sco_tx;

    float voice_volume;
    bool in_call;
    bool tty_mode;
    bool bluetooth_nrec;
    bool wb_amr;
    bool two_mic_control;

    int hdmi_drv_fd;
    audio_channel_mask_t in_channel_mask;

    /* RIL */
    struct ril_handle ril;

    struct stream_out *outputs[OUTPUT_TOTAL];
    pthread_mutex_t lock_outputs; /* see note below on mutex acquisition order */
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    pthread_mutex_t pre_lock;   /* acquire before lock to prevent playback thread DOS */
    struct pcm *pcm[PCM_TOTAL];
    struct pcm_config config;
    unsigned int pcm_device;
    bool standby; /* true if all PCMs are inactive */
    audio_devices_t device;
    /* FIXME: when HDMI multichannel output is active, other outputs must be disabled as
     * HDMI and WM1811 share the same I2S. This means that notifications and other sounds are
     * silent when watching a 5.1 movie. */
    bool disabled;

    audio_channel_mask_t channel_mask;
    /* Array of supported channel mask configurations. +1 so that the last entry is always 0 */
    audio_channel_mask_t supported_channel_masks[HDMI_MAX_SUPPORTED_CHANNEL_MASKS + 1];
    bool muted;
    uint64_t written; /* total frames written, not cleared when entering standby */
    int64_t last_write_time_us;

    struct audio_device *dev;
};

struct stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    pthread_mutex_t pre_lock;   /* acquire before lock to prevent playback thread DOS */
    struct pcm *pcm;
    bool standby;

    unsigned int requested_rate;
    struct resampler_itfe *resampler;
    struct resampler_buffer_provider buf_provider;
    int16_t *buffer;
    size_t buffer_size;
    size_t frames_in;
    int64_t frames_read; /* total frames read, not cleared when entering standby */
    int64_t last_read_time_us;
    int read_status;

    audio_source_t input_source;
    audio_io_handle_t io_handle;
    audio_devices_t device;

    uint16_t ramp_vol;
    uint16_t ramp_step;
    size_t ramp_frames;

    audio_channel_mask_t channel_mask;
    audio_input_flags_t flags;
    struct pcm_config *config;

    struct audio_device *dev;
};
#endif

#define STRING_TO_ENUM(string) { #string, string }

struct string_to_enum {
    const char *name;
    uint32_t value;
};

const struct string_to_enum out_channels_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
};

struct timespec time_spec_diff(struct timespec time1, struct timespec time0) {
    struct timespec ret;
    int xsec = 0;
    int sign = 1;

    if (time0.tv_nsec > time1.tv_nsec) {
        xsec = (int) ((time0.tv_nsec - time1.tv_nsec) / (1E9 + 1));
        time0.tv_nsec -= (long int) (1E9 * xsec);
        time0.tv_sec += xsec;
    }

    if ((time1.tv_nsec - time0.tv_nsec) > 1E9) {
        xsec = (int) ((time1.tv_nsec - time0.tv_nsec) / 1E9);
        time0.tv_nsec += (long int) (1E9 * xsec);
        time0.tv_sec -= xsec;
    }

    ret.tv_sec = time1.tv_sec - time0.tv_sec;
    ret.tv_nsec = time1.tv_nsec - time0.tv_nsec;

    if (time1.tv_sec < time0.tv_sec) {
        sign = -1;
    }

    ret.tv_sec = ret.tv_sec * sign;

    return ret;
}

static const char *get_snd_device_name(snd_device_t snd_device)
{
    const char *name = NULL;

    if (snd_device >= SND_DEVICE_MIN && snd_device < SND_DEVICE_MAX) {
        name = device_table[snd_device];
    }

    ALOGE_IF(name == NULL, "%s: invalid snd device %d", __func__, snd_device);

   return name;
}

static const char *get_snd_device_display_name(snd_device_t snd_device)
{
    const char *name = get_snd_device_name(snd_device);

    if (name == NULL) {
        name = "SND DEVICE NOT FOUND";
    }

    return name;
}

static struct pcm_device_profile *get_pcm_device(usecase_type_t uc_type,
                                                 audio_devices_t devices)
{
    int i;

    devices &= ~AUDIO_DEVICE_BIT_IN;

    for (i = 0; pcm_devices[i] != NULL; i++) {
        if ((pcm_devices[i]->type == uc_type) &&
            (devices & pcm_devices[i]->devices)) {
            return pcm_devices[i];
        }
    }

    return NULL;
}

static struct audio_usecase *get_usecase_from_id(struct audio_device *adev,
                                                 audio_usecase_t uc_id)
{
    struct audio_usecase *usecase;
    struct listnode *node;

    list_for_each(node, &adev->usecase_list) {
        usecase = node_to_item(node, struct audio_usecase, adev_list_node);
        if (usecase->id == uc_id) {
            return usecase;
        }
    }

    return NULL;
}

static struct audio_usecase *get_usecase_from_type(struct audio_device *adev,
                                                   usecase_type_t type)
{
    struct audio_usecase *usecase;
    struct listnode *node;

    list_for_each(node, &adev->usecase_list) {
        usecase = node_to_item(node, struct audio_usecase, adev_list_node);
        if (usecase->type & type) {
            return usecase;
        }
    }

    return NULL;
}

/* always called with adev lock held */
static int set_voice_volume_l(struct audio_device *adev, float volume)
{
    int err = 0;

    adev->voice.volume = volume;

    if (adev->mode == AUDIO_MODE_IN_CALL) {
        enum _SoundType sound_type;

        switch (adev->out_device) {
            case AUDIO_DEVICE_OUT_EARPIECE:
                sound_type = SOUND_TYPE_VOICE;
                break;
            case AUDIO_DEVICE_OUT_SPEAKER:
                sound_type = SOUND_TYPE_SPEAKER;
                break;
            case AUDIO_DEVICE_OUT_WIRED_HEADSET:
            case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
                sound_type = SOUND_TYPE_HEADSET;
                break;
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            case AUDIO_DEVICE_OUT_ALL_SCO:
                sound_type = SOUND_TYPE_BTVOICE;
                break;
            default:
                sound_type = SOUND_TYPE_VOICE;
        }

        ril_set_call_volume(&adev->ril, sound_type, volume);
    }

    return err;
}

static snd_device_t get_output_snd_device(struct audio_device *adev,
                                          audio_devices_t devices)
{

    snd_device_t snd_device = SND_DEVICE_NONE;
    audio_mode_t mode = adev->mode;
    bool amr_wb = adev->voice.wb_amr;

    ALOGV("%s: enter: output devices(%#x), mode(%d)", __func__, devices, mode);

    if (devices == AUDIO_DEVICE_NONE ||
        devices & AUDIO_DEVICE_BIT_IN) {
        ALOGV("%s: Invalid output devices (%#x)", __func__, devices);
        goto exit;
    }

    if (mode == AUDIO_MODE_IN_CALL) {
        if (devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
            devices & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            if (wb_amr) {
                snd_device = SND_DEVICE_OUT_VOICE_HEADPHONES_WB;
            } else {
                snd_device = SND_DEVICE_OUT_VOICE_HEADPHONES;
            }
        } else if (devices & AUDIO_DEVICE_OUT_SPEAKER) {
            if (wb_amr) {
                snd_device = SND_DEVICE_OUT_VOICE_SPEAKER_WB;
            } else {
                snd_device = SND_DEVICE_OUT_VOICE_SPEAKER;
            }
        } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
            if (wb_amr) {
                snd_device = SND_DEVICE_OUT_VOICE_EARPIECE_WB;
            } else {
                snd_device = SND_DEVICE_OUT_VOICE_EARPIECE;
            }
        } else if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
            snd_device = SND_DEVICE_OUT_BT_SCO;
        }

        if (snd_device != SND_DEVICE_NONE) {
            goto exit;
        }
    }

    if (popcount(devices) == 2) {
        if (devices == (AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
                        AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES;
        } else if (devices == (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                               AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES;
        } else {
            ALOGE("%s: Invalid combo device(%#x)", __func__, devices);
            goto exit;
        }

        if (snd_device != SND_DEVICE_NONE) {
            goto exit;
        }
    }

    if (popcount(devices) != 1) {
        ALOGE("%s: Invalid output devices(%#x)", __func__, devices);
        goto exit;
    }

    if (devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
        devices & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
        snd_device = SND_DEVICE_OUT_HEADPHONES;
    } else if (devices & AUDIO_DEVICE_OUT_SPEAKER) {
        snd_device = SND_DEVICE_OUT_SPEAKER;
    } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
        snd_device = SND_DEVICE_OUT_HANDSET;
    } else if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
        snd_device = SND_DEVICE_OUT_BT_SCO;
    } else {
        ALOGE("%s: Unknown device(s) %#x", __func__, devices);
    }

exit:
    ALOGV("%s: exit: snd_device(%s)", __func__, device_table[snd_device]);

    return snd_device;
}

static snd_device_t get_input_snd_device(struct audio_device *adev,
                                         audio_devices_t out_device)
{
    audio_source_t source;
    audio_mode_t mode = adev->mode;
    audio_devices_t in_device;
    audio_channel_mask_t channel_mask;
    snd_device_t snd_device = SND_DEVICE_NONE;
    struct stream_in *active_input = NULL;
    struct audio_usecase *usecase;

    usecase = get_usecase_from_type(adev, PCM_CAPTURE | VOICE_CALL);
    if (usecase != NULL) {
        active_input = (struct stream_in *)usecase->stream;
    }
    source = (active_input == NULL) ?
                    AUDIO_SOURCE_DEFAULT :
                    active_input->source;

    in_device = ((active_input == NULL) ?
                    AUDIO_DEVICE_NONE :
                    active_input->devices) & ~AUDIO_DEVICE_BIT_IN;

    channel_mask = (active_input == NULL) ?
                    AUDIO_CHANNEL_IN_MONO :
                    active_input->main_channels;

    ALOGV("%s: enter: out_device(%#x) in_device(%#x)",
          __func__,
          out_device,
          in_device);

    if (mode == AUDIO_MODE_IN_CALL) {
        if (out_device == AUDIO_DEVICE_NONE) {
            ALOGE("%s: No output device set for voice call", __func__);
            goto exit;
        }

        if (out_device & AUDIO_DEVICE_OUT_EARPIECE ||
            out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            snd_device = SND_DEVICE_IN_EARPIECE_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_VOICE_HEADSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            snd_device = SND_DEVICE_IN_VOICE_SPEAKER_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC ;
        }
    } else if (source == AUDIO_SOURCE_CAMCORDER) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC ||
            in_device & AUDIO_DEVICE_IN_BACK_MIC) {
            snd_device = SND_DEVICE_IN_CAMCORDER_MIC;
        }
    } else if (source == AUDIO_SOURCE_VOICE_COMMUNICATION ||
               source == AUDIO_SOURCE_MIC) {
        if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            in_device = AUDIO_DEVICE_IN_BACK_MIC;
        }
#if 0
        if (active_input) {
            if (active_input->enable_aec) {
                if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
                    snd_device = SND_DEVICE_IN_SPEAKER_MIC_AEC;
                } else if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
                    if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
                        snd_device = SND_DEVICE_IN_SPEAKER_MIC_AEC;
                    } else {
                        snd_device = SND_DEVICE_IN_HANDSET_MIC_AEC;
                    }
                } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
                    snd_device = SND_DEVICE_IN_HEADSET_MIC_AEC;
                }
            }
            /* TODO: set echo reference */
        }
#endif
    } else if (source == AUDIO_SOURCE_DEFAULT) {
        goto exit;
    }

    if (snd_device != SND_DEVICE_NONE) {
        goto exit;
    }

    if (in_device != AUDIO_DEVICE_NONE &&
        !(in_device & AUDIO_DEVICE_IN_VOICE_CALL) &&
        !(in_device & AUDIO_DEVICE_IN_COMMUNICATION)) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            snd_device = SND_DEVICE_IN_EARPIECE_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
            snd_device = SND_DEVICE_IN_SPEAKER_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_HEADSET_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC ;
        } else if (in_device & AUDIO_DEVICE_IN_AUX_DIGITAL) {
            snd_device = SND_DEVICE_IN_HDMI_MIC;
        } else {
            ALOGE("%s: Unknown input device(s) %#x", __func__, in_device);
            ALOGW("%s: Using default handset-mic", __func__);
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        }
    } else {
        if (out_device & AUDIO_DEVICE_OUT_EARPIECE) {
            snd_device = SND_DEVICE_IN_EARPIECE_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_HEADSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            snd_device = SND_DEVICE_IN_SPEAKER_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET) {
            snd_device = SND_DEVICE_IN_BT_SCO_MIC;
        } else {
            ALOGE("%s: Unknown output device(s) %#x", __func__, out_device);
            ALOGW("%s: Using default handset-mic", __func__);
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        }
    }

exit:
    ALOGV("%s: exit: in_snd_device(%s)", __func__, device_table[snd_device]);

    return snd_device;
}

static int enable_snd_device(struct audio_device *adev,
                             struct audio_usecase *uc_info,
                             snd_device_t snd_device)
{
    const char *snd_device_name = get_snd_device_name(snd_device);
    struct timespec activation_time;
    struct timespec elapsed_time;

    if (snd_device_name == NULL) {
        return -EINVAL;
    }

    if (snd_device == SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES) {
        ALOGV("Request to enable combo device: enable individual devices\n");

        enable_snd_device(adev, uc_info, SND_DEVICE_OUT_SPEAKER);
        enable_snd_device(adev, uc_info, SND_DEVICE_OUT_HEADPHONES);

        return 0;
    }

    adev->snd_dev_ref_cnt[snd_device]++;
    if (adev->snd_dev_ref_cnt[snd_device] > 1) {
        ALOGV("%s: snd_device(%d: %s) is already active",
              __func__,
              snd_device,
              snd_device_name);

        return 0;
    }

    ALOGV("%s: snd_device(%d: %s)",
          __func__,
          snd_device,
          snd_device_name);

    clock_gettime(CLOCK_MONOTONIC, &activation_time);

    elapsed_time = time_spec_diff(adev->mixer.shutdown_time,
                                  activation_time);
    if (elapsed_time.tv_sec == 0) {
        long elapsed_usec = elapsed_time.tv_nsec / 1000;

        if (elapsed_usec < DAPM_STUTDOWN_TIME) {
            usleep(DAPM_STUTDOWN_TIME - elapsed_usec);
        }
    }

    audio_route_apply_and_update_path(adev->mixer.audio_route,
                                      snd_device_name);

    return 0;
}

static int disable_snd_device(struct audio_device *adev,
                              struct audio_usecase *uc_info,
                              snd_device_t snd_device)
{
    const char *snd_device_name = get_snd_device_name(snd_device);
    struct mixer_card *mixer_card;

    if (snd_device_name == NULL) {
        return -EINVAL;
    }

    if (snd_device == SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES) {
        ALOGV("Request to disable combo device: disable individual devices\n");

        disable_snd_device(adev, uc_info, SND_DEVICE_OUT_SPEAKER);
        disable_snd_device(adev, uc_info, SND_DEVICE_OUT_HEADPHONES);

        return 0;
    }

    if (adev->snd_dev_ref_cnt[snd_device] <= 0) {
        ALOGE("%s: device ref cnt is already 0", __func__);

        return -EINVAL;
    }

    adev->snd_dev_ref_cnt[snd_device]--;
    if (adev->snd_dev_ref_cnt[snd_device] == 0) {
        ALOGV("%s: snd_device(%d: %s)", __func__,
              snd_device,
              snd_device_name);

        audio_route_reset_and_update_path(adev->mixer.audio_route,
                                          snd_device_name);

        /* Store the shutdown time */
        clock_gettime(CLOCK_MONOTONIC, &adev->mixer.last_shutdown);
    }

    return 0;
}

/* TODO */
static void start_ril_call(struct audio_device *adev)
{
    switch (adev->out_device) {
    case AUDIO_DEVICE_OUT_EARPIECE:
    case AUDIO_DEVICE_OUT_SPEAKER:
        adev->two_mic_control = true;
        break;
    default:
        adev->two_mic_control = false;
        break;
    }

    if (adev->two_mic_control) {
        ALOGV("%s: enabling two mic control", __func__);
        ril_set_two_mic_control(&adev->ril, AUDIENCE, TWO_MIC_SOLUTION_ON);
    } else {
        ALOGV("%s: disabling two mic control", __func__);
        ril_set_two_mic_control(&adev->ril, AUDIENCE, TWO_MIC_SOLUTION_OFF);
    }

    adev_set_call_audio_path(adev);
    voice_set_volume(&adev->hw_device, adev->voice_volume);

    ril_set_call_clock_sync(&adev->ril, SOUND_CLOCK_START);
}

static int select_devices(struct audio_device *adev,
                          audio_usecase_t uc_id)
{
    snd_device_t out_snd_device = SND_DEVICE_NONE;
    snd_device_t in_snd_device = SND_DEVICE_NONE;
    struct audio_usecase *usecase = NULL;
    struct audio_usecase *vc_usecase = NULL;
    struct stream_in *active_input = NULL;
    struct stream_out *active_out;

    ALOGV("%s: usecase(%d)", __func__, uc_id);

    if (uc_id == USECASE_AUDIO_CAPTURE_HOTWORD) {
        return 0;
    }

    usecase = get_usecase_from_type(adev, PCM_CAPTURE|VOICE_CALL);
    if (usecase != NULL) {
        active_input = (struct stream_in *)usecase->stream;
    }

    usecase = get_usecase_from_id(adev, uc_id);
    if (usecase == NULL) {
        ALOGE("%s: Could not find the usecase(%d)", __func__, uc_id);
        return -EINVAL;
    }
    active_out = (struct stream_out *)usecase->stream;

    if (usecase->type == VOICE_CALL) {
        out_snd_device = get_output_snd_device(adev, active_out->devices);
        in_snd_device = get_input_snd_device(adev, active_out->devices);
        usecase->devices = active_out->devices;
    } else {
        /*
         * If the voice call is active, use the sound devices of voice call
         * usecase so that it would not result any device switch. All the
         * usecases will be switched to new device when select_devices() is
         * called for voice call usecase.
         */
        if (adev->voice.in_call) {
            vc_usecase = get_usecase_from_id(adev, USECASE_VOICE_CALL);
            if (usecase == NULL) {
                ALOGE("%s: Could not find the voice call usecase", __func__);
            } else {
                in_snd_device = vc_usecase->in_snd_device;
                out_snd_device = vc_usecase->out_snd_device;
            }
        }

        if (usecase->type == PCM_PLAYBACK) {
            usecase->devices = active_out->devices;
            in_snd_device = SND_DEVICE_NONE;
            if (out_snd_device == SND_DEVICE_NONE) {
                out_snd_device = get_output_snd_device(adev,
                                                       active_out->devices);
                if (active_out == adev->primary_output &&
                    active_input &&
                    active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION) {
                    select_devices(adev, active_input->usecase);
                }
            }
        } else if (usecase->type == PCM_CAPTURE) {
            usecase->devices = ((struct stream_in *)usecase->stream)->devices;
            out_snd_device = SND_DEVICE_NONE;
            if (in_snd_device == SND_DEVICE_NONE) {
                if (active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION &&
                    adev->primary_output && !adev->primary_output->standby) {
                    in_snd_device = get_input_snd_device(adev,
                                                         adev->primary_output->devices);
                } else {
                    in_snd_device = get_input_snd_device(adev,
                                                         AUDIO_DEVICE_NONE);
                }
            }
        }
    }

    if (out_snd_device == usecase->out_snd_device &&
        in_snd_device == usecase->in_snd_device) {
        return 0;
    }

    ALOGV("%s: out_snd_device(%d: %s) in_snd_device(%d: %s)",
          __func__,
          out_snd_device,
          get_snd_device_display_name(out_snd_device),
          in_snd_device,
          get_snd_device_display_name(in_snd_device));


    /* Disable current sound devices */
    if (usecase->out_snd_device != SND_DEVICE_NONE) {
        disable_snd_device(adev, usecase, usecase->out_snd_device);
    }

    if (usecase->in_snd_device != SND_DEVICE_NONE) {
        disable_snd_device(adev, usecase, usecase->in_snd_device);
    }

    /*
     * Already tell the modem that we are in a call. This should make it
     * faster to accept an incoming call.
     */
    if (adev->voice.in_call) {
        start_ril_call(adev);
    }

    /* Enable new sound devices */
    if (out_snd_device != SND_DEVICE_NONE) {
        enable_snd_device(adev, usecase, out_snd_device);
    }

    if (in_snd_device != SND_DEVICE_NONE) {
        enable_snd_device(adev, usecase, in_snd_device);
    }

    usecase->in_snd_device = in_snd_device;
    usecase->out_snd_device = out_snd_device;

    return 0;
}

static int get_playback_delay(struct stream_out *out,
                              size_t frames,
                              struct echo_reference_buffer *buffer)
{
    struct pcm_device *pcm_device;
    unsigned int kernel_frames;
    int status;
    int primary_pcm = 0;

    pcm_device = node_to_item(list_head(&out->pcm_dev_list),
                              struct pcm_device, stream_list_node);

    status = pcm_get_htimestamp(pcm_device->pcm,
                                &kernel_frames,
                                &buffer->time_stamp);
    if (status < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        ALOGV("get_playback_delay(): pcm_get_htimestamp error,"
                "setting playbackTimestamp to 0");
        return status;
    }

    kernel_frames = pcm_get_buffer_size(pcm_device->pcm) - kernel_frames;

    /* adjust render time stamp with delay added by current driver buffer.
     * Add the duration of current frame as we want the render time of the last
     * sample being written. */
    buffer->delay_ns = (long)(((int64_t)(kernel_frames + frames) * 1000000000) /
                       out->config.rate);

    ALOGT("get_playback_delay_time_stamp: secs: [%10ld], nsecs: [%9ld], "
          "kernel_frames: [%5u], delay_ns: [%d],",
          buffer->time_stamp.tv_sec,
          buffer->time_stamp.tv_nsec,
          kernel_frames,
          buffer->delay_ns);

    return 0;
}

/* This function reads PCM data and:
 * - resample if needed
 * - process if pre-processors are attached
 * - discard unwanted channels
 */
static ssize_t read_and_process_frames(struct stream_in *in, void* buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;
    audio_buffer_t in_buf;
    audio_buffer_t out_buf;
    size_t src_channels = in->config.channels;
    size_t dst_channels = audio_channel_count_from_in_mask(in->main_channels);
    int i;
    void *proc_buf_out;
    struct pcm_device *pcm_device;
    bool has_additional_channels = (dst_channels != src_channels) ? true : false;

    /* Additional channels might be added on top of main_channels:
    * - aux_channels (by processing effects)
    * - extra channels due to HW limitations
    * In case of additional channels, we cannot work inplace
    */
    if (has_additional_channels) {
        proc_buf_out = in->proc_buf_out;
    } else {
        proc_buf_out = buffer;
    }

    if (list_empty(&in->pcm_dev_list)) {
        ALOGE("%s: pcm device list empty", __func__);
        return -EINVAL;
    }

    pcm_device = node_to_item(list_head(&in->pcm_dev_list),
                              struct pcm_device,
                              stream_list_node);

    /* No processing effects attached */
    if (has_additional_channels) {
        /* With additional channels, we cannot use original buffer */
        if (in->proc_buf_size < (size_t)frames) {
            size_t size_in_bytes = pcm_frames_to_bytes(pcm_device->pcm, frames);
            in->proc_buf_size = (size_t)frames;
            in->proc_buf_out = (int16_t *)realloc(in->proc_buf_out,
                                                  size_in_bytes);
            ALOG_ASSERT((in->proc_buf_out != NULL),
                        "process_frames() failed to reallocate proc_buf_out");
            proc_buf_out = in->proc_buf_out;
        }
    }
    frames_wr = read_frames(in, proc_buf_out, frames);

    /* Remove all additional channels that have been added on top of main_channels:
     * - aux_channels
     * - extra channels from HW due to HW limitations
     * Assumption is made that the channels are interleaved and that the main
     * channels are first. */

    if (has_additional_channels) {
        int16_t *src_buffer = (int16_t *)proc_buf_out;
        int16_t *dst_buffer = (int16_t *)buffer;

        if (dst_channels == 1) {
            for (i = frames_wr; i > 0; i--) {
                *dst_buffer++ = *src_buffer;
                src_buffer += src_channels;
            }
        } else {
            for (i = frames_wr; i > 0; i--) {
                memcpy(dst_buffer, src_buffer, dst_channels*sizeof(int16_t));
                dst_buffer += dst_channels;
                src_buffer += src_channels;
            }
        }
    }

    return frames_wr;
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                           struct resampler_buffer *buffer)
{
    struct stream_in *in;
    struct pcm_device *pcm_device;

    if (buffer_provider == NULL || buffer == NULL) {
        return -EINVAL;
    }

    in = (struct stream_in *)((char *)buffer_provider -
            offsetof(struct stream_in, buf_provider));

    if (list_empty(&in->pcm_dev_list)) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    pcm_device = node_to_item(list_head(&in->pcm_dev_list),
                              struct pcm_device,
                              stream_list_node);

    if (in->read_buf_frames == 0) {
        size_t size_in_bytes = pcm_frames_to_bytes(pcm_device->pcm,
                                                   in->config.period_size);
        if (in->read_buf_size < in->config.period_size) {
            in->read_buf_size = in->config.period_size;
            in->read_buf = (int16_t *)realloc(in->read_buf, size_in_bytes);
            ALOG_ASSERT((in->read_buf != NULL),
                        "get_next_buffer() failed to reallocate read_buf");
        }

        in->read_status = pcm_read(pcm_device->pcm,
                                   (void*)in->read_buf,
                                   size_in_bytes);

        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }
        in->read_buf_frames = in->config.period_size;
    }

    buffer->frame_count = (buffer->frame_count > in->read_buf_frames) ?
                            in->read_buf_frames :
                            buffer->frame_count;
    buffer->i16 =
        in->read_buf +
        (in->config.period_size - in->read_buf_frames) * in->config.channels;

    return in->read_status;
}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                           struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    in->read_buf_frames -= buffer->frame_count;
}

/*
 * read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer
 * specified
 */
static ssize_t read_frames(struct stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;

    struct pcm_device *pcm_device;

    if (list_empty(&in->pcm_dev_list)) {
        ALOGE("%s: pcm device list empty", __func__);
        return -EINVAL;
    }

    pcm_device = node_to_item(list_head(&in->pcm_dev_list),
                              struct pcm_device,
                              stream_list_node);

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;

        ALOGT("%s: frames_rd: %zd, frames_wr: %zd, in->config.channels: %d",
               __func__,
               frames_rd,
               frames_wr,
               in->config.channels);

        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer + pcm_frames_to_bytes(pcm_device->pcm, frames_wr)),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                .raw = NULL,
                .frame_count = frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                            pcm_frames_to_bytes(pcm_device->pcm, frames_wr),
                        buf.raw,
                        pcm_frames_to_bytes(pcm_device->pcm, buf.frame_count));
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }

    return frames_wr;
}

static int in_release_pcm_devices(struct stream_in *in)
{
    struct pcm_device *pcm_device;
    struct listnode *node;
    struct listnode *next;

    list_for_each_safe(node, next, &in->pcm_dev_list) {
        pcm_device = node_to_item(node, struct pcm_device, stream_list_node);
        list_remove(node);
        free(pcm_device);
    }

    return 0;
}

static int stop_input_stream(struct stream_in *in)
{
    struct audio_usecase *uc_info;
    struct audio_device *adev = in->dev;

    adev->active_input = NULL;

    ALOGV("%s: enter: usecase(%d: %s)",
          __func__,
          in->usecase,
          use_case_table[in->usecase]);

    uc_info = get_usecase_from_id(adev, in->usecase);
    if (uc_info == NULL) {
        ALOGE("%s: Could not find the usecase (%d) in the list",
              __func__,
              in->usecase);
        return -EINVAL;
    }

    /* Disable the tx device */
    disable_snd_device(adev, uc_info, uc_info->in_snd_device);

    list_remove(&uc_info->adev_list_node);
    free(uc_info);

    if (list_empty(&in->pcm_dev_list)) {
        ALOGE("%s: pcm device list empty", __func__);
        return -EINVAL;
    }

    in_release_pcm_devices(in);
    list_init(&in->pcm_dev_list);

    ALOGV("%s: exit", __func__);

    return 0;
}

static int start_input_stream(struct stream_in *in)
{
    /* Enable output device and stream routing controls */
    struct pcm_device_profile *pcm_profile;
    struct audio_device *adev = in->dev;
    struct audio_usecase *uc_info;
    struct pcm_device *pcm_device;
    bool recreate_resampler = false;
    int ret = 0;

    ALOGV("%s: enter: usecase(%d)", __func__, in->usecase);

    adev->active_input = in;

    pcm_profile = get_pcm_device(in->usecase_type, in->devices);
    if (pcm_profile == NULL) {
        ALOGE("%s: Could not find PCM device id for the usecase(%d)",
              __func__,
              in->usecase);
        ret = -EINVAL;
        goto error_config;
    }

    uc_info = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));
    if (uc_info == NULL) {
        ret = -ENOMEM;
        goto error_config;
    }

    uc_info->id = in->usecase;
    uc_info->type = PCM_CAPTURE;
    uc_info->stream = (struct audio_stream *)in;
    uc_info->devices = in->devices;
    uc_info->in_snd_device = SND_DEVICE_NONE;
    uc_info->out_snd_device = SND_DEVICE_NONE;

    pcm_device = (struct pcm_device *)calloc(1, sizeof(struct pcm_device));
    if (pcm_device == NULL) {
        free(uc_info);
        ret = -ENOMEM;
        goto error_config;
    }

    pcm_device->pcm_profile = pcm_profile;
    list_init(&in->pcm_dev_list);
    list_add_tail(&in->pcm_dev_list, &pcm_device->stream_list_node);

    list_add_tail(&adev->usecase_list, &uc_info->adev_list_node);

    select_devices(adev, in->usecase);

    /* Config should be updated as profile can be changed between different calls
     * to this function:
     * - Trigger resampler creation
     * - Config needs to be updated */
    if (in->config.rate != pcm_profile->config.rate) {
        recreate_resampler = true;
    }
    in->config = pcm_profile->config;

    if (in->requested_rate != in->config.rate) {
        recreate_resampler = true;
    }

    if (recreate_resampler) {
        if (in->resampler) {
            release_resampler(in->resampler);
            in->resampler = NULL;
        }
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;
        ret = create_resampler(in->config.rate,
                               in->requested_rate,
                               in->config.channels,
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
    }

    /*
     * Open the PCM device.
     * The HW is limited to support only the default pcm_profile settings.  As
     * such a change in aux_channels will not have an effect.
     */
    ALOGV("%s: Opening PCM device card_id(%d) device_id(%d), channels %d, "
          "smp rate %d format %d, period_size %d",
          __func__,
          pcm_device->pcm_profile->card,
          pcm_device->pcm_profile->id,
          pcm_device->pcm_profile->config.channels,
          pcm_device->pcm_profile->config.rate,
          pcm_device->pcm_profile->config.format,
          pcm_device->pcm_profile->config.period_size);

    pcm_device->pcm = pcm_open(pcm_device->pcm_profile->card,
                               pcm_device->pcm_profile->id,
                               PCM_IN|PCM_MONOTONIC,
                               &pcm_device->pcm_profile->config);

    if (pcm_device->pcm && !pcm_is_ready(pcm_device->pcm)) {
        ALOGE("%s: %s", __func__, pcm_get_error(pcm_device->pcm));
        pcm_close(pcm_device->pcm);
        pcm_device->pcm = NULL;
        ret = -EIO;
        goto error_open;
    }

    /* force read and proc buffer reallocation in case of frame size or
     * channel count change */
    in->proc_buf_frames = 0;
    in->proc_buf_size = 0;
    in->read_buf_size = 0;
    in->read_buf_frames = 0;

    /* if no supported sample rate is available, use the resampler */
    if (in->resampler) {
        in->resampler->reset(in->resampler);
    }

    ALOGV("%s: exit", __func__);

    return ret;

error_open:
    if (in->resampler) {
        release_resampler(in->resampler);
        in->resampler = NULL;
    }
    stop_input_stream(in);

error_config:
    ALOGV("%s: exit: status(%d)", __func__, ret);

    adev->active_input = NULL;

    return ret;
}

static void lock_input_stream(struct stream_in *in)
{
    pthread_mutex_lock(&in->pre_lock);
    pthread_mutex_lock(&in->lock);
    pthread_mutex_unlock(&in->pre_lock);
}

static void unlock_input_stream(struct stream_in *in)
{
    pthread_mutex_unlock(&in->lock);
}

static void lock_output_stream(struct stream_out *out)
{
    pthread_mutex_lock(&out->pre_lock);
    pthread_mutex_lock(&out->lock);
    pthread_mutex_unlock(&out->pre_lock);
}

static void unlock_output_stream(struct stream_out *out)
{
    pthread_mutex_unlock(&out->lock);
}

static int uc_release_pcm_devices(struct audio_usecase *usecase)
{
    struct stream_out *out = (struct stream_out *)usecase->stream;
    struct pcm_device *pcm_device;
    struct listnode *node;
    struct listnode *next;

    list_for_each_safe(node, next, &out->pcm_dev_list) {
        pcm_device = node_to_item(node, struct pcm_device, stream_list_node);
        list_remove(node);
        free(pcm_device);
    }

    return 0;
}

static int uc_select_pcm_devices(struct audio_usecase *usecase)
{
    struct stream_out *out = (struct stream_out *)usecase->stream;
    struct pcm_device *pcm_device;
    struct pcm_device_profile *pcm_profile;
    struct mixer_card *mixer_card;
    audio_devices_t devices = usecase->devices;

    list_init(&out->pcm_dev_list);


    pcm_profile = get_pcm_device(usecase->type, devices);
    while (pcm_profile != NULL) {
        pcm_device = calloc(1, sizeof(struct pcm_device));
        if (pcm_device == NULL) {
            return -ENOMEM;
        }

        pcm_device->pcm_profile = pcm_profile;
        list_add_tail(&out->pcm_dev_list, &pcm_device->stream_list_node);
        devices &= ~pcm_profile->devices;

        pcm_profile = get_pcm_device(usecase->type, devices);
    }

    return 0;
}

static int out_close_pcm_devices(struct stream_out *out)
{
    struct pcm_device *pcm_device;
    struct listnode *node;
    struct audio_device *adev = out->dev;

    list_for_each(node, &out->pcm_dev_list) {
        pcm_device = node_to_item(node, struct pcm_device, stream_list_node);
        if (pcm_device->pcm) {
            pcm_close(pcm_device->pcm);
            pcm_device->pcm = NULL;
        }

        if (pcm_device->resampler) {
            release_resampler(pcm_device->resampler);
            pcm_device->resampler = NULL;
        }

        if (pcm_device->res_buffer) {
            free(pcm_device->res_buffer);
            pcm_device->res_buffer = NULL;
        }
    }

    return 0;
}

static int out_open_pcm_devices(struct stream_out *out)
{
    struct pcm_device *pcm_device;
    struct listnode *node;
    int ret = 0;

    list_for_each(node, &out->pcm_dev_list) {
        pcm_device = node_to_item(node, struct pcm_device, stream_list_node);

        ALOGV("%s: Opening PCM device card_id(%d) device_id(%d)",
              __func__,
              pcm_device->pcm_profile->card,
              pcm_device->pcm_profile->id);

        pcm_device->pcm = pcm_open(pcm_device->pcm_profile->card,
                                   pcm_device->pcm_profile->id,
                                   PCM_OUT|PCM_MONOTONIC,
                                   &pcm_device->pcm_profile->config);
        if (pcm_device->pcm && !pcm_is_ready(pcm_device->pcm)) {
            ALOGE("%s: %s", __func__, pcm_get_error(pcm_device->pcm));
            pcm_device->pcm = NULL;
            ret = -EIO;
            goto error_open;
        }

        /*
        * If the stream rate differs from the PCM rate, we need to
        * create a resampler.
        */
        if (out->sample_rate != pcm_device->pcm_profile->config.rate) {
            ALOGV("%s: create_resampler(), pcm_device_card(%d), "
                  "pcm_device_id(%d), out_rate(%d), device_rate(%d)",
                  __func__,
                  pcm_device->pcm_profile->card,
                  pcm_device->pcm_profile->id,
                  out->sample_rate,
                  pcm_device->pcm_profile->config.rate);

            ret = create_resampler(out->sample_rate,
                                   pcm_device->pcm_profile->config.rate,
                                   audio_channel_count_from_out_mask(out->channel_mask),
                                   RESAMPLER_QUALITY_DEFAULT,
                                   NULL,
                                   &pcm_device->resampler);
            pcm_device->res_byte_count = 0;
            pcm_device->res_buffer = NULL;
        }
    }

    return ret;

error_open:
    out_close_pcm_devices(out);

    return ret;
}

static int disable_output_path_l(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    struct audio_usecase *uc_info;

    uc_info = get_usecase_from_id(adev, out->usecase);
    if (uc_info == NULL) {
        ALOGE("%s: Could not find the usecase (%d) in the list",
             __func__,
             out->usecase);
        return -EINVAL;
    }

    disable_snd_device(adev, uc_info, uc_info->out_snd_device);
    uc_release_pcm_devices(uc_info);
    list_remove(&uc_info->adev_list_node);
    free(uc_info);

    return 0;
}

static int enable_output_path_l(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    struct audio_usecase *uc_info;

    uc_info = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));
    if (uc_info == NULL) {
        return -ENOMEM;
    }
    uc_info->id = out->usecase;
    uc_info->type = PCM_PLAYBACK;
    uc_info->stream = (struct audio_stream *)out;
    uc_info->devices = out->devices;
    uc_info->in_snd_device = SND_DEVICE_NONE;
    uc_info->out_snd_device = SND_DEVICE_NONE;
    uc_select_pcm_devices(uc_info);

    list_add_tail(&adev->usecase_list, &uc_info->adev_list_node);
    select_devices(adev, out->usecase);

    return 0;
}

static int stop_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int ret = 0;

    ALOGV("%s: enter: usecase(%d: %s)", __func__,
          out->usecase,
          use_case_table[out->usecase]);

    ret = disable_output_path_l(out);

    ALOGV("%s: exit: status(%d)", __func__, ret);

    return ret;
}

static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int ret = 0;

    ALOGV("%s: enter: usecase(%d: %s) devices(%#x) channels(%d)",
          __func__,
          out->usecase,
          use_case_table[out->usecase],
          out->devices, out->config.channels);

    ret = enable_output_path_l(out);
    if (ret != 0) {
        return ret;
    }

    out->compr = NULL;
    ret = out_open_pcm_devices(out);
    if (ret != 0) {
        goto error_open;
    }

    ALOGV("%s: exit", __func__);

    return 0;

error_open:
    stop_output_stream(out);

    return ret;
}

static int stop_voice_call(struct audio_device *adev)
{
    struct audio_usecase *uc_info;

    ALOGV("%s: enter", __func__);
    if (!adev->voice.in_call) {
        return;
    }

    adev->voice.in_call = false;

    /* Do not change devices if we are switching to WB */
    if (adev->mode != AUDIO_MODE_IN_CALL) {
        uc_info = get_usecase_from_id(adev, USECASE_VOICE_CALL);
        if (uc_info == NULL) {
            ALOGE("%s: Could not find the usecase (%d) in the list",
                  __func__, USECASE_VOICE_CALL);
            return -EINVAL;
        }

        disable_snd_device(adev, uc_info, uc_info->out_snd_device);
        disable_snd_device(adev, uc_info, uc_info->in_snd_device);

        uc_release_pcm_devices(uc_info);
        list_remove(&uc_info->adev_list_node);
        free(uc_info);
    }

    ALOGV("%s: exit", __func__);

    return 0;
}

/* always called with adev lock held */
static int start_voice_call(struct audio_device *adev)
{
    struct audio_usecase *uc_info;

    ALOGV("%s: enter", __func__);

    uc_info = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));
    uc_info->id = USECASE_VOICE_CALL;
    uc_info->type = VOICE_CALL;
    uc_info->stream = (struct audio_stream *)adev->primary_output;
    uc_info->devices = adev->primary_output->devices;
    uc_info->in_snd_device = SND_DEVICE_NONE;
    uc_info->out_snd_device = SND_DEVICE_NONE;

    uc_select_pcm_devices(uc_info);

    list_add_tail(&adev->usecase_list, &uc_info->adev_list_node);

    select_devices(adev, USECASE_VOICE_CALL);


    /* TODO: implement voice call start */

    /* set cached volume */
    set_voice_volume_l(adev, adev->voice_volume);

    adev->in_call = true;
    ALOGV("%s: exit", __func__);
    return 0;
}

static int check_input_parameters(uint32_t sample_rate,
                                  audio_format_t format,
                                  int channel_count)
{
    if (format != AUDIO_FORMAT_PCM_16_BIT) {
        return -EINVAL;
    }

    if ((channel_count < 1) || (channel_count > 2)) {
        return -EINVAL;
    }

    switch (sample_rate) {
    case 8000:
    case 11025:
    case 12000:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static size_t get_input_buffer_size(uint32_t sample_rate,
                                    audio_format_t format,
                                    int channel_count,
                                    usecase_type_t usecase_type,
                                    audio_devices_t devices)
{
    struct pcm_device_profile *pcm_profile;
    size_t size = 0;
    int ret;

    ret = check_input_parameters(sample_rate, format, channel_count);
    if (ret != 0) {
        return 0;
    }

    pcm_profile = get_pcm_device(usecase_type, devices);
    if (pcm_profile == NULL)
        return 0;

    /*
     * Take resampling into account and return the closest majoring multiple of
     * 16 frames, as audioflinger expects audio buffers to be a multiple of 16
     * frames.
     */
    size = (pcm_profile->config.period_size * sample_rate) / pcm_profile->config.rate;
    size = ((size + 15) / 16) * 16;

    return (size * channel_count * audio_bytes_per_sample(format));
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->sample_rate;
}

static int out_set_sample_rate(struct audio_stream *stream __unused,
                               uint32_t rate __unused)
{
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->config.period_size *
        audio_stream_out_frame_size((const struct audio_stream_out *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->format;
}

static int out_set_format(struct audio_stream *stream __unused,
                          audio_format_t format __unused)
{
    return -ENOSYS;
}

static int do_out_standby_l(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int status = 0;

    out->standby = true;
    stop_compressed_output_l(out);
    out->gapless_mdata.encoder_delay = 0;
    out->gapless_mdata.encoder_padding = 0;
    if (out->compr != NULL) {
        compress_close(out->compr);
        out->compr = NULL;
    }
    status = stop_output_stream(out);

    return status;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;

    ALOGV("%s: enter: usecase(%d: %s)",
          __func__,
          out->usecase,
          use_case_table[out->usecase]);

    lock_output_stream(out);
    if (!out->standby) {
        pthread_mutex_lock(&adev->lock);
        do_out_standby_l(out);
        pthread_mutex_unlock(&adev->lock);
    }
    unlock_output_stream(out);

    ALOGV("%s: exit", __func__);

    return 0;
}

static int out_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

/**********************************************************
 * Samsung RIL functions
 **********************************************************/

/*
 * This function must be called with hw device mutex locked, OK to hold other
 * mutexes
 */
static int start_voice_call(struct audio_device *adev)
{
    struct pcm_config *voice_config;

    if (adev->pcm_voice_rx != NULL || adev->pcm_voice_tx != NULL) {
        ALOGW("%s: Voice PCMs already open!\n", __func__);
        return 0;
    }

    ALOGV("%s: Opening voice PCMs", __func__);

    if (adev->wb_amr) {
        voice_config = &pcm_config_voice_wide;
    } else {
        voice_config = &pcm_config_voice;
    }

    /* Open modem PCM channels */
    adev->pcm_voice_rx = pcm_open(PCM_CARD,
                                  PCM_DEVICE_VOICE,
                                  PCM_OUT | PCM_MONOTONIC,
                                  voice_config);
    if (adev->pcm_voice_rx != NULL && !pcm_is_ready(adev->pcm_voice_rx)) {
        ALOGE("%s: cannot open PCM voice RX stream: %s",
              __func__, pcm_get_error(adev->pcm_voice_rx));
        goto err_voice_rx;
    }

    adev->pcm_voice_tx = pcm_open(PCM_CARD,
                                  PCM_DEVICE_VOICE,
                                  PCM_IN | PCM_MONOTONIC,
                                  voice_config);
    if (adev->pcm_voice_tx != NULL && !pcm_is_ready(adev->pcm_voice_tx)) {
        ALOGE("%s: cannot open PCM voice TX stream: %s",
              __func__, pcm_get_error(adev->pcm_voice_tx));
        goto err_voice_tx;
    }

    pcm_start(adev->pcm_voice_rx);
    pcm_start(adev->pcm_voice_tx);

    /* start SCO stream if needed */
    if (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
        start_bt_sco(adev);
    }

    return 0;

err_voice_tx:
    pcm_close(adev->pcm_voice_tx);
    adev->pcm_voice_tx = NULL;
err_voice_rx:
    pcm_close(adev->pcm_voice_rx);
    adev->pcm_voice_rx = NULL;

    return -ENOMEM;
}

/*
 * This function must be called with hw device mutex locked, OK to hold other
 * mutexes
 */
static void stop_voice_call(struct audio_device *adev)
{
    int status = 0;

    ALOGV("%s: Closing active PCMs", __func__);

    if (adev->pcm_voice_rx) {
        pcm_stop(adev->pcm_voice_rx);
        pcm_close(adev->pcm_voice_rx);
        adev->pcm_voice_rx = NULL;
        status++;
    }

    if (adev->pcm_voice_tx) {
        pcm_stop(adev->pcm_voice_tx);
        pcm_close(adev->pcm_voice_tx);
        adev->pcm_voice_tx = NULL;
        status++;
    }

    /* End SCO stream if needed */
    if (adev->out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
        stop_bt_sco(adev);
        status++;
    }

    ALOGV("%s: Successfully closed %d active PCMs", __func__, status);
}

static void start_ril_call(struct audio_device *adev)
{
    switch (adev->out_device) {
    case AUDIO_DEVICE_OUT_EARPIECE:
    case AUDIO_DEVICE_OUT_SPEAKER:
        adev->two_mic_control = true;
        break;
    default:
        adev->two_mic_control = false;
        break;
    }

    if (adev->two_mic_control) {
        ALOGV("%s: enabling two mic control", __func__);
        ril_set_two_mic_control(&adev->ril, AUDIENCE, TWO_MIC_SOLUTION_ON);
    } else {
        ALOGV("%s: disabling two mic control", __func__);
        ril_set_two_mic_control(&adev->ril, AUDIENCE, TWO_MIC_SOLUTION_OFF);
    }

    adev_set_call_audio_path(adev);
    voice_set_volume(&adev->hw_device, adev->voice_volume);

    ril_set_call_clock_sync(&adev->ril, SOUND_CLOCK_START);
}

static void start_call(struct audio_device *adev)
{
    if (adev->in_call) {
        return;
    }

    adev->in_call = true;

    if (adev->out_device == AUDIO_DEVICE_NONE &&
        adev->in_device == AUDIO_DEVICE_NONE) {
        ALOGV("%s: No device selected, use earpiece as the default",
              __func__);
        adev->out_device = AUDIO_DEVICE_OUT_EARPIECE;
    }
    adev->input_source = AUDIO_SOURCE_VOICE_CALL;

    select_devices(adev);
    start_voice_call(adev);
}

static void stop_call(struct audio_device *adev)
{
    if (!adev->in_call) {
        return;
    }

    ril_set_call_clock_sync(&adev->ril, SOUND_CLOCK_STOP);
    stop_voice_call(adev);

    /* Do not change devices if we are switching to WB */
    if (adev->mode != AUDIO_MODE_IN_CALL) {
        /* Use speaker as the default. We do not want to stay in earpiece mode */
        if (adev->out_device == AUDIO_DEVICE_NONE ||
            adev->out_device == AUDIO_DEVICE_OUT_EARPIECE) {
            adev->out_device = AUDIO_DEVICE_OUT_SPEAKER;
        }
        adev->input_source = AUDIO_SOURCE_DEFAULT;

        ALOGV("*** %s: Reset route to out devices=%#x, input src=%#x",
              __func__,
              adev->out_device,
              adev->input_source);

        adev->in_call = false;
        select_devices(adev);
    }

    adev->in_call = false;
}

static void adev_set_wb_amr_callback(void *data, int enable)
{
    struct audio_device *adev = (struct audio_device *)data;

    pthread_mutex_lock(&adev->lock);

    if (adev->wb_amr != enable) {
        adev->wb_amr = enable;

        /* reopen the modem PCMs at the new rate */
        if (adev->in_call && route_changed(adev)) {
            ALOGV("%s: %s Incall Wide Band support",
                  __func__,
                  enable ? "Turn on" : "Turn off");

            stop_call(adev);
            start_call(adev);
        }
    }

    pthread_mutex_unlock(&adev->lock);
}

static void adev_set_call_audio_path(struct audio_device *adev)
{
    enum _AudioPath device_type;

    switch(adev->out_device) {
        case AUDIO_DEVICE_OUT_SPEAKER:
            device_type = SOUND_AUDIO_PATH_SPEAKER;
            break;
        case AUDIO_DEVICE_OUT_EARPIECE:
            device_type = SOUND_AUDIO_PATH_HANDSET;
            break;
        case AUDIO_DEVICE_OUT_WIRED_HEADSET:
            device_type = SOUND_AUDIO_PATH_HEADSET;
            break;
        case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
            device_type = SOUND_AUDIO_PATH_HEADPHONE;
            break;
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            device_type = SOUND_AUDIO_PATH_BLUETOOTH;
            break;
        default:
            /* if output device isn't supported, use handset by default */
            device_type = SOUND_AUDIO_PATH_HANDSET;
            break;
    }

    ALOGV("%s: ril_set_call_audio_path(%d)", __func__, device_type);

    ril_set_call_audio_path(&adev->ril, device_type);
}

/* must be called with hw device outputs list, output stream, and hw device mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;

    ALOGV("%s: starting stream", __func__);

    if (out == adev->outputs[OUTPUT_HDMI]) {
        force_non_hdmi_out_standby(adev);
    } else if (adev->outputs[OUTPUT_HDMI] && !adev->outputs[OUTPUT_HDMI]->standby) {
        out->disabled = true;
        return 0;
    }

    out->disabled = false;

    if (out->device & (AUDIO_DEVICE_OUT_SPEAKER |
                       AUDIO_DEVICE_OUT_WIRED_HEADSET |
                       AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
                       AUDIO_DEVICE_OUT_AUX_DIGITAL |
                       AUDIO_DEVICE_OUT_ALL_SCO)) {
        out->pcm[PCM_CARD] = pcm_open(PCM_CARD,
                                      out->pcm_device,
                                      PCM_OUT | PCM_MONOTONIC,
                                      &out->config);
        if (out->pcm[PCM_CARD] && !pcm_is_ready(out->pcm[PCM_CARD])) {
            ALOGE("pcm_open(PCM_CARD) failed: %s",
                  pcm_get_error(out->pcm[PCM_CARD]));
            pcm_close(out->pcm[PCM_CARD]);
            return -ENOMEM;
        }
    }

    if (out->device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
        out->pcm[PCM_CARD_SPDIF] = pcm_open(PCM_CARD_SPDIF,
                                            out->pcm_device,
                                            PCM_OUT | PCM_MONOTONIC,
                                            &out->config);
        if (out->pcm[PCM_CARD_SPDIF] &&
                !pcm_is_ready(out->pcm[PCM_CARD_SPDIF])) {
            ALOGE("pcm_open(PCM_CARD_SPDIF) failed: %s",
                  pcm_get_error(out->pcm[PCM_CARD_SPDIF]));
            pcm_close(out->pcm[PCM_CARD_SPDIF]);
            return -ENOMEM;
        }
    }

    /* in call routing must go through set_parameters */
    if (!adev->in_call) {
        adev->out_device |= out->device;
        select_devices(adev);
    }

    if (out->device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        set_hdmi_channels(adev, out->config.channels);
    }

    ALOGV("%s: stream out device: %d, actual: %d",
          __func__, out->device, adev->out_device);

    return 0;
}

/* must be called with input stream and hw device mutexes locked */
static int start_input_stream(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

    in->pcm = pcm_open(PCM_CARD,
                       PCM_DEVICE,
                       PCM_IN | PCM_MONOTONIC,
                       in->config);
    if (in->pcm && !pcm_is_ready(in->pcm)) {
        ALOGE("pcm_open() failed: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }

    /* if no supported sample rate is available, use the resampler */
    if (in->resampler) {
        in->resampler->reset(in->resampler);
    }

    in->frames_in = 0;
    in->buffer_size = 0;

    /* in call routing must go through set_parameters */
    if (!adev->in_call) {
        adev->input_source = in->input_source;
        adev->in_device = in->device;
        adev->in_channel_mask = in->channel_mask;

        select_devices(adev);
    }

    /* initialize volume ramp */
    in->ramp_frames = (CAPTURE_START_RAMP_MS * in->requested_rate) / 1000;
    in->ramp_step = (uint16_t)(USHRT_MAX / in->ramp_frames);
    in->ramp_vol = 0;

    return 0;
}

static size_t get_input_buffer_size(unsigned int sample_rate,
                                    audio_format_t format,
                                    unsigned int channel_count,
                                    bool is_low_latency)
{
    const struct pcm_config *config = is_low_latency ?
            &pcm_config_in_low_latency : &pcm_config_in;
    size_t size;

    /*
     * take resampling into account and return the closest majoring
     * multiple of 16 frames, as audioflinger expects audio buffers to
     * be a multiple of 16 frames
     */
    size = (config->period_size * sample_rate) / config->rate;
    size = ((size + 15) / 16) * 16;

    return size * channel_count * audio_bytes_per_sample(format);
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                           struct resampler_buffer* buffer)
{
    struct stream_in *in;
    size_t i;

    if (buffer_provider == NULL || buffer == NULL) {
        return -EINVAL;
    }

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->frames_in == 0) {
        size_t size_in_bytes = pcm_frames_to_bytes(in->pcm,
                                                   in->config->period_size);
        if (in->buffer_size < in->config->period_size) {
            in->buffer_size = in->config->period_size;
            in->buffer = (int16_t *)realloc(in->buffer, size_in_bytes);
            ALOG_ASSERT((in->buffer != NULL),
                        "%s: failed to reallocate read_buf", __func__);
        }
        in->read_status = pcm_read(in->pcm,
                                   (void *)in->buffer,
                                   size_in_bytes);
        if (in->read_status != 0) {
            ALOGE("%s: pcm_read error %d", __func__, in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }

        in->frames_in = in->config->period_size;

        /* Do stereo to mono conversion in place by discarding right channel */
        if (in->channel_mask == AUDIO_CHANNEL_IN_MONO)
            for (i = 1; i < in->frames_in; i++)
                in->buffer[i] = in->buffer[i * 2];
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer +
            (in->config->period_size - in->frames_in) *
                audio_channel_count_from_in_mask(in->channel_mask);

    return in->read_status;

}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer)
{
    struct stream_in *in;

    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct stream_in *)((char *)buffer_provider -
                                   offsetof(struct stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;

        ALOGT("%s: frames_rd: %zd, frames_wr: %zd, in->config.channels: %d\n",
              __func__,
              frames_rd,
              frames_wr,
              in->config->channels);

        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer + pcm_frames_to_bytes(in->pcm, frames_wr)),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                .raw = NULL,
                .frame_count = frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer + pcm_frames_to_bytes(in->pcm, frames_wr),
                        buf.raw,
                        pcm_frames_to_bytes(in->pcm, buf.frame_count));
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }

    return frames_wr;
}

/* API functions */

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream __unused,
                               uint32_t rate __unused)
{
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->config.period_size *
           audio_stream_out_frame_size((const struct audio_stream_out *)stream);
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->channel_mask;
}

static audio_format_t out_get_format(const struct audio_stream *stream __unused)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream __unused,
                          audio_format_t format __unused)
{
    return -ENOSYS;
}

/* Return the set of output devices associated with active streams
 * other than out.  Assumes out is non-NULL and out->dev is locked.
 */
static audio_devices_t output_devices(struct stream_out *out)
{
    struct audio_device *dev = out->dev;
    enum output_type type;
    audio_devices_t devices = AUDIO_DEVICE_NONE;

    for (type = 0; type < OUTPUT_TOTAL; ++type) {
        struct stream_out *other = dev->outputs[type];
        if (other && (other != out) && !other->standby) {
            // TODO no longer accurate
            /* safe to access other stream without a mutex,
             * because we hold the dev lock,
             * which prevents the other stream from being closed
             */
            devices |= other->device;
        }
    }

    return devices;
}

/* must be called with hw device outputs list, all out streams, and hw device mutex locked */
static void do_out_standby(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int i;

    ALOGV("%s: output standby: %d", __func__, out->standby);

    if (!out->standby) {
        for (i = 0; i < PCM_TOTAL; i++) {
            if (out->pcm[i]) {
                pcm_close(out->pcm[i]);
                out->pcm[i] = NULL;
            }
        }
        out->standby = true;

        if (out == adev->outputs[OUTPUT_HDMI]) {
            /* force standby on low latency output stream so that it can reuse HDMI driver if
             * necessary when restarted */
            force_non_hdmi_out_standby(adev);
        }

        /* re-calculate the set of active devices from other streams */
        adev->out_device = output_devices(out);

        /* Skip resetting the mixer if no output device is active */
        if (adev->out_device)
            select_devices(adev);
    }
}

/* lock outputs list, all output streams, and device */
static void lock_all_outputs(struct audio_device *adev)
{
    enum output_type type;
    pthread_mutex_lock(&adev->lock_outputs);
    for (type = 0; type < OUTPUT_TOTAL; ++type) {
        struct stream_out *out = adev->outputs[type];
        if (out) {
            lock_output_stream(out);
        }
    }
    pthread_mutex_lock(&adev->lock);
}

/* unlock device, all output streams (except specified stream), and outputs list */
static void unlock_all_outputs(struct audio_device *adev, struct stream_out *except)
{
    /* unlock order is irrelevant, but for cleanliness we unlock in reverse order */
    pthread_mutex_unlock(&adev->lock);
    enum output_type type = OUTPUT_TOTAL;
    do {
        struct stream_out *out = adev->outputs[--type];
        if (out && out != except) {
            unlock_output_stream(out);
        }
    } while (type != (enum output_type) 0);
    pthread_mutex_unlock(&adev->lock_outputs);
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;

    lock_all_outputs(adev);

    do_out_standby(out);

    unlock_all_outputs(adev, NULL);

    return 0;
}

static int out_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;

    ALOGV("%s: key value pairs: %s", __func__, kvpairs);

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);

        lock_all_outputs(adev);

        if ((out->device != val) && (val != 0)) {
            /* Force standby if moving to/from SPDIF or if the output
             * device changes when in SPDIF mode */
            if (((val & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) ^
                 (adev->out_device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)) ||
                (adev->out_device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)) {
                do_out_standby(out);
            }

#if 0
            /* force output standby to start or stop SCO pcm stream if needed */
            if ((val & AUDIO_DEVICE_OUT_ALL_SCO) ^
                (out->device & AUDIO_DEVICE_OUT_ALL_SCO)) {
                do_out_standby(out);
            }
#endif

#ifndef HDMI_INCAPABLE
            if (!out->standby && (out == adev->outputs[OUTPUT_HDMI] ||
                !adev->outputs[OUTPUT_HDMI] ||
                adev->outputs[OUTPUT_HDMI]->standby)) {
                adev->out_device = output_devices(out) | val;
                select_devices(adev);
            }
#endif

            out->device = val;
            adev->out_device = output_devices(out) | val;

            /*
             * If we switch from earpiece to speaker, we need to fully reset the
             * modem audio path.
             */
            if (adev->in_call) {
                if (route_changed(adev)) {
                    stop_call(adev);
                    start_call(adev);
                }
            } else {
                select_devices(adev);
            }

            /* start SCO stream if needed */
            if (val & AUDIO_DEVICE_OUT_ALL_SCO) {
                start_bt_sco(adev);
            }
        }

        unlock_all_outputs(adev, NULL);
    }

    str_parms_destroy(parms);
    return ret;
}

/*
 * Returns a pointer to a heap allocated string. The caller is responsible
 * for freeing the memory for it using free().
 */
static char *out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct str_parms *query = str_parms_create_str(keys);
    const char *str;
    char value[256];
    struct str_parms *reply = str_parms_create();
    size_t i, j;
    int ret;
    bool first = true;

    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value, sizeof(value));
    if (ret >= 0) {
        value[0] = '\0';
        i = 0;
        /* the last entry in supported_channel_masks[] is always 0 */
        while (out->supported_channel_masks[i] != 0) {
            for (j = 0; j < ARRAY_SIZE(out_channels_name_to_enum_table); j++) {
                if (out_channels_name_to_enum_table[j].value == out->supported_channel_masks[i]) {
                    if (!first) {
                        strcat(value, "|");
                    }
                    strcat(value, out_channels_name_to_enum_table[j].name);
                    first = false;
                    break;
                }
            }
            i++;
        }
        str_parms_add_str(reply, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value);
        str = str_parms_to_str(reply);
    } else {
        str = keys;
    }

    str_parms_destroy(query);
    str_parms_destroy(reply);
    return strdup(str);
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return (out->config.period_size * out->config.period_count * 1000) /
            out->config.rate;
}

static int out_set_volume(struct audio_stream_out *stream,
                          float left,
                          float right __unused)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;

    if (out == adev->outputs[OUTPUT_HDMI]) {
        /* only take left channel into account: the API is for stereo anyway */
        out->muted = (left == 0.0f);
        return 0;
    }
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret = 0;
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    int i;

    /* FIXME This comment is no longer correct
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the output stream mutex - e.g.
     * executing out_set_parameters() while holding the hw device
     * mutex
     */
    lock_output_stream(out);
    if (out->standby) {
        unlock_output_stream(out);
        lock_all_outputs(adev);
        if (!out->standby) {
            unlock_all_outputs(adev, out);
            goto false_alarm;
        }
        ret = start_output_stream(out);
        if (ret < 0) {
            unlock_all_outputs(adev, NULL);
            goto final_exit;
        }
        out->standby = false;
        unlock_all_outputs(adev, out);
    }
false_alarm:

    if (out->disabled) {
        ret = -EPIPE;
        goto exit;
    }

    if (out->muted)
        memset((void *)buffer, 0, bytes);

    /* Write to all active PCMs */
    for (i = 0; i < PCM_TOTAL; i++)
        if (out->pcm[i]) {
            ret = pcm_write(out->pcm[i], (void *)buffer, bytes);
            if (ret != 0)
                break;
        }
    if (ret == 0)
        out->written += bytes / (out->config.channels * sizeof(short));

exit:
    unlock_output_stream(out);
final_exit:

    if (ret != 0) {
        struct timespec t = { .tv_sec = 0, .tv_nsec = 0 };
        clock_gettime(CLOCK_MONOTONIC, &t);
        const int64_t now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;
        const int64_t elapsed_time_since_last_write = now - out->last_write_time_us;
        int64_t sleep_time = bytes * 1000000LL / audio_stream_out_frame_size(stream) /
                   out_get_sample_rate(&stream->common) - elapsed_time_since_last_write;
        if (sleep_time > 0) {
            usleep(sleep_time);
        } else {
            // we don't sleep when we exit standby (this is typical for a real alsa buffer).
            sleep_time = 0;
        }
        out->last_write_time_us = now + sleep_time;
        // last_write_time_us is an approximation of when the (simulated) alsa
        // buffer is believed completely full. The usleep above waits for more space
        // in the buffer, but by the end of the sleep the buffer is considered
        // topped-off.
        //
        // On the subsequent out_write(), we measure the elapsed time spent in
        // the mixer. This is subtracted from the sleep estimate based on frames,
        // thereby accounting for drain in the alsa buffer during mixing.
        // This is a crude approximation; we don't handle underruns precisely.
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream __unused,
                                   uint32_t *dsp_frames __unused)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream __unused,
                                effect_handle_t effect __unused)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream __unused,
                                   effect_handle_t effect __unused)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream __unused,
                                        int64_t *timestamp __unused)
{
    return -EINVAL;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                   uint64_t *frames, struct timespec *timestamp)
{
    struct stream_out *out = (struct stream_out *)stream;
    int ret = -1;

    lock_output_stream(out);

    int i;
    // There is a question how to implement this correctly when there is more than one PCM stream.
    // We are just interested in the frames pending for playback in the kernel buffer here,
    // not the total played since start.  The current behavior should be safe because the
    // cases where both cards are active are marginal.
    for (i = 0; i < PCM_TOTAL; i++)
        if (out->pcm[i]) {
            size_t avail;
            if (pcm_get_htimestamp(out->pcm[i], &avail, timestamp) == 0) {
                size_t kernel_buffer_size = out->config.period_size * out->config.period_count;
                // FIXME This calculation is incorrect if there is buffering after app processor
                int64_t signed_frames = out->written - kernel_buffer_size + avail;
                // It would be unusual for this value to be negative, but check just in case ...
                if (signed_frames >= 0) {
                    *frames = signed_frames;
                    ret = 0;
                }
                break;
            }
        }

    unlock_output_stream(out);

    return ret;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream __unused,
                              uint32_t rate __unused)
{
    return 0;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->channel_mask;
}


static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return get_input_buffer_size(in->requested_rate,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 audio_channel_count_from_in_mask(in_get_channels(stream)),
                                 (in->flags & AUDIO_INPUT_FLAG_FAST) != 0);
}

static audio_format_t in_get_format(const struct audio_stream *stream __unused)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream __unused,
                         audio_format_t format __unused)
{
    return -ENOSYS;
}

/* must be called with in stream and hw device mutex locked */
static void do_in_standby(struct stream_in *in)
{
    struct audio_device *adev = in->dev;

    if (!in->standby) {
        in->standby = true;
        if (in->pcm != NULL) {
            pcm_close(in->pcm);
            in->pcm = NULL;
        }

        if (adev->mode != AUDIO_MODE_IN_CALL) {
            in->dev->input_source = AUDIO_SOURCE_DEFAULT;
            in->dev->in_device = AUDIO_DEVICE_NONE;
            in->dev->in_channel_mask = 0;
            select_devices(adev);
        }
    }
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    lock_input_stream(in);
    pthread_mutex_lock(&in->dev->lock);

    do_in_standby(in);

    pthread_mutex_unlock(&in->dev->lock);
    unlock_input_stream(in);

    return 0;
}

static int in_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    unsigned int val;
    bool apply_now = false;

    parms = str_parms_create_str(kvpairs);

    lock_input_stream(in);
    pthread_mutex_lock(&adev->lock);
    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE,
                            value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        /* no audio source uses val == 0 */
        if ((in->input_source != val) && (val != 0)) {
            in->input_source = val;
            apply_now = !in->standby;
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING,
                            value, sizeof(value));
    if (ret >= 0) {
        /* strip AUDIO_DEVICE_BIT_IN to allow bitwise comparisons */
        val = atoi(value) & ~AUDIO_DEVICE_BIT_IN;
        /* no audio device uses val == 0 */
        if ((in->device != val) && (val != 0)) {
#if 0
            /* force output standby to start or stop SCO pcm stream if needed */
            if ((val & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) ^
                    (in->device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET)) {
                do_in_standby(in);
            }
#endif
            in->device = val;
            apply_now = !in->standby;
        }
    }

    if (apply_now) {
        adev->input_source = in->input_source;
        adev->in_device = in->device;
        select_devices(adev);
    }

    pthread_mutex_unlock(&adev->lock);
    unlock_input_stream(in);

    str_parms_destroy(parms);
    return ret;
}

static char *in_get_parameters(const struct audio_stream *stream __unused,
                               const char *keys __unused)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream __unused,
                       float gain __unused)
{
    return 0;
}

static void in_apply_ramp(struct stream_in *in, int16_t *buffer, size_t frames)
{
    size_t i;
    uint16_t vol = in->ramp_vol;
    uint16_t step = in->ramp_step;

    frames = (frames < in->ramp_frames) ? frames : in->ramp_frames;

    if (in->channel_mask == AUDIO_CHANNEL_IN_MONO)
        for (i = 0; i < frames; i++)
        {
            buffer[i] = (int16_t)((buffer[i] * vol) >> 16);
            vol += step;
        }
    else
        for (i = 0; i < frames; i++)
        {
            buffer[2*i] = (int16_t)((buffer[2*i] * vol) >> 16);
            buffer[2*i + 1] = (int16_t)((buffer[2*i + 1] * vol) >> 16);
            vol += step;
        }


    in->ramp_vol = vol;
    in->ramp_frames -= frames;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    int ret = 0;
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    size_t frames_rq = bytes / audio_stream_in_frame_size(stream);

    /*
     * acquiring hw device mutex systematically is useful if a low
     * priority thread is waiting on the input stream mutex - e.g.
     * executing in_set_parameters() while holding the hw device
     * mutex
     */
    lock_input_stream(in);
    if (in->standby) {
        pthread_mutex_lock(&adev->lock);
        ret = start_input_stream(in);
        pthread_mutex_unlock(&adev->lock);
        if (ret < 0)
            goto exit;
        in->standby = false;
    }

    /*if (in->num_preprocessors != 0)
        ret = process_frames(in, buffer, frames_rq);
      else */
    ret = read_frames(in, buffer, frames_rq);

    if (ret > 0)
        ret = 0;

    if (in->ramp_frames > 0)
        in_apply_ramp(in, buffer, frames_rq);

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

exit:
    if (ret != 0) {
        struct timespec t = { .tv_sec = 0, .tv_nsec = 0 };
        clock_gettime(CLOCK_MONOTONIC, &t);
        const int64_t now = (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000;

        // we do a full sleep when exiting standby.
        const bool standby = in->last_read_time_us == 0;
        const int64_t elapsed_time_since_last_read = standby ?
                0 : now - in->last_read_time_us;
        int64_t sleep_time = bytes * 1000000LL / audio_stream_in_frame_size(stream) /
                in_get_sample_rate(&stream->common) - elapsed_time_since_last_read;
        if (sleep_time > 0) {
            usleep(sleep_time);
        } else {
            sleep_time = 0;
        }
        in->last_read_time_us = now + sleep_time;
        // last_read_time_us is an approximation of when the (simulated) alsa
        // buffer is drained by the read, and is empty.
        //
        // On the subsequent in_read(), we measure the elapsed time spent in
        // the recording thread. This is subtracted from the sleep estimate based on frames,
        // thereby accounting for fill in the alsa buffer during the interim.
        memset(buffer, 0, bytes);
    }

    if (bytes > 0) {
        in->frames_read += bytes / audio_stream_in_frame_size(stream);
    }

    unlock_input_stream(in);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream __unused)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream __unused,
                               effect_handle_t effect __unused)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream __unused,
                                  effect_handle_t effect __unused)
{
    return 0;
}

static int in_get_capture_position(const struct audio_stream_in *stream,
                                   int64_t *frames,
                                   int64_t *time)
{
    struct stream_in *in;
    int rc = -ENOSYS;

    if (stream == NULL || frames == NULL || time == NULL) {
        return -EINVAL;
    }
    in = (struct stream_in *)stream;

    lock_input_stream(in);
    if (in->pcm != NULL) {
        struct timespec timestamp;
        unsigned int avail;

        rc = pcm_get_htimestamp(in->pcm, &avail, &timestamp);
        if (rc != 0) {
            rc = -EINVAL;
        } else {
            *frames = in->frames_read + avail;
            *time = timestamp.tv_sec * 1000000000LL + timestamp.tv_nsec;
            rc = 0;
        }
    }
    unlock_input_stream(in);

    return rc;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle __unused,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int ret;
    enum output_type type;

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_STEREO;
    out->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    if (devices == AUDIO_DEVICE_NONE)
        devices = AUDIO_DEVICE_OUT_SPEAKER;
    out->device = devices;

    if (flags & AUDIO_OUTPUT_FLAG_DIRECT &&
        devices == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        pthread_mutex_lock(&adev->lock);
        ret = read_hdmi_channel_masks(adev, out);
        pthread_mutex_unlock(&adev->lock);
        if (ret != 0)
            goto err_open;
        if (config->sample_rate == 0)
            config->sample_rate = HDMI_MULTI_DEFAULT_SAMPLING_RATE;
        if (config->channel_mask == 0)
            config->channel_mask = AUDIO_CHANNEL_OUT_5POINT1;
        out->channel_mask = config->channel_mask;
        out->config = pcm_config_hdmi_multi;
        out->config.rate = config->sample_rate;
        out->config.channels = popcount(config->channel_mask);
        out->pcm_device = PCM_DEVICE;
        type = OUTPUT_HDMI;
    } else if (flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) {
        ALOGV("*** %s: Deep buffer pcm config", __func__);
        out->config = pcm_config_deep;
        out->pcm_device = PCM_DEVICE_DEEP;
        type = OUTPUT_DEEP_BUF;
    } else {
        ALOGV("*** %s: Fast buffer pcm config", __func__);
        out->config = pcm_config_fast;
        out->pcm_device = PCM_DEVICE;
        type = OUTPUT_LOW_LATENCY;
    }

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->stream.get_presentation_position = out_get_presentation_position;

    out->dev = adev;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    out->standby = true;
    /* out->muted = false; by calloc() */
    /* out->written = 0; by calloc() */

    pthread_mutex_init(&out->lock, (const pthread_mutexattr_t *) NULL);
    pthread_mutex_init(&out->pre_lock, (const pthread_mutexattr_t *) NULL);

    pthread_mutex_lock(&adev->lock_outputs);
    if (adev->outputs[type]) {
        pthread_mutex_unlock(&adev->lock_outputs);
        ret = -EBUSY;
        goto err_open;
    }
    adev->outputs[type] = out;
    pthread_mutex_unlock(&adev->lock_outputs);

    *stream_out = &out->stream;

    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct audio_device *adev;
    enum output_type type;

    out_standby(&stream->common);
    adev = (struct audio_device *)dev;
    pthread_mutex_lock(&adev->lock_outputs);
    for (type = 0; type < OUTPUT_TOTAL; type++) {
        if (adev->outputs[type] == (struct stream_out *) stream) {
            adev->outputs[type] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&adev->lock_outputs);
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms;
    char value[32];
    int ret;

    parms = str_parms_create_str(kvpairs);
    ret = str_parms_get_str(parms,
                            AUDIO_PARAMETER_KEY_BT_NREC,
                            value,
                            sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0) {
            adev->bluetooth_nrec = true;
        } else {
            adev->bluetooth_nrec = false;
        }
    }

    /* FIXME: This does not work with LL, see workaround in this HAL */
    ret = str_parms_get_str(parms, "noise_suppression", value, sizeof(value));
    if (ret >= 0) {
        ALOGV("*** %s: noise_suppression=%s", __func__, value);

        /* value is either off or auto */
        if (strcmp(value, "off") == 0) {
            adev->two_mic_control = false;
        } else {
            adev->two_mic_control = true;
        }
    }

    str_parms_destroy(parms);
    return ret;
}

static char *adev_get_parameters(const struct audio_hw_device *dev __unused,
                                 const char *keys __unused)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev __unused)
{
    return 0;
}

static int voice_set_volume(struct audio_hw_device *dev, float volume)
{
    struct audio_device *adev = (struct audio_device *)dev;

    ALOGT("%s: Set volume to %f\n", __func__, volume);

    adev->voice_volume = volume;

    if (adev->mode == AUDIO_MODE_IN_CALL) {
        enum _SoundType sound_type;

        switch (adev->out_device) {
            case AUDIO_DEVICE_OUT_EARPIECE:
                sound_type = SOUND_TYPE_VOICE;
                break;
            case AUDIO_DEVICE_OUT_SPEAKER:
                sound_type = SOUND_TYPE_SPEAKER;
                break;
            case AUDIO_DEVICE_OUT_WIRED_HEADSET:
            case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
                sound_type = SOUND_TYPE_HEADSET;
                break;
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            case AUDIO_DEVICE_OUT_ALL_SCO:
                sound_type = SOUND_TYPE_BTVOICE;
                break;
            default:
                sound_type = SOUND_TYPE_VOICE;
        }

        ril_set_call_volume(&adev->ril, sound_type, volume);
    }

    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    struct audio_device *adev = (struct audio_device *)dev;
    int rc;

    ALOGT("%s: Set volume to %f\n", __func__, volume);

    pthread_mutex_lock(&adev->lock);
    rc = voice_set_volume(dev, volume);
    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_set_master_volume(struct audio_hw_device *dev __unused,
                                  float volume __unused)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    struct audio_device *adev = (struct audio_device *)dev;

    if (adev->mode == mode) {
        return 0;
    }

    pthread_mutex_lock(&adev->lock);
    adev->mode = mode;

    if (adev->mode == AUDIO_MODE_IN_CALL) {
        ALOGV("*** %s: Entering IN_CALL mode", __func__);
        start_call(adev);
    } else {
        ALOGV("*** %s: Leaving IN_CALL mode", __func__);
        stop_call(adev);
    }

    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct audio_device *adev = (struct audio_device *)dev;
    enum _MuteCondition mute_condition = state ? TX_MUTE : TX_UNMUTE;

    ALOGT("%s: Set mic mute: %d\n", __func__, state);

    pthread_mutex_lock(&adev->lock);
    if (adev->in_call) {
        ril_set_mute(&adev->ril, mute_condition);
    }

    adev->mic_mute = state;
    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct audio_device *adev = (struct audio_device *)dev;

    *state = adev->mic_mute;

    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev __unused,
                                         const struct audio_config *config)
{

    return get_input_buffer_size(config->sample_rate, config->format,
                                 audio_channel_count_from_in_mask(config->channel_mask),
                                 false /* is_low_latency: since we don't know, be conservative */);
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags,
                                  const char *address __unused,
                                  audio_source_t source __unused)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    int ret;

    *stream_in = NULL;

    /* Respond with a request for mono if a different format is given. */
    if (config->channel_mask != AUDIO_CHANNEL_IN_MONO &&
        config->channel_mask != AUDIO_CHANNEL_IN_FRONT_BACK) {
        if (!(adev->in_call && adev->two_mic_control)) {
            /* Not in a call and no explicit FRONT_BACK input requested */
            config->channel_mask = AUDIO_CHANNEL_IN_MONO;
            return -EINVAL;
        }
    }

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));
    if (in == NULL) {
        return -ENOMEM;
    }

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;
    in->stream.get_capture_position = in_get_capture_position;

    in->dev = adev;
    in->standby = true;
    in->requested_rate = config->sample_rate;
    in->input_source = AUDIO_SOURCE_DEFAULT;
    /* strip AUDIO_DEVICE_BIT_IN to allow bitwise comparisons */
    in->device = devices & ~AUDIO_DEVICE_BIT_IN;
    in->io_handle = handle;
    in->channel_mask = config->channel_mask;
    in->flags = flags;
    struct pcm_config *pcm_config = flags & AUDIO_INPUT_FLAG_FAST ?
            &pcm_config_in_low_latency : &pcm_config_in;
    in->config = pcm_config;

    in->buffer = malloc(pcm_config->period_size * pcm_config->channels
                                               * audio_stream_in_frame_size(&in->stream));

    if (!in->buffer) {
        ret = -ENOMEM;
        goto err_malloc;
    }

    if (in->requested_rate != pcm_config->rate) {
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;

        ret = create_resampler(pcm_config->rate,
                               in->requested_rate,
                               audio_channel_count_from_in_mask(in->channel_mask),
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);
        if (ret != 0) {
            ret = -EINVAL;
            goto err_resampler;
        }

        ALOGV("%s: Created resampler converting %d -> %d\n",
              __func__, pcm_config_in.rate, in->requested_rate);
    }

    ALOGV("%s: Requesting input stream with rate: %d, channels: 0x%x\n",
          __func__, config->sample_rate, config->channel_mask);

    pthread_mutex_init(&in->lock, (const pthread_mutexattr_t *) NULL);
    pthread_mutex_init(&in->pre_lock, (const pthread_mutexattr_t *) NULL);

    *stream_in = &in->stream;
    return 0;

err_resampler:
    free(in->buffer);
err_malloc:
    free(in);
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev __unused,
                                   struct audio_stream_in *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    in_standby(&stream->common);
    if (in->resampler) {
        release_resampler(in->resampler);
        in->resampler = NULL;
    }
    free(in->buffer);
    free(stream);
}

static int adev_dump(const audio_hw_device_t *device __unused, int fd __unused)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    audio_route_free(adev->audio_route);

    if (adev->hdmi_drv_fd >= 0) {
        close(adev->hdmi_drv_fd);
    }

    /* RIL */
    ril_close(&adev->ril);

    free(device);
    return 0;
}

#if 0
static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) {
        return -EINVAL;
    }

    adev = calloc(1, sizeof(struct audio_device));
    if (adev == NULL) {
        return -ENOMEM;
    }

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    adev->audio_route = audio_route_init(MIXER_CARD, NULL);
    adev->input_source = AUDIO_SOURCE_DEFAULT;
    adev->active_output.dev_id = -1;
    /* adev->cur_route_id initial value is 0 and such that first device
     * selection is always applied by select_devices() */

    adev->hdmi_drv_fd = -1;

    adev->mode = AUDIO_MODE_NORMAL;
    adev->voice_volume = 1.0f;

    /* RIL */
    ril_open(&adev->ril);
    /* register callback for wideband AMR setting */
    ril_register_set_wb_amr_callback(adev_set_wb_amr_callback, (void *)adev);

    *device = &adev->hw_device.common;

    return 0;
}
#endif

static int adev_open(const hw_module_t *module,
                     const char *name,
                     hw_device_t **device)
{
    struct audio_device *adev;
    int retry_count = 0;
    int cmp;

    ALOGV("%s: enter", __func__);

    *device = NULL;

    cmp = strcmp(name, AUDIO_HARDWARE_INTERFACE);
    if (cmp != 0) {
        return -EINVAL;
    }

    adev = calloc(1, sizeof(struct audio_device));
    if (adev == NULL) {
        return -ENOMEM;
    }

    adev->device.common.tag = HARDWARE_DEVICE_TAG;
    adev->device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->device.common.module = (struct hw_module_t *)module;
    adev->device.common.close = adev_close;

    adev->device.init_check = adev_init_check;
    adev->device.set_voice_volume = adev_set_voice_volume;
    adev->device.set_master_volume = adev_set_master_volume;
    adev->device.get_master_volume = adev_get_master_volume;
    adev->device.set_master_mute = adev_set_master_mute;
    adev->device.get_master_mute = adev_get_master_mute;
    adev->device.set_mode = adev_set_mode;
    adev->device.set_mic_mute = adev_set_mic_mute;
    adev->device.get_mic_mute = adev_get_mic_mute;
    adev->device.set_parameters = adev_set_parameters;
    adev->device.get_parameters = adev_get_parameters;
    adev->device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->device.open_output_stream = adev_open_output_stream;
    adev->device.close_output_stream = adev_close_output_stream;
    adev->device.open_input_stream = adev_open_input_stream;
    adev->device.close_input_stream = adev_close_input_stream;
    adev->device.dump = adev_dump;

    /* Set the default route before the PCM stream is opened */
    adev->mode = AUDIO_MODE_NORMAL;
    adev->active_input = NULL;
    adev->primary_output = NULL;
    adev->voice_volume = 1.0f;
    adev->tty_mode = TTY_MODE_OFF;
    adev->bluetooth_nrec = true;
    adev->in_call = false;
    /* adev->cur_hdmi_channels = 0;  by calloc() */
    adev->snd_dev_ref_cnt = calloc(SND_DEVICE_MAX, sizeof(int));

    adev->dualmic_config = DUALMIC_CONFIG_NONE;
    adev->ns_in_voice_rec = false;

    list_init(&adev->usecase_list);

    adev->mixer.audio_route = audio_route_init(MIXER_CARD, NULL);
    if (adev->mixer.audio_route == NULL) {
        ALOGE("%s: Failed to init, aborting.", __func__);

        free(adev->snd_dev_ref_cnt);
        free(adev);

        return -EINVAL;
    }

    /* Do not sleep on first enable_snd_device() */
    adev->mixer.shutdown_time = {
        .tv_sec = 1,
    }

    /* RIL */
    ril_open(&adev->ril);
    /* register callback for wideband AMR setting */
    ril_register_set_wb_amr_callback(adev_set_wb_amr_callback, (void *)adev);

    *device = &adev->device.common;

    char value[PROPERTY_VALUE_MAX];
    if (property_get("audio_hal.period_size", value, NULL) > 0) {
        int trial = atoi(value);
        if (period_size_is_plausible_for_low_latency(trial)) {

            pcm_device_playback.config.period_size = trial;
            pcm_device_playback.config.start_threshold =
                    PLAYBACK_START_THRESHOLD(trial, PLAYBACK_PERIOD_COUNT);
            pcm_device_playback.config.stop_threshold =
                    PLAYBACK_STOP_THRESHOLD(trial, PLAYBACK_PERIOD_COUNT);

            pcm_device_capture_low_latency.config.period_size = trial;
        }
    }

    ALOGV("%s: exit", __func__);

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Exynos543x Audio HAL",
        .author = "The LineageOS Project",
        .methods = &hal_module_methods,
    },
};
