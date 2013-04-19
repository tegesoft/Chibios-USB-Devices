/**
 * @file    usb_msd.h
 * @brief   USB mass storage driver and functions
 */

#ifndef _USB_MSD_H_
#define _USB_MSD_H_

#include "ch.h"
#include "hal.h"

/**
 * @brief Command Block Wrapper structure
 */
PACK_STRUCT_BEGIN typedef struct {
	uint32_t signature;
	uint32_t tag;
	uint32_t data_len;
	uint8_t flags;
	uint8_t lun;
	uint8_t scsi_cmd_len;
	uint8_t scsi_cmd_data[16];
} PACK_STRUCT_STRUCT msd_cbw_t PACK_STRUCT_END;

/**
 * @brief Command Status Wrapper structure
 */
PACK_STRUCT_BEGIN typedef struct {
	uint32_t signature;
	uint32_t tag;
	uint32_t data_residue;
	uint8_t status;
} PACK_STRUCT_STRUCT msd_csw_t PACK_STRUCT_END;

/**
 * @brief Structure holding sense data (status/error information)
 */
PACK_STRUCT_BEGIN typedef struct {
		uint8_t byte[18];
} PACK_STRUCT_STRUCT msd_scsi_sense_response_t PACK_STRUCT_END;

/**
 * @brief Possible states for the USB mass storage driver
 */
typedef enum {
    MSD_IDLE,
    MSD_READ_COMMAND_BLOCK,
    MSD_EJECTED
} msd_state_t;

/**
 * @brief Declares a valid USBDecriptor instance from device properties
 */
#define MSD_DECLARE_DEVICE_DESCRIPTOR(name, vendor_id, product_id)  \
    static const uint8_t name##_data_[] = {                         \
        USB_DESC_DEVICE(0x0200,        /* bcdUSB (2.0).          */ \
                        0x00,          /* bDeviceClass (None).   */ \
                        0x00,          /* bDeviceSubClass.       */ \
                        0x00,          /* bDeviceProtocol.       */ \
                        0x40,          /* Control Endpoint Size. */ \
                        vendor_id,     /* idVendor (ST).         */ \
                        product_id,    /* idProduct.             */ \
                        0x0100,        /* bcdDevice.             */ \
                        1,             /* iManufacturer.         */ \
                        2,             /* iProduct.              */ \
                        3,             /* iSerialNumber.         */ \
                        1)             /* bNumConfigurations.    */ \
    };                                                              \
    static const USBDescriptor name = {                             \
        sizeof name##_data_,                                        \
        name##_data_                                                \
    }

/**
 * @brief Declares a valid USBDecriptor instance from a string
 * @note  The string will be used as a Unicode string, therefore each character
 *        must be followed by a 0. The length argument must include these zeroes.
 *        Since the string is a variable-length list of character literals,
 *        it cannot be passed directly to the macro, it must be first defined
 *        as a macro itself.
 *        Example:
 *          #define VENDOR_STRING 'm', 0, 'y', 0, 'c', 0, 'o', 0, 'm', 0, 'p', 0
 *          MSD_DECLARE_DESCRIPTOR(mycomp_descriptor, 12, VENDOR_STRING);
 */
#define MSD_DECLARE_STRING_DESCRIPTOR(name, length, string) \
    static const uint8_t name##_data_[] = {                 \
        USB_DESC_BYTE(length + 2),                          \
        USB_DESC_BYTE(USB_DESCRIPTOR_STRING),               \
        string                                              \
    };                                                      \
    static const USBDescriptor name = {                     \
        sizeof name##_data_,                                \
        name##_data_                                        \
    }

/**
 * @brief Driver configuration structure
 */
typedef struct {
  /**
   * @brief USB driver to use for communication
   */
  USBDriver *usbp;

  /**
   * @brief Block device to use for storage
   */
  BaseBlockDevice *bbdp;

  /**
   * @brief Optional callback that will be called whenever there is
   *        read/write activity
   * @note  The callback is called with argument TRUE when activity starts,
   *        and FALSE when activity stops.
   */
  void (*rw_activity_callback)(bool_t);

  /**
   * @brief Device description
   * @note  To define such a valid USBDescriptor, see the MSD_DECLARE_DEVICE_DESCRIPTOR macro.
   *        If null, a default device description is used.
   */
  const USBDescriptor* device_descriptor;

  /**
   * @brief Vendor description
   * @note  To define such a valid USBDescriptor, see the MSD_DECLARE_STRING_DESCRIPTOR macro.
   *        If null, a default vendor description is used.
   */
  const USBDescriptor* vendor_descriptor;

  /**
   * @brief Product description
   * @note  To define such a valid USBDescriptor, see the MSD_DECLARE_STRING_DESCRIPTOR macro.
   *        If null, a default product description is used.
   */
  const USBDescriptor* product_descriptor;

  /**
   * @brief Serial number description
   * @note  To define such a valid USBDescriptor, see the MSD_DECLARE_STRING_DESCRIPTOR macro.
   *        If null, a default serial number description is used.
   *        This description string must contain at least 12 valid digits.
   */
  const USBDescriptor* serial_number_descriptor;

} USBMassStorageConfig;

/**
 * @brief   USB mass storage driver structure.
 * @details This structure holds all the states and members of a USB mass
 *          storage driver.
 */
typedef struct {
    const USBMassStorageConfig* config;
	BinarySemaphore bsem;
	EventSource evt_connected, evt_ejected;
	BlockDeviceInfo block_dev_info;
	msd_state_t state;
	msd_cbw_t cbw;
	msd_csw_t csw;
	msd_scsi_sense_response_t sense;
	bool_t result;
} USBMassStorageDriver;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   Initialize USB mass storage with the given configuration.
 * @details This function is sufficient to have USB mass storage running, it internally
 *          runs a thread that handles USB requests and transfers.
 *          The block device must be connected but no file system must be mounted,
 *          everything is handled by the host system.
 */
void msdInit(USBMassStorageDriver *msdp, const USBMassStorageConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* _USB_MSD_H_ */
