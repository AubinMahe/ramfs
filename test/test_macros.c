/* Vérifie que, avec USE_RAMFILE, le code "à l'ancienne" (open/read/write/fopen...)
 * est redirigé vers ramfile et ne touche pas au disque.
 * Les en-têtes système doivent être inclus AVANT ramfile.h : les macros
 * remplacent open/read/write/close/... et casseraient leurs prototypes. */
#include <errno.h>
#include <fcntl.h>
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

#define USE_RAMFILE
#include "ramfile.h"

#define FILENAME "ramfile_macro_test.dat"

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
   rf_init(4, 4);

   /* API de bas niveau */
   int fd = open(FILENAME, O_CREAT | O_RDWR | O_TRUNC, 0644);
   EXPECT(fd != RF_INVALID_HANDLE);
   EXPECT(write(fd, "hello", 5) == 5);
   EXPECT(lseek(fd, 0, SEEK_SET) == 0);
   memset(buf, 0, sizeof(buf));
   EXPECT(read(fd, buf, sizeof(buf) - 1) == 5);
   EXPECT(strcmp(buf, "hello") == 0);

   /* fstat et stat redirigés vers ramfile */
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
   EXPECT(open(FILENAME, O_RDONLY) == RF_INVALID_HANDLE && errno == ENOENT);

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

   rf_destroy();
   return g_fail;
}
