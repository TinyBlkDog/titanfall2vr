option casemap:none

; GATE 1 -- COLLAPSE THE ARMS' BONES INSTEAD OF TRYING TO HIDE THE ENTITY.
;
; FH3 established what draws the arms and it is not any switch on the entity.
; They are EF_NODRAW, they hold no render handle (+0x6D5 = 0) and ShouldDraw
; returns false, so they are in no render list at all. They are drawn as a
; bonemerged child, from BONE MATRICES, inside the parent's pass:
;
;   arms vtable slot 205 = client.dll+0x0EE950 = SetupBones
;     rcx = this   rdx = ?   r8 = matrix3x4[128]   r9 = matrix3x4[128]
;     [rsp+28h] = a fifth pointer argument
;
;   caller client.dll+0x0FEC65, inside renderable slot 13 (0x0FE760). The two
;   output buffers are 1800h apart, which is 128 * sizeof(matrix3x4_t), and
;   they live in the CALLER's stack frame.
;
; The wearer proved the draw consumes them: suppressing the call left last
; frame's matrices in that stack memory and the arms came adrift of the gun and
; drifted with the view. They did not disappear, because nothing asks this
; entity whether to draw -- it asks the matrices where to put the vertices.
;
; So: let SetupBones run, then flatten what it wrote. Wrap and overwrite the
; committed output, which is exactly the shape the placement pin already
; proved.
;
; WHY A RETURN-ADDRESS SWAP AND NOT AN INTERCEPTOR THAT CALLS A TRAMPOLINE.
; SetupBones takes FIVE arguments and the fifth arrives on the stack at
; [rsp+28h]. An interceptor that issued its own `call` would push a return
; address and shift every stack argument by 8, handing the original a garbage
; fifth argument. Swapping the return address instead leaves rsp and the whole
; argument block exactly as the caller built them: the original runs believing
; it was called directly, and returns into our post-process rather than to the
; caller.
;
; ALIGNMENT. At our entry rsp = 8 (mod 16) as always. We only jmp, so the
; original sees the same. Its `ret` pops our fake address and leaves rsp = 0
; (mod 16) at armsBonesPost, where 40h of frame keeps it 0 (mod 16) for the
; call.
;
; REENTRANCY. g_armsBonesInFlight is a plain guard, not a lock: a second entry
; while one is outstanding takes the passthrough and is simply not collapsed,
; because g_armsBonesSavedReturn holds exactly one address and a second swap
; would lose the first. Failing to collapse a frame is a cosmetic miss;
; returning to the wrong address is a crash. If the original ever unwinds
; without returning, the flag stays set and the collapse stops for good, which
; is the safe direction to fail in.

EXTERN g_armsBonesInstance:QWORD      ; the arms entity, or 0
EXTERN g_armsBonesArmed:BYTE
EXTERN g_armsBonesInFlight:BYTE
EXTERN g_armsBonesArray:QWORD         ; r8  captured for the post-process
EXTERN g_armsBonesArray2:QWORD        ; r9  captured for the post-process
EXTERN g_armsBonesModel:QWORD         ; rdx captured: studiohdr lives at [rdx+8]
EXTERN g_armsBonesSavedReturn:QWORD
EXTERN g_armsBonesOriginal:QWORD
EXTERN CollapseArmsBones:PROC

.code

PUBLIC armsBonesInterceptor
; THE ENGINE TELLS US WHICH OBJECT. WE DO NOT GUESS.
;
; This used to compare rcx against an instance resolved on the plugin frame by
; walking the handle table, and that cost four rounds of the body coming back.
; The bone cache at +0x1260 is only populated during the draw, so a plugin-frame
; check of it reads nothing and picks nothing -- and an instance chosen by
; vtable alone can be a pooled attachment that never animates.
;
; This vtable slot belongs to C_ViewmodelAttachmentModel, so every call through
; it is already the right CLASS, and rcx is the exact object the engine is
; drawing this instant. That is the answer, delivered, on the only clock where
; the bone cache is real. The gun is C_BaseViewModel with its own vtable and is
; excluded by construction, which is what the instance filter was ever for.
armsBonesInterceptor PROC
        cmp     byte ptr [g_armsBonesArmed], 0
        je      passthrough
        cmp     byte ptr [g_armsBonesInFlight], 0
        jne     passthrough

        mov     byte ptr [g_armsBonesInFlight], 1
        mov     qword ptr [g_armsBonesInstance], rcx
        mov     qword ptr [g_armsBonesArray], r8
        mov     qword ptr [g_armsBonesArray2], r9
        mov     qword ptr [g_armsBonesModel], rdx
        mov     r10, qword ptr [rsp]
        mov     qword ptr [g_armsBonesSavedReturn], r10
        lea     r10, armsBonesPost
        mov     qword ptr [rsp], r10

passthrough:
        jmp     qword ptr [g_armsBonesOriginal]
armsBonesInterceptor ENDP

; The original has returned. rax carries its result and xmm0 might; every other
; volatile register is already the original's business, so only those two are
; preserved across the post-process.
armsBonesPost PROC
        sub     rsp, 40h
        mov     qword ptr [rsp+20h], rax
        movdqu  xmmword ptr [rsp+30h], xmm0
        call    CollapseArmsBones
        movdqu  xmm0, xmmword ptr [rsp+30h]
        mov     rax, qword ptr [rsp+20h]
        add     rsp, 40h
        mov     byte ptr [g_armsBonesInFlight], 0
        jmp     qword ptr [g_armsBonesSavedReturn]
armsBonesPost ENDP

END
