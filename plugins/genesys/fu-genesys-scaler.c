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
	guint8 model;
	guint8 level;
	guint8 public_key[0x212];
};

G_DEFINE_TYPE(FuGenesysScaler, fu_genesys_scaler, FU_TYPE_DEVICE)

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
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
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
					   (guint)GENESYS_SCALER_USB_TIMEOUT,
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
						   (guint)GENESYS_SCALER_USB_TIMEOUT,
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
fu_genesys_scaler_probe(FuDevice *device, GError **error)
{
	FuGenesysScaler *self = FU_GENESYS_SCALER(device);
	guint8 buf[0x10];
	char *vers = (char *)buf;

	self->model = MCPU_TSUM_G; /* Assuming model is TSUM_G for now */

	if (!fu_genesys_scaler_get_level(self, &self->level, error))
		return FALSE;

	if (!fu_genesys_scaler_get_version(self, buf, sizeof(buf), error))
		return FALSE;
	vers[10] = 0;
	vers++;

	if (!fu_genesys_scaler_get_public_key(self, self->public_key,
					      sizeof(self->public_key), error))
		return FALSE;

	fu_device_set_id(device, "Scaler");
	fu_device_set_name(device, "MStar MSB9100");
	fu_device_set_version(device, vers);
	fu_device_set_version_format(device, FWUPD_VERSION_FORMAT_PLAIN);
	fu_device_add_vendor_id(device, "MStar");
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_protocol(device, "com.genesys.scaler");

	if (g_getenv("FWUPD_GENESYS_SCALER_VERBOSE") != NULL) {
		fu_common_dump_raw(G_LOG_DOMAIN, "level", &self->level,
				   sizeof(self->level));
		fu_common_dump_raw(G_LOG_DOMAIN, "version", (guint8 *)vers,
				   10);
		fu_common_dump_raw(G_LOG_DOMAIN, "public-key",
				   self->public_key, sizeof(self->public_key));
	}

	return TRUE;
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
}

FuGenesysScaler *
fu_genesys_scaler_new(void)
{
	FuGenesysScaler *device = NULL;
	device = g_object_new(FU_TYPE_GENESYS_SCALER, NULL);
	return device;
}
