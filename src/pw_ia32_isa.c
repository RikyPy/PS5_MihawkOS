/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_ia32_isa.h"
#include <stdatomic.h>

/* The .def's short names, for the length of the two expansions below only. */
#define PW_IA32_SEL_0 0
#define PW_IA32_SEL_1 1
#define PW_IA32_SEL_2 2
#define PW_IA32_SEL_3 3
#define PW_IA32_SEL_4 4
#define PW_IA32_SEL_5 5
#define PW_IA32_SEL_6 6
#define PW_IA32_SEL_7 7
#define SEL(reg, mod, rm) PW_IA32_SEL_##reg, PW_IA32_MOD_##mod, PW_IA32_SEL_##rm
#define OPS(a, b, c) { PW_IA32_OP_##a, PW_IA32_OP_##b, PW_IA32_OP_##c }
#define C PW_IA32_F_C
#define P PW_IA32_F_P
#define A PW_IA32_F_A
#define Z PW_IA32_F_Z
#define S PW_IA32_F_S
#define D PW_IA32_F_D
#define O PW_IA32_F_O
#define NOF 0
#define LOCK PW_IA32_ATTR_LOCK
#define REP PW_IA32_ATTR_REP
#define REPCC PW_IA32_ATTR_REPCC
#define BRANCH PW_IA32_ATTR_BRANCH
#define OPREG PW_IA32_ATTR_OPREG
#define O1 PW_IA32_ATTR_O1
#define PRIV PW_IA32_ATTR_PRIV
#define XACQ PW_IA32_ATTR_XACQ
#define XREL PW_IA32_ATTR_XREL
#define CMPPRED PW_IA32_ATTR_CMPPRED
#define UD PW_IA32_ATTR_UD
#define MRO PW_IA32_ATTR_MRO
#define ADDR PW_IA32_ATTR_ADDR
#define STACK PW_IA32_ATTR_STACK
#define STR PW_IA32_ATTR_STR
#define IMPL PW_IA32_ATTR_IMPL
#define MASK PW_IA32_ATTR_MASK
#define NO64 PW_IA32_ATTR_NO64
#define X64 PW_IA32_ATTR_X64
#define D64 PW_IA32_ATTR_D64
#define VEX PW_IA32_ATTR_VEX
#define VEXONLY PW_IA32_ATTR_VEXONLY
#define L0 PW_IA32_ATTR_L0
#define L1 PW_IA32_ATTR_L1
#define W0 PW_IA32_ATTR_W0
#define W1 PW_IA32_ATTR_W1
#define VLH PW_IA32_ATTR_VLH
#define NOV PW_IA32_ATTR_NOV
#define NOATT 0

typedef struct GridRow {
    uint8_t map, row;
    const char *codes;  /* 16 codes separated by spaces */
} GridRow;

static const GridRow grid[] = {
#define PW_IA32_GRID(map, row, codes) { PW_IA32_MAP_##map, row, codes },
#define PW_IA32_FORM(...)
#include "pw_ia32_isa.def"
#undef PW_IA32_GRID
#undef PW_IA32_FORM
};

static const PwIa32Form forms[] = {
#define PW_IA32_GRID(map, row, codes)
#define PW_IA32_FORM(name, mnemonic, map, opcode, sel, prefix, size, ops, reads, writes, undefined, attributes) \
    { #name, mnemonic, PW_IA32_MAP_##map, opcode, sel, PW_IA32_PFX_##prefix, PW_IA32_SIZE_##size, ops, \
      reads, writes, undefined, attributes },
#include "pw_ia32_isa.def"
#undef PW_IA32_GRID
#undef PW_IA32_FORM
};

#undef SEL
#undef OPS
#undef C
#undef P
#undef A
#undef Z
#undef S
#undef D
#undef O
#undef NOF
#undef LOCK
#undef REP
#undef REPCC
#undef BRANCH
#undef OPREG
#undef O1
#undef PRIV
#undef XACQ
#undef XREL
#undef CMPPRED
#undef UD
#undef MRO
#undef ADDR
#undef STACK
#undef STR
#undef IMPL
#undef MASK
#undef NO64
#undef X64
#undef D64
#undef VEX
#undef VEXONLY
#undef L0
#undef L1
#undef W0
#undef W1
#undef VLH
#undef NOV
#undef NOATT

enum {
    FORMS = sizeof(forms) / sizeof(forms[0]),
    GRID_ROWS = sizeof(grid) / sizeof(grid[0]),
    SLOTS = PW_IA32_MAPS * 256,
};
_Static_assert(GRID_ROWS == PW_IA32_MAPS * 16, "the .def has one grid row per 16 opcodes of each map");
_Static_assert(FORMS * 8 < 65536, "row indices fit in uint16_t");

char pw_ia32_shape(unsigned map, unsigned opcode)
{
    if (map >= PW_IA32_MAPS || opcode > 0xff) return '-';
    /* Grid rows are in map order, sixteen to a map (the host test checks the .def keeps them so). */
    const GridRow *row = &grid[map * 16 + (opcode >> 4)];
    if (row->map != map || row->row != (opcode & 0xf0)) return '-';
    return row->codes[(opcode & 0xf) * 2];
}

size_t pw_ia32_form_count(void)
{
    return FORMS;
}

const PwIa32Form *pw_ia32_form_at(size_t index)
{
    return index < FORMS ? &forms[index] : NULL;
}

/* The index from map/opcode to rows, built once on first use. start[slot]..start[slot + 1] are the
 * slot's entries in rows[]. State 0: not built; 1: being built; 2: ready. */
static uint16_t index_start[SLOTS + 1];
static uint16_t index_rows[FORMS * 8];
static atomic_int index_state;

static unsigned form_slot(const PwIa32Form *form, unsigned k)
{
    return form->map * 256u + form->opcode + k;
}

static unsigned form_span(const PwIa32Form *form)
{
    return (form->attributes & PW_IA32_ATTR_OPREG) ? 8 : 1;
}

static void index_build(void)
{
    uint16_t fill[SLOTS];
    for (unsigned s = 0; s <= SLOTS; s++) index_start[s] = 0;
    for (unsigned i = 0; i < FORMS; i++)
        for (unsigned k = 0; k < form_span(&forms[i]); k++) index_start[form_slot(&forms[i], k) + 1]++;
    for (unsigned s = 0; s < SLOTS; s++) {
        index_start[s + 1] += index_start[s];
        fill[s] = index_start[s];
    }
    for (unsigned i = 0; i < FORMS; i++)
        for (unsigned k = 0; k < form_span(&forms[i]); k++) index_rows[fill[form_slot(&forms[i], k)]++] = (uint16_t)i;
}

const uint16_t *pw_ia32_forms_for(unsigned map, unsigned opcode, size_t *count)
{
    if (!count) return NULL;
    *count = 0;
    if (map >= PW_IA32_MAPS || opcode > 0xff) return NULL;
    if (atomic_load_explicit(&index_state, memory_order_acquire) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&index_state, &expected, 1, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            index_build();
            atomic_store_explicit(&index_state, 2, memory_order_release);
        } else {
            while (atomic_load_explicit(&index_state, memory_order_acquire) != 2) {}
        }
    }
    const unsigned slot = map * 256u + opcode;
    *count = (size_t)(index_start[slot + 1] - index_start[slot]);
    return &index_rows[index_start[slot]];
}
