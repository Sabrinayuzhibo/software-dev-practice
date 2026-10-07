---
name: huawei-coding
description: Apply the supplied Huawei programming standard when implementing or reviewing C++/Qt, TypeScript/Electron, and tests in this Backup System project.
---

# Huawei Coding Standard for Backup System

Source: [软件编程规范-华为.doc](../../../ref/软件编程规范-华为.doc),
《软件编程规范总则》第一版 (1999-02). Rule numbers below refer to that document;
its rules and recommendations sometimes reuse the same numbers. Read
[the adaptation notes](references/adaptation.md) when interpreting an old rule.

## Writing code

- Use four spaces, no tabs, one statement per line, braces for every control
  body, and separate independent blocks. C++ braces go on their own lines;
  wrap long expressions and parameters near 80 columns (1-1 through 1-11).
  `.clang-format` defines the C++ formatting. TypeScript keeps braces on the
  same line to respect JavaScript syntax, with four-space indentation.
- Preserve the established names: PascalCase types, camelCase functions and
  variables, trailing underscore for private C++ members, `kName` constants;
  wire fields retain existing snake_case (3-1 through 3-5). Avoid unexplained
  abbreviations and magic numbers (4-1, 4-2).
- Give modules one responsibility, functions one job, and split lengthy
  functions (6, recommendation 6-2 suggests at most 200 lines). Do not add
  abstractions or configuration for speculative extensions.
- Explain non-obvious contracts, ownership, failure behavior, and ordering
  beside the code (2-4 through 2-13, recommendations 2-2 and 2-3). Public
  interfaces need concise input/output and error descriptions. Keep comments
  current; do not restate simple assignments or add padding comments.
- Initialize state and validate data at external boundaries. Check file,
  socket, parsing, and persistence results; report errors with useful context
  (5-6, 6-1, 6-10/11 recommendations, 9-8 through 9-10).
- Use RAII for descriptors, files, threads, and temporary data (9-3 through
  9-7). Do not use assertions for invalid user input or normal I/O failures
  (7-7). Runtime checks must remain effective in release builds.
- Keep Qt objects in their owning thread. Shared mutable state needs a clear
  owner or synchronization (6-4); workers exit cooperatively (9-17).

## Backup-specific application

These are project requirements, not quotations from Huawei:

- Stream large files with bounded buffers; validate sizes and offsets before
  allocating or writing. Keep file bytes out of the renderer.
- Failed backups must not publish a version or alter previous backups.
  Publish only after content verification and durable writes.
- Restore only into a new/empty directory; reject source/repository overlap,
  traversal, symlink parents, and overwrites. Verify content before publishing
  each restored file. Report partial results after failure.
- Keep credentials out of logs and task files. Persist task changes and
  operation results atomically; never report success before saving succeeds.

## Verification

Enable normal compiler warnings; fix new warnings rather than disabling them
(10-1 through 10-3). Run `./scripts/build.sh`, relevant integration/GUI tests,
and `git diff --check`. Exercise normal operation, corrupt data, invalid paths,
source changes, interrupted transfer, and persistence/restart where affected
(11-3/4 and recommendations 11-3 through 11-6). Record commands and actual
results in `docs/test-report.md`; do not infer full coverage from passing tests.
