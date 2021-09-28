/*
 * Copyright (C) 2021 Ricardo Cañuelo <ricardo.canuelo@collabora.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_GENESYS_USBHUB (fu_genesys_usbhub_get_type())
G_DECLARE_FINAL_TYPE(FuGenesysUsbhub, fu_genesys_usbhub, FU, GENESYS_USBHUB, FuUsbDevice)
