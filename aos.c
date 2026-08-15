#include "linux/compiler_types.h"
#include "linux/device.h"
#include "linux/gfp_types.h"
#include "linux/printk.h"
#include "linux/slab.h"
#include "linux/uaccess.h"
#include "linux/usb/cdc.h"
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/usb/cdc.h>

#define VENDOR_ID 0x239a
#define PRODUCT_ID 0x80f0
#define BUF_SIZE 1024*2 //2 KiB
#define DTR 0x01 //Data Terminal Ready
#define RTS 0x02 //Request To Send


static struct usb_device *usb_dev;
static struct usb_device_id usb_dev_table [] = 
{
    //I check for a specific interface since probe would get called multiple times since the device has a lot of interfaces
    {USB_DEVICE_AND_INTERFACE_INFO(VENDOR_ID,PRODUCT_ID,10,0,0)},
    {},
};
MODULE_DEVICE_TABLE(usb, usb_dev_table);



static dev_t dev_nr;
static struct cdev aos_cdev;
static struct class *aos_class;

static struct usb_cdc_line_coding line_coding =
{
    .bCharFormat = 0,
    .bDataBits = 8,
    .bParityType = 0,
    .dwDTERate = cpu_to_le32(115200),
};

struct read_buffer 
{
    char *data;
    int available;
};

static struct read_buffer read_buf;

static int aos_probe(struct usb_interface *intf, const struct usb_device_id *id) 
{
    int status;

    usb_dev  = interface_to_usbdev(intf);
    if (usb_dev == NULL){
        pr_err("AOS:  Error getting device from interface\n");
        return -1;
    }

    //Sends the line coding parameter to the device
    status = usb_control_msg(usb_dev,usb_sndctrlpipe(usb_dev, 0),USB_CDC_REQ_SET_LINE_CODING,
    USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT,0,0,&line_coding,sizeof(line_coding),1000);
    
    if (status < 0){
        pr_err("AOS: Probe failed setting LINE_CODING\n");
        return status;
    }

    //Sets up the device to accept bulk transfer on the CDC endpoint
    status = usb_control_msg(usb_dev,usb_sndctrlpipe(usb_dev, 0),USB_CDC_REQ_SET_CONTROL_LINE_STATE,
    USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT, DTR | RTS,
    0 /* interface 0 (CDC control)*/ ,NULL,0,1000);

    if (status < 0){
        pr_err("AOS: Probe failed setting CONTROL_LINE_STATE\n");
        return status;
    }
    //If the device is properly set up I create the corresponding device file
    if (!device_create(aos_class, NULL, dev_nr, NULL, "AOSdev")) {
        pr_err("AOS:  Could not create device AOSdev\n");
        return -1;
    }
    pr_info("AOS:  Created device under /sys/class/aos_class/AOSdev\n");
    pr_info("AOS:  Device file /dev/AOSdev\n");

    pr_info("AOS: Probe sucessful\n");
    return 0;
}

static void flushBuf()
{
    memset(read_buf.data,0,BUF_SIZE);
    read_buf.available = 0;
}

static void aos_disconnect(struct usb_interface *intf) 
{   
    device_destroy(aos_class, dev_nr);
    flushBuf();
    usb_dev = NULL;
    pr_info("AOS: Disconnected device\n");
}

static struct usb_driver aos_usb_driver = 
{
    .name = "AOS_Driver",
    .id_table = usb_dev_table,
    .probe = aos_probe,
    .disconnect = aos_disconnect,
};

/** 
* The device keeps a local buffer of output characters,
* the buffer has a limited size and if the device does not
* manage to free up space by sending those character it stops
* accepting any input character at all.
*
* This function reads from the device's buffer until it's empty
*/
static void empty_device_buffer()
{
    char *reply;
    int actually_copied;
    reply = kzalloc(64,GFP_KERNEL);
    do{
        usb_bulk_msg(usb_dev, usb_rcvbulkpipe(usb_dev,2), reply, 64, &actually_copied, 100);
        //Flushes the buffer
        if(read_buf.available + actually_copied > BUF_SIZE){
            flushBuf();
        }
        memcpy(&read_buf.data[read_buf.available ],reply,actually_copied);
        read_buf.available += actually_copied;
    }
    while (actually_copied !=0 );
}

static ssize_t aos_read(struct file *f, char __user *user_buf, size_t len, loff_t *off)
{   
    int to_copy, not_copied, delta;

    if(usb_dev == NULL){
        pr_info("AOS: No device plugged in\n");
        return -1;
    }

    to_copy = read_buf.available - *off;
    if(to_copy <= 0 ){
        flushBuf();
        return 0;
    }
    to_copy = min(to_copy,len);
    not_copied = copy_to_user(user_buf,read_buf.data + *off,to_copy);
    delta = to_copy - not_copied;
    *off+=delta;
    pr_info("AOS:  Read successful\n");

    return delta;
}

static ssize_t aos_write(struct file *f, const char *user_buf, size_t len, loff_t *off)
{
    char *text;
    int to_copy, not_copied, delta, status;
    int actually_copied;

    if(usb_dev == NULL){
        pr_info("AOS: No device plugged in\n");
        return -1;
    }
    
    text = kzalloc(64, GFP_KERNEL);

    to_copy = min(len, 64);
    not_copied = copy_from_user(text, user_buf, to_copy);
    delta = to_copy - not_copied;

    status = usb_bulk_msg(usb_dev, usb_sndbulkpipe(usb_dev, 2), text, delta, &actually_copied, 2000);
    if (status < 0){
        pr_err("AOS: Error sending bulk msg, ERRNO = %d\n",status);
        return status;
    }
    pr_info("AOS:  Write successful:\n\"\t%s\t\"\n",text);

    empty_device_buffer();

    kfree(text);
    return actually_copied;
}

static struct file_operations fops = 
{
    .read = aos_read,
    .write = aos_write
};

static int __init aos_init(void) 
{
    int status;
    read_buf.data = kzalloc(BUF_SIZE,GFP_KERNEL);
    read_buf.available = 0;
    pr_info("AOS:  Init Called\n");
    status = usb_register(&aos_usb_driver);
    if(status) {
        pr_err("AOS:  Error during usb_register\n");
        return status;
    }

    status = alloc_chrdev_region(&dev_nr, 0, 10, "AOS_dev");
    if (status) {
        pr_err("AOS: Error reserving the region of device numbers\n");
        goto deregister;
    }
    cdev_init(&aos_cdev, &fops);
    aos_cdev.owner = THIS_MODULE;
    status = cdev_add(&aos_cdev, dev_nr, 10);
    if (status) {
        pr_err("AOS: Error adding cdev\n");
        goto free_devnr;
    }


    aos_class = class_create("aos_class");

    if (IS_ERR(aos_class)) {
        pr_err("AOS:  Could not create class aos_class\n");
        status = ENOMEM;
        goto delete_cdev;
    }
    return 0;
    
delete_cdev:
    cdev_del(&aos_cdev);
free_devnr:
    unregister_chrdev_region(dev_nr,  10);
deregister:
    usb_deregister(&aos_usb_driver);
    return status;
}

static void __exit aos_exit(void) 
{
    usb_deregister(&aos_usb_driver);
    cdev_del(&aos_cdev);
    unregister_chrdev_region(dev_nr, MINORMASK + 1);
    class_destroy(aos_class);
    pr_info("AOS:  Exit function called\n");
}

module_init(aos_init);
module_exit(aos_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Samuele Lo Piccolo");
MODULE_DESCRIPTION("This module implements CDC bulk transfers for the Adafruit Neo Trinkey - SAMD21 USB Key \
    This allows to send textual commands to the REPL, a python command line interface provided by CircuitPython");
