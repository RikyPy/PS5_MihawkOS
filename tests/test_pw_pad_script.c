/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host test of the pad script (src/pw_pad_script.c): parsing, what is held when, events fired once.
 */
#include "../src/pw_pad_script.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct PwPadScript script;

int main(int argc, char **argv)
{
    /* A whole script, comments and blank lines included. */
    const char *text =
        "# load the test save\n"
        "anchor frame\n"
        "\n"
        "12.5 cross\n"
        "14 down 0.6   # walk off the bed\n"
        "20 lx -1 0.8\n"
        "20.4 lx 0.5 0.2\n"
        "21 r2 1 0.1\n"
        "22 capture\n"
        "90 stop\n";
    assert(pw_pad_script_parse(text, &script) == 0);
    assert(script.from_frame && script.count == 7);

    struct PwPadScriptState state;
    pw_pad_script_state(&script, 12499, &state);
    assert(state.buttons == 0);
    pw_pad_script_state(&script, 12500, &state);
    assert(state.buttons == PW_PAD_SCRIPT_CROSS);
    pw_pad_script_state(&script, 12500 + PW_PAD_SCRIPT_TAP_MS - 1, &state);
    assert(state.buttons == PW_PAD_SCRIPT_CROSS);
    pw_pad_script_state(&script, 12500 + PW_PAD_SCRIPT_TAP_MS, &state);
    assert(state.buttons == 0);
    pw_pad_script_state(&script, 14599, &state);
    assert(state.buttons == PW_PAD_SCRIPT_DOWN);
    pw_pad_script_state(&script, 14600, &state);
    assert(state.buttons == 0);

    /* Axes: a later step overrides an earlier one while both hold, then the earlier one again. */
    pw_pad_script_state(&script, 20100, &state);
    assert(state.axis_set[PW_PAD_SCRIPT_LX] && state.axis[PW_PAD_SCRIPT_LX] == -1.f);
    assert(!state.axis_set[PW_PAD_SCRIPT_LY]);
    pw_pad_script_state(&script, 20500, &state);
    assert(state.axis[PW_PAD_SCRIPT_LX] == 0.5f);
    pw_pad_script_state(&script, 20700, &state);
    assert(state.axis[PW_PAD_SCRIPT_LX] == -1.f);
    pw_pad_script_state(&script, 20800, &state);
    assert(!state.axis_set[PW_PAD_SCRIPT_LX]);
    pw_pad_script_state(&script, 21050, &state);
    assert(state.axis_set[PW_PAD_SCRIPT_R2] && state.axis[PW_PAD_SCRIPT_R2] == 1.f);

    /* Events fire once each when ticks pass over them, whatever the tick length. */
    const struct PwPadScriptStep *events[4];
    unsigned fired_capture = 0, fired_stop = 0, last = 0;
    for (unsigned now = 0; now <= 100000; now += 8) {
        const unsigned n = pw_pad_script_events(&script, last, now, events, 4);
        for (unsigned i = 0; i < n; ++i) {
            if (events[i]->kind == PW_PAD_SCRIPT_CAPTURE) { ++fired_capture; assert(events[i]->start_ms == 22000); }
            if (events[i]->kind == PW_PAD_SCRIPT_STOP) { ++fired_stop; assert(events[i]->start_ms == 90000); }
        }
        last = now;
    }
    assert(fired_capture == 1 && fired_stop == 1);
    /* An event at time 0 fires on the first call. */
    assert(pw_pad_script_parse("0 capture\n", &script) == 0);
    assert(pw_pad_script_events(&script, 0, 8, events, 4) == 1);

    /* Raw readings as the pad gives them. */
    assert(pw_pad_script_raw(PW_PAD_SCRIPT_LX, 0.f) == 128);
    assert(pw_pad_script_raw(PW_PAD_SCRIPT_LX, -1.f) == 1);
    assert(pw_pad_script_raw(PW_PAD_SCRIPT_LY, 1.f) == 255);
    assert(pw_pad_script_raw(PW_PAD_SCRIPT_L2, 0.f) == 0);
    assert(pw_pad_script_raw(PW_PAD_SCRIPT_R2, 1.f) == 255);

    /* Refusals name the first bad line and keep what came before it. */
    assert(pw_pad_script_parse("1 cross\n2 jump\n3 cross\n", &script) == 2 && script.count == 1);
    assert(pw_pad_script_parse("1 lx 2 0.5\n", &script) == 1);      /* outside -1..1 */
    assert(pw_pad_script_parse("1 l2 -0.5 0.5\n", &script) == 1);   /* a trigger is 0..1 */
    assert(pw_pad_script_parse("1 lx 0.5\n", &script) == 1);        /* an axis needs a duration */
    assert(pw_pad_script_parse("-1 cross\n", &script) == 1);
    assert(pw_pad_script_parse("1 cross 0\n", &script) == 1);
    assert(pw_pad_script_parse("1 capture now\n", &script) == 1);
    assert(pw_pad_script_parse("anchor sometime\n", &script) == 1);
    assert(pw_pad_script_parse("x cross\n", &script) == 1);
    char long_line[400];
    memset(long_line, ' ', sizeof(long_line) - 1);
    long_line[sizeof(long_line) - 1] = 0;
    assert(pw_pad_script_parse(long_line, &script) == 1);
    /* Without a final newline, and with CRLF line ends. */
    assert(pw_pad_script_parse("1 cross\r\n2 circle", &script) == 0 && script.count == 2 && !script.from_frame);

    /* A finger on the touchpad, moved between two steps; a bad position or a missing duration is refused. */
    {
        struct PwPadScript touch;
        assert(pw_pad_script_parse("1 touch 100 200 0.5\n1.5 touch 300 200 0.5\n", &touch) == 0 && touch.count == 2);
        pw_pad_script_state(&touch, 1200, &state);
        assert(state.touching && state.touch_x == 100 && state.touch_y == 200);
        pw_pad_script_state(&touch, 1700, &state);
        assert(state.touching && state.touch_x == 300);
        pw_pad_script_state(&touch, 2100, &state);
        assert(!state.touching);
        assert(pw_pad_script_parse("1 touch 2000 200 0.5\n", &touch) == 1);
        assert(pw_pad_script_parse("1 touch 100 200\n", &touch) == 1);
    }

    /* Parse the actual readiness-anchored battery scripts too. */
    for (int i = 1; i < argc; ++i) {
        char text[8192];
        FILE *file = fopen(argv[i], "r");
        assert(file);
        const size_t bytes = fread(text, 1, sizeof(text) - 1, file);
        assert(!ferror(file) && feof(file));
        fclose(file);
        text[bytes] = 0;
        assert(!pw_pad_script_parse(text, &script) && script.from_frame && script.count);
    }
    puts("pad script: parse, state, events, raw readings, touches and refusals: PASS");
    return 0;
}
