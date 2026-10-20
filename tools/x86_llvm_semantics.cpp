// SPDX-License-Identifier: GPL-3.0-or-later
//
// LLVM's description of an x86 instruction, for the decoder oracle (tools/ia32_decode_oracle.c):
// what libLLVM's MC layer records about the instruction it decodes from some bytes, which the C
// disassembler API does not expose. The oracle checks the table's memory, stack and control-flow
// attributes against it in long mode (docs/MMU4K.md, section 4.1).
//
//   x86_semantics_open(mode)                 32 or 64; 0 on success
//   x86_semantics(bytes, size, &flags)       LLVM's length (0: it decodes nothing) and X86_SEM_* flags
//
// X86_SEM_LOAD / STORE: MCInstrDesc::mayLoad / mayStore (conservative for some instructions: the
// oracle's differences file says which). FLOW: mayAffectControlFlow (branch, call, return, or a
// write of RIP). STACK: an implicit use or definition of a register overlapping RSP.
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <cstdint>
#include <memory>
#include <string>

namespace {
struct Llvm {
    std::unique_ptr<llvm::MCRegisterInfo> registers;
    std::unique_ptr<llvm::MCAsmInfo> asm_info;
    std::unique_ptr<llvm::MCSubtargetInfo> subtarget;
    std::unique_ptr<llvm::MCInstrInfo> instructions;
    std::unique_ptr<llvm::MCContext> context;
    std::unique_ptr<llvm::MCDisassembler> disassembler;
    unsigned rsp = 0;
};
Llvm *llvm_state;
}

extern "C" {
enum { X86_SEM_LOAD = 1, X86_SEM_STORE = 2, X86_SEM_FLOW = 4, X86_SEM_STACK = 8 };

int x86_semantics_open(int mode)
{
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86Disassembler();
    llvm::Triple triple(mode == 64 ? "x86_64-unknown-unknown" : "i386-unknown-unknown");
    std::string error;
    const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) return -1;
    auto *s = new Llvm;
    s->registers.reset(target->createMCRegInfo(triple));
    llvm::MCTargetOptions options;
    s->asm_info.reset(target->createMCAsmInfo(*s->registers, triple, options));
    s->subtarget.reset(target->createMCSubtargetInfo(triple, "", ""));
    s->instructions.reset(target->createMCInstrInfo());
    if (!s->registers || !s->asm_info || !s->subtarget || !s->instructions) return -1;
    s->context = std::make_unique<llvm::MCContext>(triple, *s->asm_info, *s->registers, *s->subtarget);
    s->disassembler.reset(target->createMCDisassembler(*s->subtarget, *s->context));
    if (!s->disassembler) return -1;
    for (unsigned r = 1; r < s->registers->getNumRegs(); r++)
        if (std::string(s->registers->getName(r)) == "RSP") s->rsp = r;
    if (!s->rsp) return -1;
    llvm_state = s;
    return 0;
}

int x86_semantics(const uint8_t *bytes, size_t size, unsigned *flags)
{
    *flags = 0;
    if (!llvm_state) return 0;
    llvm::MCInst inst;
    uint64_t length = 0;
    if (llvm_state->disassembler->getInstruction(inst, length, llvm::ArrayRef<uint8_t>(bytes, size), 0,
                                                 llvm::nulls()) != llvm::MCDisassembler::Success)
        return 0;
    const llvm::MCInstrDesc &desc = llvm_state->instructions->get(inst.getOpcode());
    const llvm::MCRegisterInfo &registers = *llvm_state->registers;
    if (desc.mayLoad()) *flags |= X86_SEM_LOAD;
    if (desc.mayStore()) *flags |= X86_SEM_STORE;
    if (desc.mayAffectControlFlow(inst, registers)) *flags |= X86_SEM_FLOW;
    for (llvm::MCPhysReg r : desc.implicit_uses())
        if (registers.regsOverlap(r, llvm_state->rsp)) *flags |= X86_SEM_STACK;
    for (llvm::MCPhysReg r : desc.implicit_defs())
        if (registers.regsOverlap(r, llvm_state->rsp)) *flags |= X86_SEM_STACK;
    return (int)length;
}
}
