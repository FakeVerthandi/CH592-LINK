#include "link_session.h"
static uint8_t current_owner;
uint8_t link_session_claim(uint8_t owner)
{
    if (!owner || (current_owner && current_owner != owner)) return 0;
    current_owner = owner;
    return 1;
}
void link_session_release(uint8_t owner)
{
    if (current_owner == owner) current_owner = LINK_OWNER_NONE;
}
uint8_t link_session_owner(void) { return current_owner; }
