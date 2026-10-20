/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A timed script of pad input (pw_pad_script.h).
 */
#include "pw_pad_script.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name; uint32_t bit; } buttons[] = {
    {"cross", PW_PAD_SCRIPT_CROSS}, {"circle", PW_PAD_SCRIPT_CIRCLE}, {"square", PW_PAD_SCRIPT_SQUARE},
    {"triangle", PW_PAD_SCRIPT_TRIANGLE}, {"up", PW_PAD_SCRIPT_UP}, {"down", PW_PAD_SCRIPT_DOWN},
    {"left", PW_PAD_SCRIPT_LEFT}, {"right", PW_PAD_SCRIPT_RIGHT}, {"options", PW_PAD_SCRIPT_OPTIONS},
    {"l1", PW_PAD_SCRIPT_L1}, {"r1", PW_PAD_SCRIPT_R1}, {"l3", PW_PAD_SCRIPT_L3}, {"r3", PW_PAD_SCRIPT_R3},
    {"touchpad", PW_PAD_SCRIPT_TOUCH_PAD},
};
static const char *const axes[PW_PAD_SCRIPT_AXES] = {"lx", "ly", "rx", "ry", "l2", "r2"};

/* Seconds as a word ("12", "12.5"), to milliseconds; false for anything else or a negative time. */
static bool seconds(const char *word, unsigned *ms)
{
    char *end;
    const double value = strtod(word, &end);
    if (end == word || *end || !(value >= 0) || value > 86400) return false;
    *ms = (unsigned)lround(value * 1000);
    return true;
}

static bool number(const char *word, float *out)
{
    char *end;
    const double value = strtod(word, &end);
    if (end == word || *end || !(value >= -1e9 && value <= 1e9)) return false;   /* NaN fails both */
    *out = (float)value;
    return true;
}

static bool parse_line(char *line, struct PwPadScript *out)
{
    char *words[5];
    unsigned count = 0;
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    for (char *word = strtok(line, " \t\r"); word; word = strtok(NULL, " \t\r")) {
        if (count == 5) return false;
        words[count++] = word;
    }
    if (!count) return true;
    if (!strcmp(words[0], "anchor")) {
        if (count != 2) return false;
        if (!strcmp(words[1], "frame")) out->from_frame = true;
        else if (!strcmp(words[1], "start")) out->from_frame = false;
        else return false;
        return true;
    }
    struct PwPadScriptStep step = {0};
    if (count < 2 || out->count == PW_PAD_SCRIPT_MAX || !seconds(words[0], &step.start_ms)) return false;
    if (!strcmp(words[1], "capture") || !strcmp(words[1], "stop")) {
        if (count != 2) return false;
        step.kind = words[1][0] == 'c' ? PW_PAD_SCRIPT_CAPTURE : PW_PAD_SCRIPT_STOP;
        out->steps[out->count++] = step;
        return true;
    }
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        if (strcmp(words[1], buttons[i].name)) continue;
        if (count > 3) return false;
        step.kind = PW_PAD_SCRIPT_BUTTON;
        step.button = buttons[i].bit;
        step.duration_ms = PW_PAD_SCRIPT_TAP_MS;
        if (count == 3 && (!seconds(words[2], &step.duration_ms) || !step.duration_ms)) return false;
        out->steps[out->count++] = step;
        return true;
    }
    if (!strcmp(words[1], "touch")) {
        if (count != 5 || !number(words[2], &step.x) || !number(words[3], &step.y) ||
            !seconds(words[4], &step.duration_ms) || !step.duration_ms ||
            step.x < 0 || step.x > 1919 || step.y < 0 || step.y > 1079)
            return false;
        step.kind = PW_PAD_SCRIPT_TOUCH;
        out->steps[out->count++] = step;
        return true;
    }
    for (int axis = 0; axis < PW_PAD_SCRIPT_AXES; ++axis) {
        if (strcmp(words[1], axes[axis])) continue;
        if (count != 4 || !number(words[2], &step.value) || !seconds(words[3], &step.duration_ms) ||
            !step.duration_ms)
            return false;
        const float low = axis >= PW_PAD_SCRIPT_L2 ? 0.f : -1.f;
        if (step.value < low || step.value > 1.f) return false;
        step.kind = PW_PAD_SCRIPT_AXIS;
        step.axis = axis;
        out->steps[out->count++] = step;
        return true;
    }
    return false;
}

unsigned pw_pad_script_parse(const char *text, struct PwPadScript *out)
{
    memset(out, 0, sizeof(*out));
    unsigned number_of_line = 0;
    while (*text) {
        const char *end = strchr(text, '\n');
        const size_t length = end ? (size_t)(end - text) : strlen(text);
        char line[256];
        ++number_of_line;
        if (length >= sizeof(line)) return number_of_line;
        memcpy(line, text, length);
        line[length] = 0;
        if (!parse_line(line, out)) return number_of_line;
        text += length + (end ? 1 : 0);
    }
    return 0;
}

void pw_pad_script_state(const struct PwPadScript *script, unsigned now_ms, struct PwPadScriptState *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < script->count; ++i) {
        const struct PwPadScriptStep *step = &script->steps[i];
        if (now_ms < step->start_ms || now_ms - step->start_ms >= step->duration_ms) continue;
        if (step->kind == PW_PAD_SCRIPT_BUTTON) {
            out->buttons |= step->button;
        } else if (step->kind == PW_PAD_SCRIPT_AXIS) {
            out->axis_set[step->axis] = true;
            out->axis[step->axis] = step->value;
        } else if (step->kind == PW_PAD_SCRIPT_TOUCH) {
            out->touching = true;
            out->touch_x = (uint16_t)step->x;
            out->touch_y = (uint16_t)step->y;
        }
    }
}

unsigned pw_pad_script_events(const struct PwPadScript *script, unsigned from_ms, unsigned to_ms,
                              const struct PwPadScriptStep **out, unsigned max)
{
    unsigned written = 0;
    for (unsigned i = 0; i < script->count && written < max; ++i) {
        const struct PwPadScriptStep *step = &script->steps[i];
        if ((step->kind == PW_PAD_SCRIPT_CAPTURE || step->kind == PW_PAD_SCRIPT_STOP) &&
            step->start_ms >= from_ms && step->start_ms < to_ms)
            out[written++] = step;
    }
    return written;
}

uint8_t pw_pad_script_raw(int axis, float value)
{
    const float raw = axis >= PW_PAD_SCRIPT_L2 ? value * 255.f : 128.f + value * 127.f;
    return (uint8_t)lroundf(raw < 0 ? 0 : raw > 255 ? 255 : raw);
}
