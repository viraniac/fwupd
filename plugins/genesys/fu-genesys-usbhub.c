/*
 * Copyright (C) 2021 Ricardo Cañuelo <ricardo.canuelo@collabora.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>

#include <gusb.h>
#include <string.h>

#include "fu-genesys-common.h"
#include "fu-genesys-flash-info-table.h"
#include "fu-genesys-usbhub.h"

#define GENESYS_USBHUB_STATIC_TOOL_DESC_IDX_USB_3_0  0x84
#define GENESYS_USBHUB_DYNAMIC_TOOL_DESC_IDX_USB_3_0 0x85
#define GENESYS_USBHUB_STATIC_TOOL_DESC_IDX_USB_2_0  0x81
#define GENESYS_USBHUB_DYNAMIC_TOOL_DESC_IDX_USB_2_0 0x82
#define GENESYS_USBHUB_FW_INFO_DESC_IDX		     0x83
#define GENESYS_USBHUB_VENDOR_SUPPORT_DESC_IDX	     0x86

#define GENESYS_USBHUB_FW_SIG_OFFSET   0xFC
#define GENESYS_USBHUB_FW_SIG_LEN      4
#define GENESYS_USBHUB_FW_SIG_TEXT_HUB "XROM"

#define GENESYS_USBHUB_CODE_SIZE_OFFSET 0xFB

#define GENESYS_USBHUB_CS_ISP_SW     0xA1
#define GENESYS_USBHUB_CS_ISP_READ   0xA2
#define GENESYS_USBHUB_CS_ISP_WRITE  0xA3
#define GENESYS_USBHUB_GL_HUB_VERIFY 0x71

#define GENESYS_USBHUB_ENCRYPT_REGION_START 0x01
#define GENESYS_USBHUB_ENCRYPT_REGION_END   0x15

#define GL3523_PUBLIC_KEY_LEN 0x212
#define GL3523_SIG_LEN	      0x100

#define GENESYS_USBHUB_USB_TIMEOUT	   5000 /* ms */
#define GENESYS_USBHUB_FLASH_WRITE_TIMEOUT 500	/* ms */

typedef enum {
	TOOL_STRING_VERSION_9BYTE_DYNAMIC,
	TOOL_STRING_VERSION_BONDING,
	TOOL_STRING_VERSION_BONDING_QC,
	TOOL_STRING_VERSION_VENDOR_SUPPORT,
	TOOL_STRING_VERSION_MULTI_TOKEN,
	TOOL_STRING_VERSION_2ND_DYNAMIC,
	TOOL_STRING_VERSION_RESERVED,
	TOOL_STRING_VERSION_13BYTE_DYNAMIC,
} ToolStringVersion;

typedef enum {
	ISP_EXIT,
	ISP_ENTER,
} IspMode;

typedef enum {
	FLASH_ERASE,
	FLASH_WRITE,
} FlashOperationCmd;

typedef enum {
	ISP_MODEL_UNKNOWN,

	/* hub */
	ISP_MODEL_HUB_GL3510,
	ISP_MODEL_HUB_GL3521,
	ISP_MODEL_HUB_GL3523,
	ISP_MODEL_HUB_GL3590,
	ISP_MODEL_HUB_GL7000,
	ISP_MODEL_HUB_GL3525,

	/* PD */
	ISP_MODEL_PD_GL9510,
} IspModel;

typedef struct __attribute__((packed)) {
	guint8 running_mode; /* 'M' or 'C' */

	guint8 ss_port_number; /* super-speed port number */
	guint8 hs_port_number; /* high-speed port number */

	guint8 ss_connection_status; /* bit field. ON = DFP is a super-speed device */
	guint8 hs_connection_status; /* bit field. ON = DFP is a high-speed device */
	guint8 fs_connection_status; /* bit field. ON = DFP is a full-speed device */
	guint8 ls_connection_status; /* bit field. ON = DFP is a low-speed device */

	guint8 charging;		  /* bit field. ON = DFP is a charging port */
	guint8 non_removable_port_status; /* bit field. ON = DFP is a non-removable port */

	/*
	 * Bonding reports Hardware register status for GL3523:
	 *   2 / 4 ports         : 1 means 4 ports, 0 means 2 ports
	 *   MTT / STT           : 1 means Multi Token Transfer, 0 means Single TT
	 *   Type - C            : 1 means disable, 0 means enable
	 *   QC                  : 1 means disable, 0 means enable
	 *   Flash dump location : 1 means 32KB offset, 0 means 0 offset.
	 *
	 * Tool string Version 1:
	 *   Bit3 : Flash dump location, BIT2 : Type - C, BIT1 : MTT / STT, BIT0 : 2 / 4 ports
	 * Tool string Version 2 or newer :
	 *   Bit4 : Flash dump location, BIT3 : Type - C, BIT2 : MTT / STT, BIT1 : 2 / 4 ports, BIT0
	 * : QC
	 *
	 * Bonding for GL3590:
	 *   Bit7 : Flash dump location, 0 means bank 0, 1 means bank 1.
	 */
	guint8 bonding;

	guint8 reserved[22];
} DynamicToolString;

typedef struct __attribute__((packed)) {
	guint8 tool_version[6]; /* ISP tool defined by itself */
	guint8 address_mode;
	guint8 build_fw_time[12];  /* YYYYMMDDhhmm */
	guint8 update_fw_time[12]; /* YYYYMMDDhhmm */
} FirmwareInfoToolString;

typedef struct __attribute__((packed)) {
	guint8 version[2];
	/*
	 * 0 means N/A, 1 means support generic type, above 2 means vendor support.
	 *
	 * dfp_device:     1 = Share Flash Chip.
	 *                 2 = GL3523-50/ Billboard.
	 *                 3 = GL3523-50/ C-Bridge.
	 * mstar_scaler:   2 = MSB9100/ RT1711P.
	 * hp_proprietary: 1 = Support code sign.
	 *                 2 = HP HW Check Code Signed.
	 *                 3 = HP SW Check Code Signed.
	 *                 4 = HP Code Signed Slave.
	 *                 5 = HP Hub Check Code Signed.
	 *                 7 = HP HW Check Code Signed (Debug).
	 *                 8 = HP SW Check Code Signed (Debug).
	 *                 9 = HP Code Signed Slave (Debug).
	 */
	guint8 dfp_device;
	guint8 mstar_scaler;
	guint8 realtek_scaler;
	guint8 richtek_pd;
	guint8 ti_pd;
	guint8 stm_pd;
	guint8 rohm_pd;
	guint8 eevertech_pd;
	guint8 hp_proprietary;
	guint8 belkin_arbitrator;
	guint8 gl_9510;
	guint8 gl_i2c_master_kit;
	guint8 support_mcu;
	guint8 support_hid;
} VendorSupportToolString;

typedef struct {
	guint8 req_switch;
	guint8 req_read;
	guint8 req_write;
} VendorCommandSetting;

typedef struct {
	guint8 reg;
	guint8 expected_val;
} WaitFlashRegisterHelper;

struct _FuGenesysUsbhub {
	FuUsbDevice parent_instance;
	StaticToolString static_tool_info;
	DynamicToolString dynamic_tool_info;
	FirmwareInfoToolString fwinfo_tool_info;
	VendorSupportToolString vendor_support_tool_info;
	VendorCommandSetting vcs;
	guint32 isp_model;
	guint8 isp_revision;
	guint32 flash_erase_delay;
	guint32 flash_write_delay;
	guint flash_rw_size;
	int flash_chip_idx;

	gboolean is_mask_code;
	gboolean support_fw_recovery;

	guint32 fw_bank_addr[2];
	guint32 code_size; /* 0: get from device */
	guint32 fw_data_total_count;
	guint32 extend_size;
};

G_DEFINE_TYPE(FuGenesysUsbhub, fu_genesys_usbhub, FU_TYPE_USB_DEVICE)

/* [TODO] Think about moving this to the quirk file */
static guint
fu_genesys_usbhub_get_flash_rw_size(FuGenesysUsbhub *self)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(self));
	guint rw_size;

	/*
	 * Workaround for GL3523-10 mask code bug. AAI programming
	 * doesn't work -> use 1-byte r/w size.
	 */
	if (flash_info[self->flash_chip_idx][FLASH_INFO_AAI_MODE_LOW] &&
	    self->isp_model == ISP_MODEL_HUB_GL3523 && self->isp_revision == 10 &&
	    self->is_mask_code)
		return 1;

	rw_size = flash_info[self->flash_chip_idx][FLASH_INFO_FLASH_WRITE_LENGTH];
	if (rw_size <= 64)
		return rw_size;

	if (g_usb_device_get_spec(usb_device) >= 0x300) {
		if (rw_size <= 128)
			return rw_size;
		rw_size -= 128;
		rw_size *= 128;
		if (rw_size > 128) {
			if (self->isp_model == ISP_MODEL_HUB_GL3523 ||
			    self->isp_model == ISP_MODEL_HUB_GL3590)
				rw_size = 256;
			else
				rw_size = 64;
		}
		if (rw_size > 512)
			return 512;
	}

	/* USB2.0 */
	return 64;
}

static gboolean
fu_genesys_usbhub_read_flash(FuGenesysUsbhub *self,
			     guint start_addr,
			     guint8 *buf,
			     guint len,
			     GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(self));
	guint addr = start_addr;
	guint count = 0;

	if (self->flash_rw_size == 0) {
		self->flash_rw_size = fu_genesys_usbhub_get_flash_rw_size(self);
	}
	while (count < len) {
		int transfer_len = ((len - count) < self->flash_rw_size) ? len - count
									 : self->flash_rw_size;
		if (!g_usb_device_control_transfer(usb_device,
						   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
						   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
						   G_USB_DEVICE_RECIPIENT_DEVICE,
						   self->vcs.req_read,
						   (addr & 0x0f0000) >> 4, /* value */
						   addr & 0xffff,	   /* idx */
						   buf + count,		   /* data */
						   transfer_len,	   /* data length */
						   NULL,		   /* actual length */
						   (guint)GENESYS_USBHUB_USB_TIMEOUT,
						   NULL,
						   error)) {
			g_prefix_error(error, "error reading flash at @%0x: ", addr);
			return FALSE;
		}
		addr += transfer_len;
		count += transfer_len;
	}

	return TRUE;
}

static gboolean
fu_genesys_usbhub_reset(FuGenesysUsbhub *self, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(self));

	/* send data to device */
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   self->vcs.req_switch,
					   0x0003, /* value */
					   0,	   /* idx */
					   NULL,   /* data */
					   0,	   /* data length */
					   NULL,   /* actual length */
					   (guint)GENESYS_USBHUB_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error resetting device: ");
		return FALSE;
	}

	return TRUE;
}

/*
 * [TODO]: Think about moving the required flash chip attributes to the
 * quirk file.
 *
 * Returns an index into the flash_info table. -1 in case of error
 * NOTE: This requires the device to be in ISP mode, eg.:
 *
 *   fu_genesys_usbhub_set_isp_mode(self, ISP_ENTER, error);
 */
static int
fu_genesys_usbhub_get_flash_chip_idx(FuGenesysUsbhub *self, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(self));
	int idx;
	guint8 i;

	for (i = 0; i < FLASH_CHIP_SUPPORTED_CHIPS; i++) {
		guint16 val = 0;
		guint8 buffer[64] = {0};

		if (flash_info[i][FLASH_INFO_RDID_DUMMY_ADDRESS] > 0)
			val = 0x0001;
		else
			val = 0x0002;
		val |= flash_info[i][FLASH_INFO_RDID_CMD] << 8;

		if (!g_usb_device_control_transfer(
			usb_device,
			G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
			G_USB_DEVICE_REQUEST_TYPE_VENDOR,
			G_USB_DEVICE_RECIPIENT_DEVICE,
			self->vcs.req_read,
			val,					   /* value */
			0,					   /* idx */
			buffer,					   /* data */
			flash_info[i][FLASH_INFO_RDID_CMD_LENGTH], /* data length */
			NULL,					   /* actual length */
			(guint)GENESYS_USBHUB_USB_TIMEOUT,
			NULL,
			error)) {
			g_prefix_error(error, "error reading flash chip: ");
			return -1;
		}
		if (memcmp(buffer,
			   &flash_info[i][FLASH_INFO_READ_DATA_1],
			   flash_info[i][FLASH_INFO_RDID_CMD_LENGTH]) == 0) {
			idx = i;
			break;
		}
	}
	if (i >= FLASH_CHIP_SUPPORTED_CHIPS) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "Unknown flash chip");
		return -1;
	}

	self->flash_erase_delay = flash_info[idx][FLASH_INFO_ERASE_DELAY_TIME];
	if (flash_info[idx][FLASH_INFO_CHIP_ERASE_UNIT] == 1)
		self->flash_erase_delay *= 1000;
	else
		self->flash_erase_delay *= 100;
	self->flash_write_delay = MAX(flash_info[idx][FLASH_INFO_WRITE_DELAY_TIME],
				      GENESYS_USBHUB_FLASH_WRITE_TIMEOUT);

	return idx;
}

static gboolean
fu_genesys_usbhub_wait_flash_status_register_cb(FuDevice *dev, gpointer user_data, GError **error)
{
	FuGenesysUsbhub *self = FU_GENESYS_USBHUB(dev);
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(dev));
	guint8 status = 0;
	WaitFlashRegisterHelper *helper = user_data;

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   self->vcs.req_read,
					   helper->reg << 8 | 0x02, /* value */
					   0,			    /* idx */
					   &status,		    /* data */
					   1,			    /* data length */
					   NULL,		    /* actual length */
					   (guint)GENESYS_USBHUB_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error, "error getting flash status register (0x%0x): ", helper->reg);
		return FALSE;
	}
	if (status != helper->expected_val) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "wrong value in flash status register");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_usbhub_set_isp_mode(FuGenesysUsbhub *self, IspMode mode, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(self));
	WaitFlashRegisterHelper helper;

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   self->vcs.req_switch,
					   mode, /* value */
					   0,	 /* idx */
					   NULL, /* data */
					   0,	 /* data length */
					   NULL, /* actual length */
					   (guint)GENESYS_USBHUB_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error,
			       "error setting isp mode - "
			       "control transfer error (reg 0x%0x) ",
			       self->vcs.req_switch);
		return FALSE;
	}
	helper.reg = 5;
	helper.expected_val = 0;
	if (!fu_device_retry(FU_DEVICE(self),
			     fu_genesys_usbhub_wait_flash_status_register_cb,
			     5,
			     &helper,
			     error)) {
		g_prefix_error(error, "error setting isp mode: ");
	}

	return TRUE;
}

#if 0
/* unneeded? */
static gboolean
fu_genesys_usbhub_unprotect_flash(FuGenesysUsbhub *self, FlashOperationCmd cmd, GError **error)
{
	int cmd_size = 0;

	if (cmd == FLASH_WRITE) {
		if (self->static_tool_info.tool_string_version >= TOOL_STRING_VERSION_VENDOR_SUPPORT ||
			(flash_info[self->flash_chip_idx][FLASH_INFO_UNPROTECT_FLAG] & 0x60) <= 0)
			return TRUE;
		if (flash_info[self->flash_chip_idx][FLASH_INFO_UNPROTECT_FLAG] & (1 << 7)) {
			/* enter ISP mode first */
			if (!fu_genesys_usbhub_set_isp_mode(self, ISP_ENTER, error)) {
				g_prefix_error(error, "error unprotecting flash for writing");
				return FALSE;
			}
		}
	}

	cmd_size = flash_info[self->flash_chip_idx][FLASH_INFO_UNPROTECT_FLAG] & 0x0f;
	if (opcode
}
#endif

static gboolean
fu_genesys_usbhub_authentication_request(FuGenesysUsbhub *self,
					 guint8 offset_start,
					 guint8 offset_end,
					 guint8 data_check,
					 GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(self));
	guint8 buf = 0;

	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_USBHUB_GL_HUB_VERIFY,
					   (offset_end << 8) | offset_start, /* value */
					   0,				     /* idx */
					   &buf,			     /* data */
					   1,				     /* data length */
					   NULL,			     /* actual length */
					   (guint)GENESYS_USBHUB_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error,
			       "control transfer error (req: 0x%0x): ",
			       (guint)GENESYS_USBHUB_GL_HUB_VERIFY);
		return FALSE;
	}
	if (!g_usb_device_control_transfer(usb_device,
					   G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					   G_USB_DEVICE_REQUEST_TYPE_VENDOR,
					   G_USB_DEVICE_RECIPIENT_DEVICE,
					   GENESYS_USBHUB_GL_HUB_VERIFY,
					   (offset_end << 8) | offset_start, /* value */
					   1 | (data_check << 8),	     /* idx */
					   &buf,			     /* data */
					   1,				     /* data length */
					   NULL,			     /* actual length */
					   (guint)GENESYS_USBHUB_USB_TIMEOUT,
					   NULL,
					   error)) {
		g_prefix_error(error,
			       "control transfer error (req: 0x%0x): ",
			       (guint)GENESYS_USBHUB_GL_HUB_VERIFY);
		return FALSE;
	}
	if (buf != 1) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "device authentication failed");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_usbhub_authenticate(FuGenesysUsbhub *self, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(self));
	guint8 low_byte;
	guint8 high_byte;
	guint8 temp_byte;
	guint16 offset_start;
	guint16 offset_end;
	guint8 *fwinfo = (guint8 *)&self->fwinfo_tool_info;

	if (self->vcs.req_switch != GENESYS_USBHUB_CS_ISP_SW) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "device authentication not supported");
		return FALSE;
	}

	low_byte = g_usb_device_get_release(usb_device) & 0xff;
	high_byte = (g_usb_device_get_release(usb_device) & 0xff00) >> 8;
	temp_byte = low_byte ^ high_byte;

	offset_start = g_random_int_range(GENESYS_USBHUB_ENCRYPT_REGION_START,
					  GENESYS_USBHUB_ENCRYPT_REGION_END - 1);
	offset_end = g_random_int_range(offset_start + 1, GENESYS_USBHUB_ENCRYPT_REGION_END);
	for (guint8 i = offset_start; i <= offset_end; i++) {
		temp_byte ^= fwinfo[i];
	}
	if (!fu_genesys_usbhub_authentication_request(self,
						      offset_start,
						      offset_end,
						      temp_byte,
						      error)) {
		g_prefix_error(error, "error authenticating device: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_genesys_usbhub_probe(FuDevice *device, GError **error)
{
	/* FuUsbDevice->probe */
	if (!FU_DEVICE_CLASS(fu_genesys_usbhub_parent_class)->probe(device, error))
		return FALSE;

	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_UPDATABLE);

	return TRUE;
}

static gboolean
fu_genesys_usbhub_open(FuDevice *device, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(device));

	/* FuUsbDevice->open */
	if (!FU_DEVICE_CLASS(fu_genesys_usbhub_parent_class)->open(device, error))
		return FALSE;

	if (!g_usb_device_claim_interface(usb_device,
					  0,
					  G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					  error)) {
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_genesys_usbhub_get_descriptor_data(GBytes *desc_bytes,
				      guint8 *dst,
				      guint dst_size,
				      GError **error)
{
	const guint8 *buf;
	gsize bufsz = 0;

	buf = g_bytes_get_data(desc_bytes, &bufsz);
	/* discard first 2 bytes (desc. length and type) */
	buf += 2;
	bufsz -= 2;
	for (guint8 i = 0, j = 0; i < bufsz && j < dst_size; i += 2, j++)
		dst[j] = buf[i];

	return TRUE;
}

static gboolean
fu_genesys_usbhub_check_fw_signature(FuGenesysUsbhub *self, int bank_num, GError **error)
{
	guint8 sig[GENESYS_USBHUB_FW_SIG_LEN] = {0};
	g_return_val_if_fail(bank_num < 2, FALSE);

	if (!fu_genesys_usbhub_read_flash(self,
					  self->fw_bank_addr[bank_num] +
					      GENESYS_USBHUB_FW_SIG_OFFSET,
					  sig,
					  GENESYS_USBHUB_FW_SIG_LEN,
					  error)) {
		g_prefix_error(error,
			       "error getting fw signature (bank %d) from device: ",
			       bank_num);
		return FALSE;
	}
	if (memcmp(sig, GENESYS_USBHUB_FW_SIG_TEXT_HUB, GENESYS_USBHUB_FW_SIG_LEN) != 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "wrong firmware signature");
		return FALSE;
	}

	return TRUE;
}

/* read the firmware size from the firmware stored in the device */
static gboolean
fu_genesys_usbhub_get_fw_size(FuGenesysUsbhub *self, int bank_num, GError **error)
{
	guint8 kbs = 0;

	g_return_val_if_fail(bank_num < 2, FALSE);
	g_return_val_if_fail(self->code_size == 0, FALSE);

	if (!fu_genesys_usbhub_check_fw_signature(self, bank_num, error))
		return FALSE;

	/* get firmware size from device */
	if (!fu_genesys_usbhub_read_flash(self,
					  self->fw_bank_addr[bank_num] +
					      GENESYS_USBHUB_CODE_SIZE_OFFSET,
					  &kbs,
					  1,
					  error)) {
		g_prefix_error(error, "error getting fw size from device: ");
		return FALSE;
	}
	self->code_size = 1024 * kbs;

	return TRUE;
}

static GBytes *
fu_genesys_usbhub_dump_firmware(FuDevice *device, FuProgress *progress, GError **error)
{
	FuGenesysUsbhub *self = FU_GENESYS_USBHUB(device);
	g_autofree guint8 *buf = NULL;
	int size = self->code_size + self->extend_size;

	if (!fu_genesys_usbhub_authenticate(self, error))
		return NULL;
	if (!fu_genesys_usbhub_set_isp_mode(self, ISP_ENTER, error))
		return NULL;

	buf = g_malloc0(size);
	if (!fu_genesys_usbhub_read_flash(self, self->fw_bank_addr[0], buf, size, error))
		return NULL;

	return g_bytes_new_take(g_steal_pointer(&buf), size);
}

static gboolean
fu_genesys_usbhub_setup(FuDevice *device, GError **error)
{
	FuGenesysUsbhub *self = FU_GENESYS_USBHUB(device);
	GUsbDevice *usb_device = fu_usb_device_get_dev(FU_USB_DEVICE(device));
	g_autofree gchar *manufacturer_str = NULL;
	g_autofree gchar *product_str = NULL;
	g_autoptr(GBytes) static_tool_buf = NULL;
	g_autoptr(GBytes) dynamic_tool_buf = NULL;
	g_autoptr(GBytes) fw_info_buf = NULL;
	g_autoptr(GBytes) vendor_support_buf = NULL;
	guint8 static_tool_desc_idx = 0;
	guint8 dynamic_tool_desc_idx = 0;

	/* FuUsbDevice->setup */
	if (!FU_DEVICE_CLASS(fu_genesys_usbhub_parent_class)->setup(device, error))
		return FALSE;

	/* [DEBUG] - additional info from device:
	 * release version: g_usb_device_get_release(usb_device)
	 */

	/* read standard string descriptors */
	if (g_usb_device_get_spec(usb_device) >= 0x300) {
		static_tool_desc_idx = GENESYS_USBHUB_STATIC_TOOL_DESC_IDX_USB_3_0;
		dynamic_tool_desc_idx = GENESYS_USBHUB_DYNAMIC_TOOL_DESC_IDX_USB_3_0;
	} else {
		static_tool_desc_idx = GENESYS_USBHUB_STATIC_TOOL_DESC_IDX_USB_2_0;
		dynamic_tool_desc_idx = GENESYS_USBHUB_DYNAMIC_TOOL_DESC_IDX_USB_2_0;
	}
	manufacturer_str =
	    g_usb_device_get_string_descriptor(usb_device,
					       g_usb_device_get_manufacturer_index(usb_device),
					       error);
	if (manufacturer_str == NULL)
		return FALSE;
	fu_device_set_vendor(device, manufacturer_str);

	product_str = g_usb_device_get_string_descriptor(usb_device,
							 g_usb_device_get_product_index(usb_device),
							 error);
	if (product_str == NULL)
		return FALSE;
	fu_device_set_name(device, product_str);

#if G_USB_CHECK_VERSION(0, 3, 8)
	/*
	 * Read/parse vendor-specific string descriptors and use that
	 * data to setup device attributes.
	 */
	static_tool_buf =
	    g_usb_device_get_string_descriptor_bytes_full(usb_device,
							  static_tool_desc_idx,
							  G_USB_DEVICE_LANGID_ENGLISH_UNITED_STATES,
							  64,
							  error);
	if (static_tool_buf != NULL) {
		if (!fu_genesys_usbhub_get_descriptor_data(static_tool_buf,
							   (guint8 *)&self->static_tool_info,
							   sizeof(StaticToolString),
							   error)) {
			g_prefix_error(error, "failed to get static tool info from device: ");
			return FALSE;
		}
		if (g_getenv("FWUPD_GENESYS_USBHUB_VERBOSE") != NULL)
			fu_common_dump_raw(G_LOG_DOMAIN,
					   "Static info",
					   (guint8 *)&self->static_tool_info,
					   sizeof(StaticToolString));
	} else {
		g_prefix_error(error, "failed to get static tool info from device: ");
		return FALSE;
	}
	if (self->static_tool_info.tool_string_version != 0xff) {
		char rev[3] = {0};
		guint64 tmp;

		/* [TODO] Move this to the quirk file */
		if (memcmp(self->static_tool_info.mask_project_ic_type, "3523", 4) == 0) {
			self->isp_model = ISP_MODEL_HUB_GL3523;
		} else if (memcmp(self->static_tool_info.mask_project_ic_type, "3590", 4) == 0) {
			self->isp_model = ISP_MODEL_HUB_GL3590;
		} else {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INTERNAL,
					    "Unknown ISP model");
			return FALSE;
		}
		memcpy(rev, &self->static_tool_info.mask_project_ic_type[4], 2);
		tmp = fu_common_strtoull(rev);
		self->isp_revision = tmp;
	}

	dynamic_tool_buf =
	    g_usb_device_get_string_descriptor_bytes_full(usb_device,
							  dynamic_tool_desc_idx,
							  G_USB_DEVICE_LANGID_ENGLISH_UNITED_STATES,
							  64,
							  error);
	if (dynamic_tool_buf != NULL) {
		if (!fu_genesys_usbhub_get_descriptor_data(dynamic_tool_buf,
							   (guint8 *)&self->dynamic_tool_info,
							   sizeof(DynamicToolString),
							   error)) {
			g_prefix_error(error, "failed to get dynamic tool info from device: ");
			return FALSE;
		}
		if (g_getenv("FWUPD_GENESYS_USBHUB_VERBOSE") != NULL)
			fu_common_dump_raw(G_LOG_DOMAIN,
					   "Dynamic info",
					   (guint8 *)&self->dynamic_tool_info,
					   sizeof(DynamicToolString));
	} else {
		g_prefix_error(error, "failed to get dynamic tool info from device: ");
		return FALSE;
	}
	if (self->dynamic_tool_info.running_mode == 'M')
		self->is_mask_code = TRUE;

	fw_info_buf =
	    g_usb_device_get_string_descriptor_bytes_full(usb_device,
							  GENESYS_USBHUB_FW_INFO_DESC_IDX,
							  G_USB_DEVICE_LANGID_ENGLISH_UNITED_STATES,
							  64,
							  error);
	if (fw_info_buf != NULL) {
		if (!fu_genesys_usbhub_get_descriptor_data(fw_info_buf,
							   (guint8 *)&self->fwinfo_tool_info,
							   sizeof(FirmwareInfoToolString),
							   error)) {
			g_prefix_error(error, "failed to get firmware info from device: ");
			return FALSE;
		}
		if (g_getenv("FWUPD_GENESYS_USBHUB_VERBOSE") != NULL)
			fu_common_dump_raw(G_LOG_DOMAIN,
					   "Fw info",
					   (guint8 *)&self->fwinfo_tool_info,
					   sizeof(FirmwareInfoToolString));
	} else {
		g_prefix_error(error, "failed to get firmware info from device: ");
		return FALSE;
	}

	if (self->static_tool_info.tool_string_version >= TOOL_STRING_VERSION_VENDOR_SUPPORT) {
		vendor_support_buf = g_usb_device_get_string_descriptor_bytes_full(
		    usb_device,
		    GENESYS_USBHUB_VENDOR_SUPPORT_DESC_IDX,
		    G_USB_DEVICE_LANGID_ENGLISH_UNITED_STATES,
		    64,
		    error);
		if (vendor_support_buf != NULL) {
			if (!fu_genesys_usbhub_get_descriptor_data(
				vendor_support_buf,
				(guint8 *)&self->vendor_support_tool_info,
				sizeof(VendorSupportToolString),
				error)) {
				g_prefix_error(error,
					       "failed to get vendor support info from device: ");
				return FALSE;
			}
			if (g_getenv("FWUPD_GENESYS_USBHUB_VERBOSE") != NULL)
				fu_common_dump_raw(G_LOG_DOMAIN,
						   "Vendor support info",
						   (guint8 *)&self->vendor_support_tool_info,
						   sizeof(VendorSupportToolString));
		} else {
			g_prefix_error(error, "failed to get vendor support info from device: ");
			return FALSE;
		}
	}

	/*
	 * Device-specific configuration.
	 * [TODO]: Consider moving these to the quirk file.
	 */
	if (self->vendor_support_tool_info.hp_proprietary) {
		self->vcs.req_switch = GENESYS_USBHUB_CS_ISP_SW;
		self->vcs.req_read = GENESYS_USBHUB_CS_ISP_READ;
		self->vcs.req_write = GENESYS_USBHUB_CS_ISP_WRITE;
	} else {
		self->vcs.req_switch = 0x81;
		self->vcs.req_read = 0x82;
		self->vcs.req_write = 0x83;
	}

	if (!fu_genesys_usbhub_authenticate(self, error))
		return FALSE;
	/* Identify the flash chip */
	if (!fu_genesys_usbhub_set_isp_mode(self, ISP_ENTER, error))
		return FALSE;
	self->flash_chip_idx = fu_genesys_usbhub_get_flash_chip_idx(self, error);
	if (self->flash_chip_idx < 0)
		return FALSE;
	self->flash_rw_size = fu_genesys_usbhub_get_flash_rw_size(self);

	/* setup firmware parameters */
	switch (self->isp_model) {
	case ISP_MODEL_HUB_GL3523:
		self->support_fw_recovery = TRUE;
		self->fw_bank_addr[0] = 0x0000;
		self->fw_bank_addr[1] = 0x8000;
		self->fw_data_total_count = 0x6000;
		self->extend_size = GL3523_PUBLIC_KEY_LEN + GL3523_SIG_LEN;
		if (self->isp_revision == 50) {
			self->fw_data_total_count = 0x8000;
			if (!fu_genesys_usbhub_get_fw_size(self, 0, error))
				return FALSE;
		} else {
			self->code_size = self->fw_data_total_count;
		}
		break;
	case ISP_MODEL_HUB_GL3590:
		self->support_fw_recovery = TRUE;
		if (!fu_genesys_usbhub_get_fw_size(self, 0, error))
			return FALSE;
		self->fw_bank_addr[0] = 0x0000;
		self->fw_bank_addr[1] = 0x10000;
		/* [TODO]: bank address for bridge, necessary? */
		self->fw_data_total_count = 0x8000;
		break;
	default:
		break;
	}
#else
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_NOT_SUPPORTED,
		    "GUsb version %s is too old, "
		    "fwupd needs to be rebuilt against 0.3.6 or later",
		    g_usb_version_string());
	return FALSE;
#endif
	return TRUE;
}

static void
fu_genesys_usbhub_init(FuGenesysUsbhub *self)
{
	fu_device_add_protocol(FU_DEVICE(self), "com.genesys.usbhub");
	fu_device_retry_set_delay(FU_DEVICE(self), 30); /* ms */
}

static void
fu_genesys_usbhub_class_init(FuGenesysUsbhubClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS(klass);
	klass_device->probe = fu_genesys_usbhub_probe;
	klass_device->open = fu_genesys_usbhub_open;
	klass_device->setup = fu_genesys_usbhub_setup;
	klass_device->dump_firmware = fu_genesys_usbhub_dump_firmware;
}
