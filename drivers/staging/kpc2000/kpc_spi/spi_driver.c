// SPDX-License-Identifier: GPL-2.0+
/*
 * KP2000 SPI controller driver
 *
 * Copyright (C) 2014-2018 Daktronics
 * Author: Matt Sickler <matt.sickler@daktronics.com>
 * Very loosely based on spi-omap2-mcspi.c
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/gcd.h>
#include <linux/spi/spi.h>
#include <linux/spi/flash.h>
#include <linux/mtd/partitions.h>

#include "../kpc.h"
#include "spi_parts.h"


/***************
 * SPI Defines *
 ***************/
#define KP_SPI_REG_CONFIG 0x0 /* 0x00 */
#define KP_SPI_REG_STATUS 0x1 /* 0x08 */
#define KP_SPI_REG_FFCTRL 0x2 /* 0x10 */
#define KP_SPI_REG_TXDATA 0x3 /* 0x18 */
#define KP_SPI_REG_RXDATA 0x4 /* 0x20 */

#define KP_SPI_CLK           48000000
#define KP_SPI_MAX_FIFODEPTH 64
#define KP_SPI_MAX_FIFOWCNT  0xFFFF

#define KP_SPI_REG_CONFIG_TRM_TXRX 0
#define KP_SPI_REG_CONFIG_TRM_RX   1
#define KP_SPI_REG_CONFIG_TRM_TX   2

#define KP_SPI_REG_STATUS_RXS   0x01
#define KP_SPI_REG_STATUS_TXS   0x02
#define KP_SPI_REG_STATUS_EOT   0x04
#define KP_SPI_REG_STATUS_TXFFE 0x10
#define KP_SPI_REG_STATUS_TXFFF 0x20
#define KP_SPI_REG_STATUS_RXFFE 0x40
#define KP_SPI_REG_STATUS_RXFFF 0x80



/******************
 * SPI Structures *
 ******************/
struct kp_spi {
	struct spi_master  *master;
	u64 __iomem        *base;
	unsigned long       phys;
	struct device      *dev;
	int                 fifo_depth;
	unsigned int        pin_dir:1;
};


struct kp_spi_controller_state {
    void __iomem   *base;
    unsigned long   phys;
    unsigned char   chip_select;
    int             word_len;
    s64             conf_cache;
};


union kp_spi_config {
    /* use this to access individual elements */
    struct __attribute__((packed)) spi_config_bitfield {
        unsigned int pha       : 1; /* spim_clk Phase      */
        unsigned int pol       : 1; /* spim_clk Polarity   */
        unsigned int epol      : 1; /* spim_csx Polarity   */
        unsigned int dpe       : 1; /* Transmission Enable */
        unsigned int wl        : 5; /* Word Length         */
        unsigned int           : 3;
        unsigned int trm       : 2; /* TxRx Mode           */
        unsigned int cs        : 4; /* Chip Select         */
        unsigned int wcnt      : 7; /* Word Count          */
        unsigned int ffen      : 1; /* FIFO Enable         */
        unsigned int spi_en    : 1; /* SPI Enable          */
        unsigned int           : 5;
    } bitfield;
    /* use this to grab the whole register */
    u32 reg;
};



union kp_spi_status {
    struct __attribute__((packed)) spi_status_bitfield {
        unsigned int rx    :  1; /* Rx Status       */
        unsigned int tx    :  1; /* Tx Status       */
        unsigned int eo    :  1; /* End of Transfer */
        unsigned int       :  1;
        unsigned int txffe :  1; /* Tx FIFO Empty   */
        unsigned int txfff :  1; /* Tx FIFO Full    */
        unsigned int rxffe :  1; /* Rx FIFO Empty   */
        unsigned int rxfff :  1; /* Rx FIFO Full    */
        unsigned int       : 24;
    } bitfield;
    u32 reg;
};



union kp_spi_ffctrl {
    struct __attribute__((packed)) spi_ffctrl_bitfield {
        unsigned int ffstart :  1; /* FIFO Start */
        unsigned int         : 31;
    } bitfield;
    u32 reg;
};



/***************
 * SPI Helpers *
 ***************/
static inline int
kp_spi_bytes_per_word(int word_len)
{
    if (word_len <= 8){
        return 1;
    }
    else if (word_len <= 16) {
        return 2;
    }
    else { /* word_len <= 32 */
        return 4;
    }
}

static inline u64
kp_spi_read_reg(struct kp_spi_controller_state *cs, int idx)
{
    u64 __iomem *addr = cs->base;
    u64 val;

    addr += idx;
    if ((idx == KP_SPI_REG_CONFIG) && (cs->conf_cache >= 0)){
        return cs->conf_cache;
    }
    val = readq((void*)addr);
    return val;
}

static inline void
kp_spi_write_reg(struct kp_spi_controller_state *cs, int idx, u64 val)
{
    u64 __iomem *addr = cs->base;
    addr += idx;
    writeq(val, (void*)addr);
    if (idx == KP_SPI_REG_CONFIG)
        cs->conf_cache = val;
}

static int
kp_spi_wait_for_reg_bit(struct kp_spi_controller_state *cs, int idx, unsigned long bit)
{
    unsigned long timeout;
    timeout = jiffies + msecs_to_jiffies(1000);
    while (!(kp_spi_read_reg(cs, idx) & bit)) {
        if (time_after(jiffies, timeout)) {
            if (!(kp_spi_read_reg(cs, idx) & bit)) {
                return -ETIMEDOUT;
            } else {
                return 0;
            }
        }
        cpu_relax();
    }
    return 0;
}

static unsigned
kp_spi_txrx_pio(struct spi_device *spidev, struct spi_transfer *transfer)
{
    struct kp_spi_controller_state *cs = spidev->controller_state;
    unsigned int count = transfer->len;
    unsigned int c = count;
    
    int i;
    u8 *rx       = transfer->rx_buf;
    const u8 *tx = transfer->tx_buf;
    int processed = 0;
    
    if (tx) {
        for (i = 0 ; i < c ; i++) {
            char val = *tx++;
            
            if (kp_spi_wait_for_reg_bit(cs, KP_SPI_REG_STATUS, KP_SPI_REG_STATUS_TXS) < 0) {
                goto out;
            }
            
            kp_spi_write_reg(cs, KP_SPI_REG_TXDATA, val);
            processed++;
        }
    }
    else if(rx) {
        for (i = 0 ; i < c ; i++) {
            char test=0;
            
            kp_spi_write_reg(cs, KP_SPI_REG_TXDATA, 0x00);
            
            if (kp_spi_wait_for_reg_bit(cs, KP_SPI_REG_STATUS, KP_SPI_REG_STATUS_RXS) < 0) {
                goto out;
            }
            
            test = kp_spi_read_reg(cs, KP_SPI_REG_RXDATA);
            *rx++ = test;
            processed++;
        }
    }
    
    if (kp_spi_wait_for_reg_bit(cs, KP_SPI_REG_STATUS, KP_SPI_REG_STATUS_EOT) < 0) {
        //TODO: Figure out how to abort transaction??  This has never happened in practice though...
    }
    
 out:
    return processed;
}

/*****************
 * SPI Functions *
 *****************/
static int
kp_spi_setup(struct spi_device *spidev)
{
    union kp_spi_config sc;
    struct kp_spi *kpspi = spi_master_get_devdata(spidev->master);
    struct kp_spi_controller_state *cs;
    
    /* setup controller state */
    cs = spidev->controller_state;
    if (!cs) {
        cs = kzalloc(sizeof(*cs), GFP_KERNEL);
        if(!cs) {
            return -ENOMEM;
        }
        cs->base = kpspi->base;
        cs->phys = kpspi->phys;
        cs->chip_select = spidev->chip_select;
        cs->word_len = spidev->bits_per_word;
        cs->conf_cache = -1;
        spidev->controller_state = cs;
    }
    
    /* set config register */
    sc.bitfield.wl = spidev->bits_per_word - 1;
    sc.bitfield.cs = spidev->chip_select;
    sc.bitfield.spi_en = 0;
    sc.bitfield.trm = 0;
    sc.bitfield.ffen = 0;
    kp_spi_write_reg(spidev->controller_state, KP_SPI_REG_CONFIG, sc.reg);
    return 0;
}

static int
kp_spi_transfer_one_message(struct spi_master *master, struct spi_message *m)
{
    struct kp_spi_controller_state *cs;
    struct spi_device   *spidev;
    struct kp_spi       *kpspi;
    struct spi_transfer *transfer;
    union kp_spi_config sc;
    int status = 0;
    
    spidev = m->spi;
    kpspi = spi_master_get_devdata(master);
    m->actual_length = 0;
    m->status = 0;
    
    cs = spidev->controller_state;
    
    /* reject invalid messages and transfers */
    if (list_empty(&m->transfers)) {
        return -EINVAL;
    }
    
    /* validate input */
    list_for_each_entry(transfer, &m->transfers, transfer_list) {
        const void *tx_buf = transfer->tx_buf;
        void       *rx_buf = transfer->rx_buf;
        unsigned    len = transfer->len;
        
        if (transfer->speed_hz > KP_SPI_CLK || (len && !(rx_buf || tx_buf))) {
            dev_dbg(kpspi->dev, "  transfer: %d Hz, %d %s%s, %d bpw\n",
                    transfer->speed_hz,
                    len,
                    tx_buf ? "tx" : "",
                    rx_buf ? "rx" : "",
                    transfer->bits_per_word);
            dev_dbg(kpspi->dev, "  transfer -EINVAL\n");
            return -EINVAL;
        }
        if (transfer->speed_hz && (transfer->speed_hz < (KP_SPI_CLK >> 15))) {
            dev_dbg(kpspi->dev, "speed_hz %d below minimum %d Hz\n",
                    transfer->speed_hz,
                    KP_SPI_CLK >> 15);
            dev_dbg(kpspi->dev, "  speed_hz -EINVAL\n");
            return -EINVAL;
        }
    }
    
    /* assert chip select to start the sequence*/
    sc.reg = kp_spi_read_reg(cs, KP_SPI_REG_CONFIG);
    sc.bitfield.spi_en = 1;
    kp_spi_write_reg(cs, KP_SPI_REG_CONFIG, sc.reg);
    
    /* work */
    if (kp_spi_wait_for_reg_bit(cs, KP_SPI_REG_STATUS, KP_SPI_REG_STATUS_EOT) < 0) {
        dev_info(kpspi->dev, "EOT timed out\n");
        goto out;
    }
    
    /* do the transfers for this message */
    list_for_each_entry(transfer, &m->transfers, transfer_list) {
        if (transfer->tx_buf == NULL && transfer->rx_buf == NULL && transfer->len) {
            status = -EINVAL;
            break;
        }
        
        /* transfer */
        if (transfer->len) {
            unsigned int word_len = spidev->bits_per_word;
            unsigned count;
            
            /* set up the transfer... */
            sc.reg = kp_spi_read_reg(cs, KP_SPI_REG_CONFIG);
            
            /* ...direction */
            if (transfer->tx_buf) {
                sc.bitfield.trm = KP_SPI_REG_CONFIG_TRM_TX;
            }
            else if (transfer->rx_buf) {
                sc.bitfield.trm = KP_SPI_REG_CONFIG_TRM_RX;
            }
            
            /* ...word length */
            if (transfer->bits_per_word) {
                word_len = transfer->bits_per_word;
            }
            cs->word_len = word_len;
            sc.bitfield.wl = word_len-1;
            
            /* ...chip select */
            sc.bitfield.cs = spidev->chip_select;
            
            /* ...and write the new settings */
            kp_spi_write_reg(cs, KP_SPI_REG_CONFIG, sc.reg);
            
            /* do the transfer */
            count = kp_spi_txrx_pio(spidev, transfer);
            m->actual_length += count;
            
            if (count != transfer->len) {
                status = -EIO;
                break;
            }
        }
        
        if (transfer->delay_usecs) {
            udelay(transfer->delay_usecs);
        }
    }
    
    /* de-assert chip select to end the sequence */
    sc.reg = kp_spi_read_reg(cs, KP_SPI_REG_CONFIG);
    sc.bitfield.spi_en = 0;
    kp_spi_write_reg(cs, KP_SPI_REG_CONFIG, sc.reg);
    
 out:
    /* done work */
    spi_finalize_current_message(master);
    return 0;
}

static void
kp_spi_cleanup(struct spi_device *spidev)
{
    struct kp_spi_controller_state *cs = spidev->controller_state;
    if (cs) {
        kfree(cs);
    }
}



/******************
 * Probe / Remove *
 ******************/
static int
kp_spi_probe(struct platform_device *pldev)
{
    struct kpc_core_device_platdata *drvdata;
    struct spi_master *master;
    struct kp_spi *kpspi;
    struct resource *r;
    int status = 0;
    int i;

    drvdata = pldev->dev.platform_data;
    if (!drvdata){
        dev_err(&pldev->dev, "kp_spi_probe: platform_data is NULL!\n");
        return -ENODEV;
    }
    
    master = spi_alloc_master(&pldev->dev, sizeof(struct kp_spi));
    if (master == NULL) {
        dev_err(&pldev->dev, "kp_spi_probe: master allocation failed\n");
        return -ENOMEM;
    }
    
    /* set up the spi functions */
    master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;
    master->bits_per_word_mask = (unsigned int)SPI_BPW_RANGE_MASK(4, 32);
    master->setup = kp_spi_setup;
    master->transfer_one_message = kp_spi_transfer_one_message;
    master->cleanup = kp_spi_cleanup;
    
    platform_set_drvdata(pldev, master);
    
    kpspi = spi_master_get_devdata(master);
    kpspi->master = master;
    kpspi->dev = &pldev->dev;
    
    master->num_chipselect = 4;
    if (pldev->id != -1) {
        master->bus_num = pldev->id;
    }
    kpspi->pin_dir = 0;
    
    r = platform_get_resource(pldev, IORESOURCE_MEM, 0);
    if (r == NULL) {
        dev_err(&pldev->dev, "kp_spi_probe: Unable to get platform resources\n");
        status = -ENODEV;
        goto free_master;
    }
    
    kpspi->phys = (unsigned long)ioremap_nocache(r->start, resource_size(r));
    kpspi->base = (u64 __iomem *)kpspi->phys;
    
    status = spi_register_master(master);
    if (status < 0) {
        dev_err(&pldev->dev, "Unable to register SPI device\n");
        goto free_master;
    }
    
    /* register the slave boards */
    #define NEW_SPI_DEVICE_FROM_BOARD_INFO_TABLE(table) \
        for (i = 0 ; i < ARRAY_SIZE(table) ; i++) { \
            spi_new_device(master, &(table[i])); \
        }
    
    switch ((drvdata->card_id & 0xFFFF0000) >> 16){
        case PCI_DEVICE_ID_DAKTRONICS_KADOKA_P2KR0:
            NEW_SPI_DEVICE_FROM_BOARD_INFO_TABLE(p2kr0_board_info);
            break;
        default:
            dev_err(&pldev->dev, "Unknown hardware, cant know what partition table to use!\n");
            goto free_master;
            break;
    }
    
    return status;

 free_master:
    spi_master_put(master);
    return status;
}

static int
kp_spi_remove(struct platform_device *pldev)
{
    struct spi_master * master = platform_get_drvdata(pldev);
    spi_unregister_master(master);
    return 0;
}


static struct platform_driver kp_spi_driver = {
    .driver = {
        .name =     KP_DRIVER_NAME_SPI,
    },
    .probe =    kp_spi_probe,
    .remove =   kp_spi_remove,
};

module_platform_driver(kp_spi_driver);
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:kp_spi");
