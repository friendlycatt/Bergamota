/* Minimal XNU services for running the real pe_fiq.c on QEMU hardware. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef const uint8_t *DTEntry;
typedef int kern_return_t;
#define kSuccess 0
#define SECURITY_READ_ONLY_LATE(type) type
#define MACHINE_TIMEOUT(name, key, value, ...) uint64_t name = (value)
#define MACHINE_TIMEOUT_UNIT_NSEC 1
#define os_atomic_load(p, order) __atomic_load_n(p, __ATOMIC_RELAXED)
#define MPIDR_AFF0_MASK 0xffULL
#define MPIDR_AFF1_MASK 0xff00ULL
#define DSB_SY 15
#define DSB_ISHST 10
#define ISB_SY 15

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int SecureDTLookupEntry(DTEntry, const char *, DTEntry *);
int SecureDTGetProperty(DTEntry, const char *, const void **, unsigned int *);
vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
unsigned int ml_get_cpu_number(uint32_t);
unsigned int ml_get_cpu_number_local(void);
void nanoseconds_to_deadline(uint64_t, uint64_t *);
uint64_t mach_absolute_time(void);
void kprintf(const char *, ...);
void panic(const char *, ...) __attribute__((noreturn));
void _doprnt(const char *, va_list *, void (*)(char), int);
