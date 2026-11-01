# `ramfile` : Pseudo-fichiers en mémoire

Bibliothèque C (C11) autonome, multiplateforme (Linux & Windows) et haute performance, conçue pour émuler les entrées/sorties fichiers standard (POSIX et `<stdio.h>`) directement en mémoire vive.

---

## Contexte & Problématique

Dans les anciennes applications, de nombreux modules utilisent intensivement le système de fichiers pour stocker des états intermédiaires, faute de disposer de capacité mémoire suffisante à l'époque.

Sur des architectures modernes, ces opérations génèrent des millions d'appels système (`open`, `write`, `read`, `close`), saturent le VFS du noyau et dégradent les performances.

**`ramfile` résout ce problème à la racine :**

* **Redirection transparente :** En activant `#define USE_RAMFILE`, les fonctions standard (`open`, `read`, `fopen`, `fprintf`, etc.) sont redirigées sans modifier une seule ligne du code métier legacy.
* **Zéro E/S disque :** Les fichiers vivent exclusivement en RAM.
* **Persistance à la demande :** Sauvegarde sélective sur disque ou dans une archive compressée `.tar.gz` en fin de traitement.

---

## Architecture & Choix Techniques

### Catalogue & Table de hachage

* **Adressage ouvert & sondage linéaire :** Facteur de charge maintenu ≤ 0,5 (taille toujours une puissance de 2), garantissant un coût d'accès en *O*(1) moyen.
* **Hachage matériel CRC32C (SSE4.2) :** Traitement par blocs de 8 octets en 3 cycles processeur, remplaçant FNV-1a pour une accélération de x20 sur la résolution des chemins, avec repli portable.
* **Suppression par décalage arrière (*backward shift deletion*) :** Pas de pierres tombales (*tombstones*), maintenant l'intégrité et la vitesse des chaînes de sondage après des milliers d'ajouts/suppressions.
* **Recyclage des descripteurs :** Pile LIFO (`g_free_stack`) pour une réattribution immédiate des emplacements libérés.

### Couche d'accès & Sémantique POSIX

* **Concurrence lecteur/écrivain :** Lecteurs multiples autorisés (`O_RDONLY`), mais exclusivité stricte dès qu'un écrivain est actif (`EBUSY`).
* **Fichiers creux (*sparse files*) :** Un `lseek` au-delà de la fin du fichier suivi d'une écriture comble automatiquement le trou avec des zéros (`0x00`).
* **Drapeaux supportés :** `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_RDONLY`, `O_WRONLY`, `O_RDWR`.

### Couche `stdio` & Faux pointeurs `FILE*`

* Pour éviter d'allouer de vraies structures internes de la libc ou de dépendre d'extensions non portables (`fopencookie`/`funopen`), `ramfile` encode ses descripteurs dans une plage d'adresses virtuelles débutant à `0xBA5E0000`.
* **Sécurité mémoire :** Tout vrai pointeur de flux (`stdin`, `stdout`, `stderr` ou fichier réel) passé aux fonctions `rf_*` est intercepté et rejeté proprement avec `EBADF`, éliminant tout risque de plantage ou de corruption.

### Support Flex, Bison & Moteurs d'indexation

* **Compatibilité Flex (`fileno` & `isatty`) :** Le scanner généré par Flex appelle inconditionnellement `isatty(fileno(file))` pour tester l'interactivité de son flux d'entrée (`yy_is_interactive`). `ramfile` intercepte ces appels : `rf_fileno` extrait le descripteur virtuel sans déréférencer le pointeur, et `rf_isatty` renvoie `0` (`ENOTTY`), prévenant tout SIGSEGV libc et garantissant un comportement non-interactif déterministe.
* **Refoulement `ungetc` :** Support complet de la réinjection de caractères pour les automates lexicaux et grammaires SQL (Bison), avec réinitialisation conforme de l'indicateur `EOF`.
* **Commutation atomique (`rename`) :** Remplacement atomique de fichiers temporaires (`.tmp`) vers des fichiers de production (`.idx`, `.bin`) directement en mémoire vive, avec gestion de la concurrence (`EBUSY` si la cible est ouverte).
* **Vérification de droits (`access`) :** Validation instantanée de la présence (`F_OK`) et des droits d'accès (`R_OK`, `W_OK`) pour les requêtes, dictionnaires et index.

### Modèle d'exécution : Mono-thread & Multi-processus

> [!WARNING]
> **`ramfile` n'est pas thread-safe (*Not Multi-Thread Safe*)**, et ce **par choix délibéré d'architecture**.
> L'absence de verrous (`mutex`, sections critiques) élimine tout surcoût de synchronisation et permet d'atteindre le débit mémoire maximal en mono-thread.
>
> **Parallélisme multi-processus :** En revanche, `ramfile` est idéal pour exécuter plusieurs instances d'applications en parallèle (multi-processus, workers batch). Chaque processus disposant de son propre espace d'adressage isolé en RAM, la concurrence s'effectue sans aucune collision de noms de fichiers réels, sans verrous système et sans saturation de bande passante disque.

---

## Sauvegarde & Chargement : `rf_save` / `rf_load`

Deux fonctions permettent de persister et de restaurer l'état mémoire :

```c
/* Sauvegarde sélective ou totale vers une archive .tar.gz ou le disque */
int rf_save(const char *const *paths, size_t path_count, const char *archive_path, const char *dest_dir);

/* Chargement et décompression d'une archive .tar.gz ou importation depuis le disque */
int rf_load(const char *archive_path, const char *src_dir, const char *prefix);
```

* **Pérennité du format d'archive :** Utilisation du format standard **POSIX ustar compressé via `zlib` (`.tar.gz`)**. Les archives générées sont directement lisibles et extractibles par les outils système (`tar -tvf archive.tar.gz`).
* **Arborescence relative :** L'organisation hiérarchique des dossiers (ex: `dir/sub/file.dat`) est intégralement préservée.
* **Relocalisation :** Le paramètre `prefix` de `rf_load` permet de restaurer une archive sous un sous-dossier virtuel en mémoire.

---

## Performances & Mesures

Sur un scénario de **4 000 fichiers** manipulés (création, écriture de 512 octets, double relecture) :

* **Vitesse brute :** `ramfile` est mesuré **25x à 30x plus rapide** que le système de fichiers réel sur SSD (sans `fsync`/`fflush`).
* **Régime établi (*steady state*) :** Une fois les fichiers initialisés, `ramfile` réutilise ses tampons existants (`O_TRUNC`), éliminant 100 % des allocations dynamiques (`malloc`) et des *page-faults* du système d'exploitation.

---

## Compilation & Assurance Qualité (QA)

Le projet dispose d'une chaîne de build et de test complète pilotée par le `Makefile` :

### Prérequis

```bash
# Sous Debian / Ubuntu :
sudo apt install build-essential zlib1g-dev libz-mingw-w64-dev valgrind wine

# Sous RHEL 9 / Rocky Linux 9 (dépôt EPEL requis pour MinGW et Wine) :
sudo dnf install -y epel-release  # ou https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm
sudo dnf install -y gcc make zlib-devel mingw64-gcc mingw64-zlib valgrind wine
```

### Commandes principales

| Commande | Description |
| :--- | :--- |
| `make linux` | Compile la bibliothèque dynamique `libramfile.so`, les tests et benchmarks Linux |
| `make test_linux` | Exécute la suite de tests unitaires Linux (17 400+ assertions) |
| `make windows` | Compile la DLL Windows `ramfile.dll` et les exécutables via MinGW-w64 |
| `make test_win` | Exécute la suite de tests Windows sous Wine |
| `make asan` | Compile et teste avec **AddressSanitizer** et **UndefinedBehaviorSanitizer** |
| `make valgrind` | Analyse approfondie des fuites mémoire et accès invalides via Valgrind |
| `make efence` | Traque des dépassements de tampons mémoires via Electric Fence |
| `make perf` | Mesure des compteurs matériels et profilage CPU via Linux `perf` |
| `make format-sources`| Formate l'ensemble des sources (`.c`, `.h`) selon `.clang-format` |
| `make clean` | Nettoie tous les répertoires de compilation |
