#ifndef BADUSB_VIEW_H
#define BADUSB_VIEW_H

#include "managers/display_manager.h"

extern View badusb_view;

// Called by command handler when S3 sends status updates over GhostLink
// status: "waiting", "running", or "done"
void badusb_view_update_status(const char *status);

/* Deep-link: accept a legacy bare name, a path relative to ghostesp/badusb,
 * or an absolute SD path within that directory. Safe before view creation;
 * applied on create or immediately if the view is already live. */
void badusb_view_open_script(const char *name);

#endif // BADUSB_VIEW_H
