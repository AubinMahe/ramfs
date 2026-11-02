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
#define mkdir(path, mode) _mkdir(path)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "ramfs.h"

#define NUM_FILES 4000
#define FILE_SIZE 512
#ifndef MEASURES
#define MEASURES 1
#endif
#define TEST_DIR "/sur_le_ssd/bench_dir"

static char payload[FILE_SIZE];

void run_scenario(int is_ram) {
   char filename[PATH_MAX];
   char read_buf[FILE_SIZE];
   for (int i = 0; i < NUM_FILES; i++) {
      snprintf(filename, sizeof(filename), TEST_DIR "/file_%04d.dat", i);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
      snprintf(payload, sizeof(payload), "%s\n", filename);
#pragma GCC diagnostic pop
      payload[sizeof(payload) - 2] = '\n';
      payload[sizeof(payload) - 1] = '\0';
      int fd;
      if (is_ram) {
         fd = rfs_open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
      } else {
         fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
      }
      if (fd >= 0) {
         if (is_ram) {
            rfs_write(fd, payload, FILE_SIZE);
            rfs_close(fd);
         } else {
            ssize_t unused = write(fd, payload, FILE_SIZE);
            (void)unused;
            close(fd);
         }
      } else {
         perror(filename);
         exit(1);
      }
   }
   for (int j = 0; j < 2; j++) {
      for (int i = 0; i < NUM_FILES; i++) {
         snprintf(filename, sizeof(filename), TEST_DIR "/file_%04d.dat", i);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
         snprintf(payload, sizeof(payload), "%s\n", filename);
#pragma GCC diagnostic pop
         int fd;
         if (is_ram) {
            fd = rfs_open(filename, O_RDONLY);
         } else {
            fd = open(filename, O_RDONLY);
         }
         if (fd >= 0) {
            if (is_ram) {
               rfs_read(fd, read_buf, FILE_SIZE);
               rfs_close(fd);
            } else {
               ssize_t unused = read(fd, read_buf, FILE_SIZE);
               (void)unused;
               close(fd);
            }
            assert(strcmp(read_buf, payload) == 0);
         } else {
            perror(filename);
            exit(2);
         }
      }
   }
}

#ifdef _WIN32
#include <windows.h>

static double now_seconds(void) {
   LARGE_INTEGER freq, cnt;
   QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&cnt);
   return (double)cnt.QuadPart / (double)freq.QuadPart;
}
#else
static double now_seconds(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

int main(void) {
   memset(payload, 'A', FILE_SIZE - 1);
   payload[FILE_SIZE - 1] = '\0';
   mkdir(TEST_DIR, 0755);
   double disk = 0.0;
   unsigned count = MEASURES;
   while (count--) {
      const double start = now_seconds();
      run_scenario(0);
      disk += now_seconds() - start;
   }
   printf("Disque : %.4f secondes\n", disk);
   rfs_init(NUM_FILES, 1);
   double ram = 0.0;
   count = MEASURES;
   while (count--) {
      const double start = now_seconds();
      run_scenario(1);
      ram += now_seconds() - start;
   }
   printf("RAM    : %.4f secondes\n", ram);
   double ratio = disk / ram;
   if (ram > 0) {
      printf("ramfs est environ %.1fx plus rapide que le filesystem SSD.\n", ratio);
   } else {
      printf("Le temps RAM est trop proche de 0 pour calculer un ratio précis.\n");
   }
   char filename[128];
   for (int i = 0; i < NUM_FILES; i++) {
      snprintf(filename, sizeof(filename), TEST_DIR "/file_%04d.dat", i);
      remove(filename);
   }
   rmdir(TEST_DIR);
   rfs_destroy();
   return 0;
}
