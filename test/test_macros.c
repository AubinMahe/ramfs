/* Vérifie que, avec USE_RAMFS, le code "à l'ancienne" (open/read/write/fopen...)
 * est redirigé vers ramfs et ne touche pas au disque.
 * Les en-têtes système doivent être inclus AVANT ramfs.h : les macros
 * remplacent open/read/write/close/... et casseraient leurs prototypes. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

static int disk_file_exists(const char *path) {
   FILE *f = fopen(path, "rb");
   if (f) {
      fclose(f);
      return 1;
   }
   return 0;
}

#define USE_RAMFS
#include "ramfs.h"

static int helper_macro_vfprintf(FILE *stream, const char *fmt, ...) {
   va_list args;
   va_start(args, fmt);
   int ret = vfprintf(stream, fmt, args);
   va_end(args);
   return ret;
}

#define FILENAME "ramfs_macro_test.dat"

static int g_fail;

#define EXPECT(cond)                                                                                                               \
   do {                                                                                                                            \
      if (!(cond)) {                                                                                                               \
         g_fail++;                                                                                                                 \
         fprintf(stderr, "ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond);                                                         \
      }                                                                                                                            \
   } while (0)

int test_macros(void) {
   char buf[64];
   struct stat st;
   g_fail = 0;
   rfs_init(4, 4);

   /* API de bas niveau */
   int fd = open(FILENAME, O_CREAT | O_RDWR | O_TRUNC, 0644);
   EXPECT(fd != RFS_INVALID_HANDLE);
   EXPECT(write(fd, "hello", 5) == 5);
   EXPECT(lseek(fd, 0, SEEK_SET) == 0);
   memset(buf, 0, sizeof(buf));
   EXPECT(read(fd, buf, sizeof(buf) - 1) == 5);
   EXPECT(strcmp(buf, "hello") == 0);

   /* fstat et stat redirigés vers ramfs */
   EXPECT(fstat(fd, &st) == 0);
   EXPECT(st.st_size == 5);
   EXPECT(close(fd) == 0);
   EXPECT(stat(FILENAME, &st) == 0);
   EXPECT(st.st_size == 5);
   EXPECT(!disk_file_exists(FILENAME)); /* aucun fichier réel créé sur disque */

   EXPECT(unlink(FILENAME) == 0);
   errno = 0;
   EXPECT(stat(FILENAME, &st) != 0 && errno == ENOENT);
   errno = 0;
   EXPECT(open(FILENAME, O_RDONLY) == RFS_INVALID_HANDLE && errno == ENOENT);

   /* API stdio */
   FILE *f = fopen(FILENAME, "w");
   EXPECT(f != NULL);
   EXPECT(fprintf(f, "PORT=%d TIMEOUT=%.1f MODE=%s\n", 8080, 45.5, "PRODUCTION") > 0);
   EXPECT(fputs("EXTRA_LINE=OK\n", f) >= 0);
   EXPECT(fputc('X', f) == 'X');
   EXPECT(putc('\n', f) == '\n');
   EXPECT(fflush(f) == 0);
   EXPECT(fclose(f) == 0);
   EXPECT(!disk_file_exists(FILENAME));
   EXPECT(stat(FILENAME, &st) == 0);
   EXPECT(st.st_size > 0);

   f = fopen(FILENAME, "r");
   EXPECT(f != NULL);
   EXPECT(feof(f) == 0);
   EXPECT(ferror(f) == 0);
   int port = 0;
   double timeout = 0.0;
   char mode[32] = {0};
   EXPECT(fscanf(f, "PORT=%d TIMEOUT=%lf MODE=%31s\n", &port, &timeout, mode) == 3);
   EXPECT(port == 8080 && timeout == 45.5 && strcmp(mode, "PRODUCTION") == 0);

   memset(buf, 0, sizeof(buf));
   EXPECT(fgets(buf, sizeof(buf), f) == buf);
   EXPECT(strcmp(buf, "EXTRA_LINE=OK\n") == 0);

   int c1 = fgetc(f);
   EXPECT(c1 == 'X');
   int c2 = getc(f);
   EXPECT(c2 == '\n');

   EXPECT(fgetc(f) == EOF);
   EXPECT(feof(f) != 0);

   rewind(f);
   EXPECT(feof(f) == 0);
   EXPECT(ftell(f) == 0);
   clearerr(f);
   EXPECT(fclose(f) == 0);

   EXPECT(remove(FILENAME) == 0);
   errno = 0;
   EXPECT(stat(FILENAME, &st) != 0 && errno == ENOENT);
   errno = 0;
   EXPECT(fopen(FILENAME, "r") == NULL && errno == ENOENT);

   /* Macros Flex & Bison / Indexation : fileno, isatty, ungetc, access, rename */
   FILE *flex_f = fopen("lexer_input.sql", "w+");
   EXPECT(flex_f != NULL);
   int flex_fd = fileno(flex_f);
   EXPECT(flex_fd >= 0);
   errno = 0;
   EXPECT(isatty(flex_fd) == 0 && errno == ENOTTY);

   EXPECT(fputs("FROM TABLE", flex_f) >= 0);
   rewind(flex_f);
   EXPECT(fgetc(flex_f) == 'F');
   EXPECT(ungetc('F', flex_f) == 'F');
   EXPECT(fgetc(flex_f) == 'F');
   EXPECT(fclose(flex_f) == 0);

   /* access */
   EXPECT(access("lexer_input.sql", F_OK) == 0);
   EXPECT(access("lexer_input.sql", R_OK | W_OK) == 0);
   errno = 0;
   EXPECT(access("missing_lexer.sql", F_OK) != 0 && errno == ENOENT);

   /* rename */
   EXPECT(rename("lexer_input.sql", "lexer_input.idx") == 0);
   EXPECT(!disk_file_exists("lexer_input.idx"));
   EXPECT(access("lexer_input.sql", F_OK) != 0);
   EXPECT(access("lexer_input.idx", F_OK) == 0);
   EXPECT(remove("lexer_input.idx") == 0);

   /* vfprintf */
   FILE *vf = fopen("vfprintf_test.txt", "w");
   EXPECT(vf != NULL);
   EXPECT(helper_macro_vfprintf(vf, "LOG_CODE=%d MSG=%s\n", 42, "SUCCESS") > 0);
   EXPECT(fclose(vf) == 0);
   EXPECT(!disk_file_exists("vfprintf_test.txt"));
   EXPECT(remove("vfprintf_test.txt") == 0);

   /* mkdir et rmdir */
   EXPECT(mkdir("macro_dir", 0755) == 0);
   EXPECT(!disk_file_exists("macro_dir"));
   EXPECT(access("macro_dir", F_OK) == 0);
   EXPECT(rmdir("macro_dir") == 0);
   EXPECT(access("macro_dir", F_OK) != 0);

   EXPECT(_mkdir("macro_dir2") == 0);
   EXPECT(!disk_file_exists("macro_dir2"));
   EXPECT(access("macro_dir2", F_OK) == 0);
   EXPECT(_rmdir("macro_dir2") == 0);
   EXPECT(access("macro_dir2", F_OK) != 0);

   /* dup, dup2, ftruncate, truncate via macros */
   int fd_m = open("macro_dup.dat", O_CREAT | O_RDWR, 0644);
   EXPECT(fd_m >= 0);
   EXPECT(write(fd_m, "hello world test", 16) == 16);
   int fd_m2 = dup(fd_m);
   EXPECT(fd_m2 >= 0);
   EXPECT(lseek(fd_m, 6, SEEK_SET) == 6);
   EXPECT(lseek(fd_m2, 0, SEEK_CUR) == 6);
   EXPECT(close(fd_m) == 0);
   EXPECT(ftruncate(fd_m2, 5) == 0);
   EXPECT(close(fd_m2) == 0);
   EXPECT(truncate("macro_dup.dat", 4) == 0);
   EXPECT(stat("macro_dup.dat", &st) == 0 && st.st_size == 4);
   EXPECT(unlink("macro_dup.dat") == 0);

   /* chdir, getcwd, _chdir, _getcwd, opendir, readdir, closedir via macros */
   EXPECT(mkdir("dir_macro", 0755) == 0);
   int f_in_dir = open("dir_macro/f.txt", O_CREAT | O_WRONLY, 0644);
   EXPECT(f_in_dir >= 0 && close(f_in_dir) == 0);

   DIR *d_m = opendir("dir_macro");
   EXPECT(d_m != NULL);
   int cnt = 0;
   struct dirent *ent_m;
   while ((ent_m = readdir(d_m)) != NULL) {
      cnt++;
   }
   EXPECT(cnt == 3); /* ., .., f.txt */
   EXPECT(closedir(d_m) == 0);

   char cwd_buf[256];
   EXPECT(getcwd(cwd_buf, sizeof(cwd_buf)) != NULL);
   EXPECT(chdir("dir_macro") == 0);
   EXPECT(stat("f.txt", &st) == 0);
   EXPECT(chdir("..") == 0);
   EXPECT(unlink("dir_macro/f.txt") == 0);
   EXPECT(rmdir("dir_macro") == 0);

   /* tmpfile et getline via macros */
   FILE *t_m = tmpfile();
   EXPECT(t_m != NULL);
   EXPECT(fprintf(t_m, "macro line 1\nmacro line 2\n") > 0);
   rewind(t_m);
   char *l_m = NULL;
   size_t n_m = 0;
   EXPECT(getline(&l_m, &n_m, t_m) > 0);
   EXPECT(strcmp(l_m, "macro line 1\n") == 0);
   EXPECT(getline(&l_m, &n_m, t_m) > 0);
   EXPECT(strcmp(l_m, "macro line 2\n") == 0);
   free(l_m);
   EXPECT(fclose(t_m) == 0);

   /* Mode VFS Hybride via macros */
   EXPECT(rfs_mount("/ramfs") == 0);
   unlink("build_linux/macro_real.txt");
   int real_m = open("build_linux/macro_real.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
   EXPECT(real_m >= 0 && (unsigned)real_m < 0x40000000);
   EXPECT(write(real_m, "real", 4) == 4);
   EXPECT(close(real_m) == 0);

   int ram_m = open("/ramfs/macro_ram.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
   EXPECT(ram_m >= (int)0x40000000);
   EXPECT(write(ram_m, "ram", 3) == 3);
   EXPECT(close(ram_m) == 0);

   EXPECT(disk_file_exists("build_linux/macro_real.txt"));
   EXPECT(!disk_file_exists("/ramfs/macro_ram.txt"));

   EXPECT(unlink("build_linux/macro_real.txt") == 0);
   EXPECT(unlink("/ramfs/macro_ram.txt") == 0);
   rfs_unmount();

   rfs_destroy();
   return g_fail;
}
