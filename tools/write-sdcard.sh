#!/usr/bin/env bash
#
# write-sdcard.sh — prepare a fresh microSD card for the FDC+ Serial Disk Server (macOS).
#
# It expects a newly FAT-formatted card that mounts as the default "NO NAME" volume, and:
#   1. aborts unless "/Volumes/NO NAME" is present (so it can't touch the wrong disk);
#   2. shows what is currently on the card and asks you to confirm before doing anything;
#   3. renames the volume to ESP32FDC;
#   4. erases everything already on the card;
#   5. copies the contents of the repo's SDCARD/ tree, skipping macOS junk
#      (.DS_Store, AppleDouble ._* sidecars, .Spotlight-V100, .fseventsd, .Trashes, …).
#
# It does NOT eject the card — flush is forced with sync and you eject from Finder.
# (Scripted diskutil eject on this setup wedges DiskArbitration on reinsert.)
#
# Usage:  tools/write-sdcard.sh
#
set -euo pipefail

VOLNAME="NO NAME"           # default label macOS gives a freshly FAT-formatted card
NEWNAME="ESP32FDC"          # label to give the prepared card
DSTVOL="/Volumes/$NEWNAME"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/SDCARD"

die() { echo "error: $*" >&2; exit 1; }

# Volume label of a mounted volume, by mount point (empty if it can't be read).
vol_label() {
    diskutil info "$1" 2>/dev/null \
        | awk -F':[[:space:]]+' '/^ *Volume Name:/ {print $2; exit}'
}

# --- sanity checks -----------------------------------------------------------
[ -d "$SRC" ] || die "source $SRC not found (run from the fdc-sds-pio checkout)"

# Find the card by its LABEL, not by assuming it sits at "/Volumes/NO NAME" — a
# second FAT card also called "NO NAME" mounts as "/Volumes/NO NAME 1", so the
# path is not a reliable handle for a given disk. Bail out unless exactly one
# volume is labelled "$VOLNAME", so we can never wipe the wrong disk.
matches=()
for mp in /Volumes/*; do
    [ -d "$mp" ] || continue
    [ "$(vol_label "$mp")" = "$VOLNAME" ] && matches+=("$mp")
done

case "${#matches[@]}" in
    0) die "no volume labelled \"$VOLNAME\" is mounted. Insert a freshly FAT-formatted
       card (it should mount as the default \"$VOLNAME\" volume). Nothing was changed." ;;
    1) SRCVOL="${matches[0]}" ;;
    *) { echo "error: more than one volume is labelled \"$VOLNAME\":" >&2
         for mp in "${matches[@]}"; do
             echo "         $mp  ($(diskutil info "$mp" | awk -F':[[:space:]]+' '/Device Identifier/{print $2}'))" >&2
         done
         echo "       Unmount all but the card you want to write, then re-run. Nothing was changed." >&2
         exit 1; } ;;
esac

DEVID="$(diskutil info "$SRCVOL" | awk -F':[[:space:]]+' '/Device Identifier/{print $2; exit}')"
DEVSIZE="$(diskutil info "$SRCVOL" | awk -F':[[:space:]]+' '/Disk Size/{print $2; exit}')"

# --- show the card and confirm ----------------------------------------------
echo "Found card at: $SRCVOL  ($DEVID, $DEVSIZE)"
echo
echo "Current contents:"
ls -la "$SRCVOL"
echo
echo "This will RENAME the volume to \"$NEWNAME\", ERASE everything above, and copy"
echo "the contents of $SRC onto it."
echo
printf 'Continue? [y/N] '
read -r reply
case "$reply" in
    y|Y|yes|YES) ;;
    *) echo "Aborted. Nothing was changed."; exit 1 ;;
esac

# --- rename ------------------------------------------------------------------
echo
echo "Renaming \"$VOLNAME\" -> \"$NEWNAME\"..."
diskutil rename "$SRCVOL" "$NEWNAME" >/dev/null
[ -d "$DSTVOL" ] || die "rename did not produce $DSTVOL"

# --- erase existing contents -------------------------------------------------
echo "Erasing existing files..."
# Turn Spotlight off for the volume first: while it is indexing it holds
# .Spotlight-V100 and SIP refuses to delete it ("Operation not permitted"),
# which under `set -e` aborts the whole run before anything is copied.
mdutil -i off "$DSTVOL" >/dev/null 2>&1 || true
mdutil -E    "$DSTVOL" >/dev/null 2>&1 || true
# Remove every top-level entry, but never let a still-protected macOS system
# dir (.Spotlight-V100, .fseventsd, .Trashes, …) abort us — those are excluded
# from the copy below and macOS regenerates them anyway.
find "$DSTVOL" -mindepth 1 -maxdepth 1 -print0 \
    | while IFS= read -r -d '' entry; do
        rm -rf "$entry" 2>/dev/null || echo "  (skipping protected ${entry##*/})"
    done

# --- copy SDCARD/ without Mac junk ------------------------------------------
echo "Copying $SRC/ -> $DSTVOL/ ..."
rsync -rt --modify-window=1 \
    --exclude='.DS_Store' --exclude='._*' \
    --exclude='.Spotlight-V100' --exclude='.fseventsd' \
    --exclude='.Trashes' --exclude='.TemporaryItems' \
    --exclude='.apDisk' \
    "$SRC/" "$DSTVOL/"

# belt-and-suspenders: strip any metadata that slipped through, then merge/clear sidecars
find "$DSTVOL" \( -name '._*' -o -name '.DS_Store' \) -delete 2>/dev/null || true
dot_clean -m "$DSTVOL" 2>/dev/null || true

# Clear the macOS bookkeeping dirs/files it allows us to. .fseventsd and the
# never-index marker are plain, removable entries; NOTE: .Spotlight-V100 is an
# mds-owned, rootless stub — macOS recreates it empty on every mount and refuses
# to delete it even with sudo, so a card written on macOS always carries that one
# empty dir. The firmware must therefore ignore dot-prefixed entries when
# enumerating disks.
rm -rf "$DSTVOL/.fseventsd" "$DSTVOL/.metadata_never_index" 2>/dev/null || true
sync

echo
echo "Done. \"$NEWNAME\" is ready at $DSTVOL."
echo "Eject it from Finder (or the desktop) before removing the card."
