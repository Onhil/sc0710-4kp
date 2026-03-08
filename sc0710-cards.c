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

#include <linux/firmware.h>
#include <linux/vmalloc.h>
#include "sc0710.h"

struct sc0710_board sc0710_boards[] = {
	[SC0710_BOARD_UNKNOWN] = {
		.name		= "UNKNOWN/GENERIC",
		/* Ensure safe default for unknown boards */
	},
	[SC0710_BOARD_ELGATEO_4KP60_MK2] = {
		.name		= "Elgato 4k60 Pro mk.2",
	},
	[SC0710_BOARD_ELGATEO_4KP] = {
		.name		= "Elgato 4K Pro",
	},
};
const unsigned int sc0710_bcount = ARRAY_SIZE(sc0710_boards);

struct sc0710_subid sc0710_subids[] = {
	{
		.subvendor = 0x1cfa,
		.subdevice = 0x000e,
		.card      = SC0710_BOARD_ELGATEO_4KP60_MK2,
	}, {
		.subvendor = 0x1cfa,
		.subdevice = 0x0012,
		.card      = SC0710_BOARD_ELGATEO_4KP,
	}
};
const unsigned int sc0710_idcount = ARRAY_SIZE(sc0710_subids);

void sc0710_card_list(struct sc0710_dev *dev)
{
	int i;

	if (0 == dev->pci->subsystem_vendor &&
	    0 == dev->pci->subsystem_device) {
		printk(KERN_INFO
			"%s: Board has no valid PCIe Subsystem ID and can't\n"
		       "%s: be autodetected. Pass card=<n> insmod option\n"
		       "%s: to workaround that. Redirect complaints to the\n"
		       "%s: vendor of the TV card.  Best regards,\n"
		       "%s:         -- tux\n",
		       dev->name, dev->name, dev->name, dev->name, dev->name);
	} else {
		printk(KERN_INFO
			"%s: Your board isn't known (yet) to the driver.\n"
		       "%s: Try to pick one of the existing card configs via\n"
		       "%s: card=<n> insmod option.  Updating to the latest\n"
		       "%s: version might help as well.\n",
		       dev->name, dev->name, dev->name, dev->name);
	}
	printk(KERN_INFO "%s: Here is a list of valid choices for the card=<n> insmod option:\n",
	       dev->name);
	for (i = 0; i < sc0710_bcount; i++)
		printk(KERN_INFO "%s:    card=%d -> %s\n",
		       dev->name, i, sc0710_boards[i].name);
}

void sc0710_gpio_setup(struct sc0710_dev *dev)
{
	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		break;
	}
}

/* --- Lattice ECP5 firmware programming via AXI SPI at BAR0+0x2000 --- */

#define SPI_BASE   0x2000
#define SPI_SOFTR  (SPI_BASE + 0x40)
#define SPI_CR     (SPI_BASE + 0x60)
#define SPI_SR     (SPI_BASE + 0x64)
#define SPI_DTR    (SPI_BASE + 0x68)
#define SPI_DRR    (SPI_BASE + 0x6C)
#define SPI_SSR    (SPI_BASE + 0x70)

/* ECP5 ISC commands */
#define ECP5_READ_ID           0xE0
#define ECP5_ISC_ENABLE        0xC6
#define ECP5_ISC_ERASE         0x0E
#define ECP5_ISC_DISABLE       0x26
#define ECP5_LSC_CHECK_BUSY    0xF0
#define ECP5_LSC_INIT_ADDRESS  0x46
#define ECP5_LSC_BITSTREAM_BURST 0x7A
#define ECP5_LSC_READ_STATUS   0x3C
#define ECP5_LSC_REFRESH       0x79
#define ECP5_USERCODE          0xC0

/* Firmware file constants */
#define FWI_HEADER_SIZE  16
#define FWI_MAGIC_0      0x00
#define FWI_MAGIC_1      0x11
#define FWI_XOR_FIRST    0x5A
#define FWI_XOR_SECOND   0xA5

static void ecp5_spi_reset(struct sc0710_dev *dev)
{
	sc_write(dev, 0, SPI_SOFTR, 0x0A);
	udelay(100);
	/* CR: Master, SPE, Manual_SS, TX/RX FIFO reset */
	sc_write(dev, 0, SPI_CR, 0x1E6);
	udelay(10);
	/* Clear FIFO resets, keep master inhibit */
	sc_write(dev, 0, SPI_CR, 0x186);
}

/* Send bytes via SPI and optionally read response.
 * tx/tx_len: bytes to send (command + dummy for readback)
 * rx/rx_len: bytes to read from response (taken from end of transfer)
 */
static int ecp5_spi_xfer(struct sc0710_dev *dev, const u8 *tx, int tx_len,
			  u8 *rx, int rx_len)
{
	int i, poll;
	int total = tx_len;

	/* Assert slave select */
	sc_write(dev, 0, SPI_SSR, 0xFFFFFFFE);

	/* Fill TX FIFO */
	for (i = 0; i < total; i++)
		sc_write(dev, 0, SPI_DTR, tx[i]);

	/* Clear master inhibit to start */
	sc_write(dev, 0, SPI_CR, 0x86);

	/* Poll for TX empty (bit 2) */
	for (poll = 0; poll < 10000; poll++) {
		if (sc_read(dev, 0, SPI_SR) & 0x04)
			break;
		udelay(10);
	}
	udelay(100);

	/* Re-assert master inhibit */
	sc_write(dev, 0, SPI_CR, 0x186);
	/* Deassert slave select */
	sc_write(dev, 0, SPI_SSR, 0xFFFFFFFF);

	/* Read all RX bytes (full-duplex: same count as TX) */
	if (rx && rx_len > 0) {
		/* Discard leading bytes, keep last rx_len */
		int skip = total - rx_len;
		for (i = 0; i < total; i++) {
			u8 b = sc_read(dev, 0, SPI_DRR) & 0xFF;
			if (i >= skip)
				rx[i - skip] = b;
		}
	} else {
		/* Drain RX FIFO */
		for (i = 0; i < total; i++)
			sc_read(dev, 0, SPI_DRR);
	}

	return 0;
}

/* Send BITSTREAM_BURST command + data in ONE CS cycle, matching Windows.
 * Protocol per byte: write DTR → poll SPISR (TX empty) → read DRR (drain RX).
 */
static void ecp5_spi_burst_write(struct sc0710_dev *dev, const u8 *data, u32 len)
{
	u32 i;

	/* Reset FIFOs, assert CS, enable master */
	sc_write(dev, 0, SPI_CR, 0x1E6);
	sc_write(dev, 0, SPI_SSR, 0xFFFFFFFE);
	sc_write(dev, 0, SPI_CR, 0x86);

	/* Send command: 7A 00 00 00 */
	sc_write(dev, 0, SPI_DTR, ECP5_LSC_BITSTREAM_BURST);
	while (!(sc_read(dev, 0, SPI_SR) & 0x04))
		;
	sc_read(dev, 0, SPI_DRR);

	sc_write(dev, 0, SPI_DTR, 0x00);
	while (!(sc_read(dev, 0, SPI_SR) & 0x04))
		;
	sc_read(dev, 0, SPI_DRR);

	sc_write(dev, 0, SPI_DTR, 0x00);
	while (!(sc_read(dev, 0, SPI_SR) & 0x04))
		;
	sc_read(dev, 0, SPI_DRR);

	sc_write(dev, 0, SPI_DTR, 0x00);
	while (!(sc_read(dev, 0, SPI_SR) & 0x04))
		;
	sc_read(dev, 0, SPI_DRR);

	/* Send data bytes */
	for (i = 0; i < len; i++) {
		sc_write(dev, 0, SPI_DTR, data[i]);
		while (!(sc_read(dev, 0, SPI_SR) & 0x04))
			;
		sc_read(dev, 0, SPI_DRR);

		if ((i & 0x7FFF) == 0 && i > 0)
			printk(KERN_INFO "%s: ECP5 programming %u/%u bytes\n",
				dev->name, i, len);
	}

	/* Inhibit master, deassert CS */
	sc_write(dev, 0, SPI_CR, 0x186);
	sc_write(dev, 0, SPI_SSR, 0xFFFFFFFF);
}

static u32 ecp5_read_idcode(struct sc0710_dev *dev)
{
	u8 tx[8] = { ECP5_READ_ID, 0, 0, 0, 0, 0, 0, 0 };
	u8 rx[4] = { 0 };

	ecp5_spi_reset(dev);
	ecp5_spi_xfer(dev, tx, 8, rx, 4);
	return (rx[0] << 24) | (rx[1] << 16) | (rx[2] << 8) | rx[3];
}

static int ecp5_check_busy(struct sc0710_dev *dev, int timeout_ms)
{
	/* ECP5 SPI: 1 command byte + 3 padding bytes; busy if response byte != 0 */
	u8 tx[4] = { ECP5_LSC_CHECK_BUSY, 0, 0, 0 };
	u8 rx[1];
	int i;

	for (i = 0; i < timeout_ms; i++) {
		ecp5_spi_xfer(dev, tx, 4, rx, 1);
		if (!rx[0])
			return 0;
		msleep(1);
	}
	return -ETIMEDOUT;
}

static int ecp5_read_status(struct sc0710_dev *dev, u32 *status)
{
	u8 tx[8] = { ECP5_LSC_READ_STATUS, 0, 0, 0, 0, 0, 0, 0 };
	u8 rx[4];

	ecp5_spi_xfer(dev, tx, 8, rx, 4);
	*status = (rx[0] << 24) | (rx[1] << 16) | (rx[2] << 8) | rx[3];
	return 0;
}

static u32 ecp5_read_usercode(struct sc0710_dev *dev)
{
	u8 tx[8] = { ECP5_USERCODE, 0, 0, 0, 0, 0, 0, 0 };
	u8 rx[4] = { 0 };

	ecp5_spi_xfer(dev, tx, 8, rx, 4);
	return (rx[0] << 24) | (rx[1] << 16) | (rx[2] << 8) | rx[3];
}

/* Program the Lattice ECP5 with a raw bitstream via ISC commands */
static int ecp5_program_bitstream(struct sc0710_dev *dev, const u8 *data, u32 len)
{
	u8 cmd[4];
	u32 status;
	int ret;

	printk(KERN_INFO "%s: Programming ECP5 (%u bytes)...\n", dev->name, len);

	/* SPI reset at start of programming sequence */
	ecp5_spi_reset(dev);

	/* Check if ECP5 is responsive. If SPI returns all FFs (e.g. after
	 * a failed programming attempt), send REFRESH to reset the device
	 * back to factory firmware so it responds to ISC commands.
	 * Windows doesn't need this because it always starts from clean state.
	 */
	{
		u32 id = ecp5_read_idcode(dev);
		if (id == 0xFFFFFFFF) {
			printk(KERN_INFO "%s: ECP5 SPI unresponsive, sending REFRESH\n",
				dev->name);
			cmd[0] = ECP5_LSC_REFRESH;
			cmd[1] = 0x00;
			cmd[2] = 0x00;
			cmd[3] = 0x00;
			ecp5_spi_xfer(dev, cmd, 4, NULL, 0);
			msleep(200);
			ecp5_spi_reset(dev);
		}
	}

	/* 1. ISC_ENABLE — operand 0x00 for normal SRAM programming.
	 * Note: 0x08 is "transparent" mode (MachXO2), NOT correct for ECP5.
	 */
	cmd[0] = ECP5_ISC_ENABLE;
	cmd[1] = 0x00;
	cmd[2] = 0x00;
	cmd[3] = 0x00;
	ecp5_spi_xfer(dev, cmd, 4, NULL, 0);
	msleep(1);

	ret = ecp5_check_busy(dev, 100);
	if (ret) {
		ecp5_read_status(dev, &status);
		printk(KERN_ERR "%s: ECP5 busy after ISC_ENABLE (status: %08x)\n",
			dev->name, status);
		return ret;
	}

	ecp5_read_status(dev, &status);
	printk(KERN_INFO "%s: ECP5 status after ISC_ENABLE: %08x\n", dev->name, status);

	/* 2. ISC_ERASE — erase SRAM configuration */
	cmd[0] = ECP5_ISC_ERASE;
	cmd[1] = 0x01; /* erase SRAM */
	cmd[2] = 0x00;
	cmd[3] = 0x00;
	ecp5_spi_xfer(dev, cmd, 4, NULL, 0);

	/* Erase can take up to 5 seconds */
	ret = ecp5_check_busy(dev, 5000);
	if (ret) {
		ecp5_read_status(dev, &status);
		printk(KERN_ERR "%s: ECP5 busy after ISC_ERASE (status: %08x)\n",
			dev->name, status);
		return ret;
	}

	ecp5_read_status(dev, &status);
	printk(KERN_INFO "%s: ECP5 status after erase: %08x\n", dev->name, status);

	/* 3. LSC_INIT_ADDRESS — reset frame address */
	cmd[0] = ECP5_LSC_INIT_ADDRESS;
	cmd[1] = 0x01;
	cmd[2] = 0x00;
	cmd[3] = 0x00;
	ecp5_spi_xfer(dev, cmd, 4, NULL, 0);
	msleep(1);

	/* 4. LSC_BITSTREAM_BURST — command + all data in ONE CS cycle.
	 * Windows sends everything in a single CS assertion: command (4 bytes)
	 * + data (356,448 bytes), byte-by-byte with SPISR polling.
	 */
	ecp5_spi_burst_write(dev, data, len);

	/* Match Windows post-burst sequence exactly:
	 * ISC_DISABLE → NOP → READ_STATUS (no delays between)
	 */
	cmd[0] = ECP5_ISC_DISABLE;
	cmd[1] = 0x00;
	cmd[2] = 0x00;
	cmd[3] = 0x00;
	ecp5_spi_xfer(dev, cmd, 4, NULL, 0);

	/* NOP (0xFF) — Windows sends this between ISC_DISABLE and status read */
	cmd[0] = 0xFF;
	cmd[1] = 0xFF;
	cmd[2] = 0xFF;
	cmd[3] = 0xFF;
	ecp5_spi_xfer(dev, cmd, 4, NULL, 0);

	ecp5_read_status(dev, &status);
	printk(KERN_INFO "%s: ECP5 status after programming: %08x (DONE=%d)\n",
		dev->name, status, (status >> 8) & 1);

	if (!(status & 0x100) && status != 0xFFFFFFFF) {
		printk(KERN_ERR "%s: ECP5 DONE not set after programming\n", dev->name);
		return -EIO;
	}

	return 0;
}

/* Load and program ECP5 firmware if outdated */
static int sc0710_ecp5_firmware_check(struct sc0710_dev *dev)
{
	const struct firmware *fw;
	u32 idcode, half_size, status, usercode;
	u8 *decoded;
	int ret, i;

	/* Probe: try JEDEC SPI flash ID (0x9F) to check if SPI is
	 * connected to a flash chip or the ECP5 SSPI port.
	 */
	{
		u8 jtx[4] = { 0x9F, 0x00, 0x00, 0x00 };
		u8 jrx[3] = { 0 };
		ecp5_spi_reset(dev);
		ecp5_spi_xfer(dev, jtx, 4, jrx, 3);
		printk(KERN_INFO "%s: SPI JEDEC probe: %02x %02x %02x\n",
			dev->name, jrx[0], jrx[1], jrx[2]);
	}

	idcode = ecp5_read_idcode(dev);
	ecp5_read_status(dev, &status);
	usercode = ecp5_read_usercode(dev);
	printk(KERN_INFO "%s: ECP5 IDCODE: %08x, status: %08x (DONE=%d), USERCODE: %08x\n",
		dev->name, idcode, status, (status >> 8) & 1, usercode);

	/* If DONE=1, the ECP5 is already configured (e.g. warm reboot from Windows).
	 * TODO: restore skip once cold boot programming verified.
	 */
	if (status & 0x100)
		printk(KERN_INFO "%s: ECP5 DONE=1, re-programming for testing\n",
			dev->name);

	printk(KERN_INFO "%s: Programming ECP5 — uploading SC0710.FWI.HEX\n",
		dev->name);

	ret = request_firmware(&fw, "sc0710/SC0710.FWI.HEX", &dev->pci->dev);
	if (ret) {
		printk(KERN_ERR "%s: Failed to load firmware sc0710/SC0710.FWI.HEX: %d\n",
			dev->name, ret);
		printk(KERN_ERR "%s: Place SC0710.FWI.HEX in /lib/firmware/sc0710/\n",
			dev->name);
		return ret;
	}

	/* Validate header */
	if (fw->size < FWI_HEADER_SIZE + 2 ||
	    fw->data[0] != FWI_MAGIC_0 || fw->data[1] != FWI_MAGIC_1) {
		printk(KERN_ERR "%s: Invalid firmware file header\n", dev->name);
		release_firmware(fw);
		return -EINVAL;
	}

	half_size = (fw->size - FWI_HEADER_SIZE) / 2;

	/* FWI format: 16-byte header + two halves with swapped order.
	 * Full .bit file = (second half XOR 0xA5) + (first half XOR 0x5A).
	 * Windows sends all 356,448 bytes including text header.
	 */
	decoded = vmalloc(half_size * 2);
	if (!decoded) {
		release_firmware(fw);
		return -ENOMEM;
	}

	/* First part of bitstream: FWI second half XOR 0xA5 */
	for (i = 0; i < half_size; i++)
		decoded[i] = fw->data[FWI_HEADER_SIZE + half_size + i] ^ FWI_XOR_SECOND;

	/* Second part of bitstream: FWI first half XOR 0x5A */
	for (i = 0; i < half_size; i++)
		decoded[half_size + i] = fw->data[FWI_HEADER_SIZE + i] ^ FWI_XOR_FIRST;

	release_firmware(fw);

	printk(KERN_INFO "%s: Bitstream: %u bytes, starts: %*ph\n",
		dev->name, half_size * 2, 16, decoded);

	ret = ecp5_program_bitstream(dev, decoded, half_size * 2);
	vfree(decoded);

	if (ret == 0) {
		u32 a8;
		int tries;

		printk(KERN_INFO "%s: ECP5 firmware upload complete, waiting for pipeline\n",
			dev->name);

		/* Wait for A8 to become non-zero — indicates the ECP5 design
		 * is running and the LT6911 TX→FPGA path is active.
		 * This can take several hundred ms after programming.
		 */
		for (tries = 0; tries < 50; tries++) {
			msleep(100);
			a8 = sc_read(dev, 0, 0x00A8);
			if (a8) {
				printk(KERN_INFO "%s: ECP5 pipeline active, A8=%08x (after %dms)\n",
					dev->name, a8, (tries + 1) * 100);
				break;
			}
		}
		if (!a8)
			printk(KERN_WARNING "%s: ECP5 pipeline not active after 5s (A8=0)\n",
				dev->name);
	}

	return ret;
}

void sc0710_card_setup(struct sc0710_dev *dev)
{
	switch (dev->board) {
	case SC0710_BOARD_ELGATEO_4KP60_MK2:
	case SC0710_BOARD_ELGATEO_4KP:
		/* Check and update Lattice ECP5 companion FPGA firmware.
		 * The 4KP has a Lattice ECP5 connected via AXI SPI at BAR0+0x2000.
		 * Factory firmware (IDCODE 0x41112043) doesn't enable LT6911 TX.
		 * The Windows driver uploads updated firmware on every boot.
		 */
		if (dev->board == SC0710_BOARD_ELGATEO_4KP)
			sc0710_ecp5_firmware_check(dev);

		/* Read MCU state BEFORE any I2C init — detect factory vs
		 * corrupted state from default module's mk2 commands.
		 * Minimal IIC bootstrap: soft reset + timing on instance 1 only.
		 * Uses raw AXI IIC register access (no exported read function).
		 */
		if (dev->board == SC0710_BOARD_ELGATEO_4KP) {
			u8 rbuf[16];
			u32 v;
			int i, cnt;

			/* Bootstrap IIC instance 1 (base 0x3200) */
			sc_write(dev, 0, 0x3240, 0x0000000a); /* SOFTR */
			udelay(10);
			sc_write(dev, 0, 0x3328, 0x0000002d);
			sc_write(dev, 0, 0x332c, 0x0000002d);
			sc_write(dev, 0, 0x3330, 0x0000002d);
			sc_write(dev, 0, 0x3334, 0x00000014);
			sc_write(dev, 0, 0x3338, 0x00000050);
			sc_write(dev, 0, 0x333c, 0x00000076);
			sc_write(dev, 0, 0x3340, 0x00000076);
			sc_write(dev, 0, 0x3344, 0x00000001);

			/* Read MCU 0x64 sub-address 0x00 and 0x10 */
			{
				u8 subaddrs[2] = { 0x00, 0x10 };
				int s;
				for (s = 0; s < 2; s++) {
					/* TX reset + enable */
					sc_write(dev, 0, 0x3300, 0x02);
					sc_write(dev, 0, 0x3300, 0x01);
					/* START + write addr */
					sc_write(dev, 0, 0x3308, (1 << 8) | 0x64);
					udelay(200);
					/* Sub-address */
					sc_write(dev, 0, 0x3308, subaddrs[s]);
					udelay(200);
					/* Clear ISR */
					sc_write(dev, 0, 0x3320, 0x0f);
					/* TX reset */
					sc_write(dev, 0, 0x3300, 0x02);
					sc_write(dev, 0, 0x3300, 0x00);
					/* START + read addr */
					sc_write(dev, 0, 0x3308, (1 << 8) | 0x65);
					/* STOP + byte count */
					sc_write(dev, 0, 0x3308, (1 << 9) | 16);
					/* Enable */
					sc_write(dev, 0, 0x3300, 0x01);
					/* Read 16 bytes */
					for (i = 0; i < 16; i++) {
						cnt = 100;
						while (cnt-- > 0) {
							v = sc_read(dev, 0, 0x3304);
							if (!(v & 0x40)) /* RX not empty */
								break;
							udelay(100);
						}
						rbuf[i] = sc_read(dev, 0, 0x330C) & 0xFF;
					}
					printk(KERN_INFO "%s: MCU FACTORY [%02x]: %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x\n",
						dev->name, subaddrs[s],
						rbuf[0], rbuf[1], rbuf[2], rbuf[3],
						rbuf[4], rbuf[5], rbuf[6], rbuf[7],
						rbuf[8], rbuf[9], rbuf[10], rbuf[11],
						rbuf[12], rbuf[13], rbuf[14], rbuf[15]);
				}
			}
		}

		sc_write(dev, 0, BAR0_00C4, 0x000f0000);

		/* Soft reset and configure all 8 AXI IIC instances (0x3000-0x3E00).
		 * Windows driver initializes all 8 identically. Each instance is
		 * at a 0x200 offset: SOFTR at base+0x040, timing at base+0x128..0x144.
		 * Without the soft reset, the 4KP's I2C controller starts wedged.
		 */
		{
			int iic;
			for (iic = 0; iic < 8; iic++) {
				u32 base = 0x3000 + (iic * 0x200);
				sc_write(dev, 0, base + 0x040, 0x0000000a); /* SOFTR */
			}
			udelay(10);
			for (iic = 0; iic < 8; iic++) {
				u32 base = 0x3000 + (iic * 0x200);
				sc_write(dev, 0, base + 0x128, 0x0000002d); /* TSUSTA */
				sc_write(dev, 0, base + 0x12c, 0x0000002d); /* TSUSTO */
				sc_write(dev, 0, base + 0x130, 0x0000002d); /* THDSTA */
				sc_write(dev, 0, base + 0x134, 0x00000014); /* TSUDAT */
				sc_write(dev, 0, base + 0x138, 0x00000050); /* TBUF */
				sc_write(dev, 0, base + 0x13c, 0x00000076); /* THIGH */
				sc_write(dev, 0, base + 0x140, 0x00000076); /* TLOW */
				sc_write(dev, 0, base + 0x144, 0x00000001); /* THDDAT */
			}
		}

		sc_write(dev, 1, BAR1_0094, 0x00fffe3e);
		sc_write(dev, 1, BAR1_0008, 0x00fffe3e);
		sc_write(dev, 1, BAR1_0194, 0x00fffe3e);
		sc_write(dev, 1, BAR1_0108, 0x00fffe3e);
		sc_write(dev, 1, BAR1_1094, 0x00fffe7e);
		sc_write(dev, 1, BAR1_1008, 0x00fffe7e);
		sc_write(dev, 1, BAR1_1194, 0x00fffe7e);
		sc_write(dev, 1, BAR1_1108, 0x00fffe7e);
		sc_write(dev, 1, BAR1_2080, 0);
		sc_write(dev, 1, BAR1_2084, 0);
		sc_write(dev, 1, BAR1_2088, 0);
		sc_write(dev, 1, BAR1_208C, 0);
		sc_write(dev, 1, BAR1_20A0, 0);
		sc_write(dev, 1, BAR1_20A4, 0);

		/* 4KP: Configure FPGA pipeline early.
		 * Windows driver sets EC bits 0x10 (no signal) or 0x20 (active signal),
		 * NOT bit 0x01 as we previously assumed.
		 * Test: use Windows-style EC values.
		 */
		if (dev->board == SC0710_BOARD_ELGATEO_4KP) {
			u32 ec_before = sc_read(dev, 0, 0xEC);

			sc_write(dev, 0, BAR0_00C8, 0x0870);  /* input height: 2160 */
			sc_write(dev, 0, BAR0_00D8, 0x0438);  /* scaler output: 1080 */
			sc_write(dev, 0, BAR0_00D0, 0x4100);  /* pipeline config */
			sc_write(dev, 0, 0xCC, 0x00000000);
			sc_write(dev, 0, BAR0_00D0, 0x4300);  /* reset */
			sc_write(dev, 0, BAR0_00D0, 0x4100);
			sc_set(dev, 0, BAR0_00D0, 0x0001);    /* pipeline enable */

			/* Try Windows-style EC: bit 0x10 first (no-signal state),
			 * then bit 0x20 (active signal state).
			 */
			sc_write(dev, 0, 0xEC, 0x00000010);
			printk(KERN_INFO "%s: Early pipeline: EC before=%08x, now D0=%08x EC=%08x A8=%08x\n",
				dev->name, ec_before,
				sc_read(dev, 0, BAR0_00D0),
				sc_read(dev, 0, 0xEC),
				sc_read(dev, 0, 0xa8));
		}
		break;
	}
}
