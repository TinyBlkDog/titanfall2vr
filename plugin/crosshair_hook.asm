option casemap:none

; THE RETICLE'S PER-ELEMENT FLOAT, SCALED ON ITS WAY IN.
;
; Target: client.dll+0x158EB0. It is called from exactly two places, both inside
; the crosshair draw (client.dll+0x15EF90), and from nowhere else in the image --
; so this hook cannot touch anything but the reticle. That scoping is why it was
; chosen over the draw function itself.
;
; The crosshair draw reads a per-element float out of a table at
; crosshairObj + index*4 + 0x2E50 and passes it as the FIFTH argument; 0x158EB0
; forwards it unchanged into 0x3D3710 for every sub-element it emits. A
; per-element float threaded through the draw untouched is the shape of a scale,
; which is the hypothesis this exists to test.
;
; WHY THIS IS A TAIL JUMP AND NOT A WRAPPER.
;
; The fifth argument lives in the caller's stack, at [rsp+28h] on entry -- above
; the 32 bytes of shadow space, exactly where the caller put it (it does
; `movss [rsp+20h], xmm6` immediately before the call). Modifying it in place and
; jumping straight to the original means the original sees its own frame, byte
; for byte, with one float changed. No pushes, no sub rsp, no shadow space of our
; own, and nothing to unwind: this frame never exists as far as the callee is
; concerned.
;
; That also means there is no post-process and no return path to get wrong, which
; on a function called several times a frame is worth more than the flexibility.
;
; The displaced bytes are 15, not 14: the prologue is three 5-byte stores
;
;   48 89 5C 24 08   mov [rsp+8], rbx
;   48 89 6C 24 10   mov [rsp+10h], rbp
;   48 89 74 24 18   mov [rsp+18h], rsi
;
; which is where the next instruction boundary falls. 14 would land inside the
; third store. The trampoline therefore carries 15 bytes and the patch is a
; 14-byte absolute jump plus one nop.

EXTERN g_crosshairScaleArmed:BYTE
EXTERN g_crosshairScale:DWORD
EXTERN g_crosshairScaleHits:QWORD
EXTERN g_crosshairTrampoline:QWORD

.code

PUBLIC crosshairScaleInterceptor
crosshairScaleInterceptor PROC
    cmp  byte ptr [g_crosshairScaleArmed], 0
    je   crosshair_passthrough
    lock inc qword ptr [g_crosshairScaleHits]
    ; xmm0 is volatile and this is the very first thing in the call, before the
    ; callee has touched anything -- so it is free to use and nothing needs
    ; saving. The float is read, scaled and written back to the caller's own
    ; slot, which the callee then reads exactly as it would have.
    movss xmm0, dword ptr [rsp+28h]
    mulss xmm0, dword ptr [g_crosshairScale]
    movss dword ptr [rsp+28h], xmm0
crosshair_passthrough:
    jmp  qword ptr [g_crosshairTrampoline]
crosshairScaleInterceptor ENDP

END
