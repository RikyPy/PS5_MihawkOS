/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_SHELL_LINK_H
#define PW_SHELL_LINK_H
#include <stddef.h>
#include <stdint.h>

/* Windows shortcuts (.lnk, [MS-SHLLINK]) written by the title, with no Wine running: one per catalog game in the
 * Mihawk OS prefix's <prefix>/drive_c/users/Public/Desktop/, synchronised at each desktop session start, before
 * Explorer starts (src/pw_desktop_session.h).
 *
 * What Wine reads, and therefore what is written (files and lines in .deps/wine-ps5/source; tools/check-shell-link.sh
 * reads sample bytes back through Wine's own IShellLinkW, under the host's Wine):
 *  - dlls/shell32/shelllink.c:716 IPersistStream_fnLoad: the 76-byte header (size 0x4C and CLSID_ShellLink, or the
 *    load fails), IconIndex from the header, then each part its flags name, in order. No LinkTargetIDList is
 *    written, so GetPath is the LinkInfo's LocalBasePath and IPersistFile::Load (:258) leaves it as is (no relative
 *    path is written either).
 *  - :617 Stream_LoadLocation, :544 Stream_ReadChunk, :601 Stream_LoadPath: the LinkInfo is one sized chunk; its
 *    7-DWORD header (0x1C) gives the VolumeID and LocalBasePath offsets; the path is read up to its NUL as CP_ACP
 *    (:609). So the target must be 7-bit ASCII (EINVAL otherwise): any other byte would decode by the prefix's
 *    code page. The layout is the one Wine's own writer makes (:908 Stream_WriteLocationInfo): VolumeID right
 *    after the header (DRIVE_FIXED, serial 0, empty label), the path, an empty CommonPathSuffix.
 *  - :487 Stream_LoadString: a uint16 count of UTF-16LE units, then the units, no terminator (Wine's writer adds
 *    one and counts it; the reader takes both). The count is multiplied by 2 in a USHORT (:503), so a string of
 *    more than 32767 units is misread (host Wine 11.19, a hand-made 32768-unit argument: read back empty, and the
 *    icon location read from inside it): PW_SHELL_LINK_UNITS_MAX, not 65535, is the limit (ENAMETOOLONG).
 *  - :854 a 4-byte zero terminal block (Wine only warns without it; written anyway).
 *
 * What Explorer requires (programs/explorer/desktop.c): add_folder (:546) lists *.lnk in the user's and the public
 * desktop folders; add_launcher (:363) keeps a launcher only when load_shelllink (:296) succeeds and extract_icon
 * (:322) finds an icon, in the icon location or else the raw path (with the same index), both read into MAX_PATH
 * buffers and passed through ExpandEnvironmentStringsW. A launcher without an icon is dropped silently. The icon
 * location is always written (the target when no icon path is given; Explorer's fallback would read the same) and
 * must be shorter than PW_SHELL_LINK_ICON_UNITS_MAX units (ENAMETOOLONG). So a target with no icon resource and no
 * icon_path gives no launcher at all (read, not run). The title is the file name up to its last dot (build_title,
 * :343); a double click is ShellExecute(path) (:835): shell32's lnkfile context-menu handler loads the link and
 * runs ShellExecuteEx on the LinkInfo path with the link's arguments and working directory; no LinkTargetIDList is
 * needed (shelllink.c:2450 ShellLink_InvokeCommand, sei.lpIDList NULL, no SEE_MASK_IDLIST; read, not run).
 *
 * Ownership (pw_shell_link_sync): the manifest lists, one file name per line, the shortcuts this module owns in the
 * folder. Only those are ever rewritten or removed; any other file there is the user's and is never touched, and a
 * catalog name that collides with one is a failure (EEXIST), not an overwrite. A listed name owns the file of that
 * exact spelling, or another spelling that is the same file (st_dev and st_ino: a file system that folds case or
 * normalization); on a case-sensitive one "Foo.lnk" listed owns nothing at "foo.lnk". Only a regular file is
 * rewritten or removed: a directory or symbolic link at an owned name is released, never followed or deleted. An
 * unowned file whose bytes are exactly the ones this module would write is adopted (counted unchanged). A name not
 * listed exactly is first appended to the existing manifest (a claim), so a sync that stops part way never leaves a
 * file it wrote unowned. The new manifest replaces the old one only when it was written whole and every entry's
 * ownership was read, and only such a one decides removals; otherwise the old one, with its claims, stays and the
 * next sync decides again. Manifest lines this module could not have written (not "*.lnk", or holding a byte
 * pw_shell_link_file_name drops) are ignored. "<folder>/.pw-shell-link.tmp" and "<manifest_path>.tmp" are this
 * module's: whatever is there is unlinked and the file made anew (O_EXCL), never written through.
 *
 * Errors are positive errno codes; nothing aborts. Portable: no allocation; only pw_shell_link_sync touches the file
 * system (POSIX open, read, write, fsync, close, rename, unlink, mkdir, stat, lstat). */

enum {
    PW_SHELL_LINK_HEADER_BYTES = 0x4C,
    PW_SHELL_LINK_INFO_HEADER_BYTES = 0x1C,  /* LinkInfoHeaderSize: no Unicode path offsets (Wine reads none) */
    PW_SHELL_LINK_VOLUME_BYTES = 0x11,       /* VolumeID: 16 bytes and an empty label's NUL */
    PW_SHELL_LINK_UNITS_MAX = 32767,         /* UTF-16 units of one string (shelllink.c:503) */
    PW_SHELL_LINK_ICON_UNITS_MAX = 260,      /* MAX_PATH: the icon location must be shorter (desktop.c:324) */
    PW_SHELL_LINK_NAME_MAX = 255,            /* bytes of a file name, ".lnk" included (NAME_MAX) */
    PW_SHELL_LINK_SYNC_BYTES = 8192,         /* the largest link pw_shell_link_sync writes (a stack buffer) */
};

/* LinkFlags ([MS-SHLLINK] 2.1.1; Wine's SLDF_* in include/shlobj.h). */
enum {
    PW_SHELL_LINK_HAS_ID_LIST = 0x01,
    PW_SHELL_LINK_HAS_LINK_INFO = 0x02,
    PW_SHELL_LINK_HAS_NAME = 0x04,
    PW_SHELL_LINK_HAS_RELATIVE_PATH = 0x08,
    PW_SHELL_LINK_HAS_WORKING_DIR = 0x10,
    PW_SHELL_LINK_HAS_ARGUMENTS = 0x20,
    PW_SHELL_LINK_HAS_ICON_LOCATION = 0x40,
    PW_SHELL_LINK_IS_UNICODE = 0x80,
};

/* One shortcut. Strings are UTF-8; target, working_directory and icon_path are absolute Windows paths such as
 * "C:\Games\Example Game\Example Game.exe". NULL or "" is absent, except target, which is required, ASCII and
 * drive-absolute ("X:\..." or "X:/..."). An absent icon_path means the target, with icon_index. */
typedef struct PwShellLink {
    const char *target;
    const char *arguments;
    const char *working_directory;
    const char *icon_path;
    int icon_index;
    const char *description;
} PwShellLink;

typedef struct PwShellLinkEntry {
    const char *display_name;  /* the desktop title; the file name is pw_shell_link_file_name's */
    PwShellLink link;
} PwShellLinkEntry;

typedef struct PwShellLinkSyncResult {
    unsigned written;          /* files created or rewritten */
    unsigned unchanged;        /* files that already held exactly these bytes */
    unsigned removed;          /* owned files no entry produces any more */
    unsigned failed;           /* entries and removals that failed; the sync went on past each */
    int first_error;           /* 0, or the errno code of the first failure */
    const char *first_error_step;  /* "" or "arguments", "folder", "name", "duplicate", "build", "read", "conflict",
                                      "write", "remove", "manifest" (an entry's: its ownership could not be read) */
    long first_error_entry;    /* the entry index of the first failure, -1 when it is not an entry's */
} PwShellLinkSyncResult;

#ifdef __cplusplus
extern "C" {
#endif

/* The .lnk bytes of link into out. 0; ENOSPC when capacity is too small, with *bytes the size needed (out may be
 * NULL when capacity is 0); EINVAL for a missing, non-ASCII or relative target or NULL arguments; EILSEQ for
 * invalid UTF-8; ENAMETOOLONG for a string over PW_SHELL_LINK_UNITS_MAX units or an icon location of
 * PW_SHELL_LINK_ICON_UNITS_MAX units or more. *bytes is set only on success and ENOSPC; out only on success. */
int pw_shell_link_build(const PwShellLink *link, unsigned char *out, size_t capacity, size_t *bytes);

/* "<display_name>.lnk", safe as a Windows file name: bytes below 0x20 and <>:"/\|?* are dropped, trailing dots and
 * spaces trimmed, nothing else changed. A name Wine takes for a device (ntdll/path.c:66 RtlIsDosDeviceName_U: the
 * part before the first dot, trailing spaces trimmed, is CON, PRN, AUX, NUL, COM1-9, LPT1-9, CONIN$ or CONOUT$, in
 * any case) gets " (game)" after that part: "con" is "con (game).lnk", "Nul.txt" is "Nul (game).txt.lnk". 0;
 * EINVAL when nothing is left or display_name is NULL; EILSEQ for invalid UTF-8; ENAMETOOLONG over
 * PW_SHELL_LINK_NAME_MAX bytes; ENOSPC when out (capacity, NUL included) is too small. */
int pw_shell_link_file_name(const char *display_name, char *out, size_t capacity);

/* Makes folder hold exactly one shortcut per entry, by the ownership rule above. folder is created when missing
 * (one level: its parent must exist). For each entry: a file already holding the built bytes is unchanged;
 * otherwise the bytes go to "<folder>/.pw-shell-link.tmp" (not *.lnk, so add_folder never lists it), are synced
 * and renamed over "<folder>/<file name>". Two entries whose file names match ignoring ASCII case, or that are one
 * file on a folding file system, are a failure for the later one (EEXIST, "duplicate"). Then every owned name the
 * new manifest does not keep is removed, unless it is a kept file under another spelling or not a regular file,
 * and the manifest is replaced last ("<manifest_path>.tmp", renamed). An owned entry that fails keeps its file and
 * its ownership, under the name the file has (a spelling differing only in ASCII case included), so the next sync
 * retries it; a removal that fails stays owned too. Each failure is counted and the sync goes on. No entries remove
 * every owned file.
 *
 * Returns result->first_error: 0 when every entry and removal succeeded. One caller at a time per folder. Meant
 * for before Explorer starts. With Explorer running, desktop.c:445 process_changes (read, not run) adds a launcher
 * on a rename's new name and removes one only for a removed or renamed-away name, so a rewritten shortcut would
 * show twice until Explorer restarts. Stack: about 16 KiB. */
int pw_shell_link_sync(const char *folder, const char *manifest_path, const PwShellLinkEntry *entries, size_t count,
                       PwShellLinkSyncResult *result);

#ifdef __cplusplus
}
#endif
#endif
