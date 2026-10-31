/* Tests unitaires et fonctionnels de ramfile.
 * N'utilise pas assert() : les vérifications restent actives avec -DNDEBUG.
 * Code de retour : 0 si tout passe, 1 sinon. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define RMDIR(p) rmdir(p)
#endif

#include "ramfile.h"

int test_macros(void); /* test_macros.c : test de la redirection par macros */

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                                                                \
   do {                                                                                                                            \
      g_checks++;                                                                                                                  \
      if (!(cond)) {                                                                                                               \
         g_failures++;                                                                                                             \
         fprintf(stderr, "ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond);                                                         \
      }                                                                                                                            \
   } while (0)

/* Vérifie la valeur de retour ET errno. */
#define CHECK_FAIL(call, ret, err)                                                                                                 \
   do {                                                                                                                            \
      errno = 0;                                                                                                                   \
      long long r_ = (long long)(call);                                                                                            \
      int e_ = errno;                                                                                                              \
      CHECK(r_ == (long long)(ret));                                                                                               \
      CHECK(e_ == (err));                                                                                                          \
   } while (0)

#define RUN(t)                                                                                                                     \
   do {                                                                                                                            \
      int before_ = g_failures;                                                                                                    \
      t();                                                                                                                         \
      printf("%-28s %s\n", #t, g_failures == before_ ? "OK" : "ÉCHEC");                                                            \
   } while (0)

/* ---------- utilitaires ---------- */

static int put(const char *name, const char *content) {
   rf_handle_t fd = rf_open(name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
   if (fd == RF_INVALID_HANDLE) {
      return 0;
   }
   size_t len = strlen(content);
   int ok = rf_write(fd, content, len) == (ssize_t)len;
   return (rf_close(fd) == 0) && ok;
}

static int has_content(const char *name, const char *expected) {
   char buf[256];
   rf_handle_t fd = rf_open(name, O_RDONLY);
   if (fd == RF_INVALID_HANDLE) {
      return 0;
   }
   ssize_t n = rf_read(fd, buf, sizeof(buf));
   rf_close(fd);
   return n == (ssize_t)strlen(expected) && memcmp(buf, expected, (size_t)n) == 0;
}

/* ---------- tests ---------- */

static void test_not_initialized(void) {
   char c = 0;
   rf_destroy(); /* sans effet, ne doit pas planter */
   CHECK_FAIL(rf_open("x", O_RDONLY), RF_INVALID_HANDLE, ENODEV);
   CHECK_FAIL(rf_read(0, &c, 1), -1, ENODEV);
   CHECK_FAIL(rf_write(0, &c, 1), -1, ENODEV);
   CHECK_FAIL(rf_lseek(0, 0, SEEK_SET), -1, ENODEV);
   CHECK_FAIL(rf_close(0), -1, ENODEV);
}

static void test_open_errors(void) {
   rf_init(2, 2);
   CHECK_FAIL(rf_open(NULL, O_RDONLY), RF_INVALID_HANDLE, EINVAL);
   CHECK_FAIL(rf_open("a", O_RDONLY), RF_INVALID_HANDLE, ENOENT);

   rf_handle_t w = rf_open("a", O_CREAT | O_WRONLY, 0644);
   CHECK(w >= 0);
   CHECK_FAIL(rf_open("a", O_CREAT | O_EXCL | O_WRONLY, 0644), RF_INVALID_HANDLE, EEXIST);
   CHECK_FAIL(rf_open("a", O_RDONLY), RF_INVALID_HANDLE, EBUSY); /* écrivain actif */
   CHECK(rf_close(w) == 0);

   rf_handle_t r1 = rf_open("a", O_RDONLY);
   rf_handle_t r2 = rf_open("a", O_RDONLY);
   CHECK(r1 >= 0 && r2 >= 0 && r1 != r2); /* lecteurs multiples autorisés */
   CHECK_FAIL(rf_open("a", O_WRONLY), RF_INVALID_HANDLE, EBUSY);
   CHECK_FAIL(rf_open("a", O_RDONLY), RF_INVALID_HANDLE, ENOMEM); /* table d'accès pleine */
   CHECK(rf_close(r1) == 0);
   CHECK(rf_close(r2) == 0);

   rf_handle_t b = rf_open("b", O_CREAT | O_WRONLY, 0644);
   CHECK(b >= 0);
   CHECK(rf_close(b) == 0);
   CHECK_FAIL(rf_open("c", O_CREAT | O_WRONLY, 0644), RF_INVALID_HANDLE, ENOMEM); /* catalogue plein */
   CHECK_FAIL(rf_open("c", O_RDONLY), RF_INVALID_HANDLE, ENOENT);                 /* rien n'a été créé */
   rf_destroy();
}

static void test_bad_handles(void) {
   char c = 0;
   rf_init(2, 2);
   CHECK_FAIL(rf_read(-1, &c, 1), -1, EBADF);
   CHECK_FAIL(rf_read(2, &c, 1), -1, EBADF); /* >= capacité */
   CHECK_FAIL(rf_read(0, &c, 1), -1, EBADF); /* jamais ouvert */
   CHECK_FAIL(rf_write(0, &c, 1), -1, EBADF);
   CHECK_FAIL(rf_lseek(0, 0, SEEK_SET), -1, EBADF);
   CHECK_FAIL(rf_close(0), -1, EBADF);

   rf_handle_t fd = rf_open("m", O_CREAT | O_WRONLY, 0644);
   CHECK_FAIL(rf_read(fd, &c, 1), -1, EBADF); /* lecture sur O_WRONLY */
   CHECK(rf_close(fd) == 0);
   CHECK_FAIL(rf_write(fd, &c, 1), -1, EBADF); /* handle fermé */
   fd = rf_open("m", O_RDONLY);
   CHECK_FAIL(rf_write(fd, &c, 1), -1, EBADF); /* écriture sur O_RDONLY */
   CHECK(rf_close(fd) == 0);
   CHECK_FAIL(rf_close(fd), -1, EBADF); /* double close */
   rf_destroy();
}

static void test_read_write_seek(void) {
   char buf[32];
   rf_init(8, 4);
   rf_handle_t fd = rf_open("f", O_CREAT | O_RDWR | O_TRUNC, 0644);
   CHECK(rf_write(fd, "0123456789", 10) == 10);
   CHECK(rf_lseek(fd, 0, SEEK_CUR) == 10);
   CHECK(rf_lseek(fd, 0, SEEK_SET) == 0);
   memset(buf, 0, sizeof(buf));
   CHECK(rf_read(fd, buf, 4) == 4 && memcmp(buf, "0123", 4) == 0);
   CHECK(rf_lseek(fd, 2, SEEK_CUR) == 6);
   CHECK(rf_read(fd, buf, sizeof(buf)) == 4 && memcmp(buf, "6789", 4) == 0);
   CHECK(rf_read(fd, buf, sizeof(buf)) == 0); /* fin de fichier */
   CHECK(rf_lseek(fd, -3, SEEK_END) == 7);
   CHECK(rf_read(fd, buf, sizeof(buf)) == 3 && memcmp(buf, "789", 3) == 0);

   CHECK_FAIL(rf_lseek(fd, -1, SEEK_SET), -1, EINVAL);
   CHECK_FAIL(rf_lseek(fd, -11, SEEK_END), -1, EINVAL);
   CHECK_FAIL(rf_lseek(fd, 0, 99), -1, EINVAL);
   CHECK(rf_lseek(fd, 0, SEEK_CUR) == 10); /* offset inchangé après échec */

   /* réécriture au milieu */
   CHECK(rf_lseek(fd, 3, SEEK_SET) == 3);
   CHECK(rf_write(fd, "XY", 2) == 2);
   CHECK(rf_lseek(fd, 0, SEEK_SET) == 0);
   memset(buf, 0, sizeof(buf));
   CHECK(rf_read(fd, buf, sizeof(buf)) == 10 && memcmp(buf, "012XY56789", 10) == 0);

   /* écriture après la fin : le trou doit être rempli de zéros */
   CHECK(rf_lseek(fd, 20, SEEK_SET) == 20);
   CHECK(rf_write(fd, "Z", 1) == 1);
   CHECK(rf_lseek(fd, 0, SEEK_END) == 21);
   CHECK(rf_lseek(fd, 10, SEEK_SET) == 10);
   {
      char zeros[10] = {0};
      memset(buf, 0xFF, sizeof(buf));
      CHECK(rf_read(fd, buf, 10) == 10 && memcmp(buf, zeros, 10) == 0);
   }
   CHECK(rf_close(fd) == 0);

   /* O_TRUNC */
   fd = rf_open("f", O_WRONLY | O_TRUNC);
   CHECK(fd >= 0);
   CHECK(rf_lseek(fd, 0, SEEK_END) == 0);
   CHECK(rf_close(fd) == 0);

   /* O_APPEND : l'écriture se fait toujours à la fin, même après lseek */
   CHECK(put("ap", "abc"));
   fd = rf_open("ap", O_WRONLY | O_APPEND);
   CHECK(fd >= 0);
   CHECK(rf_lseek(fd, 0, SEEK_SET) == 0);
   CHECK(rf_write(fd, "def", 3) == 3);
   CHECK(rf_close(fd) == 0);
   CHECK(has_content("ap", "abcdef"));
   rf_destroy();
}

static void test_growth(void) {
   static unsigned char src[5000], dst[5000];
   rf_init(2, 2);
   for (size_t i = 0; i < sizeof(src); i++) {
      src[i] = (unsigned char)((i * 7) % 251);
   }
   rf_handle_t fd = rf_open("big", O_CREAT | O_RDWR, 0644);
   size_t done = 0;
   while (done < sizeof(src)) { /* écritures de 777 octets : plusieurs croissances */
      size_t n = sizeof(src) - done < 777 ? sizeof(src) - done : 777;
      CHECK(rf_write(fd, src + done, n) == (ssize_t)n);
      done += n;
   }
   CHECK(rf_lseek(fd, 0, SEEK_SET) == 0);
   CHECK(rf_read(fd, dst, sizeof(dst)) == (ssize_t)sizeof(dst));
   CHECK(memcmp(src, dst, sizeof(src)) == 0);
   CHECK(rf_close(fd) == 0);
   rf_destroy();
}

static void test_unlink_and_slots(void) {
   char name[16];
   rf_init(8, 4);
   for (int i = 0; i < 8; i++) {
      snprintf(name, sizeof(name), "f%d", i);
      CHECK(put(name, name));
   }
   CHECK_FAIL(rf_open("f8", O_CREAT | O_WRONLY, 0644), RF_INVALID_HANDLE, ENOMEM);

   CHECK_FAIL(rf_unlink(NULL), -1, EINVAL);
   CHECK(rf_unlink("f3") == 0);
   CHECK_FAIL(rf_unlink("f3"), -1, ENOENT);
   CHECK_FAIL(rf_open("f3", O_RDONLY), RF_INVALID_HANDLE, ENOENT);

   CHECK(put("nouveau", "n")); /* l'emplacement libéré est réutilisé */
   CHECK(has_content("nouveau", "n"));
   CHECK(has_content("f2", "f2") && has_content("f4", "f4"));

   rf_handle_t fd = rf_open("f1", O_RDONLY);
   CHECK_FAIL(rf_unlink("f1"), -1, EBUSY);
   CHECK(rf_close(fd) == 0);
   CHECK(rf_unlink("f1") == 0);

   /* un fichier recréé après unlink repart vide */
   CHECK(rf_unlink("nouveau") == 0);
   fd = rf_open("nouveau", O_CREAT | O_RDWR, 0644);
   CHECK(fd >= 0);
   CHECK(rf_lseek(fd, 0, SEEK_END) == 0);
   CHECK(rf_close(fd) == 0);
   rf_destroy();
}

#define NB_FILES 2000

static void make_name(char *out, size_t cap, int i) {
   snprintf(out, cap, "dir/file_%04d.dat", i); /* préfixe commun, comme en production */
}

/* Insertions/suppressions massives : vérifie la table de hachage (collisions,
   décalage arrière à la suppression, restitution des emplacements libres). */
static void test_hash_integrity(void) {
   char name[64], content[96];
   rf_init(NB_FILES, 2);
   for (int i = 0; i < NB_FILES; i++) {
      make_name(name, sizeof(name), i);
      CHECK(put(name, name));
   }
   CHECK_FAIL(rf_open("debordement", O_CREAT | O_WRONLY, 0644), RF_INVALID_HANDLE, ENOMEM);

   for (int i = 0; i < NB_FILES; i += 2) {
      make_name(name, sizeof(name), i);
      CHECK(rf_unlink(name) == 0);
   }
   for (int i = 0; i < NB_FILES; i++) {
      make_name(name, sizeof(name), i);
      if (i % 2) {
         CHECK(has_content(name, name));
      } else {
         CHECK_FAIL(rf_open(name, O_RDONLY), RF_INVALID_HANDLE, ENOENT);
      }
   }
   for (int i = 0; i < NB_FILES; i += 2) {
      make_name(name, sizeof(name), i);
      snprintf(content, sizeof(content), "v2:%s", name);
      CHECK(put(name, content));
   }
   for (int i = 0; i < NB_FILES; i++) {
      make_name(name, sizeof(name), i);
      snprintf(content, sizeof(content), "v2:%s", name);
      CHECK(has_content(name, (i % 2) ? name : content));
   }
   for (int i = 0; i < NB_FILES; i++) {
      make_name(name, sizeof(name), i);
      CHECK(rf_unlink(name) == 0);
   }
   for (int i = 0; i < NB_FILES; i++) {
      make_name(name, sizeof(name), i);
      CHECK_FAIL(rf_open(name, O_RDONLY), RF_INVALID_HANDLE, ENOENT);
   }
   for (int i = 0; i < NB_FILES; i++) { /* tous les emplacements sont redevenus libres */
      make_name(name, sizeof(name), i);
      CHECK(put(name, name));
   }
   rf_destroy();
}

static void test_stdio_layer(void) {
   char buf[64];
   rf_init(8, 4);
   CHECK_FAIL(rf_fopen("s", "z"), NULL, EINVAL);
   CHECK_FAIL(rf_fopen(NULL, "r"), NULL, EINVAL);
   CHECK_FAIL(rf_fopen("absent", "r"), NULL, ENOENT);

   FILE *f = rf_fopen("s", "w");
   CHECK(f != NULL);
   CHECK(rf_fwrite("abcdef", 1, 6, f) == 6);
   CHECK(rf_fprintf(f, "|%d|%s\n", 42, "xy") == 7);
   CHECK(rf_fclose(f) == 0);

   f = rf_fopen("s", "r");
   CHECK(f != NULL);
   CHECK(rf_fread(buf, 1, 6, f) == 6 && memcmp(buf, "abcdef", 6) == 0);
   CHECK(rf_ftell(f) == 6);
   CHECK(rf_fseek(f, 1, SEEK_SET) == 0);
   CHECK(rf_ftell(f) == 1);
   CHECK(rf_fread(buf, 2, 3, f) == 3 && memcmp(buf, "bcdef|", 6) == 0);
   CHECK(rf_fread(buf, 4, 10, f) == 1); /* 6 octets restants : 1 élément complet de 4 */
   CHECK(rf_fread(buf, 1, 1, f) == 0);  /* fin de fichier */
   CHECK(rf_fseek(f, 0, 99) == -1);
   CHECK(rf_fclose(f) == 0);

   f = rf_fopen("s", "a");
   CHECK(f != NULL);
   CHECK(rf_fprintf(f, "tail") == 4);
   CHECK(rf_fclose(f) == 0);
   f = rf_fopen("s", "r");
   CHECK(rf_fseek(f, 0, SEEK_END) == 0);
   CHECK(rf_ftell(f) == 17); /* 6 + 7 + 4 */
   CHECK(rf_fclose(f) == 0);

   f = rf_fopen("s", "r+");
   CHECK(f != NULL);
   CHECK(rf_fwrite("ZZ", 1, 2, f) == 2);
   CHECK(rf_fseek(f, 0, SEEK_SET) == 0);
   CHECK(rf_fread(buf, 1, 4, f) == 4 && memcmp(buf, "ZZcd", 4) == 0);
   CHECK(rf_fclose(f) == 0);

   f = rf_fopen("s", "w"); /* "w" tronque */
   CHECK(rf_fseek(f, 0, SEEK_END) == 0 && rf_ftell(f) == 0);
   CHECK(rf_fclose(f) == 0);

   CHECK_FAIL(rf_fclose(NULL), -1, EBADF);
   rf_destroy();
}

/* Un vrai FILE* ne doit jamais être pris pour un faux : refus propre, sans plantage. */
static void test_foreign_streams(void) {
   char buf[8];
   rf_init(8, 4);
   CHECK_FAIL(rf_fclose(stdout), -1, EBADF);
   CHECK_FAIL(rf_fclose(stdin), -1, EBADF);
   CHECK_FAIL(rf_fprintf(stderr, "x"), -1, EBADF);
   CHECK(rf_fread(buf, 1, sizeof(buf), stdin) == 0);
   CHECK(rf_fwrite("x", 1, 1, stdout) == 0);
   CHECK_FAIL(rf_fseek(stdout, 0, SEEK_SET), -1, EBADF);
   CHECK_FAIL(rf_ftell(stdout), -1, EBADF);
#if UINTPTR_MAX > 0xFFFFFFFFu
   {
      /* Pointeur dont les 32 bits bas égalent ceux d'un faux FILE* valide :
         sans contrôle de plage, la troncature en int le ferait passer pour valide. */
      FILE *f = rf_fopen("alias", "w");
      FILE *alias = (FILE *)((uintptr_t)f + ((uintptr_t)1 << 32));
      CHECK(f != NULL);
      CHECK_FAIL(rf_fclose(alias), -1, EBADF);
      CHECK(rf_fclose(f) == 0);
   }
#endif
   CHECK(printf("%s", "") == 0); /* stdout est toujours utilisable */
   rf_destroy();
}

static void test_fscanf(void) {
   int port = 0;
   double timeout = 0.0;
   char mode[32] = {0};
   const char *fmt = "PORT=%d TIMEOUT=%lf MODE=%31s\n";
   rf_init(2, 2);

   FILE *f = rf_fopen("cfg", "w");
   CHECK(f != NULL);
   CHECK(rf_fprintf(f, "PORT=%d TIMEOUT=%.1f MODE=%s\n", 8080, 45.5, "PRODUCTION") > 0);
   CHECK(rf_fprintf(f, "PORT=%d TIMEOUT=%.1f MODE=%s\n", 9090, 1.5, "TEST") > 0);
   CHECK(rf_fclose(f) == 0);

   f = rf_fopen("cfg", "r");
   CHECK(f != NULL);
   CHECK(rf_fscanf(f, fmt, &port, &timeout, mode) == 3);
   CHECK(port == 8080 && timeout == 45.5 && strcmp(mode, "PRODUCTION") == 0);
   CHECK(rf_fscanf(f, fmt, &port, &timeout, mode) == 3); /* l'offset a avancé */
   CHECK(port == 9090 && timeout == 1.5 && strcmp(mode, "TEST") == 0);
   CHECK(rf_fscanf(f, fmt, &port, &timeout, mode) == EOF);
   CHECK(rf_fclose(f) == 0);
   rf_destroy();
}

static int read_real(const char *path, char *buf, size_t cap) {
   FILE *f = fopen(path, "rb");
   if (!f) {
      return -1;
   }
   size_t n = fread(buf, 1, cap, f);
   fclose(f);
   return (int)n;
}

static void test_flush_to_disk(void) {
   char buf[16];
   struct stat st;
   rf_init(4, 2);
   CHECK(put("dir/sub/a.dat", "AAA"));
   CHECK(put("b.dat", "BBBB"));
   CHECK_FAIL(rf_flush_to_disk(NULL), -1, EINVAL);
   CHECK(rf_flush_to_disk("test-out") == 0);

   CHECK(read_real("test-out/dir/sub/a.dat", buf, sizeof(buf)) == 3 && memcmp(buf, "AAA", 3) == 0);
   CHECK(read_real("test-out/b.dat", buf, sizeof(buf)) == 4 && memcmp(buf, "BBBB", 4) == 0);

   remove("test-out/dir/sub/a.dat");
   remove("test-out/b.dat");
   RMDIR("test-out/dir/sub");
   RMDIR("test-out/dir");
   RMDIR("test-out");
   CHECK(stat("test-out", &st) != 0); /* nettoyage effectif */
   rf_destroy();
}

int main(void) {
   RUN(test_not_initialized);
   RUN(test_open_errors);
   RUN(test_bad_handles);
   RUN(test_read_write_seek);
   RUN(test_growth);
   RUN(test_unlink_and_slots);
   RUN(test_hash_integrity);
   RUN(test_stdio_layer);
   RUN(test_foreign_streams);
   RUN(test_fscanf);
   RUN(test_flush_to_disk);
   {
      int before = g_failures;
      CHECK(test_macros() == 0);
      printf("%-28s %s\n", "test_macros", g_failures == before ? "OK" : "ÉCHEC");
   }
   printf("\n%d vérifications, %d échec(s)\n", g_checks, g_failures);
   return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
