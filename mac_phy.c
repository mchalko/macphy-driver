// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/* NCV7410 MACPHY
 *
 */

#include "open_alliance.h"

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cache.h>
#include <linux/crc8.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/regulator/consumer.h>
#include <linux/phy.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <asm/unaligned.h>

#define REG_ADDRFILT_LOW        0x10
#define REG_ADDRFILT_HIGH       0x11
#define REG_ADDRFILT_MMS        0x1
#define REG_ADDRFILT_HIGH_EN    BIT(31)

#define REG_ADDRMASK_LOW        0x20
#define REG_ADDRMASK_HIGH       0x21
#define REG_ADDRMASK_MMS        0x1

#define REG_CONTROL0            0x0
#define REG_CONTROL0_MMS        0x1
#define REG_CONTROL0_ADRF       BIT(16)
#define REG_CONTROL0_FCSA       BIT(8)
#define REG_CONTROL0_TXEN       BIT(1)
#define REG_CONTROL0_RXEN       BIT(0)

#define REG_PHYCON              0xFF00
#define REG_PHYCON_MMS          0x0
#define REG_PHYCON_LINKCTRL     BIT(12)

#define REG_TWEAKS1             32769
#define REG_TWEAKS1_MMS         0x4
#define REG_TWEAKS1_UNJT        BIT(6)
#define REG_TWEAKS1_ENIE        BIT(7)

#define MAC_PHY_DEV_MAC_ADDR_SLOT   0
#define MAC_PHY_BROADCAST_MAC_ADDR_SLOT   1
#define MAC_PHY_MULTICAST_MAC_ADDR_SLOT   2

static const struct of_device_id mac_phy_match_table[] = {
        {.compatible = "ncv,ncv7410"},
        {}
};

static const struct spi_device_id mac_phy_spi_id[] = {
        {.name = "ncv7410"},
        {}
};

struct mac_phy_net_priv {
    struct mac_phy_dev_priv *dev_priv;
    struct net_device *netdev;
    struct work_struct tx_work;
    struct sk_buff_head txq;
};

struct mac_phy_dev_priv {
    struct mac_phy_net_priv *net_priv;
    struct spi_device *spidev;
    struct open_alliance *oa;
    spinlock_t locK_tx_space;
    u32 tx_space;
};

static irqreturn_t mac_phy_irq(int irq, void *p) {
    struct mac_phy_dev_priv *priv = p;

    oa_process_interrupt(priv->oa);

    return IRQ_HANDLED;
}

static int mac_phy_write_mac_address(struct open_alliance* oa, int mac_nr, u8 *addr,
                                     u8 *mask) {
    u32 offset = mac_nr * 2;
    int ret;
    u32 val;

    val = get_unaligned_be16(&addr[0]) | REG_ADDRFILT_HIGH_EN;
    ret = oa_write_reg(oa, REG_ADDRFILT_MMS, REG_ADDRFILT_HIGH + offset, val);
    if (ret < 0)
        return ret;

    val = get_unaligned_be32(&addr[2]);
    ret = oa_write_reg(oa, REG_ADDRFILT_MMS, REG_ADDRFILT_LOW + offset, val);
    if (ret < 0)
        return ret;

    val = get_unaligned_be16(&mask[0]);
    ret = oa_write_reg(oa, REG_ADDRFILT_MMS, REG_ADDRMASK_HIGH + offset, val);
    if (ret < 0)
        return ret;

    val = get_unaligned_be32(&mask[2]);
    return oa_write_reg(oa, REG_ADDRFILT_MMS, REG_ADDRMASK_LOW + offset, val);
}

static int mac_phy_set_mac_address(struct net_device *netdev, void *addr) {
    struct mac_phy_net_priv *net_priv = netdev_priv(netdev);
    struct sockaddr *sa = addr;
    u8 mask[ETH_ALEN];

    if (netif_running(netdev))
        return -EBUSY;

    if (!is_valid_ether_addr(sa->sa_data))
        return -EADDRNOTAVAIL;

    ether_addr_copy(netdev->dev_addr, sa->sa_data);
    memset(mask, 0xFF, ETH_ALEN);

    return mac_phy_write_mac_address(net_priv->dev_priv->oa, MAC_PHY_DEV_MAC_ADDR_SLOT, netdev->dev_addr, mask);
}

static int mac_phy_init_mac(struct mac_phy_net_priv *net_priv) {
    struct net_device *netdev = net_priv->netdev;
    u8 mask[ETH_ALEN];
    u8 mac[ETH_ALEN];
    int ret;

    memset(mask, 0xFF, ETH_ALEN);
    memset(mac, 0xFF, ETH_ALEN);

    ret = mac_phy_write_mac_address(net_priv->dev_priv->oa, MAC_PHY_DEV_MAC_ADDR_SLOT, netdev->dev_addr, mask);
    if (ret < 0) {
        netdev_err(netdev, "Could not set MAC address: %pM, %d\n", netdev->dev_addr, ret);
        return ret;
    }

    ret = mac_phy_write_mac_address(net_priv->dev_priv->oa, MAC_PHY_BROADCAST_MAC_ADDR_SLOT, mac, mask);
    if (ret < 0) {
        netdev_err(netdev, "Could not set Broadcast MAC address: %d\n", ret);
        return ret;
    }

    return 0;
}

static int mac_phy_configure_phy(void * context){
    struct mac_phy_dev_priv *dev_priv = context;
    int ret;
    u32 val;

    // configure mac address
    ret = mac_phy_init_mac(dev_priv->net_priv);

    if (ret < 0)
        return ret;
    // enable communication, filter adresses and append crc
    val = REG_CONTROL0_TXEN | REG_CONTROL0_RXEN |
          REG_CONTROL0_FCSA | REG_CONTROL0_ADRF;
    ret = oa_write_reg(dev_priv->oa, REG_CONTROL0_MMS, REG_CONTROL0, val);
    if (ret < 0)
        return ret;
    // unjabber timer and noise immunity enhancement
    val = REG_TWEAKS1_ENIE | REG_TWEAKS1_UNJT;
    ret = oa_reg_set_bits(dev_priv->oa, REG_TWEAKS1_MMS, REG_TWEAKS1, val, val);
    if (ret < 0)
        return ret;
    // enable link
    val = REG_PHYCON_LINKCTRL;
    ret = oa_write_reg(dev_priv->oa, REG_PHYCON_MMS, REG_PHYCON, val);
    if (ret < 0)
        return ret;
    return 0;
}

static int mac_phy_net_open(struct net_device *net_dev) {
    struct mac_phy_net_priv *net_priv = netdev_priv(net_dev);
    struct mac_phy_dev_priv *dev_priv = net_priv->dev_priv;
    int ret;

    ret = oa_net_open(dev_priv->oa, mac_phy_configure_phy, dev_priv);
    if (ret < 0)
        return ret;
    enable_irq(dev_priv->spidev->irq);
    netif_start_queue(net_dev);
    OA_DEBUG("MACPHY: net device opened");
    return 0;
}

static int mac_phy_net_stop(struct net_device *net_dev) {
    struct mac_phy_net_priv *net_priv = netdev_priv(net_dev);
    struct mac_phy_dev_priv *dev_priv = net_priv->dev_priv;

    netif_stop_queue(net_priv->netdev);
    flush_work(&net_priv->tx_work);
    disable_irq(dev_priv->spidev->irq);
    OA_DEBUG("MACPHY: net device closed");
    return oa_net_stop(net_priv->dev_priv->oa);
}

static void mac_phy_tx_work(struct work_struct *work) {
    struct mac_phy_net_priv *net_priv = container_of(work, struct mac_phy_net_priv, tx_work);
    struct mac_phy_dev_priv *dev_priv = net_priv->dev_priv;
    struct sk_buff *skb;
    int ret;

    while (!skb_queue_empty(&net_priv->txq)) {
        skb = skb_dequeue(&net_priv->txq);

        if (skb) {
            ret = oa_write(dev_priv->oa, skb);
            if (ret < 0)
                netdev_err(net_priv->netdev, "MACPHY: Frame write error %d\n", ret);

            dev_kfree_skb(skb);
        }
    }
}

static netdev_tx_t mac_phy_start_xmit(struct sk_buff *skb, struct net_device *dev) {
    struct mac_phy_net_priv *net_priv = netdev_priv(dev);
    struct mac_phy_dev_priv *dev_priv = net_priv->dev_priv;
    netdev_tx_t netdev_ret = NETDEV_TX_OK;
    u32 tx_space_needed = skb->len;

    spin_lock(&dev_priv->locK_tx_space);
    if (tx_space_needed > dev_priv->tx_space) {
        netif_stop_queue(dev);
        netdev_ret = NETDEV_TX_BUSY;
    } else {
        dev_priv->tx_space -= tx_space_needed;
        skb_queue_tail(&net_priv->txq, skb);
    }
    spin_unlock(&dev_priv->locK_tx_space);

    schedule_work(&net_priv->tx_work);

    return netdev_ret;
}

static const struct net_device_ops mac_phy_netdev_ops = {
        .ndo_open        = mac_phy_net_open,
        .ndo_stop        = mac_phy_net_stop,
        .ndo_start_xmit        = mac_phy_start_xmit,
        .ndo_set_mac_address    = mac_phy_set_mac_address,
        .ndo_validate_addr    = eth_validate_addr,
};

static void mac_phy_remove_open_alliance(void *data) {
    struct open_alliance* oa = data;
    oa_delete(oa);
}

static void mac_phy_tx_finished_cb(void* context){
    struct mac_phy_dev_priv *dev_priv = context;

    spin_lock(&dev_priv->locK_tx_space);

    dev_priv->tx_space = oa_write_space(dev_priv->oa);
    if(dev_priv->tx_space > 0)
        netif_wake_queue(dev_priv->net_priv->netdev);

    spin_unlock(&dev_priv->locK_tx_space);
}

static int mac_phy_init_netdev(struct mac_phy_dev_priv *dev_priv) {
    struct device *dev = &dev_priv->spidev->dev;
    const struct spi_device_id *dev_id = spi_get_device_id(dev_priv->spidev);
    struct mac_phy_net_priv *net_priv;
    struct net_device *netdev;
    const u8 *mac_addr;
    u8 mac[ETH_ALEN];

    netdev = devm_alloc_etherdev(dev, sizeof(*net_priv));
    if (!netdev)
        return -ENOMEM;

    net_priv = netdev_priv(netdev);
    net_priv->netdev = netdev;
    net_priv->dev_priv = dev_priv;
    dev_priv->net_priv = net_priv;
    SET_NETDEV_DEV(netdev, dev);

    mac_addr = device_get_mac_address(dev, mac, ETH_ALEN);
    if (!mac_addr) {
        netdev_err(netdev, "MAC address invalid: %pM\n", mac);
        return -EINVAL;
    }
    ether_addr_copy(netdev->dev_addr, mac);

    netdev->irq = dev_priv->spidev->irq;
    INIT_WORK(&net_priv->tx_work, mac_phy_tx_work);
    skb_queue_head_init(&net_priv->txq);

    netdev->if_port = IF_PORT_10BASET;
    netdev->netdev_ops = &mac_phy_netdev_ops;
    snprintf(net_priv->netdev->name, IFNAMSIZ, "%s-%u", dev_id->name,
             dev_priv->spidev->chip_select);

    dev_priv->oa = oa_init(dev_priv->spidev, netdev);
    if (IS_ERR(dev_priv->oa)) {
        printk(KERN_INFO "open alliance init ERROR\n");
        return -EFAULT;
    }
    oa_set_callback_tx_finished(dev_priv->oa, mac_phy_tx_finished_cb, dev_priv);
    dev_priv->tx_space = oa_write_space(dev_priv->oa);

    return devm_add_action_or_reset(dev, mac_phy_remove_open_alliance, dev_priv->oa);
}

static int mac_phy_register_netdev(struct mac_phy_dev_priv *dev_priv) {
    struct device *dev = &dev_priv->spidev->dev;
    int ret;

    ret = devm_register_netdev(dev, dev_priv->net_priv->netdev);
    if (ret < 0) {
        dev_err(dev, "failed to register network device\n");
        return ret;
    }

    ret = devm_request_threaded_irq(dev, dev_priv->spidev->irq, NULL, mac_phy_irq,
                                     IRQF_TRIGGER_LOW | IRQF_ONESHOT, dev_name(dev), dev_priv);
    if(ret < 0)
        return ret;

    disable_irq(dev_priv->spidev->irq);
    return 0;
}

static int mac_phy_probe(struct spi_device *spi) {
    struct mac_phy_dev_priv *dev_priv;
    int ret;
    OA_DEBUG("MACPHY: probe started");
    dev_priv = devm_kzalloc(&spi->dev, sizeof(struct mac_phy_dev_priv), GFP_KERNEL);
    if (!dev_priv)
        return -ENOMEM;

    dev_priv->spidev = spi;
    spi->bits_per_word = 8;

    spin_lock_init(&dev_priv->locK_tx_space);

    ret = mac_phy_init_netdev(dev_priv);
    if (ret < 0)
        return ret;

    ret = mac_phy_register_netdev(dev_priv);
    OA_DEBUG("MACPHY: probe finished %d", ret)

    return ret;
}

MODULE_DEVICE_TABLE(of, mac_phy_match_table);

static struct spi_driver mac_phy_driver = {
        .driver = {
                .name = "MACPHY",
                .of_match_table = mac_phy_match_table,
        },
        .probe = mac_phy_probe,
        .id_table = mac_phy_spi_id,
};

module_spi_driver(mac_phy_driver);

MODULE_DESCRIPTION("Automotive Ethernet MACPHY Network driver");
MODULE_AUTHOR("Miroslav Chalko <miroslav.chalko@onsemi.com>");
MODULE_LICENSE("Dual BSD/GPL");
