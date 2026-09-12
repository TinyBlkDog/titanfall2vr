option casemap:none
.code
PUBLIC CallEngineDecline
CallEngineDecline PROC
    push rbx
    sub rsp, 20h
    mov rbx, rdx
    call rcx
    add rsp, 20h
    pop rbx
    ret
CallEngineDecline ENDP
END
