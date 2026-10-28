// SPDX-License-Identifier: GPL-2.0
/*
 * Bluetooth on the MT6739 connectivity combo (Rufus Cuff): an HCI device whose packets
 * travel as STP type 0 over BTIF through the CONSYS driver (soc/mediatek/mt6739-consys.c),
 * H4-framed as the vendor's /dev/stpbt carried them.
 *
 * At every power-on, setup() replays stock's init for CONSYS chips (libbluetooth_mtk_pure's
 * bt_init_script_consys) from the watch's BT NVRAM: without it the controller runs but its
 * receiver hears nothing.
 */
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mt6739-consys.h>
#include <linux/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

struct btmt6739 {
	struct hci_dev *hdev;
	struct sk_buff *rx_skb;		/* packet being reassembled */
	unsigned int rx_hdr;		/* header bytes of the current packet type */
	unsigned int rx_need;		/* bytes still missing */
	bool rx_payload;		/* header done, rx_need counts the payload */
};

/*
 * The firmware (MT0699, LMP 4.2 subversion 0x326) runs the Bluetooth 4.0 LE commands but
 * reports octets 25-28 of Read Local Supported Commands as zero. The core then leaves the LE
 * event mask without advertising reports, and scans find nothing. Mark them supported.
 */
static void btmt6739_fix_commands(struct sk_buff *skb)
{
	static const u8 le_cmds[] = { 0xf7, 0xff, 0xff, 0x7f };	/* octets 25-28 */
	u8 *ev = skb->data;

	if (hci_skb_pkt_type(skb) != HCI_EVENT_PKT || skb->len < 6 + 64 ||
	    ev[0] != HCI_EV_CMD_COMPLETE || get_unaligned_le16(ev + 3) != HCI_OP_READ_LOCAL_COMMANDS ||
	    ev[5])
		return;
	memcpy(ev + 6 + 25, le_cmds, sizeof(le_cmds));
}

/* H4 packets from the chip, possibly split or joined across STP packets. */
static void btmt6739_recv(void *priv, const u8 *data, size_t len)
{
	struct btmt6739 *bt = priv;
	struct sk_buff *skb;
	size_t n;

	while (len) {
		if (!bt->rx_skb) {
			switch (data[0]) {
			case HCI_EVENT_PKT: bt->rx_hdr = HCI_EVENT_HDR_SIZE; break;
			case HCI_ACLDATA_PKT: bt->rx_hdr = HCI_ACL_HDR_SIZE; break;
			case HCI_SCODATA_PKT: bt->rx_hdr = HCI_SCO_HDR_SIZE; break;
			case HCI_ISODATA_PKT: bt->rx_hdr = HCI_ISO_HDR_SIZE; break;
			default:
				bt_dev_err(bt->hdev, "unknown packet type 0x%02x, dropping %zu bytes",
					   data[0], len);
				bt->hdev->stat.err_rx++;
				return;
			}
			bt->rx_skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_KERNEL);
			if (!bt->rx_skb)
				return;
			hci_skb_pkt_type(bt->rx_skb) = data[0];
			bt->rx_need = bt->rx_hdr;
			bt->rx_payload = false;
			data++;
			len--;
			continue;
		}

		skb = bt->rx_skb;
		n = min_t(size_t, len, bt->rx_need);
		if (skb->len + n > HCI_MAX_FRAME_SIZE) {
			bt_dev_err(bt->hdev, "packet too long");
			kfree_skb(skb);
			bt->rx_skb = NULL;
			return;
		}
		skb_put_data(skb, data, n);
		data += n;
		len -= n;
		bt->rx_need -= n;
		if (bt->rx_need)
			continue;

		if (!bt->rx_payload) {
			bt->rx_payload = true;
			switch (hci_skb_pkt_type(skb)) {
			case HCI_EVENT_PKT: bt->rx_need = skb->data[1]; break;
			case HCI_ACLDATA_PKT: bt->rx_need = get_unaligned_le16(skb->data + 2); break;
			case HCI_SCODATA_PKT: bt->rx_need = skb->data[2]; break;
			case HCI_ISODATA_PKT: bt->rx_need = get_unaligned_le16(skb->data + 2) & 0x3fff; break;
			}
			if (bt->rx_need)
				continue;
		}
		bt->rx_skb = NULL;
		bt->hdev->stat.byte_rx += skb->len;
		btmt6739_fix_commands(skb);
		hci_recv_frame(bt->hdev, skb);
	}
}

static int btmt6739_open(struct hci_dev *hdev)
{
	struct btmt6739 *bt = hci_get_drvdata(hdev);

	return mt6739_consys_bt_open(btmt6739_recv, bt);
}

static int btmt6739_close(struct hci_dev *hdev)
{
	struct btmt6739 *bt = hci_get_drvdata(hdev);

	mt6739_consys_bt_close();
	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
	return 0;
}

static int btmt6739_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	int ret;

	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	ret = mt6739_consys_bt_send(skb->data, skb->len);
	if (ret) {
		hdev->stat.err_tx++;
		return ret;	/* hci_send_frame() frees the skb on errors */
	}
	hdev->stat.byte_tx += skb->len;
	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT: hdev->stat.cmd_tx++; break;
	case HCI_ACLDATA_PKT: hdev->stat.acl_tx++; break;
	case HCI_SCODATA_PKT: hdev->stat.sco_tx++; break;
	}
	kfree_skb(skb);
	return 0;
}

/* MediaTek's vendor command for the public address (as btmtk_set_bdaddr). */
static int btmt6739_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;

	skb = __hci_cmd_sync(hdev, 0xfc1a, 6, bdaddr, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);
	kfree_skb(skb);
	return 0;
}

static int btmt6739_cmd(struct hci_dev *hdev, u16 opcode, const u8 *param, u32 len)
{
	struct sk_buff *skb = __hci_cmd_sync(hdev, opcode, len, param, HCI_INIT_TIMEOUT);

	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "vendor command 0x%04x failed (%ld)", opcode, PTR_ERR(skb));
		return PTR_ERR(skb);
	}
	kfree_skb(skb);
	return 0;
}

/*
 * Android's /data/nvram/APCFG/APRDEB/BT_Addr (ap_nvram_btradio_struct): address at 0 (most
 * significant byte first), Radio[6] at 12, Sleep[7] at 18, TxPWOffset[3] at 27. The
 * commands carry two (radio) and three (TX power) extension bytes after those, sent as zero:
 * zero in this watch's NVRAM too, so their NVRAM offsets were not pinned down. The core's
 * HCI Reset after setup() stands in for the reset that ends stock's script.
 */
#define BT_NVRAM		"mt6739-bt-nvram.bin"
#define BT_NVRAM_ADDR		0
#define BT_NVRAM_RADIO		12
#define BT_NVRAM_SLEEP		18
#define BT_NVRAM_TX_POWER	27

static int btmt6739_setup(struct hci_dev *hdev)
{
	const struct firmware *fw;
	const u8 *nv;
	u8 p[8] = { };
	int i, ret;

	ret = request_firmware(&fw, BT_NVRAM, hdev->dev.parent);
	if (ret)
		return ret;
	if (fw->size < BT_NVRAM_TX_POWER + 3) {
		bt_dev_err(hdev, "%s too short (%zu bytes)", BT_NVRAM, fw->size);
		release_firmware(fw);
		return -EINVAL;
	}
	nv = fw->data;

	if (memchr_inv(nv + BT_NVRAM_ADDR, 0, 6)) {
		for (i = 0; i < 6; i++)
			p[i] = nv[BT_NVRAM_ADDR + 5 - i];
		ret = btmt6739_cmd(hdev, 0xfc1a, p, 6);
	}
	memcpy(p, nv + BT_NVRAM_RADIO, 6);
	p[6] = p[7] = 0;
	if (!ret)
		ret = btmt6739_cmd(hdev, 0xfc79, p, 8);
	memcpy(p, nv + BT_NVRAM_TX_POWER, 3);
	memset(p + 3, 0, 3);
	if (!ret)
		ret = btmt6739_cmd(hdev, 0xfc93, p, 6);
	if (!ret)
		ret = btmt6739_cmd(hdev, 0xfc7a, nv + BT_NVRAM_SLEEP, 7);
	release_firmware(fw);
	return ret;
}

static int btmt6739_probe(struct platform_device *pdev)
{
	struct btmt6739 *bt;
	struct hci_dev *hdev;
	int ret;

	/*
	 * hci_register_dev() powers the device on once; if that open fails, the core never
	 * retries and the adapter stays in setup, invisible to mgmt (bluetoothd).
	 */
	ret = mt6739_consys_probed();
	if (ret)
		return ret;

	bt = devm_kzalloc(&pdev->dev, sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;
	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;
	bt->hdev = hdev;
	hdev->bus = HCI_UART;
	hci_set_drvdata(hdev, bt);
	SET_HCIDEV_DEV(hdev, &pdev->dev);
	hdev->open = btmt6739_open;
	hdev->close = btmt6739_close;
	hdev->send = btmt6739_send;
	hdev->setup = btmt6739_setup;
	hdev->set_bdaddr = btmt6739_set_bdaddr;
	/* Closing switches the BT function off in WMT, which forgets the setup. */
	hci_set_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	platform_set_drvdata(pdev, bt);

	ret = hci_register_dev(hdev);
	if (ret)
		hci_free_dev(hdev);
	return ret;
}

static void btmt6739_remove(struct platform_device *pdev)
{
	struct btmt6739 *bt = platform_get_drvdata(pdev);

	hci_unregister_dev(bt->hdev);
	hci_free_dev(bt->hdev);
}

static const struct of_device_id btmt6739_of_match[] = {
	{ .compatible = "rufus,mt6739-bluetooth" },
	{ }
};
MODULE_DEVICE_TABLE(of, btmt6739_of_match);

static struct platform_driver btmt6739_driver = {
	.probe = btmt6739_probe,
	.remove = btmt6739_remove,
	.driver = {
		.name = "btmt6739",
		.of_match_table = btmt6739_of_match,
	},
};
module_platform_driver(btmt6739_driver);

MODULE_DESCRIPTION("Bluetooth over the MT6739 CONSYS (STP)");
MODULE_LICENSE("GPL");
