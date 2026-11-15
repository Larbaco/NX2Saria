// The amiibo daily limit (amiibo_daily.c): the patch set amiibo_daily's switch.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Called at every controller poll the game makes (ov_input.c npad_states): switches the set to follow
// NX2SARIA_AMIIBO_DAILY_LIMIT, so a change in the menu reaches the next figure the game reads.
void amiibo_daily_poll(void);
// 1 when the game version has the patch set amiibo_daily and nothing keeps it off (NX2SARIA_AMIIBO_DAILY_PATCH=0, a mod
// patching its site): the Game tab shows the row then. The set itself runs only while the setting is 0.
int amiibo_daily_available(void);

#ifdef __cplusplus
}
#endif
