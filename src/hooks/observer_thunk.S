/*
 */

.text
.p2align 4
.globl taxi_engine_hook_thunk
.seh_proc taxi_engine_hook_thunk
taxi_engine_hook_thunk:
  pushq %rbp
  .seh_pushreg %rbp
.globl taxi_hook_after_push_rbp
taxi_hook_after_push_rbp:
  pushfq
  .seh_stackalloc 8
.globl taxi_hook_after_push_flags
taxi_hook_after_push_flags:
  subq $216, %rsp
  .seh_stackalloc 216
.globl taxi_hook_after_alloc
taxi_hook_after_alloc:
  leaq 224(%rsp), %rbp
  .seh_setframe %rbp, 224
  .seh_endprologue
.globl taxi_hook_after_frame
taxi_hook_after_frame:

  /* RSP is 16-byte aligned. Bytes 0..31 are observer shadow space. */
  movq %rax, 32(%rsp)
  movq %rcx, 40(%rsp)
  movq %rdx, 48(%rsp)
  movq %r8, 56(%rsp)
  movq %r9, 64(%rsp)
  movq %r10, 72(%rsp)
  movq %r11, 80(%rsp)
  movdqu %xmm0, 96(%rsp)
  movdqu %xmm1, 112(%rsp)
  movdqu %xmm2, 128(%rsp)
  movdqu %xmm3, 144(%rsp)
  movdqu %xmm4, 160(%rsp)
  movdqu %xmm5, 176(%rsp)
  stmxcsr 192(%rsp)
  call taxi_engine_hook_observe
  ldmxcsr 192(%rsp)
  movdqu 96(%rsp), %xmm0
  movdqu 112(%rsp), %xmm1
  movdqu 128(%rsp), %xmm2
  movdqu 144(%rsp), %xmm3
  movdqu 160(%rsp), %xmm4
  movdqu 176(%rsp), %xmm5
  movq 32(%rsp), %rax
  movq 40(%rsp), %rcx
  movq 48(%rsp), %rdx
  movq 56(%rsp), %r8
  movq 64(%rsp), %r9
  movq 72(%rsp), %r10
  movq 80(%rsp), %r11

  /* A frame pointer makes this temporary push unwindable. Restore flags
     before the legal LEA/POP/indirect-JMP epilogue, which leaves them intact. */
.globl taxi_hook_before_restore_flags
taxi_hook_before_restore_flags:
  pushq -8(%rbp)
.globl taxi_hook_after_restore_push
taxi_hook_after_restore_push:
  popfq
.globl taxi_hook_after_restore_flags
taxi_hook_after_restore_flags:
  leaq (%rbp), %rsp
.globl taxi_hook_after_stack_restore
taxi_hook_after_stack_restore:
  popq %rbp
.globl taxi_hook_after_frame_restore
taxi_hook_after_frame_restore:
  jmpq *taxi_engine_hook_original(%rip)
.seh_endproc
