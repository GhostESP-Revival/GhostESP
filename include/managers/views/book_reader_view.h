#ifndef BOOK_READER_VIEW_H
#define BOOK_READER_VIEW_H

#include "managers/display_manager.h"

#ifdef CONFIG_CROWPANEL_EPAPER_42

void book_reader_create(void);
void book_reader_destroy(void);
void book_reader_get_callback(void **callback);

extern View book_reader_view;

#endif /* CONFIG_CROWPANEL_EPAPER_42 */

#endif /* BOOK_READER_VIEW_H */
