/*
 *  Driver for the Elgato 4k60 Pro mk.2 HDMI capture card.
 *
 *  Copyright (c) 2021-2022 Steven Toth <stoth@kernellabs.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include "sc0710.h"

int sc0710_dma_channels_resize(struct sc0710_dev *dev)
{
	printk(KERN_ERR "%s()\n", __func__);
	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		sc0710_dma_channel_resize(dev, 0, CHDIR_INPUT, 0x1000, CHTYPE_VIDEO);
		/* Audio uses fixed buffer size, do not resize as it may be active via ALSA */
		/* sc0710_dma_channel_resize(dev, 1, CHDIR_INPUT, 0x1100, CHTYPE_AUDIO); */
		break;
	}

	return 0;
}

int sc0710_dma_channels_alloc(struct sc0710_dev *dev)
{
	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		sc0710_dma_channel_alloc(dev, 0, CHDIR_INPUT, 0x1000, CHTYPE_VIDEO);
		sc0710_dma_channel_alloc(dev, 1, CHDIR_INPUT, 0x1100, CHTYPE_AUDIO);
		break;
	}

	return 0;
}

void sc0710_dma_channels_free(struct sc0710_dev *dev)
{
	int i;

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		sc0710_dma_channel_free(dev, i);
	}
}

void sc0710_dma_channels_stop(struct sc0710_dev *dev)
{
	int i, ret;

	printk("%s()\n", __func__);

	sc_clr(dev, 0, BAR0_00D0, 0x0001);

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		ret = sc0710_dma_channel_stop(&dev->channel[i]);
	}
}

int sc0710_dma_channels_start(struct sc0710_dev *dev)
{
	int i, ret;

	printk("%s()\n", __func__);

	/* Enable LT6911 HDMI receiver output before starting FPGA pipeline.
	 * The FPGA needs video data from the receiver to activate its pipeline.
	 */
	if (dev->board == SC0710_BOARD_ELGATEO_4KP) {
		mutex_lock(&dev->signalMutex);
		sc0710_lt6911_enable_output(dev);
		mutex_unlock(&dev->signalMutex);
	}

	/* Prepare all DMA channels to start */
	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		ret = sc0710_dma_channel_start_prep(&dev->channel[i]);
	}

	/* Set the height register to the incoming signal format height */
	if (dev->fmt) {
		sc_write(dev, 0, BAR0_00C8, dev->fmt->height);
	} else {
		sc_write(dev, 0, BAR0_00C8, 0x438); /* 1080 default */
	}
	sc_write(dev, 0, BAR0_00D8, 0x438);  /* Scaler output height (1080p) */
	sc_write(dev, 0, BAR0_00DC, 0x1050); /* Pipeline control */

	/* Enable FPGA streaming pipeline BEFORE starting DMA.
	 * The FPGA may need to be armed before the DMA engine starts
	 * fetching descriptors, so data is ready when the engine runs.
	 */
	sc_write(dev, 0, 0x100, 0xc8);
	sc_write(dev, 0, 0xec, 0x01);

	sc_write(dev, 0, BAR0_00D0, 0x4100);

	/* Start all DMA channels. */
	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		ret = sc0710_dma_channel_start(&dev->channel[i]);
	}

	sc_set(dev, 0, BAR0_00D0, 0x0001);

	/* Debug: verify register state after start */
	if (sc0710_debug_mode) {
		u32 c8, d0, d8, dc, e4, ec, r100;
		usleep_range(1000, 2000);
		c8   = sc_read(dev, 0, BAR0_00C8);
		d0   = sc_read(dev, 0, BAR0_00D0);
		d8   = sc_read(dev, 0, BAR0_00D8);
		dc   = sc_read(dev, 0, BAR0_00DC);
		e4   = sc_read(dev, 0, 0xe4);
		ec   = sc_read(dev, 0, 0xec);
		r100 = sc_read(dev, 0, 0x100);
		printk(KERN_INFO "%s: POST-START C8=%08x D0=%08x D8=%08x DC=%08x E4=%08x EC=%08x 0x100=%08x\n",
			dev->name, c8, d0, d8, dc, e4, ec, r100);

		for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
			struct sc0710_dma_channel *ch = &dev->channel[i];
			if (!ch->enabled)
				continue;
			printk(KERN_INFO "%s: ch#%d ctrl=%08x s1=%08x s2=%08x desc=%08x sgcred=%08x sg_l=%08x\n",
				ch->dev->name, ch->nr,
				sc_read(dev, 1, ch->reg_dma_control),
				sc_read(dev, 1, ch->reg_dma_status1),
				sc_read(dev, 1, ch->reg_dma_status2),
				sc_read(dev, 1, ch->reg_dma_completed_descriptor_count),
				sc_read(dev, 1, ch->reg_sg_credits),
				sc_read(dev, 1, ch->reg_sg_start_l));
		}

		/* Check if FPGA scaler coefficient table is loaded (BAR0 0x1060+) */
		printk(KERN_INFO "%s: BAR0 scaler check: 0x1060=%08x 0x1064=%08x 0x1070=%08x\n",
			dev->name,
			sc_read(dev, 0, 0x1060),
			sc_read(dev, 0, 0x1064),
			sc_read(dev, 0, 0x1070));

		/* Dump full MCU register map to discover pipeline control registers */
		mutex_lock(&dev->signalMutex);
		sc0710_i2c_dump_mcu_regs(dev);
		mutex_unlock(&dev->signalMutex);
	}

	return 0;
}

/* Called every 2m in polled DMA mode, check
 * each dma channel. If writeback metadata suggests a transfer
 * has completed, process it and hand the audio/video to linux
 * subsystems.
 */
int sc0710_dma_channels_service(struct sc0710_dev *dev)
{
	int i, ret;

	for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
		ret = sc0710_dma_channel_service(&dev->channel[i]);
	}

	return 0;
}
