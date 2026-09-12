option casemap:none

; THE RUI LAYER INSET -- THE ENGINE'S OWN CENTRED-SHRINK MECHANISM.
;
; This replaces an earlier version that scaled the layer's coordinate-space size
; before the call. That worked -- it shrank the HUD -- but the space grows from
; its origin at TOP LEFT, so everything shrank *toward the top-left corner*
; rather than toward the middle. The wearer spotted it immediately as "a shrink
; plus a translate up". Scaling one axis alone does not fix that either: it
; stops the sideways travel only by distorting the aspect ratio.
;
; Reading FC500 to the end gives the real mechanism. Two parallel pipelines feed
; the draw, and the safe-area branch and the plain branch differ only in what
; they put into them:
;
;   size    [r10+2DD0h] -> [r10+2DE0h]
;   offset  [r10+3A90h] -> [r10+3AB0h]
;   mask    [r10+3A80h] -> [r10+3AA0h]
;
; The safe-area branch (layer types 4..7) writes size*(1-2f) and f. The plain
; branch, which is where the HUD's type 3 goes, copies a ZERO offset through.
; So [r10+3AB0h] is the engine's own per-edge inset -- the thing that insets the
; HUD from all four edges at once, which is a shrink about the CENTRE.
;
; We therefore stop inventing a transform and start filling in the one the
; engine already has, for a layer type it happens to skip. The size vector is
; laid out [w, w, h, h] (from `shufps xmm7, xmm6, 0`), so the offset lanes are
; per-edge too -- which is what makes an asymmetric inset a POSITION control as
; well as a size one.
;
; AFTER THE CALL, NOT BEFORE. FC500 zeroes [r10+3A90h] itself early on, so a
; value written before the call is overwritten by the time the copy happens.
; The effective slots are written once the original has produced its own values,
; and the arithmetic is done in C++ where it can be read and checked.
;
; STACK. Entered by a CALL, so rsp = 8 (mod 16). push rbx -> 0, sub rsp, 30h
; (48, a multiple of 16) -> still 0, correct at both calls. rbx is non-volatile
; and holds the OUTPUT struct across the original call, because rdx is volatile
; and FC500 is free to destroy it. rax carries FC500's return value to a caller
; that does `test al, al`, so it is parked in the frame while the C++ helper
; runs and restored before returning.

EXTERN g_ruiProbeArmed:BYTE
EXTERN g_ruiOriginal:QWORD
EXTERN g_ruiTypeMask:DWORD
EXTERN g_ruiWrites:QWORD
EXTERN g_ruiCensusArmed:BYTE
EXTERN RuiCensusSample:PROC
EXTERN g_ruiSuppressTarget:QWORD
EXTERN g_ruiSuppressed:QWORD


.code

PUBLIC ruiCallInterceptor
ruiCallInterceptor PROC
    cmp  byte ptr [g_ruiProbeArmed], 0
    je   rui_passthrough

    ; ---- RUNG A1: the read-only identity census -------------------------
    ;
    ; BEFORE anything is changed, so the census measures the game's own layer
    ; identities rather than ours. Gated on its own byte, so a run that only
    ; wants the shipped inset pays one compare and a not-taken branch.
    ;
    ; FC500 takes exactly two inputs -- rcx (the context) and rdx (the layer);
    ; it reads no xmm arguments and no r8/r9. So those two are the only
    ; registers that have to survive the helper, and they are already sitting
    ; in the argument registers the helper wants them in.
    ;
    ; STACK. Entered by a CALL, so rsp = 8 (mod 16). push rcx -> 0,
    ; push rdx -> 8, sub rsp, 28h (40) -> 0, which is what the ABI wants at
    ; the call instruction. 28h is 20h of shadow space plus the 8 of padding
    ; the two pushes cost.
    cmp  byte ptr [g_ruiCensusArmed], 0
    je   rui_no_census
    push rcx
    push rdx
    sub  rsp, 28h
    call RuiCensusSample
    add  rsp, 28h
    pop  rdx
    pop  rcx
rui_no_census:

    ; ---- RUNG A2: selective suppression by RESOLVED DRAW TARGET ----------
    ;
    ; The identity is `*(*(void**)rdx + 0x68)` -- the function FC500 would
    ; virtual-call at 0xFC6B1. A1 measured that each RUI widget class has its
    ; own such function, in ui(11).dll, so this is a semantic key and a stable
    ; one: a code address in a shipped module, not a heap pointer.
    ;
    ; SUPPRESSION IS THE ENGINE'S OWN PATH, NOT A NEW ONE. Returning 0 without
    ; calling FC500 is exactly what FC500 itself returns when [rbx+29h] is set,
    ; and the caller's `test al, al / je FC8F8` then skips this layer's draw
    ; record and moves to the next. So we are taking a branch the engine takes
    ; every time it declines to draw a layer -- which is also how Northstar's
    ; rui_drawEnable blanks the whole HUD, only per-identity.
    ;
    ; r10 and r11 are volatile AND recomputed from rdx by FC500's own first two
    ; instructions, so clobbering them here cannot disturb the passthrough.
    mov  r10, qword ptr [g_ruiSuppressTarget]
    test r10, r10
    je   rui_no_suppress
    mov  r11, [rdx]
    test r11, r11
    je   rui_no_suppress
    cmp  r10, [r11+68h]
    jne  rui_no_suppress
    lock inc qword ptr [g_ruiSuppressed]
    xor  eax, eax
    ret
rui_no_suppress:

    ; The layer type, read exactly where FC500 reads it: movzx ecx, byte [rdi+10h]
    ; with rdi = rcx.
    ;
    ; A MASK, NOT ONE TYPE. The wearer found the HUD split across layers: type 3
    ; carries the top-left cluster and the lower-left health group is somewhere
    ; else entirely. Only types 0..3 are candidates -- 4..7 already take the
    ; inset path on their own and need no help.
    movzx eax, byte ptr [rcx+10h]
    cmp  eax, 7
    ja   rui_passthrough
    bt   dword ptr [g_ruiTypeMask], eax
    jnc  rui_passthrough

    push rbx
    sub  rsp, 30h
    ; Hand FC500 a layer type it treats as safe-area, for THIS CALL ONLY.
    ; Its own branch is `movzx ecx, byte [rdi+10h]; shl eax, cl; test al, 0F0h`,
    ; so any type in 4..7 takes the inset path. Type 3 is one bit short of it.
    mov  rbx, rcx
    mov  al, byte ptr [rcx+10h]
    mov  byte ptr [rsp+28h], al
    mov  byte ptr [rcx+10h], 4
    lock inc qword ptr [g_ruiWrites]

    call qword ptr [g_ruiOriginal]

    ; Put the engine byte back before anything else can read it.
    mov  cl, byte ptr [rsp+28h]
    mov  byte ptr [rbx+10h], cl
    add  rsp, 30h
    pop  rbx
    ret

rui_passthrough:
    jmp  qword ptr [g_ruiOriginal]
ruiCallInterceptor ENDP


; ---------------------------------------------------------------------------
; RUNG A3 -- the substituted draw target.
;
; Reached because we wrote our own address into a DESCRIPTOR's +68h slot, which
; is a DATA write into engine.dll's .data, not a code patch. FC500 then
; virtual-calls us at 0xFC6B1 instead of ui(11).dll's own draw function, at the
; one instant where the placement it just computed is still only in memory and
; has not been read.
;
; ARGUMENTS, from FC500's own setup at FC664..FC6AE:
;     rcx = the RUI primitive interface (engine.dll+0x5F4320)
;     rdx = [rdi], the context's first qword
;     r8  = the LAYER  -- and *(void**)r8 is the descriptor, which is how the
;           helper works out whose original to hand back
;     r9  = layer+0x40
;
; xmm0..xmm5 hold the values FC500 just stored, but they are NOT arguments: all
; four parameters are integers, so anything further would go on the stack. They
; are volatile, the callee cannot read them as inputs, and FC500 itself uses
; only xmm6/xmm7 after the call -- both of which the helper preserves because
; they are non-volatile. So there is nothing to save.
;
; STACK. Entered by a CALL, so rsp = 8 (mod 16). Four pushes -> 8, sub rsp, 28h
; -> 0, which is what the ABI wants at the call. Restoring all four leaves the
; return address exactly where the original expects it, so the tail JMP hands
; it a frame indistinguishable from the one FC500 built.

EXTERN RuiA3Transform:PROC
EXTERN RuiA3AfterDraw:PROC
EXTERN RuiMarkerRemoveSlotSwap:PROC

PUBLIC ruiA3Interceptor
; CALL-THEN-READ, not a tail jump any more.
;
; The wobble is now measured to be in the GAME'S BACKBUFFER: the group is rigid
; across eight frames standing still and moves +43 px right, -102 px up across
; eight frames of a stick turn. So the placement is wrong before our XR layer
; ever sees it, and the thing to read is what the widget WRITES.
;
; The widget fills its parameter block at scratch+0x2DD0 during the call. A
; tail jump hands control away and never comes back, so the block could never
; be read. Calling it and returning here costs one frame pointer and lets the
; census see exactly what the widget decided this frame.
;
; STACK. Entered by a CALL, so rsp = 8 (mod 16). push rbx -> 0, sub rsp, 40h
; (64, a multiple of 16) -> still 0, which is what the ABI wants at every call
; below. rbx is non-volatile and carries the layer across the original call,
; because r8 is volatile and the callee may destroy it. The four argument
; registers are parked in the frame above the shadow space and restored
; immediately before the original runs.
;
; The original's return value is not used by FC500 -- it does
; `cmp byte [rbx+29h]` next and never reads rax -- but it is preserved anyway
; rather than relying on that staying true.

ruiA3Interceptor PROC
    push rbx
    sub  rsp, 40h
    mov  rbx, r8                ; the layer, safe across everything
    mov  [rsp+30h], rcx
    mov  [rsp+38h], rdx
    mov  [rsp+28h], r9
    ; rcx is the RUI PRIMITIVE INTERFACE -- the widget's own first act is
    ; `mov rsi, rcx` (ui(11)+0x809F3), and every `call [rsi+NN]` it makes goes
    ; through it. Passed as the second argument so the transform can swap slot
    ; +30h for the span of this draw; it was previously thrown away.
    mov  rdx, rcx               ; 2nd arg: the interface
    mov  rcx, r8                ; 1st arg: the layer
    call RuiA3Transform         ; transforms the placement, returns the original
    mov  rcx, [rsp+30h]
    mov  rdx, [rsp+38h]
    mov  r9,  [rsp+28h]
    mov  r8,  rbx
    ; A null would mean the helper could not identify the descriptor. Calling
    ; it would execute address zero. Returning here is NOT a clean decline:
    ; the layer's register file stays unfilled and FC7A0 still hands it to
    ; F9530, which faults (engine+0xF5C17, 2026-09-06, hud.widget_isolate).
    ; The only safe way to drop a layer is FC500's own path, the
    ; `xor eax,eax; ret` in ruiCallInterceptor above.
    test rax, rax
    je   a3_nothing
    call rax                    ; the widget's own draw
    mov  [rsp+20h], rax         ; park its return across the census call
    ; REMOVE THE SLOT SWAP FIRST, and unconditionally. RuiA3Transform installs
    ; it only for our marker widget; taking it out here means the interface is
    ; ours for exactly one widget's draw and nobody else's. Doing it before the
    ; census call keeps the window as small as it can be, and doing it on this
    ; single path is safe because the swap is only ever installed on this path.
    call RuiMarkerRemoveSlotSwap
    mov  rcx, rbx
    call RuiA3AfterDraw         ; read the block the widget just filled
    mov  rax, [rsp+20h]
a3_nothing:
    add  rsp, 40h
    pop  rbx
    ret
ruiA3Interceptor ENDP


; ---------------------------------------------------------------------------
; THE MARKER'S POSITION FIX, AT THE ONLY MOMENT IT SURVIVES.
;
; Measured 2026-09-05: writing block+0x50 BEFORE the widget draws does nothing,
; on 1118 proven draws. ui(11)+0x80EFC is why -- the widget fills the block
; itself from data bindings during its own call:
;
;   call [rsi+18h]                 ; block pointer
;   call [rsi+20h] / movaps [rbx+40h], xmm0
;   call [rsi+20h] / movaps [rbx+50h], xmm0   <- the position, overwrites us
;   call [rsi+20h] ... x2
;   call [rsi+30h]                 ; THE LAST CALL: consumes the block
;
; So slot +30h is the one instant after every fill and before consumption.
; This thunk is swapped into that slot for EXACTLY the span of our widget's
; draw -- installed just before `call rax` below and removed the moment it
; returns -- so no other widget can reach it.
;
; IT TAKES NO ARGUMENTS AND MUST DESTROY NONE. The callee's signature is not
; known, so every volatile register that could carry one is preserved across
; the C++ call: rcx, rdx, r8, r9 and xmm0-xmm3. The fix reads globals only.
;
; STACK: entered by CALL, so rsp = 8 (mod 16). sub rsp, 88h -> 8 + 88h = 90h,
; and 90h mod 16 = 0, which is the alignment the ABI wants at the call below.
; 20h of shadow space sits at the bottom and every save lives above it:
;
;   +00h..1Fh  shadow space for RuiMarkerFixBlock
;   +28h rcx   +30h rdx   +38h r8   +40h r9
;   +48h xmm0  +58h xmm1  +68h xmm2  +78h xmm3      (ends exactly at 88h)
;
; movdqu, not movaps: rsp is 16-aligned on entry to the call, so +48h is 8 mod
; 16 and an aligned store would fault.
EXTERN RuiMarkerFixBlock:PROC
EXTERN g_ruiPrimEndOriginal:QWORD

ruiPrimEndInterceptor PROC
    sub  rsp, 88h
    mov  [rsp+28h], rcx
    mov  [rsp+30h], rdx
    mov  [rsp+38h], r8
    mov  [rsp+40h], r9
    movdqu [rsp+48h], xmm0
    movdqu [rsp+58h], xmm1
    movdqu [rsp+68h], xmm2
    movdqu [rsp+78h], xmm3
    call RuiMarkerFixBlock
    movdqu xmm3, [rsp+78h]
    movdqu xmm2, [rsp+68h]
    movdqu xmm1, [rsp+58h]
    movdqu xmm0, [rsp+48h]
    mov  r9,  [rsp+40h]
    mov  r8,  [rsp+38h]
    mov  rdx, [rsp+30h]
    mov  rcx, [rsp+28h]
    add  rsp, 88h
    ; Tail-jump to the original: this thunk adds nothing to the call chain, so
    ; the callee returns straight to the widget exactly as it always did.
    jmp  QWORD PTR [g_ruiPrimEndOriginal]
ruiPrimEndInterceptor ENDP

END
