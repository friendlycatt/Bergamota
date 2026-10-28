/* Linux/x86_64 migcom only: glibc scalar types with XNU's real Mach ABI headers.
 * Never included when compiling target kernel code or preprocessing .defs.
 * Suppress Darwin's libc scalar typedefs, which collide with glibc's typedefs.
 */
#include <stdint.h>
#include <sys/types.h>
#if !defined(__linux__) || !defined(__x86_64__)
#error This host adapter is for Linux/x86_64 only
#endif
#define _I386_MACHTYPES_H_ 1
#define _BSD_I386__TYPES_H_ 1
typedef unsigned int __darwin_natural_t;
typedef uint64_t user_addr_t;
#define __kernel_ptr_semantics
#define __enum_decl(name, type, ...) typedef type name; enum __VA_ARGS__
#define __options_decl(name, type, ...) typedef type name; enum __VA_ARGS__
