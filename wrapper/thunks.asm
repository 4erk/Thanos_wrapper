; thunks.asm (MASM x64) - exported stubs that jump through g_procTable[]
OPTION CASEMAP:NONE

; Use .data? for extern declarations (no storage here)
.data?

EXTERN g_procTable:QWORD

.code

THUNK MACRO name:req, index:req
    PUBLIC name
    name PROC
        jmp QWORD PTR [g_procTable + (index*8)]
    name ENDP
ENDM

INCLUDE exports.inc

END
