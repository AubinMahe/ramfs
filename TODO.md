# TODO & Pistes d'évolution pour `ramfile`

Ce document récapitule les compléments identifiés pour parfaire l'intégration de `ramfile` dans une base de code legacy des années 90, classés par priorité.

---

## 1. Priorité Haute : Couverture des flux `<stdio.h>` (Prévention des SIGSEGV) - [FAIT]

Dans l'implémentation, un flux `FILE*` retourné par `rf_fopen` est un pseudo-pointeur encodé (`0xBA5E0000 + handle`).
L'interception de l'ensemble des fonctions `<stdio.h>` évite tout déréférencement invalide par la libc.

Fonctions implémentées et redirigées :

- [x] **`fflush(FILE *stream)`** :
  - *Comportement :* Enregistrement immédiat en mémoire (*no-op* retournant 0) pour les flux `ramfile` ; relais transparent vers `fflush` de la libc pour `stdout`/`stderr`/`stdin`.
- [x] **`feof(FILE *stream)` & `ferror(FILE *stream)` & `clearerr(FILE *stream)`** :
  - *Comportement :* Drapeaux d'état EOF et d'erreur maintenus dans `ram_file_access_t`. `rf_clearerr` et `rf_rewind` réinitialisent ces indicateurs.
- [x] **`rewind(FILE *stream)`** :
  - *Comportement :* Remise à zéro de l'offset et des drapeaux EOF/erreur.
- [x] **`fgets(char *s, int size, FILE *stream)` & `fgetc(FILE *stream)` & `getc(FILE *stream)`** :
  - *Comportement :* Lecture d'octets jusqu'à `\n` ou fin de tampon mémoire. Préservation des valeurs non signées (`unsigned char`).
- [x] **`fputs(const char *s, FILE *stream)` & `fputc(int c, FILE *stream)` & `putc(int c, FILE *stream)`** :
  - *Comportement :* Écritures directes avec gestion des erreurs et allocation dynamique de tampon.
- [x] **`remove(const char *pathname)`** :
  - *Comportement :* Fonction et macro redirigées vers `rf_unlink`.

> **Méthode :** Utiliser `./scripts/scan_io.sh --summary <chemin_code_legacy>` pour vérifier précisément quelles fonctions sont appelées par le code cible avant d'en entamer l'implémentation.

---

## 2. Priorité Moyenne : Sonde de taille de fichier (`stat` / `fstat`) - [FAIT]

- [x] **`rf_stat(const char *path, struct stat *buf)`**
- [x] **`rf_fstat(rf_handle_t fd, struct stat *buf)`**
  - *Comportement :* Intercepte les appels `stat` et `fstat`, renseigne `st_size = file->size`, `st_mode = S_IFREG | 0644`, `st_nlink = 1`, `st_mtime/atime/ctime` et les blocs 512o sous Linux/Unix.
  - *Résultat :* Le motif idiomatique des années 90 (`stat` pour allouer le tampon puis `read`/`fread`) fonctionne directement en mémoire vive sans accès disque.

---

## 3. Priorité Moyenne : Normalisation des chemins (Path Sanitization) - [FAIT]

- [x] **Normalisation syntaxique des chemins avant hachage / recherche (`rf_normalize_path`) :**
  - *Comportement :* Remplacement automatique des séparateurs `\` par `/`, élimination des préfixes et composants redondants `./`, repliement des doubles séparateurs `//`, résolution des composants relatifs `..` et conservation des lettres de lecteur Windows (`C:`).
  - *Résultat :* `rf_open`, `rf_unlink`, `rf_stat` et `rf_save` résolvent de manière identique `"donnees.dat"`, `"./donnees.dat"`, `"dir\\sub\\f.dat"` et `"dir//sub/./f.dat"`.

---

## 4. Priorité Basse : Dimensionnement & Jauge de capacité (*High-Water Mark*) - [FAIT]

- [x] **Suivi des métriques d'utilisation et de crête (`rf_get_stats`, `rf_dump_stats`) :**
  - *Choix architectural :* Rejet délibéré du redimensionnement dynamique (`realloc`) pour préserver les pointeurs directs (`access->file`), éviter le coût de re-hachage complet du catalogue et garantir un régime établi à 0 allocation / 0 pic de latence.
  - *Comportement :* Suivi en $O(1)$ du nombre de fichiers (`files_used`, `peak_files`, `files_capacity`) et de descripteurs ouverts (`access_used`, `peak_access`, `access_capacity`).
  - *Résultat :* Calibrage précis de `rf_init(file_count, access_count)` lors des campagnes de test et qualification.

---

## 5. Rappel d'architecture : Concurrence & Threads - [CLASSÉ]

- [x] **Hypothèse mono-thread validée :**
  - L'application legacy cible est strictement mono-thread.
  - La bibliothèque `ramfile` n'embarque aucun verrouillage (`mutex` / sections critiques), par choix délibéré d'architecture afin d'éliminer tout surcoût de synchronisation et maximiser le débit mémoire brut.
  - **Parallélisme multi-processus :** `ramfile` isole naturellement chaque instance dans son propre espace mémoire, facilitant le passage à l'échelle horizontal (multi-processus, batchs parallèles) sans contention ni goulot d'étranglement sur les disques physiques.

---

## 6. Priorité Haute : Support Flex & Bison et moteurs d'indexation - [FAIT]

Le code legacy s'appuie sur **Flex & Bison** pour analyser des requêtes textuelles (langage SQL en français) et des sources binaires, ainsi que sur un moteur d'indexation produisant des fichiers `.idx` et binaires.

Fonctions implémentées et redirigées :

- [x] **`fileno(FILE *stream)` & `isatty(rf_handle_t fd)`** :
  - *Contexte critique :* Flex appelle systématiquement `isatty(fileno(file))` dans `yy_init_buffer` / `yyrestart`. Sans interception, `fileno` de la libc déréférence le faux pointeur `FILE*` et déclenche immédiatement un SIGSEGV.
  - *Comportement :* `rf_fileno` extrait le descripteur mémoire `handle` (ou délègue à libc pour stdin/stdout/stderr). `rf_isatty` renvoie 0 avec `errno = ENOTTY` pour les flux en mémoire, garantissant une détection non-interactive sûre.
- [x] **`ungetc(int c, FILE *stream)`** :
  - *Contexte critique :* Les analyseurs lexicaux (Flex) et parseurs SQL utilisent intensivement `ungetc` pour repousser un caractère de lookahead dans le flux d'entrée.
  - *Comportement :* Décrémente l'offset d'accès, stocke le caractère `c` dans le tampon mémoire et réinitialise le drapeau `eof` (conformément à la norme ISO C).
- [x] **`rename(const char *oldpath, const char *newpath)`** :
  - *Contexte critique :* Les générateurs de fichiers binaires et d'index écrivent leurs résultats dans un fichier temporaire (`.tmp`) puis appellent `rename` pour le substituer de façon atomique au fichier final (`.idx`).
  - *Comportement :* Renommage atomique en mémoire avec normalisation de chemin, remplacement sécurisé si la cible existe déjà (avec protection `EBUSY` si la cible est ouverte) et conservation de la validité des descripteurs déjà ouverts sur l'ancien fichier.
- [x] **`access(const char *pathname, int mode)`** :
  - *Contexte critique :* Vérification d'existence (`F_OK`) et de permissions (`R_OK`, `W_OK`) avant de charger les requêtes, dictionnaires de grammaire et fichiers d'index.
  - *Comportement :* Interrogation en $O(1)$ du catalogue mémoire avec normalisation de chemin. Rejette `X_OK` avec `EACCES` car les pseudo-fichiers en mémoire sont des données.
