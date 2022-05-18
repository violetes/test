/*======================================================================
    fmvj18x_cs.c 2.8 2002/03/23

    A fmvj18x (and its compatibles) PCMCIA client driver

    Contributed by Shingo Fujimoto, shingo@flab.fujitsu.co.jp

    TDK LAK-CD021 and CONTEC C-NET(PC)C support added by 
    Nobuhiro Katayama, kata-n@po.iijnet.or.jp

    The PCMCIA client code is based on code written by David Hinds.
    Network code is based on the "FMV-18x driver" by Yutaka TAMIYA
    but is actually largely Donald Becker's AT1700 driver, which
    carries the following attribution:

    Written 1993-94 by Donald Becker.

    Copyright 1993 United States Government as represented by the
    Director, National Security Agency.
    
    This software may be used and distributed according to the terms
    of the GNU General Public License, incorporated herein by reference.
    
    The author may be reached as becker@scyld.com, or C/O
    Scyld Computing Corporation
    410 Severn Ave., Suite 210
    Annapolis MD 21403
   
======================================================================*/

#define DRV_NAME	"fmvj18x_cs"
#define DRV_VERSION	"2.8"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/ioport.h>
#include <linux/crc32.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ciscode.h>
#include <pcmcia/ds.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/system.h>

/*====================================================================*/

/* Module parameters */

MODULE_DESCRIPTION("fmvj18x and compatible PCMCIA ethernet driver");
MODULE_LICENSE("GPL");

#define INT_MODULE_PARM(n, v) static int n = v; MODULE_PARM(n, "i")

/* Bit map of interrupts to choose from */
/* This means pick from 15, 14, 12, 11, 10, 9, 7, 5, 4, and 3 */
INT_MODULE_PARM(irq_mask, 0xdeb8);
static int irq_list[4] = { -1 };
MODULE_PARM(irq_list, "1-4i");

/* SRAM configuration */
/* 0:4KB*2 TX buffer   else:8KB*2 TX buffer */
INT_MODULE_PARM(sram_config, 0);

#ifdef PCMCIA_DEBUG
INT_MODULE_PARM(pc_debug, PCMCIA_DEBUG);
#define DEBUG(n, args...) if (pc_debug>(n)) printk(KERN_DEBUG args)
static char *version = DRV_NAME ".c " DRV_VERSION " 2002/03/23";
#else
#define DEBUG(n, args...)
#endif

/*====================================================================*/
/*
    PCMCIA event handlers
 */
static void fmvj18x_config(dev_link_t *link);
static int fmvj18x_get_hwinfo(dev_link_t *link, u_char *node_id);
static int fmvj18x_setup_mfc(dev_link_t *link);
static void fmvj18x_release(dev_link_t *link);
static int fmvj18x_event(event_t event, int priority,
			  event_callback_args_t *args);
static dev_link_t *fmvj18x_attach(void);
static void fmvj18x_detach(dev_link_t *);

/*
    LAN controller(MBH86960A) specific routines
 */
static int fjn_config(struct net_device *dev, struct ifmap *map);
static int fjn_open(struct net_device *dev);
static int fjn_close(struct net_device *dev);
static int fjn_start_xmit(struct sk_buff *skb, struct net_device *dev);
static irqreturn_t fjn_interrupt(int irq, void *dev_id, struct pt_regs *regs);
static void fjn_rx(struct net_device *dev);
static void fjn_reset(struct net_device *dev);
static struct net_device_stats *fjn_get_stats(struct net_device *dev);
static void set_rx_mode(struct net_device *dev);
static void fjn_tx_timeout(struct net_device *dev);
static struct ethtool_ops netdev_ethtool_ops;

static dev_info_t dev_info = "fmvj18x_cs";
static dev_link_t *dev_list;

/*
    card type
 */
typedef enum { MBH10302, MBH10304, TDK, CONTEC, LA501, UNGERMANN, 
	       XXX10304
} cardtype_t;

/*
    driver specific data structure
*/
typedef struct local_info_t {
    dev_link_t link;
    dev_node_t node;
    struct net_device_stats stats;
    long open_time;
    uint tx_started:1;
    uint tx_queue;
    u_short tx_queue_len;
    cardtype_t cardtype;
    u_short sent;
    u_char mc_filter[8];
} local_info_t;

#define MC_FILTERBREAK 64

/*====================================================================*/
/* 
    ioport offset from the base address 
 */
#define TX_STATUS               0 /* transmit status register */
#define RX_STATUS               1 /* receive status register */
#define TX_INTR                 2 /* transmit interrupt mask register */
#define RX_INTR                 3 /* receive interrupt mask register */
#define TX_MODE                 4 /* transmit mode register */
#define RX_MODE                 5 /* receive mode register */
#define CONFIG_0                6 /* configuration register 0 */
#define CONFIG_1                7 /* configuration register 1 */

#define NODE_ID                 8 /* node ID register            (bank 0) */
#define MAR_ADR                 8 /* multicast address registers (bank 1) */

#define DATAPORT                8 /* buffer mem port registers   (bank 2) */
#define TX_START               10 /* transmit start register */
#define COL_CTRL               11 /* 16 collision control register */
#define BMPR12                 12 /* reserved */
#define BMPR13                 13 /* reserved */
#define RX_SKIP                14 /* skip received packet register */

#define LAN_CTRL               16 /* LAN card control register */

#define MAC_ID               0x1a /* hardware address */
#define UNGERMANN_MAC_ID     0x18 /* UNGERMANN-BASS hardware address */

/* 
    control bits 
 */
#define ENA_TMT_OK           0x80
#define ENA_TMT_REC          0x20
#define ENA_COL              0x04
#define ENA_16_COL           0x02
#define ENA_TBUS_ERR         0x01

#define ENA_PKT_RDY          0x80
#define ENA_BUS_ERR          0x40
#define ENA_LEN_ERR          0x08
#define ENA_ALG_ERR          0x04
#define ENA_CRC_ERR          0x02
#define ENA_OVR_FLO          0x01

/* flags */
#define F_TMT_RDY            0x80 /* can accept new packet */
#define F_NET_BSY            0x40 /* carrier is detected */
#define F_TMT_OK             0x20 /* send packet successfully */
#define F_SRT_PKT            0x10 /* short packet error */
#define F_COL_ERR            0x04 /* collision error */
#define F_16_COL             0x02 /* 16 collision error */
#define F_TBUS_ERR           0x01 /* bus read error */

#define F_PKT_RDY            0x80 /* packet(s) in buffer */
#define F_BUS_ERR            0x40 /* bus read error */
#define F_LEN_ERR            0x08 /* short packet */
#define F_ALG_ERR            0x04 /* frame error */
#define F_CRC_ERR            0x02 /* CRC error */
#define F_OVR_FLO            0x01 /* overflow error */

#define F_BUF_EMP            0x40 /* receive buffer is empty */

#define F_SKP_PKT            0x05 /* drop packet in buffer */

/* default bitmaps */
#define D_TX_INTR  ( ENA_TMT_OK )
#define D_RX_INTR  ( ENA_PKT_RDY | ENA_LEN_ERR \
		   | ENA_ALG_ERR | ENA_CRC_ERR | ENA_OVR_FLO )
#define TX_STAT_M  ( F_TMT_RDY )
#define RX_STAT_M  ( F_PKT_RDY | F_LEN_ERR \
                   | F_ALG_ERR | F_CRC_ERR | F_OVR_FLO )

/* commands */
#define D_TX_MODE            0x06 /* no tests, detect carrier */
#define ID_MATCHED           0x02 /* (RX_MODE) */
#define RECV_ALL             0x03 /* (RX_MODE) */
#define CONFIG0_DFL          0x5a /* 16bit bus, 4K x 2 Tx queues */
#define CONFIG0_DFL_1        0x5e /* 16bit bus, 8K x 2 Tx queues */
#define CONFIG0_RST          0xda /* Data Link Controller off (CONFIG_0) */
#define CONFIG0_RST_1        0xde /* Data Link Controller off (CONFIG_0) */
#define BANK_0               0xa0 /* bank 0 (CONFIG_1) */
#define BANK_1               0xa4 /* bank 1 (CONFIG_1) */
#define BANK_2               0xa8 /* bank 2 (CONFIG_1) */
#define CHIP_OFF             0x80 /* contrl chip power off (CONFIG_1) */
#define DO_TX                0x80 /* do transmit packet */
#define SEND_PKT             0x81 /* send a packet */
#define AUTO_MODE            0x07 /* Auto skip packet on 16 col detected */
#define MANU_MODE            0x03 /* Stop and skip packet on 16 col */
#define TDK_AUTO_MODE        0x47 /* Auto skip packet on 16 col detected */
#define TDK_MANU_MODE        0x43 /* Stop and skip packet on 16 col */
#define INTR_OFF             0x0d /* LAN controller ignores interrupts */
#define INTR_ON              0x1d /* LAN controller will catch interrupts */

#define TX_TIMEOUT		((400*HZ)/1000)

#define BANK_0U              0x20 /* bank 0 (CONFIG_1) */
#define BANK_1U              0x24 /* bank 1 (CONFIG_1) */
#define BANK_2U              0x28 /* bank 2 (CONFIG_1) */

static dev_link_t *fmvj18x_attach(void)
{
    local_info_t *lp;
    dev_link_t *link;
    struct net_device *dev;
    client_reg_t client_reg;
    int i, ret;
    
    DEBUG(0, "fmvj18x_attach()\n");

    /* Make up a FMVJ18x specific data structure */
    dev = alloc_etherdev(sizeof(local_info_t));
    if (!dev)
	return NULL;
    lp = dev->priv;
    link = &lp->link;
    link->priv = dev;

    /* The io structure describes IO port mapping */
    link->io.NumPorts1 = 32;
    link->io.Attributes1 = IO_DATA_PATH_WIDTH_AUTO;
    link->io.IOAddrLines = 5;

    /* Interrupt setup */
    link->irq.Attributes = IRQ_TYPE_EXCLUSIVE | IRQ_HANDLE_PRESENT;
    link->irq.IRQInfo1 = IRQ_INFO2_VALID|IRQ_LEVEL_ID;
    if (irq_list[0] == -1)
	link->irq.IRQInfo2 = irq_mask;
    else
	for (i = 0; i < 4; i++)
	    link->irq.IRQInfo2 |= 1 << irq_list[i];
    link->irq.Handler = &fjn_interrupt;
    link->irq.Instance = dev;
    
    /* General socket configuration */
    link->conf.Attributes = CONF_ENABLE_IRQ;
    link->conf.Vcc = 50;
    link->conf.IntType = INT_MEMORY_AND_IO;

    /* The FMVJ18x specific entries in the device structure. */
    SET_MODULE_OWNER(dev);
    dev->hard_start_xmit = &fjn_start_xmit;
    dev->set_config = &fjn_config;
    dev->get_stats = &fjn_get_stats;
    dev->set_multicast_list = &set_rx_mode;
    dev->open = &fjn_open;
    dev->stop = &fjn_close;
#ifdef HAVE_TX_TIMEOUT
    dev->tx_timeout = fjn_tx_timeout;
    dev->watchdog_timeo = TX_TIMEOUT;
#endif
    SET_ETHTOOL_OPS(dev, &netdev_ethtool_ops);
    
    /* Register with Card Services */
    link->next = dev_list;
    dev_list = link;
    client_reg.dev_info = &dev_info;
    client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
    client_reg.EventMask =
	CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
	CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
	CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
    client_reg.event_handler = &fmvj18x_event;
    client_reg.Version = 0x0210;
    client_reg.event_callback_args.client_data = link;
    ret = CardServices(RegisterClient, &link->handle, &client_reg);
    if (ret != 0) {
	cs_error(link->handle, RegisterClient, ret);
	fmvj18x_detach(link);
	return NULL;
    }

    return link;
} /* fmvj18x_attach */

/*====================================================================*/

static void fmvj18x_detach(dev_link_t *link)
{
    struct net_device *dev = link->priv;
    dev_link_t **linkp;
    
    DEBUG(0, "fmvj18x_detach(0x%p)\n", link);
    
    /* Locate device structure */
    for (linkp = &dev_list; *linkp; linkp = &(*linkp)->next)
	if (*linkp == link) break;
    if (*linkp == NULL)
	return;

    if (link->state & DEV_CONFIG) {
	fmvj18x_release(link);
	if (link->state & DEV_STALE_CONFIG)
	    return;
    }

    /* Break the link with Card Services */
    if (link->handle)
	CardServices(DeregisterClient, link->handle);
    
    /* Unlink device structure, free pieces */
    *linkp = link->next;
    if (link->dev) {
	unregister_netdev(dev);
	free_netdev(dev);
    } else
    	kfree(dev);
    
} /* fmvj18x_detach */

/*====================================================================*/

#define CS_CHECK(fn, args...) \
while ((last_ret=CardServices(last_fn=(fn), args))!=0) goto cs_failed

static int mfc_try_io_port(dev_link_t *link)
{
    int i, ret;
    static ioaddr_t serial_base[5] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8, 0x0 };

    for (i = 0; i < 5; i++) {
	link->io.BasePort2 = serial_base[i];
	link->io.Attributes2 = IO_DATA_PATH_WIDTH_8;
	if (link->io.BasePort2 == 0) {
	    link->io.NumPorts2 = 0;
	    printk(KERN_NOTICE "fmvj18x_cs: out of resource for serial\n");
	}
	ret = CardServices(RequestIO, link->handle, &link->io);
	if (ret == CS_SUCCESS) return ret;
    }
    return ret;
}

static int ungermann_try_io_port(dev_link_t *link)
{
    int ret;
    ioaddr_t ioaddr;
    /*
	Ungermann-Bass Access/CARD accepts 0x300,0x320,0x340,0x360
	0x380,0x3c0 only for ioport.
    */
    for (ioaddr = 0x300; ioaddr < 0x3e0; ioaddr += 0x20) {
	link->io.BasePort1 = ioaddr;
	ret = CardServices(RequestIO, link->handle, &link->io);
	if (ret == CS_SUCCESS) {
	    /* calculate ConfigIndex value */
	    link->conf.ConfigIndex = 
		((link->io.BasePort1 & 0x0f0) >> 3) | 0x22;
	    return ret;
	}
    }
    return ret;	/* RequestIO failed */
}

static void fmvj18x_config(dev_link_t *link)
{
    client_handle_t handle = link->handle;
    struct net_device *dev = link->priv;
    local_info_t *lp = dev->priv;
    tuple_t tuple;
    cisparse_t parse;
    u_short buf[32];
    int i, last_fn, last_ret, ret;
    ioaddr_t ioaddr;
    cardtype_t cardtype;
    char *card_name = "unknown";
    u_char *node_id;

    DEBUG(0, "fmvj18x_config(0x%p)\n", link);

    /*
       This reads the card's CONFIG tuple to find its configuration
       registers.
    */
    tuple.DesiredTuple = CISTPL_CONFIG;
    CS_CHECK(GetFirstTuple, handle, &tuple);
    tuple.TupleData = (u_char *)buf;
    tuple.TupleDataMax = 64;
    tuple.TupleOffset = 0;
    CS_CHECK(GetTupleData, handle, &tuple);
    CS_CHECK(ParseTuple, handle, &tuple, &parse);
    
    /* Configure card */
    link->state |= DEV_CONFIG;

    link->conf.ConfigBase = parse.config.base; 
    link->conf.Present = parse.config.rmask[0];

    tuple.DesiredTuple = CISTPL_FUNCE;
    tuple.TupleOffset = 0;
    if (CardServices(GetFirstTuple, handle, &tuple) == CS_SUCCESS) {
	/* Yes, I have CISTPL_FUNCE. Let's check CISTPL_MANFID */
	tuple.DesiredTuple = CISTPL_CFTABLE_ENTRY;
	CS_CHECK(GetFirstTuple, handle, &tuple);
	CS_CHECK(GetTupleData, handle, &tuple);
	CS_CHECK(ParseTuple, handle, &tuple, &parse);
	link->conf.ConfigIndex = parse.cftable_entry.index;
	tuple.DesiredTuple = CISTPL_MANFID;
	if (CardServices(GetFirstTuple, handle, &tuple) == CS_SUCCESS)
	    CS_CHECK(GetTupleData, handle, &tuple);
	else
	    buf[0] = 0xffff;
	switch (le16_to_cpu(buf[0])) {
	case MANFID_TDK:
	    cardtype = TDK;
	    if (le16_to_cpu(buf[1]) == PRODID_TDK_CF010) {
		cs_status_t status;
		CardServices(GetStatus, handle, &status);
		if (status.CardState & CS_EVENT_3VCARD)
		    link->conf.Vcc = 33; /* inserted in 3.3V slot */
	    } else if (le16_to_cpu(buf[1]) == PRODID_TDK_GN3410) {
		/* MultiFunction Card */
		link->conf.ConfigBase = 0x800;
		link->conf.ConfigIndex = 0x47;
		link->io.NumPorts2 = 8;
	    }
	    break;
	case MANFID_CONTEC:
	    cardtype = CONTEC;
	    break;
	case MANFID_FUJITSU:
	    if (le16_to_cpu(buf[1]) == PRODID_FUJITSU_MBH10302)
                /* RATOC REX-5588/9822/4886's PRODID are 0004(=MBH10302),
                   but these are MBH10304 based card. */ 
		cardtype = MBH10304;
	    else if (le16_to_cpu(buf[1]) == PRODID_FUJITSU_MBH10304)
		cardtype = MBH10304;
	    else
		cardtype = LA501;
	    break;
	default:
	    cardtype = MBH10304;
	}
    } else {
	/* old type card */
	tuple.DesiredTuple = CISTPL_MANFID;
	if (CardServices(GetFirstTuple, handle, &tuple) == CS_SUCCESS)
	    CS_CHECK(GetTupleData, handle, &tuple);
	else
	    buf[0] = 0xffff;
	switch (le16_to_cpu(buf[0])) {
	case MANFID_FUJITSU:
	    if (le16_to_cpu(buf[1]) == PRODID_FUJITSU_MBH10304) {
		cardtype = XXX10304;    /* MBH10304 with buggy CIS */
	        link->conf.ConfigIndex = 0x20;
	    } else {
		cardtype = MBH10302;    /* NextCom NC5310, etc. */
		link->conf.ConfigIndex = 1;
	    }
	    break;
	case MANFID_UNGERMANN:
	    cardtype = UNGERMANN;
	    break;
	default:
	    cardtype = MBH10302;
	    link->conf.ConfigIndex = 1;
	}
    }

    if (link->io.NumPorts2 != 0) {
    	link->irq.Attributes =
		IRQ_TYPE_DYNAMIC_SHARING|IRQ_FIRST_SHARED|IRQ_HANDLE_PRESENT;
	ret = mfc_try_io_port(link);
	if (ret != CS_SUCCESS) goto cs_failed;
    } else if (cardtype == UNGERMANN) {
	ret = ungermann_try_io_port(link);
	if (ret != CS_SUCCESS) goto cs_failed;
    } else { 
	CS_CHECK(RequestIO, link->handle, &link->io);
    }
    CS_CHECK(RequestIRQ, link->handle, &link->irq);
    CS_CHECK(RequestConfiguration, link->handle, &link->conf);
    dev->irq = link->irq.AssignedIRQ;
    dev->base_addr = link->io.BasePort1;
    if (register_netdev(dev) != 0) {
	printk(KERN_NOTICE "fmvj18x_cs: register_netdev() failed\n");
	goto failed;
    }

    if (link->io.BasePort2 != 0)
	fmvj18x_setup_mfc(link);

    ioaddr = dev->base_addr;

    /* Reset controller */
    if (sram_config == 0) 
	outb(CONFIG0_RST, ioaddr + CONFIG_0);
    else
	outb(CONFIG0_RST_1, ioaddr + CONFIG_0);

    /* Power On chip and select bank 0 */
    if (cardtype == MBH10302)
	outb(BANK_0, ioaddr + CONFIG_1);
    else
	outb(BANK_0U, ioaddr + CONFIG_1);
    
    /* Set hardware address */
    switch (cardtype) {
    case MBH10304:
    case TDK:
    case LA501:
    case CONTEC:
	tuple.DesiredTuple = CISTPL_FUNCE;
	tuple.TupleOffset = 0;
	CS_CHECK(GetFirstTuple, handle, &tuple);
	tuple.TupleOffset = 0;
	CS_CHECK(GetTupleData, handle, &tuple);
	if (cardtype == MBH10304) {
	    /* MBH10304's CIS_FUNCE is corrupted */
	    node_id = &(tuple.TupleData[5]);
	    card_name = "FMV-J182";
	} else {
	    while (tuple.TupleData[0] != CISTPL_FUNCE_LAN_NODE_ID ) {
		CS_CHECK(GetNextTuple, handle, &tuple) ;
		CS_CHECK(GetTupleData, handle, &tuple) ;
	    }
	    node_id = &(tuple.TupleData[2]);
	    if( cardtype == TDK ) {
		card_name = "TDK LAK-CD021";
	    } else if( cardtype == LA501 ) {
		card_name = "LA501";
	    } else {
		card_name = "C-NET(PC)C";
	    }
	}
	/* Read MACID from CIS */
	for (i = 0; i < 6; i++)
	    dev->dev_addr[i] = node_id[i];
	break;
    case UNGERMANN:
	/* Read MACID from register */
	for (i = 0; i < 6; i++) 
	    dev->dev_addr[i] = inb(ioaddr + UNGERMANN_MAC_ID + i);
	card_name = "Access/CARD";
	break;
    case XXX10304:
	/* Read MACID from Buggy CIS */
	if (fmvj18x_get_hwinfo(link, tuple.TupleData) == -1) {
	    printk(KERN_NOTICE "fmvj18x_cs: unable to read hardware net address.\n");
	    unregister_netdev(dev);
	    goto failed;
	}
	for (i = 0 ; i < 6; i++) {
	    dev->dev_addr[i] = tuple.TupleData[i];
	}
	card_name = "FMV-J182";
	break;
    case MBH10302:
    default:
	/* Read MACID from register */
	for (i = 0; i < 6; i++) 
	    dev->dev_addr[i] = inb(ioaddr + MAC_ID + i);
	card_name = "FMV-J181";
	break;
    }

    strcpy(lp->node.dev_name, dev->name);
    link->dev = &lp->node;

    lp->cardtype = cardtype;
    /* print current configuration */
    printk(KERN_INFO "%s: %s, sram %s, port %#3lx, irq %d, hw_addr ", 
	   dev->name, card_name, sram_config == 0 ? "4K TX*2" : "8K TX*2", 
	   dev->base_addr, dev->irq);
    for (i = 0; i < 6; i++)
	printk("%02X%s", dev->dev_addr[i], ((i<5) ? ":" : "\n"));

    link->state &= ~DEV_CONFIG_PENDING;
    return;
    
cs_failed:
    /* All Card Services errors end up here */
    cs_error(link->handle, last_fn, last_ret);
failed:
    fmvj18x_release(link);
    link->state &= ~DEV_CONFIG_PENDING;

} /* fmvj18x_config */
/*====================================================================*/

static int fmvj18x_get_hwinfo(dev_link_t *link, u_char *node_id)
{
    win_req_t req;
    memreq_t mem;
    u_char *base;
    int i, j;

    /* Allocate a small memory window */
    req.Attributes = WIN_DATA_WIDTH_8|WIN_MEMORY_TYPE_AM|WIN_ENABLE;
    req.Base = 0; req.Size = 0;
    req.AccessSpeed = 0;
    link->win = (window_handle_t)link->handle;
    i = CardServices(RequestWindow, &link->win, &req);
    if (i != CS_SUCCESS) {
	cs_error(link->handle, RequestWindow, i);
	return -1;
    }

    base = ioremap(req.Base, req.Size);
    mem.Page = 0;
    mem.CardOffset = 0;
    CardServices(MapMemPage, link->win, &mem);

    /*
     *  MBH10304 CISTPL_FUNCE_LAN_NODE_ID format
     *  22 0d xx xx xx 04 06 yy yy yy yy yy yy ff
     *  'xx' is garbage.
     *  'yy' is MAC address.
    */ 
    for (i = 0; i < 0x200; i++) {
	if (readb(base+i*2) == 0x22) {	
	    if (readb(base+(i-1)*2) == 0xff
	     && readb(base+(i+5)*2) == 0x04
	     && readb(base+(i+6)*2) == 0x06
	     && readb(base+(i+13)*2) == 0xff) 
		break;
	}
    }

    if (i != 0x200) {
	for (j = 0 ; j < 6; j++,i++) {
	    node_id[j] = readb(base+(i+7)*2);
	}
    }

    iounmap(base);
    j = CardServices(ReleaseWindow, link->win);
    if (j != CS_SUCCESS)
	cs_error(link->handle, ReleaseWindow, j);
    return (i != 0x200) ? 0 : -1;

} /* fmvj18x_get_hwinfo */
/*====================================================================*/

static int fmvj18x_setup_mfc(dev_link_t *link)
{
    win_req_t req;
    memreq_t mem;
    u_char *base;
    int i, j;
    struct net_device *dev = link->priv;
    ioaddr_t ioaddr;

    /* Allocate a small memory window */
    req.Attributes = WIN_DATA_WIDTH_8|WIN_MEMORY_TYPE_AM|WIN_ENABLE;
    req.Base = 0; req.Size = 0;
    req.AccessSpeed = 0;
    link->win = (window_handle_t)link->handle;
    i = CardServices(RequestWindow, &link->win, &req);
    if (i != CS_SUCCESS) {
	cs_error(link->handle, RequestWindow, i);
	return -1;
    }

    base = ioremap(req.Base, req.Size);
    mem.Page = 0;
    mem.CardOffset = 0;
    CardServices(MapMemPage, link->win, &mem);

    ioaddr = dev->base_addr;
    writeb(0x47, base+0x800);	/* Config Option Register of LAN */
    writeb(0x0, base+0x802);	/* Config and Status Register */

    writeb(ioaddr & 0xff, base+0x80a);		/* I/O Base(Low) of LAN */
    writeb((ioaddr >> 8) & 0xff, base+0x80c);	/* I/O Base(High) of LAN */
   
    writeb(0x45, base+0x820);	/* Config Option Register of Modem */
    writeb(0x8, base+0x822);	/* Config and Status Register */

    iounmap(base);
    j = CardServices(ReleaseWindow, link->win);
    if (j != CS_SUCCESS)
	cs_error(link->handle, ReleaseWindow, j);
    return 0;

}
/*====================================================================*/

static void fmvj18x_release(dev_link_t *link)
{

    DEBUG(0, "fmvj18x_release(0x%p)\n", link);

    /*
       If the device is currently in use, we won't release until it
       is actually closed.
    */
    if (link->open) {
	DEBUG(1, "fmvj18x_cs: release postponed, '%s' "
	      "still open\n", link->dev->dev_name);
	link->state |= DEV_STALE_CONFIG;
	return;
    }

    /* Don't bother checking to see if these succeed or not */
    CardServices(ReleaseWindow, link->win);
    CardServices(ReleaseConfiguration, link->handle);
    CardServices(ReleaseIO, link->handle, &link->io);
    CardServices(ReleaseIRQ, link->handle, &link->irq);
    
    link->state &= ~DEV_CONFIG;

    if (link->state & DEV_STALE_CONFIG)
	    fmvj18x_detach(link);
}

/*====================================================================*/

static int fmvj18x_event(event_t event, int priority,
			  event_callback_args_t *args)
{
    dev_link_t *link = args->client_data;
    struct net_device *dev = link->priv;

    DEBUG(1, "fmvj18x_event(0x%06x)\n", event);
    
    switch (event) {
    case CS_EVENT_CARD_REMOVAL:
	link->state &= ~DEV_PRESENT;
	if (link->state & DEV_CONFIG) {
	    netif_device_detach(dev);
	    fmvj18x_release(link);
	}
	break;
    case CS_EVENT_CARD_INSERTION:
	link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
	fmvj18x_config(link);
	break;
    case CS_EVENT_PM_SUSPEND:
	link->state |= DEV_SUSPEND;
	/* Fall through... */
    case CS_EVENT_RESET_PHYSICAL:
	if (link->state & DEV_CONFIG) {
	    if (link->open)
		netif_device_detach(dev);
	    CardServices(ReleaseConfiguration, link->handle);
	}
	break;
    case CS_EVENT_PM_RESUME:
	link->state &= ~DEV_SUSPEND;
	/* Fall through... */
    case CS_EVENT_CARD_RESET:
	if (link->state & DEV_CONFIG) {
	    CardServices(RequestConfiguration, link->handle, &link->conf);
	    if (link->open) {
		fjn_reset(dev);
		netif_device_attach(dev);
	    }
	}
	break;
    }
    return 0;
} /* fmvj18x_event */

static struct pcmcia_driver fmvj18x_cs_driver = {
	.owner		= THIS_MODULE,
	.drv		= {
		.name	= "fmvj18x_cs",
	},
	.attach		= fmvj18x_attach,
	.detach		= fmvj18x_detach,
};

static int __init init_fmvj18x_cs(void)
{
	return pcmcia_register_driver(&fmvj18x_cs_driver);
}

static void __exit exit_fmvj18x_cs(void)
{
	pcmcia_unregister_driver(&fmvj18x_cs_driver);
	while (dev_list != NULL)
		fmvj18x_detach(dev_list);
}

module_init(init_fmvj18x_cs);
module_exit(exit_fmvj18x_cs);

/*====================================================================*/

static irqreturn_t fjn_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    struct net_device *dev = dev_id;
    local_info_t *lp = dev->priv;
    ioaddr_t ioaddr;
    unsigned short tx_stat, rx_stat;

    if (lp == NULL) {
        printk(KERN_NOTICE "fjn_interrupt(): irq %d for "
	       "unknown device.\n", irq);
        return IRQ_NONE;
    }
    ioaddr = dev->base_addr;

    /* avoid multiple interrupts */
    outw(0x0000, ioaddr + TX_INTR);

    /* wait for a while */
    udelay(1);

    /* get status */
    tx_stat = inb(ioaddr + TX_STATUS);
    rx_stat = inb(ioaddr + RX_STATUS);

    /* clear status */
    outb(tx_stat, ioaddr + TX_STATUS);
    outb(rx_stat, ioaddr + RX_STATUS);
    
    DEBUG(4, "%s: interrupt, rx_status %02x.\n", dev->name, rx_stat);
    DEBUG(4, "               tx_status %02x.\n", tx_stat);
    
    if (rx_stat || (inb(ioaddr + RX_MODE) & F_BUF_EMP) == 0) {
	/* there is packet(s) in rx buffer */
	fjn_rx(dev);
    }
    if (tx_stat & F_TMT_RDY) {
	lp->stats.tx_packets += lp->sent ;
        lp->sent = 0 ;
	if (lp->tx_queue) {
	    outb(DO_TX | lp->tx_queue, ioaddr + TX_START);
	    lp->sent = lp->tx_queue ;
	    lp->tx_queue = 0;
	    lp->tx_queue_len = 0;
	    dev->trans_start = jiffies;
	} else {
	    lp->tx_started = 0;
	}
	netif_wake_queue(dev);
    }
    DEBUG(4, "%s: exiting interrupt,\n", dev->name);
    DEBUG(4, "    tx_status %02x, rx_status %02x.\n", tx_stat, rx_stat);

    outb(D_TX_INTR, ioaddr + TX_INTR);
    outb(D_RX_INTR, ioaddr + RX_INTR);
    return IRQ_HANDLED;

} /* fjn_interrupt */

/*====================================================================*/

static void fjn_tx_timeout(struct net_device *dev)
{
    struct local_info_t *lp = (struct local_info_t *)dev->priv;
    ioaddr_t ioaddr = dev->base_addr;

    printk(KERN_NOTICE "%s: transmit timed out with status %04x, %s?\n",
	   dev->name, htons(inw(ioaddr + TX_STATUS)),
	   inb(ioaddr + TX_STATUS) & F_TMT_RDY
	   ? "IRQ conflict" : "network cable problem");
    printk(KERN_NOTICE "%s: timeout registers: %04x %04x %04x "
	   "%04x %04x %04x %04x %04x.\n",
	   dev->name, htons(inw(ioaddr + 0)),
	   htons(inw(ioaddr + 2)), htons(inw(ioaddr + 4)),
	   htons(inw(ioaddr + 6)), htons(inw(ioaddr + 8)),
	   htons(inw(ioaddr +10)), htons(inw(ioaddr +12)),
	   htons(inw(ioaddr +14)));
    lp->stats.tx_errors++;
    /* ToDo: We should try to restart the adaptor... */
    local_irq_disable();
    fjn_reset(dev);

    lp->tx_started = 0;
    lp->tx_queue = 0;
    lp->tx_queue_len = 0;
    lp->sent = 0;
    lp->open_time = jiffies;
    local_irq_enable();
    netif_wake_queue(dev);
}

static int fjn_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct local_info_t *lp = (struct local_info_t *)dev->priv;
    ioaddr_t ioaddr = dev->base_addr;
    short length = skb->len;
    
    if (length < ETH_ZLEN)
    {
    	skb = skb_padto(skb, ETH_ZLEN);
    	if (skb == NULL)
    		return 0;
    	length = ETH_ZLEN;
    }

    netif_stop_queue(dev);

    {
	unsigned char *buf = skb->data;

	if (length > ETH_FRAME_LEN) {
	    printk(KERN_NOTICE "%s: Attempting to send a large packet"
		   " (%d bytes).\n", dev->name, length);
	    return 1;
	}

	DEBUG(4, "%s: Transmitting a packet of length %lu.\n",
	      dev->name, (unsigned long)skb->len);
	lp->stats.tx_bytes += skb->len;

	/* Disable both interrupts. */
	outw(0x0000, ioaddr + TX_INTR);

	/* wait for a while */
	udelay(1);

	outw(length, ioaddr + DATAPORT);
	outsw(ioaddr + DATAPORT, buf, (length + 1) >> 1);

	lp->tx_queue++;
	lp->tx_queue_len += ((length+3) & ~1);

	if (lp->tx_started == 0) {
	    /* If the Tx is idle, always trigger a transmit. */
	    outb(DO_TX | lp->tx_queue, ioaddr + TX_START);
	    lp->sent = lp->tx_queue ;
	    lp->tx_queue = 0;
	    lp->tx_queue_len = 0;
	    dev->trans_start = jiffies;
	    lp->tx_started = 1;
	    netif_start_queue(dev);
	} else {
	    if( sram_config == 0 ) {
		if (lp->tx_queue_len < (4096 - (ETH_FRAME_LEN +2)) )
		    /* Yes, there is room for one more packet. */
		    netif_start_queue(dev);
	    } else {
		if (lp->tx_queue_len < (8192 - (ETH_FRAME_LEN +2)) && 
						lp->tx_queue < 127 )
		    /* Yes, there is room for one more packet. */
		    netif_start_queue(dev);
	    }
	}

	/* Re-enable interrupts */
	outb(D_TX_INTR, ioaddr + TX_INTR);
	outb(D_RX_INTR, ioaddr + RX_INTR);
    }
    dev_kfree_skb (skb);

    return 0;
} /* fjn_start_xmit */

/*====================================================================*/

static void fjn_reset(struct net_device *dev)
{
    struct local_info_t *lp = (struct local_info_t *)dev->priv;
    ioaddr_t ioaddr = dev->base_addr;
    int i;

    DEBUG(4, "fjn_reset(%s) called.\n",dev->name);

    /* Reset controller */
    if( sram_config == 0 ) 
	outb(CONFIG0_RST, ioaddr + CONFIG_0);
    else
	outb(CONFIG0_RST_1, ioaddr + CONFIG_0);

    /* Power On chip and select bank 0 */
    if (lp->cardtype == MBH10302)
	outb(BANK_0, ioaddr + CONFIG_1);
    else
	outb(BANK_0U, ioaddr + CONFIG_1);

    /* Set Tx modes */
    outb(D_TX_MODE, ioaddr + TX_MODE);
    /* set Rx modes */
    outb(ID_MATCHED, ioaddr + RX_MODE);

    /* Set hardware address */
    for (i = 0; i < 6; i++) 
        outb(dev->dev_addr[i], ioaddr + NODE_ID + i);

    /* Switch to bank 1 */
    if (lp->cardtype == MBH10302)
	outb(BANK_1, ioaddr + CONFIG_1);
    else
	outb(BANK_1U, ioaddr + CONFIG_1);

    /* set the multicast table to accept none. */
    for (i = 0; i < 6; i++) 
        outb(0x00, ioaddr + MAR_ADR + i);

    /* Switch to bank 2 (runtime mode) */
    if (lp->cardtype == MBH10302)
	outb(BANK_2, ioaddr + CONFIG_1);
    else
	outb(BANK_2U, ioaddr + CONFIG_1);

    /* set 16col ctrl bits */
    if( lp->cardtype == TDK || lp->cardtype == CONTEC) 
        outb(TDK_AUTO_MODE, ioaddr + COL_CTRL);
    else
        outb(AUTO_MODE, ioaddr + COL_CTRL);

    /* clear Reserved Regs */
    outb(0x00, ioaddr + BMPR12);
    outb(0x00, ioaddr + BMPR13);

    /* reset Skip packet reg. */
    outb(0x01, ioaddr + RX_SKIP);

    /* Enable Tx and Rx */
    if( sram_config == 0 )
	outb(CONFIG0_DFL, ioaddr + CONFIG_0);
    else
	outb(CONFIG0_DFL_1, ioaddr + CONFIG_0);

    /* Init receive pointer ? */
    inw(ioaddr + DATAPORT);
    inw(ioaddr + DATAPORT);

    /* Clear all status */
    outb(0xff, ioaddr + TX_STATUS);
    outb(0xff, ioaddr + RX_STATUS);

    if (lp->cardtype == MBH10302)
	outb(INTR_OFF, ioaddr + LAN_CTRL);

    /* Turn on Rx interrupts */
    outb(D_TX_INTR, ioaddr + TX_INTR);
    outb(D_RX_INTR, ioaddr + RX_INTR);

    /* Turn on interrupts from LAN card controller */
    if (lp->cardtype == MBH10302)
	outb(INTR_ON, ioaddr + LAN_CTRL);
} /* fjn_reset */

/*====================================================================*/

static void fjn_rx(struct net_device *dev)
{
    struct local_info_t *lp = (struct local_info_t *)dev->priv;
    ioaddr_t ioaddr = dev->base_addr;
    int boguscount = 10;	/* 5 -> 10: by agy 19940922 */

    DEBUG(4, "%s: in rx_packet(), rx_status %02x.\n",
	  dev->name, inb(ioaddr + RX_STATUS));

    while ((inb(ioaddr + RX_MODE) & F_BUF_EMP) == 0) {
	u_short status = inw(ioaddr + DATAPORT);

	DEBUG(4, "%s: Rxing packet mode %02x status %04x.\n",
	      dev->name, inb(ioaddr + RX_MODE), status);
#ifndef final_version
	if (status == 0) {
	    outb(F_SKP_PKT, ioaddr + RX_SKIP);
	    break;
	}
#endif
	if ((status & 0xF0) != 0x20) {	/* There was an error. */
	    lp->stats.rx_errors++;
	    if (status & F_LEN_ERR) lp->stats.rx_length_errors++;
	    if (status & F_ALG_ERR) lp->stats.rx_frame_errors++;
	    if (status & F_CRC_ERR) lp->stats.rx_crc_errors++;
	    if (status & F_OVR_FLO) lp->stats.rx_over_errors++;
	} else {
	    u_short pkt_len = inw(ioaddr + DATAPORT);
	    /* Malloc up new buffer. */
	    struct sk_buff *skb;

	    if (pkt_len > 1550) {
		printk(KERN_NOTICE "%s: The FMV-18x claimed a very "
		       "large packet, size %d.\n", dev->name, pkt_len);
		outb(F_SKP_PKT, ioaddr + RX_SKIP);
		lp->stats.rx_errors++;
		break;
	    }
	    skb = dev_alloc_skb(pkt_len+2);
	    if (skb == NULL) {
		printk(KERN_NOTICE "%s: Memory squeeze, dropping "
		       "packet (len %d).\n", dev->name, pkt_len);
		outb(F_SKP_PKT, ioaddr + RX_SKIP);
		lp->stats.rx_dropped++;
		break;
	    }
	    skb->dev = dev;

	    skb_reserve(skb, 2);
	    insw(ioaddr + DATAPORT, skb_put(skb, pkt_len),
		 (pkt_len + 1) >> 1);
	    skb->protocol = eth_type_trans(skb, dev);

#ifdef PCMCIA_DEBUG
	    if (pc_debug > 5) {
		int i;
		printk(KERN_DEBUG "%s: Rxed packet of length %d: ",
		       dev->name, pkt_len);
		for (i = 0; i < 14; i++)
		    printk(" %02x", skb->data[i]);
		printk(".\n");
	    }
#endif

	    netif_rx(skb);
	    dev->last_rx = jiffies;
	    lp->stats.rx_packets++;
	    lp->stats.rx_bytes += pkt_len;
	}
	if (--boguscount <= 0)
	    break;
    }

    /* If any worth-while packets have been received, dev_rint()
	   has done a netif_wake_queue() for us and will work on them
	   when we get to the bottom-half routine. */
/*
    if (lp->cardtype != TDK) {
	int i;
	for (i = 0; i < 20; i++) {
	    if ((inb(ioaddr + RX_MODE) & F_BUF_EMP) == F_BUF_EMP)
		break;
	    (void)inw(ioaddr + DATAPORT);  /+ dummy status read +/
	    outb(F_SKP_PKT, ioaddr + RX_SKIP);
	}

	if (i > 0)
	    DEBUG(5, "%s: Exint Rx packet with mode %02x after "
		  "%d ticks.\n", dev->name, inb(ioaddr + RX_MODE), i);
    }
*/

    return;
} /* fjn_rx */

/*====================================================================*/

static void netdev_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *info)
{
	strcpy(info->driver, DRV_NAME);
	strcpy(info->version, DRV_VERSION);
	sprintf(info->bus_info, "PCMCIA 0x%lx", dev->base_addr);
}

#ifdef PCMCIA_DEBUG
static u32 netdev_get_msglevel(struct net_device *dev)
{
	return pc_debug;
}

static void netdev_set_msglevel(struct net_device *dev, u32 level)
{
	pc_debug = level;
}
#endif /* PCMCIA_DEBUG */

static struct ethtool_ops netdev_ethtool_ops = {
	.get_drvinfo		= netdev_get_drvinfo,
#ifdef PCMCIA_DEBUG
	.get_msglevel		= netdev_get_msglevel,
	.set_msglevel		= netdev_set_msglevel,
#endif /* PCMCIA_DEBUG */
};

static int fjn_config(struct net_device *dev, struct ifmap *map){
    return 0;
}

static int fjn_open(struct net_device *dev)
{
    struct local_info_t *lp = (struct local_info_t *)dev->priv;
    dev_link_t *link = &lp->link;

    DEBUG(4, "fjn_open('%s').\n", dev->name);

    if (!DEV_OK(link))
	return -ENODEV;
    
    link->open++;
    
    fjn_reset(dev);
    
    lp->tx_started = 0;
    lp->tx_queue = 0;
    lp->tx_queue_len = 0;
    lp->open_time = jiffies;
    netif_start_queue(dev);
    
    return 0;
} /* fjn_open */

/*====================================================================*/

static int fjn_close(struct net_device *dev)
{
    struct local_info_t *lp = (struct local_info_t *)dev->priv;
    dev_link_t *link = &lp->link;
    ioaddr_t ioaddr = dev->base_addr;

    DEBUG(4, "fjn_close('%s').\n", dev->name);

    lp->open_time = 0;
    netif_stop_queue(dev);

    /* Set configuration register 0 to disable Tx and Rx. */
    if( sram_config == 0 ) 
	outb(CONFIG0_RST ,ioaddr + CONFIG_0);
    else
	outb(CONFIG0_RST_1 ,ioaddr + CONFIG_0);

    /* Update the statistics -- ToDo. */

    /* Power-down the chip.  Green, green, green! */
    outb(CHIP_OFF ,ioaddr + CONFIG_1);

    /* Set the ethernet adaptor disable IRQ */
    if (lp->cardtype == MBH10302)
	outb(INTR_OFF, ioaddr + LAN_CTRL);

    link->open--;
    if (link->state & DEV_STALE_CONFIG)
	    fmvj18x_release(link);

    return 0;
} /* fjn_close */

/*====================================================================*/

static struct net_device_stats *fjn_get_stats(struct net_device *dev)
{
    local_info_t *lp = (local_info_t *)dev->priv;
    return &lp->stats;
} /* fjn_get_stats */

/*====================================================================*/

/*
  Set the multicast/promiscuous mode for this adaptor.
*/

static void set_rx_mode(struct net_device *dev)
{
    ioaddr_t ioaddr = dev->base_addr;
    struct local_info_t *lp = (struct local_info_t *)dev->priv;
    u_char mc_filter[8];		 /* Multicast hash filter */
    u_long flags;
    int i;
    
    if (dev->flags & IFF_PROMISC) {
	/* Unconditionally log net taps. */
	printk("%s: Promiscuous mode enabled.\n", dev->name);
	memset(mc_filter, 0xff, sizeof(mc_filter));
	outb(3, ioaddr + RX_MODE);	/* Enable promiscuous mode */
    } else if (dev->mc_count > MC_FILTERBREAK
	       ||  (dev->flags & IFF_ALLMULTI)) {
	/* Too many to filter perfectly -- accept all multicasts. */
	memset(mc_filter, 0xff, sizeof(mc_filter));
	outb(2, ioaddr + RX_MODE);	/* Use normal mode. */
    } else if (dev->mc_count == 0) {
	memset(mc_filter, 0x00, sizeof(mc_filter));
	outb(1, ioaddr + RX_MODE);	/* Ignore almost all multicasts. */
    } else {
	struct dev_mc_list *mclist;
	int i;
	
	memset(mc_filter, 0, sizeof(mc_filter));
	for (i = 0, mclist = dev->mc_list; mclist && i < dev->mc_count;
	     i++, mclist = mclist->next) {
	    unsigned int bit =
	    	ether_crc_le(ETH_ALEN, mclist->dmi_addr) & 0x3f;
	    mc_filter[bit >> 3] |= (1 << bit);
	}
    }

    local_irq_save(flags); 
    if (memcmp(mc_filter, lp->mc_filter, sizeof(mc_filter))) {
	int saved_bank = inb(ioaddr + CONFIG_1);
	/* Switch to bank 1 and set the multicast table. */
	outb(0xe4, ioaddr + CONFIG_1);
	for (i = 0; i < 8; i++)
	    outb(mc_filter[i], ioaddr + 8 + i);
	memcpy(lp->mc_filter, mc_filter, sizeof(mc_filter));
	outb(saved_bank, ioaddr + CONFIG_1);
    }
    local_irq_restore(flags);
}
