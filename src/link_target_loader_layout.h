#ifndef LINK_TARGET_LOADER_LAYOUT_H
#define LINK_TARGET_LOADER_LAYOUT_H

#define TARGET_LOADER_BASE      0x20000000u
#define TARGET_LOADER_CODE_MAX  0x600u
#define TARGET_LOADER_DATA      0x20000600u
#define TARGET_LOADER_MAILBOX   0x20000700u
#define TARGET_LOADER_STACK     0x20000C00u
#define TARGET_LOADER_MAX       256u
#define TARGET_FLASH_BASE       0x08000000u
#define TARGET_VERIFY_CHUNK     1024u

/* Mailbox layout (8 words at TARGET_LOADER_MAILBOX):
 *   word 0: cmd_word   = (op << 28) | (flags << 16) | arg0
 *   word 1: offset     = byte offset from TARGET_FLASH_BASE
 *   word 2: flash_size = active target Flash size in bytes
 *   word 3: checksum   = ~(word0 ^ word1 ^ word2)
 *   word 4: out_value  = CRC32 written before publishing word 0 result
 *
 * Completion written to word 0:
 *   RESULT_TAG | (status << 9) | completed_bytes
 */
#define TARGET_OP_SHIFT         28u
#define TARGET_OP_WRITE         0u
#define TARGET_OP_ERASE         1u
#define TARGET_OP_VERIFY        2u

#define TARGET_FLAG_SHIFT       16u
#define TARGET_FLAG_F3_OBR      0x01u

#define TARGET_RESULT_WRITE_TAG  0xb4100000u
#define TARGET_RESULT_VERIFY_TAG 0xb4110000u
#define TARGET_RESULT_ERASE_TAG  0xb4120000u
#define TARGET_RESULT_MASK       0xffff0000u
#define TARGET_MAILBOX_CRC       (TARGET_LOADER_MAILBOX + 16u)

enum {
    TARGET_LOADER_DONE = 0,
    TARGET_LOADER_BAD_REQUEST,
    TARGET_LOADER_CLOCK_OFF,
    TARGET_LOADER_BUSY_TIMEOUT,
    TARGET_LOADER_LOCKED,
    TARGET_LOADER_NOT_BLANK,
    TARGET_LOADER_PROGRAM_ERROR,
    TARGET_LOADER_PROTECTED
};

#endif
