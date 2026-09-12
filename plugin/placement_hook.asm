option casemap:none

; PLAN-CURRENT F2 -- WRAP THE PLACEMENT COMMIT AND OVERWRITE ITS OUTPUT.
;
; Target: client.dll+0x3DB4A0, which F1 identified and which the unwind chain
; proved is the REAL function entry.
;
; F1's watchpoint recorded the store at client.dll+0x3DB7B7. That address is
; NOT a hook target: its .pdata entry carries UNW_FLAG_CHAININFO and chains
; 0x3DB75B -> 0x3DB566 -> 0x3DB4A0, so it is a continuation FRAGMENT reached by
; a jump, with no return address on the stack and rsi already live. Wrapping it
; as a function would have called a trampoline and then executed `ret` against
; whatever happened to be at [rsp]. The unwind chain was resolved before a byte
; was written, which is the standing rule for this project and is why this
; comment exists rather than a crash log.
;
; The real entry is an ordinary function:
;
;   push rbp / push rsi / lea rbp,[rsp-4Fh] / sub rsp,0F8h   = 15 bytes
;   mov eax,[rcx+50h] / mov rsi,rcx / bt eax,0Bh / jb recompute
;
; so rcx is THIS, and the 15 displaced bytes are whole instructions with no
; rip-relative operand -- which is what makes copying them into a trampoline
; legal at all. The byte check in placement_pin.cpp is load-bearing, not
; decorative.
;
; THE DIRTY BIT, AND WHY THE POST-PROCESS IS CONDITIONAL.
;
; This is CalcAbsolutePosition: bit 0x800 of [this+50h] means "the placement is
; stale". Set, it recomputes and commits and clears the bit; clear, it returns
; immediately having touched nothing.
;
; A post-process that ran on EVERY call would, on those early-return calls,
; read back the value WE wrote last time and add the offset to it again. That
; is the compounding this project has already been bitten by once, and it looks
; exactly like a working drive for the first few frames. So the bit is sampled
; BEFORE the original runs, and the post-process fires only when the original
; was actually going to recompute.
;
; ALIGNMENT. At entry rsp = 8 (mod 16). Two pushes take it to 8; sub 28h takes
; it to 0, which is what the callee expects at a call, and 28h carries the 32
; bytes of shadow space. The post-process call is aligned the same way, and rax
; is preserved across it.

EXTERN g_placementHits:QWORD
EXTERN g_placementTrampoline:QWORD
EXTERN g_placementInstance:QWORD
EXTERN g_placementDriveArmed:BYTE
EXTERN ApplyPlacementPin:PROC

.code

PUBLIC placementCommitInterceptor
placementCommitInterceptor PROC
    lock inc qword ptr [g_placementHits]
    push rbx
    push rdi
    mov  rbx, rcx                             ; this, across the call: rcx is
                                              ; volatile and the original may
                                              ; clobber it. rbx is not, and the
                                              ; original restores what it uses.
    xor  edi, edi
    mov  eax, dword ptr [rcx+50h]
    bt   eax, 0Bh                             ; 0x800: placement is stale
    jnc  placement_no_recompute
    mov  edi, 1                               ; the original WILL commit
placement_no_recompute:
    sub  rsp, 28h
    call qword ptr [g_placementTrampoline]    ; displaced bytes, then entry+15
    add  rsp, 28h

    cmp  byte ptr [g_placementDriveArmed], 0
    je   placement_done
    test edi, edi
    je   placement_done                       ; early-out call: touch nothing,
                                              ; or the offset compounds
    ; THE INSTANCE FILTER, IN ASM AND BEFORE THE CALL.
    ;
    ; This is a generic entity-level commit -- every entity whose placement is
    ; recomputed comes through here, which is thousands of calls a frame. F1's
    ; watchpoint only ever saw our viewmodel because it was scoped to an
    ; ADDRESS; a code wrapper has no such luck. Two instructions here beat a
    ; C++ call with shadow space on every entity in the map.
    mov  rax, qword ptr [g_placementInstance]
    cmp  rbx, rax
    jne  placement_done

    push rax
    sub  rsp, 20h
    mov  rcx, rbx
    call ApplyPlacementPin
    add  rsp, 20h
    pop  rax
placement_done:
    pop  rdi
    pop  rbx
    ret
placementCommitInterceptor ENDP

END
