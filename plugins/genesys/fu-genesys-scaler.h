/*
 * Copyright (C) 2021 Gaël PORTAY <gael.portay@collabora.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_GENESYS_SCALER (fu_genesys_scaler_get_type())
G_DECLARE_FINAL_TYPE(FuGenesysScaler, fu_genesys_scaler, FU, GENESYS_SCALER, FuDevice)

FuGenesysScaler *
fu_genesys_scaler_new(void);
