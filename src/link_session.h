#ifndef LINK_SESSION_H
#define LINK_SESSION_H
#include <stdint.h>
#define LINK_OWNER_NONE 0u
#define LINK_OWNER_USB 1u
#define LINK_OWNER_BLE 2u
/* Main-loop only: callbacks enqueue, never touch the SWD bus. */
uint8_t link_session_claim(uint8_t owner);
void link_session_release(uint8_t owner);
uint8_t link_session_owner(void);
#endif
