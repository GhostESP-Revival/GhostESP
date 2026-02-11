#ifndef BADUSB_VIEW_H
#define BADUSB_VIEW_H

#include "managers/display_manager.h"

extern View badusb_view;

// Called by command handler when S3 sends status updates over GhostLink
// status: "waiting", "running", or "done"
void badusb_view_update_status(const char *status);

#endif // BADUSB_VIEW_H
