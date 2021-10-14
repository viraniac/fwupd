/*
 * Copyright (C) 2021 Gaël PORTAY <gael.portay@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>

#include "fu-genesys-common.h"

#include "fu-genesys-firmware.h"

struct _FuGenesysFirmware {
	FuFirmwareClass parent_instance;
	guint16 raw_fw_version;
};

G_DEFINE_TYPE(FuGenesysFirmware, fu_genesys_firmware, FU_TYPE_FIRMWARE)

static gboolean
fu_genesys_firmware_parse(FuFirmware *firmware,
			  GBytes *fw,
			  guint64 addr_start,
			  guint64 addr_end,
			  FwupdInstallFlags flags,
			  GError **error)
{
	FuGenesysFirmware *self = FU_GENESYS_FIRMWARE(firmware);
	gsize bufsz = 0;
	const guint8 *buf = g_bytes_get_data(fw, &bufsz);
	g_autofree gchar *fw_version = NULL;

	/* Get firmware version */
	if (!fu_common_read_uint16_safe(buf,
					bufsz,
					0x10E,
					&self->raw_fw_version,
					G_BIG_ENDIAN,
					error))
		return FALSE;

	fu_firmware_set_version_raw(firmware, self->raw_fw_version);
	fw_version = g_strdup_printf("%02x.%02x",
				     (self->raw_fw_version & 0x00FFU),
				     (self->raw_fw_version & 0xFF00U) >> 8);
	fu_firmware_set_version(firmware, fw_version);

	return TRUE;
}

static void
fu_genesys_firmware_init(FuGenesysFirmware *self)
{
}

static void
fu_genesys_firmware_class_init(FuGenesysFirmwareClass *klass)
{
	FuFirmwareClass *klass_firmware = FU_FIRMWARE_CLASS(klass);
	klass_firmware->parse = fu_genesys_firmware_parse;
}
