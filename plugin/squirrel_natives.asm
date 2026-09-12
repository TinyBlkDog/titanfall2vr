option casemap:none

; THE SQUIRREL NATIVE REGISTRATION TABLE, CAPTURED AS IT IS BUILT.
;
; Every Squirrel native is a thin VM wrapper around an engine C++ function, and
; the bridge between the two is SQFuncRegistration -- name, signature and
; function pointer together. Recording those structs turns the whole script API
; into a list of directly callable client.dll addresses, which is what Phase C
; is built from.
;
; WHY NOT client.dll+0x108E0, THE FUNCTION NORTHSTAR NAMES.
;
; Northstar hooks 0x108E0 itself (squirrel.cpp:782), and its ON_DLL_LOAD
; callback runs BEFORE plugins are told the library loaded --
; libsys.cpp:49-50 calls CallLoadLibraryACallbacks and only then
; InformDllLoad. By the time this plugin sees client.dll those bytes are
; already MinHook's relative jump, and copying them into a trampoline without
; relocating the disp32 would send the "call the original" path into garbage.
; That is the exact shape of the probe defects that crashed this project three
; times, so this hooks somewhere else.
;
; 0x13790 is the inner registration function 0x108E0 tail-calls into
; (0x108E0+0x10 is `call 0x13790`), it is untouched by Northstar, and it is
; reached by five callers rather than one -- so it sees MORE registrations
; than the documented entry point does, not fewer. Offline verification:
; 958 E8 calls reach 0x108E0, its first instruction after the prologue is
; `mov r11,[rdx]`, and rdx is therefore the SQFuncRegistration pointer.
;
; DISPLACED BYTES.
;
; mov [rsp+20h],rbx ; push rbp ; push rdi ; push r14 ; sub rsp,80h
; 5 + 1 + 1 + 2 + 7 = 16 bytes, every one position independent -- no rip
; relative operand among them, which is what makes replaying them legal. The
; next instruction is a rip-relative call, and displacing it would have needed
; a relocation this does not do; 16 is where the boundary falls and 16 is what
; is taken.

EXTERN g_sqRegisterContinue:QWORD
EXTERN RecordSquirrelRegistration:PROC

.code

; Entered by jmp from the patched entry, so rsp is exactly what it was at the
; function's first instruction: [rsp] is the return address and the arguments
; are in rcx/rdx/r8/r9 with a fifth on the stack at [rsp+28h].
;
; The recorder runs BEFORE the displaced bytes are replayed, and restores rsp
; to the byte, so the replayed prologue sees the frame it expects and the
; stack argument stays where the callee will look for it.
;
; ALIGNMENT. At entry rsp = 8 (mod 16). Four pushes take it to 8; sub 28h takes
; it to 0, which is what the callee expects at a call, and 28h carries the 32
; bytes of shadow space RecordSquirrelRegistration may write into. rcx/rdx/r8/r9
; are saved across the call because they are the real function's arguments;
; rax, r10 and r11 are volatile at entry and are not, because the function
; itself overwrites r11 in its first instruction after the prologue.
PUBLIC sqRegisterInterceptor
sqRegisterInterceptor PROC
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 28h
    mov rcx, rdx                              ; SQFuncRegistration*
    call RecordSquirrelRegistration
    add rsp, 28h
    pop r9
    pop r8
    pop rdx
    pop rcx

    ; The displaced 16 bytes, verbatim.
    mov qword ptr [rsp+20h], rbx
    push rbp
    push rdi
    push r14
    sub rsp, 80h
    jmp qword ptr [g_sqRegisterContinue]
sqRegisterInterceptor ENDP

END
