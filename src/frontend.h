/* frontend.h - IR4th parser, type-checker, and compiler driver API
 *
 * Compiles an .ir4 source stream in two passes (parse/type-check into IR,
 * reachability-based dead-code elimination, and emission via Backend).
 */
#ifndef FRONTEND_H
#define FRONTEND_H

#include "ir4.h"

void ir4_add_lib_root(const char *path);
void ir4_compile_stream(FILE *in, const char *path, const Backend *target_be, const char *platform);
int ir4_find_fn(const char *name);
size_t ir4_fn_addr(int idx);
const FnSig *ir4_fn_sig(int idx);

#endif /* FRONTEND_H */
