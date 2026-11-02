# TODO & Pistes d'évolution pour `ramfs`

Ce document récapitule les compléments identifiés pour parfaire l'intégration de `ramfs` dans une base de code legacy des années 90, classés par priorité.

---

## 1. Priorité Haute : Couverture des flux `<stdio.h>` (Prévention des SIGSEGV) - [FAIT]

Dans l'implémentation, un flux `FILE*` retourné par `rfs_fopen` est un pseudo-pointeur encodé (`0xBA5E0000 + handle`).
L'interception de l'ensemble des fonctions `<stdio.h>` évite tout déréférencement invalide par la libc.

Fonctions implémentées et redirigées :

- [x] **`fflush(FILE *stream)`** :
  - *Comportement :* Enregistrement immédiat en mémoire (*no-op* retournant 0) pour les flux `ramfs` ; relais transparent vers `fflush` de la libc pour `stdout`/`stderr`/`stdin`.
- [x] **`feof(FILE *stream)` & `ferror(FILE *stream)` & `clearerr(FILE *stream)`** :
  - *Comportement :* Drapeaux d'état EOF et d'erreur maintenus dans `ram_file_access_t`. `rfs_clearerr` et `rfs_rewind` réinitialisent ces indicateurs.
- [x] **`rewind(FILE *stream)`** :
  - *Comportement :* Remise à zéro de l'offset et des drapeaux EOF/erreur.
- [x] **`fgets(char *s, int size, FILE *stream)` & `fgetc(FILE *stream)` & `getc(FILE *stream)`** :
  - *Comportement :* Lecture d'octets jusqu'à `\n` ou fin de tampon mémoire. Préservation des valeurs non signées (`unsigned char`).
- [x] **`fputs(const char *s, FILE *stream)` & `fputc(int c, FILE *stream)` & `putc(int c, FILE *stream)`** :
  - *Comportement :* Écritures directes avec gestion des erreurs et allocation dynamique de tampon.
- [x] **`vfprintf(FILE *stream, const char *format, va_list ap)`** :
  - *Comportement :* Écriture formatée via liste d'arguments variables `va_list` (utilisée par les fonctions de log et de rapport d'erreur) ; relais transparent vers `vfprintf` de la libc pour les flux standard.
- [x] **`remove(const char *pathname)`** :
  - *Comportement :* Fonction et macro redirigées vers `rfs_unlink`.

> **Méthode :** Utiliser `./scripts/scan_io.sh --summary <chemin_code_legacy>` pour vérifier précisément quelles fonctions sont appelées par le code cible avant d'en entamer l'implémentation.

---

## 2. Priorité Moyenne : Sonde de taille de fichier (`stat` / `fstat`) - [FAIT]

- [x] **`rfs_stat(const char *path, struct stat *buf)`**
- [x] **`rfs_fstat(rfs_handle_t fd, struct stat *buf)`**
  - *Comportement :* Intercepte les appels `stat` et `fstat`, renseigne `st_size = file->size`, `st_mode = S_IFREG | 0644`, `st_nlink = 1`, `st_mtime/atime/ctime` et les blocs 512o sous Linux/Unix.
  - *Résultat :* Le motif idiomatique des années 90 (`stat` pour allouer le tampon puis `read`/`fread`) fonctionne directement en mémoire vive sans accès disque.

---

## 3. Priorité Moyenne : Normalisation des chemins (Path Sanitization) - [FAIT]

- [x] **Normalisation syntaxique des chemins avant hachage / recherche (`rfs_normalize_path`) :**
  - *Comportement :* Remplacement automatique des séparateurs `\` par `/`, élimination des préfixes et composants redondants `./`, repliement des doubles séparateurs `//`, résolution des composants relatifs `..` et conservation des lettres de lecteur Windows (`C:`).
  - *Résultat :* `rfs_open`, `rfs_unlink`, `rfs_stat` et `rfs_save` résolvent de manière identique `"donnees.dat"`, `"./donnees.dat"`, `"dir\\sub\\f.dat"` et `"dir//sub/./f.dat"`.

---

## 4. Priorité Basse : Dimensionnement & Jauge de capacité (*High-Water Mark*) - [FAIT]

- [x] **Suivi des métriques d'utilisation et de crête (`rfs_get_stats`, `rfs_dump_stats`) :**
  - *Choix architectural :* Rejet délibéré du redimensionnement dynamique (`realloc`) pour préserver les pointeurs directs (`access->file`), éviter le coût de re-hachage complet du catalogue et garantir un régime établi à 0 allocation / 0 pic de latence.
  - *Comportement :* Suivi en $O(1)$ du nombre de fichiers (`files_used`, `peak_files`, `files_capacity`) et de descripteurs ouverts (`access_used`, `peak_access`, `access_capacity`).
  - *Résultat :* Calibrage précis de `rfs_init(file_count, access_count)` lors des campagnes de test et qualification.

---

## 5. Rappel d'architecture : Concurrence & Threads - [CLASSÉ]

- [x] **Hypothèse mono-thread validée :**
  - L'application legacy cible est strictement mono-thread.
  - La bibliothèque `ramfs` n'embarque aucun verrouillage (`mutex` / sections critiques), par choix délibéré d'architecture afin d'éliminer tout surcoût de synchronisation et maximiser le débit mémoire brut.
  - **Parallélisme multi-processus :** `ramfs` isole naturellement chaque instance dans son propre espace mémoire, facilitant le passage à l'échelle horizontal (multi-processus, batchs parallèles) sans contention ni goulot d'étranglement sur les disques physiques.

---

## 6. Priorité Haute : Support Flex & Bison et moteurs d'indexation - [FAIT]

Le code legacy s'appuie sur **Flex & Bison** pour analyser des requêtes textuelles (langage SQL en français) et des sources binaires, ainsi que sur un moteur d'indexation produisant des fichiers `.idx` et binaires.

Fonctions implémentées et redirigées :

- [x] **`fileno(FILE *stream)` & `isatty(rfs_handle_t fd)`** :
  - *Contexte critique :* Flex appelle systématiquement `isatty(fileno(file))` dans `yy_init_buffer` / `yyrestart`. Sans interception, `fileno` de la libc déréférence le faux pointeur `FILE*` et déclenche immédiatement un SIGSEGV.
  - *Comportement :* `rfs_fileno` extrait le descripteur mémoire `handle` (ou délègue à libc pour stdin/stdout/stderr). `rfs_isatty` renvoie 0 avec `errno = ENOTTY` pour les flux en mémoire, garantissant une détection non-interactive sûre.
- [x] **`ungetc(int c, FILE *stream)`** :
  - *Contexte critique :* Les analyseurs lexicaux (Flex) et parseurs SQL utilisent intensivement `ungetc` pour repousser un caractère de lookahead dans le flux d'entrée.
  - *Comportement :* Décrémente l'offset d'accès, stocke le caractère `c` dans le tampon mémoire et réinitialise le drapeau `eof` (conformément à la norme ISO C).
- [x] **`rename(const char *oldpath, const char *newpath)`** :
  - *Contexte critique :* Les générateurs de fichiers binaires et d'index écrivent leurs résultats dans un fichier temporaire (`.tmp`) puis appellent `rename` pour le substituer de façon atomique au fichier final (`.idx`).
  - *Comportement :* Renommage atomique en mémoire avec normalisation de chemin, remplacement sécurisé si la cible existe déjà (avec protection `EBUSY` si la cible est ouverte) et conservation de la validité des descripteurs déjà ouverts sur l'ancien fichier.
- [x] **`access(const char *pathname, int mode)`** :
  - *Contexte critique :* Vérification d'existence (`F_OK`) et de permissions (`R_OK`, `W_OK`) avant de charger les requêtes, dictionnaires de grammaire et fichiers d'index.
  - *Comportement :* Interrogation en $O(1)$ du catalogue mémoire avec normalisation de chemin. Rejette `X_OK` avec `EACCES` car les pseudo-fichiers en mémoire sont des données.

---

## 7. Priorité Moyenne : Gestion des répertoires virtuels (`mkdir`, `rmdir`) - [FAIT]

Le code legacy ou les scripts de traitement créent ou suppriment des sous-dossiers lors de l'exécution ou de l'import d'arborescences.

Fonctions implémentées et redirigées :

- [x] **`mkdir(const char *pathname, ...)` & `_mkdir(const char *pathname)`** :
  - *Comportement :* Création d'entrées de répertoires virtuels en mémoire (`file->is_dir = 1`), vérification de l'existence du répertoire parent (`ENOENT`), refus des doublons (`EEXIST`). Signature variadique compatible à la fois avec le `mkdir` POSIX (2 arguments) et le `_mkdir` Windows MinGW (1 argument).
- [x] **`rmdir(const char *pathname)` & `_rmdir(const char *pathname)`** :
  - *Comportement :* Suppression sécurisée de répertoires vides en mémoire. Refus des dossiers non vides (`ENOTEMPTY`) et des fichiers réguliers (`ENOTDIR`).
- [x] **Cohérence système (`open`, `unlink`, `remove`, `stat`, `access`, `rename`)** :
  - `rfs_open` et `rfs_unlink` rejettent les répertoires avec `EISDIR`.
  - `rfs_remove` délègue automatiquement à `rfs_rmdir` pour les répertoires et à `rfs_unlink` pour les fichiers.
  - `rfs_stat` positionne `S_IFDIR | 0755` pour les répertoires.
  - `rfs_access` autorise `X_OK` sur les répertoires virtuels.
  - `rfs_save` et `rfs_load` prennent en charge les dossiers (typeflag `'5'` dans les archives `.tar.gz`, création récursive sur disque sans appel erroné à `fopen`).
