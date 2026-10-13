#ifndef ANGOS_DIAGNOSTICS_H
#define ANGOS_DIAGNOSTICS_H

#include <types.h>

void diagnostics_init(void);
void diagnostics_log(const char *message);

__attribute__((noreturn))
void diagnostics_panic(
    const char *reason,
    u8 vector,
    const void *frame,
    bool has_error_code,
    u64 error_code,
    bool has_fault_address,
    u64 fault_address
);

#endif
