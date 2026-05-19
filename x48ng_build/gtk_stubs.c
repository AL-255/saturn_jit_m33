/* gtk_stubs.c — satisfy the unconditional FRONTEND_GTK call sites in
 * src/ui4x/src/api.c when this build is compiled without GTK4.
 *
 * The SDL backend is selected via --sdl on the command line. These stubs
 * exist solely so the linker is satisfied; they should never run if the
 * user doesn't pass --gtk.
 */

#include <stdio.h>
#include <stdlib.h>

static void die_no_gtk(const char *fn)
{
    fprintf(stderr,
            "x48ng: built without GTK support; %s is unimplemented. "
            "Use --sdl or --tui instead.\n",
            fn);
    exit(1);
}

void gtk_ui4x_init(void)                  { die_no_gtk("gtk_ui4x_init"); }
void gtk_ui4x_handle_pending_inputs(void) { die_no_gtk("gtk_ui4x_handle_pending_inputs"); }
void gtk_ui_refresh_lcd(void)             { die_no_gtk("gtk_ui_refresh_lcd"); }
void gtk_exit(void)                       { /* nothing to clean up */ }
