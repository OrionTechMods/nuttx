/*
 * Copyright (c) 2015 Motorola Mobility, LLC.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <ctype.h>
#include <errno.h>
#include <debug.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arch/byteorder.h>
#include <nuttx/audio/audio.h>
#include <nuttx/i2c.h>
#include <nuttx/kmalloc.h>
#include <arch/board/tfa9890_firmware.h>

#include "tfa9890.h"

#define CLRBIT(val, mask)   (val & ~(mask))
#define SETBIT(val, mask)   (val | mask)

#define CLRBITS(val, mask)  CLRBIT(val, mask)
#define SETBITS(val, mask, s) (CLRBITS(val, mask) | s)

#define TFA9890_STATUS_UP_MASK  (TFA9890_STATUS_PLLS | \
                    TFA9890_STATUS_CLKS | \
                    TFA9890_STATUS_VDDS | \
                    TFA9890_STATUS_ARFS | TFA9890_STATUS_MTPB)

struct tfa9890_dev_s {
    struct audio_lowerhalf_s dev;
    bool is_dsp_cfg_done;
    FAR struct i2c_dev_s *i2c;
    uint32_t i2c_addr;
    int current_eq;
    int vol_step;
    int speaker_imp;
};

static int tfa9890_reg_read(FAR struct tfa9890_dev_s *priv, uint8_t reg);
static int tfa9890_reg_write(FAR struct tfa9890_dev_s *priv, uint8_t reg,
                uint16_t val);
static int tfa9890_modify(FAR struct tfa9890_dev_s *priv, uint16_t reg,
                uint16_t mask, uint16_t s, uint16_t offset);

/* lower half ops */
static int      tfa9890_configure(FAR struct audio_lowerhalf_s *dev,
                  FAR const struct audio_caps_s *caps);
static int      tfa9890_stop(FAR struct audio_lowerhalf_s *dev);
static int      tfa9890_start(FAR struct audio_lowerhalf_s *dev);

static const struct audio_ops_s g_audioops =
{
    NULL,                  /* getcaps        */
    tfa9890_configure,     /* configure      */
    NULL,                  /* shutdown       */
    tfa9890_start,         /* start          */
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
    tfa9890_stop,          /* stop           */
#endif
#ifndef CONFIG_AUDIO_EXCLUDE_PAUSE_RESUME
    NULL,                  /* pause          */
    NULL,                  /* resume         */
#endif
    NULL,                  /* allocbuffer    */
    NULL,                  /* freebuffer     */
    NULL,                  /* enqueue_buffer */
    NULL,                  /* cancel_buffer  */
    NULL,                  /* ioctl          */
    NULL,                  /* read           */
    NULL,                  /* write          */
    NULL,                  /* reserve        */
    NULL                   /* release        */
};

struct preset_entry
{
    uint8_t data[TFA9890_PST_FW_SIZE];
};

struct preset
{
   const int cnt;
   struct preset_entry *preset;
};

struct preset music_preset =
{
   .cnt = sizeof(tfa9890_music_table_preset) / sizeof(struct preset_entry),
   .preset = (struct preset_entry *) tfa9890_music_table_preset,
};

static uint8_t chunk_buf[TFA9890_MAX_I2C_SIZE + 1];
static sem_t bulk_write_lock = SEM_INITIALIZER(1);

#ifdef DEBUG
static void tfa9890_dump_reg(FAR struct tfa9890_dev_s *priv, uint16_t reg)
{
    int ret = tfa9890_reg_read(priv, reg);
    if (ret < 0)
    {
         lldbg("failed to read 0x%04x\n", reg);
         return;
    }
    lldbg("tfa9890 reg 0x%04x value 0x%04x\n", reg, ret);
}

static void tfa9890_dump_regs(FAR struct tfa9890_dev_s *priv)
{
     uint16_t i;

     for (i = 0; i <= 0xB; i++)
     {
          tfa9890_dump_reg(priv, i);
     }
     tfa9890_dump_reg(priv, 0x80);
}
#endif

int tfa9890_reg_read(FAR struct tfa9890_dev_s *priv, uint8_t reg)
{
     uint8_t reg_val[2];
     int ret;

     I2C_SETADDRESS(priv->i2c, priv->i2c_addr, 7);

     ret = I2C_WRITEREAD(priv->i2c, (uint8_t *)&reg, sizeof(reg),
              reg_val, sizeof(reg_val));
     if (ret)
         return ret;

#ifdef CONFIG_ENDIAN_BIG
     ret = *((uint16_t *)reg_val);
#else
     ret = be16_to_cpu(*((uint16_t *)reg_val));
#endif
     return ret;
}

int tfa9890_reg_write(FAR struct tfa9890_dev_s *priv, uint8_t reg, uint16_t val)
{
    uint8_t buf[3];

    I2C_SETADDRESS(priv->i2c, priv->i2c_addr, 7);

    buf[0] = reg;
#ifdef CONFIG_ENDIAN_BIG
    buf[1] = val & 0x00ff;
    buf[2] = (val & 0xff00) >> 8;
#else
    buf[2] = val & 0x00ff;
    buf[1] = (val & 0xff00) >> 8;
#endif
    return I2C_WRITE(priv->i2c, (uint8_t *)buf, 4);
}

static int tfa9890_modify(FAR struct tfa9890_dev_s *priv, uint16_t reg,
                                    uint16_t mask, uint16_t s, uint16_t offset)
{
    int val = tfa9890_reg_read(priv, reg);

    if (val < 0)
    {
        lldbg("reg_read failed for 0x%02x rv = %d\n", reg, val);
        return val;
    }

    val = SETBITS(val, mask << offset, s << offset);
    return tfa9890_reg_write(priv, reg, val);
}

static int tfa9890_bulk_read(FAR struct i2c_dev_s *i2c_dev, uint32_t i2c_addr,
                                           uint8_t reg, uint8_t *data, int len)
{
    int ret = 0;
    int offset = 0;
    int remaining_bytes = len;
    int chunk_size = TFA9890_MAX_I2C_SIZE;

    struct i2c_msg_s msgs[] =
    {
        {
            .addr = i2c_addr,
            .flags = 0,
            .buffer = &reg,
            .length = 1,
        },
        {
            .addr = i2c_addr,
            .flags = I2C_M_READ,
            .buffer = NULL,
            .length = len,
        },
    };

    while (remaining_bytes > 0)
    {
        if (remaining_bytes < chunk_size)
            chunk_size = remaining_bytes;
        msgs[1].buffer = data + offset;
        I2C_TRANSFER(i2c_dev, msgs, chunk_size);
        offset = offset + chunk_size;
        remaining_bytes = remaining_bytes - chunk_size;
    }

    return ret;
}

static int tfa9890_bulk_write(FAR struct i2c_dev_s *i2c_dev, uint32_t i2c_addr,
                uint8_t reg, const void *data, size_t len)
{
    int offset = 0;
    int ret = 0;

    /* first byte is mem address */
    int remaining_bytes = len - 1;
    int chunk_size = TFA9890_MAX_I2C_SIZE;

    struct i2c_msg_s msg[] =
    {
        {
            .addr = i2c_addr,
            .flags = 0,
            .buffer = chunk_buf,
            .length = len,
        },
    };
    sem_wait(&bulk_write_lock);
    chunk_buf[0] = reg & 0xff;
    while ((remaining_bytes > 0))
    {
        if (remaining_bytes <= chunk_size)
           chunk_size = remaining_bytes;

        memcpy(chunk_buf + 1, data + 1 + offset, chunk_size);
        msg[0].length = chunk_size +1;
        ret = I2C_TRANSFER(i2c_dev, msg, 1);

        if (ret)
        {
            lldbg("I2C_TRANSFER error = %d\n", ret);
            goto out;
        }

        offset += chunk_size;
        remaining_bytes -= chunk_size;

    }
out:
    sem_post(&bulk_write_lock);
    return ret;
}

/* RPC Protocol to Access DSP Memory is implemented in tfa9890_dsp_transfer func
 *- The host writes into a certain DSP memory location the data for the RPC
 *- Module and Id of the function
 *- Parameters for the function (in case of SetParam), maximum 145 parameters
 *- The host triggers the DSP that there is valid RPC data
 *  - The DSP executes the RPC and stores the result
 *      -An error code
 *      -Return parameters (in case of GetParam)
 *  - The host reads the result
 */

static int tfa9890_dsp_transfer(FAR struct tfa9890_dev_s *priv, int module_id,
                int param_id, const uint8_t *bytes,
                int len, int type, uint8_t *read)
{
    uint8_t buffer[8];
    /* DSP mem access control */
    uint16_t cf_ctrl = TFA9890_CF_CTL_MEM_REQ;
    /* memory address to be accessed (0 : Status, 1 : ID, 2 : parameters) */
    uint16_t cf_mad = TFA9890_CF_MAD_ID;
    int err;
    int rpc_status;
    int cf_status;

    /* first the data for CF_CONTROLS */
    buffer[0] = TFA9890_CF_CONTROLS;
    buffer[1] = ((cf_ctrl >> 8) & 0xFF);
    buffer[2] = (cf_ctrl & 0xFF);
    /* write the contents of CF_MAD which is the subaddress
     * following CF_CONTROLS.
     */
    buffer[3] = ((cf_mad >> 8) & 0xFF);
    buffer[4] = (cf_mad & 0xFF);
    /* write the module and RPC id into CF_MEM, which follows CF_MAD */
    buffer[5] = 0;
    buffer[6] = module_id + 128;
    buffer[7] = param_id;

    err = tfa9890_bulk_write(priv->i2c, priv->i2c_addr, TFA9890_CF_CONTROLS,
                         buffer, sizeof(buffer));
    if (err < 0)
    {
        lldbg("tfa9890: Failed to Write DSP controls req %d:\n", err);
        return err;
    }
    /* check transfer type */
    if (type == TFA9890_DSP_WRITE)
    {
        /* write data to DSP memory */
        err = tfa9890_bulk_write(priv->i2c, priv->i2c_addr, TFA9890_CF_MEM,
                         bytes, len);
        if (err < 0)
          return err;
    }

    /* wake up the DSP to process the data */
    cf_ctrl |= TFA9890_CF_ACK_REQ | TFA9890_CF_INT_REQ;
    err = tfa9890_reg_write(priv, TFA9890_CF_CONTROLS, cf_ctrl);
    if (err)
    {
        lldbg("failed to wake up DSP to process the data err = %d\n", err);
        return err;
    }
    /* wait for ~1 msec (max per spec) and check for DSP status */
    usleep(1000);
    cf_status = tfa9890_reg_read(priv, TFA9890_CF_STATUS);
    if ((cf_status & TFA9890_STATUS_CF) == 0)
    {
        lldbg("tfa9890: Failed to ack DSP CTL req 0x%04x:\n", cf_status);
        return -EIO;
    }

    /* read the RPC Status */
    cf_ctrl = TFA9890_CF_CTL_MEM_REQ;
    buffer[0] = TFA9890_CF_CONTROLS;
    buffer[1] = (unsigned char)((cf_ctrl >> 8) & 0xFF);
    buffer[2] = (unsigned char)(cf_ctrl & 0xFF);
    cf_mad = TFA9890_CF_MAD_STATUS;
    buffer[3] = (unsigned char)((cf_mad >> 8) & 0xFF);
    buffer[4] = (unsigned char)(cf_mad & 0xFF);
    err = tfa9890_bulk_write(priv->i2c, priv->i2c_addr, TFA9890_CF_CONTROLS,
                        buffer, 5);
    if (err < 0)
    {
        lldbg("tfa9890: Failed to Write NXP DSP CTL reg for rpc check %d\n",
           err);
        return err;
    }

    rpc_status = tfa9890_reg_read(priv, TFA9890_CF_MEM);
    if (rpc_status != TFA9890_STATUS_OK)
    {
        lldbg("tfa9890: RPC status check failed %x\n", rpc_status);
        return -EIO;
    }
    if (type == TFA9890_DSP_READ)
    {
        cf_mad = TFA9890_CF_MAD_PARAM;
        tfa9890_reg_write(priv, TFA9890_CF_MAD, cf_mad);
        tfa9890_bulk_read(priv->i2c, priv->i2c_addr, TFA9890_CF_MEM, read, len);
    }

    return 0;
}

static int tfa9887_load_dsp_patch(FAR struct tfa9890_dev_s *priv,
                                            const uint8_t *fw, int fw_len)
{
    int index = 0;
    int length;
    int size = 0;
    const uint8_t *fw_data;
    int err = -EINVAL;

    length = fw_len - TFA9890_PATCH_HEADER;
    fw_data = fw + TFA9890_PATCH_HEADER;

    /* process the firmware */
    while (index < length)
    {
        /* extract length from first two bytes*/
        size = *(fw_data + index) + (*(fw_data + index + 1)) * 256;
        index += 2;
        if ((index + size) > length)
        {
            /* outside the buffer, error in the input data */
            lldbg("tfa9890: invalid length\n");
            goto out;
        }
        if ((size) > TFA9890_MAX_I2C_SIZE)
        {
            /* too big */
            lldbg("tfa9890: ivalid dsp patch\n");
            goto out;
        }
        err = tfa9890_bulk_write(priv->i2c, priv->i2c_addr, *(fw_data + index),
                     (fw_data + index), size);
        if (err < 0)
        {
            lldbg("tfa9890: writing dsp patch failed\n");
            goto out;
        }
        index += size;
    }
    err = 0;

out:
    return err;
}

void tfa9890_powerup(FAR struct tfa9890_dev_s *priv, int on)
{
    int tries = 0;

    if (on)
    {
        /* power on */
        tfa9890_modify(priv, TFA9890_REG_SYSTEM_CTRL_1, 1, 0, TFA9890_PWDN_OFFSET);
        /* amp on */
        tfa9890_modify(priv, TFA9890_REG_SYSTEM_CTRL_1, 1, 1, TFA9890_AMP_OFFSET);
    }
    else
    {
        /* amp off */
        tfa9890_modify(priv, TFA9890_REG_SYSTEM_CTRL_1, 1, 0, TFA9890_AMP_OFFSET);
        do
        {
            /* need to wait for amp to stop switching, to minimize
             * pop, before turning OFF IC.
             */
            if (tfa9890_reg_read(priv, TFA9890_REG_SYSTEM_STATUS)
                                   & TFA9890_STATUS_AMP_SWS)
                break;
            usleep(1000);
        } while (++tries < 20);

        tfa9890_modify(priv, TFA9890_REG_SYSTEM_CTRL_1, 1, 1, TFA9890_PWDN_OFFSET);
     }
}

int tfa9890_load_config(FAR struct tfa9890_dev_s *priv)
{
    int ret = -EIO;

    ret = tfa9887_load_dsp_patch(priv, tfa9890_n1c2_patch,
                     tfa9890_n1c2_patch_len);
    if (ret)
    {
        lldbg("tfa9890: Failed to load dsp patch!!\n");
        goto out;
    }

    ret = tfa9890_dsp_transfer(priv, TFA9890_DSP_MOD_SPEAKERBOOST,
                   TFA9890_PARAM_SET_LSMODEL, tfa9890_speaker,
                   tfa9890_speaker_len, TFA9890_DSP_WRITE, 0);
    if (ret)
    {
        lldbg("tfa9890: Failed to load speaker!!\n");
        goto out;
    }

    ret = tfa9890_dsp_transfer(priv, TFA9890_DSP_MOD_SPEAKERBOOST,
                   TFA9890_PARAM_SET_CONFIG, tfa9890_config,
                   tfa9890_config_len, TFA9890_DSP_WRITE, 0);
    if (ret)
    {
         lldbg("tfa9890: Failed to load config!!\n");
         goto out;
    }

    ret = tfa9890_dsp_transfer(priv, TFA9890_DSP_MOD_BIQUADFILTERBANK,
                   0, tfa9890_eq, tfa9890_eq_len,
                   TFA9890_DSP_WRITE, 0);
    if (ret)
    {
         lldbg("tfa9890: Failed to load eq!!\n");
         goto out;
    }

    ret = tfa9890_dsp_transfer(priv, TFA9890_DSP_MOD_SPEAKERBOOST,
                 TFA9890_PARAM_SET_PRESET, (const uint8_t *)&music_preset.preset[0],
                   sizeof(struct preset_entry),
                   TFA9890_DSP_WRITE, 0);
    if (ret)
    {
        lldbg("tfa9890: Failed to load preset!!\n");
        goto out;
    }

    /* set all dsp config loaded */
    tfa9890_modify(priv, TFA9890_REG_SYSTEM_CTRL_1, 1, 1, TFA9890_SBSL_OFFSET);


    lldbg("%s() dsp loaded successfully \n", __func__);

    ret = 0;

out:
    return ret;
}

static int tfa9890_read_spkr_imp(struct tfa9890_dev_s *priv)
{
    int imp;
    uint8_t buf[3] = {0, 0, 0};
    int ret;

    ret = tfa9890_dsp_transfer(priv,
            TFA9890_DSP_MOD_SPEAKERBOOST,
            TFA9890_PARAM_GET_RE0, 0, 3,
            TFA9890_DSP_READ, buf);
    if (ret == 0)
    {
        imp = (buf[0] << 16 | buf[1] << 8 | buf[2]);
        imp = imp/(1 << (23 - TFA9890_SPKR_IMP_EXP));
    } else
        imp = 0;

    return imp;
}

static void tfa9890_calibaration(struct tfa9890_dev_s *priv)
{
    uint16_t val;

    /* Ensure no audio playback while calibarating, mute but leave
     * amp enabled
     */
    val = tfa9890_reg_read(priv, TFA9890_REG_VOLUME_CTRL);
    val = val | (TFA9890_STATUS_MUTE);
    tfa9890_reg_write(priv, TFA9890_REG_VOLUME_CTRL, val);

    /* unlock write access MTP memory, no need to relock
     * it will lock after copy from MTP memory is done.
     */
    tfa9890_reg_write(priv, TFA9890_REG_MTP_KEY, TFA9890_MTK_KEY);

    val = tfa9890_reg_read(priv, TFA9890_MTP_REG);
    /* set MTPOTC = 1 & MTPEX = 0 */
    val = val | TFA9890_MTPOTC;
    val = val & (~(TFA9890_STATUS_MTPEX));
    tfa9890_reg_write(priv, TFA9890_MTP_REG, val);

    /* set CIMTB to initiate copy of calib values */
    val = tfa9890_reg_read(priv, TFA9890_REG_I2C_TO_MTP);
    val = val | TFA9890_STATUS_CIMTP;
    tfa9890_reg_write(priv, TFA9890_REG_I2C_TO_MTP, val);
}

static int tfa9890_wait_pll_sync(struct tfa9890_dev_s *priv)
{
    int ret = 0;
    int tries = 0;
    uint16_t val;

    /* check if DSP pll is synced, should not take longer than 10msec */
    do
    {
        val = tfa9890_reg_read(priv, TFA9890_REG_SYSTEM_STATUS);
        if ((val & TFA9890_STATUS_UP_MASK) == TFA9890_STATUS_UP_MASK)
            break;
         usleep(1000);
    } while ((++tries < 10));

    if (tries >= 10)
    {
        lldbg("tfa9890 0X%x:DSP pll sync failed!!\n",  priv->i2c_addr);
        ret = -EIO;
    }

    return ret;
}

int tfa9890_start(FAR struct audio_lowerhalf_s *dev)
{
    int ret = 0;
    uint16_t val;

    struct tfa9890_dev_s *priv= dev->priv;

    lldbg("%s activating 0X%x\n", __func__, priv->i2c_addr);
    tfa9890_powerup(priv, 1);
    tfa9890_wait_pll_sync(priv);
    if (!priv->is_dsp_cfg_done)
    {
       ret = tfa9890_load_config(priv);
       if (ret == 0)
       {
           priv->is_dsp_cfg_done = 1;
           val =  tfa9890_reg_read(priv, TFA9890_MTP_REG);
            /* check if calibaration completed, Calibaration is one time event.
            * Will be done only once when device boots up for the first time.Once
            * calibarated info is stored in non-volatile memory of the device.
            * It will be part of the factory test to validate spkr imp.
            * The MTPEX will be set to 1 always after calibration, on subsequent
            * power down/up as well.
            */
            if (!(val & TFA9890_STATUS_MTPEX))
            {
                lldbg("tfa9890:Calibration not completed initiating seq");
                tfa9890_calibaration(priv);
                val = tfa9890_reg_read(priv, TFA9890_REG_VOLUME_CTRL);
                val = val & ~(TFA9890_STATUS_MUTE);
                tfa9890_reg_write(priv, TFA9890_REG_VOLUME_CTRL, val);
            }
        }
    }
    /* read speaker impedence*/
    priv->speaker_imp = tfa9890_read_spkr_imp(priv);
    lldbg("tfa9890: Calibration imp %d\n ", priv->speaker_imp);
#ifdef DEBUG
    tfa9890_dump_regs(priv);
#endif

    return ret;
}

static int tfa9890_stop(FAR struct audio_lowerhalf_s *dev)
{
    struct tfa9890_dev_s *priv = dev->priv;

    lldbg("%s deactivating %x\n", __func__, priv->i2c_addr);

    tfa9890_powerup(priv, 0);

    return 0;
}

static int tfa9890_configure(FAR struct audio_lowerhalf_s *dev,
                            FAR const struct audio_caps_s *caps)
{
    struct tfa9890_dev_s *priv = dev->priv;

    lldbg("%s() audio feature: %d type: %d value: %d\n",
             __func__, caps->ac_type, caps->ac_format.hw,
             caps->ac_controls.hw[0]);

    if(!priv || !caps)
        return -EINVAL;

    switch (caps->ac_type)
    {
        case AUDIO_TYPE_FEATURE:
            switch (caps->ac_format.hw)
            {
                case AUDIO_FU_VOLUME:
                     /* vol step range 0-255
                      * 0 -0db attenuation, 1 -0.5 attenuation
                      */
                     priv->vol_step = caps->ac_controls.hw[0] & 0xFF;
                     tfa9890_modify(priv, TFA9890_REG_VOLUME_CTRL,
                                     0xff, priv->vol_step,
                                     TFA9890_VOLUME_CTRL_OFFSET);

                    break;
                case AUDIO_FU_EQUALIZER:
                     /* TODO load use case based eq and preset to dsp here
                      * and update current eq profile .
                      */
                     priv->current_eq = caps->ac_controls.hw[0];
                     break;
                case AUDIO_FU_LOUDNESS:
                     /* TODO map system volume to preset/eq profile */
                     break;
                 default:
                     break;
             }
             break;
        default:
            break;
    }

    return 0;
}

static void tfa9890_init_registers(FAR struct tfa9890_dev_s *priv)
{
    /* set up initial register values*/
    tfa9890_reg_write(priv, TFA9890_REG_I2S_CTRL, 0x889b);
    tfa9890_reg_write(priv, TFA9890_REG_BS_CTRL, 0x13A2);
    tfa9890_reg_write(priv, TFA9890_REG_VOLUME_CTRL, 0x000f);
    tfa9890_reg_write(priv, TFA9890_REG_DC_TO_DC_CTRL, 0x8FE6);
    tfa9890_reg_write(priv, TFA9890_REG_SPEAKER_CTRL , 0x3832);
    tfa9890_reg_write(priv, TFA9890_REG_SYSTEM_CTRL_1, 0x827D);
    tfa9890_reg_write(priv, TFA9890_REG_SYSTEM_CTRL_2, 0x38D2);
    tfa9890_reg_write(priv, TFA9890_PWM_CTL_REG, 0x0308);
    tfa9890_reg_write(priv, TFA9890_REG_CURRENT_SENSE, 0x7be1);
    tfa9890_reg_write(priv, TFA9890_CURRT_SNS2_REG, 0x340);
    tfa9890_reg_write(priv, TFA9890_REG_CLIP_SENSE , 0xAD93);
    tfa9890_reg_write(priv, TFA9890_DEM_CTL_REG, 0x00);
}

FAR struct audio_lowerhalf_s *tfa9890_driver_init(FAR struct i2c_dev_s *i2c,
                                                   uint32_t i2c_addr,
                                                   uint32_t frequency)
{
    int ret;
    FAR struct tfa9890_dev_s *priv;

    lldbg("%s()\n", __func__);

    priv = (FAR struct tfa9890_dev_s  *)kmm_zalloc(sizeof(FAR struct tfa9890_dev_s));
    if (!priv)
        return NULL;

    I2C_SETFREQUENCY(i2c, frequency);

    priv->i2c = i2c;
    priv->i2c_addr = i2c_addr;

    priv->dev.ops = &g_audioops;
    priv->dev.priv = (void*)priv;

    /* verify the lowest 7 bits of the ID are 80h */
    ret = tfa9890_reg_read(priv, TFA9890_REG_ID);
    if (ret != 0x80)
    {
        lldbg("tfa9890 0X%x: failed rev check \n", i2c_addr);
        goto err;
    }
    tfa9890_modify(priv, TFA9890_REG_SYSTEM_CTRL_1, 1, 1, TFA9890_RESET_OFFSET);

    tfa9890_init_registers(priv);

#ifdef DEBUG
       tfa9890_dump_regs(priv);
#endif
    return &priv->dev;

err:
    kmm_free(priv);
    return NULL;
}