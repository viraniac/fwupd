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

#define GENESYS_SCALER_MSTAR_READ  0x7d
#define GENESYS_SCALER_MSTAR_WRITE 0x7e

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
	guint8 data[] = { 0x10, 0x00, 0x00, 0x00, 0x00 };

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
	 * S + 0x93 + (read) Data1 + Data2 + Data3 + P
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
	guint addr = 0x000000;

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
fu_genesys_scaler_init(FuGenesysScaler *self)
{
	fu_device_add_protocol(FU_DEVICE(self), "com.genesys.scaler");
}

static void
fu_genesys_scaler_class_init(FuGenesysScalerClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS(klass);
	klass_device->probe = fu_genesys_scaler_probe;
	klass_device->open = fu_genesys_scaler_open;
	klass_device->close = fu_genesys_scaler_close;
	klass_device->dump_firmware = fu_genesys_scaler_dump_firmware;
}

FuGenesysScaler *
fu_genesys_scaler_new(void)
{
	FuGenesysScaler *device = NULL;
	device = g_object_new(FU_TYPE_GENESYS_SCALER, NULL);
	return device;
}
