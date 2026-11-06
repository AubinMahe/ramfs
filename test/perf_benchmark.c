#include "ramfs.h"
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)

static double now_sec(void) {
   LARGE_INTEGER freq, cnt;
   QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&cnt);
   return (double)cnt.QuadPart / (double)freq.QuadPart;
}
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define RMDIR(p) rmdir(p)

static double now_sec(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

#define NUM_FILES 5000
#define FILE_SIZE 1024
#define DISK_DIR "build_linux/bench_disk"

static char g_payload[FILE_SIZE];
static char g_read_buf[FILE_SIZE];

typedef struct {
   double t_create_write;
   double t_read_verify;
   double t_update_seek;
   double t_stat;
   double t_unlink;
   double t_total;
} bench_metrics_t;

/* =========================================================================
 * Exécution du banc de test : mode disque (0) ou mode ramfs (1)
 * ========================================================================= */
static int run_benchmark_workload(int is_ram, bench_metrics_t *m) {
   char path[PATH_MAX];
   double t0;

   /* 1. Création et écriture séquentielle */
   t0 = now_sec();
   for (int i = 0; i < NUM_FILES; i++) {
      if (is_ram) {
         snprintf(path, sizeof(path), "/bench_file_%05d.dat", i);
         rfs_handle_t fd = rfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
         if (fd < 0) {
            return -1;
         }
         if (rfs_write(fd, g_payload, FILE_SIZE) != FILE_SIZE) {
            rfs_close(fd);
            return -1;
         }
         rfs_close(fd);
      } else {
         snprintf(path, sizeof(path), "%s/bench_file_%05d.dat", DISK_DIR, i);
         int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
         if (fd < 0) {
            return -1;
         }
         if (write(fd, g_payload, FILE_SIZE) != FILE_SIZE) {
            close(fd);
            return -1;
         }
         close(fd);
      }
   }
   m->t_create_write = now_sec() - t0;

   /* 2. Relecture avec vérification de contenu (2 passes complètes) */
   t0 = now_sec();
   for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < NUM_FILES; i++) {
         if (is_ram) {
            snprintf(path, sizeof(path), "/bench_file_%05d.dat", i);
            rfs_handle_t fd = rfs_open(path, O_RDONLY, 0);
            if (fd < 0) {
               return -1;
            }
            if (rfs_read(fd, g_read_buf, FILE_SIZE) != FILE_SIZE) {
               rfs_close(fd);
               return -1;
            }
            rfs_close(fd);
         } else {
            snprintf(path, sizeof(path), "%s/bench_file_%05d.dat", DISK_DIR, i);
            int fd = open(path, O_RDONLY, 0);
            if (fd < 0) {
               return -1;
            }
            if (read(fd, g_read_buf, FILE_SIZE) != FILE_SIZE) {
               close(fd);
               return -1;
            }
            close(fd);
         }
      }
   }
   m->t_read_verify = now_sec() - t0;

   /* 3. Mises à jour in-place avec seek (2 500 fichiers modifiés) */
   t0 = now_sec();
   char update_token[] = "MODIFIED_IN_PLACE_BLOCK";
   for (int i = 0; i < NUM_FILES / 2; i++) {
      if (is_ram) {
         snprintf(path, sizeof(path), "/bench_file_%05d.dat", i);
         rfs_handle_t fd = rfs_open(path, O_RDWR, 0);
         if (fd < 0) {
            return -1;
         }
         rfs_lseek(fd, 256, SEEK_SET);
         if (rfs_write(fd, update_token, sizeof(update_token)) != (ssize_t)sizeof(update_token)) {
            rfs_close(fd);
            return -1;
         }
         rfs_close(fd);
      } else {
         snprintf(path, sizeof(path), "%s/bench_file_%05d.dat", DISK_DIR, i);
         int fd = open(path, O_RDWR, 0);
         if (fd < 0) {
            return -1;
         }
         lseek(fd, 256, SEEK_SET);
         if (write(fd, update_token, sizeof(update_token)) != (ssize_t)sizeof(update_token)) {
            close(fd);
            return -1;
         }
         close(fd);
      }
   }
   m->t_update_seek = now_sec() - t0;

   /* 4. Sondage d'état (stat) */
   t0 = now_sec();
   struct stat st;
   for (int i = 0; i < NUM_FILES; i++) {
      if (is_ram) {
         snprintf(path, sizeof(path), "/bench_file_%05d.dat", i);
         if (rfs_stat(path, &st) != 0) {
            return -1;
         }
      } else {
         snprintf(path, sizeof(path), "%s/bench_file_%05d.dat", DISK_DIR, i);
         if (stat(path, &st) != 0) {
            return -1;
         }
      }
   }
   m->t_stat = now_sec() - t0;

   /* 5. Suppression de fichiers (unlink) */
   t0 = now_sec();
   for (int i = 0; i < NUM_FILES; i++) {
      if (is_ram) {
         snprintf(path, sizeof(path), "/bench_file_%05d.dat", i);
         if (rfs_unlink(path) != 0) {
            return -1;
         }
      } else {
         snprintf(path, sizeof(path), "%s/bench_file_%05d.dat", DISK_DIR, i);
         if (unlink(path) != 0) {
            return -1;
         }
      }
   }
   m->t_unlink = now_sec() - t0;

   m->t_total = m->t_create_write + m->t_read_verify + m->t_update_seek + m->t_stat + m->t_unlink;
   return 0;
}

int main(void) {
   /* Initialisation du motif mémoire */
   memset(g_payload, 'K', sizeof(g_payload) - 1);
   g_payload[sizeof(g_payload) - 1] = '\0';

   printf("================================================================================\n");
   printf("       BANC COMPARATIF DISQUE vs RAMFS (%d fichiers de %d octets)\n", NUM_FILES, FILE_SIZE);
   printf("       Version ramfs testée : %s\n", rfs_version());
   printf("================================================================================\n\n");

   /* -------------------------------------------------------------------------
    * Phase 1 : Exécution sur DISQUE PHYSIQUE
    * ------------------------------------------------------------------------- */
   MKDIR("build_linux");
   MKDIR(DISK_DIR);
   bench_metrics_t m_disk = {0};
   printf("[1/2] Exécution des opérations sur DISQUE PHYSIQUE (%s) ...\n", DISK_DIR);
   if (run_benchmark_workload(0, &m_disk) != 0) {
      fprintf(stderr, "Erreur lors de l'exécution sur disque\n");
      RMDIR(DISK_DIR);
      return 1;
   }
   RMDIR(DISK_DIR);
   printf("      Terminé en %.4f secondes.\n\n", m_disk.t_total);

   /* -------------------------------------------------------------------------
    * Phase 2 : Exécution sur RAMFS
    * ------------------------------------------------------------------------- */
   bench_metrics_t m_ram = {0};
   printf("[2/2] Exécution des mêmes opérations sur RAMFS ...\n");
   if (rfs_init(NUM_FILES + 1024, 16) != 0) {
      fprintf(stderr, "Erreur rfs_init\n");
      return 1;
   }
   if (run_benchmark_workload(1, &m_ram) != 0) {
      fprintf(stderr, "Erreur lors de l'exécution sur ramfs\n");
      rfs_destroy();
      return 1;
   }
   rfs_destroy();
   printf("      Terminé en %.4f secondes.\n\n", m_ram.t_total);

   /* -------------------------------------------------------------------------
    * Rapport comparatif détaillé & Calcul du ratio d'accélération
    * ------------------------------------------------------------------------- */
   printf("================================================================================\n");
   printf("                      RÉSULTATS DÉTAILLÉS PAR OPÉRATION                         \n");
   printf("================================================================================\n");
   printf("%-30s | %-12s | %-12s | %-10s\n", "Phase", "Disque (s)", "ramfs (s)", "Facteur");
   printf("--------------------------------------------------------------------------------\n");

   double r_write = (m_ram.t_create_write > 0) ? (m_disk.t_create_write / m_ram.t_create_write) : 0;
   double r_read = (m_ram.t_read_verify > 0) ? (m_disk.t_read_verify / m_ram.t_read_verify) : 0;
   double r_upd = (m_ram.t_update_seek > 0) ? (m_disk.t_update_seek / m_ram.t_update_seek) : 0;
   double r_stat = (m_ram.t_stat > 0) ? (m_disk.t_stat / m_ram.t_stat) : 0;
   double r_unl = (m_ram.t_unlink > 0) ? (m_disk.t_unlink / m_ram.t_unlink) : 0;
   double r_total = (m_ram.t_total > 0) ? (m_disk.t_total / m_ram.t_total) : 0;

   printf("%-30s | %10.4f s | %10.4f s | %8.1fx\n", "Création & écriture (5000)", m_disk.t_create_write, m_ram.t_create_write,
          r_write);
   printf("%-30s | %10.4f s | %10.4f s | %8.1fx\n", "Relecture x2 (10000)", m_disk.t_read_verify, m_ram.t_read_verify, r_read);
   printf("%-30s | %10.4f s | %10.4f s | %8.1fx\n", "Mises à jour in-place (2500)", m_disk.t_update_seek, m_ram.t_update_seek,
          r_upd);
   printf("%-30s | %10.4f s | %10.4f s | %8.1fx\n", "Sondage stat (5000)", m_disk.t_stat, m_ram.t_stat, r_stat);
   printf("%-30s | %10.4f s | %10.4f s | %8.1fx\n", "Suppression unlink (5000)", m_disk.t_unlink, m_ram.t_unlink, r_unl);
   printf("================================================================================\n");
   printf("%-30s | %10.4f s | %10.4f s | %8.1fx\n", "TOTAL GÉNÉRAL", m_disk.t_total, m_ram.t_total, r_total);
   printf("================================================================================\n\n");

   if (r_total >= 30.0) {
      printf(">>> RÉSULTAT CONFORME : Facteur d'accélération de %.1fx (>= objectif de 30x) <<<\n\n", r_total);
   } else {
      printf(">>> AVERTISSEMENT : Facteur d'accélération de %.1fx (< objectif de 30x) <<<\n\n", r_total);
   }

   return 0;
}
