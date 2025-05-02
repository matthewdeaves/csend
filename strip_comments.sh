#!/bin/bash

# strip_comments_inplace.sh
# Performs actions on C/C++ files (.c, .h) in specified directories:
# 1. Strip comments (default, use --backup).
# 2. Restore from backups (--restore).
# 3. Clean up backup files (--clean-backups).
#
# Uses gcc -P for stripping.
#
# WARNING: Stripping MODIFIES YOUR ORIGINAL SOURCE FILES without --backup.
#          Restoring OVERWRITES current files with backups.
#          Cleaning DELETES backup files permanently.

# --- Configuration ---
TARGET_DIRS=("shared" "posix" "classic_mac") # Directories to process
BACKUP_SUFFIX=".bak"                         # Suffix for backup files
DO_BACKUP=false                              # Flag for stripping: create backups
DO_RESTORE=false                             # Flag: restore from backups
DO_CLEAN_BACKUPS=false                       # Flag: delete backup files

# --- Helper Functions ---
show_help() {
    echo "Usage: $0 [options]"
    echo
    echo "Performs one of three actions on files within the directories:"
    printf "  - %s\n" "${TARGET_DIRS[@]}"
    echo
    echo "Actions (only one can be chosen):"
    echo "  Default: Strip C/C++ comments from .c/.h files, modifying IN PLACE."
    echo "           Requires 'gcc'. Use --backup for safety."
    echo "  -r, --restore: Restore original files from backups ('*${BACKUP_SUFFIX}')."
    echo "                 Overwrites current .c/.h files if backups exist."
    echo "  -c, --clean-backups: Delete backup files ('*${BACKUP_SUFFIX}')."
    echo "                       Leaves other files untouched."
    echo
    echo "Options for Stripping Mode:"
    echo "  -b, --backup: Create backup files ('*${BACKUP_SUFFIX}') before stripping."
    echo "                HIGHLY RECOMMENDED when stripping."
    echo
    echo "Other Options:"
    echo "  -h, --help: Display this help message."
    echo
    echo "WARNINGS:"
    echo " - Stripping without --backup permanently modifies originals."
    echo " - Restoring overwrites current files."
    echo " - Cleaning permanently deletes backup files."
}

# --- Argument Parsing ---
while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--backup)
            DO_BACKUP=true
            shift
            ;;
        -r|--restore)
            DO_RESTORE=true
            shift
            ;;
        -c|--clean-backups)
            DO_CLEAN_BACKUPS=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "Error: Unknown option: $1" >&2
            show_help
            exit 1
            ;;
    esac
done

# --- Action Validation ---
action_count=0
[ "$DO_RESTORE" = true ] && ((action_count++))
[ "$DO_CLEAN_BACKUPS" = true ] && ((action_count++))
# Stripping is the default if no other action is specified.
# --backup is an option *for* stripping, not a separate action.

if [ $action_count -gt 1 ]; then
    echo "Error: --restore and --clean-backups are mutually exclusive actions." >&2
    show_help
    exit 1
fi

if [ "$DO_BACKUP" = true ] && { [ "$DO_RESTORE" = true ] || [ "$DO_CLEAN_BACKUPS" = true ]; }; then
    echo "Error: --backup option is only applicable when stripping comments (the default action)." >&2
    echo "       It cannot be used with --restore or --clean-backups." >&2
    show_help
    exit 1
fi

# --- Restore Logic ---
if [ "$DO_RESTORE" = true ]; then
    echo "Starting backup restoration process..."
    restored_count=0
    failed_count=0

    for dir in "${TARGET_DIRS[@]}"; do
        if [ ! -d "$dir" ]; then
            echo "Info: Directory '$dir' not found, skipping for restore."
            continue
        fi

        echo "Processing directory for restore: $dir"
        found_in_dir=0
        find "$dir" -maxdepth 1 -name "*${BACKUP_SUFFIX}" -print0 | while IFS= read -r -d $'\0' backup_file; do
            found_in_dir=1
            original_file="${backup_file%${BACKUP_SUFFIX}}"

            if [ -z "$original_file" ] || [ "$original_file" == "$backup_file" ]; then
                 echo "    ERROR: Could not derive original filename for '$backup_file'. Skipping." >&2
                 ((failed_count++))
                 continue
            fi

            echo "  Found backup: $backup_file"
            echo "    Attempting to restore to: $original_file"

            if mv -f "$backup_file" "$original_file"; then
                echo "    Successfully restored '$original_file' from backup."
                ((restored_count++))
            else
                echo "    ERROR: Failed to restore '$original_file' from '$backup_file'. Backup file NOT deleted." >&2
                ((failed_count++))
            fi
        done
        # Report if no backups were found in a directory that exists
        if [ $found_in_dir -eq 0 ]; then
             echo "  No backup files ('*${BACKUP_SUFFIX}') found in '$dir'."
        fi
    done

    echo "------------------------------------"
    echo "Backup restoration process finished."
    echo "Successfully restored: $restored_count files."
    echo "Failed attempts: $failed_count files."
    if [ $failed_count -gt 0 ]; then
        echo "Warning: Some files could not be restored. Please review the errors above." >&2
        exit 1
    fi
    if [ $restored_count -eq 0 ] && [ $failed_count -eq 0 ]; then
        echo "Info: No backup files found to restore in the target directories."
    fi
    echo "Restore operation completed."
    exit 0
fi

# --- Clean Backups Logic ---
if [ "$DO_CLEAN_BACKUPS" = true ]; then
    echo "Starting backup cleanup process..."
    deleted_count=0
    failed_count=0

    echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    echo "!! WARNING: This will permanently delete all files ending with       !!"
    echo "!!          '${BACKUP_SUFFIX}' in the target directories.                 !!"
    echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    read -p "Are you sure you want to delete these backup files? (yes/NO): " confirm
    if [[ ! "$confirm" =~ ^[Yy][Ee][Ss]$ ]]; then
        echo "Operation cancelled by user."
        exit 0
    fi
    echo "Proceeding with backup file deletion..."


    for dir in "${TARGET_DIRS[@]}"; do
        if [ ! -d "$dir" ]; then
            echo "Info: Directory '$dir' not found, skipping for cleanup."
            continue
        fi

        echo "Processing directory for cleanup: $dir"
        found_in_dir=0
        # Use find ... -delete for efficiency, but loop for better reporting
        find "$dir" -maxdepth 1 -name "*${BACKUP_SUFFIX}" -print0 | while IFS= read -r -d $'\0' backup_file; do
             found_in_dir=1
             echo "  Deleting backup file: $backup_file"
             if rm -f "$backup_file"; then
                 ((deleted_count++))
             else
                 echo "    ERROR: Failed to delete '$backup_file'." >&2
                 ((failed_count++))
             fi
        done
         # Report if no backups were found in a directory that exists
        if [ $found_in_dir -eq 0 ]; then
             echo "  No backup files ('*${BACKUP_SUFFIX}') found to delete in '$dir'."
        fi
    done

    echo "------------------------------------"
    echo "Backup cleanup process finished."
    echo "Successfully deleted: $deleted_count backup files."
    echo "Failed attempts: $failed_count files."
    if [ $failed_count -gt 0 ]; then
        echo "Warning: Some backup files could not be deleted. Please review the errors above." >&2
        exit 1
    fi
     if [ $deleted_count -eq 0 ] && [ $failed_count -eq 0 ]; then
        echo "Info: No backup files found to delete in the target directories."
    fi
    echo "Cleanup operation completed."
    exit 0
fi


# --- Stripping Logic (Default Action) ---

# Pre-execution Checks for Stripping
if ! command -v gcc &> /dev/null; then
    echo "Error: 'gcc' command not found, but it is required for comment stripping." >&2
    echo "Please install gcc and ensure it's in your PATH." >&2
    exit 1
fi
echo "Info: Using 'gcc' for comment stripping."

if [ "$DO_BACKUP" = false ]; then
    echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    echo "!! WARNING: You are running comment stripping WITHOUT the --backup   !!"
    echo "!!          option. This will PERMANENTLY MODIFY your original .c    !!"
    echo "!!          and .h files. This action is IRREVERSIBLE without a      !!"
    echo "!!          version control system or prior backups.                 !!"
    echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    read -p "Are you absolutely sure you want to continue stripping comments? (yes/NO): " confirm
    if [[ ! "$confirm" =~ ^[Yy][Ee][Ss]$ ]]; then
        echo "Operation cancelled by user."
        exit 0
    fi
    echo "Proceeding with comment stripping without backups..."
else
    echo "Info: Backup enabled for stripping. Original files will be saved with '$BACKUP_SUFFIX' suffix."
fi

# Main Stripping Process
processed_count=0
failed_strip_count=0
skipped_backup_fail=0
temp_file=""

cleanup() {
    if [ -n "$temp_file" ] && [ -f "$temp_file" ]; then
        rm -f "$temp_file"
    fi
}
trap cleanup EXIT INT TERM

echo "Starting in-place comment stripping..."

for dir in "${TARGET_DIRS[@]}"; do
    if [ ! -d "$dir" ]; then
        echo "Info: Directory '$dir' not found, skipping for stripping."
        continue
    fi

    echo "Processing directory for stripping: $dir"

    find "$dir" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -print0 | while IFS= read -r -d $'\0' file; do
        if [[ "$file" == *"$BACKUP_SUFFIX" ]]; then
            continue # Skip actual backup files
        fi

        echo "  Processing file: $file"
        temp_file=$(mktemp)

        if [ "$DO_BACKUP" = true ]; then
            backup_file="${file}${BACKUP_SUFFIX}"
            if ! cp -p "$file" "$backup_file"; then
                echo "    ERROR: Failed to create backup '$backup_file'. Skipping modification for this file." >&2
                rm -f "$temp_file"
                ((skipped_backup_fail++))
                continue
            fi
        fi

        if gcc -P -fpreprocessed -dD -E "$file" -o "$temp_file" 2>/dev/null; then
            if ! mv "$temp_file" "$file"; then
                 echo "    ERROR: Failed to move temporary file to '$file'. Original might be lost if backup failed/disabled!" >&2
                 ((failed_strip_count++))
            else
                 ((processed_count++))
                 temp_file="" # Prevent cleanup trap removal
            fi
        else
            local gcc_exit_code=$?
            echo "    ERROR: gcc failed (exit code $gcc_exit_code) while processing '$file'. Original file NOT modified." >&2
            rm -f "$temp_file"
            ((failed_strip_count++))
            temp_file="" # Prevent cleanup trap removal
        fi
    done
done

# Final Stripping Summary
echo "------------------------------------"
echo "Comment stripping process finished."
echo "Successfully processed (modified): $processed_count files."
echo "Failed attempts (originals untouched): $failed_strip_count files."
if [ "$DO_BACKUP" = true ]; then
    echo "Skipped due to backup failure: $skipped_backup_fail files."
    echo "Backups were created with suffix: $BACKUP_SUFFIX"
fi
if [ $failed_strip_count -gt 0 ] || [ $skipped_backup_fail -gt 0 ]; then
    echo "Warning: Some files could not be processed or backed up. Please review the errors above." >&2
    exit 1
fi

echo "Stripping operation completed successfully."
exit 0