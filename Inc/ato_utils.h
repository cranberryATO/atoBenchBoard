#ifndef ATO_UTILS_H
#define ATO_UTILS_H

// True if `a` is an array (false for a pointer). GCC builtin, compile time.
#define IS_ARRAY(a)                                                            \
  (!__builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0])))

// Number of elements of an array, as a constant expression. Fails to compile
// if given a pointer (where sizeof would silently give a wrong count).
// Can be replaced by C2y _Countof(a) once the toolchain is GCC >= 16.
#define COUNT_OF(a)                                                            \
  (sizeof(a) / sizeof((a)[0]) + 0 * sizeof(char[IS_ARRAY(a) ? 1 : -1]))

#endif /* ATO_UTILS_H */
