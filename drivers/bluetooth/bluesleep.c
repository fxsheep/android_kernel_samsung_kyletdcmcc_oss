/*

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2 as
   published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.


   Copyright (C) 2006-2007 - Motorola
   Copyright (c) 2008-2009, Code Aurora Forum. All rights reserved.

   Date         Author           Comment
   -----------  --------------   --------------------------------
   2006-Apr-28  Motorola         The kernel module for running the Bluetooth(R)
                                 Sleep-Mode Protocol from the Host side
   2006-Sep-08  Motorola         Added workqueue for handling sleep work.
   2007-Jan-24  Motorola         Added mbm_handle_ioi() call to ISR.
   2009-Aug-10  Motorola         Changed "add_timer" to "mod_timer" to solve
                                 race when flurry of queued work comes in.
*/

#include <linux/module.h>       /* kernel module definitions */
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>

#include <linux/irq.h>
#include <linux/param.h>
#include <linux/bitops.h>
#include <linux/termios.h>
#include <linux/wakelock.h>
#include <mach/gpio.h>

#include <linux/serial_core.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h> /* event notifications */
#include "hci_uart.h"

#define BT_SLEEP_DBG(fmt,args...)   printk(KERN_DEBUG "bluesleep: " fmt, ##args);
//#define BT_SLEEP_DBG(fmt,args...) { printk("%s, %s : ", __FILE__ , __FUNCTION__ ); printk(fmt, ## args);}
//#define BT_SLEEP_DBG pr_debug

/*
 * Defines
 */

#define VERSION         "1.1"
#define PROC_DIR        "bluetooth/sleep"

struct bluesleep_info {
        unsigned host_wake;
        unsigned ext_wake;
        unsigned host_wake_irq;
        struct uart_port *uport;
        struct wake_lock wake_lock;
};

/* work function */
static void bluesleep_sleep_work(struct work_struct *work);

/* work queue */
DECLARE_DELAYED_WORK(sleep_workqueue, bluesleep_sleep_work);

/* Macros for handling sleep work */
#define bluesleep_rx_busy()     schedule_delayed_work(&sleep_workqueue, 0)
#define bluesleep_tx_busy()     schedule_delayed_work(&sleep_workqueue, 0)
#define bluesleep_rx_idle()     schedule_delayed_work(&sleep_workqueue, 0)
#define bluesleep_tx_idle()     schedule_delayed_work(&sleep_workqueue, 0)

/* 1 second timeout */
#define TX_TIMER_INTERVAL      6 

/* state variable names and bit positions */
#define BT_PROTO        0x01
#define BT_TXDATA       0x02
#define BT_ASLEEP       0x04

/* global pointer to a single hci device. */
static struct hci_dev *bluesleep_hdev;

static struct bluesleep_info *bsi;

/* module usage */
static atomic_t open_count = ATOMIC_INIT(1);

/*
 * Local function prototypes
 */
//
static int bluesleep_hci_event(struct notifier_block *this,
                            unsigned long event, void *data);
static int bluesleep_start(void);
static void bluesleep_stop(void);

/*
 * Global variables
 */

/** Global state flags */
static unsigned long flags;

/** Workqueue to respond to change in hostwake line */
struct work_struct hostwake_work;
struct workqueue_struct *hostwake_work_queue;

/** Transmission timer */
static void bluesleep_tx_timer_expire(unsigned long data);
static DEFINE_TIMER(tx_timer, bluesleep_tx_timer_expire, 0, 0);

/** Lock for state transitions */
static spinlock_t rw_lock;

/** Notifier block for HCI events */
struct notifier_block hci_event_nblock = {
        .notifier_call = bluesleep_hci_event,
};

struct proc_dir_entry *bluetooth_dir, *sleep_dir;

/*
 * Local functions
 */

static void hsuart_power(int on)
{
    if (on) {
      // To Do
    } else {
      // To Do
    }
    //BT_SLEEP_DBG("uart power %d\n", on);
}


/**
 * @return 1 if the Host can go to sleep, 0 otherwise.
 */
int bluesleep_can_sleep(void)
{
        /* check if WAKE_BT_GPIO and BT_WAKE_GPIO are both deasserted */
        BT_SLEEP_DBG("bt_wake %d, host_wake %d, uport %p\n"
                  , gpio_get_value(bsi->ext_wake)
                  , gpio_get_value(bsi->host_wake)
                  , bsi->uport);
        return !gpio_get_value(bsi->ext_wake) &&
                !gpio_get_value(bsi->host_wake) &&
                (bsi->uport != NULL);
}

void bluesleep_sleep_wakeup(void)
{
        if (test_bit(BT_ASLEEP, &flags)) {
                BT_SLEEP_DBG("waking up...\n");
                wake_lock(&bsi->wake_lock);
                /* Start the timer */
                mod_timer(&tx_timer, jiffies + (TX_TIMER_INTERVAL * HZ));
                gpio_set_value(bsi->ext_wake, 1);
                clear_bit(BT_ASLEEP, &flags);
                /*Activating UART */
                hsuart_power(1);
        }
}

/**
 * @brief@  main sleep work handling function which update the flags
 * and activate and deactivate UART ,check FIFO.
 */
static void bluesleep_sleep_work(struct work_struct *work)
{
        if (bluesleep_can_sleep()) {
                BT_SLEEP_DBG("can sleep...\n");
                /* already asleep, this is an error case */
                if (test_bit(BT_ASLEEP, &flags)) {
                        BT_SLEEP_DBG("already asleep\n");
                        return;
                }

                if (bsi->uport->ops->tx_empty(bsi->uport)) {
                        BT_SLEEP_DBG("going to sleep...\n");
                        set_bit(BT_ASLEEP, &flags);
                        /*Deactivating UART */
                        hsuart_power(0);
                        /* UART clk is not turned off immediately. Release
                         * wakelock after 500 ms.
                         */
                        wake_lock_timeout(&bsi->wake_lock, HZ / 2);
                } else {
                        BT_SLEEP_DBG("tx buffer is not empty, modify timer...\n");
                        gpio_set_value(bsi->ext_wake, 1);
                        mod_timer(&tx_timer, jiffies + (TX_TIMER_INTERVAL * HZ));
                        return;
                }
        }else if (gpio_get_value(bsi->ext_wake) && !test_bit(BT_ASLEEP, &flags)) {
                BT_SLEEP_DBG("can not sleep, bt_wake %d\n", gpio_get_value(bsi->ext_wake) );
                mod_timer(&tx_timer, jiffies + (TX_TIMER_INTERVAL * HZ));
                gpio_set_value(bsi->ext_wake, 1);
        }else {
                bluesleep_sleep_wakeup();
        }
}

/**
 * A workqueue that runs in workqueue context and reads the value
 * of the HOST_WAKE GPIO pin and further defer the work.
 * @param work Not used.
 */
static void bluesleep_hostwake_work(struct work_struct *work)
{
        unsigned long irq_flags;

        //BT_SLEEP_DBG("hostwake line change.hostwake=[%d] \n", gpio_get_value(bsi->host_wake));

        spin_lock_irqsave(&rw_lock, irq_flags);
        bluesleep_sleep_wakeup();



        spin_unlock_irqrestore(&rw_lock, irq_flags);
}

/**
 * Handles proper timer action when outgoing data is delivered to the
 * HCI line discipline. Sets BT_TXDATA.
 */
static void bluesleep_outgoing_data(struct hci_dev *hdev)
{
        unsigned long irq_flags;

        spin_lock_irqsave(&rw_lock, irq_flags);

        /* log data passing by */
        set_bit(BT_TXDATA, &flags);

        /* if the tx side is sleeping... */
        if (!gpio_get_value(bsi->ext_wake)) {

                BT_SLEEP_DBG("tx was sleeping\n");
                bluesleep_sleep_wakeup();
        }

        spin_unlock_irqrestore(&rw_lock, irq_flags);
}

/**
 * Handles HCI device events.
 * @param this Not used.
 * @param event The event that occurred.
 * @param data The HCI device associated with the event.
 * @return <code>NOTIFY_DONE</code>.
 */
static int bluesleep_hci_event(struct notifier_block *this,
                                unsigned long event, void *data)
{
        struct hci_dev *hdev = (struct hci_dev *) data;
        struct hci_uart *hu;
        struct uart_state *state;

        if (!hdev)
                return NOTIFY_DONE;

        //BT_SLEEP_DBG("hci event %ld\n", event);
        switch (event) {
        case HCI_DEV_REG:
        BT_SLEEP_DBG("hci event %ld hdev = %p\n", event, hdev);
                if (!bluesleep_hdev) {
                        bluesleep_hdev = hdev;
                        hu  = (struct hci_uart *) hdev->driver_data;
                        state = (struct uart_state *) (hu->tty->driver_data);
                        bsi->uport = state->uart_port;
                        hdev->wake_peer = bluesleep_outgoing_data;
                }
                break;
        case HCI_DEV_UNREG:
                bluesleep_hdev = NULL;
                bsi->uport = NULL;
                break;
        //case HCI_DEV_WRITE:
        //        bluesleep_outgoing_data();
        //        break;
        }

        return NOTIFY_DONE;
}

/**
 * Handles transmission timer expiration.
 * @param data Not used.
 */
static void bluesleep_tx_timer_expire(unsigned long data)
{
        unsigned long irq_flags;

        spin_lock_irqsave(&rw_lock, irq_flags);

        BT_SLEEP_DBG("Tx timer expired\n");

        /* were we silent during the last timeout? */
        if (!test_bit(BT_TXDATA, &flags)) {
                BT_SLEEP_DBG("Tx has been idle\n");
                gpio_set_value(bsi->ext_wake, 0); 
                bluesleep_tx_idle();
        } else {
                BT_SLEEP_DBG("Tx data during last period\n");
                mod_timer(&tx_timer, jiffies + (TX_TIMER_INTERVAL*HZ));
        }

        /* clear the incoming data flag */
        clear_bit(BT_TXDATA, &flags);

        spin_unlock_irqrestore(&rw_lock, irq_flags);
}

/**
 * Schedules a workqueue to run when receiving an interrupt on the
 * <code>HOST_WAKE</code> GPIO pin.
 * @param irq Not used.
 * @param dev_id Not used.
 */
static irqreturn_t bluesleep_hostwake_isr(int irq, void *dev_id)
{

        /* schedule a workqueue to handle the change in the host wake line */
        queue_work(hostwake_work_queue, &hostwake_work);
        return IRQ_HANDLED;
}

/**
 * Starts the Sleep-Mode Protocol on the Host.
 * @return On success, 0. On error, -1, and <code>errno</code> is set
 * appropriately.
 */
static int bluesleep_start(void)
{
        int retval;
        unsigned long irq_flags;

        BT_SLEEP_DBG("bluesleep_start\n");
 
        spin_lock_irqsave(&rw_lock, irq_flags);

        if (test_bit(BT_PROTO, &flags)) {
                spin_unlock_irqrestore(&rw_lock, irq_flags);
                return 0;
        }

        spin_unlock_irqrestore(&rw_lock, irq_flags);

        if (!atomic_dec_and_test(&open_count)) {
                atomic_inc(&open_count);
                return -EBUSY;
        }

        /* start the timer */
        mod_timer(&tx_timer, jiffies + (TX_TIMER_INTERVAL * HZ));

        /* assert BT_WAKE */
        gpio_set_value(bsi->ext_wake, 1);
        retval = request_irq(bsi->host_wake_irq, bluesleep_hostwake_isr,
                                IRQF_DISABLED | IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                                "bluetooth hostwake", NULL);
        if (retval  < 0) {
                BT_SLEEP_DBG("Couldn't acquire BT_HOST_WAKE IRQ err (%d)\n", retval);
                goto fail;
        }
/*
        retval = enable_irq_wake(bsi->host_wake_irq);
        if (retval < 0) {
                BT_SLEEP_DBG("Couldn't enable BT_HOST_WAKE as wakeup interrupt (%d)\n", retval);
                free_irq(bsi->host_wake_irq, NULL);
                goto fail;
        }
*/
        BT_SLEEP_DBG("set BT_PROTO\n");
        set_bit(BT_PROTO, &flags);
        wake_lock(&bsi->wake_lock);
        return 0;
fail:
        del_timer(&tx_timer);
        atomic_inc(&open_count);

        return retval;
}

/**
 * Stops the Sleep-Mode Protocol on the Host.
 */
static void bluesleep_stop(void)
{
        unsigned long irq_flags;

        spin_lock_irqsave(&rw_lock, irq_flags);

        if (!test_bit(BT_PROTO, &flags)) {
                spin_unlock_irqrestore(&rw_lock, irq_flags);
                return;
        }

        /* deassert BT_WAKE */
        gpio_set_value(bsi->ext_wake, 0); 
        del_timer(&tx_timer);
        clear_bit(BT_PROTO, &flags);

        if (test_bit(BT_ASLEEP, &flags)) {
                clear_bit(BT_ASLEEP, &flags);
                hsuart_power(1);
        }

        atomic_inc(&open_count);

        spin_unlock_irqrestore(&rw_lock, irq_flags);
        if (disable_irq_wake(bsi->host_wake_irq))
                BT_SLEEP_DBG("Couldn't disable hostwake IRQ wakeup mode\n");
        free_irq(bsi->host_wake_irq, NULL);
        wake_lock_timeout(&bsi->wake_lock, HZ / 2);
}
/**
 * Read the <code>BT_WAKE</code> GPIO pin value via the proc interface.
 * When this function returns, <code>page</code> will contain a 1 if the
 * pin is high, 0 otherwise.
 * @param page Buffer for writing data.
 * @param start Not used.
 * @param offset Not used.
 * @param count Not used.
 * @param eof Whether or not there is more data to be read.
 * @param data Not used.
 * @return The number of bytes written.
 */
static int bluepower_read_proc_btwake(char *page, char **start, off_t offset,
                                              int count, int *eof, void *data)
{
        *eof = 1;
        return sprintf(page, "btwake:%u\n", gpio_get_value(bsi->ext_wake));
}

/**
 * Write the <code>BT_WAKE</code> GPIO pin value via the proc interface.
 * @param file Not used.
 * @param buffer The buffer to read from.
 * @param count The number of bytes to be written.
 * @param data Not used.
 * @return On success, the number of bytes written. On error, -1, and
 * <code>errno</code> is set appropriately.
 */
static int bluepower_write_proc_btwake(struct file *file, const char *buffer,
                                            unsigned long count, void *data)
{
        char *buf;

        if (count < 1)
              return -EINVAL;

        buf = kmalloc(count, GFP_KERNEL);
        if (!buf)
              return -ENOMEM;

        if (copy_from_user(buf, buffer, count)) {
              kfree(buf);
              return -EFAULT;
        }

        BT_SLEEP_DBG("bluepower_write_proc_btwake\n");

        if (buf[0] == '0') {
              gpio_set_value(bsi->ext_wake, 0); 
        } else if (buf[0] == '1') {
              gpio_set_value(bsi->ext_wake, 1);
        } else {
              kfree(buf);
              return -EINVAL;
        }

        kfree(buf);

        return count;
}

/**
 * Read the <code>BT_HOST_WAKE</code> GPIO pin value via the proc interface.
 * When this function returns, <code>page</code> will contain a 1 if the pin
 * is high, 0 otherwise.
 * @param page Buffer for writing data.
 * @param start Not used.
 * @param offset Not used.
 * @param count Not used.
 * @param eof Whether or not there is more data to be read.
 * @param data Not used.
 * @return The number of bytes written.
 */
static int bluepower_read_proc_hostwake(char *page, char **start, off_t offset,
                                                int count, int *eof, void *data)
{
        *eof = 1;
        return sprintf(page, "hostwake: %u \n", gpio_get_value(bsi->host_wake));
}


/**
 * Read the low-power status of the Host via the proc interface.
 * When this function returns, <code>page</code> contains a 1 if the Host
 * is asleep, 0 otherwise.
 * @param page Buffer for writing data.
 * @param start Not used.
 * @param offset Not used.
 * @param count Not used.
 * @param eof Whether or not there is more data to be read.
 * @param data Not used.
 * @return The number of bytes written.
 */
static int bluesleep_read_proc_asleep(char *page, char **start, off_t offset,
                                        int count, int *eof, void *data)
{
        unsigned int asleep;

        asleep = test_bit(BT_ASLEEP, &flags) ? 1 : 0;
        *eof = 1;
        return sprintf(page, "asleep: %u\n", asleep);
}

/**
 * Read the low-power protocol being used by the Host via the proc interface.
 * When this function returns, <code>page</code> will contain a 1 if the Host
 * is using the Sleep Mode Protocol, 0 otherwise.
 * @param page Buffer for writing data.
 * @param start Not used.
 * @param offset Not used.
 * @param count Not used.
 * @param eof Whether or not there is more data to be read.
 * @param data Not used.
 * @return The number of bytes written.
 */
static int bluesleep_read_proc_proto(char *page, char **start, off_t offset,
                                        int count, int *eof, void *data)
{
        unsigned int proto;

        proto = test_bit(BT_PROTO, &flags) ? 1 : 0;
        *eof = 1;
        return sprintf(page, "proto: %u\n", proto);
}

/**
 * Modify the low-power protocol used by the Host via the proc interface.
 * @param file Not used.
 * @param buffer The buffer to read from.
 * @param count The number of bytes to be written.
 * @param data Not used.
 * @return On success, the number of bytes written. On error, -1, and
 * <code>errno</code> is set appropriately.
 */
static int bluesleep_write_proc_proto(struct file *file, const char *buffer,
                                        unsigned long count, void *data)
{
        char proto;

        if (count < 1)
                return -EINVAL;

        if (copy_from_user(&proto, buffer, 1))
                return -EFAULT;

        BT_SLEEP_DBG("write proto %c\n", proto);

        if (proto == '0')
                bluesleep_stop();
        else
                bluesleep_start();

        /* claim that we wrote everything */
        return count;
}


static int /*__init*/ bluesleep_probe(struct platform_device *pdev)
{
        int ret;
        struct resource *res;

        BT_SLEEP_DBG("enter\n");
        bsi = kzalloc(sizeof(struct bluesleep_info), GFP_KERNEL);
        memset(bsi, 0, sizeof(struct bluesleep_info ));

        if (!bsi)
                return -ENOMEM;

        BT_SLEEP_DBG("mallac bluesleep_info done\n");

        res = platform_get_resource_byname(pdev, IORESOURCE_IO,
                                "gpio_host_wake");
        if (!res) {
                BT_SLEEP_DBG("couldn't find host_wake gpio\n");
                ret = -ENODEV;
                goto free_bsi;
        }
        bsi->host_wake = res->start;

        BT_SLEEP_DBG("hostwake = %d\n", bsi->host_wake);

        ret = gpio_request(bsi->host_wake, "bt_host_wake");
        if (ret)
                goto free_bsi;
        // configure host_wake as input
        BT_SLEEP_DBG("set hostwake as input\n");
        ret = gpio_direction_input(bsi->host_wake);
        if (ret < 0) {
                pr_err("gpio-keys: failed to configure input"
                            " direction for GPIO %d, error %d\n",
                              bsi->host_wake, ret);
                gpio_free(bsi->host_wake);
                goto free_bsi;
        }

        BT_SLEEP_DBG("try to get bt_wake");
        res = platform_get_resource_byname(pdev, IORESOURCE_IO,
                                "gpio_ext_wake");
        if (!res) {
                BT_SLEEP_DBG("couldn't find ext_wake gpio\n");
                ret = -ENODEV;
                goto free_bt_host_wake;
        }
        bsi->ext_wake = res->start;

        BT_SLEEP_DBG("btwake = %d\n", bsi->ext_wake);
        ret = gpio_request(bsi->ext_wake, "bt_ext_wake");
        if (ret)
                goto free_bt_host_wake;

        // configure ext_wake as output mode
        BT_SLEEP_DBG("set bt_wake as output\n");
        ret = gpio_direction_output(bsi->ext_wake, 0); 
        if (ret < 0) {
                pr_err("gpio-keys: failed to configure output"
                            " direction for GPIO %d, error %d\n",
                              bsi->ext_wake, ret);
                gpio_free(bsi->ext_wake);
                goto free_bt_host_wake;
        }
#if 0
        bsi->host_wake_irq = platform_get_irq_byname(pdev, "host_wake");
        if (bsi->host_wake_irq < 0) {
                BT_SLEEP_DBG("couldn't find host_wake irq\n");
                ret = -ENODEV;
                goto free_bt_ext_wake;
        }
#else
        BT_SLEEP_DBG("allocat irq hostwake = %d\n", bsi->host_wake);

        bsi->host_wake_irq = gpio_to_irq(bsi->host_wake);  
        BT_SLEEP_DBG("irq = bsi->host_wake_irq %d\n", bsi->host_wake_irq);
        if (bsi->host_wake_irq < 0) {
                BT_SLEEP_DBG("couldn't find host_wake irq\n");
                ret = -ENODEV;
                goto free_bt_ext_wake;
        }
#endif

        wake_lock_init(&bsi->wake_lock, WAKE_LOCK_SUSPEND, "bluesleep");

        BT_SLEEP_DBG("exit probe\n");
        return 0;

free_bt_ext_wake:
        gpio_free(bsi->ext_wake);
free_bt_host_wake:
        gpio_free(bsi->host_wake);
free_bsi:
        kfree(bsi);
        return ret;
}

static int bluesleep_remove(struct platform_device *pdev)
{
        gpio_free(bsi->host_wake);
        gpio_free(bsi->ext_wake);
        wake_lock_destroy(&bsi->wake_lock);
        kfree(bsi);
        return 0;
}

static struct platform_driver bluesleep_driver = {
        .probe = bluesleep_probe,
        .remove = bluesleep_remove,
        .driver = {
                .name = "bluesleep",
                .owner = THIS_MODULE,
        },
};
/**
 * Initializes the module.
 * @return On success, 0. On error, -1, and <code>errno</code> is set
 * appropriately.
 */
static int __init bluesleep_init(void)
{
        int retval;
        struct proc_dir_entry *ent;
        char name[64];

        BT_SLEEP_DBG("BlueSleep Mode Driver Ver %s\n", VERSION);

        retval = platform_driver_register(&bluesleep_driver);
        if (retval)
                return retval;

        bluesleep_hdev = NULL;

        bluetooth_dir = proc_mkdir("bluetooth", NULL);
        if (bluetooth_dir == NULL) {
                BT_SLEEP_DBG("Unable to create /proc/bluetooth directory\n");
                return -ENOMEM;
        }

        sleep_dir = proc_mkdir("sleep", bluetooth_dir);
        if (sleep_dir == NULL) {
                BT_SLEEP_DBG("Unable to create /proc/%s directory\n", PROC_DIR);
                return -ENOMEM;
        }

        /* Creating read/write "btwake" entry */
        ent = create_proc_entry("btwake", 0, sleep_dir);
        if (ent == NULL) {
                BT_SLEEP_DBG("Unable to create /proc/%s/btwake entry\n", PROC_DIR);
                retval = -ENOMEM;
                goto fail;
        }
        ent->read_proc = bluepower_read_proc_btwake;
        ent->write_proc = bluepower_write_proc_btwake;

        /* read only proc entries */
        if (create_proc_read_entry("hostwake", 0, sleep_dir,
                                bluepower_read_proc_hostwake, NULL) == NULL) {
                BT_SLEEP_DBG("Unable to create /proc/%s/hostwake entry\n", PROC_DIR);
                retval = -ENOMEM;
                goto fail;
        }

        /* read/write proc entries */
        ent = create_proc_entry("proto", 0, sleep_dir);
        if (ent == NULL) {
                BT_SLEEP_DBG("Unable to create /proc/%s/proto entry\n", PROC_DIR);
                retval = -ENOMEM;
                goto fail;
        }
        ent->read_proc = bluesleep_read_proc_proto;
        ent->write_proc = bluesleep_write_proc_proto;

        /* read only proc entries */
        if (create_proc_read_entry("asleep", 0,
                        sleep_dir, bluesleep_read_proc_asleep, NULL) == NULL) {
                BT_SLEEP_DBG("Unable to create /proc/%s/asleep entry\n", PROC_DIR);
                retval = -ENOMEM;
                goto fail;
        }

        flags = 0; /* clear all status bits */

        /* Initialize spinlock. */
        spin_lock_init(&rw_lock);

        /* Initialize timer */
        init_timer(&tx_timer);
        tx_timer.function = bluesleep_tx_timer_expire;
        tx_timer.data = 0;

        /* create the workqueue for the hostwake */
        snprintf(name, sizeof(name), "blue_sleep");
        hostwake_work_queue = create_singlethread_workqueue(name);
        if (hostwake_work_queue == NULL) {
        BT_SLEEP_DBG("Unable to create workqueue \n");
        retval = -ENODEV;
        goto fail;
        }
    
        /* Initialise the work */
        INIT_WORK(&hostwake_work, bluesleep_hostwake_work);

        /* assert bt wake */
        gpio_set_value(bsi->ext_wake, 0); 
        hci_register_notifier(&hci_event_nblock);

        return 0;

fail:
        remove_proc_entry("asleep", sleep_dir);
        remove_proc_entry("proto", sleep_dir);
        remove_proc_entry("hostwake", sleep_dir);
        remove_proc_entry("btwake", sleep_dir);
        remove_proc_entry("sleep", bluetooth_dir);
        remove_proc_entry("bluetooth", 0);
        return retval;
}

/**
 * Cleans up the module.
 */
static void __exit bluesleep_exit(void)
{
        /* assert bt wake */
        gpio_set_value(bsi->ext_wake, 0); 
        if (test_bit(BT_PROTO, &flags)) {
                if (disable_irq_wake(bsi->host_wake_irq))
                        BT_SLEEP_DBG("Couldn't disable hostwake IRQ wakeup mode \n");
                free_irq(bsi->host_wake_irq, NULL);
                del_timer(&tx_timer);
                if (test_bit(BT_ASLEEP, &flags))
                        hsuart_power(1);
        }

        hci_unregister_notifier(&hci_event_nblock);
        platform_driver_unregister(&bluesleep_driver);

        remove_proc_entry("asleep", sleep_dir);
        remove_proc_entry("proto", sleep_dir);
        remove_proc_entry("hostwake", sleep_dir);
        remove_proc_entry("btwake", sleep_dir);
        remove_proc_entry("sleep", bluetooth_dir);
        remove_proc_entry("bluetooth", 0);
        destroy_workqueue(hostwake_work_queue);
}

module_init(bluesleep_init);
module_exit(bluesleep_exit);

MODULE_DESCRIPTION("Bluetooth Sleep Mode Driver ver %s " VERSION);
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

