/*
 * Local mock targets only; offsets are checked by validation.cpp.
 */

.text
.p2align 4
.globl validation_invoke
.seh_proc validation_invoke
validation_invoke:
  pushq %rbx
  .seh_pushreg %rbx
  pushq %r12
  .seh_pushreg %r12
  subq $72, %rsp
  .seh_stackalloc 72
  .seh_endprologue
  movq %rcx, %r12
  movq %rdx, %rbx
  movq 160(%r12), %rax
  movq %rax, 32(%rsp)
  movq 168(%r12), %rax
  movq %rax, 40(%rsp)
  movq 176(%r12), %rax
  movq %rax, 48(%rsp)
  movq 184(%r12), %rax
  movq %rax, 56(%rsp)
  leaq -8(%rsp), %rax
  movq %rax, 192(%r12)
  ldmxcsr 200(%r12)
  movdqu 64(%r12), %xmm0
  movdqu 80(%r12), %xmm1
  movdqu 96(%r12), %xmm2
  movdqu 112(%r12), %xmm3
  movdqu 128(%r12), %xmm4
  movdqu 144(%r12), %xmm5
  movq 8(%r12), %rcx
  movq 16(%r12), %rdx
  movq 24(%r12), %r8
  movq 32(%r12), %r9
  movq 40(%r12), %r10
  movq 48(%r12), %r11
  movq (%r12), %rax
  pushq 56(%r12)
  popfq
  callq *(%rbx)
  movq %rax, 424(%r12)
  movdqu %xmm0, 432(%r12)
  addq $72, %rsp
  popq %r12
  popq %rbx
  retq
.seh_endproc

.p2align 4
.globl validation_target
validation_target:
  movq %rax, 208(%rcx)
  movq %rcx, 216(%rcx)
  movq %rdx, 224(%rcx)
  movq %r8, 232(%rcx)
  movq %r9, 240(%rcx)
  movq %r10, 248(%rcx)
  movq %r11, 256(%rcx)
  pushfq
  popq 264(%rcx)
  movdqu %xmm0, 272(%rcx)
  movdqu %xmm1, 288(%rcx)
  movdqu %xmm2, 304(%rcx)
  movdqu %xmm3, 320(%rcx)
  movdqu %xmm4, 336(%rcx)
  movdqu %xmm5, 352(%rcx)
  movq 40(%rsp), %rax
  movq %rax, 368(%rcx)
  movq 48(%rsp), %rax
  movq %rax, 376(%rcx)
  movq 56(%rsp), %rax
  movq %rax, 384(%rcx)
  movq 64(%rsp), %rax
  movq %rax, 392(%rcx)
  movq %rsp, 400(%rcx)
  stmxcsr 408(%rcx)
  incq 416(%rcx)
  movabsq $0x1020304050607080, %rax
  movdqu validation_return_vector(%rip), %xmm0
  retq

.p2align 4
.globl validation_observer
.seh_proc validation_observer
validation_observer:
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  call validation_observer_body
  /* Deliberately disturb every covered volatile register and FP status. */
  movl $0x1fbf, 32(%rsp)
  ldmxcsr 32(%rsp)
  pcmpeqd %xmm0, %xmm0
  pcmpeqd %xmm1, %xmm1
  pcmpeqd %xmm2, %xmm2
  pcmpeqd %xmm3, %xmm3
  pcmpeqd %xmm4, %xmm4
  pcmpeqd %xmm5, %xmm5
  movabsq $0xfedcba9876543210, %rax
  movq %rax, %rcx
  movq %rax, %rdx
  movq %rax, %r8
  movq %rax, %r9
  movq %rax, %r10
  movq %rax, %r11
  addq $40, %rsp
  retq
.seh_endproc

.section .rdata,"dr"
.p2align 4
validation_return_vector:
  .quad 0x8899aabbccddeeff, 0x0011223344556677
