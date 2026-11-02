#!/usr/bin/env bash
# ==============================================================================
# scan_io.sh - Analyse et détection des E/S fichiers (int fd et FILE*)
# ==============================================================================
#
# LISTE DES FONCTIONS RECHERCHÉES :
#
# 1. Descripteurs bas niveau / POSIX (int fd) :
#    - Cycle de vie       : open, creat, openat, close, dup, dup2, dup3
#    - Transfert données  : read, write, pread, pwrite, readv, writev
#    - Positionnement     : lseek, llseek
#    - Contrôle & sync    : fcntl, ioctl, fsync, fdatasync, ftruncate, isatty
#    - Métadonnées        : fstat, fchmod, fchown
#    - Suppression        : unlink, unlinkat
#    - Équivalents Win32  : _open, _read, _write, _close, _lseek, _chsize, _isatty
#
# 2. Flux standard C <stdio.h> (FILE*) :
#    - Ouverture/Fermeture: fopen, freopen, fdopen, fclose, fcloseall, tmpfile
#    - Blocs binaires     : fread, fwrite
#    - E/S formatées      : fprintf, vfprintf, fscanf, vfscanf
#    - Caractères & lignes: fgets, fputs, fgetc, fputc, getc, putc, ungetc, getline, getdelim
#    - Positionnement     : fseek, fseeko, ftell, ftello, rewind, fgetpos, fsetpos
#    - État & tampon      : feof, ferror, clearerr, fflush, fileno, setbuf, setvbuf
#    - Tubes / Processus  : popen, pclose
#
# 3. Opérations sur le système de fichiers (chemins réels) :
#    - Gestion entrées    : remove, rename, renameat, access, stat, lstat
#
# 4. Parcours d'arborescence et répertoires :
#    - Création / Suppression : mkdir, _mkdir, mkdirat, rmdir, _rmdir
#    - Flux répertoire        : opendir, fdopendir, readdir, readdir_r, closedir, rewinddir, seekdir, telldir, dirfd
#    - Balayage & Tri         : scandir, scandirat, alphasort, versionsort
#    - Arborescences & Globs  : ftw, nftw, glob, globfree
#    - Répertoire de travail  : chdir, fchdir, _chdir, getcwd, _getcwd, getwd, get_current_dir_name
# ==============================================================================

set -euo pipefail

# Listes exhaustives pour la recherche
FD_FUNCS=(
   open creat openat close dup dup2 dup3
   read write pread pwrite readv writev
   lseek llseek fcntl ioctl fsync fdatasync
   fstat fchmod fchown ftruncate unlink unlinkat isatty
   _open _read _write _close _lseek _chsize _isatty
)

FILE_FUNCS=(
   fopen freopen fdopen fclose fcloseall tmpfile
   fread fwrite fprintf vfprintf fscanf vfscanf
   fgets fputs fgetc fputc getc putc ungetc
   getline getdelim fseek fseeko ftell ftello
   rewind fgetpos fsetpos feof ferror clearerr
   fflush fileno setbuf setvbuf popen pclose
)

FS_FUNCS=(
   remove rename renameat access stat lstat
)

DIR_FUNCS=(
   mkdir _mkdir mkdirat rmdir _rmdir
   opendir fdopendir readdir readdir_r closedir rewinddir seekdir telldir dirfd
   scandir scandirat alphasort versionsort
   ftw nftw glob globfree
   chdir fchdir _chdir getcwd _getcwd getwd get_current_dir_name
)

# Couleurs pour la lisibilité
C_RESET="\033[0m"
C_BOLD="\033[1m"
C_CYAN="\033[36m"
C_GREEN="\033[32m"
C_YELLOW="\033[33m"
C_RED="\033[31m"
C_GRAY="\033[90m"

# Affichage de l'en-tête listant les fonctions recherchées
print_header() {
   cat << 'EOF'
==============================================================================
   SCAN DES ENTRÉES/SORTIES FICHIERS DANS LE CODE SOURCE (int fd & FILE*)
==============================================================================
EOF
   echo -e "${C_BOLD}1. Descripteurs bas niveau (int fd) :${C_RESET}"
   echo -e "   ${FD_FUNCS[*]}"
   echo ""
   echo -e "${C_BOLD}2. Flux standard C <stdio.h> (FILE*) :${C_RESET}"
   echo -e "   ${FILE_FUNCS[*]}"
   echo ""
   echo -e "${C_BOLD}3. Gestion système de fichiers (chemins) :${C_RESET}"
   echo -e "   ${FS_FUNCS[*]}"
   echo ""
   echo -e "${C_BOLD}4. Parcours d'arborescence et répertoires :${C_RESET}"
   echo -e "   ${DIR_FUNCS[*]}"
   echo "=============================================================================="
   echo ""
}

usage() {
   echo "Usage: $0 [OPTIONS] [RÉPERTOIRE_OU_FICHIER]"
   echo ""
   echo "Options:"
   echo "  -a, --all         Recherche toutes les catégories (défaut)"
   echo "  -f, --fd-only     Recherche uniquement les descripteurs (int fd)"
   echo "  -F, --file-only   Recherche uniquement les flux (FILE*)"
   echo "  -d, --dir-only    Recherche uniquement les répertoires et arborescences (mkdir, opendir, readdir, ...)"
   echo "  -s, --summary     Affiche le récapitulatif (fonctions et fichiers concernés)"
   echo "  -l, --files       Affiche uniquement la liste des fichiers concernés"
   echo "  -t, --todo        Filtre pour n'afficher que les fichiers restant à équiper"
   echo "      --raw         Affiche uniquement les chemins bruts des fichiers (un par ligne)"
   echo "  -h, --help        Affiche cette aide"
   echo ""
   echo "Exemples :"
   echo "  $0 /chemin/vers/code_legacy"
   echo "  $0 --summary ./legacy_src"
   echo "  $0 --files ./legacy_src"
   echo "  $0 -l --todo ./legacy_src"
   echo "  $0 -l --raw ./legacy_src"
   exit 0
}

# Paramètres par défaut
TARGET_PATH="."
MODE="all"
SUMMARY_ONLY=0
FILES_ONLY=0
TODO_ONLY=0
RAW_OUTPUT=0

while [[ $# -gt 0 ]]; do
   case "$1" in
      -a|--all)
         MODE="all"
         shift
         ;;
      -f|--fd-only)
         MODE="fd"
         shift
         ;;
      -F|--file-only)
         MODE="file"
         shift
         ;;
      -d|--dir-only)
         MODE="dir"
         shift
         ;;
      -s|--summary)
         SUMMARY_ONLY=1
         shift
         ;;
      -l|--files|--files-only)
         FILES_ONLY=1
         shift
         ;;
      -t|--todo)
         TODO_ONLY=1
         shift
         ;;
      --raw)
         RAW_OUTPUT=1
         shift
         ;;
      -h|--help)
         usage
         ;;
      -*)
         echo -e "${C_RED}Option inconnue : $1${C_RESET}" >&2
         usage
         ;;
      *)
         TARGET_PATH="$1"
         shift
         ;;
   esac
done

if [[ ! -e "$TARGET_PATH" ]]; then
   echo -e "${C_RED}Erreur : La cible '$TARGET_PATH' n'existe pas.${C_RESET}" >&2
   exit 1
fi

# Affichage de l'en-tête (sauf en mode chemins bruts ou liste exclusive)
if [[ $RAW_OUTPUT -eq 0 && $FILES_ONLY -eq 0 ]]; then
   print_header
fi

# Construction de la liste des fonctions selon le mode
SELECTED_FUNCS=()
case "$MODE" in
   fd)
      SELECTED_FUNCS=("${FD_FUNCS[@]}")
      [[ $RAW_OUTPUT -eq 0 && $FILES_ONLY -eq 0 ]] && echo -e "${C_YELLOW}Mode actif : Descripteurs (int fd) uniquement${C_RESET}"
      ;;
   file)
      SELECTED_FUNCS=("${FILE_FUNCS[@]}")
      [[ $RAW_OUTPUT -eq 0 && $FILES_ONLY -eq 0 ]] && echo -e "${C_YELLOW}Mode actif : Flux (FILE*) uniquement${C_RESET}"
      ;;
   dir)
      SELECTED_FUNCS=("${DIR_FUNCS[@]}")
      [[ $RAW_OUTPUT -eq 0 && $FILES_ONLY -eq 0 ]] && echo -e "${C_YELLOW}Mode actif : Répertoires et arborescences uniquement${C_RESET}"
      ;;
   all)
      SELECTED_FUNCS=("${FD_FUNCS[@]}" "${FILE_FUNCS[@]}" "${FS_FUNCS[@]}" "${DIR_FUNCS[@]}")
      [[ $RAW_OUTPUT -eq 0 && $FILES_ONLY -eq 0 ]] && echo -e "${C_YELLOW}Mode actif : Toutes les fonctions (fd, FILE*, chemins, répertoires)${C_RESET}"
      ;;
esac

# Affichage du mode et de la cible (sauf en mode brut ou liste exclusive)
if [[ $RAW_OUTPUT -eq 0 && $FILES_ONLY -eq 0 ]]; then
   echo -e "${C_GRAY}Cible analysée : $TARGET_PATH${C_RESET}"
   echo ""
fi

# Construction de la regex d'appel de fonction : \b(func1|func2|...)[[:space:]]*\(
REGEX_FUNCS=$(IFS='|'; echo "${SELECTED_FUNCS[*]}")
PATTERN="\\b(${REGEX_FUNCS})[[:space:]]*\\("

# Liste des fichiers sources à analyser (C, C++, headers)
FIND_CMD=(find "$TARGET_PATH" -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" -o -name "*.hpp" \) \
          ! -path "*/.git/*" ! -path "*/build_*/*" ! -path "*/build/*")

FILES=()
while IFS= read -r file; do
   FILES+=("$file")
done < <("${FIND_CMD[@]}" 2>/dev/null || true)

if [[ ${#FILES[@]} -eq 0 ]]; then
   if [[ -f "$TARGET_PATH" ]]; then
      FILES=("$TARGET_PATH")
   else
      if [[ $RAW_OUTPUT -eq 0 ]]; then
         echo -e "${C_YELLOW}Aucun fichier source C/C++ trouvé dans '$TARGET_PATH'.${C_RESET}"
      fi
      exit 0
   fi
fi

# Tableau associatif pour compter les occurrences par fonction (Bash 4+)
declare -A COUNTS
for f in "${SELECTED_FUNCS[@]}"; do
   COUNTS["$f"]=0
done

declare -A FILE_CALLS
declare -A FILE_EQUIPPED
MATCHED_FILES=()

TOTAL_MATCHES=0

if [[ $SUMMARY_ONLY -eq 0 && $FILES_ONLY -eq 0 && $RAW_OUTPUT -eq 0 ]]; then
   echo -e "${C_BOLD}Occurrences détaillées trouvées :${C_RESET}"
   echo "------------------------------------------------------------------------------"
fi

# Parcours des fichiers et extraction des lignes
for file in "${FILES[@]}"; do
   # Extraction des correspondances avec numéros de lignes
   MATCHES=$(grep -En "$PATTERN" "$file" 2>/dev/null || true)
   if [[ -n "$MATCHES" ]]; then
      FILE_COUNT=0
      if [[ $SUMMARY_ONLY -eq 0 && $FILES_ONLY -eq 0 && $RAW_OUTPUT -eq 0 ]]; then
         echo -e "${C_CYAN}${C_BOLD}$file${C_RESET}"
      fi

      # Décompte ultra-rapide des occurrences par fonction dans le fichier
      while IFS= read -r fname; do
         [[ -z "$fname" ]] && continue
         if [[ -v COUNTS["$fname"] ]]; then
            COUNTS["$fname"]=$(( COUNTS["$fname"] + 1 ))
            TOTAL_MATCHES=$(( TOTAL_MATCHES + 1 ))
            FILE_COUNT=$(( FILE_COUNT + 1 ))
         fi
      done < <(grep -o -E "$PATTERN" "$file" 2>/dev/null | sed -E 's/[[:space:]]*\(//' || true)

      # Affichage des lignes avec numéros (uniquement si mode détaillé)
      if [[ $SUMMARY_ONLY -eq 0 && $FILES_ONLY -eq 0 && $RAW_OUTPUT -eq 0 ]]; then
         while IFS= read -r line; do
            LINE_NUM="${line%%:*}"
            LINE_CONTENT="${line#*:}"
            echo -e "  ${C_GREEN}${LINE_NUM}${C_RESET}: $LINE_CONTENT"
         done <<< "$MATCHES"
      fi

      if [[ $FILE_COUNT -gt 0 ]]; then
         MATCHED_FILES+=("$file")
         FILE_CALLS["$file"]=$FILE_COUNT
         if grep -qE "(USE_RAMFS|ramfs\.h)" "$file" 2>/dev/null; then
            FILE_EQUIPPED["$file"]=1
         else
            FILE_EQUIPPED["$file"]=0
         fi
      fi

      if [[ $SUMMARY_ONLY -eq 0 && $FILES_ONLY -eq 0 && $RAW_OUTPUT -eq 0 ]]; then
         echo ""
      fi
   fi
done

# Affichage du tableau récapitulatif des fonctions (sauf si mode fichiers uniquement ou brut)
if [[ $FILES_ONLY -eq 0 && $RAW_OUTPUT -eq 0 ]]; then
   echo "=============================================================================="
   echo -e "${C_BOLD}RÉCAPITULATIF DES OCCURRENCES TROUVÉES (Total : $TOTAL_MATCHES)${C_RESET}"
   echo "=============================================================================="
   printf "%-20s %s\n" "Fonction" "Nombre d'appels"
   echo "------------------------------------------------------------------------------"

   ANY_FOUND=0
   for f in "${SELECTED_FUNCS[@]}"; do
      CNT="${COUNTS["$f"]}"
      if [[ $CNT -gt 0 ]]; then
         printf "%-20s %b%d%b\n" "$f" "${C_BOLD}${C_YELLOW}" "$CNT" "${C_RESET}"
         ANY_FOUND=1
      fi
   done

   if [[ $ANY_FOUND -eq 0 ]]; then
      echo -e "${C_GREEN}Aucun appel d'E/S de fichier détecté dans les fichiers analysés.${C_RESET}"
   fi
   echo "=============================================================================="
fi

# Affichage des fichiers concernés
print_affected_files() {
   local total_files=${#MATCHED_FILES[@]}
   local needed_count=0
   local already_count=0

   for file in "${MATCHED_FILES[@]}"; do
      if [[ "${FILE_EQUIPPED["$file"]}" -eq 1 ]]; then
         already_count=$(( already_count + 1 ))
      else
         needed_count=$(( needed_count + 1 ))
      fi
   done

   if [[ $RAW_OUTPUT -eq 1 ]]; then
      for file in "${MATCHED_FILES[@]}"; do
         if [[ $TODO_ONLY -eq 1 && "${FILE_EQUIPPED["$file"]}" -eq 1 ]]; then
            continue
         fi
         echo "$file"
      done
      return
   fi

   echo ""
   echo "=============================================================================="
   echo -e "${C_BOLD}FICHIERS CONCERNÉS (Total : $total_files fichier(s) détecté(s))${C_RESET}"
   echo "=============================================================================="
   echo -e "Pour chaque fichier cible, ajouter en tête (après les en-têtes système <...>)"
   echo -e "et avant tout appel d'E/S :"
   echo -e "  ${C_CYAN}#define USE_RAMFS${C_RESET}"
   echo -e "  ${C_CYAN}#include \"ramfs.h\"${C_RESET}"
   echo "------------------------------------------------------------------------------"
   printf "  %-50s %-16s %s\n" "Fichier" "Appels d'E/S" "Statut"
   echo "------------------------------------------------------------------------------"

   local shown=0
   for file in "${MATCHED_FILES[@]}"; do
      if [[ $TODO_ONLY -eq 1 && "${FILE_EQUIPPED["$file"]}" -eq 1 ]]; then
         continue
      fi
      local calls="${FILE_CALLS["$file"]}"
      local status_str
      if [[ "${FILE_EQUIPPED["$file"]}" -eq 1 ]]; then
         status_str="${C_GREEN}[DÉJÀ ÉQUIPÉ]${C_RESET}"
      else
         status_str="${C_YELLOW}${C_BOLD}[À ÉQUIPER]${C_RESET}"
      fi
      printf "  %-50s %-16s %b\n" "$file" "($calls appel(s))" "$status_str"
      shown=$(( shown + 1 ))
   done

   if [[ $shown -eq 0 ]]; then
      if [[ $total_files -gt 0 && $TODO_ONLY -eq 1 ]]; then
         echo -e "  ${C_GREEN}Tous les fichiers concernés ($total_files) sont déjà équipés !${C_RESET}"
      else
         echo -e "  ${C_GREEN}Aucun fichier ne nécessite de redirection ramfs.${C_RESET}"
      fi
   fi
   echo "------------------------------------------------------------------------------"
   echo -e "Bilan : ${C_BOLD}${C_YELLOW}$needed_count fichier(s) à équiper${C_RESET}, ${C_GREEN}$already_count déjà équipé(s)${C_RESET}."
   echo "=============================================================================="
}

print_affected_files

