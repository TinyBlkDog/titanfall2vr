option casemap:none

; STUDIORENDER MODEL DRAW -- A TRANSPARENT WATCHER, BY CONSTRUCTION.
;
; studiorender.dll+0x15D10 is the virtual entry (class vtable +0x65A10) above
; the whole arms draw subtree:
;
;   +0x15D10 -> +0x12380 -> +0xDE10 -> +0xDCC0 -> +0xD810
;     -> materialsystem_dx11+0x72F30 -> +0x1E480 -> +0x1C410 -> IASetVertexBuffers
;
; WHY ASSEMBLY, AND NOT A C++ WRAPPER. A C++ wrapper was tried on 2026-09-06
; and cost the wearer a run: the model renderer was entered 33237 times and
; drew NOTHING -- gun, body and arms all gone, only the RUI ammo counter left.
; The wrapper forwarded FOUR arguments; the routine reads a SIXTH off the
; stack (`mov ebp,[rsp+0x118]`, = [entry+0x30]) and it steers the draw.
; This thunk NEVER REBUILDS THE CALL FRAME: no call, no pushed return address,
; no shadow space, and -- since this revision -- no touch of rsp at all. Every
; argument stays exactly where the caller put it, however many there are.
;
; PER-THREAD, KEYED ON THE THREAD ID (2026-09-06, fourth revision).
; Revision 1 recorded the descriptor in ONE global. With 110524 model draws a
; run against 3262 arms draws the reader caught another model's descriptor: it
; reported a LOD count of 0, which the routine's own entry check forbids, so
; it was provably stale.
; Revision 2 used TlsAlloc and the TEB's inline slot array at TEB+1480h. That
; was wrong: TlsAlloc returned index 87, the inline array only holds 64, the
; thunk correctly skipped the write and the whole run fell back to the racy
; global and was void. TlsAlloc's index is simply not bounded to that array.
; This revision uses the thread id itself (TEB+48h) as the key, so nothing can
; be out of range. Each thread writes its own {tid, descriptor} pair; the
; reader matches on tid, so a collision between two threads is DETECTED rather
; than silently believed -- and revision 3 was detected doing exactly that on
; every reading. Windows thread ids are always MULTIPLES OF FOUR, so its
; `tid AND 15` could only ever yield 0, 4, 8 or 12: four live slots out of
; sixteen, shared between every thread that draws models 133255 times a run.
; The low two bits carry no information and are shifted out before masking.
;
; Registers: only rax, r10 and r11 are written. All three are volatile and
; carry no argument at a call boundary (arguments are rcx, rdx, r8, r9), so
; the original sees an untouched argument set. FLAGS are clobbered, which is
; safe -- nothing is passed in flags.

extern g_studioDrawOrig:QWORD
extern g_studioDrawCalls:QWORD
extern g_studioDrawDesc:QWORD
extern g_studioDescTable:QWORD

.code

StudioDrawThunk proc
    mov     qword ptr [g_studioDrawDesc], r8
    lock inc qword ptr [g_studioDrawCalls]
    mov     r10, gs:[48h]                   ; TEB.ClientId.UniqueThread
    mov     rax, r10
    shr     rax, 2                          ; thread ids are multiples of 4
    and     rax, 3Fh                        ; 64 slots
    shl     rax, 4                          ; each slot is {tid, desc} = 16 bytes
    lea     r11, [g_studioDescTable]
    add     r11, rax
    mov     qword ptr [r11+8], r8           ; descriptor first ...
    mov     qword ptr [r11], r10            ; ... then publish the tid
    jmp     qword ptr [g_studioDrawOrig]
StudioDrawThunk endp

end
