/*
 * Copyright (C) 2021 Gaël PORTAY <gael.portay@collabora.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>

#include <gusb.h>
#include <string.h>

#include "fu-genesys-common.h"
#include "fu-genesys-scaler.h"

#define GENESYS_SCALER_MSTAR_READ  0x7a
#define GENESYS_SCALER_MSTAR_WRITE 0x7b

#define GENESYS_SCALER_CMD_DATA_WRITE 0x10
#define GENESYS_SCALER_CMD_DATA_READ  0x11
#define GENESYS_SCALER_CMD_DATA_END   0x12

#define GENESYS_SCALER_FLASH_CONTROL_WRITE_ENABLE  0x06
#define GENESYS_SCALER_FLASH_CONTROL_WRITE_DISABLE 0x04
#define GENESYS_SCALER_FLASH_CONTROL_READ_STATUS   0x05
#define GENESYS_SCALER_FLASH_CONTROL_WRITE_STATUS  0x01
#define GENESYS_SCALER_FLASH_CONTROL_READ          0x03
#define GENESYS_SCALER_FLASH_CONTROL_FAST_READ     0x0b
#define GENESYS_SCALER_FLASH_CONTROL_PAGE_PROGRAM  0x02
#define GENESYS_SCALER_FLASH_CONTROL_CHIP_ERASE    0x60
#define GENESYS_SCALER_FLASH_CONTROL_SECTOR_ERASE  0x20
#define GENESYS_SCALER_FLASH_CONTROL_READ_ID       0x9f

#define GENESYS_SCALER_INFO 0xA4

#define GENESYS_SCALER_USB_TIMEOUT 5000 /* ms */

#define MTK_RSA_HEADER	"MTK_RSA_HEADER"

typedef struct __attribute__((packed)) {
	guint8 default_head[14];
	guint8 reserved_0e_0f[2];
	guint8 model_name[16];
	guint8 reserved_20;
	guint8 size[2];
	guint8 reserved_23_27[5];
	guint8 scaler_group[10];
	guint8 reserved_32_53[34];
	guint8 panel_type[10];
	guint8 scaler_packet_date[8];
	guint8 reserved_66_67[2];
	guint8 scaler_packet_version[4];
	guint8 reserved_6c_7f[20];
	union {
		guint8 r8;
		struct {
			guint8 decrypt_mode : 1;
			guint8 second_image : 1;
			guint8 dual_image_turn : 1;
			guint8 special_protect_sector : 1;
			guint8 hawk_bypass_mode : 1;
			guint8 boot_code_size_in_header : 1;
			guint8 reserved_6_7 : 2;
		} __attribute__((packed)) bits;
	} configuration_setting;
	guint8 reserved_81_85[5];
	guint8 second_image_program_addr[4];
	guint8 scaler_public_key_addr[4];
	union {
		guint32 r32;
		struct {
			guint8 addr_low[2];
			guint8 addr_high : 4;
			guint8 size : 4;
		} __attribute__((packed)) area;
	} protect_sector[2];
	guint32 boot_code_size;
} MTKRSAHeader;

typedef union __attribute__((packed)) {
	guint8 raw[0x312];
	struct {
		struct __attribute__((packed)) {
			guint8 N[0x206];
			guint8 E[0x00c];
		} public_key;
		MTKRSAHeader header;
	} data;
} MTKFooter;

/* [TODO]: Move to fu-genesys-common.h */
typedef struct {
	guint8 reg;
	guint8 expected_val;
} WaitFlashRegisterHelper;

typedef enum {
	MCPU_NONE,
	MCPU_TSUM_V,
	MCPU_TSUM_C,
	MCPU_TSUM_D,
	MCPU_TSUM_9,
	MCPU_TSUM_F,
	MCPU_TSUM_K,
	MCPU_TSUM_G,
	MCPU_TSUM_U,
	MSB6010,
	MCPU_TSUM_CD,
	MCPU_TSUM_G_MSB6010,
	MCPU_TSUM_2,
	MCPU_TSUM_B,
	MCPU_TSUM_O,
	MCPU_MST9U = 51,
	MCPU_MST9U2,
	MCPU_MST9U3,
	MCPU_MST9U4,
} MStarChipID;

struct _FuGenesysScaler {
	FuDevice parent_instance;
	MStarChipID cpu_model;
	guint8 level;
	guint8 public_key[0x212];
	guint8 flash_id[3];
	gboolean enable_security_wp;
};

G_DEFINE_TYPE(FuGenesysScaler, fu_genesys_scaler, FU_TYPE_DEVICE)

static gboolean
fu_genesys_scaler_enter_serial_debug_mode(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x53, 0x45, 0x52, 0x44, 0x42 };

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0001,		/* value */
					   0x0000,		/* idx */
					   data,		/* data */
					   sizeof(data),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error entering Serial Debug Mode: ");
		return FALSE;
	}

	g_usleep(1000); /* 1 ms */

	return TRUE;
}

static gboolean
fu_genesys_scaler_exit_serial_debug_mode(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x45 };

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0001,		/* value */
					   0x0000,		/* idx */
					   data,		/* data */
					   sizeof(data),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error exiting Serial Debug Mode: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_enter_single_step_mode(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data1[] = { 0x10, 0xc0, 0xc1, 0x53 };
	guint8 data2[] = { 0x10, 0x1f, 0xc1, 0x53 };

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0001,		/* value */
					   0x0000,		/* idx */
					   data1,		/* data */
					   sizeof(data1),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error entering Single Step Mode: ");
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0001,		/* value */
					   0x0000,		/* idx */
					   data2,		/* data */
					   sizeof(data2),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error entering Single Step Mode: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_exit_single_step_mode(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x10, 0xc0, 0xc1, 0xff };

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0001,		/* value */
					   0x0000,		/* idx */
					   data,		/* data */
					   sizeof(data),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error exiting Single Step Mode: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_enter_debug_mode(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x10, 0x00, 0x00, 0x00 };

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0001,		/* value */
					   0x0000,		/* idx */
					   data,		/* data */
					   sizeof(data),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error entering Debug Mode: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_mst_i2c_bus_ctrl(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x35, 0x71 };

	for (guint i = 0; i < sizeof(data); i++) {
		if (!g_usb_device_control_transfer(usb_device,
						   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
						   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
						   G_USB_DEVICE_RECIPIENT_DEVICE,
						   GENESYS_SCALER_MSTAR_WRITE,
						   0x0001,		/* value */
						   0x0000,		/* idx */
						   &data[i],		/* data */
						   sizeof(data[i]),	/* data length */
						   NULL,		/* actual length */
						   GENESYS_SCALER_USB_TIMEOUT,
						   NULL,
						   error)) {
			g_prefix_error(error, "error sending i2c bus ctrl %02x: ", data[i]);
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_mst_i2c_bus_switch_to_ch0(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x80, 0x82, 0x84, 0x51, 0x7f, 0x37, 0x61 };

	for (guint i = 0; i < sizeof(data); i++) {
		if (!g_usb_device_control_transfer(usb_device,
						   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
						   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
						   G_USB_DEVICE_RECIPIENT_DEVICE,
						   GENESYS_SCALER_MSTAR_WRITE,
						   0x0001,		/* value */
						   0x0000,		/* idx */
						   &data[i],		/* data */
						   sizeof(data[i]),	/* data length */
						   NULL,		/* actual length */
						   GENESYS_SCALER_USB_TIMEOUT,
						   NULL,
						   error)) {
			g_prefix_error(error, "error sending i2c bus ch0 %02x: ", data[i]);
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_mst_i2c_bus_switch_to_ch4(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x80, 0x82, 0x85, 0x53, 0x7f };

	for (guint i = 0; i < sizeof(data); i++) {
		if (!g_usb_device_control_transfer(usb_device,
						   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
						   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
						   G_USB_DEVICE_RECIPIENT_DEVICE,
						   GENESYS_SCALER_MSTAR_WRITE,
						   0x0001,		/* value */
						   0x0000,		/* idx */
						   &data[i],		/* data */
						   sizeof(data[i]),	/* data length */
						   NULL,		/* actual length */
						   GENESYS_SCALER_USB_TIMEOUT,
						   NULL,
						   error)) {
			g_prefix_error(error, "error sending i2c bus ch4 %02x: ", data[i]);
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_disable_wp(FuGenesysScaler *self, gboolean disable, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data_out[] = { 0x10, 0x00 /* gpio_out_h */, 0x00 /* gpio_out_l */, 0x00 /* gpio_out_val */ };
	guint8 data_en[] = { 0x10, 0x00 /* gpio_en_h */, 0x00 /* gpio_en_l */, 0x00 /* gpio_en_val */ };

	/*
	 * ------------------------------------------------------------------------------------------------
	 * Chip		GPIO Out	GPIO Enable	GPIO Number		GPIO Number for Security WP
	 * ------------------------------------------------------------------------------------------------
	 * MST9U	0x0426[0] = 1	0x0428[0] = 0	GPIO 10
	 * -----
	 * TSUM_CD	0x0226[0] = 1	0x0228[0] = 0	GPIO 10			GPIO3: OUT: Reg 0x0220[3] = 1
	 * -----								OEN:        Reg 0x0222[3] = 0
	 * TSUM_2	0x0202[0] = 1	0x0203[0] = 0	GPIO 10
	 * -----
	 * TSUM_V	0x1B26[1:2] = 1	0x1B28[1:2] = 0	GPIO 11 / GPIO 12
	 * -----
	 * TSUM_B	0x1B26[0] = 1	0x1B28[0] = 0	GPIO 10
	 * -----
	 * TSUM_U	0x0200[7] = 1	0x0201[7] = 0	GPIO 07
	 * -----
	 * TSUM_G 	0x0434[4] = 1	0x0436[4] = 0	GPIO 04
	 * ------------------------------------------------------------------------------------------------
	 */

	if (self->cpu_model == MCPU_TSUM_CD) {
		if (self->enable_security_wp) {
			data_out[1] = 0x02;
			data_out[2] = 0x20;
			data_en[1] = 0x02;
			data_en[2] = 0x22;
		} else {
			data_out[1] = 0x02;
			data_out[2] = 0x26;
			data_en[1] = 0x02;
			data_en[2] = 0x28;
		}
	} else if (self->cpu_model == MCPU_TSUM_U) {
		data_out[1] = 0x02;
		data_out[2] = 0x00;
		data_en[1] = 0x02;
		data_en[2] = 0x01;
	} else if ((self->cpu_model == MCPU_TSUM_V) ||
		   (self->cpu_model == MCPU_TSUM_B)) {
		data_out[1] = 0x1B;
		data_out[2] = 0x26;
		data_en[1] = 0x1B;
		data_en[2] = 0x28;
	} else if (self->cpu_model == MCPU_TSUM_G_MSB6010) {
		data_out[1] = 0x04;
		data_out[2] = 0x34;
		data_en[1] = 0x04;
		data_en[2] = 0x36;
	} else if (self->cpu_model == MSB6010) {
		data_out[1] = 0x04;
		data_out[2] = 0x41;
		data_en[1] = 0x04;
		data_en[2] = 0x45;
	} else if (self->cpu_model == MCPU_TSUM_C) {
		data_out[1] = 0x02;
		data_out[2] = 0x26;
		data_en[1] = 0x02;
		data_en[2] = 0x28;
	} else if (self->cpu_model == MCPU_TSUM_2) {
		data_out[1] = 0x02;
		data_out[2] = 0x02;
		data_en[1] = 0x02;
		data_en[2] = 0x03;
	} else {
		data_out[1] = 0x04;
		data_out[2] = 0x26;
		data_en[1] = 0x04;
		data_en[2] = 0x28;
	}

	/*
	 * Disable Write Protect [Out]
	 */

	/* Read GPIO-Out Register */
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0003,			/* value */
					   0x0000,			/* idx */
					   data_out,			/* data */
					   sizeof(data_out) - 1,	/* data length */
					   NULL,			/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error reading GPIO-Out Register %02x%02x: ",
			       data_out[1], data_out[2]);
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_READ,
					   0x0003,			/* value */
					   0x0000,			/* idx */
					   &data_out[3],		/* data */
					   sizeof(data_out[3]),		/* data length */
					   NULL,			/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error reading GPIO-Out Register %02x%02x: ",
			       data_out[1], data_out[2]);
		return FALSE;
	}

	if (data_out[3] == 0xff) {
		g_prefix_error(error, "error reading GPIO-Out Register %02x%02x: ",
			       data_out[1], data_out[2]);
		return FALSE;
	}

	/* Write GPIO-Out Register */
	if (self->cpu_model == MCPU_TSUM_CD) {
		if (self->enable_security_wp) {
			if (disable)
				data_out[3] |= 0x0a; /* Pull High */
			else
				data_out[3] &= 0xf5; /* Pull Low */
		} else {
			if (disable)
				data_out[3] |= 0x01; /* Pull High */
			else
				data_out[3] &= 0xfe; /* Pull Low */
		}
	} else if (self->cpu_model == MCPU_TSUM_U) {
		if (self->enable_security_wp) {
			if (disable)
				data_out[3] |= 0x21; /* Pull High */
			else
				data_out[3] &= 0xde; /* Pull Low */
		} else {
			if (disable)
				data_out[3] |= 0x80; /* Pull High */
			else
				data_out[3] &= 0x7f; /* Pull Low */
		}
	} else if (self->cpu_model == MCPU_TSUM_V) {
		if (disable)
			data_out[3] |= 0x06; /* Pull High */
		else
			data_out[3] &= 0xf9; /* Pull Low */
	} else if (self->cpu_model == MCPU_TSUM_G_MSB6010) {
		if (disable)
			data_out[3] |= 0x10; /* Pull High */
		else
			data_out[3] &= 0xef; /* Pull Low */
	} else if (self->cpu_model == MSB6010) {
		if (disable)
			data_out[3] |= 0x04; /* Pull High */
		else
			data_out[3] &= 0xfb; /* Pull Low */
	} else if (self->cpu_model == MCPU_TSUM_G ||
		   self->cpu_model == MCPU_TSUM_F) {
		if (disable)
			data_out[3] |= 0x01; /* Pull Low */
		else
			data_out[3] &= 0xff; /* TODO: 0xfe? Pull Low */
	} else {
		if (disable)
			data_out[3] |= 0x01; /* Pull High */
		else
			data_out[3] &= 0xfe; /* Pull Low */
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0001,			/* value */
					   0x0000,			/* idx */
					   data_out,			/* data */
					   sizeof(data_out),		/* data length */
					   NULL,			/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error writing GPIO-Out Register %02x%02x=%02x: ",
			       data_out[1], data_out[2], data_out[3]);
		return FALSE;
	}

	/*
	 * Disable Write Protect [Output Enable]
	 */

	/* Read GPIO-Enable Register */
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0003,			/* value */
					   0x0000,			/* idx */
					   data_en,			/* data */
					   sizeof(data_en) - 1,		/* data length */
					   NULL,			/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error writing GPIO-Enable Register %02x%02x: ",
			       data_en[1], data_en[2]);
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_READ,
					   0x0003,			/* value */
					   0x0000,			/* idx */
					   &data_en[3],			/* data */
					   sizeof(data_en[3]),		/* data length */
					   NULL,			/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error reading GPIO-Out Register %02x%02x: ",
			       data_en[1], data_en[2]);
		return FALSE;
	}

	if (data_en[3] == 0xff) {
		g_prefix_error(error, "error reading GPIO-Enable Register %02x%02x: ",
			       data_en[1], data_en[2]);
		return FALSE;
	}

	if (self->cpu_model == MCPU_TSUM_CD) {
		if (self->enable_security_wp)
			data_en[3] &= 0xf5;
		else
			data_en[3] &= 0xfe;
	} else if (self->cpu_model == MCPU_TSUM_U) {
		if (self->enable_security_wp)
			data_en[3] &= 0xde;
		else
			data_en[3] &= 0x7f;
	} else if (self->cpu_model == MCPU_TSUM_V) {
		data_en[3] &= 0xf9;
	} else if (self->cpu_model == MCPU_TSUM_G_MSB6010) {
		data_en[3] &= 0xef;
	} else if (self->cpu_model == MSB6010) {
		data_en[3] &= 0xfb;
	} else {
		data_en[3] &= 0xfe;
	}

	/* Write GPIO-Enable Register */
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0001,		/* value */
					   0x0000,		/* idx */
					   data_en,		/* data */
					   sizeof(data_en),	/* data length */
					   NULL,		/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error writing GPIO-Enable Register %02x%02x=%02x: ",
			       data_en[1], data_en[2], data_en[3]);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_pause_r2_cpu(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x10, 0x00, 0x10, 0x0F, 0xD7, 0x00 };

	/*
	 * MST9U only!
	 *
	 * Pause R2 CPU for preventing Scaler entering Power Saving Mode also
	 * need for Disable SPI Flash Write Protect Mode
	 */

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0003,		/* value */
					   0x0000,		/* idx */
					   data,		/* data */
					   sizeof(data) - 1,	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error reading register %02x%02x%02x%02x%02x: ",
			       data[0], data[1], data[2], data[3], data[4]);
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_READ,
					   0x0003,		/* value */
					   0x0000,		/* idx */
					   &data[5],		/* data */
					   sizeof(data[5]),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error reading register %02x%02x%02x%02x%02x: ",
			       data[0], data[1], data[2], data[3], data[4]);
		return FALSE;
	}

	if (data[5] == 0xff) {
		g_prefix_error(error, "error reading register %02x%02x%02x%02x%02x: ",
			       data[0], data[1], data[2], data[3], data[4]);
		return FALSE;
	}

	data[5] |= 0x80;
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0003,		/* value */
					   0x0000,		/* idx */
					   data,		/* data */
					   sizeof(data),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error writing register %02x%02x%02x%02x%02x: ",
			       data[0], data[1], data[2], data[3], data[4]);
		return FALSE;
	}

	g_usleep(200000); /* 200ms */

	return TRUE;
}

static gboolean
fu_genesys_scaler_enter_isp_mode(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x4d, 0x53, 0x54, 0x41, 0x52 };

	/*
	 * Enter ISP mode:
	 *
	 * S + 0x92 + 0x4d + 0x53 + 0x54 + 0x41 + 0x52 + P
	 *
	 * Note: MStar application note say execute twice to avoid race
	 * condition
	 */
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data,		/* data */
					   sizeof(data),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_usleep(1000); /* 1ms */

		/* second try */
		if (!g_usb_device_control_transfer(usb_device,
						   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
						   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
						   G_USB_DEVICE_RECIPIENT_DEVICE,
						   GENESYS_SCALER_MSTAR_WRITE,
						   0x0000,		/* value */
						   0x0000,		/* idx */
						   data,		/* data */
						   sizeof(data),	/* data length */
						   NULL,		/* actual length */
						   GENESYS_SCALER_USB_TIMEOUT,
						   NULL,
						   error)) {
			g_prefix_error(error, "error entering ISP mode: ");
			return FALSE;
		}
	}

	g_usleep(1000); /* 1ms */

	return TRUE;
}

static gboolean
fu_genesys_scaler_exit_isp_mode(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data[] = { 0x24 };

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data,		/* data */
					   sizeof(data),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error exiting ISP mode: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_enter_isp(FuGenesysScaler *self, GError **error)
{
	/*
	 * Important: do not change the order below; otherwise, unexpected
	 * condition occurs.
	 */

	if (!fu_genesys_scaler_enter_serial_debug_mode(self, error))
		return FALSE;

	if (!fu_genesys_scaler_enter_single_step_mode(self, error))
		return FALSE;

	if (self->cpu_model == MCPU_MST9U || self->cpu_model == MCPU_TSUM_G)
		if (!fu_genesys_scaler_mst_i2c_bus_switch_to_ch0(self, error))
			return FALSE;

	if (!fu_genesys_scaler_enter_debug_mode(self, error))
		return FALSE;

	if (!fu_genesys_scaler_mst_i2c_bus_ctrl(self, error))
		return FALSE;

	if (!fu_genesys_scaler_disable_wp(self, TRUE, error))
		return FALSE;

	if (self->cpu_model == MCPU_MST9U) {
		/* Turn off powersaving */
		if (!fu_genesys_scaler_mst_i2c_bus_switch_to_ch4(self, error))
			return FALSE;

		if (!fu_genesys_scaler_mst_i2c_bus_ctrl(self, error))
			return FALSE;

		if (!fu_genesys_scaler_pause_r2_cpu(self, error))
			return FALSE;
	}

	if (!fu_genesys_scaler_enter_isp_mode(self, error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_genesys_scaler_exit(FuGenesysScaler *self, GError **error)
{
	/* [TODO]: CLI: commented in source */
#if 0
	if (!fu_genesys_scaler_disable_wp(self, FALSE, error))
		return FALSE;
#endif

	if (!fu_genesys_scaler_exit_single_step_mode(self, error))
		return FALSE;

	if (!fu_genesys_scaler_exit_serial_debug_mode(self, error))
		return FALSE;

	if (!fu_genesys_scaler_exit_isp_mode(self, error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_genesys_scaler_query_flash_id(FuGenesysScaler *self, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data1[] = {
		GENESYS_SCALER_CMD_DATA_WRITE,
		GENESYS_SCALER_FLASH_CONTROL_READ_ID,
	};
	guint8 data2[] = {
		GENESYS_SCALER_CMD_DATA_READ,
	};
	guint8 data3[] = {
		GENESYS_SCALER_CMD_DATA_END,
	};
	gsize actual_len;

	/*
	 * Read Flash ID:
	 *
	 * S + 0x92 + 0x10 + 0x9F + P
	 * S + 0x92 + 0x11 + P
	 * S + 0x93 + (read) data1 + data2 + data3 + P
	 * S + 0x92 + 0x12 + P
	 */
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data1,		/* data */
					   sizeof(data1),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error getting Flash ID: ");
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data2,		/* data */
					   sizeof(data2),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error getting Flash ID: ");
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_READ,
					   0x0000,			/* value */
					   0x0000,			/* idx */
					   self->flash_id,		/* data */
					   sizeof(self->flash_id),	/* data length */
					   &actual_len,			/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error getting Flash ID: ");
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data3,		/* data */
					   sizeof(data3),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error getting Flash ID: ");
		return FALSE;
	}

	if (((self->flash_id[0] == 0x00) && (self->flash_id[1] == 0x00) && (self->flash_id[2] == 0x00)) ||
	    ((self->flash_id[0] == 0xff) && (self->flash_id[1] == 0xff) && (self->flash_id[2] == 0xff))) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "Unknown flash chip");
		return FALSE;
	}

	if (g_getenv("FWUPD_GENESYS_SCALER_VERBOSE") != NULL)
		fu_common_dump_raw(G_LOG_DOMAIN, "Scaler Flash ID",
				   self->flash_id, sizeof(self->flash_id));

	return TRUE;
}

static gboolean
fu_genesys_scaler_get_level(FuGenesysScaler *self,
			    guint8 *level,
			    GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_INFO,
					   0x0004,		/* value */
					   0x0000,		/* idx */
					   level,		/* data */
					   1,			/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error getting level: ");
		return FALSE;
	}

	g_usleep(100000); /* 100ms */

	return TRUE;
}

static gboolean
fu_genesys_scaler_get_version(FuGenesysScaler *self,
			      guint8 *buf,
			      guint len,
			      GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_INFO,
					   0x0005,		/* value */
					   0x0000,		/* idx */
					   buf,			/* data */
					   len,			/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error getting version: ");
		return FALSE;
	}

	g_usleep(100000); /* 100ms */

	return TRUE;
}

static gboolean
fu_genesys_scaler_get_public_key(FuGenesysScaler *self,
				 guint8 *buf,
				 guint len,
				 GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	const gsize data_size = 0x20;
	guint16 index = 0x0000;
	guint count = 0;

	while (count < len) {
		int transfer_len = ((len - count) < data_size) ? len - count
							       : data_size;

		if (!g_usb_device_control_transfer(usb_device,
						   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
						   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
						   G_USB_DEVICE_RECIPIENT_DEVICE,
						   GENESYS_SCALER_INFO,
						   0x0006,		/* value */
						   index,		/* idx */
						   buf + count,		/* data */
						   transfer_len,	/* data length */
						   NULL,		/* actual length */
						   GENESYS_SCALER_USB_TIMEOUT,
						   NULL,
						   error)) {
			g_prefix_error(error, "error getting public key: ");
			return FALSE;
		}

		g_usleep(100000); /* 100ms */

		index += transfer_len;
		count += transfer_len;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_read_flash(FuGenesysScaler *self,
			     guint start_addr,
			     guint8 *buf,
			     guint len,
			     GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data1[] = {
		GENESYS_SCALER_CMD_DATA_WRITE,
		GENESYS_SCALER_FLASH_CONTROL_READ,
		(start_addr & 0x00ff0000) >> 16,
		(start_addr & 0x0000ff00) >> 8,
		(start_addr & 0x000000ff),
	};
	guint8 data2[] = {
		GENESYS_SCALER_CMD_DATA_READ,
	};
	guint8 data3[] = {
		GENESYS_SCALER_CMD_DATA_END,
	};
	const guint flash_rw_size = 64;
	guint addr = start_addr;
	guint count = 0;

	/*
	 * Read:
	 *
	 * S + 0x92 + 0x10 + 0x03 + addr1 + addr2 + addr3 + P
	 * S + 0x92 + 0x11
	 * S + 0x93 + (read) data1 + data2 + data3 + ... + data1024 + P
	 * S + 0x92 + 0x12 + P
	 */
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data1,		/* data */
					   sizeof(data1),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error reading flash at @%0x: ",
			       start_addr);
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data2,		/* data */
					   sizeof(data2),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error reading flash at @%0x: ",
			       start_addr);
		return FALSE;
	}

	while (count < len) {
		int transfer_len = ((len - count) < flash_rw_size) ? len - count
								   : flash_rw_size;
		if (!g_usb_device_control_transfer(usb_device,
						   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
						   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
						   G_USB_DEVICE_RECIPIENT_DEVICE,
						   GENESYS_SCALER_MSTAR_READ,
						   0x0000,		/* value */
						   0x0000,		/* idx */
						   buf + count,		/* data */
						   transfer_len,	/* data length */
						   NULL,		/* actual length */
						   GENESYS_SCALER_USB_TIMEOUT,
						   NULL,
						   error)) {
			g_prefix_error(error, "error reading flash at @%0x: ",
				       addr);
			return FALSE;
		}

		count += transfer_len;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data3,		/* data */
					   sizeof(data3),	/* data length */
					   NULL,		/* actual length */
					   GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error reading flash at @%0x: ",
			       start_addr);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_wait_flash_control_register_cb(FuDevice *dev,
						 gpointer user_data,
						 GError **error)
{
	FuGenesysScaler *self = FU_GENESYS_SCALER(dev);
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	WaitFlashRegisterHelper *helper = user_data;
	guint8 status = 0;

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_READ,
					   helper->reg << 8 | 0x04,	/* value */
					   0x0000,			/* idx */
					   &status,			/* data */
					   sizeof(status),		/* data length */
					   NULL,			/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error reading flash control register: ");
		return FALSE;
	}

	if ((status & 0x81) != helper->expected_val) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "wrong value in flash control register");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_flash_control_write_enable(FuGenesysScaler *self,
					     GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data1[] = {
		GENESYS_SCALER_CMD_DATA_WRITE,
		GENESYS_SCALER_FLASH_CONTROL_WRITE_ENABLE,
	};
	guint8 data2[] = {
		GENESYS_SCALER_CMD_DATA_END,
	};

	/*
	 * Write Enable:
	 *
	 * S + 0x92 + 0x10 + 0x06 + P
	 * S + 0x92 + 0x12 + P
	 */

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data1,		/* data */
					   sizeof(data1),	/* data length */
					   NULL,		/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error sending flash control write enable: ");
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data2,		/* data */
					   sizeof(data2),	/* data length */
					   NULL,		/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error sending flash control write enable: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_flash_control_write_status(FuGenesysScaler *self,
					     guint8 status,
					     GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	guint8 data1[] = {
		GENESYS_SCALER_CMD_DATA_WRITE,
		GENESYS_SCALER_FLASH_CONTROL_WRITE_STATUS,
		status,
	};
	guint8 data2[] = {
		GENESYS_SCALER_CMD_DATA_END,
	};

	/*
	 * Write Status Register:
	 *
	 * S + 0x92 + 0x10 + 0x01 + value + P
	 * S + 0x92 + 0x12 + P
	 */

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data1,		/* data */
					   sizeof(data1),	/* data length */
					   NULL,		/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error sending flash control write status 0x%02x: ", status);
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data2,		/* data */
					   sizeof(data2),	/* data length */
					   NULL,		/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error sending flash control write status 0x%02x: ", status);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_flash_control_sector_erase(FuGenesysScaler *self,
					     guint addr,
					     GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	WaitFlashRegisterHelper helper = {
		.reg = GENESYS_SCALER_FLASH_CONTROL_READ_STATUS,
		.expected_val = 0,
	};
	guint8 data1[] = {
		GENESYS_SCALER_CMD_DATA_WRITE,
		GENESYS_SCALER_FLASH_CONTROL_SECTOR_ERASE,
		(addr & 0x00ff0000) >> 16,
		(addr & 0x0000ff00) >> 8,
		(addr & 0x000000ff),
	};
	guint8 data2[] = {
		GENESYS_SCALER_CMD_DATA_END,
	};

	if (!fu_genesys_scaler_flash_control_write_enable(self, error))
		return FALSE;

	if (!fu_genesys_scaler_flash_control_write_status(self, 0x00, error))
		return FALSE;

	/* 5s: 500 x 10ms retries */
	if (!fu_device_retry(FU_DEVICE(self),
			     fu_genesys_scaler_wait_flash_control_register_cb,
			     500,
			     &helper,
			     error)) {
		g_prefix_error(error, "error waiting for flash control read status register: ");
		return FALSE;
	}

	/*
	 * Sector Erase, every 4K bytes:
	 *
	 * S + 0x92 + 0x10 + 0x20 + addr1 + addr2 + addr3 + P
	 * S + 0x92 + 0x12 + P
	 */

	if (!fu_genesys_scaler_flash_control_write_enable(self, error))
		return FALSE;

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data1,		/* data */
					   sizeof(data1),	/* data length */
					   NULL,		/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error sending flash control erase at address #%06x: ", addr);
		return FALSE;
	}

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0000,		/* value */
					   0x0000,		/* idx */
					   data2,		/* data */
					   sizeof(data2),	/* data length */
					   NULL,		/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error sending flash control erase at address #%06x: ", addr);
		return FALSE;
	}

	/* 5s: 500 x 10ms retries */
	if (!fu_device_retry(FU_DEVICE(self),
			     fu_genesys_scaler_wait_flash_control_register_cb,
			     500,
			     &helper,
			     error)) {
		g_prefix_error(error, "error waiting for flash control read status register: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_erase_flash(FuGenesysScaler *self,
			      guint start_addr,
			      guint len,
			      GError **error)
{
	const guint flash_erase_len = 4096;
	guint addr, count;

	addr = start_addr;
	count = (len + flash_erase_len - 1) / flash_erase_len;
	for (guint i = 0; i < count; i++) {
		if (!fu_genesys_scaler_flash_control_sector_erase(self, addr, error)) {
			g_prefix_error(error, "error erasing flash at address #%06x: ", addr);
			return FALSE;
		}

		addr += flash_erase_len;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_flash_control_page_program(FuGenesysScaler *self,
					     guint start_addr,
					     const guint8 *buf,
					     guint len,
					     GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(FU_DEVICE(self));
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(parent_device));
	WaitFlashRegisterHelper helper = {
		.reg = GENESYS_SCALER_FLASH_CONTROL_READ_STATUS,
		.expected_val = 0,
	};
	g_autofree guint8 *data = NULL;
	guint8 data1[] = {
		GENESYS_SCALER_CMD_DATA_WRITE,
		GENESYS_SCALER_FLASH_CONTROL_PAGE_PROGRAM,
		(start_addr & 0x00ff0000) >> 16,
		(start_addr & 0x0000ff00) >> 8,
		(start_addr & 0x000000ff),
	};
#if 0
	guint8 data2[] = {
		GENESYS_SCALER_CMD_DATA_END,
	};
#endif
	const guint trf_len = 64;
	guint count;

	/*
	 * Page Program, every 256 bytes:
	 *
	 * S + 0x92 + 0x10 + 0x02(program) + addr1 + addr2 + addr3 + code1 + code2 + code3 + ... + codeN + P
	 * [TODO]: ISP Tool comments says S + 0x92 + 0x12 + P, but it does not exist in source.
	 */

	data = g_malloc0(trf_len);
	memcpy(data, data1, sizeof(data1));
	memcpy(data + sizeof(data1), buf, trf_len - sizeof(data1));
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   0x0010,	/* value */
					   0x0000,	/* idx */
					   data,	/* data */
					   trf_len,	/* data length */
					   NULL,	/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error sending flash control page program at address #%06x: ",
			       start_addr);
		return FALSE;
	}

	count = len / trf_len;
	for (guint i = 1; i < count; i++) {
		memcpy(data, buf + (i * trf_len) - sizeof(data1), trf_len);
		if (!g_usb_device_control_transfer(usb_device,
						   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
						   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
						   G_USB_DEVICE_RECIPIENT_DEVICE,
						   GENESYS_SCALER_MSTAR_WRITE,
						   0x0010 + (0x0010 * i),	/* value */
						   0x0000,			/* idx */
						   data,			/* data */
						   trf_len,			/* data length */
						   NULL,			/* actual length */
						   (guint)GENESYS_SCALER_USB_TIMEOUT,
						   NULL,
						   error)) {
			g_prefix_error(error, "error sending flash control page program at address #%06x: ",
				       start_addr);
			return FALSE;
		}
	}

	memcpy(data, buf + (count * trf_len) - sizeof(data1), sizeof(data1));
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   (0x0010 + (0x0010 * count)) | 0x0080,	/* value */
					   0x0000,					/* idx */
					   data,					/* data */
					   sizeof(data1),				/* data length */
					   NULL,					/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error sending flash control page program at address #%06x: ",
			       start_addr);
		return FALSE;
	}

#if 0
	/* [TODO]: ISP Tool comments says S + 0x92 + 0x12 + P, but it does not exist in source. */
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_SCALER_MSTAR_WRITE,
					   /* ??? */,		/* value */
					   /* ??? */,		/* idx */
					   data2,		/* data */
					   sizeof(data2),	/* data length */
					   NULL,		/* actual length */
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error sending flash control page program at address #%06x: ",
			       addr);
		return FALSE;
	}
#endif

	/* [TODO]: CLI: was 200ms: 40 x 5ms retries */
	/* 200ms: 20 x 10ms retries */
	if (!fu_device_retry(FU_DEVICE(self),
			     fu_genesys_scaler_wait_flash_control_register_cb,
			     20,
			     &helper,
			     error)) {
		g_prefix_error(error, "error waiting for flash control read status register: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_write_sector(FuGenesysScaler *self,
			       guint start_addr,
			       const guint8 *buf,
			       guint len,
			       GError **error)
{
	const guint flash_page_program_len = 256;

	for (guint i = 0; i < len; i += flash_page_program_len)
		if (!fu_genesys_scaler_flash_control_page_program(self,
								  start_addr + i,
								  (guint8 *)buf + i,
								  flash_page_program_len,
								  error))
			return FALSE;

	return TRUE;
}

static gboolean
fu_genesys_scaler_write_flash(FuGenesysScaler *self,
			      guint start_addr,
			      const guint8 *buf,
			      guint len,
			      GError **error)
{
	const guint flash_sector_len = 4096;

	for (guint i = 0; i < len; i += flash_sector_len)
		if (!fu_genesys_scaler_write_sector(self,
						    start_addr + i,
						    (guint8 *)buf + i,
						    flash_sector_len,
						    error))
			return FALSE;

	return TRUE;
}

static gboolean
fu_genesys_scaler_probe(FuDevice *device, GError **error)
{
	FuGenesysScaler *self = FU_GENESYS_SCALER(device);
	guint8 version[7+1] = {0}; /* ?RIM123; where ? is 0x06 (length?) */
	self->cpu_model = MCPU_TSUM_G; /* Assuming model is TSUM_G for now */

	if (!fu_genesys_scaler_get_level(self, &self->level, error))
		return FALSE;

	if (!fu_genesys_scaler_get_version(self, version, sizeof(version), error))
		return FALSE;
	version[7] = 0;

	if (!fu_genesys_scaler_get_public_key(self, self->public_key,
					      sizeof(self->public_key), error))
		return FALSE;

	fu_device_set_version(device, (gchar *)&version[1]);
	fu_device_set_version_format(device, FWUPD_VERSION_FORMAT_PLAIN);
	fu_device_set_logical_id(device, "scaler");
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_UPDATABLE);

	if (g_getenv("FWUPD_GENESYS_SCALER_VERBOSE") != NULL) {
		fu_common_dump_raw(G_LOG_DOMAIN, "level", &self->level,
				   sizeof(self->level));
		fu_common_dump_raw(G_LOG_DOMAIN, "version", &version[1],
				   10);
		fu_common_dump_raw(G_LOG_DOMAIN, "public-key",
				   self->public_key, sizeof(self->public_key));
	}

	return TRUE;
}

static gboolean
fu_genesys_scaler_open(FuDevice *device, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(device);

	return fu_device_open(parent_device, error);
}

static gboolean
fu_genesys_scaler_close(FuDevice *device, GError **error)
{
	FuDevice *parent_device = fu_device_get_parent(device);

	return fu_device_open(parent_device, error);
}

static GBytes *
fu_genesys_scaler_dump_firmware(FuDevice *device, FuProgress *progress, GError **error)
{
	FuGenesysScaler *self = FU_GENESYS_SCALER(device);
	g_autofree guint8 *buf = NULL;
	gsize size = 0x200000;
	guint addr = 0x200000; /* [FIXME]: 0 if second-image is not supported */

	if (!fu_genesys_scaler_enter_isp(self, error))
		return NULL;

	/* [TODO] Think about moving this to the quirk file */
	if (!fu_genesys_scaler_query_flash_id(self, error))
		goto error;

	buf = g_malloc0(size);
	if (!fu_genesys_scaler_read_flash(self, addr, buf, size, error))
		goto error;

	if (!fu_genesys_scaler_exit(self, error))
		return NULL;

	return g_bytes_new_take(g_steal_pointer(&buf), size);

error:
	fu_genesys_scaler_exit(self, error);
	return NULL;
}

static void
fu_genesys_scaler_decrypt(guint8 *buf, gsize bufsz)
{
	const char *key = "mstar";
	const gsize keylen = strlen(key);

	for (guint i = 0; i < bufsz; i++)
		buf[i] ^= key[i % keylen];
}

static gboolean
fu_genesys_scaler_write_firmware(FuDevice *device,
				 FuFirmware *fw,
				 FuProgress *progress,
				 FwupdInstallFlags flags,
				 GError **error)
{
	FuGenesysScaler *self = FU_GENESYS_SCALER(device);
	g_autoptr(FuDeviceLocker) locker = NULL;
	guint protect_sector_addr[2] = { 0x000000 };
	gsize protect_sector_size[2] = { 0x000000 };
	guint public_key_addr = 0x000000;
	gsize public_key_size = 0x000000;
	guint addr = 0x000000;
	gsize size = 0x200000;
	const guint8 *data;
	MTKFooter footer;
	GBytes *fw_blob;

	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_ERASE, 7);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 93);

	fw_blob = fu_firmware_get_bytes(fw, error);
	if (!fw_blob)
		return FALSE;

	data = g_bytes_get_data(fw_blob, &size);
	memcpy(&footer, data + size - sizeof(footer), sizeof(footer));
	fu_genesys_scaler_decrypt((guint8 *)&footer, sizeof(footer));
	if (memcmp(footer.data.header.default_head, MTK_RSA_HEADER,
		   sizeof(footer.data.header.default_head)) != 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "invalid footer");
		return FALSE;
	}
	size -= sizeof(footer);

	if (footer.data.header.configuration_setting.bits.second_image)
		addr = *(guint32 *)footer.data.header.second_image_program_addr;

	if (footer.data.header.configuration_setting.bits.decrypt_mode) {
		public_key_addr = *(guint32 *)footer.data.header.scaler_public_key_addr;
		public_key_size = 0x1000;
	}

	if (footer.data.header.configuration_setting.bits.special_protect_sector) {
		if (footer.data.header.protect_sector[0].area.size) {
			protect_sector_addr[0] = (footer.data.header.protect_sector[0].area.addr_high  << 16) |
						 (footer.data.header.protect_sector[0].area.addr_low[1] << 8) |
						 (footer.data.header.protect_sector[0].area.addr_low[0]);
			protect_sector_addr[0] *= 0x1000;
			protect_sector_size[0] = footer.data.header.protect_sector[0].area.size * 0x1000;
		}

		if (footer.data.header.protect_sector[1].area.size) {
			protect_sector_addr[1] = (footer.data.header.protect_sector[1].area.addr_high  << 16) |
						 (footer.data.header.protect_sector[1].area.addr_low[1] << 8) |
						 (footer.data.header.protect_sector[1].area.addr_low[0]);
			protect_sector_addr[1] *= 0x1000;
			protect_sector_size[1] = footer.data.header.protect_sector[1].area.size * 0x1000;
		}
	}

	if (!fu_genesys_scaler_enter_isp(self, error))
		goto error;

	/* [TODO] Think about moving this to the quirk file */
	if (!fu_genesys_scaler_query_flash_id(self, error))
		goto error;

	if (!fu_genesys_scaler_erase_flash(self, addr, size, error))
		goto error;
	fu_progress_step_done(progress);                                        

	if (!fu_genesys_scaler_write_flash(self, addr, data, size, error))
		goto error;
	fu_progress_step_done(progress);

	if (!fu_genesys_scaler_exit(self, error))
		return FALSE;

	return TRUE;

error:
	fu_genesys_scaler_exit(self, error);
	return FALSE;
}

static void
fu_genesys_scaler_init(FuGenesysScaler *self)
{
	fu_device_add_protocol(FU_DEVICE(self), "com.genesys.scaler");
	fu_device_retry_set_delay(FU_DEVICE(self), 10); /* ms */
}

static void
fu_genesys_scaler_class_init(FuGenesysScalerClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS(klass);
	klass_device->probe = fu_genesys_scaler_probe;
	klass_device->open = fu_genesys_scaler_open;
	klass_device->close = fu_genesys_scaler_close;
	klass_device->dump_firmware = fu_genesys_scaler_dump_firmware;
	klass_device->write_firmware = fu_genesys_scaler_write_firmware;
}

FuGenesysScaler *
fu_genesys_scaler_new(void)
{
	FuGenesysScaler *device = NULL;
	device = g_object_new(FU_TYPE_GENESYS_SCALER, NULL);
	return device;
}
