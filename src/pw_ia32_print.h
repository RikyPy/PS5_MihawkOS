/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_IA32_PRINT_H
#define PW_IA32_PRINT_H
#include "pw_ia32_decode.h"

/* One decoded instruction as Intel-syntax text, spelled as LLVM's disassembler spells it
 * (tools/ia32-decode-oracle.sh compares the two, in both modes): "mnemonic\toperands", prefixes
 * first ("lock\t", "rep\t"), decimal numbers, branch targets relative to the next instruction.
 *
 * PW_OK; PW_ERR_PRECONDITION for a null argument or an instruction without a form;
 * PW_ERR_UNSUPPORTED for a VEX form (its rows leave out the VEX.vvvv register, so only its memory
 * operand prints, below); PW_ERR_LIMIT when the text does not fit in `capacity` (out then holds a
 * truncated, terminated string). */
int pw_ia32_print(const PwIa32Inst *inst, char *out, size_t capacity);

/* The ModRM memory operand alone, as LLVM spells it: "dword ptr fs:[rax + 4*r12 - 16]", with the
 * size word for `bits` (8-256; another value prints none). PW_OK, PW_ERR_PRECONDITION (a null
 * argument, or no memory operand), PW_ERR_LIMIT. */
int pw_ia32_print_memory(const PwIa32Inst *inst, unsigned bits, char *out, size_t capacity);

#endif
