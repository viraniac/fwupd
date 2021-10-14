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

static guint16
fu_genesys_firmware_checksum(const guint8 *buf, gsize bufsz)
{
	guint16 checksum = 0;

	for (guint i = 0; i < bufsz; i++)
		checksum += buf[i];

	return checksum;
}

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
	guint16 fw_checksum, checksum;
	guint16 code_size = 0x6000;

	/* Get checksum */
	if (!fu_common_read_uint16_safe(buf,
					bufsz,
					0x5FFE,
					&fw_checksum,
					G_BIG_ENDIAN,
					error))
		return FALSE;

	/* Calculate checksum */
	checksum = fu_genesys_firmware_checksum(buf,
						code_size - sizeof(checksum));
	if (checksum != fw_checksum)
		g_warning("checksum mismatch, got 0x%04x, expected 0x%04x",
			  checksum, fw_checksum);

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
        fu_firmware_add_flag(FU_FIRMWARE(self), FU_FIRMWARE_FLAG_HAS_CHECKSUM); 
}

static void
fu_genesys_firmware_class_init(FuGenesysFirmwareClass *klass)
{
	FuFirmwareClass *klass_firmware = FU_FIRMWARE_CLASS(klass);
	klass_firmware->parse = fu_genesys_firmware_parse;
}
