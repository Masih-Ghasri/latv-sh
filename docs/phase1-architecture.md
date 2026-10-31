# latv-sh — Phase 1: Architectural Blueprint & Edge-Case Analysis

**Status:** awaiting review · **Target:** POSIX.1-2008 C99, single translation unit (`latv_sh.c`)
**Purpose of this document:** lock the design before any implementation code is written.

---

## 0. Scope decisions (locked for Phase 2)

| Area | Decision |
|---|---|
| Input | `getline()`-backed REPL; no fixed line cap |
| Tokenization | ASCII whitespace delimiters, `'…'` / `"…"`, backslash escapes, `$NAME` and `$?` expansion |
| Redirection | `>` (write/truncate) and `<` (read). `>>`, `2>`, `>&` are rejected with an explicit syntax error, never silently mis-parsed |
| Pipelines | `cmd1 | cmd2 | …`, arbitrary stage count, dynamic allocation |
| Built-ins | `cd`, `exit` (table-driven; adding `pwd` later is one row) |
| Process model | Foreground only. No `&`, no job control, no globbing, no `$(…)` substitution |
| Output | English (`docs/`, `README.md`); conversation with maintainer in Persian |

Deliberate POSIX deviation to document in the README: expansion results are **not**
re-split into fields (`x="a b"; echo $x` prints `a b` as one argument). This avoids
surprising whitespace injection and keeps the pipeline boundary the only word splitter.

---

## 1. Core lifecycle & architecture

### 1.1 The REPL loop

The shell is a single long-lived process whose life is one loop:

```
shell_init ──► PROMPT ──► READ ──► PARSE ──► DISPATCH ──► (REAP) ──► PROMPT …
                 ▲                                                               
                 └──────────────────── EOF (Ctrl+D) ──► return last_status
```

`shell_init` installs signal dispositions and nothing else; parsing runs on the
plain heap, no global parser state. Per iteration:

1. **Read.** `getline()` owns a growable buffer reused across iterations. Prompt is
   written to **stderr** and only when `isatty(STDIN_FILENO)` — so `latv-sh > log`
   never puts prompts in the log, and `latv-sh < script` runs silently in batch mode.
   On `EINTR` (Ctrl+C at the prompt) we clear the interrupt flag and re-prompt
   instead of exiting; on EOF we print a newline and return `last_status`.
2. **Strip.** Remove exactly one trailing `\n`. Nothing else is trimmed yet —
   whitespace-only input must be *detected*, not silently mutated (see §4.2).
3. **Parse.** Two passes, both in place over the input buffer:
   - *Pipeline split:* a quote-aware scan finds unquoted `|`, writes `'\0'` over
     each and collects segment pointers into a dynamically grown array.
   - *Segment tokenize:* each segment is lexed by the state machine in §1.2 into
     an `argv`/`argc` pair plus optional `<`/`>` targets.
4. **Dispatch.** Apply the invariant in §1.4, run, record `last_status`.
5. **Reap & free.** One `waitpid` loop, one free loop. The input buffer and the
   parsed segments have disjoint ownership; neither outlives the iteration.

### 1.2 Tokenization contract

The tokenizer's output must be exactly what `execvp` expects: a `char *` array of
`argc` entries followed by a `NULL`. The `NULL` terminator is an invariant owned by
one function (`tokenize`) and asserted there — it is never patched up downstream,
because `execvp` scans for `NULL` and a missing one reads freed memory.

Character-level state machine (word buffer `w` is a growable `struct buf`):

| State | On input | Action |
|---|---|---|
| OUTSIDE | space/tab | skip |
| OUTSIDE | `'` / `"` | enter quote state; mark `in_word` |
| OUTSIDE | `\` | next char is literal (ESC) |
| OUTSIDE | `\|` `>` `<` | emit operator token |
| OUTSIDE | `$` | expand into `w` |
| OUTSIDE | other | mark `in_word`, append, → WORD |
| WORD | space/tab | close token; → OUTSIDE |
| WORD | `'` / `"` / `\` | as above (quotes mid-word concatenate: `a"b c"d` → one token) |
| WORD | `\|` `>` `<` | close token, emit operator |
| WORD | `$` | expand into `w` |
| SQUOTE | `'` | → WORD; anything else literal, **no expansion** |
| DQUOTE | `"` | → WORD |
| DQUOTE | `\` | escapes only `$` `"` `\` `` ` ``; otherwise literal backslash |
| DQUOTE | `$` | expand |
| ESC | any | append literally, → prior state |

Three details that separate a working lexer from a broken one:

- **`in_word` is a boolean, not `w.len > 0`.** `echo ""` must yield one *empty*
  argument. Keying off buffer length silently drops empty words — the classic
  tokenizer bug.
- **Trailing backslash** (`echo a\`) has no char to escape; the backslash becomes
  a literal, matching POSIX.
- **Expansion** happens inside double quotes and unquoted only. `$NAME` matches
  `[A-Za-z_][A-Za-z0-9_]*`; `$?` expands to `last_status`; unset names expand to
  empty. Tokens are *copies* into `w` — the input buffer is scratch and reused
  next iteration, so no `argv` entry may ever point into it.

Redirect operators take the following token as their target; a missing target,
an operator with no segment (`| a`, `a | | b`, trailing `|`), or an unsupported
operator (`>>`, `2>`) is a syntax error: message to stderr, status `2`, shell
alive. Multiple redirects of one kind keep the **last** occurrence (bash does
the same).

### 1.3 Why built-ins must run in the parent

`chdir(2)` mutates the **calling process's** working directory — a per-process
kernel attribute (the `fs_struct` of that task), not shared state. `fork()`
gives the child a *copy*. If `cd` ran in a forked child:

1. the child's `chdir` succeeds,
2. the child exits and its address space — including its cwd reference — is destroyed,
3. the parent's prompt reappears with the *old* directory, and the user sees
   `cd /tmp` silently do nothing.

`exit` has the mirrored failure: in a child it terminates only the child. The
parent's `waitpid` reaps it and the REPL loops; the user typed `exit` and the
shell refused to die. The same argument covers any built-in whose purpose is to
modify the shell itself (a future `export`, `alias`, or `cd -`).

So the dispatch invariant is:

```
single segment  ∧  no redirection  ∧  argv[0] is a built-in   →  run in parent
anything else                                              →  fork path
```

**Built-in + redirection** (`cd /tmp > f`) is the awkward corner. Three options
existed: ignore the redirect (wrong output), fork (file created but cwd
unchosen — visible deviation from bash), or temporarily re-point the parent's
fd and restore it. We take the third: `dup` the current fd aside, `dup2` the
opened target in, run the built-in, `dup2` back. About fifteen lines, and
observable behavior matches bash for every built-in case we ship. `exit` under a
redirect terminates before the restore, which is harmless.

A built-in *inside* a pipeline (`cd /tmp | cat`) takes the fork path like any
other stage — matching POSIX and bash, where such a `cd` provably cannot persist.

### 1.4 Orchestration model (preview)

Execution is one function: build the pipeline, spawn every stage, close the
parent's ends at the earliest possible moment, wait `n` children, take the last
stage's status as `$?`. §2.1 gives the state machine and fd timeline.

---

## 2. System-call state machines & edge cases

### 2.1 Process orchestration — `fork` / `execvp` / `waitpid`

**Construction model (adjacent-pipe).** One pass over the stages, creating each
pipe immediately before the fork that needs it:

```
in_fd = -1                                  (stdin of first stage)
for i in 0 .. n-1:
    pipe → p[]          only if i < n-1     (pipe feeding stage i+1)
    fork
      child:  dup2(in_fd → 0, p[1] → 1)     whatever exists
              close every pipe fd it holds
              reset SIGINT/SIGQUIT → SIG_DFL
              open < / > targets, dup2 over the pipe ends (file wins, last wins)
              execvp  —  or perror + _exit(127|126)
      parent: close in_fd  and  p[1]        ← write end dies here, unconditionally
              in_fd = p[0]
              remember pid (the last one is the status source)
after the loop: close in_fd
```

The parent's closing discipline is the whole ballgame: the moment a child has
been forked, the parent's copy of that pipe's write end is dead weight, and its
read end after the next fork likewise. Transferring ownership stage by stage
means the parent *structurally* cannot forget — the closes live in the same loop
iteration as the fork they belong to.

**Child body.** In order: all `dup2`s, then close all pipe fds, then reset
signals, then apply file redirections (open first, so a failing `open` aborts
before any fd was re-pointed), then `execvp`. Failure paths must call **`_exit`,
never `return`**: the child is a clone of the shell, and returning would resume
the REPL on the child's stack — a second shell, executing a second copy of the
pipeline, forever. Status convention: `ENOENT` → 127 (not found), anything else
(`EACCES`, `EISDIR`, …) → 126 (found but not executable); messages go to stderr
so redirected output and piped data stay clean. `execvp` is chosen over `execve`
precisely so PATH search is the libc's problem — zero custom PATH walking.

**Reaping.** Every stage is waited for, every iteration:

```
for k = 1 .. n:
    while (waitpid(-1, &status, 0) == -1 && errno == EINTR) retry
```

- **Zombies** are exited children whose status nobody collected; the kernel keeps
  their process-table slot until the parent waits. The shell forks only
  foreground work, so a single count-driven loop before the next prompt makes
  zombies structurally impossible — including on partial failure: if stage 3 of 5
  fails to `fork`, stages 1–2 are still reaped before the shell continues. A shell
  that lives for days must not accumulate `<defunct>` rows.
- **`EINTR` is not a failure.** Our SIGINT handler deliberately lacks `SA_RESTART`
  (§2.2), so `waitpid` can be interrupted by Ctrl+C mid-wait. The retry loop is
  the correct response; treating `-1` as an error would drop children un-reaped.
- **Orphans** (parent died before waiting) are reparented to init/subreaper and
  reaped there. With no background jobs, the only realistic orphan source is the
  shell itself being killed while a pipeline runs — the stages keep running and
  finish under init, which is the correct outcome.
- **Status mapping.** `WIFEXITED` → `WEXITSTATUS`; `WIFSIGNALED` → `128 + WTERMSIG`
  (the bash convention — Ctrl+C over `sleep` must report `130`, which scripts
  actually compare against). Only the **last stage's** status becomes `$?`; the
  others are still reaped, just discarded.

`fork` returning `-1` (`EAGAIN`, insane stage counts): report with `perror`,
close whatever pipes exist, reap whatever was already spawned, skip the command,
keep the shell alive.

### 2.2 Signal handling — isolating the parent

**Delivery model.** The terminal driver, on the ISIG character (Ctrl+C), sends
`SIGINT` to the **foreground process group** — which, without job control, is the
shell *and every pipeline stage* (children inherit the group; we never call
`setpgid`/`tcsetpgrp`). Both die by default. The design goal: children die with
the conventional status, the parent does not die and does not lose its place in
the input line.

**Parent.** `sigaction(SIGINT, on_sigint, flags = 0)` — explicitly **no**
`SA_RESTART`, because the handler's job is to *interrupt* a blocking read at the
prompt. The handler body is one assignment: `interrupted = 1`. Nothing else —
no `printf`, no `errno` clobbering, nothing that is not async-signal-safe. The
REPL sees `getline` return `-1`/`EINTR`, notices the flag, reprints the prompt,
clears the flag, continues. `SIGQUIT` (Ctrl+`, whose default is a core dump)
gets the same treatment — interactive shells ignore it.

**Child.** Before `execvp`: `SIGINT` and `SIGQUIT` back to `SIG_DFL`. This is
mandatory, and the reason is the sharpest trap in this design:

> Dispositions set to *ignore* are inherited across `execve`; dispositions that
> were *caught* are reset to `SIG_DFL` by the kernel.

So if the shell expressed its own survival with `SIGINT = SIG_IGN`, that
ignorance would be inherited by every child *and survive the exec* — `sleep 300`
would become unkillable by Ctrl+C. Using a real handler avoids it, but relying
on the implicit reset is fragile (e.g. when `latv-sh` itself is launched from a
context where `SIGINT` starts ignored). The rule we write down: **both
dispositions are set explicitly in both processes; nothing is inherited on
purpose.**

**Timing edges.** Ctrl+C between `fork` and the child's reset kills the child
with default disposition before exec — harmless, the parent reaps `130`. Ctrl+C
during `waitpid` interrupts the parent, the retry loop re-enters, the child is
dying anyway, the wait completes. Ctrl+C with no child running breaks the prompt
read; nothing to reap. In every window the shell survives because its disposition
was installed before the loop started.

**SIGPIPE stays default.** `latv-sh: yes | head -1` *must* kill `yes` with
signal 13 (status 141) when `head` closes the pipe — that is correct POSIX
pipeline behavior, not a bug to defend against. The only theoretical casualty is
the shell's own writes to a dead pipe (prompt on a severed stderr); vanishingly
rare, and documented rather than defended.

Out of scope, stated plainly: Ctrl+Z (`SIGTSTP`) and `fg`/`bg` require terminal
process-group juggling (`tcsetpgrp`) — a job-control layer that Phase 1 does not
pretend to have.

### 2.3 File-descriptor plumbing

Annotated timeline for `cmd1 | cmd2` (`pipe()` returns fd 3 = read, fd 4 = write):

| Step | Parent | Stage 1 child | Stage 2 child | Held fds |
|---|---|---|---|---|
| `pipe()` | — | — | — | P:{3,4} |
| `fork` #1 | `close(4)` immediately; `in_fd=3` | `dup2(4→1)`; `close(3)`; `close(4)`; exec | — | P:{3} · A:{0,1} |
| `fork` #2 | `close(3)` after fork | — | `dup2(3→0)`; `close(3)`; `close(4)`; exec | P:{} · B:{0,1} |
| run | `waitpid` ×2 | writes on fd 1 | reads fd 0 → EOF when 4 is gone everywhere | — |

**Why the parent's `close(4)` is the load-bearing line.** A pipe reaches EOF for
its readers only when *every* write-end descriptor is closed, everywhere in the
system. If the parent forgets, `cmd2` blocks in `read(0)` forever after `cmd1`
exits, `waitpid` blocks behind it, and the shell appears frozen at a dead
prompt — no error, no timeout, just silence. The same reasoning condemns any
third process that happens to inherit fd 4 (which is why children close *both*
ends, including the one they just duplicated from — after `dup2`, the source is
redundant, and `execvp` would otherwise carry it into the new program image).
The invariant, written once:

> Across `execvp`, no process may hold a pipe descriptor it is not actively
> using. The parent holds none during the run; each child holds only its two
> redirected stdio slots.

Two fd-reuse edges worth naming, because they only fire when the shell is
launched with stdio already closed: `pipe()` may hand back fd 0 or 1, making
`dup2(fd, fd)` a no-op success and a later blind `close(fd)` a *close of stdio*.
Mitigation: every post-`dup2` close is guarded with `if (fd != target)`, which
costs nothing and removes the class.

**Pipe capacity is not the deadlock.** A writer blocking on a full 64 KiB pipe
buffer is normal backpressure and resolves as soon as the reader drains it —
both stages run concurrently, so no cycle exists. The hang we must engineer
against is exclusively the unclosed-write-end case above; `parent` never reads
from a pipe itself, so it cannot participate in a capacity stall.

**Redirection lifecycle.** `>` opens `O_WRONLY|O_CREAT|O_TRUNC` with mode `0666`
(umask applies, as intended), `<` opens `O_RDONLY`. Open happens *before* any
`dup2`, so failure (permission, missing directory, directory-as-target) leaves
the child's stdio untouched: `perror`-style message, command not run, status 1,
shell alive. After a successful `dup2`, the opened fd is closed — leaked into
the exec'd image it would outlive its purpose and, in the worst case, keep a
pipe alive. For built-ins the same open/dup2 sequence runs in the *parent* with
the previous fd saved and restored (§1.3).

**Leak audit, as a checklist for Phase 2 review:** (1) post-`dup2` source close,
guarded; (2) both pipe ends closed in every child before exec; (3) parent's write
end closed in the fork iteration, read end right after the last fork; (4) file
targets closed after `dup2`; (5) nothing above fd 2 open across `execvp`.

---

## 3. Data structures & function signatures

Single file, static linkage throughout, no header. Feature-test macro before any
include: with `-std=c99` glibc compiles in strict-ISO mode, which hides
`sigaction`, `getline`, and `strdup` unless `_POSIX_C_SOURCE 200809L` is defined
— the one build contract everything else depends on.

```c
#define _POSIX_C_SOURCE 200809L

struct buf {                    /* growable string; geometric growth */
    char  *s;
    size_t len;
    size_t cap;
};

struct segment {                /* one pipeline stage */
    char **argv;                /* argc entries + guaranteed NULL */
    int    argc;
    char  *in_file;             /* '<' target, or NULL */
    char  *out_file;            /* '>' target, or NULL */
};

struct builtin {                /* table-driven dispatch */
    const char       *name;
    int             (*run)(const struct segment *s);
};

/* --- lifecycle & signals --- */
static void  shell_init(void);
static void  on_sigint(int signo);              /* assigns sig_atomic_t only */
static int   read_line(char **line, size_t *cap); /* 0 ok · 1 EOF · -1 fatal */

/* --- parsing (owns all returned strings; input buffer is scratch) --- */
static struct segment *parse_line(char *line, size_t *nstage); /* NULL ⇒ syntax err */
static int   tokenize(char *text, struct segment *s);          /* 0 ok · -1 err */
static void  buf_putc(struct buf *b, char c);
static void  append_var(struct buf *b, const char *name, size_t nlen);

/* --- execution --- */
static int   run_command(struct segment *stages, size_t nstage);
static pid_t spawn_stage(const struct segment *s, int in_fd, int out_fd);
static void  child_prepare(const struct segment *s, int in_fd, int out_fd);
static int   reap_stages(size_t nstage, pid_t last_pid);   /* returns $? */
static int   redirect_push(const struct segment *s, int saved[2]);
static void  redirect_pop(const int saved[2]);
static int   bi_cd(const struct segment *s);
static int   bi_exit(const struct segment *s);

/* --- housekeeping --- */
static void *xmalloc(size_t n);                 /* OOM: perror + _exit(1) */
static void *xrealloc(void *p, size_t n);
static void  segments_free(struct segment *stages, size_t nstage);

static volatile sig_atomic_t interrupted;       /* set by handler, read by REPL */
static int   last_status;                       /* expands as $? */
```

Ownership rules, which the signatures encode:

- `parse_line` returns segments that **own** every string they hold
  (`argv` entries, redirect targets) — all copies, none aliasing the input
  buffer, which is reused on the next `getline`. `segments_free` releases one
  call's worth; the REPL calls it on success, on syntax error, and on the empty
  input path, so no iteration leaks.
- `read_line` keeps one buffer alive for the process lifetime (freed at exit);
  `getline` grows it, callers never see a fixed cap — the buffer-overflow class
  is removed by construction rather than defended against with length checks.
- `spawn_stage` never returns in the child: the child body ends in `execvp` or
  `_exit`. In the parent it returns a pid (or `-1`, already reported).
- No `MAX_ARGS` / `MAX_TOKENS` anywhere. Arbitrary-but-large limits are where
  academic shells develop real-world bugs; `xrealloc` growth has no ceiling but
  available memory.

Control flow of `main`, in prose because the body is Phase 2's work: init,
loop { read; on EOF return `last_status`; parse; on syntax error set status `2`
and continue; dispatch by the §1.4 invariant; free }, that is the entire program.

---

## 4. Anticipated pitfalls & mitigations

| # | Failure | Root cause | Mitigation |
|---|---|---|---|
| 1 | Long line truncation executes the *rest* of the input as a second command | fixed `fgets` buffer splitting mid-stream | `getline()`; growable buffer, no cap, no split |
| 2 | Empty / spaces-only / `a \|` / `\| a` lines fork garbage or crash | zero-token input reaches `execvp` | `nstage == 0` or `argc == 0` (without redirect) → skip silently; malformed pipeline → syntax error, status 2, no fork |
| 3 | `execvp` scans past `argv` into freed memory | missing trailing `NULL` | invariant owned and asserted inside `tokenize`: allocate `argc + 1`, store `argv[argc] = NULL` |
| 4 | Child returns from `spawn_stage` and re-runs the shell | `execvp` failure falls through | `perror`-style report then `_exit(127/126)`; `return` is a forbidden word in child bodies |
| 5 | `<defunct>` rows after every command | unreaped children | one count-driven `waitpid` loop in `run_command`, `EINTR`-retrying, on **all** exit paths including partial fork failure |
| 6 | Shell hangs silently at the prompt | parent (or a sibling) still holds a pipe write end; reader never sees EOF | closes live in the same loop iteration as their fork; children close both ends pre-exec; audit checklist §2.3 |
| 7 | Ctrl+C kills `latv-sh` | default disposition + fg-pgroup delivery | `sigaction` in `shell_init`, no `SA_RESTART`, one-assignment handler |
| 8 | Ctrl+C does *not* kill children | `SIG_IGN` inherited through `execve` | child explicitly restores `SIG_DFL`; never rely on inherited dispositions |
| 9 | `waitpid` `-1` mistaken for a real error → children dropped | `EINTR` from our own SIGINT handler | `while (… == -1 && errno == EINTR)`; only `ECHILD` ends the loop early |
| 10 | OOM crashes deep in the parser | unchecked `malloc`/`realloc` NULL | `xmalloc`/`xrealloc`: report and terminate — a shell that cannot allocate cannot run; call sites stay clean |
| 11 | RSS grows over hours of use | `argv` / redirect strings never freed | `segments_free` on every parse outcome; tokens are owned copies with a single owner |
| 12 | `argv[i]` dangles next iteration | pointers into the reused `getline` buffer | tokens are copied into `struct buf` at lex time; aliasing the input buffer is illegal by construction |
| 13 | `echo ""` loses its argument; `a"b c"d` splits | word-end keyed on buffer length | explicit `in_word` boolean independent of `w.len` |
| 14 | `'…$HOME…'` expands; `$` at end of input mis-reads | expansion not state-gated | `$` handled only in OUTSIDE/WORD/DQUOTE; SQUOTE is literal; name scan bounded by buffer end |
| 15 | `cd` error kills the shell, or `exit abc` exits with garbage | built-ins bypass the fork/`perror` safety net | built-ins return a status like any command: `chdir` failure → message + status 1 (shell continues); `exit` parses with `strtol` + full-string validation, non-numeric → error, **shell does not exit**; bare `exit` → `last_status`; `exit 300` → `& 0xFF` |
| 16 | Redirect failure still runs the command, or corrupts stdio | `dup2` before `open` | open first; on failure report + status 1, stdio untouched; parent-side save/restore for built-ins |
| 17 | Prompt pollutes redirected output | prompt on stdout | prompt → stderr, and only when stdin is a tty |
| 18 | `strtok` silently collapses delimiters and breaks empty tokens | reentrancy & delimiter-run semantics | single-pass manual scanner; `strtok`/`strsep` banned |
| 19 | `>>` mis-lexed as `>` `>` and creates a file named `>` | unsupported operator not recognized | lexer matches `>>`/`2>` explicitly and reports "unsupported redirection" |
| 20 | Missing POSIX declarations under `-std=c99 -pedantic` | strict-ISO mode hides POSIX.1 headers | `_POSIX_C_SOURCE 200809L` before the first include, enforced in Phase 2 build flags |

---

## 5. Non-goals (stated so the README can be honest)

Job control (`&`, `fg`, Ctrl+Z), globbing, tilde expansion, command substitution,
aliases, history/line editing (input is raw `getline`, not readline), scripts
(`latv-sh file` argv parsing), `>>`/`2>`/`>&`, and subshell syntax `(...)`.
Each is listed as a natural extension, not silently absent.

## 6. Phase 2 acceptance preview

The design is considered validated when these behave as specified: `cd /tmp` then
`pwd` shows `/tmp` (parent-space built-in); `sleep 100` + Ctrl+C leaves the shell
alive and `echo $?` prints `130`; `yes | head -1` finishes instantly with `$?` = 141
and no hang (write-end discipline); `cat < /etc/hosts > /tmp/x` round-trips;
`echo "a b" $USER '$HOME'` prints `a b <user> $HOME`; `ls | no_such_cmd` reports
127 on stderr without touching the redirected stdout; spaces-only input is a
no-op; `ps` shows no `<defunct>` after fifty pipelines; binary builds warning-free
under `-Wall -Wextra -pedantic`.
