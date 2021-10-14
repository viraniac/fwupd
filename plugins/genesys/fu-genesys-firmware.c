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
	StaticToolString static_tool_string;
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

	/* Get static tool string */
	if (!fu_memcpy_safe((guint8 *)&self->static_tool_string,
			    sizeof(self->static_tool_string),
			    0, /* dst */
			    buf,
			    bufsz,
			    0x221, /* src */
			    sizeof(self->static_tool_string),
			    error))
		return FALSE;

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
fu_genesys_firmware_export(FuFirmware *firmware,
			   FuFirmwareExportFlags flags,
			   XbBuilderNode *bn)
{
	FuGenesysFirmware *self = FU_GENESYS_FIRMWARE(firmware);
	if (flags & FU_FIRMWARE_EXPORT_FLAG_INCLUDE_DEBUG) {
		gchar tool_string_version[2] = { '\0' };
		gchar mask_project_code[5] = { '\0' };
		gchar mask_project_hardware[2] = { '\0' };
		gchar mask_project_firmware[3] = { '\0' };
		gchar mask_project_ic_type[7+3] = { '\0' }; /* GLxxxx-xx */
		gchar running_project_code[5] = { '\0' };
		gchar running_project_hardware[2] = { '\0' };
		gchar running_project_firmware[3] = { '\0' };
		gchar running_project_ic_type[7+3] = { '\0' }; /* GLxxxx-xx */

		memcpy(tool_string_version,
		       &self->static_tool_string.tool_string_version,
		       sizeof(tool_string_version) - 1);
		fu_xmlb_builder_insert_kv(bn, "tool_string_version",
					  tool_string_version);

		memcpy(mask_project_code,
		       &self->static_tool_string.mask_project_code,
		       sizeof(mask_project_code) - 1);
		fu_xmlb_builder_insert_kv(bn, "mask_project_code",
					  mask_project_code);

		memcpy(mask_project_hardware,
		       &self->static_tool_string.mask_project_hardware,
		       sizeof(mask_project_hardware) - 1);
		mask_project_hardware[0] += 0x10; /* '1' -> 'A'... */
		fu_xmlb_builder_insert_kv(bn, "mask_project_hardware",
					  mask_project_hardware);

		memcpy(mask_project_firmware,
		       &self->static_tool_string.mask_project_firmware,
		       sizeof(mask_project_firmware) - 1);
		fu_xmlb_builder_insert_kv(bn, "mask_project_firmware",
					  mask_project_firmware);

		memcpy(&mask_project_ic_type[2],
		       &self->static_tool_string.mask_project_ic_type,
		       4);
		memcpy(&mask_project_ic_type[7],
		       &self->static_tool_string.mask_project_ic_type[4],
		       2);
		mask_project_ic_type[0] = 'G';
		mask_project_ic_type[1] = 'L';
		mask_project_ic_type[6] = '-';
		fu_xmlb_builder_insert_kv(bn, "mask_project_ic_type",
					  mask_project_ic_type);

		memcpy(running_project_code,
		       &self->static_tool_string.running_project_code,
		       sizeof(running_project_code) - 1);
		fu_xmlb_builder_insert_kv(bn, "running_project_code",
					  running_project_code);

		memcpy(running_project_hardware,
		       &self->static_tool_string.running_project_hardware,
		       sizeof(running_project_hardware) - 1);
		running_project_hardware[0] += 0x10; /* '1' -> 'A'... */
		fu_xmlb_builder_insert_kv(bn, "running_project_hardware",
					  running_project_hardware);

		memcpy(running_project_firmware,
		       &self->static_tool_string.running_project_firmware,
		       sizeof(running_project_firmware) - 1);
		fu_xmlb_builder_insert_kv(bn, "running_project_firmware",
					  running_project_firmware);

		memcpy(&running_project_ic_type[2],
		       &self->static_tool_string.running_project_ic_type,
		       4);
		memcpy(&running_project_ic_type[7],
		       &self->static_tool_string.running_project_ic_type[4],
		       2);
		running_project_ic_type[0] = 'G';
		running_project_ic_type[1] = 'L';
		running_project_ic_type[6] = '-';
		fu_xmlb_builder_insert_kv(bn, "running_project_ic_type",
					  running_project_ic_type);
	}
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
	klass_firmware->export = fu_genesys_firmware_export;
}
