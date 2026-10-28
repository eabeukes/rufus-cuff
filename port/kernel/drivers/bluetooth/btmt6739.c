// SPDX-License-Identifier: GPL-2.0
/*
 * Bluetooth on the MT6739 connectivity combo (Rufus Cuff): an HCI device whose packets
 * travel as STP type 0 over BTIF through the CONSYS driver (soc/mediatek/mt6739-consys.c),
 * H4-framed as the vendor's /dev/stpbt carried them.
 *
 * No radio calibration from the vendor NVRAM is sent; the chip runs on its defaults.
 */
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

static int btmt6739_probe(struct platform_device *pdev)
{
	struct btmt6739 *bt;
	struct hci_dev *hdev;
	int ret;

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
	hdev->set_bdaddr = btmt6739_set_bdaddr;
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
