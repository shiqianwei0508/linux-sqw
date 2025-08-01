/* SPDX-License-Identifier: GPL-2.0 */
/* Parts of this driver are based on the following:
 *  - Kvaser linux leaf driver (version 4.78)
 *  - CAN driver for esd CAN-USB/2
 *  - Kvaser linux usbcanII driver (version 5.3)
 *  - Kvaser linux mhydra driver (version 5.24)
 *
 * Copyright (C) 2002-2018 KVASER AB, Sweden. All rights reserved.
 * Copyright (C) 2010 Matthias Fuchs <matthias.fuchs@esd.eu>, esd gmbh
 * Copyright (C) 2012 Olivier Sobrie <olivier@sobrie.be>
 * Copyright (C) 2015 Valeo S.A.
 */

#ifndef KVASER_USB_H
#define KVASER_USB_H

/* Kvaser USB CAN dongles are divided into three major platforms:
 * - Hydra: Running firmware labeled as 'mhydra'
 * - Leaf: Based on Renesas M32C or Freescale i.MX28, running firmware labeled
 *         as 'filo'
 * - UsbcanII: Based on Renesas M16C, running firmware labeled as 'helios'
 */

#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <net/devlink.h>

#include <linux/can.h>
#include <linux/can/dev.h>

#define KVASER_USB_MAX_RX_URBS			4
#define KVASER_USB_MAX_TX_URBS			128
#define KVASER_USB_TIMEOUT			1000 /* msecs */
#define KVASER_USB_RX_BUFFER_SIZE		3072
#define KVASER_USB_MAX_NET_DEVICES		5

/* Kvaser USB device quirks */
#define KVASER_USB_QUIRK_HAS_SILENT_MODE	BIT(0)
#define KVASER_USB_QUIRK_HAS_TXRX_ERRORS	BIT(1)
#define KVASER_USB_QUIRK_IGNORE_CLK_FREQ	BIT(2)

/* Device capabilities */
#define KVASER_USB_CAP_BERR_CAP			0x01
#define KVASER_USB_CAP_EXT_CAP			0x02
#define KVASER_USB_HYDRA_CAP_EXT_CMD		0x04

#define KVASER_USB_SW_VERSION_MAJOR_MASK GENMASK(31, 24)
#define KVASER_USB_SW_VERSION_MINOR_MASK GENMASK(23, 16)
#define KVASER_USB_SW_VERSION_BUILD_MASK GENMASK(15, 0)

struct kvaser_usb_dev_cfg;

enum kvaser_usb_leaf_family {
	KVASER_LEAF,
	KVASER_USBCAN,
};

enum kvaser_usb_led_state {
	KVASER_USB_LED_ON = 0,
	KVASER_USB_LED_OFF = 1,
};

#define KVASER_USB_HYDRA_MAX_CMD_LEN		128
struct kvaser_usb_dev_card_data_hydra {
	u8 channel_to_he[KVASER_USB_MAX_NET_DEVICES];
	u8 sysdbg_he;
	spinlock_t transid_lock; /* lock for transid */
	u16 transid;
	/* lock for usb_rx_leftover and usb_rx_leftover_len */
	spinlock_t usb_rx_leftover_lock;
	u8 usb_rx_leftover[KVASER_USB_HYDRA_MAX_CMD_LEN];
	u8 usb_rx_leftover_len;
};
struct kvaser_usb_dev_card_data {
	u32 ctrlmode_supported;
	u32 capabilities;
	struct kvaser_usb_dev_card_data_hydra hydra;
	u32 usbcan_timestamp_msb;
};

/* Context for an outstanding, not yet ACKed, transmission */
struct kvaser_usb_tx_urb_context {
	struct kvaser_usb_net_priv *priv;
	u32 echo_index;
};

struct kvaser_usb_fw_version {
	u8 major;
	u8 minor;
	u16 build;
};

struct kvaser_usb_busparams {
	__le32 bitrate;
	u8 tseg1;
	u8 tseg2;
	u8 sjw;
	u8 nsamples;
} __packed;

struct kvaser_usb {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct kvaser_usb_net_priv *nets[KVASER_USB_MAX_NET_DEVICES];
	const struct kvaser_usb_driver_info *driver_info;
	const struct kvaser_usb_dev_cfg *cfg;

	struct usb_endpoint_descriptor *bulk_in, *bulk_out;
	struct usb_anchor rx_submitted;

	u32 ean[2];
	u32 serial_number;
	struct kvaser_usb_fw_version fw_version;
	u8 hw_revision;
	unsigned int nchannels;
	/* @max_tx_urbs: Firmware-reported maximum number of outstanding,
	 * not yet ACKed, transmissions on this device. This value is
	 * also used as a sentinel for marking free tx contexts.
	 */
	unsigned int max_tx_urbs;
	struct kvaser_usb_dev_card_data card_data;

	bool rxinitdone;
	void *rxbuf[KVASER_USB_MAX_RX_URBS];
	dma_addr_t rxbuf_dma[KVASER_USB_MAX_RX_URBS];
};

struct kvaser_usb_net_priv {
	struct can_priv can;
	struct devlink_port devlink_port;
	struct can_berr_counter bec;

	/* subdriver-specific data */
	void *sub_priv;

	struct kvaser_usb *dev;
	struct net_device *netdev;
	int channel;

	struct completion start_comp, stop_comp, flush_comp,
			  get_busparams_comp;
	struct usb_anchor tx_submitted;

	struct kvaser_usb_busparams busparams_nominal, busparams_data;

	spinlock_t tx_contexts_lock; /* lock for active_tx_contexts */
	int active_tx_contexts;
	struct kvaser_usb_tx_urb_context tx_contexts[];
};

/**
 * struct kvaser_usb_dev_ops - Device specific functions
 * @dev_set_mode:		used for can.do_set_mode
 * @dev_set_bittiming:		used for can.do_set_bittiming
 * @dev_get_busparams:		readback arbitration busparams
 * @dev_set_data_bittiming:	used for can.fd.do_set_data_bittiming
 * @dev_get_data_busparams:	readback data busparams
 * @dev_get_berr_counter:	used for can.do_get_berr_counter
 *
 * @dev_setup_endpoints:	setup USB in and out endpoints
 * @dev_init_card:		initialize card
 * @dev_init_channel:		initialize channel
 * @dev_remove_channel:		uninitialize channel
 * @dev_get_software_info:	get software info
 * @dev_get_software_details:	get software details
 * @dev_get_card_info:		get card info
 * @dev_get_capabilities:	discover device capabilities
 * @dev_set_led:		turn on/off device LED
 *
 * @dev_set_opt_mode:		set ctrlmod
 * @dev_start_chip:		start the CAN controller
 * @dev_stop_chip:		stop the CAN controller
 * @dev_reset_chip:		reset the CAN controller
 * @dev_flush_queue:		flush outstanding CAN messages
 * @dev_read_bulk_callback:	handle incoming commands
 * @dev_frame_to_cmd:		translate struct can_frame into device command
 */
struct kvaser_usb_dev_ops {
	int (*dev_set_mode)(struct net_device *netdev, enum can_mode mode);
	int (*dev_set_bittiming)(const struct net_device *netdev,
				 const struct kvaser_usb_busparams *busparams);
	int (*dev_get_busparams)(struct kvaser_usb_net_priv *priv);
	int (*dev_set_data_bittiming)(const struct net_device *netdev,
				      const struct kvaser_usb_busparams *busparams);
	int (*dev_get_data_busparams)(struct kvaser_usb_net_priv *priv);
	int (*dev_get_berr_counter)(const struct net_device *netdev,
				    struct can_berr_counter *bec);
	int (*dev_setup_endpoints)(struct kvaser_usb *dev);
	int (*dev_init_card)(struct kvaser_usb *dev);
	int (*dev_init_channel)(struct kvaser_usb_net_priv *priv);
	void (*dev_remove_channel)(struct kvaser_usb_net_priv *priv);
	int (*dev_get_software_info)(struct kvaser_usb *dev);
	int (*dev_get_software_details)(struct kvaser_usb *dev);
	int (*dev_get_card_info)(struct kvaser_usb *dev);
	int (*dev_get_capabilities)(struct kvaser_usb *dev);
	int (*dev_set_led)(struct kvaser_usb_net_priv *priv,
			   enum kvaser_usb_led_state state,
			   u16 duration_ms);
	int (*dev_set_opt_mode)(const struct kvaser_usb_net_priv *priv);
	int (*dev_start_chip)(struct kvaser_usb_net_priv *priv);
	int (*dev_stop_chip)(struct kvaser_usb_net_priv *priv);
	int (*dev_reset_chip)(struct kvaser_usb *dev, int channel);
	int (*dev_flush_queue)(struct kvaser_usb_net_priv *priv);
	void (*dev_read_bulk_callback)(struct kvaser_usb *dev, void *buf,
				       int len);
	void *(*dev_frame_to_cmd)(const struct kvaser_usb_net_priv *priv,
				  const struct sk_buff *skb, int *cmd_len,
				  u16 transid);
};

struct kvaser_usb_driver_info {
	u32 quirks;
	enum kvaser_usb_leaf_family family;
	const struct kvaser_usb_dev_ops *ops;
};

struct kvaser_usb_dev_cfg {
	const struct can_clock clock;
	const unsigned int timestamp_freq;
	const struct can_bittiming_const * const bittiming_const;
	const struct can_bittiming_const * const data_bittiming_const;
};

extern const struct kvaser_usb_dev_ops kvaser_usb_hydra_dev_ops;
extern const struct kvaser_usb_dev_ops kvaser_usb_leaf_dev_ops;

extern const struct devlink_ops kvaser_usb_devlink_ops;

int kvaser_usb_devlink_port_register(struct kvaser_usb_net_priv *priv);
void kvaser_usb_devlink_port_unregister(struct kvaser_usb_net_priv *priv);

void kvaser_usb_unlink_tx_urbs(struct kvaser_usb_net_priv *priv);

int kvaser_usb_recv_cmd(const struct kvaser_usb *dev, void *cmd, int len,
			int *actual_len);

int kvaser_usb_send_cmd(const struct kvaser_usb *dev, void *cmd, int len);

int kvaser_usb_send_cmd_async(struct kvaser_usb_net_priv *priv, void *cmd,
			      int len);

int kvaser_usb_can_rx_over_error(struct net_device *netdev);

extern const struct can_bittiming_const kvaser_usb_flexc_bittiming_const;

static inline ktime_t kvaser_usb_ticks_to_ktime(const struct kvaser_usb_dev_cfg *cfg,
						u64 ticks)
{
	return ns_to_ktime(div_u64(ticks * 1000, cfg->timestamp_freq));
}

static inline ktime_t kvaser_usb_timestamp48_to_ktime(const struct kvaser_usb_dev_cfg *cfg,
						      const __le16 *timestamp)
{
	u64 ticks = le16_to_cpu(timestamp[0]) |
		    (u64)(le16_to_cpu(timestamp[1])) << 16 |
		    (u64)(le16_to_cpu(timestamp[2])) << 32;

	return kvaser_usb_ticks_to_ktime(cfg, ticks);
}

static inline ktime_t kvaser_usb_timestamp64_to_ktime(const struct kvaser_usb_dev_cfg *cfg,
						      __le64 timestamp)
{
	return kvaser_usb_ticks_to_ktime(cfg, le64_to_cpu(timestamp));
}

#endif /* KVASER_USB_H */
