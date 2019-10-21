/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "CodecDevice"

#include "CodecDevice.h"
#include <tinyalsa/asoundlib.h>
#include "CodecDeviceAlsa.h"
#include "CodecDeviceGsl.h"
#include "ResourceManager.h"
#include "Device.h"
#include "Speaker.h"
#include "Headphone.h"
#include "SpeakerMic.h"
#include "CodecDeviceImpl.h"
#include "Stream.h"


std::shared_ptr<Device> CodecDevice::getInstance(struct qal_device *device,
                                                 std::shared_ptr<ResourceManager> Rm)
{
    if (!device || !Rm) {
        QAL_ERR(LOG_TAG, "Invalid input parameters");
        return NULL;
    }

    QAL_ERR(LOG_TAG, "Enter device id %d", device->id);


    QAL_ERR(LOG_TAG, "Device config updated");

    //TBD: decide on supported devices from XML and not in code
    switch(device->id) {
    case QAL_DEVICE_OUT_SPEAKER:
        QAL_VERBOSE(LOG_TAG, "speaker device");
        return Speaker::getInstance(device, Rm);
        break;
    case QAL_DEVICE_OUT_WIRED_HEADSET:
    case QAL_DEVICE_OUT_WIRED_HEADPHONE:
        QAL_VERBOSE(LOG_TAG, "headphone device");
        return Headphone::getInstance(device, Rm);
        break;
    case QAL_DEVICE_IN_SPEAKER_MIC:
    case QAL_DEVICE_IN_HANDSET_MIC:
    case QAL_DEVICE_IN_TRI_MIC:
    case QAL_DEVICE_IN_QUAD_MIC:
    case QAL_DEVICE_IN_EIGHT_MIC:
        QAL_VERBOSE(LOG_TAG, "speakerMic device");
        return SpeakerMic::getInstance(device, Rm);
        break;
    default:
        QAL_ERR(LOG_TAG,"Unsupported device id %d",device->id);
        return nullptr;
    }
}


CodecDevice::CodecDevice(struct qal_device *device, std::shared_ptr<ResourceManager> Rm)
{
    struct qal_channel_info *codec_device_ch_info;
    uint16_t channels = device->config.ch_info->channels;
    uint16_t ch_info_size = sizeof(uint16_t) + sizeof(uint8_t)*channels;
    rm = Rm;

    codec_device_ch_info = (struct qal_channel_info *) calloc(1, ch_info_size);
    if (codec_device_ch_info == NULL) {
        QAL_ERR(LOG_TAG, "Allocation failed for channel map");
    }

    memset(&deviceAttr, 0, sizeof(struct qal_device));
    casa_osal_memcpy(&deviceAttr, sizeof(struct qal_device), device,
                     sizeof(struct qal_device));
    // copy channel info
    deviceAttr.config.ch_info = codec_device_ch_info;
    casa_osal_memcpy(deviceAttr.config.ch_info, ch_info_size, device->config.ch_info,
                     ch_info_size);
    mQALDeviceName.clear();

}

CodecDevice::CodecDevice()
{
    initialized = false;
    mQALDeviceName.clear();
}

CodecDevice::~CodecDevice()
{
    if (deviceAttr.config.ch_info)
        free(deviceAttr.config.ch_info);
}

int CodecDevice::open()
{
    int status = 0;
    mDeviceMutex.lock();
    QAL_ERR(LOG_TAG, "Enter. device count %d for device id %d, initialized %d",
        deviceCount, this->deviceAttr.id, initialized);
    void *stream;
    std::vector<Stream*> activestreams;
    CodecDeviceImpl *codecImpl;

    if(!initialized) {
        const qal_alsa_or_gsl alsaConf = rm->getQALConfigALSAOrGSL();
        if (ALSA == alsaConf) {
            codecImpl = new CodecDeviceAlsa();
        } else {
            codecImpl = new CodecDeviceGsl();
        }
        if (!codecImpl) {
            status = -ENOMEM;
            QAL_ERR(LOG_TAG, "CodecDeviceImpl instantiation failed status %d", status);
            goto exit;
        }
        status = codecImpl->open(&(this->deviceAttr), rm);
        if(0!= status) {
            status = -EINVAL;
            QAL_ERR(LOG_TAG,"Failed to open the device");
            delete codecImpl;
            goto exit;
        }

        deviceHandle = static_cast<void *>(codecImpl);
        mQALDeviceName = rm->getQALDeviceName(this->deviceAttr.id);
        initialized = true;
        QAL_ERR(LOG_TAG, "Device name %s, device id %d initialized %d", mQALDeviceName.c_str(), this->deviceAttr.id, initialized);
    }

    devObj = CodecDevice::getInstance(&deviceAttr, rm);
    status = rm->getActiveStream(devObj, activestreams);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "getActiveStream failed status %d, need not be fatal", status);
        status = 0;
        //free(deviceHandle);
        goto exit;
    }

    //TBD:: why is this required?
    for (int i = 0; i < activestreams.size(); i++) {
        stream = static_cast<void *>(activestreams[i]);
        QAL_VERBOSE(LOG_TAG, "Stream handle :%pK", activestreams[i]);
    }
    QAL_DBG(LOG_TAG, "Exit. device count %d", deviceCount);
exit:
    mDeviceMutex.unlock();
    return status;
}

int CodecDevice::close()
{
    int status = 0;
    mDeviceMutex.lock();
    QAL_DBG(LOG_TAG, "Enter. device id %d, device name %s, count %d", deviceAttr.id, mQALDeviceName.c_str(), deviceCount);
    if (deviceCount == 0 && initialized) {
        CodecDeviceImpl *codecDev = static_cast<CodecDeviceImpl *>(deviceHandle);
        if (!codecDev) {
            status = -EINVAL;
            QAL_ERR(LOG_TAG, "Invalid device handle status %d", status);
            goto exit;
        }
        status = codecDev->close();
        if (0 != status) {
            status = -ENOMEM;
            QAL_ERR(LOG_TAG, "Failed to close the device status %d", status);
        }
        delete codecDev;
        initialized = false;
        deviceHandle = nullptr;
    }
    QAL_DBG(LOG_TAG, "Exit. device count %d", deviceCount);
exit :
    mDeviceMutex.unlock();
    return status;
}

int CodecDevice::prepare()
{
    int status = 0;
    mDeviceMutex.lock();
    QAL_DBG(LOG_TAG, "Enter. device id %d, device name %s, count %d", deviceAttr.id, mQALDeviceName.c_str(), deviceCount);
    if (deviceCount == 0 && initialized) {
        CodecDeviceImpl *codecDev = static_cast<CodecDeviceImpl *>(deviceHandle);
         if (!codecDev) {
            status = -EINVAL;
            QAL_ERR(LOG_TAG, "Invalid device handle status %d", status);
            goto exit;
        }
        status = codecDev->prepare();
        if (0 != status) {
            status = -EINVAL;
            QAL_ERR(LOG_TAG, "Codec Prepare failed status %d", status);
            goto exit;
        }
    }
    QAL_DBG(LOG_TAG, "%s: Exit. device count %d", deviceCount);
exit :
    mDeviceMutex.unlock();
    return status;
}

int CodecDevice::start()
{
    int status = 0;
    mDeviceMutex.lock();

    QAL_ERR(LOG_TAG, "Enter %d count, initialized %d", deviceCount, initialized);
    if (deviceCount == 0 && initialized) {
        status = rm->getAudioRoute(&audioRoute);
        if (0 != status) {
            QAL_ERR(LOG_TAG, "Failed to get the audio_route address status %d", status);
            goto exit;
        }
        status = rm->getSndDeviceName(deviceAttr.id , mSndDeviceName); //getsndName

        QAL_VERBOSE(LOG_TAG, "%s: audio_route %pK SND device name %s", __func__, audioRoute, mSndDeviceName);
        if (0 != status) {
            QAL_ERR(LOG_TAG, "Failed to obtain the device name from ResourceManager status %d", status);
            goto exit;
        }

        enableDevice(audioRoute, mSndDeviceName);

        CodecDeviceImpl *codecDev = static_cast<CodecDeviceImpl *>(deviceHandle);
        if (!codecDev) {
            status = -EINVAL;
            QAL_ERR(LOG_TAG, "Invalid device handle status %d", status);
            goto exit;
        }
        status = codecDev->prepare();
        if (0 != status) {
            QAL_ERR(LOG_TAG, "Failed to prepare the device status %d", status);
            goto exit;
        }
        status = codecDev->start();
        if(0 != status)
        {
            QAL_ERR(LOG_TAG,"%s: Failed to start the device", __func__);
            goto exit;
        }
    }
    deviceCount += 1;
    QAL_DBG(LOG_TAG, "Exit. device count %d", deviceCount);
exit :
    mDeviceMutex.unlock();
    return status;
}

int CodecDevice::stop()
{
    int status = 0;
    mDeviceMutex.lock();
    QAL_DBG(LOG_TAG, "Enter. device id %d, device name %s, count %d", deviceAttr.id, mQALDeviceName.c_str(), deviceCount);
    if(deviceCount == 1 && initialized) {
        disableDevice(audioRoute, mSndDeviceName);
        CodecDeviceImpl *codecDev = static_cast<CodecDeviceImpl *>(deviceHandle);
        if (!codecDev) {
            status = -EINVAL;
            QAL_ERR(LOG_TAG, "Invalid device handle status %d", status);
            goto exit;
        }
        status = codecDev->stop();
        if (0 != status) {
            QAL_ERR(LOG_TAG, "Codec Device Stop failed status %d", status);
            goto exit;
        }
    }
    deviceCount -= 1;
    QAL_DBG(LOG_TAG, "Exit. device count %d", deviceCount);
exit :
    mDeviceMutex.unlock();
    return status;
}
