/*
 * wildcard.h — case-insensitive filename glob for `dir`/`ls <spec>` (DESIGN.md §8.4).
 *
 * Pure and ESP-IDF-free (like protocol.h) so the matcher is unit-testable on the host.
 * `*` matches any run of characters (including none), `?` matches exactly one; matching
 * is case-insensitive to fit the CP/M / DOS `DIR *.BAT` expectation and FAT semantics.
 * Iterative with backtracking — no recursion, so it is safe on a small task stack.
 */
#ifndef FDCSDS_WILDCARD_H
#define FDCSDS_WILDCARD_H

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>

static inline bool wildcard_match_ci(const char *pat, const char *str)
{
    const char *star = NULL; /* position just after the last '*' seen in pat */
    const char *ss = NULL;   /* position in str where that '*' began matching */

    while (*str) {
        if (*pat == '?' ||
            tolower((unsigned char)*pat) == tolower((unsigned char)*str)) {
            ++pat;
            ++str;
        } else if (*pat == '*') {
            star = ++pat;   /* remember the resume point... */
            ss = str;       /* ...and where str stood */
        } else if (star) {
            pat = star;     /* backtrack: let '*' swallow one more char */
            str = ++ss;
        } else {
            return false;
        }
    }

    while (*pat == '*') {
        ++pat; /* trailing stars match the empty tail */
    }
    return *pat == '\0';
}

#endif /* FDCSDS_WILDCARD_H */
