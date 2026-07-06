export type CourseIdentityInput = {
  id: string
  identityId: string
  markerIdentityId: string | null
  name: string
  path: string
  fingerprint: string | null
}

export type PersistedCourseIdentity = {
  id: string
  identity_id: string | null
  path: string
  fingerprint: string | null
  last_accessed: string | null
}

export type LessonIdentityInput = {
  name: string
  path: string
  relativePath: string | null
  sectionName: string | null
  type: string
  fileSize: number | null
}

export type PersistedLessonIdentity = {
  id: string
  course_id: string
  section_name: string | null
  name: string
  path: string
  relative_path: string | null
  type: string
  file_size: number | null
}

function firstUnclaimed<T extends { id: string }>(items: T[], claimedIds: Set<string>): T | null {
  const available = items.filter((item) => !claimedIds.has(item.id))
  return available.length === 1 ? available[0] : null
}

export function lessonIdentitySignature(input: {
  sectionName: string | null
  name: string
  type: string
  fileSize: number | null
}): string {
  return [
    input.sectionName ?? "",
    input.name,
    input.type,
    input.fileSize ?? 0,
  ].join("\u0000")
}

export function persistedLessonIdentitySignature(lesson: PersistedLessonIdentity): string {
  return lessonIdentitySignature({
    sectionName: lesson.section_name,
    name: lesson.name,
    type: lesson.type,
    fileSize: lesson.file_size,
  })
}

export function scannedLessonIdentitySignature(lesson: LessonIdentityInput): string {
  return lessonIdentitySignature({
    sectionName: lesson.sectionName,
    name: lesson.name,
    type: lesson.type,
    fileSize: lesson.fileSize,
  })
}

export function selectCourseIdentityMatch(
  course: CourseIdentityInput,
  exactPathMatch: PersistedCourseIdentity | undefined,
  identityMatches: PersistedCourseIdentity[],
  fingerprintMatches: PersistedCourseIdentity[],
  claimedCourseIds: Set<string>
): { match: PersistedCourseIdentity | null; warning: string | null } {
  if (exactPathMatch) {
    claimedCourseIds.add(exactPathMatch.id)
    return { match: exactPathMatch, warning: null }
  }

  if (course.markerIdentityId) {
    const available = identityMatches.filter((candidate) => !claimedCourseIds.has(candidate.id))

    if (available.length === 1) {
      claimedCourseIds.add(available[0].id)
      return { match: available[0], warning: null }
    }

    if (available.length > 1) {
      return {
        match: null,
        warning: `Skipped marker identity for "${course.name}": multiple existing courses have the same marker identity.`,
      }
    }

    if (identityMatches.length > 0) {
      return {
        match: null,
        warning: `Skipped marker identity for "${course.name}": that marker identity was already used by another scanned course.`,
      }
    }
  }

  if (!course.fingerprint) return { match: null, warning: null }

  const available = fingerprintMatches.filter((candidate) => !claimedCourseIds.has(candidate.id))

  if (available.length === 1) {
    claimedCourseIds.add(available[0].id)
    return { match: available[0], warning: null }
  }

  if (available.length > 1) {
    return {
      match: null,
      warning: `Skipped progress reuse for "${course.name}": multiple existing courses have the same fingerprint.`,
    }
  }

  if (fingerprintMatches.length > 0) {
    return {
      match: null,
      warning: `Skipped progress reuse for "${course.name}": a matching course fingerprint was already used by another scanned course.`,
    }
  }

  return { match: null, warning: null }
}

export function selectLessonIdentityMatch<T extends PersistedLessonIdentity>(
  courseName: string,
  courseId: string,
  lesson: LessonIdentityInput,
  exactPathMatch: T | undefined,
  relativePathMatches: T[],
  signatureMatches: T[],
  claimedLessonIds: Set<string>
): { match: T | null; warning: string | null } {
  if (exactPathMatch?.course_id === courseId && !claimedLessonIds.has(exactPathMatch.id)) {
    claimedLessonIds.add(exactPathMatch.id)
    return { match: exactPathMatch, warning: null }
  }

  const relative = firstUnclaimed(relativePathMatches, claimedLessonIds)
  if (relative) {
    claimedLessonIds.add(relative.id)
    return { match: relative, warning: null }
  }

  if (relativePathMatches.filter((candidate) => !claimedLessonIds.has(candidate.id)).length > 1) {
    return {
      match: null,
      warning: `Skipped progress reuse for lesson "${lesson.name}" in "${courseName}": multiple existing lessons share the same relative path.`,
    }
  }

  const signature = firstUnclaimed(signatureMatches, claimedLessonIds)
  if (signature) {
    claimedLessonIds.add(signature.id)
    return { match: signature, warning: null }
  }

  if (signatureMatches.filter((candidate) => !claimedLessonIds.has(candidate.id)).length > 1) {
    return {
      match: null,
      warning: `Skipped progress reuse for lesson "${lesson.name}" in "${courseName}": multiple existing lessons have the same metadata.`,
    }
  }

  return { match: null, warning: null }
}
