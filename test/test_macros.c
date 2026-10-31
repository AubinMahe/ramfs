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
   EXPECT(close(fd) == 0);
   EXPECT(stat(FILENAME, &st) != 0); /* aucun fichier réel créé */
   EXPECT(unlink(FILENAME) == 0);
   errno = 0;
   EXPECT(open(FILENAME, O_RDONLY) == RF_INVALID_HANDLE && errno == ENOENT);

   /* API stdio */
   FILE *f = fopen(FILENAME, "w");
   EXPECT(f != NULL);
   EXPECT(fprintf(f, "PORT=%d TIMEOUT=%.1f MODE=%s\n", 8080, 45.5, "PRODUCTION") > 0);
   EXPECT(fclose(f) == 0);
   EXPECT(stat(FILENAME, &st) != 0);

   f = fopen(FILENAME, "r");
   EXPECT(f != NULL);
   int port = 0;
   double timeout = 0.0;
   char mode[32] = {0};
   EXPECT(fscanf(f, "PORT=%d TIMEOUT=%lf MODE=%31s\n", &port, &timeout, mode) == 3);
   EXPECT(port == 8080 && timeout == 45.5 && strcmp(mode, "PRODUCTION") == 0);
   EXPECT(fclose(f) == 0);

   rf_destroy();
   return g_fail;
}
