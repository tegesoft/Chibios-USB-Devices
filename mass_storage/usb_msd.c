#include "usb_msd.h"

/* End-point info */
#define USB_MS_DATA_EP 1  /* data end-point index */
#define USB_MS_EP_SIZE 64 /* end-point size */

/* Request types */
#define MSD_REQ_RESET   0xFF
#define MSD_GET_MAX_LUN 0xFE

/* CBW/CSW block signatures */
#define MSD_CBW_SIGNATURE 0x43425355
#define MSD_CSW_SIGNATURE 0x53425355

/* Setup packet access macros */
#define MSD_SETUP_WORD(setup, index) (uint16_t)(((uint16_t)setup[index + 1] << 8) | (setup[index] & 0x00FF))
#define MSD_SETUP_VALUE(setup)       MSD_SETUP_WORD(setup, 2)
#define MSD_SETUP_INDEX(setup)       MSD_SETUP_WORD(setup, 4)
#define MSD_SETUP_LENGTH(setup)      MSD_SETUP_WORD(setup, 6)

/* Command statuses */
#define MSD_COMMAND_PASSED      0x00
#define MSD_COMMAND_FAILED      0x01
#define MSD_COMMAND_PHASE_ERROR 0x02

/* SCSI commands */
#define SCSI_CMD_INQUIRY                      0x12
#define SCSI_CMD_REQUEST_SENSE                0x03
#define SCSI_CMD_READ_CAPACITY_10             0x25
#define SCSI_CMD_READ_10                      0x28
#define SCSI_CMD_WRITE_10                     0x2A
#define SCSI_CMD_TEST_UNIT_READY              0x00
#define SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1E
#define SCSI_CMD_VERIFY_10                    0x2F
#define SCSI_CMD_SEND_DIAGNOSTIC              0x1D
#define SCSI_CMD_MODE_SENSE_6                 0x1A
#define SCSI_CMD_START_STOP_UNIT              0x1B
#define SCSI_CMD_READ_FORMAT_CAPACITIES       0x23

/* SCSI sense constants */
#define SCSI_SENSE_KEY_GOOD                            0x00
#define SCSI_SENSE_KEY_RECOVERED_ERROR                 0x01
#define SCSI_SENSE_KEY_NOT_READY                       0x02
#define SCSI_SENSE_KEY_MEDIUM_ERROR                    0x03
#define SCSI_SENSE_KEY_HARDWARE_ERROR                  0x04
#define SCSI_SENSE_KEY_ILLEGAL_REQUEST                 0x05
#define SCSI_SENSE_KEY_UNIT_ATTENTION                  0x06
#define SCSI_SENSE_KEY_DATA_PROTECT                    0x07
#define SCSI_SENSE_KEY_BLANK_CHECK                     0x08
#define SCSI_SENSE_KEY_VENDOR_SPECIFIC                 0x09
#define SCSI_SENSE_KEY_COPY_ABORTED                    0x0A
#define SCSI_SENSE_KEY_ABORTED_COMMAND                 0x0B
#define SCSI_SENSE_KEY_VOLUME_OVERFLOW                 0x0D
#define SCSI_SENSE_KEY_MISCOMPARE                      0x0E
#define SCSI_ASENSE_NO_ADDITIONAL_INFORMATION          0x00
#define SCSI_ASENSE_LOGICAL_UNIT_NOT_READY             0x04
#define SCSI_ASENSE_INVALID_FIELD_IN_CDB               0x24
#define SCSI_ASENSE_NOT_READY_TO_READY_CHANGE          0x28
#define SCSI_ASENSE_WRITE_PROTECTED                    0x27
#define SCSI_ASENSE_FORMAT_ERROR                       0x31
#define SCSI_ASENSE_INVALID_COMMAND                    0x20
#define SCSI_ASENSE_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE 0x21
#define SCSI_ASENSE_MEDIUM_NOT_PRESENT                 0x3A
#define SCSI_ASENSEQ_NO_QUALIFIER                      0x00
#define SCSI_ASENSEQ_FORMAT_COMMAND_FAILED             0x01
#define SCSI_ASENSEQ_INITIALIZING_COMMAND_REQUIRED     0x02
#define SCSI_ASENSEQ_OPERATION_IN_PROGRESS             0x07

/**
 * @brief Response to a regular INQUIRY SCSI command
 */
PACK_STRUCT_BEGIN typedef struct
{
    uint8_t peripheral;
    uint8_t removable;
    uint8_t version;
    uint8_t response_data_format;
    uint8_t additional_length;
    uint8_t sccstp;
    uint8_t bqueetc;
    uint8_t cmdque;
    uint8_t vendorID[8];
    uint8_t productID[16];
    uint8_t productRev[4];
} PACK_STRUCT_STRUCT msd_scsi_inquiry_response_t PACK_STRUCT_END;

/**
 * @brief Response to a READ_CAPACITY_10 SCSI command
 */
PACK_STRUCT_BEGIN typedef struct {
    uint32_t last_block_addr;
    uint32_t block_size;
} PACK_STRUCT_STRUCT msd_scsi_read_capacity_10_response_t PACK_STRUCT_END;

/**
 * @brief Response to a READ_FORMAT_CAPACITIES SCSI command
 */
PACK_STRUCT_BEGIN typedef struct {
    uint8_t reserved[3];
    uint8_t capacity_list_length;
    uint32_t block_count;
    uint32_t desc_and_block_length;
} PACK_STRUCT_STRUCT msd_scsi_read_format_capacities_response_t PACK_STRUCT_END;

/**
 * @brief   Read-write buffers (TODO: need a way of specifying the size of this)
 */
static uint8_t rw_buf[2][512];

/**
 * @brief Byte-swap a 32 bits unsigned integer
 */
#define swap_uint32(x) ((((x) & 0x000000FF) << 24) \
                      | (((x) & 0x0000FF00) << 8) \
                      | (((x) & 0x00FF0000) >> 8) \
                      | (((x) & 0xFF000000) >> 24))

/**
 * @brief Byte-swap a 16 bits unsigned integer
 */
#define swap_uint16(x) ((((x) & 0x00FF) << 8) \
                      | (((x) & 0xFF00) >> 8))

/**
 * @brief USB Device Descriptor
 */
static const uint8_t msd_device_descriptor_data[18] = {
    USB_DESC_DEVICE(0x0200,        /* bcdUSB (2.0).                    */
                    0x00,          /* bDeviceClass (None).             */
                    0x00,          /* bDeviceSubClass.                 */
                    0x00,          /* bDeviceProtocol.                 */
                    0x40,          /* Control Endpoint Size.           */
                    0x0483,        /* idVendor (ST).                   */
                    0x5742,        /* idProduct.                       */
                    0x0100,        /* bcdDevice.                       */
                    1,             /* iManufacturer.                   */
                    2,             /* iProduct.                        */
                    3,             /* iSerialNumber.                   */
                    1)             /* bNumConfigurations.              */
};

/**
 * @brief Device Descriptor wrapper
 */
static const USBDescriptor msd_device_descriptor = {
    sizeof msd_device_descriptor_data,
    msd_device_descriptor_data
};

/**
 * @brief Configuration Descriptor tree for a CDC
 */
static const uint8_t msd_configuration_descriptor_data[] = {
    /* Configuration Descriptor.*/
    USB_DESC_CONFIGURATION(0x0020,        /* wTotalLength.                    */
                           0x01,          /* bNumInterfaces.                  */
                           0x01,          /* bConfigurationValue.             */
                           0,             /* iConfiguration.                  */
                           0xC0,          /* bmAttributes (self powered).     */
                           0x32),         /* bMaxPower (100mA).               */
    /* Interface Descriptor.*/
    USB_DESC_INTERFACE    (0x00,          /* bInterfaceNumber.                */
                           0x00,          /* bAlternateSetting.               */
                           0x02,          /* bNumEndpoints.                   */
                           0x08,          /* bInterfaceClass (Mass Storage)   */
                           0x06,          /* bInterfaceSubClass (SCSI
                                                 Transparent storage class)       */
                           0x50,          /* bInterfaceProtocol (Bulk Only)   */
                           0),            /* iInterface. (none)               */
    /* Mass Storage Data In Endpoint Descriptor.*/
    USB_DESC_ENDPOINT     (USB_MS_DATA_EP|0x80,
                           0x02,          /* bmAttributes (Bulk).             */
                           USB_MS_EP_SIZE,/* wMaxPacketSize.                  */
                           0x05),         /* bInterval. 1ms                   */
    /* Mass Storage Data In Endpoint Descriptor.*/
    USB_DESC_ENDPOINT     (USB_MS_DATA_EP,
                           0x02,          /* bmAttributes (Bulk).             */
                           USB_MS_EP_SIZE,/* wMaxPacketSize.                  */
                           0x05)          /* bInterval. 1ms                   */
};

/**
 * @brief Configuration Descriptor wrapper
 */
static const USBDescriptor msd_configuration_descriptor = {
    sizeof msd_configuration_descriptor_data,
    msd_configuration_descriptor_data
};

/**
 * @brief U.S. English language identifier
 */
static const uint8_t msd_string0[] = {
    USB_DESC_BYTE(4),                     /* bLength.                         */
    USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
    USB_DESC_WORD(0x0409)                 /* wLANGID (U.S. English).          */
};

/**
 * @brief Vendor string
 */
static const uint8_t msd_string1[] = {
    USB_DESC_BYTE(38),                    /* bLength.                         */
    USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
    'S', 0, 'T', 0, 'M', 0, 'i', 0, 'c', 0, 'r', 0, 'o', 0, 'e', 0,
    'l', 0, 'e', 0, 'c', 0, 't', 0, 'r', 0, 'o', 0, 'n', 0, 'i', 0,
    'c', 0, 's', 0
};

/**
 * @brief Device Description string
 */
static const uint8_t msd_string2[] = {
    USB_DESC_BYTE(62),                    /* bLength.                         */
    USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
    'C', 0, 'h', 0, 'i', 0, 'b', 0, 'i', 0, 'O', 0, 'S', 0, '/', 0,
    'R', 0, 'T', 0, ' ', 0, 'M', 0, 'a', 0, 's', 0, 's', 0, ' ', 0,
    'S', 0, 't', 0, 'o', 0, 'r', 0, 'a', 0, 'g', 0, 'e', 0, ' ', 0,
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0
};

/**
 * @brief Device Serial Number string
 */
static const uint8_t msd_string3[] = {
    USB_DESC_BYTE(26),                    /* bLength.                         */
    USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
    'A', 0, 'E', 0, 'C', 0, 'C', 0, 'E', 0, 'C', 0, 'C', 0, 'C', 0, 'C', 0,
    '0' + CH_KERNEL_MAJOR, 0,
    '0' + CH_KERNEL_MINOR, 0,
    '0' + CH_KERNEL_PATCH, 0
};

/**
 * @brief Strings wrappers array
 */
static const USBDescriptor msd_strings[] = {
    {sizeof msd_string0, msd_string0},
    {sizeof msd_string1, msd_string1},
    {sizeof msd_string2, msd_string2},
    {sizeof msd_string3, msd_string3}
};

/**
 * @brief Handles the GET_DESCRIPTOR callback.
 *        All required descriptors must be handled here.
 */
static const USBDescriptor *msd_get_descriptor(USBDriver *usbp,
                                               uint8_t dtype,
                                               uint8_t dindex,
                                               uint16_t lang) {

    (void)usbp;
    (void)lang;

    switch (dtype) {
    case USB_DESCRIPTOR_DEVICE:
        return &msd_device_descriptor;
    case USB_DESCRIPTOR_CONFIGURATION:
        return &msd_configuration_descriptor;
    case USB_DESCRIPTOR_STRING:
        if (dindex < 4)
            return &msd_strings[dindex];
    }
    return NULL;
}

/**
 * @brief   Default requests hook.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @return              The hook status.
 * @retval TRUE         Message handled internally.
 * @retval FALSE        Message not handled.
 */
bool_t msd_handle_requests(USBDriver *usbp) {

    /* check that the request is of type Class / Interface */
    if (((usbp->setup[0] & USB_RTYPE_TYPE_MASK) == USB_RTYPE_TYPE_CLASS) &&
        ((usbp->setup[0] & USB_RTYPE_RECIPIENT_MASK) == USB_RTYPE_RECIPIENT_INTERFACE)) {

        /* check that the request is for interface 0 */
        if (MSD_SETUP_INDEX(usbp->setup) != 0)
            return FALSE;

        /* act depending on bRequest = setup[1] */
        switch (usbp->setup[1]) {
        case MSD_REQ_RESET:
            /* check that it is a HOST2DEV request */
            if (((usbp->setup[0] & USB_RTYPE_DIR_MASK) != USB_RTYPE_DIR_HOST2DEV) ||
               (MSD_SETUP_LENGTH(usbp->setup) != 0) ||
               (MSD_SETUP_VALUE(usbp->setup) != 0))
            {
                return FALSE;
            }

            /* reset all endpoints */
            /* TODO!*/
            /* The device shall NAK the status stage of the device request until
             * the Bulk-Only Mass Storage Reset is complete.
             */
            return TRUE;
        case MSD_GET_MAX_LUN:
            /* check that it is a DEV2HOST request */
            if (((usbp->setup[0] & USB_RTYPE_DIR_MASK) != USB_RTYPE_DIR_DEV2HOST) ||
               (MSD_SETUP_LENGTH(usbp->setup) != 1) ||
               (MSD_SETUP_VALUE(usbp->setup) != 0))
            {
                return FALSE;
            }

            static uint8_t len_buf[1] = {0};
            /* stall to indicate that we don't support LUN */
            usbSetupTransfer(usbp, len_buf, 1, NULL);
            return TRUE;
        default:
            return FALSE;
            break;
        }
    }

    return FALSE;
}

/**
 * @brief Wait until the end-point interrupt handler has been called
 */
static void msd_wait_for_isr(USBMassStorageDriver *msdp) {

    /* sleep until it completes */
    chSysLock();
    chBSemWaitS(&msdp->bsem);
    chSysUnlock();
}

static void msd_handle_end_point_notification(USBDriver *usbp, usbep_t ep) {

    (void)usbp;
    (void)ep;

    chSysLockFromIsr();
    chBSemSignalI(&((USBMassStorageDriver *)usbp->param)->bsem);
    chSysUnlockFromIsr();
}

/**
 * @brief IN end-point 1 state
 */
static USBInEndpointState ep1_in_state;

/**
 * @brief OUT end-point 1 state
 */
static USBOutEndpointState ep1_out_state;

/**
 * @brief End-point 1 initialization structure
 */
static const USBEndpointConfig ep_data_config = {
    USB_EP_MODE_TYPE_BULK,
    NULL,
    msd_handle_end_point_notification,
    msd_handle_end_point_notification,
    USB_MS_EP_SIZE,
    USB_MS_EP_SIZE,
    &ep1_in_state,
    &ep1_out_state,
    1,
    NULL
};

/**
 * @brief Handles the USB driver global events
 */
static void msd_usb_event(USBDriver *usbp, usbevent_t event) {

    USBMassStorageDriver *msdp = (USBMassStorageDriver *)usbp->param;

    switch (event) {
    case USB_EVENT_RESET:
        return;
    case USB_EVENT_ADDRESS:
        return;
    case USB_EVENT_CONFIGURED:
        chSysLockFromIsr();
        usbInitEndpointI(usbp, USB_MS_DATA_EP, &ep_data_config);
        /* initialise the thread */
        chBSemSignalI(&msdp->bsem);

        /* signal that the device is connected */
        chEvtBroadcastI(&msdp->evt_connected);
        chSysUnlockFromIsr();
        return;
    case USB_EVENT_SUSPEND:
        return;
    case USB_EVENT_WAKEUP:
        return;
    case USB_EVENT_STALLED:
        return;
    }
    return;
}

/**
 * @brief Global USB configuration
 */
static const USBConfig msd_usb_config = {
    msd_usb_event,
    msd_get_descriptor,
    msd_handle_requests,
    NULL
};

/**
 * @brief Changes the SCSI sense information
 */
static inline void msd_scsi_set_sense(USBMassStorageDriver *msdp, uint8_t key, uint8_t acode, uint8_t aqual) {
    msdp->sense.byte[2] = key;
    msdp->sense.byte[12] = acode;
    msdp->sense.byte[13] = aqual;
}

/**
 * @brief Processes an INQUIRY SCSI command
 */
bool_t msd_scsi_process_inquiry(USBMassStorageDriver *msdp) {

    msd_cbw_t *cbw = &(msdp->cbw);

    /* check the EVPD bit (Vital Product Data) */
    if (cbw->scsi_cmd_data[1] & 0x01) {

        /* check the Page Code byte to know the type of product data to reply */
        switch (cbw->scsi_cmd_data[2]) {

        /* unit serial number */
        case 0x80: {
            uint8_t response[] = {'0'}; /* TODO */
            usbPrepareTransmit(msdp->config->usbp, USB_MS_DATA_EP, response, sizeof(response));
            chSysLock();
            usbStartTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
            chSysUnlock();
            msdp->result = TRUE;

            /* wait for ISR */
            return TRUE;
        }

        /* unhandled */
        default:
            msd_scsi_set_sense(msdp,
                               SCSI_SENSE_KEY_ILLEGAL_REQUEST,
                               SCSI_ASENSE_INVALID_FIELD_IN_CDB,
                               SCSI_ASENSEQ_NO_QUALIFIER);
            return FALSE;
        }
    }
    else
    {
        static const msd_scsi_inquiry_response_t inquiry = {
            0x00,                        /* direct access block device  */
            0x80,                        /* removable                   */
            0x04,                         /* SPC-2                       */
            0x02,                        /* response data format        */
            0x20,                        /* response has 0x20 + 4 bytes */
            0x00,
            0x00,
            0x00,
            "Chibios",
            "Mass Storage",
            {'v', CH_KERNEL_MAJOR + '0', '.', CH_KERNEL_MINOR + '0'},
        };

        usbPrepareTransmit(msdp->config->usbp, USB_MS_DATA_EP, (uint8_t *)&inquiry,
                           sizeof(msd_scsi_inquiry_response_t));

        chSysLock();
        usbStartTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
        chSysUnlock();

        msdp->result = TRUE;

        /* wait for ISR */
        return TRUE;
    }
}

/**
 * @brief Processes a REQUEST_SENSE SCSI command
 */
bool_t msd_scsi_process_request_sense(USBMassStorageDriver *msdp) {

    usbPrepareTransmit(msdp->config->usbp, USB_MS_DATA_EP, (uint8_t *)&msdp->sense,
                       sizeof(msdp->sense));

    chSysLock();
    usbStartTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
    chSysUnlock();

    msdp->result = TRUE;

    /* wait for ISR */
    return TRUE;
}

/**
 * @brief Processes a READ_CAPACITY_10 SCSI command
 */
bool_t msd_scsi_process_read_capacity_10(USBMassStorageDriver *msdp) {

    static msd_scsi_read_capacity_10_response_t response;

    response.block_size = swap_uint32(msdp->block_dev_info.blk_size);
    response.last_block_addr = swap_uint32(msdp->block_dev_info.blk_num-1);

    usbPrepareTransmit(msdp->config->usbp, USB_MS_DATA_EP, (uint8_t *)&response, sizeof(response));

    chSysLock();
    usbStartTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
    chSysUnlock();

    msdp->result = TRUE;

    /* wait for ISR */
    return TRUE;
}

/**
 * @brief Processes a SEND_DIAGNOSTIC SCSI command
 */
bool_t msd_scsi_process_send_diagnostic(USBMassStorageDriver *msdp) {

    msd_cbw_t *cbw = &(msdp->cbw);

    if (!(cbw->scsi_cmd_data[1] & (1 << 2))) {
        /* only self-test supported - update SENSE key and fail the command */
        msd_scsi_set_sense(msdp,
                           SCSI_SENSE_KEY_ILLEGAL_REQUEST,
                           SCSI_ASENSE_INVALID_FIELD_IN_CDB,
                           SCSI_ASENSEQ_NO_QUALIFIER);
        return FALSE;
    }

    /* TODO: actually perform the test */
    msdp->result = TRUE;

    /* don't wait for ISR */
    return FALSE;
}

/**
 * @brief Processes a READ_WRITE_10 SCSI command
 */
bool_t msd_scsi_process_start_read_write_10(USBMassStorageDriver *msdp) {

    msd_cbw_t *cbw = &(msdp->cbw);

    if ((cbw->scsi_cmd_data[0] == SCSI_CMD_WRITE_10) && blkIsWriteProtected(msdp->config->bbdp)) {
        /* device is write protected and a write has been issued */
        /* block address is invalid, update SENSE key and return command fail */
        msd_scsi_set_sense(msdp,
                           SCSI_SENSE_KEY_DATA_PROTECT,
                           SCSI_ASENSE_WRITE_PROTECTED,
                           SCSI_ASENSEQ_NO_QUALIFIER);
        msdp->result = FALSE;

        /* don't wait for ISR */
        return FALSE;
    }

    uint32_t rw_block_address = swap_uint32(*(uint32_t *)&cbw->scsi_cmd_data[2]);
    uint16_t total = swap_uint16(*(uint16_t *)&cbw->scsi_cmd_data[7]);
    uint16_t i = 0;

    if (rw_block_address >= msdp->block_dev_info.blk_num) {
        /* block address is invalid, update SENSE key and return command fail */
        msd_scsi_set_sense(msdp,
                           SCSI_SENSE_KEY_DATA_PROTECT,
                           SCSI_ASENSE_WRITE_PROTECTED,
                           SCSI_ASENSEQ_NO_QUALIFIER);
        msdp->result = FALSE;

        /* don't wait for ISR */
        return FALSE;
    }

    if (cbw->scsi_cmd_data[0] == SCSI_CMD_WRITE_10) {
        /* process a write command */

        /* get the first packet */
        usbPrepareReceive(msdp->config->usbp, USB_MS_DATA_EP, rw_buf[i % 2],
                          msdp->block_dev_info.blk_size);

        chSysLock();
        usbStartReceiveI(msdp->config->usbp, USB_MS_DATA_EP);
        chSysUnlock();

        msd_wait_for_isr(msdp);

        /* loop over each block */
        for (i = 0; i < total; i++) {

            if (i < (total - 1)) {
                /* there is at least one block of data left to be read over USB */
                /* queue this read before issuing the blocking write */
                usbPrepareReceive(msdp->config->usbp, USB_MS_DATA_EP, rw_buf[(i + 1) % 2],
                                  msdp->block_dev_info.blk_size);

                chSysLock();
                usbStartReceiveI(msdp->config->usbp, USB_MS_DATA_EP);
                chSysUnlock();
            }

            /* now write the block to the block device */
            if (blkWrite(msdp->config->bbdp, rw_block_address++, rw_buf[i % 2], 1) == CH_FAILED) {
                /* TODO: handle this */
                chSysHalt();
            }

            if (i < (total - 1)) {
                /* now wait for the USB event to complete */
                msd_wait_for_isr(msdp);
            }
        }
    } else {
        /* process a read command */

        i = 0;

        /* read the first block from block device */
        if (blkRead(msdp->config->bbdp, rw_block_address++, rw_buf[i % 2], 1) == CH_FAILED) {
            /* TODO: handle this */
            chSysHalt();
        }

        /* loop over each block */
        for (i = 0; i < total; i++) {
            /* transmit the block */
            usbPrepareTransmit(msdp->config->usbp, USB_MS_DATA_EP, rw_buf[i % 2],
                               msdp->block_dev_info.blk_size);

            chSysLock();
            usbStartTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
            chSysUnlock();

            if (i < (total - 1)) {
                /* there is at least one more block to be read from device */
                /* so read that whilst the USB transfer takes place */
                if (blkRead(msdp->config->bbdp, rw_block_address++, rw_buf[(i + 1) % 2], 1) == CH_FAILED) {
                    /* TODO: handle this */
                    chSysHalt();
                }
            }

            /* wait for the USB event to complete */
            msd_wait_for_isr(msdp);
        }
    }

    msdp->result = TRUE;

    /* don't wait for ISR */
    return FALSE;
}

/**
 * @brief Processes a START_STOP_UNIT SCSI command
 */
bool_t msd_scsi_process_start_stop_unit(USBMassStorageDriver *msdp) {

    if ((msdp->cbw.scsi_cmd_data[4] & 0x03) == 0x02) {
        /* device has been ejected */
        chEvtBroadcast(&msdp->evt_ejected);
        msdp->state = MSD_EJECTED;
    }

    msdp->result = TRUE;

    /* don't wait for ISR */
    return FALSE;
}

/**
 * @brief Processes a MODE_SENSE_6 SCSI command
 */
bool_t msd_scsi_process_mode_sense_6(USBMassStorageDriver *msdp) {

    /* Send an empty header response with the Write Protect flag status */
    /* TODO set byte3 to 0x80 if disk is read only */
    static uint8_t response[4] = {0x00, 0x00, 0x00, 0x00};

    usbPrepareTransmit(msdp->config->usbp, USB_MS_DATA_EP, response, sizeof(response));

    chSysLock();
    usbStartTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
    chSysUnlock();

    msdp->result = TRUE;

    /* wait for ISR */
    return TRUE;
}

/**
 * @brief Processes a READ_FORMAT_CAPACITIES SCSI command
 */
bool_t msd_scsi_process_read_format_capacities(USBMassStorageDriver *msdp) {

    msd_scsi_read_format_capacities_response_t response;
    response.capacity_list_length = 1;
    response.block_count = swap_uint32(msdp->block_dev_info.blk_num);
    response.desc_and_block_length = swap_uint32((0x02 << 24) | (msdp->block_dev_info.blk_size & 0x00FFFFFF));

    usbPrepareTransmit(msdp->config->usbp, USB_MS_DATA_EP, (const uint8_t*)&response, sizeof(response));

    chSysLock();
    usbStartTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
    chSysUnlock();

    msdp->result = TRUE;

    /* wait for ISR */
    return TRUE;
}

/**
 * @brief Waits for a new command block
 */
bool_t msd_wait_for_command_block(USBMassStorageDriver *msdp) {

    usbPrepareReceive(msdp->config->usbp, USB_MS_DATA_EP, (uint8_t *)&msdp->cbw, sizeof(msdp->cbw));

    chSysLock();
    usbStartReceiveI(msdp->config->usbp, USB_MS_DATA_EP);
    chSysUnlock();

    msdp->state = MSD_READ_COMMAND_BLOCK;

    /* wait for ISR */
    return TRUE;
}

/**
 * @brief Reads a newly received command block
 */
bool_t msd_read_command_block(USBMassStorageDriver *msdp) {

    msd_cbw_t *cbw = &(msdp->cbw);

    /* by default transition back to the idle state */
    msdp->state = MSD_IDLE;

    /* check the command */
    if ((cbw->signature != MSD_CBW_SIGNATURE) ||
        (cbw->lun > 0) ||
        ((cbw->data_len > 0) && (cbw->flags & 0x1F)) ||
        (cbw->scsi_cmd_len == 0) ||
        (cbw->scsi_cmd_len > 16)) {

        /* stall both IN and OUT endpoints */
        chSysLock();
        usbStallReceiveI(msdp->config->usbp, USB_MS_DATA_EP);
        chSysUnlock();

        /* don't wait for ISR */
        return FALSE;
    }

    bool_t sleep = FALSE;

    /* check the command */
    switch (cbw->scsi_cmd_data[0]) {
    case SCSI_CMD_INQUIRY:
        sleep = msd_scsi_process_inquiry(msdp);
        break;
    case SCSI_CMD_REQUEST_SENSE:
        sleep = msd_scsi_process_request_sense(msdp);
        break;
    case SCSI_CMD_READ_CAPACITY_10:
        sleep = msd_scsi_process_read_capacity_10(msdp);
        break;
    case SCSI_CMD_READ_10:
    case SCSI_CMD_WRITE_10:
        if (msdp->config->rw_activity_callback)
            msdp->config->rw_activity_callback(TRUE);
        sleep = msd_scsi_process_start_read_write_10(msdp);
        if (msdp->config->rw_activity_callback)
            msdp->config->rw_activity_callback(FALSE);
        break;
    case SCSI_CMD_SEND_DIAGNOSTIC:
        sleep = msd_scsi_process_send_diagnostic(msdp);
        break;
    case SCSI_CMD_MODE_SENSE_6:
        sleep = msd_scsi_process_mode_sense_6(msdp);
        break;
    case SCSI_CMD_START_STOP_UNIT:
        sleep = msd_scsi_process_start_stop_unit(msdp);
        break;
    case SCSI_CMD_READ_FORMAT_CAPACITIES:
        sleep = msd_scsi_process_read_format_capacities(msdp);
        break;
    case SCSI_CMD_TEST_UNIT_READY:
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
    case SCSI_CMD_VERIFY_10:
        /* don't handle */
        msdp->result = TRUE;
        break;
    default:
        msd_scsi_set_sense(msdp,
                           SCSI_SENSE_KEY_ILLEGAL_REQUEST,
                           SCSI_ASENSE_INVALID_COMMAND,
                           SCSI_ASENSEQ_NO_QUALIFIER);

        /* stall IN endpoint */
        chSysLock();
        usbStallTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
        chSysUnlock();

        cbw->data_len = 0;
        return FALSE;
    }

    cbw->data_len = 0;

    if (msdp->result) {
        /* update sense with success status */
        msd_scsi_set_sense(msdp,
                           SCSI_SENSE_KEY_GOOD,
                           SCSI_ASENSE_NO_ADDITIONAL_INFORMATION,
                           SCSI_ASENSEQ_NO_QUALIFIER);
    } else {
        /* stall IN endpoint */
        chSysLock();
        usbStallTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
        chSysUnlock();

        cbw->data_len = 0;
        return FALSE;
    }

    if (sleep)
        msd_wait_for_isr(msdp);

    msd_csw_t *csw = &(msdp->csw);

    if (!msdp->result && cbw->data_len) {
        /* still bytes left to send, this is too early to send CSW? */
        chSysLock();
        usbStallReceiveI(msdp->config->usbp, USB_MS_DATA_EP);
        usbStallTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
        chSysUnlock();

        return FALSE;
    }

    csw->status = (msdp->result) ? MSD_COMMAND_PASSED : MSD_COMMAND_FAILED;
    csw->signature = MSD_CSW_SIGNATURE;
    csw->data_residue = cbw->data_len;
    csw->tag = cbw->tag;

    usbPrepareTransmit(msdp->config->usbp, USB_MS_DATA_EP, (uint8_t *)csw, sizeof(*csw));

    chSysLock();
    usbStartTransmitI(msdp->config->usbp, USB_MS_DATA_EP);
    chSysUnlock();

    /* wait on ISR */
    return TRUE;
}

/**
 * @brief Mass storage thread that processes commands
 */
static WORKING_AREA(mass_storage_thread_wa, 1024);
static msg_t mass_storage_thread(void *arg) {

    USBMassStorageDriver *msdp = (USBMassStorageDriver *)arg;

    chRegSetThreadName("USB-MSD");

    bool_t wait_for_isr = FALSE;

    /* wait for the usb to be initialised */
    msd_wait_for_isr(msdp);

    while (TRUE) {
        wait_for_isr = FALSE;

        /* wait on data depending on the current state */
        switch(msdp->state) {
        case MSD_IDLE:
            wait_for_isr = msd_wait_for_command_block(msdp);
            break;
        case MSD_READ_COMMAND_BLOCK:
            wait_for_isr = msd_read_command_block(msdp);
            break;
        case MSD_EJECTED:
            /* disconnect usb device */
            usbDisconnectBus(msdp->config->usbp);
            usbStop(msdp->config->usbp);
            chThdExit(0);
            return 0;
        }

        /* wait until the ISR wakes thread */
        if (wait_for_isr)
            msd_wait_for_isr(msdp);
    }

    return 0;
}

static Thread *msd_thread = NULL;

/**
 * @brief Initialize USB mass storage on the given USB driver, using the given block device
 */
void msdInit(USBMassStorageDriver *msdp, const USBMassStorageConfig *config) {

    chDbgCheck(msdp != NULL, "msdInit");
    chDbgCheck(config != NULL, "msdInit");

    uint8_t i;
    msdp->config = config;
    msdp->state = MSD_IDLE;

    chEvtInit(&msdp->evt_connected);
    chEvtInit(&msdp->evt_ejected);

    /* initialise binary semaphore as taken */
    chBSemInit(&msdp->bsem, TRUE);

    /* initialise sense values to zero */
    for (i = 0; i < sizeof(msdp->sense.byte); i++)
        msdp->sense.byte[i] = 0x00;

    /* response code = 0x70, additional sense length = 0x0A */
    msdp->sense.byte[0] = 0x70;
    msdp->sense.byte[7] = 0x0A;

    /* make sure block device is working and get info */
    while (TRUE) {
        blkstate_t state = blkGetDriverState(config->bbdp);
        if(state == BLK_READY)
            break;
        chThdSleepMilliseconds(50);
    }

    blkGetInfo(config->bbdp, &msdp->block_dev_info);

    usbDisconnectBus(config->usbp);
    chThdSleepMilliseconds(1000);
    config->usbp->param = (void *)msdp;

    usbStart(config->usbp, &msd_usb_config);
    usbConnectBus(config->usbp);

    if (msd_thread == NULL)
        msd_thread = chThdCreateStatic(mass_storage_thread_wa, sizeof(mass_storage_thread_wa), NORMALPRIO, mass_storage_thread, msdp);
}
