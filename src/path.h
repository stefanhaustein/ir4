#ifndef IR4_PATH_H
#define IR4_PATH_H

/* Path manipulation and executable directory lookup. */
const char *path_join(const char *a, const char *b);
const char *path_resolve(const char *base_path, const char *rel);
void path_set_exe(const char *argv0);
const char *path_exe_dir(void);

#endif
