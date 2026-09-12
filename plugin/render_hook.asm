option casemap:none

EXTERN g_renderResolver:QWORD
EXTERN g_renderResolvedObject:QWORD
EXTERN g_renderVtableTarget:QWORD
EXTERN g_renderCallCount:QWORD
EXTERN g_renderLastArgument:DWORD
EXTERN g_renderArgumentMask:QWORD
EXTERN g_renderReturnAddress:QWORD
EXTERN g_nativeSlotCaptureArmed:DWORD
EXTERN g_nativeSlotActive:DWORD
EXTERN CaptureNativeSlotPreOutput:PROC
EXTERN CaptureNativeSlotOutput:PROC
EXTERN BeginNativeSlotRenderDocMarker:PROC
EXTERN EndNativeSlotRenderDocMarker:PROC

PUBLIC renderEntryInterceptor
.code
renderEntryInterceptor PROC
    mov rax, qword ptr [rsp]
    mov qword ptr [g_renderReturnAddress], rax
    push rbx
    sub rsp, 20h
    mov ebx, edx
    mov dword ptr [g_renderLastArgument], edx
    mov eax, edx
    and eax, 3Fh
    lock bts qword ptr [g_renderArgumentMask], rax
resolve_view:
    call qword ptr [g_renderResolver]
    mov qword ptr [g_renderResolvedObject], rax
    mov r8, qword ptr [rax]
    mov rax, qword ptr [r8+0B8h]
    mov qword ptr [g_renderVtableTarget], rax
    lock inc qword ptr [g_renderCallCount]
dispatch:
    mov edx, ebx
    mov rcx, qword ptr [g_renderResolvedObject]
    ; RenderDoc is the supported GPU inspection path.  Only its own module
    ; makes these annotation helpers active; normal gameplay takes the old
    ; zero-extra-work tail-call path below.
    cmp ebx, 1
    je marked_dispatch
    cmp ebx, 2
    je marked_dispatch
    cmp dword ptr [g_nativeSlotCaptureArmed], 0
    jne capture_armed_dispatch
    add rsp, 20h
    pop rbx
    jmp qword ptr [r8+0B8h]

marked_dispatch:
    sub rsp, 40h
    mov qword ptr [rsp+20h], r8
    mov qword ptr [rsp+28h], rcx
    mov ecx, ebx
    call BeginNativeSlotRenderDocMarker
    mov r8, qword ptr [rsp+20h]
    mov rcx, qword ptr [rsp+28h]
    mov edx, ebx
    add rsp, 40h
    cmp dword ptr [g_nativeSlotCaptureArmed], 0
    jne capture_armed_dispatch
    mov dword ptr [g_nativeSlotActive], ebx
    call qword ptr [r8+0B8h]
    mov dword ptr [g_nativeSlotActive], 0
    sub rsp, 40h
    mov qword ptr [rsp+20h], rax
    movdqu xmmword ptr [rsp+30h], xmm0
    mov ecx, ebx
    call EndNativeSlotRenderDocMarker
    mov rax, qword ptr [rsp+20h]
    movdqu xmm0, xmmword ptr [rsp+30h]
    add rsp, 40h
    add rsp, 20h
    pop rbx
    ret

; This path runs only for a hotkey-armed diagnostic.  It calls the already
; resolved native target once, then snapshots the output if its slot is 1/2.
; It never invokes another engine render entry point.
capture_armed_dispatch:
    ; Snapshot before the native target. Preserve its exact ABI arguments
    ; across the diagnostic helper; no engine function is called twice.
    sub rsp, 40h
    mov qword ptr [rsp+20h], r8
    mov qword ptr [rsp+28h], rcx
    mov ecx, ebx
    call CaptureNativeSlotPreOutput
    mov r8, qword ptr [rsp+20h]
    mov rcx, qword ptr [rsp+28h]
    mov edx, ebx
    add rsp, 40h
    mov dword ptr [g_nativeSlotActive], ebx
    call qword ptr [r8+0B8h]
    mov dword ptr [g_nativeSlotActive], 0
    sub rsp, 40h
    mov qword ptr [rsp+20h], rax
    movdqu xmmword ptr [rsp+30h], xmm0
    mov ecx, ebx
    call CaptureNativeSlotOutput
    mov rax, qword ptr [rsp+20h]
    movdqu xmm0, xmmword ptr [rsp+30h]
    add rsp, 40h
    ; Slot 1/2 capture dispatches also received an annotation BeginEvent.
    ; Pair it here while preserving the native return values.
    cmp ebx, 1
    je capture_marker_end
    cmp ebx, 2
    jne capture_finish
capture_marker_end:
    sub rsp, 40h
    mov qword ptr [rsp+20h], rax
    movdqu xmmword ptr [rsp+30h], xmm0
    mov ecx, ebx
    call EndNativeSlotRenderDocMarker
    mov rax, qword ptr [rsp+20h]
    movdqu xmm0, xmmword ptr [rsp+30h]
    add rsp, 40h
capture_finish:
    add rsp, 20h
    pop rbx
    ret
renderEntryInterceptor ENDP
END
