option casemap:none

EXTERN g_cameraStructAddress:QWORD
EXTERN g_cameraAnglesAddress:QWORD
EXTERN g_cameraHookCallCount:QWORD
EXTERN g_cameraBasePitchBits:DWORD
EXTERN g_cameraBaseYawBits:DWORD
EXTERN g_cameraBaseRollBits:DWORD
EXTERN g_cameraDesiredPitchBits:DWORD
EXTERN g_cameraDesiredYawBits:DWORD
; The yaw DELTA, added to this frame's base at write time. See the yaw block.
EXTERN g_cameraYawDeltaBits:DWORD
EXTERN g_cameraDesiredRollBits:DWORD
EXTERN g_cameraYawWriteActive:BYTE
EXTERN g_cameraSourceAnglesAddress:QWORD
EXTERN g_cameraSourceBaseYawBits:DWORD
EXTERN g_cameraSourceDesiredYawBits:DWORD
EXTERN g_cameraSourceYawWriteActive:BYTE
EXTERN g_cameraStructInterceptionContinue:QWORD
EXTERN g_cameraWriteBlock:BYTE
; The angles as actually applied at this site, latched in the same block that
; latches the base. The viewmodel correction needs the PAIR the engine used for
; this frame; reading values published on a plugin frame instead let the two
; drift apart, which showed up as the weapon moving with the head for a moment
; and then snapping back.
EXTERN g_cameraAppliedPitchBits:DWORD
EXTERN g_cameraAppliedYawBits:DWORD
EXTERN g_cameraAppliedRollBits:DWORD
; Seqlock over the base/applied pair. The pair is SIX separate dwords latched on
; the game thread and read on the render thread, and the correction's whole
; result is the rotation BETWEEN them -- so a reader that catches the base from
; one view build and the applied from the next does not get a slightly stale
; rotation, it gets a meaningless one, and the weapon jumps for that frame.
; Odd while the block is mid-update, even when it is consistent.
EXTERN g_cameraAngleGeneration:DWORD
; Positional head tracking. Same pattern as the angles: latch the engine's own
; value, then optionally overwrite with one composed against it.
EXTERN g_cameraBasePosXBits:DWORD
EXTERN g_cameraBasePosYBits:DWORD
EXTERN g_cameraBasePosZBits:DWORD
EXTERN g_cameraDesiredPosXBits:DWORD
EXTERN g_cameraDesiredPosYBits:DWORD
EXTERN g_cameraDesiredPosZBits:DWORD
; The head's offset alone, in game space. Added to the base at write time so the
; two can never come from different frames. See the note at the write site.
EXTERN g_cameraOffsetXBits:DWORD
EXTERN g_cameraOffsetYBits:DWORD
EXTERN g_cameraOffsetZBits:DWORD
EXTERN g_cameraPositionWriteActive:BYTE

EXTERN g_weaponSettingsTablePtr:QWORD
EXTERN g_weaponSettingsIndex:QWORD
EXTERN g_weaponSettingsContinue:QWORD
; Total invocations. The site was assumed to run only during weapon parsing at
; level load; if it is actually hot, this detour is in a per-frame path.
EXTERN g_weaponSettingsHits:QWORD

PUBLIC cameraStructInterceptor
PUBLIC weaponSettingsInterceptor
.const
kYawHalfTurn  REAL4 180.0
kYawNegHalf   REAL4 -180.0
kYawFullTurn  REAL4 360.0

.code

; Passive capture of the weapon-settings object during the weapon script parse.
;
; The parse writes each KeyValue result into a struct through rdi. The sway
; block was recovered by locating the key strings in client.dll and following
; the RIP-relative lea that names each one: every block is
;
;     lea rdx, <key> ; set default ; mov rcx, rbx ; movss [rdi+off], xmm0 ; call
;
; where the store writes the PREVIOUS key's result, so the offsets lag the names
; by one block. That gives sway_translate_gain at +0x84 and sway_rotate_gain at
; +0x88.
;
; This hook only RECORDS rdi. It mutates nothing: the suppression itself is done
; later from C++ (always on since 2026-09-04; restored only at unload) -- a
; parse-time write could not
; be gated at all, because parsing happens at level load.
;
; The 15 displaced bytes are all position-independent (movaps/mov/movss), which
; is why this site was chosen over the neighbouring ones: no call and no
; RIP-relative operand has to be relocated into the trampoline.
weaponSettingsInterceptor PROC
    push rax
    push rcx
    lock inc qword ptr [g_weaponSettingsHits]
    mov rax, qword ptr [g_weaponSettingsTablePtr]
    test rax, rax
    je skipCapture
    mov rcx, qword ptr [g_weaponSettingsIndex]
    mov qword ptr [rax + rcx * 8], rdi
    inc rcx
    and rcx, 127
    mov qword ptr [g_weaponSettingsIndex], rcx
skipCapture:
    pop rcx
    pop rax
    ; Replayed verbatim from client.dll.
    movaps xmm2, xmm9
    mov rcx, rbx
    movss dword ptr [rdi+84h], xmm0
    jmp qword ptr [g_weaponSettingsContinue]
weaponSettingsInterceptor ENDP

; Exact passthrough for the 42 bytes beginning at client.dll+0x142C56 in
; the reference build. rsi and rdi are observed, then every displaced game
; instruction is replayed before jumping to the continuation.
;
; rdi is the destination the displaced code writes the view angles to
; (mov [rdi]/[rdi+4]/[rdi+8] below) and is never loaded inside the displaced
; region, so it is already live at entry. IGCS identifies this same pointer
; as g_cameraStructAddress+0x0C; capturing it lets us verify that rather
; than assume it. Both stores are observation only, and are placed before
; the write-block branch so the pointer is captured on both paths.
cameraStructInterceptor PROC
    mov qword ptr [g_cameraStructAddress], rsi
    mov qword ptr [g_cameraAnglesAddress], rdi
    ; Distinguishes "this hook site does not execute during gameplay" from
    ; "it executes and the game wrote identical values". The first trace could
    ; not tell those apart, and they lead to opposite conclusions.
    lock inc qword ptr [g_cameraHookCallCount]
    cmp byte ptr [g_cameraWriteBlock], 0
    jne skipPrimaryWrites
    mov dword ptr [rsi], ecx
    mov ecx, dword ptr [rax+4]
    mov dword ptr [rsi+4], ecx
    mov eax, dword ptr [rax+8]
    mov rcx, rbx
    mov dword ptr [rsi+8], eax
    mov rax, qword ptr [rbx]
    call qword ptr [rax+5B0h]
    ; --- Task 01 stage 3: the UPSTREAM angle source -----------------------
    ; rax now points at the angles the engine is about to copy into [rdi].
    ; Writing [rdi] rotated the view but left the HUD facing the original
    ; direction, so screen-space projection reads something earlier than the
    ; destination. This is that earlier thing.
    ;
    ; ecx is free here: the original code overwrites it from [rax] on the very
    ; next instruction.
    mov qword ptr [g_cameraSourceAnglesAddress], rax
    mov ecx, dword ptr [rax+4]
    mov dword ptr [g_cameraSourceBaseYawBits], ecx
    cmp byte ptr [g_cameraSourceYawWriteActive], 0
    je noSourceYawWrite
    mov ecx, dword ptr [g_cameraSourceDesiredYawBits]
    mov dword ptr [rax+4], ecx
noSourceYawWrite:
    ; ---------------------------------------------------------------------
    mov ecx, dword ptr [rax]
    mov dword ptr [rdi], ecx
    mov ecx, dword ptr [rax+4]
    mov dword ptr [rdi+4], ecx
    mov eax, dword ptr [rax+8]
    mov dword ptr [rdi+8], eax
    ; --- Task 01 stage 2: bounded yaw write test -------------------------
    ; Capture the engine's own yaw BEFORE writing. This is the frame's clean
    ; base, and publishing it is what makes the CPVR feedback loop directly
    ; observable: if writing this field feeds back into the engine's own
    ; source, the captured base grows frame over frame instead of tracking
    ; the mouse. The offset is therefore applied against a base we did not
    ; write, rather than against our own previous output.
    ;
    ; The value is moved as raw bits, so no float math and no xmm register is
    ; touched. Mid-function xmm state is live here and must not be clobbered.
    ; eax is safe: the displaced code has already consumed it and the
    ; continuation at +0x142C80 immediately makes a call, across which eax is
    ; volatile.
    ; Open the seqlock: everything from the base latch to the applied latch is
    ; one transaction, because the game's own write sits between them.
    inc dword ptr [g_cameraAngleGeneration]
    mov eax, dword ptr [rdi]
    mov dword ptr [g_cameraBasePitchBits], eax
    mov eax, dword ptr [rdi+4]
    mov dword ptr [g_cameraBaseYawBits], eax
    mov eax, dword ptr [rdi+8]
    mov dword ptr [g_cameraBaseRollBits], eax
    cmp byte ptr [g_cameraYawWriteActive], 0
    je noYawWrite
    ; Pitch and roll are HEAD-sourced absolutes. They do not derive from the
    ; base, so they carry no cross-clock error and stay raw bit moves.
    mov eax, dword ptr [g_cameraDesiredPitchBits]
    mov dword ptr [rdi], eax
    mov eax, dword ptr [g_cameraDesiredRollBits]
    mov dword ptr [rdi+8], eax
    ; YAW: BASE + DELTA, SUMMED HERE, ON THE RENDER FRAME.
    ;
    ; This used to store g_cameraDesiredYawBits, an ABSOLUTE yaw the plugin
    ; frame had already formed as (base it last saw) + delta. The base is
    ; latched a few instructions above, on the RENDER frame, so the two came
    ; from different clocks: every frame the engine's fresh yaw was clobbered
    ; with a stale one, and the error is the angular velocity times the gap.
    ; It is therefore ZERO when standing still and grows with how fast you
    ; turn -- which is exactly the reported symptom, an amplitude that rises
    ; with turn speed, invisible for a year behind a joystick's capped turn
    ; rate and obvious the moment a mouse was used.
    ;
    ; This is the SAME fix already made for the camera POSITION directly
    ; below, whose comment claims it was also made for the angles. It was not:
    ; the position path sums here, the angle path did not, and that gap is
    ; this defect.
    ;
    ; The block above is raw bit moves because mid-function xmm state is live.
    ; This needs real float adds, so exactly one register is borrowed and put
    ; back, movups so rsp alignment here does not matter -- the same borrow the
    ; position path uses.
    sub rsp, 16
    movups xmmword ptr [rsp], xmm0
    movss xmm0, dword ptr [rdi+4]
    addss xmm0, dword ptr [g_cameraYawDeltaBits]
    ; Wrap to +-180. The old path wrapped on the plugin frame; summing here
    ; means the sum can leave the range, and an unnormalised yaw at the seam
    ; would read as a new glitch rather than as this fix working.
    comiss xmm0, dword ptr [kYawHalfTurn]
    jbe yawNoWrapHigh
    subss xmm0, dword ptr [kYawFullTurn]
yawNoWrapHigh:
    comiss xmm0, dword ptr [kYawNegHalf]
    jae yawNoWrapLow
    addss xmm0, dword ptr [kYawFullTurn]
yawNoWrapLow:
    movss dword ptr [rdi+4], xmm0
    movups xmm0, xmmword ptr [rsp]
    add rsp, 16
noYawWrite:
    ; Latch what was actually left in the destination, whether we wrote it or
    ; not. Paired with g_cameraBase*Bits above, this is the exact before/after
    ; the engine used for this frame's view, which is what the viewmodel
    ; correction has to cancel. Reading a copy published on a plugin frame let
    ; the two drift by a frame during head motion.
    mov eax, dword ptr [rdi]
    mov dword ptr [g_cameraAppliedPitchBits], eax
    mov eax, dword ptr [rdi+4]
    mov dword ptr [g_cameraAppliedYawBits], eax
    mov eax, dword ptr [rdi+8]
    mov dword ptr [g_cameraAppliedRollBits], eax
    ; Close the seqlock: the pair is now consistent.
    inc dword ptr [g_cameraAngleGeneration]
    ; --- Positional head tracking ----------------------------------------
    ; rsi is the camera position pointer, non-volatile across the accessor
    ; call above and never modified in this block, so it is still live. Raw
    ; bit moves only: no float math and no xmm register is touched, because
    ; mid-function xmm state is live here.
    mov eax, dword ptr [rsi]
    mov dword ptr [g_cameraBasePosXBits], eax
    mov eax, dword ptr [rsi+4]
    mov dword ptr [g_cameraBasePosYBits], eax
    mov eax, dword ptr [rsi+8]
    mov dword ptr [g_cameraBasePosZBits], eax
    cmp byte ptr [g_cameraPositionWriteActive], 0
    je noPositionWrite
    ; BASE + OFFSET, SUMMED HERE, NOT ON THE PLUGIN FRAME.
    ;
    ; This used to write g_cameraDesiredPos*, a sum the plugin frame had already
    ; formed from whatever base it last saw. The base is latched three
    ; instructions above, on the RENDER frame, so the two came from different
    ; clocks -- and on a step the game moves up, we latch the new height, and
    ; then overwrite it with old-height-plus-offset. Exactly one frame sits at
    ; the pre-step height, measured at 4 to 11 units, which is the vertical
    ; lurch reported on stairs and confirmed by it vanishing with positional
    ; tracking off.
    ;
    ; Adding here cannot pair this frame's offset with last frame's base. It is
    ; the same fix already made for the ANGLES, which had the same shape:
    ; published on the plugin frame, consumed on the render frame.
    ;
    ; The block above is raw bit moves because mid-function xmm state is live.
    ; This needs real float adds, so exactly one register is borrowed and put
    ; back. movups, so rsp alignment here does not matter.
    sub rsp, 16
    movups xmmword ptr [rsp], xmm0
    movss xmm0, dword ptr [rsi]
    addss xmm0, dword ptr [g_cameraOffsetXBits]
    movss dword ptr [rsi], xmm0
    movss xmm0, dword ptr [rsi+4]
    addss xmm0, dword ptr [g_cameraOffsetYBits]
    movss dword ptr [rsi+4], xmm0
    movss xmm0, dword ptr [rsi+8]
    addss xmm0, dword ptr [g_cameraOffsetZBits]
    movss dword ptr [rsi+8], xmm0
    movups xmm0, xmmword ptr [rsp]
    add rsp, 16
noPositionWrite:
    ; ---------------------------------------------------------------------
    jmp qword ptr [g_cameraStructInterceptionContinue]
skipPrimaryWrites:
    ; Exact reference-mod nowrites path: preserve all source reads/call-side
    ; effects while suppressing only the six camera-float stores displaced by
    ; this primary producer.
    mov ecx, dword ptr [rax+4]
    mov eax, dword ptr [rax+8]
    mov rcx, rbx
    mov rax, qword ptr [rbx]
    call qword ptr [rax+5B0h]
    mov ecx, dword ptr [rax]
    mov ecx, dword ptr [rax+4]
    mov eax, dword ptr [rax+8]
    jmp qword ptr [g_cameraStructInterceptionContinue]
cameraStructInterceptor ENDP

END
