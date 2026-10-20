/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The 4 KiB MMU's boundaries in assembly (pw_wine_mmu4k.h, docs/MMU4K.md sections 3 and 4.2): a run's start and exit
 * trap, the call into a slot, the guarded routines that touch program memory, and the call out that keeps the
 * program's vector state. Each routine states its register contract; tests/test_pw_wine_mmu4k.c is their oracle (the
 * same programs run natively and through them). No syscall instruction: the PRX audit fails a module with one. */

/* PwWineMmu4kRun's first fields (pw_wine_mmu4k.c checks them): the program's gpr[16] (rax rcx rdx rbx rsp rbp rsi
 * rdi r8-r15), rip, rflags, then the stack pointer of pw_wine_mmu4k_exec at the slot call. */
#define RUN_RFLAGS 136
#define RUN_HOST_RSP 144

    .text

/* A run's first RIP. In: rsp the run stack's top (16-byte aligned), rdi the run, rflags 0x202. Runs the run, then
 * traps: the glue's SIGILL handler sees RIP at pw_wine_mmu4k_exit_trap and gives the program its registers back. */
    .p2align 4
    .globl pw_wine_mmu4k_start
    .type pw_wine_mmu4k_start, @function
pw_wine_mmu4k_start:
    call pw_wine_mmu4k_run@PLT
    .globl pw_wine_mmu4k_exit_trap
pw_wine_mmu4k_exit_trap:
    ud2
    .size pw_wine_mmu4k_start, .-pw_wine_mmu4k_start

/* int pw_wine_mmu4k_exec(PwWineMmu4kRun *run [rdi], const void *slot [rsi]): the slot with the program's registers.
 * Loads RFLAGS and every general register but RSP from the run, calls the slot (one instruction, then ret) on this
 * stack, stores them back, leaves DF and AC clear and returns 0; callee-saved registers kept. A fault in the slot
 * comes back through pw_wine_mmu4k_recover (the glue sets RIP there and RSP to the run's host_rsp): 1, with the
 * run's registers as they were (x86 faults are precise). */
    .p2align 4
    .globl pw_wine_mmu4k_exec
    .type pw_wine_mmu4k_exec, @function
pw_wine_mmu4k_exec:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    push %rdi                       /* [host_rsp]: the run */
    mov %rsp, RUN_HOST_RSP(%rdi)
    push %rsi                       /* the slot */
    pushq RUN_RFLAGS(%rdi)
    mov 0(%rdi), %rax
    mov 8(%rdi), %rcx
    mov 16(%rdi), %rdx
    mov 24(%rdi), %rbx
    mov 40(%rdi), %rbp
    mov 48(%rdi), %rsi
    mov 64(%rdi), %r8
    mov 72(%rdi), %r9
    mov 80(%rdi), %r10
    mov 88(%rdi), %r11
    mov 96(%rdi), %r12
    mov 104(%rdi), %r13
    mov 112(%rdi), %r14
    mov 120(%rdi), %r15
    mov 56(%rdi), %rdi
    popfq
    call *(%rsp)
    pushfq
    push %rdi
    mov 24(%rsp), %rdi              /* [rsp]: rdi, rflags, the slot, the run */
    popq 56(%rdi)
    popq RUN_RFLAGS(%rdi)
    mov %rax, 0(%rdi)
    mov %rcx, 8(%rdi)
    mov %rdx, 16(%rdi)
    mov %rbx, 24(%rdi)
    mov %rbp, 40(%rdi)
    mov %rsi, 48(%rdi)
    mov %r8, 64(%rdi)
    mov %r9, 72(%rdi)
    mov %r10, 80(%rdi)
    mov %r11, 88(%rdi)
    mov %r12, 96(%rdi)
    mov %r13, 104(%rdi)
    mov %r14, 112(%rdi)
    mov %r15, 120(%rdi)
    add $16, %rsp                   /* the slot, the run */
    xor %eax, %eax
.Lexec_out:
    pushq $0x202                    /* DF and AC clear for C */
    popfq
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
    .globl pw_wine_mmu4k_recover
pw_wine_mmu4k_recover:              /* rsp = the run's host_rsp */
    add $8, %rsp
    mov $1, %eax
    jmp .Lexec_out
    .size pw_wine_mmu4k_exec, .-pw_wine_mmu4k_exec

/* The guarded routines: leaves that push nothing, so a fault inside one (the glue sets RIP to
 * pw_wine_mmu4k_guarded_fail and keeps RSP) returns 1 to their caller. DF is clear on entry (the ABI). */
    .p2align 4
    .globl pw_wine_mmu4k_guarded_begin
pw_wine_mmu4k_guarded_begin:

/* int pw_wine_mmu4k_read(void *dst [rdi], const void *src [rsi], size_t bytes [rdx]): the program's memory read
 * (a code fetch, a POP or RET load). int pw_wine_mmu4k_write(dst, src, bytes): a PUSH or CALL store. 0, or 1. */
    .globl pw_wine_mmu4k_read
    .type pw_wine_mmu4k_read, @function
    .globl pw_wine_mmu4k_write
    .type pw_wine_mmu4k_write, @function
pw_wine_mmu4k_read:
pw_wine_mmu4k_write:
    mov %rdx, %rcx
    rep movsb
    xor %eax, %eax
    ret
    .size pw_wine_mmu4k_read, .-pw_wine_mmu4k_read
    .size pw_wine_mmu4k_write, .-pw_wine_mmu4k_write

/* int pw_wine_mmu4k_copy(void *dst [rdi], uintptr_t src [rsi], size_t count [rdx], size_t element [rcx],
 * int stos [r8d]): rep movs (src an address) or rep stos (src the value) of count elements of 1, 2, 4 or 8 bytes.
 * 0, or 1 with the run's fault_rcx (elements left), fault_rsi and fault_rdi recorded by the glue. */
    .globl pw_wine_mmu4k_copy
    .type pw_wine_mmu4k_copy, @function
pw_wine_mmu4k_copy:
    mov %rsi, %rax
    mov %rcx, %r9
    mov %rdx, %rcx
    test %r8d, %r8d
    jnz .Lstos
    cmp $8, %r9
    je .Lmovsq
    cmp $4, %r9
    je .Lmovsl
    cmp $2, %r9
    je .Lmovsw
    rep movsb
    jmp .Lcopied
.Lmovsq:
    rep movsq
    jmp .Lcopied
.Lmovsl:
    rep movsl
    jmp .Lcopied
.Lmovsw:
    rep movsw
    jmp .Lcopied
.Lstos:
    cmp $8, %r9
    je .Lstosq
    cmp $4, %r9
    je .Lstosl
    cmp $2, %r9
    je .Lstosw
    rep stosb
    jmp .Lcopied
.Lstosq:
    rep stosq
    jmp .Lcopied
.Lstosl:
    rep stosl
    jmp .Lcopied
.Lstosw:
    rep stosw
.Lcopied:
    xor %eax, %eax
    ret
    .size pw_wine_mmu4k_copy, .-pw_wine_mmu4k_copy

    .globl pw_wine_mmu4k_guarded_fail
pw_wine_mmu4k_guarded_fail:
    mov $1, %eax
    ret
    .globl pw_wine_mmu4k_guarded_end
pw_wine_mmu4k_guarded_end:

/* void pw_wine_mmu4k_call(void (*fn)(void *) [rdi], void *arg [rsi], void *xsave_area [rdx], uint64_t mask [rcx]):
 * fn(arg) with the program's x87/SSE/AVX state kept (P2): xsave64 into the area (64-byte aligned, its header's
 * reserved bytes 0) with mask (XCR0 & 7), fninit and MXCSR 0x1f80 (Wine's Unix side), the call, xrstor64. */
    .p2align 4
    .globl pw_wine_mmu4k_call
    .type pw_wine_mmu4k_call, @function
pw_wine_mmu4k_call:
    push %rbx
    push %r12
    push %r13                       /* the stack is 16-byte aligned again */
    mov %rdx, %rbx
    mov %rcx, %r12
    mov %rdi, %r13
    mov %r12d, %eax
    mov %r12, %rdx
    shr $32, %rdx
    xsave64 (%rbx)
    fninit
    pushq $0x1f80
    ldmxcsr (%rsp)
    add $8, %rsp
    mov %rsi, %rdi
    call *%r13
    mov %r12d, %eax
    mov %r12, %rdx
    shr $32, %rdx
    xrstor64 (%rbx)
    pop %r13
    pop %r12
    pop %rbx
    ret
    .size pw_wine_mmu4k_call, .-pw_wine_mmu4k_call

    .section .note.GNU-stack,"",@progbits
