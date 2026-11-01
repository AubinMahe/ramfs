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
   echo "  -s, --summary     Affiche uniquement le tableau récapitulatif des occurrences"
   echo "  -h, --help        Affiche cette aide"
   echo ""
   echo "Exemples :"
   echo "  $0 /chemin/vers/code_legacy"
   echo "  $0 --summary ./legacy_src"
   echo "  $0 --file-only ./mon_module.c"
   exit 0
}

# Paramètres par défaut
TARGET_PATH="."
MODE="all"
SUMMARY_ONLY=0

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
      -s|--summary)
         SUMMARY_ONLY=1
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

# Première action demandée : lister les fonctions en tête
print_header

# Construction de la liste des fonctions selon le mode
SELECTED_FUNCS=()
case "$MODE" in
   fd)
      SELECTED_FUNCS=("${FD_FUNCS[@]}")
      echo -e "${C_YELLOW}Mode actif : Descripteurs (int fd) uniquement${C_RESET}"
      ;;
   file)
      SELECTED_FUNCS=("${FILE_FUNCS[@]}")
      echo -e "${C_YELLOW}Mode actif : Flux (FILE*) uniquement${C_RESET}"
      ;;
   all)
      SELECTED_FUNCS=("${FD_FUNCS[@]}" "${FILE_FUNCS[@]}" "${FS_FUNCS[@]}")
      echo -e "${C_YELLOW}Mode actif : Toutes les fonctions (fd, FILE*, chemins)${C_RESET}"
      ;;
esac

echo -e "${C_GRAY}Cible analysée : $TARGET_PATH${C_RESET}"
echo ""

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
      echo -e "${C_YELLOW}Aucun fichier source C/C++ trouvé dans '$TARGET_PATH'.${C_RESET}"
      exit 0
   fi
fi

# Tableau associatif pour compter les occurrences par fonction (Bash 4+)
declare -A COUNTS
for f in "${SELECTED_FUNCS[@]}"; do
   COUNTS["$f"]=0
done

TOTAL_MATCHES=0

if [[ $SUMMARY_ONLY -eq 0 ]]; then
   echo -e "${C_BOLD}Occurrences détaillées trouvées :${C_RESET}"
   echo "------------------------------------------------------------------------------"
fi

# Parcours des fichiers et extraction des lignes
for file in "${FILES[@]}"; do
   # Extraction des correspondances avec numéros de lignes
   MATCHES=$(grep -En "$PATTERN" "$file" 2>/dev/null || true)
   if [[ -n "$MATCHES" ]]; then
      if [[ $SUMMARY_ONLY -eq 0 ]]; then
         echo -e "${C_CYAN}${C_BOLD}$file${C_RESET}"
      fi
      while IFS= read -r line; do
         LINE_NUM="${line%%:*}"
         LINE_CONTENT="${line#*:}"
         
         # Identifier quelle(s) fonction(s) ont matché sur cette ligne pour le décompte
         for f in "${SELECTED_FUNCS[@]}"; do
            # Décompte du nombre de fois où la fonction apparaît sur la ligne
            OCCURRENCES=$(grep -o -E "\\b$f[[:space:]]*\\(" <<< "$LINE_CONTENT" | wc -l || true)
            if [[ $OCCURRENCES -gt 0 ]]; then
               COUNTS["$f"]=$(( COUNTS["$f"] + OCCURRENCES ))
               TOTAL_MATCHES=$(( TOTAL_MATCHES + OCCURRENCES ))
            fi
         done
         
         if [[ $SUMMARY_ONLY -eq 0 ]]; then
            # Coloration de la ligne : numéro en vert, code indenté
            echo -e "  ${C_GREEN}${LINE_NUM}${C_RESET}: $LINE_CONTENT"
         fi
      done <<< "$MATCHES"
      if [[ $SUMMARY_ONLY -eq 0 ]]; then
         echo ""
      fi
   fi
done

# Affichage du tableau récapitulatif
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

