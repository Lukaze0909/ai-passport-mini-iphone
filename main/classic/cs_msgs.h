// main/classic/cs_msgs.h -- 留言 (Messages): an SMS-style thread with the companion.
//
// The companion writes English text ({"op":"msg",...} on the command topic); the owner
// answers with voice through the call screen's take (record / send / delete,
// voice_begin "app":"msg"). The newest 50 messages live in the "msgs" flash
// partition (cs_store_*, cs_logic.h) and survive a reboot.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Opens the message log. Call once before the network starts.
void cs_msgs_init(void);
// MQTT task: stores a "msg" command and tells the shell. False: not a msg command.
bool cs_msgs_on_command(const char *json, size_t len);

// Shell task.
void cs_msgs_enter(void);                    // the Messages screen was shown (LVGL lock held)
void cs_msgs_exit(void);                     // leaving it: an unsent take is dropped
void cs_msgs_key(uint8_t btn);               // UP / DOWN / OK tap
void cs_msgs_changed(void);                  // the shared voice take changed
// A stored message from the companion was posted: refreshes the open thread (returns
// true) or counts it unread and writes "<name>: <text>" into preview.
bool cs_msgs_incoming(char *preview, size_t cap);
int cs_msgs_unread(void);
