#pragma once
/*
 * QUBES pixel-exact dynamic resolution — shared UM<->KM escape contract.
 * Included by BOTH the KMDOD driver (kernel) and the user-mode resolution
 * corrector / patched gui-agent. All fields are fixed-width ULONG (4 bytes on
 * Windows in both KM and UM), no pointers, so the layout never drifts.
 */

#define QB_ESCAPE_MAGIC    0x51425344UL   /* 'QBSD' */
#define QB_ESCAPE_VERSION  1UL

#define QB_ESC_SET_PREFERRED_MODE  1UL

typedef struct _QB_SETPREFERREDMODE_ESCAPE {
    unsigned long Magic;      /* QB_ESCAPE_MAGIC                    */
    unsigned long Version;    /* QB_ESCAPE_VERSION                  */
    unsigned long Op;         /* QB_ESC_SET_PREFERRED_MODE          */
    unsigned long Width;      /* requested exact width  (IN)        */
    unsigned long Height;     /* requested exact height (IN)        */
    unsigned long StatusOut;  /* NTSTATUS-as-ULONG result (OUT)     */
} QB_SETPREFERREDMODE_ESCAPE, *PQB_SETPREFERREDMODE_ESCAPE;
