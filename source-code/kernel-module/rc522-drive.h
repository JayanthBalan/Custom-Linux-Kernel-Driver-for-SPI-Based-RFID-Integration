
#ifndef RC522_DRIVE_H
#define RC522_DRIVE_H

#include <linux/types.h>

#ifdef __KERNEL__
    #include <linux/cdev.h>
    #include <linux/mutex.h>
    #include <linux/spi/spi.h>
#endif

#define RC522_DRIVER_NAME "rc522"
#define RC522_DEVICE_NAME "rc522cdev"

#define RC522_MSG_NOT_FOUND "VOID\n"
#define RC522_MSG_UID_FMT "UID:"
#define RC522_MSG_MAX 32

#define RC522_FIFO_SIZE 64
#define RC522_BUF_SIZE (RC522_FIFO_SIZE + 1)
#define RC522_DEFAULT_HZ 1000000
#define RC522_XFER_TIMEOUT_MS 50
#define RC522_CRC_TIMEOUT_MS 20

#define RC522_REG_COMMAND 0x01
#define RC522_REG_COMIRQ 0x04
#define RC522_REG_DIVIRQ 0x05
#define RC522_REG_ERROR 0x06
#define RC522_REG_FIFODATA 0x09
#define RC522_REG_FIFOLEVEL 0x0A
#define RC522_REG_CONTROL 0x0C
#define RC522_REG_BITFRAMING 0x0D
#define RC522_REG_COLL 0x0E
#define RC522_REG_MODE 0x11
#define RC522_REG_TXCONTROL 0x14
#define RC522_REG_TXASK 0x15
#define RC522_REG_CRCRESULT_H 0x21
#define RC522_REG_CRCRESULT_L 0x22
#define RC522_REG_RFCFG 0x26
#define RC522_REG_TMODE 0x2A
#define RC522_REG_TPRESCALER 0x2B
#define RC522_REG_TRELOAD_H 0x2C
#define RC522_REG_TRELOAD_L 0x2D
#define RC522_REG_VERSION 0x37

#define RC522_CMD_IDLE 0x00
#define RC522_CMD_CALCCRC 0x03
#define RC522_CMD_TRANSCEIVE 0x0C
#define RC522_CMD_SOFTRESET 0x0F

#define RC522_CMD_POWERDOWN 0x10
#define RC522_IRQ_TIMER 0x01
#define RC522_IRQ_ERR 0x02
#define RC522_IRQ_IDLE 0x10
#define RC522_IRQ_RX 0x20
#define RC522_DIVIRQ_CRC 0x04
#define RC522_ERR_PROTOCOL 0x01
#define RC522_ERR_PARITY 0x02
#define RC522_ERR_CRC 0x04
#define RC522_ERR_COLL 0x08
#define RC522_ERR_BUFFER_OVFL 0x10
#define RC522_ERR_TEMP 0x40
#define RC522_ERR_WRITE 0x80
#define RC522_FIFO_FLUSH 0x80
#define RC522_BITFRAMING_START 0x80
#define RC522_COLL_VALSAFTERCOLL 0x80
#define RC522_TXCONTROL_ANT_ON 0x03

#define PICC_CMD_WUPA 0x52
#define PICC_CMD_HLTA 0x50
#define PICC_CMD_SEL_CL1 0x93
#define PICC_CMD_SEL_CL2 0x95
#define PICC_CMD_SEL_CL3 0x97
#define PICC_NVB_ANTICOLL 0x20
#define PICC_NVB_SELECT 0x70
#define PICC_CASCADE_TAG 0x88
#define PICC_SAK_CASCADE 0x04
#define PICC_UID_MAX 10

#define RC522_MSG_MIN_READ 25

#ifdef __KERNEL__
    struct rc522_dev {
        struct cdev cdev;
        dev_t devno;
        struct spi_device *spi;
        struct mutex lock;
        u8 *txbuf;
        u8 *rxbuf;
    };
#endif

#endif
