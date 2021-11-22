/*
 * Copyright (C) 2021 Gaël PORTAY <gael.portay@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>

#include "fu-genesys-scaler-firmware.h"

struct _FuGenesysScalerFirmware {
	FuFirmwareClass parent_instance;
	MTKFooter footer;
	guint protect_sector_addr[2];
	gsize protect_sector_size[2];
	guint public_key_addr;
	gsize public_key_size;
	guint addr;
	gsize size;
};

G_DEFINE_TYPE(FuGenesysScalerFirmware, fu_genesys_scaler_firmware, FU_TYPE_FIRMWARE)

void
fu_genesys_scaler_decrypt(guint8 *buf, gsize bufsz)
{
	const char *key = "mstar";
	const gsize keylen = strlen(key);

	for (guint i = 0; i < bufsz; i++)
		buf[i] ^= key[i % keylen];
}

static gboolean
fu_genesys_scaler_firmware_parse(FuFirmware *firmware,
				 GBytes *fw,
				 guint64 addr_start,
				 guint64 addr_end,
				 FwupdInstallFlags flags,
				 GError **error)
{
	FuGenesysScalerFirmware *self = FU_GENESYS_SCALER_FIRMWARE(firmware);
	gsize bufsz = 0;
	const guint8 *buf = g_bytes_get_data(fw, &bufsz);

	buf = g_bytes_get_data(fw, &bufsz);
	memcpy(&self->footer, buf + bufsz - sizeof(self->footer), sizeof(self->footer));
	fu_genesys_scaler_decrypt((guint8 *)&self->footer, sizeof(self->footer));
	if (memcmp(self->footer.data.header.default_head, MTK_RSA_HEADER,
		   sizeof(self->footer.data.header.default_head)) != 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "invalid footer");
		return FALSE;
	}
	self->size = bufsz - sizeof(self->footer);

	if (self->footer.data.header.configuration_setting.bits.second_image)
		self->addr = *(guint32 *)self->footer.data.header.second_image_program_addr;

	if (self->footer.data.header.configuration_setting.bits.decrypt_mode) {
		self->public_key_addr = *(guint32 *)self->footer.data.header.scaler_public_key_addr;
		self->public_key_size = 0x1000;
	}

	if (self->footer.data.header.configuration_setting.bits.special_protect_sector) {
		if (self->footer.data.header.protect_sector[0].area.size) {
			self->protect_sector_addr[0] = (self->footer.data.header.protect_sector[0].area.addr_high  << 16) |
						       (self->footer.data.header.protect_sector[0].area.addr_low[1] << 8) |
						       (self->footer.data.header.protect_sector[0].area.addr_low[0]);
			self->protect_sector_addr[0] *= 0x1000;
			self->protect_sector_size[0] = self->footer.data.header.protect_sector[0].area.size * 0x1000;
		}

		if (self->footer.data.header.protect_sector[1].area.size) {
			self->protect_sector_addr[1] = (self->footer.data.header.protect_sector[1].area.addr_high  << 16) |
						       (self->footer.data.header.protect_sector[1].area.addr_low[1] << 8) |
						       (self->footer.data.header.protect_sector[1].area.addr_low[0]);
			self->protect_sector_addr[1] *= 0x1000;
			self->protect_sector_size[1] = self->footer.data.header.protect_sector[1].area.size * 0x1000;
		}
	}

	return TRUE;
}

static void
fu_genesys_scaler_firmware_export(FuFirmware *firmware,
				  FuFirmwareExportFlags flags,
				  XbBuilderNode *bn)
{
	FuGenesysScalerFirmware *self = FU_GENESYS_SCALER_FIRMWARE(firmware);

	if (flags & FU_FIRMWARE_EXPORT_FLAG_INCLUDE_DEBUG) {
		fu_xmlb_builder_insert_kv(bn, "model_name",
					  (const gchar *)self->footer.data.header.model_name);

		fu_xmlb_builder_insert_kv(bn, "scaler_group",
					  (const gchar *)self->footer.data.header.scaler_group);

		fu_xmlb_builder_insert_kv(bn, "panel_type",
					  (const gchar *)self->footer.data.header.panel_type);

		fu_xmlb_builder_insert_kv(bn, "scaler_packet_date",
					  (const gchar *)self->footer.data.header.scaler_packet_date);

		fu_xmlb_builder_insert_kv(bn, "scaler_packet_version",
					  (const gchar *)self->footer.data.header.scaler_packet_version);

		fu_xmlb_builder_insert_kx(bn, "configuration_setting",
					  self->footer.data.header.configuration_setting.r8);

		if (self->footer.data.header.configuration_setting.bits.second_image)
			fu_xmlb_builder_insert_kx(bn, "second_image_program_addr", self->addr);

		if (self->footer.data.header.configuration_setting.bits.decrypt_mode) {
			gchar N[0x200+1] = { '\0' };
			gchar E[0x006+1] = { '\0' };

			fu_xmlb_builder_insert_kx(bn, "public_key_addr", self->public_key_addr);
			fu_xmlb_builder_insert_kx(bn, "public_key_size", self->public_key_size);

			memcpy(N, self->footer.data.public_key.N + 4, sizeof(N) - 1);
			fu_xmlb_builder_insert_kv(bn, "N", N);

			memcpy(E, self->footer.data.public_key.E + 4, sizeof(E) - 1);
			fu_xmlb_builder_insert_kv(bn, "E", E);
		}

		if (self->footer.data.header.configuration_setting.bits.special_protect_sector) {
			if (self->protect_sector_size[0]) {
				fu_xmlb_builder_insert_kx(bn, "protect_sector_addr[0]", self->protect_sector_addr[0]);
				fu_xmlb_builder_insert_kx(bn, "protect_sector_size[0]", self->protect_sector_size[0]);
			}

			if (self->protect_sector_size[1]) {
				fu_xmlb_builder_insert_kx(bn, "protect_sector_addr[1]", self->protect_sector_addr[1]);
				fu_xmlb_builder_insert_kx(bn, "protect_sector_size[1]", self->protect_sector_size[1]);
			}
		}

		if (self->footer.data.header.configuration_setting.bits.boot_code_size_in_header)
			fu_xmlb_builder_insert_kx(bn, "boot_code_size", self->footer.data.header.boot_code_size);

		fu_xmlb_builder_insert_kx(bn, "addr", self->addr);
		fu_xmlb_builder_insert_kx(bn, "size", self->size);
	}
}

static void
fu_genesys_scaler_firmware_init(FuGenesysScalerFirmware *self)
{
}

static void
fu_genesys_scaler_firmware_class_init(FuGenesysScalerFirmwareClass *klass)
{
	FuFirmwareClass *klass_firmware = FU_FIRMWARE_CLASS(klass);
	klass_firmware->parse = fu_genesys_scaler_firmware_parse;
	klass_firmware->export = fu_genesys_scaler_firmware_export;
}
