#ifndef ATO_ERROR_H
#define ATO_ERROR_H

extern void fatalError(const char * fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif /* ATO_ERROR_H */