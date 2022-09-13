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

#define REG_PLCAC0		        0xca01
#define REG_PLCAC0_MMS	        0x4
#define REG_PLCAC0_RST			BIT(14)	// PLCA reset
#define REG_PLCAC0_ENA			BIT(15)	// PLCA enable

#define REG_PLCAC1		        0xca02
#define REG_PLCAC1_MMS	        0x4
#define REG_PLCAC1_NODE_ID	    GENMASK(7,0)
#define REG_PLCAC1_NODE_COUNT	GENMASK(15,8)

#define MACPHY_DEV_MAC_ADDR_SLOT   0
#define MACPHY_BROADCAST_MAC_ADDR_SLOT   1
#define MACPHY_MULTICAST_MAC_ADDR_SLOT   2

static const struct of_device_id macphy_match_table[] = {
        {.compatible = "ncv,ncv7410"},
        {}
};

static const struct spi_device_id macphy_spi_id[] = {
        {.name = "ncv7410"},
        {}
};

struct macphy_dev_conf{
    bool plca_enabled;
    u32 plca_id;
    u32 plca_node_count;
};

struct macphy_priv {
    struct spi_device *spidev;
    struct net_device *netdev;
    struct open_alliance *oa;
    struct macphy_dev_conf conf;
    struct work_struct tx_work;
    struct work_struct set_rx_mode_work;
    struct sk_buff_head txq;
    spinlock_t lock_tx_work;
    spinlock_t lock_set_rx_mode_work;
    u32 flags;
    u32 tx_space;
};

static int macphy_read_config(struct macphy_priv* priv){
    int ret;
    struct fwnode_handle *fwnode = dev_fwnode(&priv->spidev->dev);
	priv->conf.plca_enabled = fwnode_property_present(fwnode, "plca-enabled");
    if(priv->conf.plca_enabled){
        ret = fwnode_property_read_u32(fwnode, "plca-id",&priv->conf.plca_id);
        if(ret < 0)
            return ret;
        ret = fwnode_property_read_u32(fwnode, "plca-node-count", &priv->conf.plca_node_count);
        if(ret < 0)
            return ret;
    }
    return 0;
}

static irqreturn_t macphy_irq(int irq, void *p) {
    struct macphy_priv *priv = p;

    oa_process_interrupt(priv->oa);

    return IRQ_HANDLED;
}

static int macphy_write_mac_address(struct open_alliance* oa, int mac_nr, u8 *addr,
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

static int macphy_set_mac_address(struct net_device *netdev, void *addr) {
    struct macphy_priv *priv = netdev_priv(netdev);
    struct sockaddr *sa = addr;
    u8 mask[ETH_ALEN];

    if (netif_running(netdev))
        return -EBUSY;

    if (!is_valid_ether_addr(sa->sa_data))
        return -EADDRNOTAVAIL;

    ether_addr_copy(netdev->dev_addr, sa->sa_data);
    memset(mask, 0xFF, ETH_ALEN);

    return macphy_write_mac_address(priv->oa, MACPHY_DEV_MAC_ADDR_SLOT, netdev->dev_addr, mask);
}

static int macphy_init_mac(struct macphy_priv *priv) {
    struct net_device *netdev = priv->netdev;
    u8 mask[ETH_ALEN];
    u8 mac[ETH_ALEN];
    int ret;

    memset(mask, 0xFF, ETH_ALEN);
    memset(mac, 0xFF, ETH_ALEN);

    ret = macphy_write_mac_address(priv->oa, MACPHY_DEV_MAC_ADDR_SLOT, netdev->dev_addr, mask);
    if (ret < 0) {
        netdev_err(netdev, "Could not set MAC address: %pM, %d\n", netdev->dev_addr, ret);
        return ret;
    }

    ret = macphy_write_mac_address(priv->oa, MACPHY_BROADCAST_MAC_ADDR_SLOT, mac, mask);
    if (ret < 0) {
        netdev_err(netdev, "Could not set Broadcast MAC address: %d\n", ret);
        return ret;
    }

    return 0;
}

static int macphy_configure_phy(void * context){
    struct macphy_priv *priv = context;
    int ret;
    u32 val;

    // configure mac address
    ret = macphy_init_mac(priv);

    if (ret < 0)
        return ret;
    // enable communication, filter adresses and append crc
    val = REG_CONTROL0_TXEN | REG_CONTROL0_RXEN |
          REG_CONTROL0_FCSA | REG_CONTROL0_ADRF;
    ret = oa_write_reg(priv->oa, REG_CONTROL0_MMS, REG_CONTROL0, val);
    if (ret < 0)
        return ret;
    // unjabber timer and noise immunity enhancement
    val = REG_TWEAKS1_ENIE | REG_TWEAKS1_UNJT;
    ret = oa_reg_set_bits(priv->oa, REG_TWEAKS1_MMS, REG_TWEAKS1, val, val);
    if (ret < 0)
        return ret;
    // enable link
    val = REG_PHYCON_LINKCTRL;
    ret = oa_write_reg(priv->oa, REG_PHYCON_MMS, REG_PHYCON, val);
    if (ret < 0)
        return ret;
    // use plca
    if(priv->conf.plca_enabled){
        val = REG_PLCAC0_ENA;
        ret = oa_write_reg(priv->oa, REG_PLCAC0_MMS, REG_PLCAC0, val);
        if (ret < 0)
            return ret;

        val = FIELD_PREP(REG_PLCAC1_NODE_COUNT, priv->conf.plca_node_count) |
              FIELD_PREP(REG_PLCAC1_NODE_ID, priv->conf.plca_id);
        ret = oa_write_reg(priv->oa, REG_PLCAC1_MMS, REG_PLCAC1, val);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int macphy_net_open(struct net_device *net_dev) {
    struct macphy_priv *priv = netdev_priv(net_dev);
    int ret;

    ret = oa_net_open(priv->oa, macphy_configure_phy, priv);
    if (ret < 0)
        return ret;
    enable_irq(priv->spidev->irq);
    netif_start_queue(net_dev);

    return 0;
}

static int macphy_net_stop(struct net_device *net_dev) {
    struct macphy_priv *priv = netdev_priv(net_dev);
    
    netif_stop_queue(priv->netdev);
    flush_work(&priv->tx_work);
    flush_work(&priv->set_rx_mode_work);
    disable_irq(priv->spidev->irq);
    return oa_net_stop(priv->oa);
}

static void macphy_tx_work(struct work_struct *work) {
    struct macphy_priv *priv = container_of(work, struct macphy_priv, tx_work);
    struct sk_buff *skb;
    int ret;

    while (!skb_queue_empty(&priv->txq)) {
        skb = skb_dequeue(&priv->txq);

        if (skb) {
            ret = oa_write(priv->oa, skb);
            if (ret < 0)
                netdev_err(priv->netdev, "MACPHY: Frame write error %d\n", ret);

            dev_kfree_skb(skb);
        }
    }
}

static netdev_tx_t macphy_start_xmit(struct sk_buff *skb, struct net_device *dev) {
    struct macphy_priv *priv = netdev_priv(dev);
    netdev_tx_t netdev_ret = NETDEV_TX_OK;
    u32 tx_space_needed = skb->len;

    spin_lock(&priv->lock_tx_work);
    if (tx_space_needed > priv->tx_space) {
        netif_stop_queue(dev);
        netdev_ret = NETDEV_TX_BUSY;
    } else {
        priv->tx_space -= tx_space_needed;
        skb_queue_tail(&priv->txq, skb);
    }
    spin_unlock(&priv->lock_tx_work);

    schedule_work(&priv->tx_work);

    return netdev_ret;
}

static void macphy_set_rx_mode_work(struct work_struct *work)
{
    struct macphy_priv *priv = container_of(work, struct macphy_priv, set_rx_mode_work);
    u32 mask;

    mask = REG_CONTROL0_ADRF;
    oa_reg_set_bits(priv->oa, REG_CONTROL0_MMS, REG_CONTROL0, mask,
                      (priv->flags & IFF_PROMISC) ? mask : 0);
}


static void macphy_set_rx_mode(struct net_device *dev)
{
    struct macphy_priv *priv = netdev_priv(dev);

    spin_lock(&priv->lock_set_rx_mode_work);

    priv->flags = dev->flags;
    schedule_work(&priv->set_rx_mode_work);

    spin_unlock(&priv->lock_set_rx_mode_work);
}

static const struct net_device_ops macphy_netdev_ops = {
        .ndo_open        = macphy_net_open,
        .ndo_stop        = macphy_net_stop,
        .ndo_start_xmit        = macphy_start_xmit,
        .ndo_set_mac_address    = macphy_set_mac_address,
        .ndo_set_rx_mode	= macphy_set_rx_mode,
        .ndo_validate_addr    = eth_validate_addr,
};

static void macphy_tx_finished_cb(void* context){
    struct macphy_priv *priv = context;

    spin_lock(&priv->lock_tx_work);

    priv->tx_space = oa_write_space(priv->oa);
    if(priv->tx_space > 0)
        netif_wake_queue(priv->netdev);

    spin_unlock(&priv->lock_tx_work);
}

static int macphy_probe(struct spi_device *spi) {
    struct macphy_priv *priv;
    struct net_device *net;
    u8 mac[ETH_ALEN];
    int ret;
    
    net = alloc_etherdev(sizeof(struct macphy_priv));
    if (!net){
        ret = -ENOMEM;
        goto error_return;
    }
    
    priv = netdev_priv(net);
    spi_set_drvdata(spi, priv);	
    
    spi->bits_per_word = 8;
    priv->netdev = net;
    priv->spidev = spi;
    spin_lock_init(&priv->lock_tx_work);
    spin_lock_init(&priv->lock_set_rx_mode_work);
    INIT_WORK(&priv->tx_work, macphy_tx_work);
    INIT_WORK(&priv->set_rx_mode_work, macphy_set_rx_mode_work);
    skb_queue_head_init(&priv->txq);
    
    net->irq = spi->irq;
    net->if_port = IF_PORT_10BASET;
    net->netdev_ops = &macphy_netdev_ops;
    SET_NETDEV_DEV(net, &spi->dev);
    
    // read configuration
    ret = macphy_read_config(priv);
    if (ret < 0)
        goto error_free_net;
    // configure interface mac address and name
    if(!device_get_mac_address(&spi->dev, mac, ETH_ALEN)){
        ret = -EINVAL;
        goto error_free_net;
    }
    ether_addr_copy(net->dev_addr, mac);
    snprintf(net->name, IFNAMSIZ, "%s-%u", spi_get_device_id(spi)->name,
             spi->chip_select);
    // configure open alliance module
    priv->oa = oa_init(spi, net);
    if (IS_ERR(priv->oa)){
        ret = -EFAULT;
        goto error_free_net;
    }
    oa_set_callback_tx_finished(priv->oa, macphy_tx_finished_cb, priv);
    priv->tx_space = oa_write_space(priv->oa);
    // register interrupt and net dev
    ret = register_netdev(net);
    if (ret < 0) 
        goto error_free_oa;
    
    ret = request_threaded_irq( spi->irq, NULL, macphy_irq, IRQF_TRIGGER_LOW | IRQF_ONESHOT, dev_name(&spi->dev), priv);
    if(ret < 0)
        goto error_unregister_net;

    disable_irq(spi->irq);
    return 0;
error_unregister_net:
	unregister_netdev(net);
error_free_oa:
    oa_delete(priv->oa);
error_free_net:
    free_netdev(net);
error_return:
    return ret;
}

static int macphy_remove(struct spi_device *spi){
	struct macphy_priv *priv = spi_get_drvdata(spi);

	free_irq(spi->irq, priv);
    unregister_netdev(priv->netdev);
	oa_delete(priv->oa);
	free_netdev(priv->netdev);

	return 0;
}


MODULE_DEVICE_TABLE(of, macphy_match_table);

static struct spi_driver macphy_driver = {
        .driver = {
                .name = "MACPHY",
                .of_match_table = macphy_match_table,
        },
        .probe = macphy_probe,
        .remove = macphy_remove,
        .id_table = macphy_spi_id,
};

module_spi_driver(macphy_driver);

MODULE_DESCRIPTION("Automotive Ethernet MACPHY Network driver");
MODULE_AUTHOR("Miroslav Chalko <miroslav.chalko@onsemi.com>");
MODULE_LICENSE("Dual BSD/GPL");
