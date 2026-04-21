# PES1UG24CS445 — OS Orange Level: PES-VCS

**Name:** Shrihari Nagesh  
**SRN:** PES1UG24CS445  
**Section:** H  
**GitHub:** [PES1UG24CS445_OS_ORANGE_LEVEL](https://github.com/ShrihariNagesh/PES1UG24CS445_OS_ORANGE_LEVEL)

---

## Overview

This repository contains a from-scratch implementation of **PES-VCS**, a Git-like version control system written in C. It implements a content-addressable object store, staging area (index), tree objects, and commits — following the same core design principles as Git.

---

## Phase 1 — Object Store (`object.c`)

Implements `object_write` and `object_read`. Objects are stored under `.pes/objects/XX/YY...` using SHA-256 content hashing. Writes are atomic via temp-file-then-rename with `fsync`.

### Screenshot 1A — All Phase 1 Tests Passed
![Screenshot 1A](Screenshot%201A.png)

### Screenshot 1B — Sharded Object Store Structure
![Screenshot 1B](Screenshot%20%201B.png)

---

## Phase 2 — Tree Objects (`tree.c`)

Implements `tree_from_index` which recursively builds a tree hierarchy from the flat index, grouping entries by directory prefix and recursing into subdirectories.

### Screenshot 2A — All Phase 2 Tests Passed
![Screenshot 2A](Screenshot%202A.png)

### Screenshot 2B — xxd of a Tree Object (Binary Format)
![Screenshot 2B](Screenshot%202B.png)

---

## Phase 3 — Staging Area / Index (`index.c`)

Implements `index_load`, `index_save`, and `index_add`. The index is a text file storing mode, hash, mtime, size, and path for each staged file, enabling fast-diff without re-hashing.

### Screenshot 3A — pes init + pes add + pes status
![Screenshot 3A](Screenshot%203A.png)

### Screenshot 3B — cat .pes/index
![Screenshot 3B](Screenshot%203B.png)

---

## Phase 4 — Commits (`commit.c`)

Implements `commit_create` which builds a tree from the index, reads the current HEAD for the parent, assembles a `Commit` struct, serializes it, writes it as an object, and atomically updates HEAD.

### Screenshot 4A — pes log (Commit History)
![Screenshot 4A](Screenshot%204A.png)

### Screenshot 4B — Full Object Store After Commits
![Screenshot 4B](Screenshot%204B.png)

### Screenshot 4C — Integration Test
![Screenshot 4C](Screenshot%204C.png)

---

## Final Screenshots

![Final Screenshot 1](FINAL%20SCREENSHOT1.png)

![Final Screenshot 2](FINAL%20SCREENSHOT%202.png)

---

## Analysis — Phases 5 & 6

### Q5.1 — How would you implement `pes checkout <branch>`?

**What files change in `.pes/`:**

`HEAD` is updated to contain `ref: refs/heads/<branch>` (if switching to a branch) or the raw commit hash (if detaching). No other `.pes/` metadata changes — the branch file itself already exists and already points to the right commit.

**What must happen to the working directory:**

1. Resolve the target branch's commit → read its tree hash.
2. Resolve the current HEAD's commit → read its tree hash.
3. Walk both trees and compute a diff: files only in the target tree must be written; files only in the current tree must be deleted; differing files must be overwritten.
4. Read each target blob from the object store and write it to its working-directory path.

**Why this is complex:**

Conflict detection, directory creation/deletion, atomicity (aborting mid-checkout leaves an inconsistent state), and restoring executable bits from tree entry modes.

---

### Q5.2 — How to detect a "dirty working directory" conflict before checkout?

Using only the **index** and the **object store** — no re-hashing required:

1. For every index entry, call `stat()` on the working-directory file.
2. Compare `st_mtime` and `st_size` against stored values in the index entry.
3. If either differs, the file is dirty.
4. Compare the index blob hash against the target branch's tree blob hash for that path. If hashes differ AND the file is dirty → abort checkout.
5. Files modified in the working directory but identical in both branches do not block checkout.

---

### Q5.3 — What is "detached HEAD" and how do you recover commits made in that state?

**What it means:** Normally `HEAD` contains an indirect reference like `ref: refs/heads/main`. In detached HEAD state, `HEAD` contains a raw commit hash directly. No branch file tracks new commits.

**Recovery:** Before switching away, create a branch at the current HEAD:
If you've already switched away, the commits still exist in the object store — you need to know a hash in the chain and create a branch at it. Git's `reflog` solves this; PES-VCS does not have one.

---

### Q6.1 — Algorithm to find and delete unreachable objects

**Mark-and-sweep:**

1. **Collect roots:** every hash in `.pes/refs/heads/*`.
2. **Mark phase (BFS/DFS):** starting from each root commit, mark it reachable, follow its tree (mark all blobs and subtrees), follow its parent commit, recurse until no new objects are discovered. Data structure: hash set keyed on the 32-byte ObjectID for O(1) average lookup.
3. **Sweep phase:** walk every file under `.pes/objects/XX/...`. Reconstruct its hash from the path. If not in the reachable set → delete.

**Estimate for 100,000 commits / 50 branches:**

Assuming ~10 trees and ~20 blobs per commit: up to ~3.1 million objects to visit. Reachable set ≈ 3.1M × 32 bytes ≈ 100 MB of memory — acceptable on a modern machine.

---

### Q6.2 — Race condition between GC and a concurrent commit

**The race:** GC's sweep phase sees a newly written blob before it has been referenced by a commit → marks it unreachable → deletes it → the commit that references it is now permanently corrupted.

**How Git avoids it:** A **grace period** (default 2 weeks). During the sweep phase, Git only deletes objects whose on-disk `mtime` is older than the threshold. A freshly written blob has a recent mtime and is spared even if no commit points to it yet.
