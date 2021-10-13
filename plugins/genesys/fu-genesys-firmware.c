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
	gsize bufsz = 0;
	const guint8 *buf = g_bytes_get_data(fw, &bufsz);
	const StaticToolString *sts = (const StaticToolString *)&buf[0x221];
	gchar version[6] = { 0x00 };

	g_snprintf(version, sizeof(version), "%c%c.%c%c",
			sts->firmware_version[0],
			sts->firmware_version[1],
			sts->firmware_version[2],
			sts->firmware_version[3]);
	fu_firmware_set_version(firmware, version);

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
