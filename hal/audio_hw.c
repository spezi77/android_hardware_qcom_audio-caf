/*
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
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

#define LOG_TAG "audio_hw_primary"
/*#define LOG_NDEBUG 0*/
/*#define VERY_VERY_VERBOSE_LOGGING*/
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>
#include <math.h>
#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/prctl.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>
#include <cutils/atomic.h>
#include <cutils/sched_policy.h>

#include <hardware/audio_effect.h>
#include <system/thread_defs.h>
#include <audio_effects/effect_aec.h>
#include <audio_effects/effect_ns.h>
#include "audio_hw.h"
#include "platform_api.h"
#include <platform.h>
#include "audio_extn.h"
#include "voice_extn.h"

#include "sound/compress_params.h"
#include "sound/asound.h"

#define COMPRESS_OFFLOAD_NUM_FRAGMENTS 4
/* ToDo: Check and update a proper value in msec */
#define COMPRESS_OFFLOAD_PLAYBACK_LATENCY 96
#define COMPRESS_PLAYBACK_VOLUME_MAX 0x2000



struct pcm_config pcm_config_deep_buffer = {
    .channels = 2,
    .rate = DEFAULT_OUTPUT_SAMPLING_RATE,
    .period_size = DEEP_BUFFER_OUTPUT_PERIOD_SIZE,
    .period_count = DEEP_BUFFER_OUTPUT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = DEEP_BUFFER_OUTPUT_PERIOD_SIZE / 4,
    .stop_threshold = INT_MAX,
    .avail_min = DEEP_BUFFER_OUTPUT_PERIOD_SIZE / 4,
};

struct pcm_config pcm_config_low_latency = {
    .channels = 2,
    .rate = DEFAULT_OUTPUT_SAMPLING_RATE,
    .period_size = LOW_LATENCY_OUTPUT_PERIOD_SIZE,
    .period_count = LOW_LATENCY_OUTPUT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = LOW_LATENCY_OUTPUT_PERIOD_SIZE / 4,
    .stop_threshold = INT_MAX,
    .avail_min = LOW_LATENCY_OUTPUT_PERIOD_SIZE / 4,
};

#define USECASE_AUDIO_PLAYBACK_PRIMARY USECASE_AUDIO_PLAYBACK_DEEP_BUFFER
#define pcm_config_primary pcm_config_deep_buffer

struct pcm_config pcm_config_hdmi_multi = {
    .channels = HDMI_MULTI_DEFAULT_CHANNEL_COUNT, /* changed when the stream is opened */
    .rate = DEFAULT_OUTPUT_SAMPLING_RATE, /* changed when the stream is opened */
    .period_size = HDMI_MULTI_PERIOD_SIZE,
    .period_count = HDMI_MULTI_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 0,
    .stop_threshold = INT_MAX,
    .avail_min = 0,
};

struct pcm_config pcm_config_audio_capture = {
    .channels = 2,
    .period_count = AUDIO_CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

const char * const use_case_table[AUDIO_USECASE_MAX] = {
    [USECASE_AUDIO_PLAYBACK_DEEP_BUFFER] = "deep-buffer-playback",
    [USECASE_AUDIO_PLAYBACK_LOW_LATENCY] = "low-latency-playback",
#ifdef ULTRA_LOW_LATENCY_ENABLED
    [USECASE_AUDIO_PLAYBACK_ULTRA_LOW_LATENCY] = "ultra-low-latency-playback",
#endif
    [USECASE_AUDIO_PLAYBACK_MULTI_CH] = "multi-channel-playback",
    [USECASE_AUDIO_PLAYBACK_OFFLOAD] = "compress-offload-playback",
#ifdef MULTIPLE_OFFLOAD_ENABLED
    [USECASE_AUDIO_PLAYBACK_OFFLOAD2] = "compress-offload-playback2",
    [USECASE_AUDIO_PLAYBACK_OFFLOAD3] = "compress-offload-playback3",
    [USECASE_AUDIO_PLAYBACK_OFFLOAD4] = "compress-offload-playback4",
    [USECASE_AUDIO_PLAYBACK_OFFLOAD5] = "compress-offload-playback5",
    [USECASE_AUDIO_PLAYBACK_OFFLOAD6] = "compress-offload-playback6",
    [USECASE_AUDIO_PLAYBACK_OFFLOAD7] = "compress-offload-playback7",
    [USECASE_AUDIO_PLAYBACK_OFFLOAD8] = "compress-offload-playback8",
    [USECASE_AUDIO_PLAYBACK_OFFLOAD9] = "compress-offload-playback9",
#endif
    [USECASE_AUDIO_RECORD] = "audio-record",
    [USECASE_AUDIO_RECORD_COMPRESS] = "audio-record-compress",
    [USECASE_AUDIO_RECORD_LOW_LATENCY] = "low-latency-record",
    [USECASE_AUDIO_RECORD_FM_VIRTUAL] = "fm-virtual-record",
    [USECASE_AUDIO_PLAYBACK_FM] = "play-fm",
    [USECASE_AUDIO_HFP_SCO] = "hfp-sco",
    [USECASE_AUDIO_HFP_SCO_WB] = "hfp-sco-wb",
    [USECASE_VOICE_CALL] = "voice-call",
    
    [USECASE_VOICE2_CALL] = "voice2-call",
    [USECASE_VOLTE_CALL] = "volte-call",
    [USECASE_QCHAT_CALL] = "qchat-call",
    [USECASE_VOWLAN_CALL] = "vowlan-call",
    [USECASE_COMPRESS_VOIP_CALL] = "compress-voip-call",
    [USECASE_INCALL_REC_UPLINK] = "incall-rec-uplink",
    [USECASE_INCALL_REC_DOWNLINK] = "incall-rec-downlink",
    [USECASE_INCALL_REC_UPLINK_AND_DOWNLINK] = "incall-rec-uplink-and-downlink",
    [USECASE_INCALL_REC_UPLINK_COMPRESS] = "incall-rec-uplink-compress",
    [USECASE_INCALL_REC_DOWNLINK_COMPRESS] = "incall-rec-downlink-compress",
    [USECASE_INCALL_REC_UPLINK_AND_DOWNLINK_COMPRESS] = "incall-rec-uplink-and-downlink-compress",

    [USECASE_INCALL_MUSIC_UPLINK] = "incall_music_uplink",
    [USECASE_INCALL_MUSIC_UPLINK2] = "incall_music_uplink2",
    [USECASE_AUDIO_SPKR_CALIB_RX] = "spkr-rx-calib",
    [USECASE_AUDIO_SPKR_CALIB_TX] = "spkr-vi-record",
};

static const audio_usecase_t offload_usecases[] = {
    USECASE_AUDIO_PLAYBACK_OFFLOAD,
#ifdef MULTIPLE_OFFLOAD_ENABLED
    USECASE_AUDIO_PLAYBACK_OFFLOAD2,
    USECASE_AUDIO_PLAYBACK_OFFLOAD3,
#ifndef PLATFORM_MSM8974
    USECASE_AUDIO_PLAYBACK_OFFLOAD4,
    USECASE_AUDIO_PLAYBACK_OFFLOAD5,
    USECASE_AUDIO_PLAYBACK_OFFLOAD6,
    USECASE_AUDIO_PLAYBACK_OFFLOAD7,
    USECASE_AUDIO_PLAYBACK_OFFLOAD8,
    USECASE_AUDIO_PLAYBACK_OFFLOAD9,
#endif
#endif
};

#define STRING_TO_ENUM(string) { #string, string }

struct string_to_enum {
    const char *name;
    uint32_t value;
};

static const struct string_to_enum out_channels_name_to_enum_table[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
};

static struct audio_device *adev = NULL;
static pthread_mutex_t adev_init_lock;
static unsigned int audio_device_ref_count;

static int set_voice_volume_l(struct audio_device *adev, float volume);

static int check_and_set_gapless_mode(struct audio_device *adev) {


    char value[PROPERTY_VALUE_MAX] = {0};
    bool gapless_enabled = false;
    const char *mixer_ctl_name = "Compress Gapless Playback";
    struct mixer_ctl *ctl;

    ALOGV("%s:", __func__);
    property_get("audio.offload.gapless.enabled", value, NULL);
    gapless_enabled = atoi(value) || !strncmp("true", value, 4);

    ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
                               __func__, mixer_ctl_name);
        return -EINVAL;
    }

    if (mixer_ctl_set_value(ctl, 0, gapless_enabled) < 0) {
        ALOGE("%s: Could not set gapless mode %d",
                       __func__, gapless_enabled);
         return -EINVAL;
    }
    return 0;
}

static bool is_supported_format(audio_format_t format)
{
    if (format == AUDIO_FORMAT_MP3 ||
        format == AUDIO_FORMAT_AAC ||
        format == AUDIO_FORMAT_WMA ||
        format == AUDIO_FORMAT_WMA_PRO ||
        format == AUDIO_FORMAT_MP2 ||
#ifdef FLAC_OFFLOAD_ENABLED
        format == AUDIO_FORMAT_FLAC ||
#endif
        format == AUDIO_FORMAT_PCM_16_BIT_OFFLOAD ||
        format == AUDIO_FORMAT_PCM_24_BIT_OFFLOAD)
           return true;

    return false;
}

static int get_snd_codec_id(audio_format_t format)
{
    int id = 0;

#ifdef ENABLE_AV_ENHANCEMENTS
    switch (format) {
    case AUDIO_FORMAT_MP3:
        id = SND_AUDIOCODEC_MP3;
        break;
    case AUDIO_FORMAT_AAC:
        id = SND_AUDIOCODEC_AAC;
        break;
#ifdef WMA_OFFLOAD_ENABLED
    case AUDIO_FORMAT_WMA:
        id = SND_AUDIOCODEC_WMA;
        break;
    case AUDIO_FORMAT_WMA_PRO:
        id = SND_AUDIOCODEC_WMA_PRO;
        break;
#endif
#ifdef MP2_OFFLOAD_ENABLED
    case AUDIO_FORMAT_MP2:
        id = SND_AUDIOCODEC_MP2;
        break;
#endif
    case AUDIO_FORMAT_PCM_16_BIT_OFFLOAD:
    case AUDIO_FORMAT_PCM_24_BIT_OFFLOAD:
        id = SND_AUDIOCODEC_PCM;
        break;
#ifdef FLAC_OFFLOAD_ENABLED
    case AUDIO_FORMAT_FLAC:
        id = SND_AUDIOCODEC_FLAC;
        break;
#endif
    default:
        ALOGE("%s: Unsupported audio format :%x", __func__, format);
    }
#endif // ENABLE_AV_ENHANCEMENTS

    return id;
}

static int get_snd_card_state(struct audio_device *adev)
{
    int snd_scard_state;

    if (!adev)
        return SND_CARD_STATE_OFFLINE;

    pthread_mutex_lock(&adev->snd_card_status.lock);
    snd_scard_state = adev->snd_card_status.state;
    pthread_mutex_unlock(&adev->snd_card_status.lock);

    return snd_scard_state;
}

static int set_snd_card_state(struct audio_device *adev, int snd_scard_state)
{
    if (!adev)
        return -ENOSYS;

    pthread_mutex_lock(&adev->snd_card_status.lock);
    adev->snd_card_status.state = snd_scard_state;
    pthread_mutex_unlock(&adev->snd_card_status.lock);

    return 0;
}

int enable_audio_route(struct audio_device *adev,
                       struct audio_usecase *usecase)
{
    snd_device_t snd_device;
    char mixer_path[MIXER_PATH_MAX_LENGTH];

    if (usecase == NULL)
        return -EINVAL;

    ALOGV("%s: enter: usecase(%d)", __func__, usecase->id);

    if (usecase->type == PCM_CAPTURE)
        snd_device = usecase->in_snd_device;
    else
        snd_device = usecase->out_snd_device;

#ifdef DS1_DOLBY_DAP_ENABLED
    audio_extn_dolby_set_dmid(adev);
    audio_extn_dolby_set_endpoint(adev);
#endif
    audio_extn_listen_update_stream_status(usecase, LISTEN_EVENT_STREAM_BUSY);
    strcpy(mixer_path, use_case_table[usecase->id]);
    platform_add_backend_name(mixer_path, snd_device);
    ALOGV("%s: apply mixer and update path: %s", __func__, mixer_path);
    audio_route_apply_and_update_path(adev->audio_route, mixer_path);
    ALOGV("%s: exit", __func__);
    return 0;
}

int disable_audio_route(struct audio_device *adev,
                        struct audio_usecase *usecase)
{
    snd_device_t snd_device;
    char mixer_path[MIXER_PATH_MAX_LENGTH];

    if (usecase == NULL)
        return -EINVAL;

    ALOGV("%s: enter: usecase(%d)", __func__, usecase->id);
    if (usecase->type == PCM_CAPTURE)
        snd_device = usecase->in_snd_device;
    else
        snd_device = usecase->out_snd_device;
    strcpy(mixer_path, use_case_table[usecase->id]);
    platform_add_backend_name(mixer_path, snd_device);
    ALOGV("%s: reset and update mixer path: %s", __func__, mixer_path);
    audio_route_reset_and_update_path(adev->audio_route, mixer_path);
    audio_extn_listen_update_stream_status(usecase, LISTEN_EVENT_STREAM_FREE);
    ALOGV("%s: exit", __func__);
    return 0;
}

int enable_snd_device(struct audio_device *adev,
                      snd_device_t snd_device)
{
    char device_name[DEVICE_NAME_MAX_SIZE] = {0};

    if (snd_device < SND_DEVICE_MIN ||
        snd_device >= SND_DEVICE_MAX) {
        ALOGE("%s: Invalid sound device %d", __func__, snd_device);
        return -EINVAL;
    }

    adev->snd_dev_ref_cnt[snd_device]++;

    if(platform_get_snd_device_name_extn(adev->platform, snd_device, device_name) < 0 ) {
        ALOGE("%s: Invalid sound device returned", __func__);
        return -EINVAL;
    }
    if (adev->snd_dev_ref_cnt[snd_device] > 1) {
        ALOGV("%s: snd_device(%d: %s) is already active",
              __func__, snd_device, device_name);
        return 0;
    }

    if (audio_extn_spkr_prot_is_enabled())
         audio_extn_spkr_prot_calib_cancel(adev);

    /* Set BT sample rate before enabling the devices. Adding sample rate mixer
     * control in use-case does not work because rate update takes place after
     * AFE port open due to the limitation of mixer control order execution.
     */
    if ((snd_device == SND_DEVICE_OUT_BT_SCO) ||
        (snd_device == SND_DEVICE_IN_BT_SCO_MIC)) {
        audio_route_apply_and_update_path(adev->audio_route, BT_SCO_SAMPLE_RATE);
    } else if ((snd_device == SND_DEVICE_OUT_BT_SCO_WB) ||
               (snd_device == SND_DEVICE_IN_BT_SCO_MIC_WB)) {
        audio_route_apply_and_update_path(adev->audio_route, BT_SCO_WB_SAMPLE_RATE);
    }

    /* start usb playback thread */
    if(SND_DEVICE_OUT_USB_HEADSET == snd_device ||
       SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET == snd_device)
        audio_extn_usb_start_playback(adev);

    /* start usb capture thread */
    if(SND_DEVICE_IN_USB_HEADSET_MIC == snd_device)
       audio_extn_usb_start_capture(adev);

    if (snd_device == SND_DEVICE_OUT_SPEAKER &&
        audio_extn_spkr_prot_is_enabled()) {
       if (audio_extn_spkr_prot_start_processing(snd_device)) {
          ALOGE("%s: spkr_start_processing failed", __func__);
          return -EINVAL;
      }
    }  else {
        ALOGV("%s: snd_device(%d: %s)", __func__,
            snd_device, device_name);
        /* due to the possibility of calibration overwrite between listen
            and audio, notify listen hal before audio calibration is sent */
        audio_extn_listen_update_device_status(snd_device,
                                        LISTEN_EVENT_SND_DEVICE_BUSY);
        if (platform_send_audio_calibration(adev->platform, snd_device) < 0) {
            adev->snd_dev_ref_cnt[snd_device]--;
            audio_extn_listen_update_device_status(snd_device,
                                        LISTEN_EVENT_SND_DEVICE_FREE);
            return -EINVAL;
        }

        audio_route_apply_and_update_path(adev->audio_route, device_name);
    }
    return 0;
}

int disable_snd_device(struct audio_device *adev,
                       snd_device_t snd_device)
{
    char device_name[DEVICE_NAME_MAX_SIZE] = {0};

    if (snd_device < SND_DEVICE_MIN ||
        snd_device >= SND_DEVICE_MAX) {
        ALOGE("%s: Invalid sound device %d", __func__, snd_device);
        return -EINVAL;
    }
    if (adev->snd_dev_ref_cnt[snd_device] <= 0) {
        ALOGE("%s: device ref cnt is already 0", __func__);
        return -EINVAL;
    }

    adev->snd_dev_ref_cnt[snd_device]--;

    if(platform_get_snd_device_name_extn(adev->platform, snd_device, device_name) < 0) {
        ALOGE("%s: Invalid sound device returned", __func__);
        return -EINVAL;
    }

    if (adev->snd_dev_ref_cnt[snd_device] == 0) {
        ALOGV("%s: snd_device(%d: %s)", __func__,
              snd_device, device_name);
        /* exit usb play back thread */
        if(SND_DEVICE_OUT_USB_HEADSET == snd_device ||
           SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET == snd_device)
            audio_extn_usb_stop_playback();

        /* exit usb capture thread */
        if(SND_DEVICE_IN_USB_HEADSET_MIC == snd_device)
            audio_extn_usb_stop_capture(adev);

        if (snd_device == SND_DEVICE_OUT_SPEAKER &&
            audio_extn_spkr_prot_is_enabled()) {
            audio_extn_spkr_prot_stop_processing();
        } else
            audio_route_reset_and_update_path(adev->audio_route, device_name);

        audio_extn_listen_update_device_status(snd_device,
                                        LISTEN_EVENT_SND_DEVICE_FREE);
    }

    return 0;
}

static void check_usecases_codec_backend(struct audio_device *adev,
                                          struct audio_usecase *uc_info,
                                          snd_device_t snd_device)
{
    struct listnode *node;
    struct audio_usecase *usecase;
    bool switch_device[AUDIO_USECASE_MAX];
    int i, num_uc_to_switch = 0;

    /*
     * This function is to make sure that all the usecases that are active on
     * the hardware codec backend are always routed to any one device that is
     * handled by the hardware codec.
     * For example, if low-latency and deep-buffer usecases are currently active
     * on speaker and out_set_parameters(headset) is received on low-latency
     * output, then we have to make sure deep-buffer is also switched to headset,
     * because of the limitation that both the devices cannot be enabled
     * at the same time as they share the same backend.
     */
    /*
     * This call is to check if we need to force routing for a particular stream
     * If there is a backend configuration change for the device when a
     * new stream starts, then ADM needs to be closed and re-opened with the new
     * configuraion. This call check if we need to re-route all the streams
     * associated with the backend. Touch tone + 24 bit playback.
     */
    bool force_routing = platform_check_and_set_codec_backend_cfg(adev, uc_info);

    /* Disable all the usecases on the shared backend other than the
       specified usecase */
    for (i = 0; i < AUDIO_USECASE_MAX; i++)
        switch_device[i] = false;

    list_for_each(node, &adev->usecase_list) {
        usecase = node_to_item(node, struct audio_usecase, list);
        if (usecase->type != PCM_CAPTURE &&
                usecase != uc_info &&
                (usecase->out_snd_device != snd_device || force_routing)  &&
                usecase->devices & AUDIO_DEVICE_OUT_ALL_CODEC_BACKEND) {
            ALOGV("%s: Usecase (%s) is active on (%s) - disabling ..",
                  __func__, use_case_table[usecase->id],
                  platform_get_snd_device_name(usecase->out_snd_device));
            disable_audio_route(adev, usecase);
            switch_device[usecase->id] = true;
            num_uc_to_switch++;
        }
    }

    if (num_uc_to_switch) {
        /* All streams have been de-routed. Disable the device */

        list_for_each(node, &adev->usecase_list) {
            usecase = node_to_item(node, struct audio_usecase, list);
            if (switch_device[usecase->id]) {
                disable_snd_device(adev, usecase->out_snd_device);
            }
        }

        list_for_each(node, &adev->usecase_list) {
            usecase = node_to_item(node, struct audio_usecase, list);
            if (switch_device[usecase->id]) {
                enable_snd_device(adev, snd_device);
            }
        }

        /* Re-route all the usecases on the shared backend other than the
           specified usecase to new snd devices */
        list_for_each(node, &adev->usecase_list) {
            usecase = node_to_item(node, struct audio_usecase, list);
            /* Update the out_snd_device only before enabling the audio route */
            if (switch_device[usecase->id] ) {
                usecase->out_snd_device = snd_device;
                enable_audio_route(adev, usecase);
            }
        }
    }
}

static void check_and_route_capture_usecases(struct audio_device *adev,
                                             struct audio_usecase *uc_info,
                                             snd_device_t snd_device)
{
    struct listnode *node;
    struct audio_usecase *usecase;
    bool switch_device[AUDIO_USECASE_MAX];
    int i, num_uc_to_switch = 0;

    /*
     * This function is to make sure that all the active capture usecases
     * are always routed to the same input sound device.
     * For example, if audio-record and voice-call usecases are currently
     * active on speaker(rx) and speaker-mic (tx) and out_set_parameters(earpiece)
     * is received for voice call then we have to make sure that audio-record
     * usecase is also switched to earpiece i.e. voice-dmic-ef,
     * because of the limitation that two devices cannot be enabled
     * at the same time if they share the same backend.
     */
    for (i = 0; i < AUDIO_USECASE_MAX; i++)
        switch_device[i] = false;

    list_for_each(node, &adev->usecase_list) {
        usecase = node_to_item(node, struct audio_usecase, list);
        if (usecase->type != PCM_PLAYBACK &&
                usecase != uc_info &&
                usecase->in_snd_device != snd_device) {
            ALOGV("%s: Usecase (%s) is active on (%s) - disabling ..",
                  __func__, use_case_table[usecase->id],
                  platform_get_snd_device_name(usecase->in_snd_device));
            disable_audio_route(adev, usecase);
            switch_device[usecase->id] = true;
            num_uc_to_switch++;
        }
    }

    if (num_uc_to_switch) {
        /* All streams have been de-routed. Disable the device */

        list_for_each(node, &adev->usecase_list) {
            usecase = node_to_item(node, struct audio_usecase, list);
            if (switch_device[usecase->id]) {
                disable_snd_device(adev, usecase->in_snd_device);
            }
        }

        list_for_each(node, &adev->usecase_list) {
            usecase = node_to_item(node, struct audio_usecase, list);
            if (switch_device[usecase->id]) {
                enable_snd_device(adev, snd_device);
            }
        }

        /* Re-route all the usecases on the shared backend other than the
           specified usecase to new snd devices */
        list_for_each(node, &adev->usecase_list) {
            usecase = node_to_item(node, struct audio_usecase, list);
            /* Update the in_snd_device only before enabling the audio route */
            if (switch_device[usecase->id] ) {
                usecase->in_snd_device = snd_device;
                enable_audio_route(adev, usecase);
            }
        }
    }
}

/* must be called with hw device mutex locked */
static int read_hdmi_channel_masks(struct stream_out *out)
{
    int ret = 0;
    int channels = platform_edid_get_max_channels(out->dev->platform);

    switch (channels) {
        /*
         * Do not handle stereo output in Multi-channel cases
         * Stereo case is handled in normal playback path
         */
    case 6:
        ALOGV("%s: HDMI supports 5.1", __func__);
        out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_5POINT1;
        break;
    case 8:
        ALOGV("%s: HDMI supports 5.1 and 7.1 channels", __func__);
        out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_5POINT1;
        out->supported_channel_masks[1] = AUDIO_CHANNEL_OUT_7POINT1;
        break;
    default:
        ALOGE("HDMI does not support multi channel playback");
        ret = -ENOSYS;
        break;
    }
    return ret;
}

static audio_usecase_t get_voice_usecase_id_from_list(struct audio_device *adev)
{
    struct audio_usecase *usecase;
    struct listnode *node;

    list_for_each(node, &adev->usecase_list) {
        usecase = node_to_item(node, struct audio_usecase, list);
        if (usecase->type == VOICE_CALL) {
            ALOGV("%s: usecase id %d", __func__, usecase->id);
            return usecase->id;
        }
    }
    return USECASE_INVALID;
}

struct audio_usecase *get_usecase_from_list(struct audio_device *adev,
                                            audio_usecase_t uc_id)
{
    struct audio_usecase *usecase;
    struct listnode *node;

    list_for_each(node, &adev->usecase_list) {
        usecase = node_to_item(node, struct audio_usecase, list);
        if (usecase->id == uc_id)
            return usecase;
    }
    return NULL;
}

int select_devices(struct audio_device *adev, audio_usecase_t uc_id)
{
    snd_device_t out_snd_device = SND_DEVICE_NONE;
    snd_device_t in_snd_device = SND_DEVICE_NONE;
    struct audio_usecase *usecase = NULL;
    struct audio_usecase *vc_usecase = NULL;
    struct audio_usecase *voip_usecase = NULL;
    struct audio_usecase *hfp_usecase = NULL;
    audio_usecase_t hfp_ucid;
    struct listnode *node;
    int status = 0;

    usecase = get_usecase_from_list(adev, uc_id);
    if (usecase == NULL) {
        ALOGE("%s: Could not find the usecase(%d)", __func__, uc_id);
        return -EINVAL;
    }

    if ((usecase->type == VOICE_CALL) ||
        (usecase->type == VOIP_CALL)  ||
        (usecase->type == PCM_HFP_CALL)) {
        out_snd_device = platform_get_output_snd_device(adev->platform,
                                                        usecase->stream.out->devices);
        in_snd_device = platform_get_input_snd_device(adev->platform, usecase->stream.out->devices);
        usecase->devices = usecase->stream.out->devices;
    } else {
        /*
         * If the voice call is active, use the sound devices of voice call usecase
         * so that it would not result any device switch. All the usecases will
         * be switched to new device when select_devices() is called for voice call
         * usecase. This is to avoid switching devices for voice call when
         * check_usecases_codec_backend() is called below.
         */
        if (voice_is_in_call(adev)) {
            vc_usecase = get_usecase_from_list(adev,
                                               get_voice_usecase_id_from_list(adev));
            if ((vc_usecase->devices & AUDIO_DEVICE_OUT_ALL_CODEC_BACKEND) ||
                (usecase->devices == AUDIO_DEVICE_IN_VOICE_CALL)) {
                in_snd_device = vc_usecase->in_snd_device;
                out_snd_device = vc_usecase->out_snd_device;
            }
        } else if (voice_extn_compress_voip_is_active(adev)) {
            voip_usecase = get_usecase_from_list(adev, USECASE_COMPRESS_VOIP_CALL);
            if ((voip_usecase->devices & AUDIO_DEVICE_OUT_ALL_CODEC_BACKEND) &&
                (usecase->devices & AUDIO_DEVICE_OUT_ALL_CODEC_BACKEND) &&
                 (voip_usecase->stream.out != adev->primary_output)) {
                    in_snd_device = voip_usecase->in_snd_device;
                    out_snd_device = voip_usecase->out_snd_device;
            }
        } else if (audio_extn_hfp_is_active(adev)) {
            hfp_ucid = audio_extn_hfp_get_usecase();
            hfp_usecase = get_usecase_from_list(adev, hfp_ucid);
            if (hfp_usecase->devices & AUDIO_DEVICE_OUT_ALL_CODEC_BACKEND) {
                   in_snd_device = hfp_usecase->in_snd_device;
                   out_snd_device = hfp_usecase->out_snd_device;
            }
        }
        if (usecase->type == PCM_PLAYBACK) {
            usecase->devices = usecase->stream.out->devices;
            in_snd_device = SND_DEVICE_NONE;
            if (out_snd_device == SND_DEVICE_NONE) {
                out_snd_device = platform_get_output_snd_device(adev->platform,
                                            usecase->stream.out->devices);
                if (usecase->stream.out == adev->primary_output &&
                        adev->active_input &&
                        adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION) {
                    select_devices(adev, adev->active_input->usecase);
                }
            }
        } else if (usecase->type == PCM_CAPTURE) {
            usecase->devices = usecase->stream.in->device;
            out_snd_device = SND_DEVICE_NONE;
            if (in_snd_device == SND_DEVICE_NONE) {
                if (adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION &&
                        adev->primary_output && !adev->primary_output->standby) {
                    in_snd_device = platform_get_input_snd_device(adev->platform,
                                        adev->primary_output->devices);
                } else {
                    in_snd_device = platform_get_input_snd_device(adev->platform,
                                                                  AUDIO_DEVICE_NONE);
                }
            }
        }
    }

    if (out_snd_device == usecase->out_snd_device &&
        in_snd_device == usecase->in_snd_device) {
        return 0;
    }

    ALOGD("%s: out_snd_device(%d: %s) in_snd_device(%d: %s)", __func__,
          out_snd_device, platform_get_snd_device_name(out_snd_device),
          in_snd_device,  platform_get_snd_device_name(in_snd_device));

    /*
     * Limitation: While in call, to do a device switch we need to disable
     * and enable both RX and TX devices though one of them is same as current
     * device.
     */
    if (usecase->type == VOICE_CALL || usecase->type == VOIP_CALL) {
        status = platform_switch_voice_call_device_pre(adev->platform);
    }

    /* Disable current sound devices */
    if (usecase->out_snd_device != SND_DEVICE_NONE) {
        disable_audio_route(adev, usecase);
        disable_snd_device(adev, usecase->out_snd_device);
    }

    if (usecase->in_snd_device != SND_DEVICE_NONE) {
        disable_audio_route(adev, usecase);
        disable_snd_device(adev, usecase->in_snd_device);
    }

#ifndef PLATFORM_MSM8960
    /* Applicable only on the targets that has external modem.
     * New device information should be sent to modem before enabling
     * the devices to reduce in-call device switch time.
     */
    if (usecase->type == VOICE_CALL)
        status = platform_switch_voice_call_enable_device_config(adev->platform,
                                                                 out_snd_device,
                                                                 in_snd_device);
#endif

    /* Enable new sound devices */
    if (out_snd_device != SND_DEVICE_NONE) {
        if (usecase->devices & AUDIO_DEVICE_OUT_ALL_CODEC_BACKEND)
            check_usecases_codec_backend(adev, usecase, out_snd_device);
        enable_snd_device(adev, out_snd_device);
    }

    if (in_snd_device != SND_DEVICE_NONE) {
        check_and_route_capture_usecases(adev, usecase, in_snd_device);
        enable_snd_device(adev, in_snd_device);
    }

    if (usecase->type == VOICE_CALL || usecase->type == VOIP_CALL)
        status = platform_switch_voice_call_device_post(adev->platform,
                                                        out_snd_device,
                                                        in_snd_device);

    usecase->in_snd_device = in_snd_device;
    usecase->out_snd_device = out_snd_device;

    enable_audio_route(adev, usecase);

#ifndef PLATFORM_MSM8960
    /* Applicable only on the targets that has external modem.
     * Enable device command should be sent to modem only after
     * enabling voice call mixer controls
     */
    if (usecase->type == VOICE_CALL)
        status = platform_switch_voice_call_usecase_route_post(adev->platform,
                                                               out_snd_device,
                                                               in_snd_device);
#endif

    ALOGD("%s: done",__func__);

    return status;
}

static int stop_input_stream(struct stream_in *in)
{
    int i, ret = 0;
    struct audio_usecase *uc_info;
    struct audio_device *adev = in->dev;

    adev->active_input = NULL;

    ALOGV("%s: enter: usecase(%d: %s)", __func__,
          in->usecase, use_case_table[in->usecase]);
    uc_info = get_usecase_from_list(adev, in->usecase);
    if (uc_info == NULL) {
        ALOGE("%s: Could not find the usecase (%d) in the list",
              __func__, in->usecase);
        return -EINVAL;
    }

#ifdef MULTI_VOICE_SESSION_ENABLED
    /* Close in-call recording streams */
    voice_check_and_stop_incall_rec_usecase(adev, in);
#endif

    /* 1. Disable stream specific mixer controls */
    disable_audio_route(adev, uc_info);

    /* 2. Disable the tx device */
    disable_snd_device(adev, uc_info->in_snd_device);

    list_remove(&uc_info->list);
    free(uc_info);

    ALOGV("%s: exit: status(%d)", __func__, ret);
    return ret;
}

int start_input_stream(struct stream_in *in)
{
    /* 1. Enable output device and stream routing controls */
    int ret = 0;
    struct audio_usecase *uc_info;
    struct audio_device *adev = in->dev;
    int snd_card_status = get_snd_card_state(adev);

    in->usecase = platform_update_usecase_from_source(in->source,in->usecase);
    ALOGD("%s: enter: stream(%p)usecase(%d: %s)",
          __func__, &in->stream, in->usecase, use_case_table[in->usecase]);


    if (SND_CARD_STATE_OFFLINE == snd_card_status) {
        ALOGE("%s: sound card is not active/SSR returning error", __func__);
        ret = -EIO;
        goto error_config;
    }

#ifdef MULTI_VOICE_SESSION_ENABLED
    /* Check if source matches incall recording usecase criteria */
    ret = voice_check_and_set_incall_rec_usecase(adev, in);
    if (ret)
        goto error_config;
    else
        ALOGV("%s: usecase(%d)", __func__, in->usecase);
#endif

    in->pcm_device_id = platform_get_pcm_device_id(in->usecase, PCM_CAPTURE);
    if (in->pcm_device_id < 0) {
        ALOGE("%s: Could not find PCM device id for the usecase(%d)",
              __func__, in->usecase);
        ret = -EINVAL;
        goto error_config;
    }

    adev->active_input = in;
    uc_info = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));

    if (!uc_info) {
        ret = -ENOMEM;
        goto error_config;
    }

    uc_info->id = in->usecase;
    uc_info->type = PCM_CAPTURE;
    uc_info->stream.in = in;
    uc_info->devices = in->device;
    uc_info->in_snd_device = SND_DEVICE_NONE;
    uc_info->out_snd_device = SND_DEVICE_NONE;

    list_add_tail(&adev->usecase_list, &uc_info->list);
    select_devices(adev, in->usecase);

    ALOGV("%s: Opening PCM device card_id(%d) device_id(%d), channels %d",
          __func__, adev->snd_card,
          in->pcm_device_id, in->config.channels);
    in->pcm = pcm_open(adev->snd_card,
                       in->pcm_device_id, PCM_IN, &in->config);
    if (in->pcm && !pcm_is_ready(in->pcm)) {
        ALOGE("%s: %s", __func__, pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        in->pcm = NULL;
        ret = -EIO;
        goto error_open;
    }

    ALOGV("%s: exit", __func__);
    return ret;

error_open:
    stop_input_stream(in);

error_config:
    adev->active_input = NULL;
    ALOGD("%s: exit: status(%d)", __func__, ret);

    return ret;
}

/* must be called with out->lock locked */
static int send_offload_cmd_l(struct stream_out* out, int command)
{
    struct offload_cmd *cmd = (struct offload_cmd *)calloc(1, sizeof(struct offload_cmd));

    if (!cmd) {
        ALOGE("failed to allocate mem for command 0x%x", command);
        return -ENOMEM;
    }

    ALOGVV("%s %d", __func__, command);

    cmd->cmd = command;
    list_add_tail(&out->offload_cmd_list, &cmd->node);
    pthread_cond_signal(&out->offload_cond);
    return 0;
}

/* must be called iwth out->lock locked */
static void stop_compressed_output_l(struct stream_out *out)
{
    out->offload_state = OFFLOAD_STATE_IDLE;
    out->playback_started = 0;
    out->send_new_metadata = 1;
    if (out->compr != NULL) {
        compress_stop(out->compr);
        while (out->offload_thread_blocked) {
            pthread_cond_wait(&out->cond, &out->lock);
        }
    }
}

bool is_offload_usecase(audio_usecase_t uc_id)
{
    unsigned int i;
    for (i = 0; i < sizeof(offload_usecases)/sizeof(offload_usecases[0]); i++) {
        if (uc_id == offload_usecases[i])
            return true;
    }
    return false;
}

static audio_usecase_t get_offload_usecase(struct audio_device *adev)
{
    audio_usecase_t ret = USECASE_AUDIO_PLAYBACK_OFFLOAD;
    unsigned int i, num_usecase = sizeof(offload_usecases)/sizeof(offload_usecases[0]);
    char value[PROPERTY_VALUE_MAX] = {0};

    property_get("audio.offload.multiple.enabled", value, NULL);
    if (!(atoi(value) || !strncmp("true", value, 4)))
        num_usecase = 1; /* If prop is not set, limit the num of offload usecases to 1 */

    ALOGV("%s: num_usecase: %d", __func__, num_usecase);
    for (i = 0; i < num_usecase; i++) {
        if (!(adev->offload_usecases_state & (0x1<<i))) {
            adev->offload_usecases_state |= 0x1 << i;
            ret = offload_usecases[i];
            break;
        }
    }
    ALOGV("%s: offload usecase is %d", __func__, ret);
    return ret;
}

static void free_offload_usecase(struct audio_device *adev,
                                 audio_usecase_t uc_id)
{
    unsigned int i;
    for (i = 0; i < sizeof(offload_usecases)/sizeof(offload_usecases[0]); i++) {
        if (offload_usecases[i] == uc_id) {
            adev->offload_usecases_state &= ~(0x1<<i);
            break;
        }
    }
    ALOGV("%s: free offload usecase %d", __func__, uc_id);
}

static void *offload_thread_loop(void *context)
{
    struct stream_out *out = (struct stream_out *) context;
    struct listnode *item;
    int ret = 0;

    setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_AUDIO);
    set_sched_policy(0, SP_FOREGROUND);
    prctl(PR_SET_NAME, (unsigned long)"Offload Callback", 0, 0, 0);

    ALOGV("%s", __func__);
    pthread_mutex_lock(&out->lock);
    for (;;) {
        struct offload_cmd *cmd = NULL;
        stream_callback_event_t event;
        bool send_callback = false;

        ALOGVV("%s offload_cmd_list %d out->offload_state %d",
              __func__, list_empty(&out->offload_cmd_list),
              out->offload_state);
        if (list_empty(&out->offload_cmd_list)) {
            ALOGV("%s SLEEPING", __func__);
            pthread_cond_wait(&out->offload_cond, &out->lock);
            ALOGV("%s RUNNING", __func__);
            continue;
        }

        item = list_head(&out->offload_cmd_list);
        cmd = node_to_item(item, struct offload_cmd, node);
        list_remove(item);

        ALOGVV("%s STATE %d CMD %d out->compr %p",
               __func__, out->offload_state, cmd->cmd, out->compr);

        if (cmd->cmd == OFFLOAD_CMD_EXIT) {
            free(cmd);
            break;
        }

        if (out->compr == NULL) {
            ALOGE("%s: Compress handle is NULL", __func__);
            pthread_cond_signal(&out->cond);
            continue;
        }
        out->offload_thread_blocked = true;
        pthread_mutex_unlock(&out->lock);
        send_callback = false;
        switch(cmd->cmd) {
        case OFFLOAD_CMD_WAIT_FOR_BUFFER:
            ALOGD("copl(%x):calling compress_wait", (unsigned int)out);
            compress_wait(out->compr, -1);
            ALOGD("copl(%x):out of compress_wait", (unsigned int)out);
            send_callback = true;
            event = STREAM_CBK_EVENT_WRITE_READY;
            break;
        case OFFLOAD_CMD_PARTIAL_DRAIN:
            ret = compress_next_track(out->compr);
            if(ret == 0) {
                ALOGD("copl(%x):calling compress_partial_drain", (unsigned int)out);
                compress_partial_drain(out->compr);
                ALOGD("copl(%x):out of compress_partial_drain", (unsigned int)out);
            }
            else if(ret == -ETIMEDOUT)
                compress_drain(out->compr);
            else
                ALOGE("%s: Next track returned error %d",__func__, ret);
            send_callback = true;
            event = STREAM_CBK_EVENT_DRAIN_READY;
            break;
        case OFFLOAD_CMD_DRAIN:
            ALOGD("copl(%x):calling compress_drain", (unsigned int)out);
            compress_drain(out->compr);
            ALOGD("copl(%x):calling compress_drain", (unsigned int)out);
            send_callback = true;
            event = STREAM_CBK_EVENT_DRAIN_READY;
            break;
        default:
            ALOGE("%s unknown command received: %d", __func__, cmd->cmd);
            break;
        }
        pthread_mutex_lock(&out->lock);
        out->offload_thread_blocked = false;
        pthread_cond_signal(&out->cond);
        if (send_callback) {
            out->offload_callback(event, NULL, out->offload_cookie);
        }
        free(cmd);
    }

    pthread_cond_signal(&out->cond);
    while (!list_empty(&out->offload_cmd_list)) {
        item = list_head(&out->offload_cmd_list);
        list_remove(item);
        free(node_to_item(item, struct offload_cmd, node));
    }
    pthread_mutex_unlock(&out->lock);

    return NULL;
}

static int create_offload_callback_thread(struct stream_out *out)
{
    pthread_cond_init(&out->offload_cond, (const pthread_condattr_t *) NULL);
    list_init(&out->offload_cmd_list);
    pthread_create(&out->offload_thread, (const pthread_attr_t *) NULL,
                    offload_thread_loop, out);
    return 0;
}

static int destroy_offload_callback_thread(struct stream_out *out)
{
    pthread_mutex_lock(&out->lock);
    stop_compressed_output_l(out);
    send_offload_cmd_l(out, OFFLOAD_CMD_EXIT);

    pthread_mutex_unlock(&out->lock);
    pthread_join(out->offload_thread, (void **) NULL);
    pthread_cond_destroy(&out->offload_cond);

    return 0;
}

static bool allow_hdmi_channel_config(struct audio_device *adev)
{
    struct listnode *node;
    struct audio_usecase *usecase;
    bool ret = true;

    list_for_each(node, &adev->usecase_list) {
        usecase = node_to_item(node, struct audio_usecase, list);
        if (usecase->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            /*
             * If voice call is already existing, do not proceed further to avoid
             * disabling/enabling both RX and TX devices, CSD calls, etc.
             * Once the voice call done, the HDMI channels can be configured to
             * max channels of remaining use cases.
             */
            if (usecase->id == USECASE_VOICE_CALL) {
                ALOGD("%s: voice call is active, no change in HDMI channels",
                      __func__);
                ret = false;
                break;
            } else if (usecase->id == USECASE_AUDIO_PLAYBACK_MULTI_CH) {
                ALOGD("%s: multi channel playback is active, "
                      "no change in HDMI channels", __func__);
                ret = false;
                break;
            } else if (is_offload_usecase(usecase->id) &&
                       popcount(usecase->stream.out->channel_mask) > 2) {
                ALOGD("%s: multi-channel(%x) compress offload playback is active, "
                      "no change in HDMI channels", __func__, usecase->stream.out->channel_mask);
                ret = false;
                break;
            }
        }
    }
    return ret;
}

static int check_and_set_hdmi_channels(struct audio_device *adev,
                                       unsigned int channels)
{
    struct listnode *node;
    struct audio_usecase *usecase;

    /* Check if change in HDMI channel config is allowed */
    if (!allow_hdmi_channel_config(adev))
        return 0;

    if (channels == adev->cur_hdmi_channels) {
        ALOGD("%s: Requested channels are same as current channels(%d)", __func__, channels);
        return 0;
    }

    platform_set_hdmi_channels(adev->platform, channels);
    adev->cur_hdmi_channels = channels;

    /*
     * Deroute all the playback streams routed to HDMI so that
     * the back end is deactivated. Note that backend will not
     * be deactivated if any one stream is connected to it.
     */
    list_for_each(node, &adev->usecase_list) {
        usecase = node_to_item(node, struct audio_usecase, list);
        if (usecase->type == PCM_PLAYBACK &&
                usecase->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            disable_audio_route(adev, usecase);
        }
    }

    /*
     * Enable all the streams disabled above. Now the HDMI backend
     * will be activated with new channel configuration
     */
    list_for_each(node, &adev->usecase_list) {
        usecase = node_to_item(node, struct audio_usecase, list);
        if (usecase->type == PCM_PLAYBACK &&
                usecase->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            enable_audio_route(adev, usecase);
        }
    }

    return 0;
}

static int stop_output_stream(struct stream_out *out)
{
    int i, ret = 0;
    struct audio_usecase *uc_info;
    struct audio_device *adev = out->dev;

    ALOGV("%s: enter: usecase(%d: %s)", __func__,
          out->usecase, use_case_table[out->usecase]);
    uc_info = get_usecase_from_list(adev, out->usecase);
    if (uc_info == NULL) {
        ALOGE("%s: Could not find the usecase (%d) in the list",
              __func__, out->usecase);
        return -EINVAL;
    }

    if (is_offload_usecase(out->usecase)) {
        if (adev->visualizer_stop_output != NULL)
            adev->visualizer_stop_output(out->handle, out->pcm_device_id);
        if (adev->offload_effects_stop_output != NULL)
            adev->offload_effects_stop_output(out->handle, out->pcm_device_id);
    }

    /* 1. Get and set stream specific mixer controls */
    disable_audio_route(adev, uc_info);

    /* 2. Disable the rx device */
    disable_snd_device(adev, uc_info->out_snd_device);

    list_remove(&uc_info->list);
    free(uc_info);

    /* Must be called after removing the usecase from list */
    if (out->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL)
        check_and_set_hdmi_channels(adev, DEFAULT_HDMI_OUT_CHANNELS);

    ALOGV("%s: exit: status(%d)", __func__, ret);
    return ret;
}

int start_output_stream(struct stream_out *out)
{
    int ret = 0;
    int sink_channels = 0;
    char prop_value[PROPERTY_VALUE_MAX] = {0};
    struct audio_usecase *uc_info;
    struct audio_device *adev = out->dev;
    int snd_card_status = get_snd_card_state(adev);

    if ((out->usecase < 0) || (out->usecase >= AUDIO_USECASE_MAX)) {
        ret = -EINVAL;
        goto error_config;
    }

    ALOGD("%s: enter: stream(%p)usecase(%d: %s) devices(%#x)",
          __func__, &out->stream, out->usecase, use_case_table[out->usecase],
          out->devices);

    if (SND_CARD_STATE_OFFLINE == snd_card_status) {
        ALOGE("%s: sound card is not active/SSR returning error", __func__);
        ret = -EIO;
        goto error_config;
    }

#ifdef ULTRA_LOW_LATENCY_ENABLED
    if (out->sample_rate == 48000 && out->usecase == USECASE_AUDIO_PLAYBACK_LOW_LATENCY)
        out->usecase = USECASE_AUDIO_PLAYBACK_ULTRA_LOW_LATENCY;
#endif

    out->pcm_device_id = platform_get_pcm_device_id(out->usecase, PCM_PLAYBACK);
    if (out->pcm_device_id < 0) {
        ALOGE("%s: Invalid PCM device id(%d) for the usecase(%d)",
              __func__, out->pcm_device_id, out->usecase);
        ret = -EINVAL;
        goto error_config;
    }

    uc_info = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));

    if (!uc_info) {
        ret = -ENOMEM;
        goto error_config;
    }

    uc_info->id = out->usecase;
    uc_info->type = PCM_PLAYBACK;
    uc_info->stream.out = out;
    uc_info->devices = out->devices;
    uc_info->in_snd_device = SND_DEVICE_NONE;
    uc_info->out_snd_device = SND_DEVICE_NONE;

    /* This must be called before adding this usecase to the list */
    if (out->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        property_get("audio.use.hdmi.sink.cap", prop_value, NULL);
        if (!strncmp("true", prop_value, 4)) {
            sink_channels = platform_edid_get_max_channels(out->dev->platform);
            ALOGD("%s: set HDMI channel count[%d] based on sink capability", __func__, sink_channels);
            check_and_set_hdmi_channels(adev, sink_channels);
        } else {
            if (is_offload_usecase(out->usecase))
                check_and_set_hdmi_channels(adev, out->compr_config.codec->ch_in);
            else
                check_and_set_hdmi_channels(adev, out->config.channels);
        }
    }

    list_add_tail(&adev->usecase_list, &uc_info->list);

    select_devices(adev, out->usecase);

    ALOGV("%s: Opening PCM device card_id(%d) device_id(%d)",
          __func__, 0, out->pcm_device_id);
    if (!is_offload_usecase(out->usecase)) {
        out->pcm = pcm_open(adev->snd_card,
                            out->pcm_device_id,
                            PCM_OUT | PCM_MONOTONIC, &out->config);
        if (out->pcm && !pcm_is_ready(out->pcm)) {
            ALOGE("%s: %s", __func__, pcm_get_error(out->pcm));
            pcm_close(out->pcm);
            out->pcm = NULL;
            ret = -EIO;
            goto error_open;
        }
    } else {
        out->pcm = NULL;
        out->compr = compress_open(adev->snd_card,
                                   out->pcm_device_id,
                                   COMPRESS_IN, &out->compr_config);
        if (out->compr && !is_compress_ready(out->compr)) {
            ALOGE("%s: %s", __func__, compress_get_error(out->compr));
            compress_close(out->compr);
            out->compr = NULL;
            ret = -EIO;
            goto error_open;
        }
        if (out->offload_callback)
            compress_nonblock(out->compr, out->non_blocking);

#ifdef DS1_DOLBY_DDP_ENABLED
        if (audio_extn_is_dolby_format(out->format))
            audio_extn_dolby_send_ddp_endp_params(adev);
#endif

        if (adev->visualizer_start_output != NULL)
            adev->visualizer_start_output(out->handle, out->pcm_device_id);
        if (adev->offload_effects_start_output != NULL)
            adev->offload_effects_start_output(out->handle, out->pcm_device_id);
    }
    ALOGV("%s: exit", __func__);
    return 0;
error_open:
    stop_output_stream(out);
error_config:
    return ret;
}

static int check_input_parameters(uint32_t sample_rate,
                                  audio_format_t format,
                                  int channel_count)
{
    int ret = 0;

    if ((format != AUDIO_FORMAT_PCM_16_BIT) &&
        !voice_extn_compress_voip_is_format_supported(format) &&
            !audio_extn_compr_cap_format_supported(format))  ret = -EINVAL;

    switch (channel_count) {
    case 1:
    case 2:
    case 6:
        break;
    default:
        ret = -EINVAL;
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
        ret = -EINVAL;
    }

    return ret;
}

static size_t get_input_buffer_size(uint32_t sample_rate,
                                    audio_format_t format,
                                    int channel_count)
{
    size_t size = 0;

    if (check_input_parameters(sample_rate, format, channel_count) != 0)
        return 0;

    size = (sample_rate * AUDIO_CAPTURE_PERIOD_DURATION_MSEC) / 1000;
    /* ToDo: should use frame_size computed based on the format and
       channel_count here. */
    size *= sizeof(short) * channel_count;

    /* make sure the size is multiple of 64 */
    size += 0x3f;
    size &= ~0x3f;

    return size;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    return out->sample_rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    if (is_offload_usecase(out->usecase))
        return out->compr_config.fragment_size;
    else if(out->usecase == USECASE_COMPRESS_VOIP_CALL)
        return voice_extn_compress_voip_out_get_buffer_size(out);

    return out->config.period_size * audio_stream_frame_size(stream);
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

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;

    ALOGD("%s: enter: stream (%p) usecase(%d: %s)", __func__,
          stream, out->usecase, use_case_table[out->usecase]);
    if (out->usecase == USECASE_COMPRESS_VOIP_CALL) {
        /* Ignore standby in case of voip call because the voip output
         * stream is closed in adev_close_output_stream()
         */
        ALOGD("%s: Ignore Standby in VOIP call", __func__);
        return 0;
    }

    pthread_mutex_lock(&out->lock);
    if (!out->standby) {
        pthread_mutex_lock(&adev->lock);
        out->standby = true;
        if (!is_offload_usecase(out->usecase)) {
            if (out->pcm) {
                pcm_close(out->pcm);
                out->pcm = NULL;
            }
        } else {
            ALOGD("copl(%x):standby", (unsigned int)out);
            stop_compressed_output_l(out);
            out->gapless_mdata.encoder_delay = 0;
            out->gapless_mdata.encoder_padding = 0;
            if (out->compr != NULL) {
                compress_close(out->compr);
                out->compr = NULL;
            }
        }
        stop_output_stream(out);
        pthread_mutex_unlock(&adev->lock);
    }
    pthread_mutex_unlock(&out->lock);
    ALOGV("%s: exit", __func__);
    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int parse_compress_metadata(struct stream_out *out, struct str_parms *parms)
{
    int ret = 0;
    char value[32];
    bool is_meta_data_params = false;
    struct compr_gapless_mdata tmp_mdata;
    tmp_mdata.encoder_delay = 0;
    tmp_mdata.encoder_padding = 0;

    if (!out || !parms) {
        ALOGE("%s: return invalid ",__func__);
        return -EINVAL;
    }

    ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_FORMAT, value, sizeof(value));
    if (ret >= 0) {
        if (atoi(value) == SND_AUDIOSTREAMFORMAT_MP4ADTS) {
            out->compr_config.codec->format = SND_AUDIOSTREAMFORMAT_MP4ADTS;
            ALOGV("ADTS format is set in offload mode");
        }
        out->send_new_metadata = 1;
    }

#ifdef FLAC_OFFLOAD_ENABLED
    if (out->format == AUDIO_FORMAT_FLAC) {
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_FLAC_MIN_BLK_SIZE, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.flac_dec.min_blk_size = atoi(value);
            out->send_new_metadata = 1;
        }
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_FLAC_MAX_BLK_SIZE, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.flac_dec.max_blk_size = atoi(value);
            out->send_new_metadata = 1;
        }
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_FLAC_MIN_FRAME_SIZE, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.flac_dec.min_frame_size = atoi(value);
            out->send_new_metadata = 1;
        }
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_FLAC_MAX_FRAME_SIZE, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.flac_dec.max_frame_size = atoi(value);
            out->send_new_metadata = 1;
        }
    }
#endif

    ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_SAMPLE_RATE, value, sizeof(value));
    if(ret >= 0)
        is_meta_data_params = true;
    ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_NUM_CHANNEL, value, sizeof(value));
    if(ret >= 0 )
        is_meta_data_params = true;
    ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_AVG_BIT_RATE, value, sizeof(value));
    if(ret >= 0 )
        is_meta_data_params = true;
    ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_DELAY_SAMPLES, value, sizeof(value));
    if (ret >= 0) {
        is_meta_data_params = true;
        tmp_mdata.encoder_delay = atoi(value); //whats a good limit check?
    }
    ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_PADDING_SAMPLES, value, sizeof(value));
    if (ret >= 0) {
        is_meta_data_params = true;
        tmp_mdata.encoder_padding = atoi(value);
    }

    if(!is_meta_data_params) {
        ALOGV("%s: Not gapless meta data params", __func__);
        return 0;
    }
    out->gapless_mdata = tmp_mdata;
    out->send_new_metadata = 1;
    ALOGV("%s new encoder delay %u and padding %u", __func__,
          out->gapless_mdata.encoder_delay, out->gapless_mdata.encoder_padding);

    if(out->format == AUDIO_FORMAT_WMA || out->format == AUDIO_FORMAT_WMA_PRO) {
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_WMA_FORMAT_TAG, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->format = atoi(value);
        }
       ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_WMA_BLOCK_ALIGN, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.wma.super_block_align = atoi(value);
        }
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_WMA_BIT_PER_SAMPLE, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.wma.bits_per_sample = atoi(value);
        }
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_WMA_CHANNEL_MASK, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.wma.channelmask = atoi(value);
        }
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_WMA_ENCODE_OPTION, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.wma.encodeopt = atoi(value);
        }
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_WMA_ENCODE_OPTION1, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.wma.encodeopt1 = atoi(value);
        }
        ret = str_parms_get_str(parms, AUDIO_OFFLOAD_CODEC_WMA_ENCODE_OPTION2, value, sizeof(value));
        if (ret >= 0) {
            out->compr_config.codec->options.wma.encodeopt2 = atoi(value);
        }
        ALOGV("WMA params: fmt %x, balgn %x, sr %d, chmsk %x, encop %x, op1 %x, op2 %x",
                                out->compr_config.codec->format,
                                out->compr_config.codec->options.wma.super_block_align,
                                out->compr_config.codec->options.wma.bits_per_sample,
                                out->compr_config.codec->options.wma.channelmask,
                                out->compr_config.codec->options.wma.encodeopt,
                                out->compr_config.codec->options.wma.encodeopt1,
                                out->compr_config.codec->options.wma.encodeopt2);
    }

    return 0;
}


static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct audio_usecase *usecase;
    struct listnode *node;
    struct str_parms *parms;
    char value[32];
    int ret = 0, val = 0, err;
    bool select_new_device = false;

    ALOGD("%s: enter: usecase(%d: %s) kvpairs: %s",
          __func__, out->usecase, use_case_table[out->usecase], kvpairs);
    parms = str_parms_create_str(kvpairs);
    err = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (err >= 0) {
        val = atoi(value);
        pthread_mutex_lock(&out->lock);
        pthread_mutex_lock(&adev->lock);

        /*
         * When HDMI cable is unplugged/usb hs is disconnected the
         * music playback is paused and the policy manager sends routing=0
         * But the audioflingercontinues to write data until standby time
         * (3sec). As the HDMI core is turned off, the write gets blocked.
         * Avoid this by routing audio to speaker until standby.
         */
        if ((out->devices == AUDIO_DEVICE_OUT_AUX_DIGITAL ||
                out->devices == AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
                out->devices == AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) &&
                val == AUDIO_DEVICE_NONE) {
            val = AUDIO_DEVICE_OUT_SPEAKER;
        }

        /*
         * select_devices() call below switches all the usecases on the same
         * backend to the new device. Refer to check_usecases_codec_backend() in
         * the select_devices(). But how do we undo this?
         *
         * For example, music playback is active on headset (deep-buffer usecase)
         * and if we go to ringtones and select a ringtone, low-latency usecase
         * will be started on headset+speaker. As we can't enable headset+speaker
         * and headset devices at the same time, select_devices() switches the music
         * playback to headset+speaker while starting low-lateny usecase for ringtone.
         * So when the ringtone playback is completed, how do we undo the same?
         *
         * We are relying on the out_set_parameters() call on deep-buffer output,
         * once the ringtone playback is ended.
         * NOTE: We should not check if the current devices are same as new devices.
         *       Because select_devices() must be called to switch back the music
         *       playback to headset.
         */
        if (val != 0) {
            out->devices = val;

            if (!out->standby)
                select_devices(adev, out->usecase);

            if ((adev->mode == AUDIO_MODE_IN_CALL) &&
                    !voice_is_in_call(adev) &&
                    (out == adev->primary_output)) {
                ret = voice_start_call(adev);
#ifdef PLATFORM_MSM8960
            } else if ((adev->mode == AUDIO_MODE_IN_CALL) &&
                            voice_is_in_call(adev) &&
# else
            } else if (voice_is_in_call(adev) &&
# endif
                            (out == adev->primary_output)) {
                voice_update_devices_for_all_voice_usecases(adev);
            }
        }

        if ((adev->mode == AUDIO_MODE_NORMAL) &&
                voice_is_in_call(adev) &&
                (out == adev->primary_output)) {
            ret = voice_stop_call(adev);
        }

        pthread_mutex_unlock(&adev->lock);
        pthread_mutex_unlock(&out->lock);
    }

    if (out == adev->primary_output) {
        pthread_mutex_lock(&adev->lock);
        audio_extn_set_parameters(adev, parms);
        pthread_mutex_unlock(&adev->lock);
    }
    if (is_offload_usecase(out->usecase)) {
        pthread_mutex_lock(&out->lock);
        ret = parse_compress_metadata(out, parms);
        pthread_mutex_unlock(&out->lock);
    }

    str_parms_destroy(parms);
    ALOGV("%s: exit: code(%d)", __func__, ret);
    return ret;
}

static char* out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct str_parms *query = str_parms_create_str(keys);
    char *str;
    char value[256];
    struct str_parms *reply = str_parms_create();
    size_t i, j;
    int ret;
    bool first = true;

    if (!query || !reply) {
        ALOGE("out_get_parameters: failed to allocate mem for query or reply");
        return NULL;
    }

    ALOGV("%s: enter: keys - %s", __func__, keys);
    ret = str_parms_get_str(query, AUDIO_PARAMETER_STREAM_SUP_CHANNELS, value, sizeof(value));
    if (ret >= 0) {
        value[0] = '\0';
        i = 0;
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
        voice_extn_out_get_parameters(out, query, reply);
        str = str_parms_to_str(reply);
        if (!strncmp(str, "", sizeof(""))) {
            free(str);
            str = strdup(keys);
        }
    }
    str_parms_destroy(query);
    str_parms_destroy(reply);
    ALOGV("%s: exit: returns - %s", __func__, str);
    return str;
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    if (is_offload_usecase(out->usecase))
        return COMPRESS_OFFLOAD_PLAYBACK_LATENCY;

    return (out->config.period_count * out->config.period_size * 1000) /
           (out->config.rate);
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    struct stream_out *out = (struct stream_out *)stream;
    int volume[2];

    if (out->usecase == USECASE_AUDIO_PLAYBACK_MULTI_CH) {
        /* only take left channel into account: the API is for stereo anyway */
        out->muted = (left == 0.0f);
        return 0;
    } else if (is_offload_usecase(out->usecase)) {
        char mixer_ctl_name[128];
        struct audio_device *adev = out->dev;
        struct mixer_ctl *ctl;
        int pcm_device_id = platform_get_pcm_device_id(out->usecase,
                                                       PCM_PLAYBACK);

        snprintf(mixer_ctl_name, sizeof(mixer_ctl_name),
                 "Compress Playback %d Volume", pcm_device_id);
        ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
        if (!ctl) {
            ALOGE("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
            return -EINVAL;
        }
        volume[0] = (int)(left * COMPRESS_PLAYBACK_VOLUME_MAX);
        volume[1] = (int)(right * COMPRESS_PLAYBACK_VOLUME_MAX);
        mixer_ctl_set_array(ctl, volume, sizeof(volume)/sizeof(volume[0]));
        return 0;
    }

    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void *buffer,
                         size_t bytes)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    int snd_scard_state = get_snd_card_state(adev);
    ssize_t ret = 0;

    pthread_mutex_lock(&out->lock);

    if (SND_CARD_STATE_OFFLINE == snd_scard_state) {
        if (out->pcm) {
            ALOGD(" %s: sound card is not active/SSR state", __func__);
            ret= -EIO;
            goto exit;
        } else if (out->usecase == USECASE_AUDIO_PLAYBACK_OFFLOAD) {
            //during SSR for compress usecase we should return error to flinger
            ALOGD(" copl %s: sound card is not active/SSR state", __func__);
            pthread_mutex_unlock(&out->lock);
            return -ENETRESET;
        }
    }

    if (out->standby) {
        out->standby = false;
        pthread_mutex_lock(&adev->lock);
        if (out->usecase == USECASE_COMPRESS_VOIP_CALL)
            ret = voice_extn_compress_voip_start_output_stream(out);
        else
            ret = start_output_stream(out);
        pthread_mutex_unlock(&adev->lock);
        /* ToDo: If use case is compress offload should return 0 */
        if (ret != 0) {
            out->standby = true;
            goto exit;
        }
    }

    if (is_offload_usecase(out->usecase)) {
        ALOGD("copl(%x): writing buffer (%d bytes) to compress device", (unsigned int)out, bytes);
        if (out->send_new_metadata) {
            ALOGD("copl(%x):send new gapless metadata", (unsigned int)out);
            compress_set_gapless_metadata(out->compr, &out->gapless_mdata);
            out->send_new_metadata = 0;
        }

        ret = compress_write(out->compr, buffer, bytes);
        ALOGVV("%s: writing buffer (%d bytes) to compress device returned %d", __func__, bytes, ret);
        if (ret >= 0 && ret < (ssize_t)bytes) {
            ALOGD("No space available in compress driver, post msg to cb thread");
            send_offload_cmd_l(out, OFFLOAD_CMD_WAIT_FOR_BUFFER);
        } else if (-ENETRESET == ret) {
            ALOGE("copl %s: received sound card offline state on compress write", __func__);
            set_snd_card_state(adev,SND_CARD_STATE_OFFLINE);
            pthread_mutex_unlock(&out->lock);
            out_standby(&out->stream.common);
            return ret;
        }
        if (!out->playback_started && ret >= 0) {
            compress_start(out->compr);
            out->playback_started = 1;
            out->offload_state = OFFLOAD_STATE_PLAYING;
        }
        pthread_mutex_unlock(&out->lock);
        return ret;
    } else {
        if (out->pcm) {
            if (out->muted)
                memset((void *)buffer, 0, bytes);
            ALOGVV("%s: writing buffer (%d bytes) to pcm device", __func__, bytes);
            ret = pcm_write(out->pcm, (void *)buffer, bytes);
            if (ret == 0)
                out->written += bytes / (out->config.channels * sizeof(short));
        }
    }

exit:
    /* ToDo: There may be a corner case when SSR happens back to back during
       start/stop. Need to post different error to handle that. */
    if (-ENETRESET == ret) {
        set_snd_card_state(adev,SND_CARD_STATE_OFFLINE);
    }

    pthread_mutex_unlock(&out->lock);

    if (ret != 0) {
        if (out->pcm)
            ALOGE("%s: error %d - %s", __func__, ret, pcm_get_error(out->pcm));
        out_standby(&out->stream.common);
        usleep(bytes * 1000000 / audio_stream_frame_size(&out->stream.common) /
                        out_get_sample_rate(&out->stream.common));

    }
    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    struct stream_out *out = (struct stream_out *)stream;
    if (is_offload_usecase(out->usecase) && (dsp_frames != NULL)) {
        ssize_t ret =  -EINVAL;
        *dsp_frames = 0;
        pthread_mutex_lock(&out->lock);
        if (out->compr != NULL) {
            ret = compress_get_tstamp(out->compr, (unsigned long *)dsp_frames,
                    &out->sample_rate);
            ALOGVV("%s rendered frames %d sample_rate %d",
                   __func__, *dsp_frames, out->sample_rate);
        }
        pthread_mutex_unlock(&out->lock);
        if (-ENETRESET == ret) {
            ALOGE(" ERROR: sound card not active Unable to get time stamp from compress driver");
            set_snd_card_state(adev,SND_CARD_STATE_OFFLINE);
            return -EINVAL;
        } else if(ret < 0) {
            if (out->compr == NULL) {
                return 0;
            }
            ALOGE(" ERROR: Unable to get time stamp from compress driver ret=%d", ret);
            return -EINVAL;
        } else {
            return 0;
        }
    } else
        return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                   uint64_t *frames, struct timespec *timestamp)
{
    struct stream_out *out = (struct stream_out *)stream;
    int ret = -1;
    unsigned long dsp_frames;

    pthread_mutex_lock(&out->lock);

    if (is_offload_usecase(out->usecase)) {
        if (out->compr != NULL) {
            compress_get_tstamp(out->compr, &dsp_frames,
                    &out->sample_rate);
            ALOGVV("%s rendered frames %ld sample_rate %d",
                   __func__, dsp_frames, out->sample_rate);
            *frames = dsp_frames;
            ret = 0;
            /* this is the best we can do */
            clock_gettime(CLOCK_MONOTONIC, timestamp);
        }
    } else {
        if (out->pcm) {
            size_t avail;
            if (pcm_get_htimestamp(out->pcm, &avail, timestamp) == 0) {
                size_t kernel_buffer_size = out->config.period_size * out->config.period_count;
                int64_t signed_frames = out->written - kernel_buffer_size + avail;
                // This adjustment accounts for buffering after app processor.
                // It is based on estimated DSP latency per use case, rather than exact.
                signed_frames -=
                    (platform_render_latency(out->usecase) * out->sample_rate / 1000000LL);

                // It would be unusual for this value to be negative, but check just in case ...
                if (signed_frames >= 0) {
                    *frames = signed_frames;
                    ret = 0;
                }
            }
        }
    }

    pthread_mutex_unlock(&out->lock);

    return ret;
}

static int out_set_callback(struct audio_stream_out *stream,
            stream_callback_t callback, void *cookie)
{
    struct stream_out *out = (struct stream_out *)stream;

    ALOGV("%s", __func__);
    pthread_mutex_lock(&out->lock);
    out->offload_callback = callback;
    out->offload_cookie = cookie;
    pthread_mutex_unlock(&out->lock);
    return 0;
}

static int out_pause(struct audio_stream_out* stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    int status = -ENOSYS;
    ALOGV("%s", __func__);
    if (is_offload_usecase(out->usecase)) {
        ALOGD("copl(%x):pause compress driver", (unsigned int)out);
        pthread_mutex_lock(&out->lock);
        if (out->compr != NULL && out->offload_state == OFFLOAD_STATE_PLAYING) {
            struct audio_device *adev = out->dev;
            int snd_scard_state = get_snd_card_state(adev);

            if (SND_CARD_STATE_ONLINE == snd_scard_state)
                status = compress_pause(out->compr);

            out->offload_state = OFFLOAD_STATE_PAUSED;
        }
        pthread_mutex_unlock(&out->lock);
    }
    return status;
}

static int out_resume(struct audio_stream_out* stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    int status = -ENOSYS;
    ALOGV("%s", __func__);
    if (is_offload_usecase(out->usecase)) {
        ALOGD("copl(%x):resume compress driver", (unsigned int)out);
        status = 0;
        pthread_mutex_lock(&out->lock);
        if (out->compr != NULL && out->offload_state == OFFLOAD_STATE_PAUSED) {
            struct audio_device *adev = out->dev;
            int snd_scard_state = get_snd_card_state(adev);

            if (SND_CARD_STATE_ONLINE == snd_scard_state)
                status = compress_resume(out->compr);

            out->offload_state = OFFLOAD_STATE_PLAYING;
        }
        pthread_mutex_unlock(&out->lock);
    }
    return status;
}

static int out_drain(struct audio_stream_out* stream, audio_drain_type_t type )
{
    struct stream_out *out = (struct stream_out *)stream;
    int status = -ENOSYS;
    ALOGV("%s", __func__);
    if (is_offload_usecase(out->usecase)) {
        pthread_mutex_lock(&out->lock);
        if (type == AUDIO_DRAIN_EARLY_NOTIFY)
            status = send_offload_cmd_l(out, OFFLOAD_CMD_PARTIAL_DRAIN);
        else
            status = send_offload_cmd_l(out, OFFLOAD_CMD_DRAIN);
        pthread_mutex_unlock(&out->lock);
    }
    return status;
}

static int out_flush(struct audio_stream_out* stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    ALOGV("%s", __func__);
    if (is_offload_usecase(out->usecase)) {
        ALOGD("copl(%x):calling compress flush", (unsigned int)out);
        pthread_mutex_lock(&out->lock);
        stop_compressed_output_l(out);
        pthread_mutex_unlock(&out->lock);
        ALOGD("copl(%x):out of compress flush", (unsigned int)out);
        return 0;
    }
    return -ENOSYS;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->config.rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    if(in->usecase == USECASE_COMPRESS_VOIP_CALL)
        return voice_extn_compress_voip_in_get_buffer_size(in);
    else if(audio_extn_compr_cap_usecase_supported(in->usecase))
        return audio_extn_compr_cap_get_buffer_size(in->config.format);

    return in->config.period_size * audio_stream_frame_size(stream);
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->channel_mask;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;

    return in->format;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    int status = 0;
    ALOGD("%s: enter: stream (%p) usecase(%d: %s)", __func__,
          stream, in->usecase, use_case_table[in->usecase]);


    if (in->usecase == USECASE_COMPRESS_VOIP_CALL) {
        /* Ignore standby in case of voip call because the voip input
         * stream is closed in adev_close_input_stream()
         */
        ALOGV("%s: Ignore Standby in VOIP call", __func__);
        return status;
    }

    pthread_mutex_lock(&in->lock);
    if (!in->standby) {
        pthread_mutex_lock(&adev->lock);
        in->standby = true;
        if (in->pcm) {
            pcm_close(in->pcm);
            in->pcm = NULL;
        }
        status = stop_input_stream(in);
        pthread_mutex_unlock(&adev->lock);
    }
    pthread_mutex_unlock(&in->lock);
    ALOGV("%s: exit:  status(%d)", __func__, status);
    return status;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret = 0, val = 0, err;

    ALOGD("%s: enter: kvpairs=%s", __func__, kvpairs);
    parms = str_parms_create_str(kvpairs);

    pthread_mutex_lock(&in->lock);
    pthread_mutex_lock(&adev->lock);

    err = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value, sizeof(value));
    if (err >= 0) {
        val = atoi(value);
        /* no audio source uses val == 0 */
        if ((in->source != val) && (val != 0)) {
            in->source = val;
            if ((in->source == AUDIO_SOURCE_VOICE_COMMUNICATION) &&
                (in->dev->mode == AUDIO_MODE_IN_COMMUNICATION) &&
                (voice_extn_compress_voip_is_format_supported(in->format)) &&
                (in->config.rate == 8000 || in->config.rate == 16000) &&
                (popcount(in->channel_mask) == 1)) {
                err = voice_extn_compress_voip_open_input_stream(in);
                if (err != 0) {
                    ALOGE("%s: Compress voip input cannot be opened, error:%d",
                          __func__, err);
                }
            }
        }
    }

    err = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (err >= 0) {
        val = atoi(value);
        if ((in->device != val) && (val != 0)) {
            in->device = val;
            /* If recording is in progress, change the tx device to new device */
            if (!in->standby)
                ret = select_devices(adev, in->usecase);
        }
    }

done:
    pthread_mutex_unlock(&adev->lock);
    pthread_mutex_unlock(&in->lock);

    str_parms_destroy(parms);
    ALOGV("%s: exit: status(%d)", __func__, ret);
    return ret;
}

static char* in_get_parameters(const struct audio_stream *stream,
                               const char *keys)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct str_parms *query = str_parms_create_str(keys);
    char *str;
    char value[256];
    struct str_parms *reply = str_parms_create();

    if (!query || !reply) {
        ALOGE("in_get_parameters: failed to create query or reply");
        return NULL;
    }

    ALOGV("%s: enter: keys - %s", __func__, keys);

#ifdef MULTI_VOICE_SESSION_ENABLED
    voice_extn_in_get_parameters(in, query, reply);
#endif

    str = str_parms_to_str(reply);
    str_parms_destroy(query);
    str_parms_destroy(reply);

    ALOGV("%s: exit: returns - %s", __func__, str);
    return str;
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void *buffer,
                       size_t bytes)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct audio_device *adev = in->dev;
    int i, ret = -1;
    int snd_scard_state = get_snd_card_state(adev);

    pthread_mutex_lock(&in->lock);

    if (in->pcm) {
        if(SND_CARD_STATE_OFFLINE == snd_scard_state) {
            ALOGD(" %s: sound card is not active/SSR state", __func__);
            ret= -EIO;;
            goto exit;
        }
    }

    if (in->standby) {
        pthread_mutex_lock(&adev->lock);
#ifdef MULTI_VOICE_SESSION_ENABLED
        if (in->usecase == USECASE_COMPRESS_VOIP_CALL)
            ret = voice_extn_compress_voip_start_input_stream(in);
        else
#endif
            ret = start_input_stream(in);
        pthread_mutex_unlock(&adev->lock);
        if (ret != 0) {
            goto exit;
        }
        in->standby = 0;
    }

    if (in->pcm) {
        if (audio_extn_ssr_get_enabled() && popcount(in->channel_mask) == 6)
            ret = audio_extn_ssr_read(stream, buffer, bytes);
        else if (audio_extn_compr_cap_usecase_supported(in->usecase))
            ret = audio_extn_compr_cap_read(in, buffer, bytes);
        else
            ret = pcm_read(in->pcm, buffer, bytes);
    }

    /*
     * Instead of writing zeroes here, we could trust the hardware
     * to always provide zeroes when muted.
     */
    if (ret == 0 && voice_get_mic_mute(adev) && !voice_is_in_call_rec_stream(in))
        memset(buffer, 0, bytes);

exit:
    /* ToDo: There may be a corner case when SSR happens back to back during
        start/stop. Need to post different error to handle that. */
    if (-ENETRESET == ret) {
        set_snd_card_state(adev,SND_CARD_STATE_OFFLINE);
    }
    pthread_mutex_unlock(&in->lock);

    if (ret != 0) {
        memset(buffer, 0, bytes);
        in_standby(&in->stream.common);
        ALOGV("%s: read failed - sleeping for buffer duration", __func__);
        usleep(bytes * 1000000 / audio_stream_frame_size(&in->stream.common) /
                                   in_get_sample_rate(&in->stream.common));
    }
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int add_remove_audio_effect(const struct audio_stream *stream,
                                   effect_handle_t effect,
                                   bool enable)
{
    struct stream_in *in = (struct stream_in *)stream;
    int status = 0;
    effect_descriptor_t desc;

    status = (*effect)->get_descriptor(effect, &desc);
    if (status != 0)
        return status;

    pthread_mutex_lock(&in->lock);
    pthread_mutex_lock(&in->dev->lock);
    if ((in->source == AUDIO_SOURCE_VOICE_COMMUNICATION) &&
            in->enable_aec != enable &&
            (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0)) {
        in->enable_aec = enable;
        if (!in->standby)
            select_devices(in->dev, in->usecase);
    }
    if (in->enable_ns != enable &&
            (memcmp(&desc.type, FX_IID_NS, sizeof(effect_uuid_t)) == 0)) {
        in->enable_ns = enable;
        if (!in->standby)
            select_devices(in->dev, in->usecase);
    }
    pthread_mutex_unlock(&in->dev->lock);
    pthread_mutex_unlock(&in->lock);

    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream,
                               effect_handle_t effect)
{
    ALOGV("%s: effect %p", __func__, effect);
    return add_remove_audio_effect(stream, effect, true);
}

static int in_remove_audio_effect(const struct audio_stream *stream,
                                  effect_handle_t effect)
{
    ALOGV("%s: effect %p", __func__, effect);
    return add_remove_audio_effect(stream, effect, false);
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int i, ret = 0;

    *stream_out = NULL;

    if ((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) &&
             (SND_CARD_STATE_OFFLINE == get_snd_card_state(adev))) {
        ALOGE(" sound card is not active rejecting compress output open request");
        return -EINVAL;
    }

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));

    ALOGD("%s: enter: sample_rate(%d) channel_mask(%#x) devices(%#x) flags(%#x)\
        stream_handle(%p)",__func__, config->sample_rate, config->channel_mask,
        devices, flags, &out->stream);


    if (!out) {
        return -ENOMEM;
    }

    pthread_mutex_init(&out->lock, (const pthread_mutexattr_t *) NULL);
    pthread_cond_init(&out->cond, (const pthread_condattr_t *) NULL);

    if (devices == AUDIO_DEVICE_NONE)
        devices = AUDIO_DEVICE_OUT_SPEAKER;

    out->flags = flags;
    out->devices = devices;
    out->dev = adev;
    out->format = config->format;
    out->sample_rate = config->sample_rate;
    out->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    out->supported_channel_masks[0] = AUDIO_CHANNEL_OUT_STEREO;
    out->handle = handle;
    out->bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;

    /* Init use case and pcm_config */
    if ((out->flags == AUDIO_OUTPUT_FLAG_DIRECT) &&
        (out->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL ||
        out->devices & AUDIO_DEVICE_OUT_PROXY)) {

        pthread_mutex_lock(&adev->lock);
        if (out->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL)
            ret = read_hdmi_channel_masks(out);

        if (out->devices & AUDIO_DEVICE_OUT_PROXY)
            ret = audio_extn_read_afe_proxy_channel_masks(out);
        pthread_mutex_unlock(&adev->lock);
        if (ret != 0)
            goto error_open;

        if (config->sample_rate == 0)
            config->sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
        if (config->channel_mask == 0)
            config->channel_mask = AUDIO_CHANNEL_OUT_5POINT1;

        out->channel_mask = config->channel_mask;
        out->sample_rate = config->sample_rate;
        out->usecase = USECASE_AUDIO_PLAYBACK_MULTI_CH;
        out->config = pcm_config_hdmi_multi;
        out->config.rate = config->sample_rate;
        out->config.channels = popcount(out->channel_mask);
        out->config.period_size = HDMI_MULTI_PERIOD_BYTES / (out->config.channels * 2);
#ifdef COMPRESS_VOIP_ENABLED
    } else if ((out->dev->mode == AUDIO_MODE_IN_COMMUNICATION) &&
               (out->flags == (AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_VOIP_RX)) &&
               (voice_extn_compress_voip_is_config_supported(config))) {
        ret = voice_extn_compress_voip_open_output_stream(out);
        if (ret != 0) {
            ALOGE("%s: Compress voip output cannot be opened, error:%d",
                  __func__, ret);
            goto error_open;
        }
#endif
    } else if (out->flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
        if (config->offload_info.version != AUDIO_INFO_INITIALIZER.version ||
            config->offload_info.size != AUDIO_INFO_INITIALIZER.size) {
            ALOGE("%s: Unsupported Offload information", __func__);
            ret = -EINVAL;
            goto error_open;
        }
        if (!is_supported_format(config->offload_info.format) 
#ifdef DS1_DOLBY_DAP_ENABLED
                && !audio_extn_is_dolby_format(config->offload_info.format)
#endif
           ) {
            ALOGE("%s: Unsupported audio format", __func__);
            ret = -EINVAL;
            goto error_open;
        }

        out->compr_config.codec = (struct snd_codec *)
                                    calloc(1, sizeof(struct snd_codec));

        if (!out->compr_config.codec) {
            ret = -ENOMEM;
            goto error_open;
        }

        out->usecase = get_offload_usecase(adev);
        if (config->offload_info.channel_mask)
            out->channel_mask = config->offload_info.channel_mask;
        else if (config->channel_mask) {
            out->channel_mask = config->channel_mask;
            config->offload_info.channel_mask = config->channel_mask;
        }
        out->format = config->offload_info.format;
        out->sample_rate = config->offload_info.sample_rate;

        out->stream.set_callback = out_set_callback;
        out->stream.pause = out_pause;
        out->stream.resume = out_resume;
        out->stream.drain = out_drain;
        out->stream.flush = out_flush;
        out->bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;

#ifdef DS1_DOLBY_DAP_ENABLED
        if (audio_extn_is_dolby_format(config->offload_info.format))
            out->compr_config.codec->id =
                audio_extn_dolby_get_snd_codec_id(adev, out,
                                                  config->offload_info.format);
        else
#endif
            out->compr_config.codec->id =
                get_snd_codec_id(config->offload_info.format);
#ifndef PLATFORM_MSM8960
        if (audio_is_offload_pcm(config->offload_info.format)) {
            out->compr_config.fragment_size =
                       platform_get_pcm_offload_buffer_size(&config->offload_info);
        } else {
            out->compr_config.fragment_size =
                       platform_get_compress_offload_buffer_size(&config->offload_info);
        }
#else
        out->compr_config.fragment_size = 0;
#endif
        out->compr_config.fragments = COMPRESS_OFFLOAD_NUM_FRAGMENTS;
        out->compr_config.codec->sample_rate =
                    compress_get_alsa_rate(config->offload_info.sample_rate);
        out->compr_config.codec->bit_rate =
                    config->offload_info.bit_rate;
        out->compr_config.codec->ch_in =
                    popcount(config->channel_mask);
        out->compr_config.codec->ch_out = out->compr_config.codec->ch_in;
        out->bit_width = config->offload_info.bit_width;

        if (config->offload_info.format == AUDIO_FORMAT_AAC)
            out->compr_config.codec->format = SND_AUDIOSTREAMFORMAT_RAW;
        if (config->offload_info.format == AUDIO_FORMAT_PCM_16_BIT_OFFLOAD)
            out->compr_config.codec->format = SNDRV_PCM_FORMAT_S16_LE;
        if(config->offload_info.format == AUDIO_FORMAT_PCM_24_BIT_OFFLOAD)
            out->compr_config.codec->format = SNDRV_PCM_FORMAT_S24_LE;

        if (config->offload_info.bit_width == 24) {
            out->compr_config.codec->format = SNDRV_PCM_FORMAT_S24_LE;
        }

        if (out->bit_width == 24 && !platform_check_24_bit_support()) {
            ALOGW("24 bit support is not enabled, using 16 bit backend");
            out->compr_config.codec->format = SNDRV_PCM_FORMAT_S16_LE;
        }

#ifdef FLAC_OFFLOAD_ENABLED
        out->compr_config.codec->options.flac_dec.sample_size = config->offload_info.bit_width;
#endif

        if (flags & AUDIO_OUTPUT_FLAG_NON_BLOCKING)
            out->non_blocking = 1;

        out->send_new_metadata = 1;
        out->offload_state = OFFLOAD_STATE_IDLE;
        out->playback_started = 0;

        create_offload_callback_thread(out);
        ALOGV("%s: offloaded output offload_info version %04x bit rate %d",
                __func__, config->offload_info.version,
                config->offload_info.bit_rate);
        //Decide if we need to use gapless mode by default
        check_and_set_gapless_mode(adev);

#ifdef MULTI_VOICE_SESSION_ENABLED
    } else if (out->flags & AUDIO_OUTPUT_FLAG_INCALL_MUSIC) {
        ret = voice_check_and_set_incall_music_usecase(adev, out);
        if (ret != 0) {
            ALOGE("%s: Incall music delivery usecase cannot be set error:%d",
                  __func__, ret);
            goto error_open;
        }
#endif
    } else if (out->flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) {
        out->usecase = USECASE_AUDIO_PLAYBACK_DEEP_BUFFER;
        out->config = pcm_config_deep_buffer;
        out->sample_rate = out->config.rate;

    } else if (out->flags & AUDIO_OUTPUT_FLAG_FAST) {
        out->usecase = USECASE_AUDIO_PLAYBACK_LOW_LATENCY;
        out->config = pcm_config_low_latency;
        out->sample_rate = out->config.rate;

    } else {
        /* primary path is the default path selected if no other outputs are available/suitable */
        out->usecase = USECASE_AUDIO_PLAYBACK_PRIMARY;
        out->config = pcm_config_primary;
        out->sample_rate = out->config.rate;
    }

    if ((out->usecase == USECASE_AUDIO_PLAYBACK_PRIMARY) ||
        (flags & AUDIO_OUTPUT_FLAG_PRIMARY)) {
        /* Ensure the default output is not selected twice */
        if(adev->primary_output == NULL)
            adev->primary_output = out;
        else {
            ALOGE("%s: Primary output is already opened", __func__);
            ret = -EEXIST;
            goto error_open;
        }
    }

    /* Check if this usecase is already existing */
    pthread_mutex_lock(&adev->lock);
    if ((get_usecase_from_list(adev, out->usecase) != NULL) &&
        (out->usecase != USECASE_COMPRESS_VOIP_CALL)) {
        ALOGE("%s: Usecase (%d) is already present", __func__, out->usecase);
        pthread_mutex_unlock(&adev->lock);
        ret = -EEXIST;
        goto error_open;
    }
    pthread_mutex_unlock(&adev->lock);

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

    out->standby = 1;
    /* out->muted = false; by calloc() */
    /* out->written = 0; by calloc() */

    config->format = out->stream.common.get_format(&out->stream.common);
    config->channel_mask = out->stream.common.get_channels(&out->stream.common);
    config->sample_rate = out->stream.common.get_sample_rate(&out->stream.common);

    *stream_out = &out->stream;
    ALOGD("%s: Stream (%p) picks up usecase (%s)", __func__, &out->stream,
        use_case_table[out->usecase]);
    ALOGV("%s: exit", __func__);
    return 0;

error_open:
    free(out);
    *stream_out = NULL;
    ALOGD("%s: exit: ret %d", __func__, ret);
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    int ret = 0;

    ALOGD("%s: enter:stream_handle(%p)",__func__, out);
#ifdef COMPRESS_VOIP_ENABLED
    if (out->usecase == USECASE_COMPRESS_VOIP_CALL) {
        ret = voice_extn_compress_voip_close_output_stream(&stream->common);
        if(ret != 0)
            ALOGE("%s: Compress voip output cannot be closed, error:%d",
                  __func__, ret);
    }
    else
#endif
        out_standby(&stream->common);

    if (is_offload_usecase(out->usecase)) {
        destroy_offload_callback_thread(out);
        free_offload_usecase(adev, out->usecase);
        if (out->compr_config.codec != NULL)
            free(out->compr_config.codec);
    }
    pthread_cond_destroy(&out->cond);
    pthread_mutex_destroy(&out->lock);
    free(stream);
    ALOGV("%s: exit", __func__);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int val;
    int ret = 0, err;

    ALOGD("%s: enter: %s", __func__, kvpairs);
    parms = str_parms_create_str(kvpairs);

    err = str_parms_get_str(parms, "SND_CARD_STATUS", value, sizeof(value));
    if (err >= 0) {
        char *snd_card_status = value+2;
        if (strstr(snd_card_status, "OFFLINE")) {
            struct listnode *node;
            struct audio_usecase *usecase;

            ALOGD("Received sound card OFFLINE status");
            set_snd_card_state(adev,SND_CARD_STATE_OFFLINE);

            pthread_mutex_lock(&adev->lock);
            //close compress session on OFFLINE status
            usecase = get_usecase_from_list(adev,USECASE_AUDIO_PLAYBACK_OFFLOAD);
            if (usecase && usecase->stream.out) {
                ALOGD(" %s closing compress session on OFFLINE state", __func__);

                struct stream_out *out = usecase->stream.out;

                pthread_mutex_unlock(&adev->lock);
                out_standby(&out->stream.common);
            } else
                pthread_mutex_unlock(&adev->lock);
        } else if (strstr(snd_card_status, "ONLINE")) {
            ALOGD("Received sound card ONLINE status");
            set_snd_card_state(adev,SND_CARD_STATE_ONLINE);
        }
    }

    pthread_mutex_lock(&adev->lock);
    ret = voice_set_parameters(adev, parms);
    if (ret != 0)
        goto done;

    ret = platform_set_parameters(adev->platform, parms);
    if (ret != 0)
        goto done;

    err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_BT_NREC, value, sizeof(value));
    if (err >= 0) {
        /* When set to false, HAL should disable EC and NS
         * But it is currently not supported.
         */
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->bluetooth_nrec = true;
        else
            adev->bluetooth_nrec = false;
    }

    err = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (err >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->screen_off = false;
        else
            adev->screen_off = true;
    }

    err = str_parms_get_int(parms, "rotation", &val);
    if (err >= 0) {
        bool reverse_speakers = false;
        switch(val) {
        // FIXME: note that the code below assumes that the speakers are in the correct placement
        //   relative to the user when the device is rotated 90deg from its default rotation. This
        //   assumption is device-specific, not platform-specific like this code.
        case 270:
            reverse_speakers = true;
            break;
        case 0:
        case 90:
        case 180:
            break;
        default:
            ALOGE("%s: unexpected rotation of %d", __func__, val);
        }
        if (adev->speaker_lr_swap != reverse_speakers) {
            adev->speaker_lr_swap = reverse_speakers;
            // only update the selected device if there is active pcm playback
            struct audio_usecase *usecase;
            struct listnode *node;
            list_for_each(node, &adev->usecase_list) {
                usecase = node_to_item(node, struct audio_usecase, list);
                if (usecase->type == PCM_PLAYBACK) {
                    select_devices(adev, usecase->id);
                    break;
                }
            }
        }
    }

    audio_extn_set_parameters(adev, parms);

done:
    str_parms_destroy(parms);
    pthread_mutex_unlock(&adev->lock);
    ALOGV("%s: exit with code(%d)", __func__, ret);
    return ret;
}

static char* adev_get_parameters(const struct audio_hw_device *dev,
                                 const char *keys)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct str_parms *reply = str_parms_create();
    struct str_parms *query = str_parms_create_str(keys);
    char *str;
    char value[256] = {0};
    int ret = 0;

    if (!query || !reply) {
        ALOGE("adev_get_parameters: failed to create query or reply");
        return NULL;
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_KEY_SND_CARD_STATUS, value,
                            sizeof(value));
    if (ret >=0) {
        int val = 1;
        pthread_mutex_lock(&adev->snd_card_status.lock);
        if (SND_CARD_STATE_OFFLINE == adev->snd_card_status.state)
            val = 0;
        pthread_mutex_unlock(&adev->snd_card_status.lock);
        str_parms_add_int(reply, AUDIO_PARAMETER_KEY_SND_CARD_STATUS, val);
        goto exit;
    }

    pthread_mutex_lock(&adev->lock);
    audio_extn_get_parameters(adev, query, reply);
    voice_get_parameters(adev, query, reply);
    platform_get_parameters(adev->platform, query, reply);
    pthread_mutex_unlock(&adev->lock);

exit:
    str = str_parms_to_str(reply);
    str_parms_destroy(query);
    str_parms_destroy(reply);

    ALOGV("%s: exit: returns - %s", __func__, str);
    return str;
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    int ret;
    struct audio_device *adev = (struct audio_device *)dev;
    pthread_mutex_lock(&adev->lock);
    /* cache volume */
    ret = voice_set_volume(adev, volume);
    pthread_mutex_unlock(&adev->lock);
    return ret;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev,
                                  float *volume)
{
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    struct audio_device *adev = (struct audio_device *)dev;
    pthread_mutex_lock(&adev->lock);
    if (adev->mode != mode) {
        ALOGD("%s mode %d\n", __func__, mode);
        adev->mode = mode;
    }
    pthread_mutex_unlock(&adev->lock);
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    int ret;

    pthread_mutex_lock(&adev->lock);
    ALOGD("%s state %d\n", __func__, state);
    ret = voice_set_mic_mute((struct audio_device *)dev, state);
    pthread_mutex_unlock(&adev->lock);

    return ret;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    *state = voice_get_mic_mute((struct audio_device *)dev);
    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    int channel_count = popcount(config->channel_mask);

    return get_input_buffer_size(config->sample_rate, config->format, channel_count);
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_in *in;
    int ret = 0, buffer_size, frame_size;
    int channel_count = popcount(config->channel_mask);


    *stream_in = NULL;
    if (check_input_parameters(config->sample_rate, config->format, channel_count) != 0)
        return -EINVAL;

    in = (struct stream_in *)calloc(1, sizeof(struct stream_in));

    if (!in) {
        ALOGE("failed to allocate input stream");
        return -ENOMEM;
    }

    ALOGD("%s: enter: sample_rate(%d) channel_mask(%#x) devices(%#x)\
        stream_handle(%p)",__func__, config->sample_rate, config->channel_mask,
        devices, &in->stream);

    pthread_mutex_init(&in->lock, (const pthread_mutexattr_t *) NULL);

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

    in->device = devices;
    in->source = AUDIO_SOURCE_DEFAULT;
    in->dev = adev;
    in->standby = 1;
    in->channel_mask = config->channel_mask;

    /* Update config params with the requested sample rate and channels */
    in->usecase = USECASE_AUDIO_RECORD;
    in->config = pcm_config_audio_capture;
    in->config.rate = config->sample_rate;
    in->format = config->format;

    if (channel_count == 6) {
        if(audio_extn_ssr_get_enabled()) {
            if(audio_extn_ssr_init(adev, in)) {
                ALOGE("%s: audio_extn_ssr_init failed", __func__);
                ret = -EINVAL;
                goto err_open;
            }
        } else {
            ret = -EINVAL;
            goto err_open;
        }
    } else if (audio_extn_compr_cap_enabled() &&
            audio_extn_compr_cap_format_supported(config->format) &&
            (in->dev->mode != AUDIO_MODE_IN_COMMUNICATION)) {
        audio_extn_compr_cap_init(adev, in);
    } else {
        in->usecase = adev->low_latency_recording ?
            USECASE_AUDIO_RECORD_LOW_LATENCY : USECASE_AUDIO_RECORD;

        in->config.channels = channel_count;
        frame_size = audio_stream_frame_size((struct audio_stream *)in);
        buffer_size = get_input_buffer_size(config->sample_rate,
                                            config->format,
                                            channel_count);
        in->config.period_size = buffer_size / frame_size;
    }

    *stream_in = &in->stream;
    ALOGV("%s: exit", __func__);
    return ret;

err_open:
    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                    struct audio_stream_in *stream)
{
    int ret;
    struct stream_in *in = (struct stream_in *)stream;
    ALOGD("%s: enter:stream_handle(%p)",__func__, in);

#ifdef COMPRESS_VOIP_ENABLED
    if (in->usecase == USECASE_COMPRESS_VOIP_CALL) {
        ret = voice_extn_compress_voip_close_input_stream(&stream->common);
        if (ret != 0)
            ALOGE("%s: Compress voip input cannot be closed, error:%d",
                  __func__, ret);
    } else
#endif
        in_standby(&stream->common);

    if (audio_extn_ssr_get_enabled() && (popcount(in->channel_mask) == 6)) {
        audio_extn_ssr_deinit();
    }
    free(stream);

    if(audio_extn_compr_cap_enabled() &&
            audio_extn_compr_cap_format_supported(in->config.format))
        audio_extn_compr_cap_deinit();
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    if (!adev)
        return 0;

    pthread_mutex_lock(&adev_init_lock);

    if ((--audio_device_ref_count) == 0) {
        audio_extn_listen_deinit(adev);
        audio_route_free(adev->audio_route);
        free(adev->snd_dev_ref_cnt);
        platform_deinit(adev->platform);
        free(device);
        adev = NULL;
    }
    pthread_mutex_unlock(&adev_init_lock);
    return 0;
}

static int adev_open(const hw_module_t *module, const char *name,
                     hw_device_t **device)
{
    int i, ret;
    char value[PROPERTY_VALUE_MAX] = {0};

    ALOGD("%s: enter", __func__);
    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) return -EINVAL;

    pthread_mutex_lock(&adev_init_lock);
    if (audio_device_ref_count != 0){
            *device = &adev->device.common;
            audio_device_ref_count++;
            ALOGD("%s: returning existing instance of adev", __func__);
            ALOGD("%s: exit", __func__);
            pthread_mutex_unlock(&adev_init_lock);
            return 0;
    }

    adev = calloc(1, sizeof(struct audio_device));

    if (!adev) {
        pthread_mutex_unlock(&adev_init_lock);
        return -ENOMEM;
    }

    pthread_mutex_init(&adev->lock, (const pthread_mutexattr_t *) NULL);

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
    adev->out_device = AUDIO_DEVICE_NONE;
    adev->bluetooth_nrec = true;
    adev->acdb_settings = TTY_MODE_OFF;
    /* adev->cur_hdmi_channels = 0;  by calloc() */
    adev->cur_codec_backend_samplerate = CODEC_BACKEND_DEFAULT_SAMPLE_RATE;
    adev->cur_codec_backend_bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    adev->snd_dev_ref_cnt = calloc(SND_DEVICE_MAX, sizeof(int));
    voice_init(adev);
    list_init(&adev->usecase_list);
    adev->cur_wfd_channels = 2;
    adev->offload_usecases_state = 0;

    property_get("audio.lowlatency.record.enabled", value, "false");
    adev->low_latency_recording = atoi(value) || !strncmp("true", value, 4);

    pthread_mutex_init(&adev->snd_card_status.lock, (const pthread_mutexattr_t *) NULL);
    adev->snd_card_status.state = SND_CARD_STATE_OFFLINE;
    /* Loads platform specific libraries dynamically */
    adev->platform = platform_init(adev);
    if (!adev->platform) {
        free(adev->snd_dev_ref_cnt);
        free(adev);
        ALOGE("%s: Failed to init platform data, aborting.", __func__);
        *device = NULL;
        pthread_mutex_unlock(&adev_init_lock);
        return -EINVAL;
    }

    adev->snd_card_status.state = SND_CARD_STATE_ONLINE;

    if (access(VISUALIZER_LIBRARY_PATH, R_OK) == 0) {
        adev->visualizer_lib = dlopen(VISUALIZER_LIBRARY_PATH, RTLD_NOW);
        if (adev->visualizer_lib == NULL) {
            ALOGE("%s: DLOPEN failed for %s", __func__, VISUALIZER_LIBRARY_PATH);
        } else {
            ALOGV("%s: DLOPEN successful for %s", __func__, VISUALIZER_LIBRARY_PATH);
            adev->visualizer_start_output =
                        (int (*)(audio_io_handle_t, int))dlsym(adev->visualizer_lib,
                                                        "visualizer_hal_start_output");
            adev->visualizer_stop_output =
                        (int (*)(audio_io_handle_t, int))dlsym(adev->visualizer_lib,
                                                        "visualizer_hal_stop_output");
        }
    }
    audio_extn_listen_init(adev, adev->snd_card);

    if (access(OFFLOAD_EFFECTS_BUNDLE_LIBRARY_PATH, R_OK) == 0) {
        adev->offload_effects_lib = dlopen(OFFLOAD_EFFECTS_BUNDLE_LIBRARY_PATH, RTLD_NOW);
        if (adev->offload_effects_lib == NULL) {
            ALOGE("%s: DLOPEN failed for %s", __func__,
                  OFFLOAD_EFFECTS_BUNDLE_LIBRARY_PATH);
        } else {
            ALOGV("%s: DLOPEN successful for %s", __func__,
                  OFFLOAD_EFFECTS_BUNDLE_LIBRARY_PATH);
            adev->offload_effects_start_output =
                        (int (*)(audio_io_handle_t, int))dlsym(adev->offload_effects_lib,
                                         "offload_effects_bundle_hal_start_output");
            adev->offload_effects_stop_output =
                        (int (*)(audio_io_handle_t, int))dlsym(adev->offload_effects_lib,
                                         "offload_effects_bundle_hal_stop_output");
        }
    }

    *device = &adev->device.common;

    audio_device_ref_count++;
    pthread_mutex_unlock(&adev_init_lock);

    ALOGV("%s: exit", __func__);
    return 0;
}

int pcm_ioctl(struct pcm *pcm, int request, ...)
{
    va_list ap;
    void * arg;
    int pcm_fd = *(int*)pcm;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    return ioctl(pcm_fd, request, arg);
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
        .name = "QCOM Audio HAL",
        .author = "The Linux Foundation",
        .methods = &hal_module_methods,
    },
};
