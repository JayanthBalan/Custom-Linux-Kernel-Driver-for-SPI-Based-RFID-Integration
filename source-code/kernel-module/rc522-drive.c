
#include "rc522-drive.h"

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/version.h>
#include <linux/errno.h>

MODULE_AUTHOR("Jayanth Balan");
MODULE_DESCRIPTION("MFRC522 RFID reader SPI character driver");
MODULE_LICENSE("GPL");

static int rc522_major;
static int rc522_minor;
static struct class *rc522_class;
static struct rc522_dev rc522_device;

#define RC522_TRY(expr) do { \
    int __ret = (expr); \
    if (__ret) return __ret; \
} while (0)

static u8 rc522_read_address(u8 reg) {
    return 0x80 | ((reg << 1) & 0x7e);
}

static u8 rc522_write_address(u8 reg) {
    return (reg << 1) & 0x7e;
}

static int rc522_read_reg(struct rc522_dev *dev, u8 reg, u8 *value) {
    struct spi_transfer xfer = {.tx_buf = dev->txbuf, .rx_buf = dev->rxbuf, .len = 2};
    int ret;

    dev->txbuf[0] = rc522_read_address(reg);
    dev->txbuf[1] = 0x00;

    ret = spi_sync_transfer(dev->spi, &xfer, 1);
    if (ret) {
        return ret;
    }
    
    *value = dev->rxbuf[1];
    return 0;
}

static int rc522_write_reg(struct rc522_dev *dev, u8 reg, u8 value) {
    dev->txbuf[0] = rc522_write_address(reg);
    dev->txbuf[1] = value;
    return spi_write(dev->spi, dev->txbuf, 2);
}

static int rc522_set_bits(struct rc522_dev *dev, u8 reg, u8 mask) {
    u8 value;

    RC522_TRY(rc522_read_reg(dev, reg, &value));
    return rc522_write_reg(dev, reg, value | mask);
}

static int rc522_clear_bits(struct rc522_dev *dev, u8 reg, u8 mask) {
    u8 value;

    RC522_TRY(rc522_read_reg(dev, reg, &value));
    return rc522_write_reg(dev, reg, value & ~mask);
}

static int rc522_write_fifo(struct rc522_dev *dev, const u8 *data, size_t len) {
    if (len > RC522_FIFO_SIZE) {
        return -EMSGSIZE;
    }

    dev->txbuf[0] = rc522_write_address(RC522_REG_FIFODATA);
    if (len) {
        memcpy(&dev->txbuf[1], data, len);
    }

    return spi_write(dev->spi, dev->txbuf, len + 1);
}

static int rc522_read_fifo(struct rc522_dev *dev, u8 *data, size_t len) {
    struct spi_transfer xfer = {.tx_buf = dev->txbuf, .rx_buf = dev->rxbuf};
    int ret;

    if (!len || len > RC522_FIFO_SIZE) {
        return -EINVAL;
    }

    dev->txbuf[0] = rc522_read_address(RC522_REG_FIFODATA);
    {
        size_t i;
        for (i = 1; i < len; i++) {
            dev->txbuf[i] = dev->txbuf[0];
        }
    }
    dev->txbuf[len] = 0x00;
    xfer.len = len + 1;

    ret = spi_sync_transfer(dev->spi, &xfer, 1);
    if (ret) {
        return ret;
    }

    memcpy(data, &dev->rxbuf[1], len);
    return 0;
}

static int rc522_flush_fifo(struct rc522_dev *dev) {
    return rc522_write_reg(dev, RC522_REG_FIFOLEVEL, RC522_FIFO_FLUSH);
}

static int rc522_calc_crc(struct rc522_dev *dev, const u8 *data, size_t len, u8 out[2]) {
    unsigned long deadline;
    u8 irq = 0;
    int ret;

    if (len > RC522_FIFO_SIZE){
        return -EMSGSIZE;
    }

    ret = rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_IDLE);
    if (ret) {
        return ret;
    }
    ret = rc522_write_reg(dev, RC522_REG_DIVIRQ, RC522_DIVIRQ_CRC);
    if (ret) {
        return ret;
    }
    ret = rc522_flush_fifo(dev);
    if (ret) {
        return ret;
    }
    ret = rc522_write_fifo(dev, data, len);
    if (ret) {
        return ret;
    }
    ret = rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_CALCCRC);
    if (ret) {
        goto out_idle;
    }

    deadline = jiffies + msecs_to_jiffies(RC522_CRC_TIMEOUT_MS);
    for (;;) {
        ret = rc522_read_reg(dev, RC522_REG_DIVIRQ, &irq);
        if (ret) {
            goto out_idle;
        }
        if (irq & RC522_DIVIRQ_CRC) {
            break;
        }
        if (time_after_eq(jiffies, deadline)) {
            ret = -ETIMEDOUT;
            goto out_idle;
        }
        usleep_range(50, 150);
    }

    ret = rc522_read_reg(dev, RC522_REG_CRCRESULT_L, &out[0]);
    if (ret) {
        goto out_idle;
    }
    ret = rc522_read_reg(dev, RC522_REG_CRCRESULT_H, &out[1]);

out_idle:
    {
        int idle_ret = rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_IDLE);
        if (!ret) {
            ret = idle_ret;
        }
    }
    return ret;
}

static int rc522_transceive(struct rc522_dev *dev, const u8 *tx, size_t txlen, u8 tx_last_bits, u8 *rx, size_t *rxlen, u8 *rx_last_bits) {
    unsigned long deadline;
    u8 irq = 0, error = 0, fifo_level = 0, control = 0;
    size_t capacity = *rxlen;
    int ret;

    if (!txlen || txlen > RC522_FIFO_SIZE || capacity > RC522_FIFO_SIZE) {
        return -EINVAL;
    }

    ret = rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_IDLE);
    if (ret) {
        return ret;
    }
    ret = rc522_write_reg(dev, RC522_REG_COMIRQ, 0x7f);
    if (ret) {
        goto out_idle;
    }
    ret = rc522_flush_fifo(dev);
    if (ret) {
        goto out_idle;
    }
    ret = rc522_write_fifo(dev, tx, txlen);
    if (ret) {
        goto out_idle;
    }
    ret = rc522_write_reg(dev, RC522_REG_BITFRAMING, tx_last_bits & 0x07);
    if (ret) {
        goto out_idle;
    }
    ret = rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_TRANSCEIVE);
    if (ret) {
        goto out_idle;
    }
    ret = rc522_write_reg(dev, RC522_REG_BITFRAMING, RC522_BITFRAMING_START | (tx_last_bits & 0x07));
    if (ret) {
        goto out_idle;
    }

    deadline = jiffies + msecs_to_jiffies(RC522_XFER_TIMEOUT_MS);
    for (;;) {
        ret = rc522_read_reg(dev, RC522_REG_COMIRQ, &irq);
        if (ret) {
            goto out_idle;
        }
        if (irq & (RC522_IRQ_RX | RC522_IRQ_IDLE | RC522_IRQ_TIMER | RC522_IRQ_ERR)) {
            break;
        }
        if (time_after_eq(jiffies, deadline)) {
            ret = -ETIMEDOUT;
            goto out_idle;
        }
        usleep_range(100, 300);
    }

    if ((irq & RC522_IRQ_TIMER) && !(irq & RC522_IRQ_RX)) {
        ret = -ETIMEDOUT;
        goto out_idle;
    }

    ret = rc522_read_reg(dev, RC522_REG_ERROR, &error);
    if (ret) {
        goto out_idle;
    }

    if (error & RC522_ERR_COLL) {
        ret = -EAGAIN;
        goto out_idle;
    }

    if (error & RC522_ERR_CRC) {
        ret = -EBADMSG;
        goto out_idle;
    }

    if (error & (RC522_ERR_PROTOCOL | RC522_ERR_PARITY | RC522_ERR_BUFFER_OVFL | RC522_ERR_TEMP | RC522_ERR_WRITE)) {
        ret = -EPROTO;
        goto out_idle;
    }

    ret = rc522_read_reg(dev, RC522_REG_FIFOLEVEL, &fifo_level);
    if (ret) {
        goto out_idle;
    }
    fifo_level &= 0x7f;

    if (fifo_level > capacity) {
        ret = -EPROTO;
        goto out_idle;
    }
    if (fifo_level && !rx) {
        ret = -EINVAL;
        goto out_idle;
    }
    if (fifo_level) {
        ret = rc522_read_fifo(dev, rx, fifo_level);
        if (ret) {
            goto out_idle;
        }
    }

    ret = rc522_read_reg(dev, RC522_REG_CONTROL, &control);
    if (ret) {
        goto out_idle;
    }

    *rxlen = fifo_level;
    *rx_last_bits = control & 0x07;
    ret = 0;

out_idle:
    rc522_write_reg(dev, RC522_REG_BITFRAMING, 0x00);
    rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_IDLE);
    return ret;
}

static int rc522_wakeup_a(struct rc522_dev *dev) {
    const u8 command = PICC_CMD_WUPA;
    u8 response[2], last_bits = 0;
    size_t response_len = sizeof(response);
    int ret;

    ret = rc522_clear_bits(dev, RC522_REG_COLL, RC522_COLL_VALSAFTERCOLL);
    if (ret) {
        return ret;
    }

    ret = rc522_transceive(dev, &command, 1, 7, response, &response_len, &last_bits);
    if (ret == -ETIMEDOUT) {
        return -EAGAIN;
    }
    if (ret) {
        return ret;
    }

    if (response_len != 2 || last_bits != 0) {
        return -EPROTO;
    }

    return 0;
}

static int rc522_select_cascade(struct rc522_dev *dev, u8 sel, u8 block[5], u8 *sak) {
    u8 request[2] = { sel, PICC_NVB_ANTICOLL };
    u8 response[5], last_bits = 0;
    u8 select_frame[9], crc[2], sak_response[3];
    size_t response_len, sak_len;
    int ret;

    response_len = sizeof(response);
    ret = rc522_transceive(dev, request, sizeof(request), 0, response, &response_len, &last_bits);
    if (ret) {
        return ret;
    }
    if (response_len != 5 || last_bits != 0) {
        return -EPROTO;
    }

    if ((u8)(response[0] ^ response[1] ^ response[2] ^ response[3]) != response[4]) {
        return -EBADMSG;
    }

    memcpy(block, response, sizeof(response));

    select_frame[0] = sel;
    select_frame[1] = PICC_NVB_SELECT;
    memcpy(&select_frame[2], block, 5);

    ret = rc522_calc_crc(dev, select_frame, 7, crc);
    if (ret) {
        return ret;
    }
    select_frame[7] = crc[0];
    select_frame[8] = crc[1];

    sak_len = sizeof(sak_response);
    ret = rc522_transceive(dev, select_frame, sizeof(select_frame), 0, sak_response, &sak_len, &last_bits);
    if (ret) {
        return ret;
    }
    if (sak_len != sizeof(sak_response) || last_bits != 0) {
        return -EPROTO;
    }

    ret = rc522_calc_crc(dev, sak_response, 1, crc);
    if (ret) {
        return ret;
    }
    if (sak_response[1] != crc[0] || sak_response[2] != crc[1]) {
        return -EBADMSG;
    }

    *sak = sak_response[0];
    return 0;
}

static int rc522_halt_a(struct rc522_dev *dev) {
    u8 frame[4] = { PICC_CMD_HLTA, 0x00, 0, 0 };
    u8 crc[2], response[2], last_bits = 0;
    size_t response_len = sizeof(response);
    int ret;

    ret = rc522_calc_crc(dev, frame, 2, crc);
    if (ret) {
        return ret;
    }
    frame[2] = crc[0];
    frame[3] = crc[1];

    ret = rc522_transceive(dev, frame, sizeof(frame), 0, response, &response_len, &last_bits);
    if (ret == -ETIMEDOUT || ret == -EPROTO || ret == -EAGAIN || ret == -EBADMSG) {
        return 0;
    }
    return ret;
}

static int rc522_get_uid(struct rc522_dev *dev, u8 uid[PICC_UID_MAX], u8 *uid_len) {
    static const u8 select_codes[] = {PICC_CMD_SEL_CL1, PICC_CMD_SEL_CL2, PICC_CMD_SEL_CL3};
    u8 block[5], sak = 0;
    size_t level;
    int ret, halt_ret;
    bool wakeup_succeeded = false;

    *uid_len = 0;
    memset(uid, 0, PICC_UID_MAX);

    ret = rc522_wakeup_a(dev);
    if (ret) {
        return ret;
    }
    wakeup_succeeded = true;

    for (level = 0; level < ARRAY_SIZE(select_codes); level++) {
        bool cascade;

        ret = rc522_select_cascade(dev, select_codes[level], block, &sak);
        if (ret) {
            goto out_halt;
        }

        cascade = (block[0] == PICC_CASCADE_TAG);
        if (cascade) {
            if (level == 2 || !(sak & PICC_SAK_CASCADE)) {
                ret = -EPROTO;
                goto out_halt;
            }
            memcpy(&uid[*uid_len], &block[1], 3);
            *uid_len += 3;
        }
        else {
            if (sak & PICC_SAK_CASCADE) {
                ret = -EPROTO;
                goto out_halt;
            }
            memcpy(&uid[*uid_len], &block[0], 4);
            *uid_len += 4;
            break;
        }
    }

    if (*uid_len != 4 && *uid_len != 7 && *uid_len != 10) {
        ret = -EPROTO;
        goto out_halt;
    }

    ret = 0;

out_halt:
    if (wakeup_succeeded) {
        halt_ret = rc522_halt_a(dev);
        if (!ret && halt_ret && halt_ret != -ETIMEDOUT) {
            ret = halt_ret;
        }
    }
    if (ret) {
        *uid_len = 0;
    }
    return ret;
}

static int rc522_hw_init(struct rc522_dev *dev) {
    u8 version = 0;
    int i, ret;

    ret = rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_SOFTRESET);
    if (ret) {
        return ret;
    }
    msleep(50);

    for (i = 0; i < 10; i++) {
        ret = rc522_read_reg(dev, RC522_REG_COMMAND, &version);
        if (ret) {
            return ret;
        }
        if (!(version & RC522_CMD_POWERDOWN)) {
            break;
        }
        msleep(10);
    }
    if (version & RC522_CMD_POWERDOWN) {
        return -ETIMEDOUT;
    }

    ret = rc522_read_reg(dev, RC522_REG_VERSION, &version);
    if (ret) {
        return ret;
    }
    dev_info(&dev->spi->dev, "MFRC522 VersionReg=0x%02x\n", version);
    if (version == 0x00 || version == 0xff) {
        dev_err(&dev->spi->dev, "invalid VersionReg 0x%02x; check power, wiring and pinmux\n", version);
        return -ENODEV;
    }

    ret = rc522_write_reg(dev, RC522_REG_TMODE, 0x80);
    if (ret) {
        return ret;
    }
    ret = rc522_write_reg(dev, RC522_REG_TPRESCALER, 0xa9);
    if (ret) {
        return ret;
    }
    ret = rc522_write_reg(dev, RC522_REG_TRELOAD_H, 0x03);
    if (ret) {
        return ret;
    }
    ret = rc522_write_reg(dev, RC522_REG_TRELOAD_L, 0xe8);
    if (ret) {
        return ret;
    }
    ret = rc522_write_reg(dev, RC522_REG_TXASK, 0x40);
    if (ret) {
        return ret;
    }
    ret = rc522_write_reg(dev, RC522_REG_MODE, 0x3d);
    if (ret) {
        return ret;
    }
    ret = rc522_write_reg(dev, RC522_REG_RFCFG, 0x70);
    if (ret) {
        return ret;
    }

    return rc522_set_bits(dev, RC522_REG_TXCONTROL, RC522_TXCONTROL_ANT_ON);
}

static int rc522_open(struct inode *inode, struct file *filp) {
    struct rc522_dev *dev = container_of(inode->i_cdev, struct rc522_dev, cdev);

    filp->private_data = dev;
    return nonseekable_open(inode, filp);
}

static int rc522_release(struct inode *inode, struct file *filp) {
    return 0;
}

static ssize_t rc522_read(struct file *filp, char __user *user_buf, size_t count, loff_t *f_pos) {
    struct rc522_dev *dev = filp->private_data;
    char message[RC522_MSG_MAX];
    u8 uid[PICC_UID_MAX];
    u8 uid_len = 0;
    size_t message_len;
    int ret, i;

    if (!count) {
        return 0;
    }

    if (count < RC522_MSG_MIN_READ) {
        return -EINVAL;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    if (!dev->spi) {
        ret = -ENODEV;
        goto unlock;
    }

    ret = rc522_get_uid(dev, uid, &uid_len);
    mutex_unlock(&dev->lock);

    if (ret == -EAGAIN || ret == -ETIMEDOUT || ret == -EPROTO || ret == -EBADMSG) {
        message_len = scnprintf(message, sizeof(message), "%s", RC522_MSG_NOT_FOUND);
    }
    else if (ret) {
        return ret;
    }
    else {
        message_len = scnprintf(message, sizeof(message), "%s", RC522_MSG_UID_FMT);
        for (i = 0; i < uid_len; i++) {
            message_len += scnprintf(message + message_len, sizeof(message) - message_len, "%02X", uid[i]);
        }
        message_len += scnprintf(message + message_len, sizeof(message) - message_len, "\n");
    }

    if (copy_to_user(user_buf, message, message_len)) {
        return -EFAULT;
    }

    return message_len;

unlock:
    mutex_unlock(&dev->lock);
    return ret;
}

static const struct file_operations rc522_fops = {
    .owner = THIS_MODULE,
    .open = rc522_open,
    .release = rc522_release,
    .read = rc522_read,
    .llseek = no_llseek,
};

static int rc522_probe(struct spi_device *spi) {
    struct rc522_dev *dev = &rc522_device;
    struct gpio_desc *reset_gpio;
    struct device *device;
    int ret;

    mutex_lock(&dev->lock);
    if (dev->spi) {
        mutex_unlock(&dev->lock);
        return -EBUSY;
    }

    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    if (!spi->max_speed_hz) {
        spi->max_speed_hz = RC522_DEFAULT_HZ;
    }

    ret = spi_setup(spi);
    if (ret) {
        goto err_unlock;
    }

    dev->txbuf = devm_kzalloc(&spi->dev, RC522_BUF_SIZE, GFP_KERNEL);
    dev->rxbuf = devm_kzalloc(&spi->dev, RC522_BUF_SIZE, GFP_KERNEL);
    if (!dev->txbuf || !dev->rxbuf) {
        ret = -ENOMEM;
        goto err_unlock;
    }

    reset_gpio = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(reset_gpio)) {
        ret = PTR_ERR(reset_gpio);
        goto err_buffers;
    }
    if (reset_gpio) {
        usleep_range(1000, 2000);
        gpiod_set_value_cansleep(reset_gpio, 0);
        msleep(50);
    }

    dev->spi = spi;
    spi_set_drvdata(spi, dev);

    ret = rc522_hw_init(dev);
    if (ret) {
        goto err_spi;
    }

    dev->devno = MKDEV(rc522_major, rc522_minor);
    cdev_init(&dev->cdev, &rc522_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devno, 1);
    if (ret) {
        goto err_spi;
    }

    device = device_create(rc522_class, &spi->dev, dev->devno, NULL, RC522_DEVICE_NAME);
    if (IS_ERR(device)) {
        ret = PTR_ERR(device);
        cdev_del(&dev->cdev);
        goto err_spi;
    }

    mutex_unlock(&dev->lock);
    dev_info(&spi->dev, "registered /dev/%s\n", RC522_DEVICE_NAME);
    return 0;

err_spi:
    spi_set_drvdata(spi, NULL);
    dev->spi = NULL;
err_buffers:
    dev->txbuf = NULL;
    dev->rxbuf = NULL;
err_unlock:
    mutex_unlock(&dev->lock);
    return ret;
}

static int rc522_remove(struct spi_device *spi) {
    struct rc522_dev *dev = spi_get_drvdata(spi);

    device_destroy(rc522_class, dev->devno);
    cdev_del(&dev->cdev);

    mutex_lock(&dev->lock);
    if (dev->spi) {
        rc522_clear_bits(dev, RC522_REG_TXCONTROL, RC522_TXCONTROL_ANT_ON);
        dev->spi = NULL;
    }
    dev->txbuf = NULL;
    dev->rxbuf = NULL;
    spi_set_drvdata(spi, NULL);
    mutex_unlock(&dev->lock);

    return 0;
}

static const struct of_device_id rc522_of_match[] = {
    { .compatible = "jayanth,mfrc522" },
    { }
};
MODULE_DEVICE_TABLE(of, rc522_of_match);

static const struct spi_device_id rc522_spi_ids[] = {
    { "mfrc522", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, rc522_spi_ids);

static struct spi_driver rc522_spi_driver = {
    .driver = {
        .name = RC522_DRIVER_NAME,
        .of_match_table = rc522_of_match,
    },
    .id_table = rc522_spi_ids,
    .probe = rc522_probe,
    .remove = rc522_remove,
};

static int __init rc522_init_module(void) {
    dev_t devno;
    int ret;

    memset(&rc522_device, 0, sizeof(rc522_device));
    mutex_init(&rc522_device.lock);

    ret = alloc_chrdev_region(&devno, 0, 1, RC522_DRIVER_NAME);
    if (ret) {
        return ret;
    }
    rc522_major = MAJOR(devno);
    rc522_minor = MINOR(devno);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    rc522_class = class_create(RC522_DRIVER_NAME);
#else
    rc522_class = class_create(THIS_MODULE, RC522_DRIVER_NAME);
#endif
    if (IS_ERR(rc522_class)) {
        ret = PTR_ERR(rc522_class);
        unregister_chrdev_region(devno, 1);
        return ret;
    }

    ret = spi_register_driver(&rc522_spi_driver);
    if (ret) {
        class_destroy(rc522_class);
        unregister_chrdev_region(devno, 1);
        return ret;
    }

    return 0;
}

static void __exit rc522_cleanup_module(void) {
    dev_t devno = MKDEV(rc522_major, rc522_minor);

    spi_unregister_driver(&rc522_spi_driver);
    class_destroy(rc522_class);
    unregister_chrdev_region(devno, 1);
}

module_init(rc522_init_module);
module_exit(rc522_cleanup_module);
