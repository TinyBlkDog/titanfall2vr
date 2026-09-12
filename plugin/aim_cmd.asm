option casemap:none

; PLAN-CURRENT P0 -- WRAP THE CLIENT COMMAND BUILD.
;
; Target: client.dll+0x254D10, CInput::CreateMove(this, sequence, frametime,
; active). One .pdata entry, 0x254D10..0x2554B1, no chaining -- a real function
; entry, checked before a byte was written.
;
; The displaced 14 bytes are seven whole instructions and not one of them is
; rip-relative, which is what makes copying them into a trampoline legal:
;
;   48 89 5C 24 18   mov [rsp+18h], rbx     5
;   55               push rbp               1
;   56               push rsi               1
;   57               push rdi               1
;   41 54            push r12               2
;   41 55            push r13               2
;   41 56            push r14               2
;                                         ----
;                                          14  -- and +14 is `push r15`, so the
;                                              patch ends exactly on a boundary.
;
; The byte check in aim_cmd.cpp is load-bearing: 14 bytes of `push` are common
; enough that a moved function would still look plausible, and writing a jump
; into the middle of the wrong instruction is how this goes wrong silently.
;
; WHY THE WORK IS AFTER THE ORIGINAL, NOT BEFORE.
;
; CreateMove builds the command from nothing: it calls CUserCmd::Reset first,
; so anything written before it runs is erased. The command is only meaningful
; once the original returns -- and that is also the only moment at which the
; engine's own two integrity values are already computed, so the post-process
; can recompute them over our value by calling the engine's own functions.
;
; WHAT IS CARRIED ACROSS THE CALL, AND WHY IN CALLEE-SAVED REGISTERS.
;
; rcx (this) and edx (sequence) are volatile and CreateMove is free to clobber
; both; it does. rbx and rsi are non-volatile and the original restores them.
; Those two values are the whole input to the post-process: the ring slot is
; sequence % 300 and the arrays hang off this.
;
; ALIGNMENT. At entry rsp = 8 (mod 16). Two pushes take it to 8; sub 28h takes
; it to 0, which is what a callee expects at a call, and 28h carries the 32
; bytes of shadow space with 8 to spare for the alignment. The second call is
; balanced the same way, with rax pushed across it because CreateMove returns a
; value and we are transparent.

EXTERN g_aimCmdHits:QWORD
EXTERN g_aimCmdTrampoline:QWORD
EXTERN g_aimCmdArmed:BYTE
EXTERN AimCmdPostProcess:PROC

.code

PUBLIC aimCreateMoveInterceptor
aimCreateMoveInterceptor PROC
    lock inc qword ptr [g_aimCmdHits]
    push rbx
    push rsi
    mov  rbx, rcx                             ; CInput* this
    mov  esi, edx                             ; sequence_number

    sub  rsp, 28h
    call qword ptr [g_aimCmdTrampoline]       ; displaced bytes, then entry+14
    add  rsp, 28h

    ; Cheapest possible gate, and it is checked AFTER the original so that a
    ; disarmed wrapper is an exact pass-through with one compare in it. This
    ; runs at the command tick rate, not per entity, so there is no instance
    ; filter to do here -- the seam hands us the object, which is the standing
    ; rule this project paid four regressions for.
    cmp  byte ptr [g_aimCmdArmed], 0
    je   aim_cmd_done

    push rax                                  ; CreateMove's return value
    sub  rsp, 20h
    mov  rcx, rbx
    mov  edx, esi
    call AimCmdPostProcess
    add  rsp, 20h
    pop  rax

aim_cmd_done:
    pop  rsi
    pop  rbx
    ret
aimCreateMoveInterceptor ENDP

END
