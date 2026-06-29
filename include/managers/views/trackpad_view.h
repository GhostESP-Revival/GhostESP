#ifndef TRACKPAD_VIEW_H
#define TRACKPAD_VIEW_H

#include "managers/display_manager.h"

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)

extern View trackpad_view;

// Optional: set the view to return to when the user backs out of the trackpad.
// Defaults to the badusb main view if unset.
void trackpad_view_set_return_view(View *view);

#endif // CONFIG_HAS_BADUSB || CONFIG_HAS_BADUSB_REMOTE

#endif // TRACKPAD_VIEW_H
