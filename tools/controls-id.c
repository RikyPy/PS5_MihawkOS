/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only harness adapter. Use exactly the production parser and defaults;
 * the harness never rewrites the player's mouse assignments. */
#include "pw_controls.h"
#include <stdio.h>
int main(int argc, char **argv)
{
    pw_controls controls;
    if (argc != 2) return 2;
    int status = pw_controls_load(&controls, argv[1]);
    printf("%s %d\n", pw_controls_button_id(controls.button[0]), status);
    return 0;
}
