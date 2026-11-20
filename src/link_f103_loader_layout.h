#ifndef LINK_F103_LOADER_LAYOUT_H
#define LINK_F103_LOADER_LAYOUT_H

#define F103_LOADER_BASE     0x20000000u
#define F103_LOADER_DATA     0x20000400u
#define F103_LOADER_MAILBOX  0x20000500u
#define F103_LOADER_STACK    0x20000800u
#define F103_LOADER_MAX      256u
#define F103_FLASH_BASE      0x08000000u
#define F103_FLASH_SIZE      0x10000u
#define F103_FLASH_PAGE      1024u

/* Write request = (byte count << 16) | flash offset.
 * Verify request = F103_REQUEST_VERIFY | flash page offset.
 * The second mailbox word complements the request. Verify writes the CRC
 * to the third word before publishing the tagged completion in the first. */
#define F103_REQUEST_VERIFY  0x80000000u
#define F103_RESULT_TAG      0xb4100000u
#define F103_VERIFY_TAG      0xb4110000u
#define F103_RESULT_MASK     0xffff0000u
#define F103_VERIFY_VALUE    (F103_LOADER_MAILBOX + 8u)
enum {
    F103_LOADER_DONE,
    F103_LOADER_BAD_REQUEST,
    F103_LOADER_CLOCK_OFF,
    F103_LOADER_BUSY_TIMEOUT,
    F103_LOADER_LOCKED,
    F103_LOADER_NOT_BLANK,
    F103_LOADER_PROGRAM_ERROR,
    F103_LOADER_PROTECTED
};
#endif
