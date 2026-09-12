option casemap:none

EXTERN g_clientViewThis:QWORD
EXTERN g_clientDispatchThis:QWORD
EXTERN g_clientOuterThis:QWORD
EXTERN g_clientViewInterceptContinue:QWORD
EXTERN g_clientViewCallCount:QWORD
EXTERN g_clientViewBuildOriginal:QWORD
EXTERN g_clientViewCallsiteContinue:QWORD
EXTERN g_clientSlotPrepare:QWORD
EXTERN g_clientSlotPrepareOriginalGlobal:QWORD
EXTERN g_clientSlotPrepareContinue:QWORD
EXTERN g_cameraStructAddress:QWORD
EXTERN g_cameraWriteBlock:BYTE
EXTERN g_stereoExperimentActive:BYTE
EXTERN g_stereoExperimentDone:BYTE
EXTERN g_stereoExperimentBaseX:DWORD
EXTERN g_stereoExperimentAppliedX:DWORD
EXTERN g_bonePinViewBuildArmed:BYTE
EXTERN ApplyWeaponBonePinAtViewBuild:PROC
EXTERN CaptureStereoFirstEye:PROC
EXTERN CaptureStereoSecondEye:PROC
EXTERN EndStereoSlotExperiment:PROC

PUBLIC clientViewBuildInterceptor
.code

; Pass-through probe for client.dll+0x35AEF0 (CViewRender virtual slot 32).
; It records the method's this pointer and replays the exact 20 overwritten
; bytes before continuing. It does not alter any game argument or state.
clientViewBuildInterceptor PROC
    mov qword ptr [g_clientViewThis], rcx
    lock inc qword ptr [g_clientViewCallCount]
    ; THE BONE WRITE'S SEAM, MOVED HERE FROM THE CAMERA-UPLOAD HOOK.
    ;
    ; This is the CalcView-equivalent point -- after the engine tick, before the
    ; render build. The old seam was the viewmodel camera upload, which is render
    ; side and downstream of whatever stages the bones the gun's draw reads, and
    ; no run there could separate "wrong array" from "right array, dead seam"
    ; because the write's survival was never measured. It is measured now, and
    ; from here.
    ;
    ; Disarmed this is one byte compare and a not-taken branch, so the idle path
    ; stays the exact pass-through the rest of this file is built on.
    cmp byte ptr [g_bonePinViewBuildArmed], 0
    je bone_pin_not_armed
    ; Seven pushes take the entry alignment (rsp = 8 mod 16) to 0 mod 16, which is
    ; what a Win64 callee expects at the call; 0A0h is a multiple of 16 and carries
    ; the 32-byte shadow space plus xmm0-xmm5. Everything is restored before the
    ; replayed prologue, because 'mov rax, rsp' below needs rsp to be exactly its
    ; entry value -- the game's frame pointer is derived from it.
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rax
    sub rsp, 0A0h
    movups xmmword ptr [rsp+20h], xmm0
    movups xmmword ptr [rsp+30h], xmm1
    movups xmmword ptr [rsp+40h], xmm2
    movups xmmword ptr [rsp+50h], xmm3
    movups xmmword ptr [rsp+60h], xmm4
    movups xmmword ptr [rsp+70h], xmm5
    call ApplyWeaponBonePinAtViewBuild
    movups xmm0, xmmword ptr [rsp+20h]
    movups xmm1, xmmword ptr [rsp+30h]
    movups xmm2, xmmword ptr [rsp+40h]
    movups xmm3, xmmword ptr [rsp+50h]
    movups xmm4, xmmword ptr [rsp+60h]
    movups xmm5, xmmword ptr [rsp+70h]
    add rsp, 0A0h
    pop rax
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
bone_pin_not_armed:
    mov rax, rsp
    push rbp
    push r15
    lea rbp, [rax-178h]
    sub rsp, 268h
    jmp qword ptr [g_clientViewInterceptContinue]
clientViewBuildInterceptor ENDP

; Replaces client+0x35AB70's high-level engine virtual render dispatch.
; On an armed F5 only, it executes the normal draw and captures the completed
; backbuffer, rebuilds the inner client view under a bounded offset, executes
; the same high-level draw again, and captures that second completed backbuffer.
clientViewCallsiteInterceptor PROC
    mov qword ptr [g_clientDispatchThis], rcx
    mov qword ptr [g_clientOuterThis], rdi
    push rbx
    sub rsp, 28h
    mov rbx, rcx
    mov rax, qword ptr [rbx]
    mov edx, 1
    call qword ptr [rax+0A0h]
finish_original_sequence:
    add rsp, 28h
    pop rbx
    jmp qword ptr [g_clientViewCallsiteContinue]
clientViewCallsiteInterceptor ENDP

; Runs after the ordinary slot-0 PrepareSlot returned, while CViewRender's
; outer job is still active.  F5 performs one extra native PrepareSlot(slot=1)
; under the known +50 source offset, then replays the exact displaced global
; continuation.  No outer-job recursion and no engine-object re-dispatch.
PUBLIC clientViewSlotPrepareInterceptor
clientViewSlotPrepareInterceptor PROC
    cmp byte ptr [g_stereoExperimentActive], 0
    je replay_original_continuation
    cmp byte ptr [g_stereoExperimentDone], 0
    jne replay_original_continuation
    mov rax, qword ptr [g_cameraStructAddress]
    test rax, rax
    je replay_original_continuation
    mov byte ptr [g_stereoExperimentDone], 1
    mov byte ptr [g_cameraWriteBlock], 1
    movss xmm0, dword ptr [g_stereoExperimentBaseX]
    addss xmm0, dword ptr [slotPrepareOffset]
    movss dword ptr [g_stereoExperimentAppliedX], xmm0
    movss dword ptr [rax], xmm0
    sub rsp, 28h
    mov rcx, rdi
    mov edx, 1
    call qword ptr [g_clientSlotPrepare]
    add rsp, 28h
replay_original_continuation:
    mov rax, qword ptr [g_clientSlotPrepareOriginalGlobal]
    mov rcx, qword ptr [rax]
    mov rax, qword ptr [rcx]
    call qword ptr [rax+3B8h]
    jmp qword ptr [g_clientSlotPrepareContinue]
clientViewSlotPrepareInterceptor ENDP

.const
stereoOffset REAL4 500.0
slotPrepareOffset REAL4 50.0

END
