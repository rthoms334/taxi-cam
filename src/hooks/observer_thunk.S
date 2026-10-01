/*
 * Manager-update hook. The observer runs before the original update; the
 * original is then called (not tail-jumped) so an after-observer can run once
 * it returns. The after-observer exists because the simulator's update syncs
 * the aircraft model to its latest physics step and places the main camera on
 * it: views placed before the update trail by one or two steps (0.9.65 trace).
 *
 * Calling instead of jumping changes the original's entry RSP, so the caller's
 * outgoing stack-argument area is copied into this frame's (Win64: the callee
 * owns those slots and the caller does not read them back). Twelve slots cover
 * any callee with up to sixteen arguments; extra slots are only read from the
 * caller's frame. The prologue is unchanged; the larger body allocation is made
 * after the frame pointer is established, so unwinding uses RBP throughout.
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

  /* Body frame: RSP = RBP - 384, 16-byte aligned (entry RSP is 8 mod 16).
     0..31 callee shadow space, 32..127 copied stack arguments (callee-owned),
     128.. saved state, never inside the callee-owned 0..127. */
  subq $160, %rsp
.globl taxi_hook_after_body_alloc
taxi_hook_after_body_alloc:
  movq %rax, 128(%rsp)
  movq %rcx, 136(%rsp)
  movq %rdx, 144(%rsp)
  movq %r8, 152(%rsp)
  movq %r9, 160(%rsp)
  movq %r10, 168(%rsp)
  movq %r11, 176(%rsp)
  movdqu %xmm0, 192(%rsp)
  movdqu %xmm1, 208(%rsp)
  movdqu %xmm2, 224(%rsp)
  movdqu %xmm3, 240(%rsp)
  movdqu %xmm4, 256(%rsp)
  movdqu %xmm5, 272(%rsp)
  stmxcsr 288(%rsp)
  call taxi_engine_hook_observe

  /* The caller's stack arguments start at entry RSP + 40 = RBP + 48. */
  movq 48(%rbp), %rax
  movq %rax, 32(%rsp)
  movq 56(%rbp), %rax
  movq %rax, 40(%rsp)
  movq 64(%rbp), %rax
  movq %rax, 48(%rsp)
  movq 72(%rbp), %rax
  movq %rax, 56(%rsp)
  movq 80(%rbp), %rax
  movq %rax, 64(%rsp)
  movq 88(%rbp), %rax
  movq %rax, 72(%rsp)
  movq 96(%rbp), %rax
  movq %rax, 80(%rsp)
  movq 104(%rbp), %rax
  movq %rax, 88(%rsp)
  movq 112(%rbp), %rax
  movq %rax, 96(%rsp)
  movq 120(%rbp), %rax
  movq %rax, 104(%rsp)
  movq 128(%rbp), %rax
  movq %rax, 112(%rsp)
  movq 136(%rbp), %rax
  movq %rax, 120(%rsp)

  ldmxcsr 288(%rsp)
  movdqu 192(%rsp), %xmm0
  movdqu 208(%rsp), %xmm1
  movdqu 224(%rsp), %xmm2
  movdqu 240(%rsp), %xmm3
  movdqu 256(%rsp), %xmm4
  movdqu 272(%rsp), %xmm5
  movq 128(%rsp), %rax
  movq 144(%rsp), %rdx
  movq 152(%rsp), %r8
  movq 160(%rsp), %r9
  movq 168(%rsp), %r10
  movq 176(%rsp), %r11
  movq 136(%rsp), %rcx

  /* The original receives the caller's arithmetic flags. */
.globl taxi_hook_before_restore_flags
taxi_hook_before_restore_flags:
  pushq -8(%rbp)
.globl taxi_hook_after_restore_push
taxi_hook_after_restore_push:
  popfq
.globl taxi_hook_after_restore_flags
taxi_hook_after_restore_flags:
  callq *taxi_engine_hook_original(%rip)
.globl taxi_hook_after_original
taxi_hook_after_original:

  /* Preserve the original's scalar/vector returns and FP status around the
     after-observer, which receives the original RCX saved above. */
  movq %rax, 296(%rsp)
  movq %rdx, 304(%rsp)
  movdqu %xmm0, 320(%rsp)
  movdqu %xmm1, 336(%rsp)
  stmxcsr 352(%rsp)
  movq 136(%rsp), %rcx
  call taxi_engine_hook_after
  ldmxcsr 352(%rsp)
  movdqu 320(%rsp), %xmm0
  movdqu 336(%rsp), %xmm1
  movq 296(%rsp), %rax
  movq 304(%rsp), %rdx

  /* Legal frame-pointer epilogue: LEA RSP,[RBP]; POP RBP; RET. */
.globl taxi_hook_before_epilogue
taxi_hook_before_epilogue:
  leaq (%rbp), %rsp
.globl taxi_hook_after_stack_restore
taxi_hook_after_stack_restore:
  popq %rbp
.globl taxi_hook_after_frame_restore
taxi_hook_after_frame_restore:
  retq
.seh_endproc
