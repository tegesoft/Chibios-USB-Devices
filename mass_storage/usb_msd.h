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
