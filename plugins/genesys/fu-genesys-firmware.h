/*
 * Copyright (C) 2021 Gaël PORTAY <gael.portay@collabora.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_GENESYS_FIRMWARE (fu_genesys_firmware_get_type())
G_DECLARE_FINAL_TYPE(FuGenesysFirmware, fu_genesys_firmware, FU, GENESYS_FIRMWARE, FuFirmware)
