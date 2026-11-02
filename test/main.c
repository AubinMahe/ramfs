/* Tests unitaires et fonctionnels de ramfs.
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

#include "ramfs.h"

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
   rfs_handle_t fd = rfs_open(name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
   if (fd == RFS_INVALID_HANDLE) {
      return 0;
   }
   size_t len = strlen(content);
   int ok = rfs_write(fd, content, len) == (ssize_t)len;
   return (rfs_close(fd) == 0) && ok;
}

static int has_content(const char *name, const char *expected) {
   char buf[256];
   rfs_handle_t fd = rfs_open(name, O_RDONLY);
   if (fd == RFS_INVALID_HANDLE) {
      return 0;
   }
   ssize_t n = rfs_read(fd, buf, sizeof(buf));
   rfs_close(fd);
   return n == (ssize_t)strlen(expected) && memcmp(buf, expected, (size_t)n) == 0;
}

/* ---------- tests ---------- */

static void test_not_initialized(void) {
   char c = 0;
   rfs_destroy(); /* sans effet, ne doit pas planter */
   CHECK_FAIL(rfs_open("x", O_RDONLY), RFS_INVALID_HANDLE, ENODEV);
   CHECK_FAIL(rfs_read(0, &c, 1), -1, ENODEV);
   CHECK_FAIL(rfs_write(0, &c, 1), -1, ENODEV);
   CHECK_FAIL(rfs_lseek(0, 0, SEEK_SET), -1, ENODEV);
   CHECK_FAIL(rfs_close(0), -1, ENODEV);
}

static void test_open_errors(void) {
   rfs_init(2, 2);
   CHECK_FAIL(rfs_open(NULL, O_RDONLY), RFS_INVALID_HANDLE, EINVAL);
   CHECK_FAIL(rfs_open("a", O_RDONLY), RFS_INVALID_HANDLE, ENOENT);

   rfs_handle_t w = rfs_open("a", O_CREAT | O_WRONLY, 0644);
   CHECK(w >= 0);
   CHECK_FAIL(rfs_open("a", O_CREAT | O_EXCL | O_WRONLY, 0644), RFS_INVALID_HANDLE, EEXIST);
   CHECK_FAIL(rfs_open("a", O_RDONLY), RFS_INVALID_HANDLE, EBUSY); /* écrivain actif */
   CHECK(rfs_close(w) == 0);

   rfs_handle_t r1 = rfs_open("a", O_RDONLY);
   rfs_handle_t r2 = rfs_open("a", O_RDONLY);
   CHECK(r1 >= 0 && r2 >= 0 && r1 != r2); /* lecteurs multiples autorisés */
   CHECK_FAIL(rfs_open("a", O_WRONLY), RFS_INVALID_HANDLE, EBUSY);
   CHECK_FAIL(rfs_open("a", O_RDONLY), RFS_INVALID_HANDLE, ENOMEM); /* table d'accès pleine */
   CHECK(rfs_close(r1) == 0);
   CHECK(rfs_close(r2) == 0);

   rfs_handle_t b = rfs_open("b", O_CREAT | O_WRONLY, 0644);
   CHECK(b >= 0);
   CHECK(rfs_close(b) == 0);
   CHECK_FAIL(rfs_open("c", O_CREAT | O_WRONLY, 0644), RFS_INVALID_HANDLE, ENOMEM); /* catalogue plein */
   CHECK_FAIL(rfs_open("c", O_RDONLY), RFS_INVALID_HANDLE, ENOENT);                 /* rien n'a été créé */
   rfs_destroy();
}

static void test_bad_handles(void) {
   char c = 0;
   rfs_init(2, 2);
   CHECK_FAIL(rfs_read(-1, &c, 1), -1, EBADF);
   CHECK_FAIL(rfs_read(2, &c, 1), -1, EBADF); /* >= capacité */
   CHECK_FAIL(rfs_read(0, &c, 1), -1, EBADF); /* jamais ouvert */
   CHECK_FAIL(rfs_write(0, &c, 1), -1, EBADF);
   CHECK_FAIL(rfs_lseek(0, 0, SEEK_SET), -1, EBADF);
   CHECK_FAIL(rfs_close(0), -1, EBADF);

   rfs_handle_t fd = rfs_open("m", O_CREAT | O_WRONLY, 0644);
   CHECK_FAIL(rfs_read(fd, &c, 1), -1, EBADF); /* lecture sur O_WRONLY */
   CHECK(rfs_close(fd) == 0);
   CHECK_FAIL(rfs_write(fd, &c, 1), -1, EBADF); /* handle fermé */
   fd = rfs_open("m", O_RDONLY);
   CHECK_FAIL(rfs_write(fd, &c, 1), -1, EBADF); /* écriture sur O_RDONLY */
   CHECK(rfs_close(fd) == 0);
   CHECK_FAIL(rfs_close(fd), -1, EBADF); /* double close */
   rfs_destroy();
}

static void test_read_write_seek(void) {
   char buf[32];
   rfs_init(8, 4);
   rfs_handle_t fd = rfs_open("f", O_CREAT | O_RDWR | O_TRUNC, 0644);
   CHECK(rfs_write(fd, "0123456789", 10) == 10);
   CHECK(rfs_lseek(fd, 0, SEEK_CUR) == 10);
   CHECK(rfs_lseek(fd, 0, SEEK_SET) == 0);
   memset(buf, 0, sizeof(buf));
   CHECK(rfs_read(fd, buf, 4) == 4 && memcmp(buf, "0123", 4) == 0);
   CHECK(rfs_lseek(fd, 2, SEEK_CUR) == 6);
   CHECK(rfs_read(fd, buf, sizeof(buf)) == 4 && memcmp(buf, "6789", 4) == 0);
   CHECK(rfs_read(fd, buf, sizeof(buf)) == 0); /* fin de fichier */
   CHECK(rfs_lseek(fd, -3, SEEK_END) == 7);
   CHECK(rfs_read(fd, buf, sizeof(buf)) == 3 && memcmp(buf, "789", 3) == 0);

   CHECK_FAIL(rfs_lseek(fd, -1, SEEK_SET), -1, EINVAL);
   CHECK_FAIL(rfs_lseek(fd, -11, SEEK_END), -1, EINVAL);
   CHECK_FAIL(rfs_lseek(fd, 0, 99), -1, EINVAL);
   CHECK(rfs_lseek(fd, 0, SEEK_CUR) == 10); /* offset inchangé après échec */

   /* réécriture au milieu */
   CHECK(rfs_lseek(fd, 3, SEEK_SET) == 3);
   CHECK(rfs_write(fd, "XY", 2) == 2);
   CHECK(rfs_lseek(fd, 0, SEEK_SET) == 0);
   memset(buf, 0, sizeof(buf));
   CHECK(rfs_read(fd, buf, sizeof(buf)) == 10 && memcmp(buf, "012XY56789", 10) == 0);

   /* écriture après la fin : le trou doit être rempli de zéros */
   CHECK(rfs_lseek(fd, 20, SEEK_SET) == 20);
   CHECK(rfs_write(fd, "Z", 1) == 1);
   CHECK(rfs_lseek(fd, 0, SEEK_END) == 21);
   CHECK(rfs_lseek(fd, 10, SEEK_SET) == 10);
   {
      char zeros[10] = {0};
      memset(buf, 0xFF, sizeof(buf));
      CHECK(rfs_read(fd, buf, 10) == 10 && memcmp(buf, zeros, 10) == 0);
   }
   CHECK(rfs_close(fd) == 0);

   /* O_TRUNC */
   fd = rfs_open("f", O_WRONLY | O_TRUNC);
   CHECK(fd >= 0);
   CHECK(rfs_lseek(fd, 0, SEEK_END) == 0);
   CHECK(rfs_close(fd) == 0);

   /* O_APPEND : l'écriture se fait toujours à la fin, même après lseek */
   CHECK(put("ap", "abc"));
   fd = rfs_open("ap", O_WRONLY | O_APPEND);
   CHECK(fd >= 0);
   CHECK(rfs_lseek(fd, 0, SEEK_SET) == 0);
   CHECK(rfs_write(fd, "def", 3) == 3);
   CHECK(rfs_close(fd) == 0);
   CHECK(has_content("ap", "abcdef"));
   rfs_destroy();
}

static void test_growth(void) {
   static unsigned char src[5000], dst[5000];
   rfs_init(2, 2);
   for (size_t i = 0; i < sizeof(src); i++) {
      src[i] = (unsigned char)((i * 7) % 251);
   }
   rfs_handle_t fd = rfs_open("big", O_CREAT | O_RDWR, 0644);
   size_t done = 0;
   while (done < sizeof(src)) { /* écritures de 777 octets : plusieurs croissances */
      size_t n = sizeof(src) - done < 777 ? sizeof(src) - done : 777;
      CHECK(rfs_write(fd, src + done, n) == (ssize_t)n);
      done += n;
   }
   CHECK(rfs_lseek(fd, 0, SEEK_SET) == 0);
   CHECK(rfs_read(fd, dst, sizeof(dst)) == (ssize_t)sizeof(dst));
   CHECK(memcmp(src, dst, sizeof(src)) == 0);
   CHECK(rfs_close(fd) == 0);
   rfs_destroy();
}

static void test_unlink_and_slots(void) {
   char name[16];
   rfs_init(8, 4);
   for (int i = 0; i < 8; i++) {
      snprintf(name, sizeof(name), "f%d", i);
      CHECK(put(name, name));
   }
   CHECK_FAIL(rfs_open("f8", O_CREAT | O_WRONLY, 0644), RFS_INVALID_HANDLE, ENOMEM);

   CHECK_FAIL(rfs_unlink(NULL), -1, EINVAL);
   CHECK(rfs_unlink("f3") == 0);
   CHECK_FAIL(rfs_unlink("f3"), -1, ENOENT);
   CHECK_FAIL(rfs_open("f3", O_RDONLY), RFS_INVALID_HANDLE, ENOENT);

   CHECK(put("nouveau", "n")); /* l'emplacement libéré est réutilisé */
   CHECK(has_content("nouveau", "n"));
   CHECK(has_content("f2", "f2") && has_content("f4", "f4"));

   rfs_handle_t fd = rfs_open("f1", O_RDONLY);
   CHECK_FAIL(rfs_unlink("f1"), -1, EBUSY);
   CHECK(rfs_close(fd) == 0);
   CHECK(rfs_unlink("f1") == 0);

   /* un fichier recréé après unlink repart vide */
   CHECK(rfs_unlink("nouveau") == 0);
   fd = rfs_open("nouveau", O_CREAT | O_RDWR, 0644);
   CHECK(fd >= 0);
   CHECK(rfs_lseek(fd, 0, SEEK_END) == 0);
   CHECK(rfs_close(fd) == 0);
   rfs_destroy();
}

#define NB_FILES 2000

static void make_name(char *out, size_t cap, int i) {
   snprintf(out, cap, "dir/file_%04d.dat", i); /* préfixe commun, comme en production */
}

/* Insertions/suppressions massives : vérifie la table de hachage (collisions,
   décalage arrière à la suppression, restitution des emplacements libres). */
static void test_hash_integrity(void) {
   char name[64], content[96];
   rfs_init(NB_FILES, 2);
   for (int i = 0; i < NB_FILES; i++) {
      make_name(name, sizeof(name), i);
      CHECK(put(name, name));
   }
   CHECK_FAIL(rfs_open("debordement", O_CREAT | O_WRONLY, 0644), RFS_INVALID_HANDLE, ENOMEM);

   for (int i = 0; i < NB_FILES; i += 2) {
      make_name(name, sizeof(name), i);
      CHECK(rfs_unlink(name) == 0);
   }
   for (int i = 0; i < NB_FILES; i++) {
      make_name(name, sizeof(name), i);
      if (i % 2) {
         CHECK(has_content(name, name));
      } else {
         CHECK_FAIL(rfs_open(name, O_RDONLY), RFS_INVALID_HANDLE, ENOENT);
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
      CHECK(rfs_unlink(name) == 0);
   }
   for (int i = 0; i < NB_FILES; i++) {
      make_name(name, sizeof(name), i);
      CHECK_FAIL(rfs_open(name, O_RDONLY), RFS_INVALID_HANDLE, ENOENT);
   }
   for (int i = 0; i < NB_FILES; i++) { /* tous les emplacements sont redevenus libres */
      make_name(name, sizeof(name), i);
      CHECK(put(name, name));
   }
   rfs_destroy();
}

static void test_stdio_layer(void) {
   char buf[64];
   rfs_init(8, 4);
   CHECK_FAIL(rfs_fopen("s", "z"), NULL, EINVAL);
   CHECK_FAIL(rfs_fopen(NULL, "r"), NULL, EINVAL);
   CHECK_FAIL(rfs_fopen("absent", "r"), NULL, ENOENT);

   FILE *f = rfs_fopen("s", "w");
   CHECK(f != NULL);
   CHECK(rfs_fwrite("abcdef", 1, 6, f) == 6);
   CHECK(rfs_fprintf(f, "|%d|%s\n", 42, "xy") == 7);
   CHECK(rfs_fclose(f) == 0);

   f = rfs_fopen("s", "r");
   CHECK(f != NULL);
   CHECK(rfs_fread(buf, 1, 6, f) == 6 && memcmp(buf, "abcdef", 6) == 0);
   CHECK(rfs_ftell(f) == 6);
   CHECK(rfs_fseek(f, 1, SEEK_SET) == 0);
   CHECK(rfs_ftell(f) == 1);
   CHECK(rfs_fread(buf, 2, 3, f) == 3 && memcmp(buf, "bcdef|", 6) == 0);
   CHECK(rfs_fread(buf, 4, 10, f) == 1); /* 6 octets restants : 1 élément complet de 4 */
   CHECK(rfs_fread(buf, 1, 1, f) == 0);  /* fin de fichier */
   CHECK(rfs_fseek(f, 0, 99) == -1);
   CHECK(rfs_fclose(f) == 0);

   f = rfs_fopen("s", "a");
   CHECK(f != NULL);
   CHECK(rfs_fprintf(f, "tail") == 4);
   CHECK(rfs_fclose(f) == 0);
   f = rfs_fopen("s", "r");
   CHECK(rfs_fseek(f, 0, SEEK_END) == 0);
   CHECK(rfs_ftell(f) == 17); /* 6 + 7 + 4 */
   CHECK(rfs_fclose(f) == 0);

   f = rfs_fopen("s", "r+");
   CHECK(f != NULL);
   CHECK(rfs_fwrite("ZZ", 1, 2, f) == 2);
   CHECK(rfs_fseek(f, 0, SEEK_SET) == 0);
   CHECK(rfs_fread(buf, 1, 4, f) == 4 && memcmp(buf, "ZZcd", 4) == 0);
   CHECK(rfs_fclose(f) == 0);

   f = rfs_fopen("s", "w"); /* "w" tronque */
   CHECK(rfs_fseek(f, 0, SEEK_END) == 0 && rfs_ftell(f) == 0);
   CHECK(rfs_fclose(f) == 0);

   CHECK_FAIL(rfs_fclose(NULL), -1, EBADF);
   rfs_destroy();
}

static int helper_rf_vfprintf(FILE *stream, const char *fmt, ...) {
   va_list args;
   va_start(args, fmt);
   int ret = rfs_vfprintf(stream, fmt, args);
   va_end(args);
   return ret;
}

/* Un vrai FILE* ne doit jamais être pris pour un faux : refus propre, sans plantage. */
static void test_foreign_streams(void) {
   char buf[8];
   rfs_init(8, 4);
   CHECK_FAIL(rfs_fclose(stdout), -1, EBADF);
   CHECK_FAIL(rfs_fclose(stdin), -1, EBADF);
   CHECK_FAIL(rfs_fprintf(stderr, "x"), -1, EBADF);
   CHECK_FAIL(helper_rf_vfprintf(stderr, "x"), -1, EBADF);
   CHECK(rfs_fread(buf, 1, sizeof(buf), stdin) == 0);
   CHECK(rfs_fwrite("x", 1, 1, stdout) == 0);
   CHECK(rfs_fgetc(stdin) == EOF);
   CHECK(rfs_fgets(buf, sizeof(buf), stdin) == NULL);
   CHECK(rfs_fputc('x', stdout) == EOF);
   CHECK(rfs_fputs("x", stdout) == EOF);
   CHECK(rfs_feof(stdin) == 0);
   CHECK(rfs_ferror(stdin) == 0);
   CHECK(rfs_fflush(stdout) == 0);
   CHECK_FAIL(rfs_fseek(stdout, 0, SEEK_SET), -1, EBADF);
   CHECK_FAIL(rfs_ftell(stdout), -1, EBADF);
#if UINTPTR_MAX > 0xFFFFFFFFu
   {
      /* Pointeur dont les 32 bits bas égalent ceux d'un faux FILE* valide :
         sans contrôle de plage, la troncature en int le ferait passer pour valide. */
      FILE *f = rfs_fopen("alias", "w");
      FILE *alias = (FILE *)((uintptr_t)f + ((uintptr_t)1 << 32));
      CHECK(f != NULL);
      CHECK_FAIL(rfs_fclose(alias), -1, EBADF);
      CHECK(rfs_fclose(f) == 0);
   }
#endif
   CHECK(printf("%s", "") == 0); /* stdout est toujours utilisable */
   rfs_destroy();
}

static void test_fscanf(void) {
   int port = 0;
   double timeout = 0.0;
   char mode[32] = {0};
   const char *fmt = "PORT=%d TIMEOUT=%lf MODE=%31s\n";
   rfs_init(2, 2);

   FILE *f = rfs_fopen("cfg", "w");
   CHECK(f != NULL);
   CHECK(rfs_fprintf(f, "PORT=%d TIMEOUT=%.1f MODE=%s\n", 8080, 45.5, "PRODUCTION") > 0);
   CHECK(rfs_fprintf(f, "PORT=%d TIMEOUT=%.1f MODE=%s\n", 9090, 1.5, "TEST") > 0);
   CHECK(rfs_fclose(f) == 0);

   f = rfs_fopen("cfg", "r");
   CHECK(f != NULL);
   CHECK(rfs_fscanf(f, fmt, &port, &timeout, mode) == 3);
   CHECK(port == 8080 && timeout == 45.5 && strcmp(mode, "PRODUCTION") == 0);
   CHECK(rfs_fscanf(f, fmt, &port, &timeout, mode) == 3); /* l'offset a avancé */
   CHECK(port == 9090 && timeout == 1.5 && strcmp(mode, "TEST") == 0);
   CHECK(rfs_fscanf(f, fmt, &port, &timeout, mode) == EOF);
   CHECK(rfs_fclose(f) == 0);
   rfs_destroy();
}

static void test_stdio_extended(void) {
   char line[64];
   rfs_init(4, 4);

   /* 1. Écriture avec fputs et fputc */
   FILE *f = rfs_fopen("text.txt", "w");
   CHECK(f != NULL);
   CHECK(rfs_fputs("Ligne 1\n", f) >= 0);
   CHECK(rfs_fputc('A', f) == 'A');
   CHECK(rfs_fputc('\n', f) == '\n');
   CHECK(rfs_fputs("Ligne 3 sans newline", f) >= 0);
   CHECK(rfs_fflush(f) == 0);
   CHECK(rfs_fclose(f) == 0);

   /* 2. Lecture avec fgets */
   f = rfs_fopen("text.txt", "r");
   CHECK(f != NULL);
   CHECK(rfs_feof(f) == 0);
   CHECK(rfs_ferror(f) == 0);

   char *p = rfs_fgets(line, sizeof(line), f);
   CHECK(p == line && strcmp(line, "Ligne 1\n") == 0);
   CHECK(rfs_feof(f) == 0);

   p = rfs_fgets(line, sizeof(line), f);
   CHECK(p == line && strcmp(line, "A\n") == 0);
   CHECK(rfs_feof(f) == 0);

   p = rfs_fgets(line, sizeof(line), f);
   CHECK(p == line && strcmp(line, "Ligne 3 sans newline") == 0);
   CHECK(rfs_feof(f) == 0);

   p = rfs_fgets(line, sizeof(line), f);
   CHECK(p == NULL);
   CHECK(rfs_feof(f) != 0);

   /* 3. rewind & fgetc */
   rfs_rewind(f);
   CHECK(rfs_feof(f) == 0);
   CHECK(rfs_ftell(f) == 0);

   int c = rfs_fgetc(f);
   CHECK(c == 'L');
   c = rfs_fgetc(f);
   CHECK(c == 'i');

   /* clearerr */
   rfs_clearerr(f);
   CHECK(rfs_feof(f) == 0);
   CHECK(rfs_ferror(f) == 0);

   CHECK(rfs_fclose(f) == 0);

   /* 4. rfs_remove */
   CHECK(rfs_remove("text.txt") == 0);
   CHECK(rfs_fopen("text.txt", "r") == NULL);

   /* 5. Cas d'erreurs et arguments invalides */
   CHECK(rfs_fgets(line, 0, f) == NULL && errno == EINVAL);
   CHECK(rfs_fgets(NULL, 10, f) == NULL && errno == EINVAL);
   CHECK(rfs_fputs(NULL, f) == EOF && errno == EINVAL);
   CHECK(rfs_fflush(NULL) == 0);

   /* 6. Test rfs_vfprintf */
   FILE *vf = rfs_fopen("vfmt.txt", "w");
   CHECK(vf != NULL);
   CHECK(helper_rf_vfprintf(vf, "CODE=%d STR=%s\n", 123, "TEST") > 0);
   CHECK(rfs_fclose(vf) == 0);
   CHECK(has_content("vfmt.txt", "CODE=123 STR=TEST\n"));
   CHECK(rfs_remove("vfmt.txt") == 0);

   /* Rejet de faux pointeurs et arguments invalides */
   CHECK_FAIL(helper_rf_vfprintf((FILE *)0xDEADBEEF, "test"), -1, EBADF);
   CHECK_FAIL(helper_rf_vfprintf(NULL, "test"), -1, EINVAL);
   CHECK_FAIL(helper_rf_vfprintf(f, NULL), -1, EINVAL);

   rfs_destroy();
}

static void test_stat_and_fstat(void) {
   struct stat st;
   rfs_init(4, 2);

   /* 1. Fichier inexistant et arguments invalides */
   CHECK_FAIL(rfs_stat("missing.dat", &st), -1, ENOENT);
   CHECK_FAIL(rfs_stat(NULL, &st), -1, EINVAL);
   CHECK_FAIL(rfs_stat("missing.dat", NULL), -1, EINVAL);
   CHECK_FAIL(rfs_fstat(RFS_INVALID_HANDLE, &st), -1, EBADF);
   CHECK_FAIL(rfs_fstat(0, NULL), -1, EINVAL);

   /* 2. Création et écriture */
   int fd = rfs_open("stat_test.dat", O_CREAT | O_WRONLY | O_TRUNC, 0644);
   CHECK(fd >= 0);
   CHECK(rfs_write(fd, "1234567890", 10) == 10);

   /* 3. fstat sur descripteur ouvert */
   memset(&st, 0, sizeof(st));
   CHECK(rfs_fstat(fd, &st) == 0);
   CHECK(st.st_size == 10);
   CHECK((st.st_mode & S_IFMT) == S_IFREG);
   CHECK(st.st_nlink == 1);
   CHECK(st.st_mtime != 0);

   /* 4. stat par chemin pendant que le fichier est ouvert */
   memset(&st, 0, sizeof(st));
   CHECK(rfs_stat("stat_test.dat", &st) == 0);
   CHECK(st.st_size == 10);
   CHECK((st.st_mode & S_IFMT) == S_IFREG);

   /* 5. stat après fermeture */
   CHECK(rfs_close(fd) == 0);
   CHECK_FAIL(rfs_fstat(fd, &st), -1, EBADF);
   memset(&st, 0, sizeof(st));
   CHECK(rfs_stat("stat_test.dat", &st) == 0);
   CHECK(st.st_size == 10);

   /* 6. stat après suppression */
   CHECK(rfs_unlink("stat_test.dat") == 0);
   CHECK_FAIL(rfs_stat("stat_test.dat", &st), -1, ENOENT);

   rfs_destroy();

   /* 7. Hors initialisation */
   CHECK_FAIL(rfs_stat("stat_test.dat", &st), -1, ENODEV);
   CHECK_FAIL(rfs_fstat(0, &st), -1, ENODEV);
}

static void test_path_normalization(void) {
   char out[128];
   char buf[32];
   struct stat st;

   /* 1. Validation unitaire de rfs_normalize_path */
   CHECK(rfs_normalize_path("donnees.dat", out, sizeof(out)) == 0 && strcmp(out, "donnees.dat") == 0);
   CHECK(rfs_normalize_path("./donnees.dat", out, sizeof(out)) == 0 && strcmp(out, "donnees.dat") == 0);
   CHECK(rfs_normalize_path("././donnees.dat", out, sizeof(out)) == 0 && strcmp(out, "donnees.dat") == 0);
   CHECK(rfs_normalize_path("dir//sub/./f.dat", out, sizeof(out)) == 0 && strcmp(out, "dir/sub/f.dat") == 0);
   CHECK(rfs_normalize_path("dir\\sub\\f.dat", out, sizeof(out)) == 0 && strcmp(out, "dir/sub/f.dat") == 0);
   CHECK(rfs_normalize_path(".\\a\\b.dat", out, sizeof(out)) == 0 && strcmp(out, "a/b.dat") == 0);
   CHECK(rfs_normalize_path("///a/b///c/", out, sizeof(out)) == 0 && strcmp(out, "/a/b/c") == 0);
   CHECK(rfs_normalize_path("a/b/../c", out, sizeof(out)) == 0 && strcmp(out, "a/c") == 0);
   CHECK(rfs_normalize_path("a/b/..", out, sizeof(out)) == 0 && strcmp(out, "a") == 0);
   CHECK(rfs_normalize_path("c:\\dir\\file.txt", out, sizeof(out)) == 0 && strcmp(out, "C:/dir/file.txt") == 0);
   CHECK(rfs_normalize_path(".", out, sizeof(out)) == 0 && strcmp(out, ".") == 0);
   CHECK(rfs_normalize_path("/", out, sizeof(out)) == 0 && strcmp(out, "/") == 0);

   /* Arguments invalides & tampons trop courts */
   CHECK_FAIL(rfs_normalize_path(NULL, out, sizeof(out)), -1, EINVAL);
   CHECK_FAIL(rfs_normalize_path("a", NULL, 10), -1, EINVAL);
   CHECK_FAIL(rfs_normalize_path("a/b/c", out, 3), -1, ENAMETOOLONG);

   /* 2. Intégration dans ramfs : résolution transparente de syntaxes variées */
   rfs_init(4, 4);

   /* Création sous chemin avec préfixe "./" */
   int fd = rfs_open("./dir/sub/f.dat", O_CREAT | O_WRONLY | O_TRUNC, 0644);
   CHECK(fd >= 0);
   CHECK(rfs_write(fd, "NORMALIZED", 10) == 10);
   CHECK(rfs_close(fd) == 0);

   /* Relecture via double slash et antislash */
   fd = rfs_open("dir//sub\\f.dat", O_RDONLY);
   CHECK(fd >= 0);
   memset(buf, 0, sizeof(buf));
   CHECK(rfs_read(fd, buf, sizeof(buf)) == 10);
   CHECK(strcmp(buf, "NORMALIZED") == 0);
   CHECK(rfs_close(fd) == 0);

   /* Relecture via chemin avec composant relatif ".." */
   fd = rfs_open("dir/sub/../sub/f.dat", O_RDONLY);
   CHECK(fd >= 0);
   CHECK(rfs_close(fd) == 0);

   /* Sondage stat avec antislash Windows */
   memset(&st, 0, sizeof(st));
   CHECK(rfs_stat(".\\dir\\sub/f.dat", &st) == 0);
   CHECK(st.st_size == 10);

   /* Suppression avec chemin normalisé différemment */
   CHECK(rfs_unlink("dir/./sub//f.dat") == 0);
   CHECK_FAIL(rfs_open("./dir/sub/f.dat", O_RDONLY), -1, ENOENT);

   rfs_destroy();
}

static void test_stats(void) {
   rfs_stats_t st;

   /* 1. Avant initialisation */
   CHECK_FAIL(rfs_get_stats(&st), -1, ENODEV);
   CHECK_FAIL(rfs_get_stats(NULL), -1, EINVAL);

   /* 2. Après initialisation */
   rfs_init(10, 5);
   CHECK(rfs_get_stats(&st) == 0);
   CHECK(st.files_used == 0 && st.peak_files == 0 && st.files_capacity == 10);
   CHECK(st.access_used == 0 && st.peak_access == 0 && st.access_capacity == 5);

   /* 3. Création et ouverture de 3 fichiers */
   int fd1 = rfs_open("a.dat", O_CREAT | O_RDWR, 0644);
   int fd2 = rfs_open("b.dat", O_CREAT | O_RDWR, 0644);
   int fd3 = rfs_open("c.dat", O_CREAT | O_RDWR, 0644);
   CHECK(fd1 >= 0 && fd2 >= 0 && fd3 >= 0);

   CHECK(rfs_get_stats(&st) == 0);
   CHECK(st.files_used == 3 && st.peak_files == 3);
   CHECK(st.access_used == 3 && st.peak_access == 3);

   /* 4. Fermeture de descripteurs : l'actuel baisse, le pic reste */
   CHECK(rfs_close(fd1) == 0);
   CHECK(rfs_close(fd2) == 0);
   CHECK(rfs_get_stats(&st) == 0);
   CHECK(st.access_used == 1);
   CHECK(st.peak_access == 3);

   /* 5. Suppression de fichier : l'actuel baisse, le pic reste */
   CHECK(rfs_unlink("a.dat") == 0);
   CHECK(rfs_get_stats(&st) == 0);
   CHECK(st.files_used == 2);
   CHECK(st.peak_files == 3);

   /* 6. Ouverture d'un nouveau fichier */
   int fd4 = rfs_open("d.dat", O_CREAT | O_RDWR, 0644);
   CHECK(fd4 >= 0);
   CHECK(rfs_get_stats(&st) == 0);
   CHECK(st.files_used == 3 && st.peak_files == 3);
   CHECK(st.access_used == 2 && st.peak_access == 3);

   CHECK(rfs_close(fd3) == 0);
   CHECK(rfs_close(fd4) == 0);

   /* 7. Affichage synthétique sans crash */
   rfs_dump_stats(NULL);

   rfs_destroy();
   CHECK_FAIL(rfs_get_stats(&st), -1, ENODEV);
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

static void test_save_load_disk(void) {
   char buf[16];
   struct stat st;
   rfs_init(8, 4);
   CHECK(put("dir/sub/a.dat", "AAA"));
   CHECK(put("b.dat", "BBBB"));

   /* Sauvegarde totale sur disque dans test-out */
   CHECK(rfs_save_dir("test-out", NULL, 0) == 0);
   CHECK(read_real("test-out/dir/sub/a.dat", buf, sizeof(buf)) == 3 && memcmp(buf, "AAA", 3) == 0);
   CHECK(read_real("test-out/b.dat", buf, sizeof(buf)) == 4 && memcmp(buf, "BBBB", 4) == 0);

   /* Sauvegarde sélective sur disque dans test-out-sel */
   const char *only_b[] = {"b.dat"};
   CHECK(rfs_save_dir("test-out-sel", only_b, 1) == 0);
   CHECK(read_real("test-out-sel/b.dat", buf, sizeof(buf)) == 4 && memcmp(buf, "BBBB", 4) == 0);
   CHECK(stat("test-out-sel/dir/sub/a.dat", &st) != 0);

   /* Erreur si chemin inexistant */
   const char *missing[] = {"absent.dat"};
   CHECK_FAIL(rfs_save_dir("test-out", missing, 1), -1, ENOENT);

   /* Chargement depuis le disque */
   rfs_destroy();
   rfs_init(8, 4);
   CHECK(rfs_load(NULL, "test-out", NULL) == 2);
   CHECK(has_content("dir/sub/a.dat", "AAA"));
   CHECK(has_content("b.dat", "BBBB"));

   /* Chargement avec préfixe */
   CHECK(rfs_load(NULL, "test-out", "imported") == 2);
   CHECK(has_content("imported/dir/sub/a.dat", "AAA"));
   CHECK(has_content("imported/b.dat", "BBBB"));

   /* Test de l'API dédiée rfs_load_dir */
   rfs_destroy();
   rfs_init(8, 4);
   CHECK(rfs_load_dir("test-out", "direct") == 2);
   CHECK(has_content("direct/dir/sub/a.dat", "AAA"));
   CHECK(has_content("direct/b.dat", "BBBB"));

   /* Nettoyage des répertoires réels */
   remove("test-out/dir/sub/a.dat");
   remove("test-out/b.dat");
   RMDIR("test-out/dir/sub");
   RMDIR("test-out/dir");
   RMDIR("test-out");
   remove("test-out-sel/b.dat");
   RMDIR("test-out-sel");

   rfs_destroy();
}

static void test_save_load_archive(void) {
   rfs_init(8, 4);
   CHECK(put("alpha.txt", "Alpha content 12345"));
   CHECK(put("folder/nested.bin", "Binary Nested Data"));

   /* Sauvegarde totale dans une archive .tar.gz */
   CHECK(rfs_save_archive("test_archive.tar.gz", NULL, 0) == 0);

   /* Sauvegarde sélective dans une autre archive */
   const char *only_alpha[] = {"alpha.txt"};
   CHECK(rfs_save_archive("test_sel.tar.gz", only_alpha, 1) == 0);

   /* Cas d'erreurs */
   const char *missing[] = {"absent.dat"};
   CHECK_FAIL(rfs_save_archive(NULL, NULL, 0), -1, EINVAL);
   CHECK_FAIL(rfs_save_archive("test_sel.tar.gz", missing, 1), -1, ENOENT);

   /* Réinitialisation de la mémoire ramfs */
   rfs_destroy();
   rfs_init(8, 4);

   /* Chargement complet de l'archive */
   CHECK(rfs_load("test_archive.tar.gz", NULL, NULL) == 2);
   CHECK(has_content("alpha.txt", "Alpha content 12345"));
   CHECK(has_content("folder/nested.bin", "Binary Nested Data"));

   /* Chargement avec préfixe */
   CHECK(rfs_load("test_sel.tar.gz", NULL, "restored") == 1);
   CHECK(has_content("restored/alpha.txt", "Alpha content 12345"));

   /* Archive inexistante */
   CHECK_FAIL(rfs_load("archive_qui_n_existe_pas.tar.gz", NULL, NULL), -1, ENOENT);

   /* Nettoyage des archives sur disque */
   remove("test_archive.tar.gz");
   remove("test_sel.tar.gz");

   rfs_destroy();
}

static void test_flex_and_index_layer(void) {
   char buf[64];
   rfs_init(8, 4);

   /* 1. Tests fileno & isatty */
   FILE *f = rfs_fopen("source.sql", "w+");
   CHECK(f != NULL);
   int fd = rfs_fileno(f);
   CHECK(fd >= 0);
   /* Un flux ramfs n'est jamais un tty : isatty doit renvoyer 0 avec errno == ENOTTY */
   errno = 0;
   CHECK(rfs_isatty(fd) == 0 && errno == ENOTTY);

   /* fileno sur flux standard : délégation à libc fileno */
   CHECK(rfs_fileno(stdin) == 0);
   CHECK(rfs_fileno(stdout) == 1);
   CHECK(rfs_fileno(stderr) == 2);

   /* fileno sur pointeur invalide ou NULL */
   CHECK_FAIL(rfs_fileno(NULL), -1, EBADF);
   CHECK_FAIL(rfs_fileno((FILE *)0xDEADBEEF), -1, EBADF);

   /* 2. Tests ungetc pour lexer / tokenizer (Flex) */
   CHECK(rfs_fputs("SELECT *", f) >= 0);
   rfs_rewind(f);

   /* Lecture caractère par caractère avec refoulement ungetc */
   int c = rfs_fgetc(f);
   CHECK(c == 'S');
   CHECK(rfs_ungetc('S', f) == 'S');
   CHECK(rfs_fgetc(f) == 'S');
   c = rfs_fgetc(f);
   CHECK(c == 'E');

   /* Modification de caractère via ungetc */
   CHECK(rfs_ungetc('X', f) == 'X');
   CHECK(rfs_fgetc(f) == 'X');
   c = rfs_fgetc(f);
   CHECK(c == 'L');

   /* Lecture jusqu'à la fin */
   memset(buf, 0, sizeof(buf));
   CHECK(rfs_fgets(buf, sizeof(buf), f) != NULL);
   CHECK(strcmp(buf, "ECT *") == 0);
   CHECK(rfs_fgetc(f) == EOF);
   CHECK(rfs_feof(f) != 0);

   /* ungetc après EOF doit effacer l'indicateur EOF */
   CHECK(rfs_ungetc('!', f) == '!');
   CHECK(rfs_feof(f) == 0);
   CHECK(rfs_fgetc(f) == '!');
   CHECK(rfs_fgetc(f) == EOF);
   CHECK(rfs_feof(f) != 0);

   /* ungetc d'un EOF n'a aucun effet */
   CHECK(rfs_ungetc(EOF, f) == EOF);

   /* ungetc au début du fichier (offset 0) doit échouer */
   rfs_rewind(f);
   CHECK(rfs_ungetc('Z', f) == EOF);

   CHECK(rfs_fclose(f) == 0);

   /* 3. Tests rename pour génération atomique d'index (.tmp -> .idx) */
   CHECK(put("table.tmp", "INDEX_HEADER_DATA_12345"));
   CHECK(rfs_rename("table.tmp", "table.idx") == 0);

   /* L'ancien nom ne doit plus exister */
   CHECK_FAIL(rfs_open("table.tmp", O_RDONLY), -1, ENOENT);
   /* Le nouveau fichier doit avoir le contenu intact */
   CHECK(has_content("table.idx", "INDEX_HEADER_DATA_12345"));

   /* Renommage avec normalisation de chemin */
   CHECK(rfs_rename("./table.idx", "sub/../table_final.idx") == 0);
   CHECK(has_content("table_final.idx", "INDEX_HEADER_DATA_12345"));

   /* Écrasement atomique d'un fichier existant */
   CHECK(put("target.idx", "OLD_INDEX"));
   CHECK(put("source.tmp", "NEW_INDEX"));
   CHECK(rfs_rename("source.tmp", "target.idx") == 0);
   CHECK_FAIL(rfs_open("source.tmp", O_RDONLY), -1, ENOENT);
   CHECK(has_content("target.idx", "NEW_INDEX"));

   /* Renommage vers soi-même */
   CHECK(rfs_rename("target.idx", "./target.idx") == 0);
   CHECK_FAIL(rfs_rename("inexistant.tmp", "inexistant.tmp"), -1, ENOENT);

   /* Renommage échoue si la cible est actuellement ouverte (EBUSY) */
   CHECK(put("locked.idx", "LOCKED_DATA"));
   CHECK(put("update.tmp", "NEW_DATA"));
   int locked_fd = rfs_open("locked.idx", O_RDONLY);
   CHECK(locked_fd >= 0);
   CHECK_FAIL(rfs_rename("update.tmp", "locked.idx"), -1, EBUSY);
   CHECK(rfs_close(locked_fd) == 0);
   /* Une fois fermée, le renommage réussit */
   CHECK(rfs_rename("update.tmp", "locked.idx") == 0);
   CHECK(has_content("locked.idx", "NEW_DATA"));

   /* Renommage pendant que la source est ouverte : les accès ouverts restent valides */
   int open_src = rfs_open("locked.idx", O_RDONLY);
   CHECK(open_src >= 0);
   CHECK(rfs_rename("locked.idx", "locked_moved.idx") == 0);
   memset(buf, 0, sizeof(buf));
   CHECK(rfs_read(open_src, buf, 8) == 8);
   CHECK(memcmp(buf, "NEW_DATA", 8) == 0);
   CHECK(rfs_close(open_src) == 0);
   CHECK(has_content("locked_moved.idx", "NEW_DATA"));

   /* 4. Tests access (vérification existence et droits) */
   CHECK(rfs_access("locked_moved.idx", F_OK) == 0);
   CHECK(rfs_access("locked_moved.idx", R_OK) == 0);
   CHECK(rfs_access("locked_moved.idx", W_OK) == 0);
   CHECK(rfs_access("locked_moved.idx", R_OK | W_OK) == 0);
   /* ramfs ne gère pas de fichiers exécutables */
   CHECK_FAIL(rfs_access("locked_moved.idx", X_OK), -1, EACCES);

   /* Normalisation dans access */
   CHECK(rfs_access("./locked_moved.idx", F_OK) == 0);

   /* Fichier inexistant et arguments invalides */
   CHECK_FAIL(rfs_access("fichier_inconnu.sql", F_OK), -1, ENOENT);
   CHECK_FAIL(rfs_access(NULL, F_OK), -1, EINVAL);

   rfs_destroy();
}

static void test_mkdir_and_rmdir(void) {
   struct stat st;

   /* 1. Avant initialisation */
   CHECK_FAIL(rfs_mkdir("dir"), -1, ENODEV);
   CHECK_FAIL(rfs_rmdir("dir"), -1, ENODEV);

   CHECK(rfs_init(16, 8) == 0);

   /* 2. Arguments invalides */
   CHECK_FAIL(rfs_mkdir(NULL), -1, EINVAL);
   CHECK_FAIL(rfs_rmdir(NULL), -1, EINVAL);

   /* 3. Création de répertoires simples et imbriqués */
   CHECK(rfs_mkdir("my_dir", 0755) == 0);
   /* Re-création échoue avec EEXIST */
   CHECK_FAIL(rfs_mkdir("my_dir", 0755), -1, EEXIST);
   CHECK_FAIL(rfs_mkdir("./my_dir"), -1, EEXIST);

   /* Vérification stat et access */
   CHECK(rfs_stat("my_dir", &st) == 0);
   CHECK(S_ISDIR(st.st_mode));
   CHECK(rfs_access("my_dir", F_OK) == 0);
   CHECK(rfs_access("my_dir", R_OK) == 0);
   CHECK(rfs_access("my_dir", X_OK) == 0);

   /* open et unlink doivent refuser un répertoire avec EISDIR */
   CHECK_FAIL(rfs_open("my_dir", O_RDONLY), RFS_INVALID_HANDLE, EISDIR);
   CHECK_FAIL(rfs_unlink("my_dir"), -1, EISDIR);

   /* Échec de mkdir si le répertoire parent n'existe pas */
   CHECK_FAIL(rfs_mkdir("a/b/c"), -1, ENOENT);

   /* Création séquentielle de l'arborescence */
   CHECK(rfs_mkdir("a") == 0);
   CHECK(rfs_mkdir("a/b") == 0);
   CHECK(rfs_mkdir("a/b/c") == 0);

   /* Création d'un fichier dans le répertoire */
   int fd = rfs_open("a/b/c/file.txt", O_CREAT | O_WRONLY, 0644);
   CHECK(fd >= 0);
   CHECK(rfs_write(fd, "nested", 6) == 6);
   CHECK(rfs_close(fd) == 0);

   /* rmdir sur répertoire non vide échoue avec ENOTEMPTY */
   CHECK_FAIL(rfs_rmdir("a/b/c"), -1, ENOTEMPTY);
   CHECK_FAIL(rfs_rmdir("a"), -1, ENOTEMPTY);

   /* rmdir sur un fichier régulier échoue avec ENOTDIR */
   CHECK_FAIL(rfs_rmdir("a/b/c/file.txt"), -1, ENOTDIR);

   /* Suppression du fichier */
   CHECK(rfs_unlink("a/b/c/file.txt") == 0);

   /* rfs_remove sur un répertoire appelle bien rmdir */
   CHECK(rfs_remove("a/b/c") == 0);
   CHECK_FAIL(rfs_stat("a/b/c", &st), -1, ENOENT);

   /* Nettoyage des répertoires restants */
   CHECK(rfs_rmdir("a/b") == 0);
   CHECK(rfs_rmdir("a") == 0);
   CHECK(rfs_rmdir("my_dir") == 0);

   /* rmdir sur répertoire inexistant échoue avec ENOENT */
   CHECK_FAIL(rfs_rmdir("non_existent"), -1, ENOENT);

   rfs_destroy();
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
   RUN(test_stdio_extended);
   RUN(test_stat_and_fstat);
   RUN(test_path_normalization);
   RUN(test_stats);
   RUN(test_save_load_disk);
   RUN(test_save_load_archive);
   RUN(test_flex_and_index_layer);
   RUN(test_mkdir_and_rmdir);
   {
      int before = g_failures;
      CHECK(test_macros() == 0);
      printf("%-28s %s\n", "test_macros", g_failures == before ? "OK" : "ÉCHEC");
   }
   printf("\n%d vérifications, %d échec(s)\n", g_checks, g_failures);
   return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
