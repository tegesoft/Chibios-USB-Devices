#include "usb_msd.h"
#include "ch.h"
#include "hal.h"
#include <stdio.h>
#include <string.h>

/* USB identifiers and strings that we'll send to the host */
MSD_DECLARE_DEVICE_DESCRIPTOR(usbDeviceDescriptor, 0x0483, 0x5742);

#define USB_VENDOR_STRING \
    'D', 0, 'e', 0, 'm', 0, 'o', 0, 'V', 0, 'e', 0, 'n', 0, 'd', 0, 'o', 0, 'r', 0
MSD_DECLARE_STRING_DESCRIPTOR(usbVendorDescriptor, 20, USB_VENDOR_STRING);

#define USB_PRODUCT_STRING \
    'D', 0, 'e', 0, 'm', 0, 'o', 0, 'P', 0, 'r', 0, 'o', 0, 'd', 0, 'u', 0, 'c', 0, 't', 0
MSD_DECLARE_STRING_DESCRIPTOR(usbProductDescriptor, 22, USB_PRODUCT_STRING);

#define USB_SERIAL_NUMBER_STRING \
    '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '1', 0
MSD_DECLARE_STRING_DESCRIPTOR(usbSerialNumberDescriptor, 24, USB_SERIAL_NUMBER_STRING);

/* Turns on a LED when there is I/O activity on the USB port */
static void usbActivity(bool_t on)
{
    if (on)
        palSetPad(GPIOC, GPIOC_LED);
    else
        palClearPad(GPIOC, GPIOC_LED);
}

/* USB mass storage configuration */
static USBMassStorageConfig msdConfig =
{
    &USBD2,
    (BaseBlockDevice*)&SDCD1,
    usbActivity,
    &usbDeviceDescriptor,
    &usbVendorDescriptor,
    &usbProductDescriptor,
    &usbSerialNumberDescriptor
};

/* USB mass storage driver */
USBMassStorageDriver UMSD1;

int main(void)
{
    /* system & hardware initialization */
    halInit();
    chSysInit();

    /* initialize the SD card */
    sdcStart(&SDCD1, NULL);
    sdcConnect(&SDCD1);

    /* turn off the test LED */
    palClearPad(GPIOC, GPIOC_LED);

    /* run the USB mass storage service */
    msdInit(&UMSD1, &msdConfig);

    /* watch the mass storage events */
    EventListener connected;
    EventListener ejected;
    chEvtRegisterMask(&UMSD1.evt_connected, &connected, EVENT_MASK(1));
    chEvtRegisterMask(&UMSD1.evt_ejected, &ejected, EVENT_MASK(2));

    while (TRUE)
    {
        eventmask_t event = chEvtWaitOne(EVENT_MASK(1) | EVENT_MASK(2));
        if (event == EVENT_MASK(1))
        {
            /* media connected */
        }
        else if (event == EVENT_MASK(2))
        {
            /* media ejected */
        }
    }

    return 0;
}
