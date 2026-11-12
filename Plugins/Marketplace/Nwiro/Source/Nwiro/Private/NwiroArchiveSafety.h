// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Containment rules for anything unpacked from an archive.
 *
 * Kept in one place so the ONE correct predicate is shared rather than reimplemented.
 * The zip extractor used by asset import had no traversal guard at all, and it unpacks
 * archives fetched from renderer-supplied URLs (IK-142).
 *
 * TWO INDEPENDENT CHECKS, and both are wanted:
 *
 *  - IsSafeMemberName() rejects the entry NAME before any path is built. This is the
 *    primary control, because it can be applied to every entry up front, so a hostile
 *    archive is refused before a single byte is written.
 *  - IsContainedWithin() re-checks the RESOLVED path against the destination root. This
 *    catches anything the name check did not anticipate — odd normalisation, drive-relative
 *    forms, a caller that builds the path differently — and costs nothing.
 *
 * Neither is a substitute for the other: the first is early and cheap, the second is final
 * and absolute.
 */
namespace NwiroArchiveSafety
{
	/**
	 * Is this archive entry name safe to write under a destination directory?
	 *
	 * Rejects: empty names, any ".." SEGMENT (in either separator form), absolute paths
	 * (POSIX root, UNC, or drive-letter), and embedded control characters. Accepts ordinary
	 * relative names including nested ones, and filenames that merely contain dots.
	 *
	 * Segment-wise rather than a blunt Contains("..") so a legitimate "archive..backup.zip"
	 * is not refused while "a/../b" and a bare ".." are.
	 */
	bool IsSafeMemberName(const FString& EntryName);

	/**
	 * Does CandidatePath resolve to somewhere inside RootDir?
	 *
	 * Both sides are made absolute and collapsed first, so this is a decision about where
	 * the path actually LANDS rather than about how it is spelled. Fails closed when either
	 * side cannot be collapsed. Compared case-insensitively because the platform this
	 * guards is Windows, where two spellings of the same path are the same file.
	 */
	bool IsContainedWithin(const FString& RootDir, const FString& CandidatePath);
}
