# latv-sh

> *Latveria's processes obey. All of them. Always.*

**latv-sh** is a minimal Unix shell written in POSIX.1-2008 C99. One file, no
dependencies, no library beyond libc — every process it spawns is created,
wired, reaped, and accounted for by the shell itself. It exists as a study in
owning the process lifecycle completely: `fork`, `execvp`, `waitpid`,
`pipe`, `dup2`, `fcntl`, `getcwd`, and `sigaction`, used deliberately rather than by default.

```
$ ./latv-sh
latv-sh$ echo hello | wc -w
1
latv-sh$ exit
$
```

## What it does

- A quote-aware parser: whitespace splitting, `'…'` and `"…"`, backslash
  escapes, `$NAME` and `$?` expansion
- Pipelines of any length (`a | b | c`), built one descriptor at a time
- Input and output redirection (`<`, `>`)
- Built-ins `cd`, `pwd`, and `exit`, executed in the shell's own address space
- `Ctrl+C` interrupts the running command, never the shell
- Exit-status conventions you can script against: `127` command not found,
  `126` not executable, `128+signal` when a stage dies from a signal (e.g., `130` for `SIGINT`)

## Architecture

### The REPL

One loop, one input buffer, one ownership rule per iteration:

```
shell_init ──► PROMPT ──► READ ──► PARSE ──► DISPATCH ──► REAP ──► PROMPT …
                  ▲
                  └── EOF (Ctrl+D) ──► return last status
```

- **READ** — `getline()` into a growable buffer, so input length has no
  ceiling and the buffer-overflow class of bugs cannot exist. The prompt goes
  to stderr and only when stdin is a tty, which keeps `latv-sh > log` clean
  and lets `latv-sh < script.sh` run silently in batch mode. An interrupted
  read (Ctrl+C at an empty prompt) re-prompts instead of exiting.
- **PARSE** — a quote-aware scan splits the line on unquoted `|`; each stage
  is lexed by a small state machine into a NULL-terminated `argv` plus
  optional `<`/`>` targets. Unsupported syntax (`>>`, `2>`, `&`, unterminated
  quotes, stray `|`) is reported and sets `$?` to 2 — the shell stays alive.
- **DISPATCH** — built-ins that must touch the shell itself run in the
  parent; everything else becomes a forked pipeline (below).
- **REAP** — one `waitpid` loop collects every child, keeping the status of
  the pipeline's last stage as `$?`.

### Pipeline descriptor plumbing

For `cmd1 | cmd2`, each pipe is created immediately before the fork that
needs it, and the parent closes its ends inside that same iteration:

```
              parent              child A (cmd1)        child B (cmd2)
              ──────              ──────────────        ──────────────
 pipe()       3(r), 4(w)
 fork(A)      close(4) ────────►  dup2(4 → 1)
                                  close 3, 4
                                  exec cmd1
 fork(B)      close(3) ──────────────────────────────►  dup2(3 → 0)
                                                          close 3, 4
                                                          exec cmd2
 waitpid × 2
```

Two rules keep this deadlock-free:

1. **The parent never holds a write end during the run.** A pipe reaches EOF
   only when every write-side descriptor is closed system-wide; a leaked one
   leaves the reader blocked in `read()` forever and the shell frozen at a
   dead prompt.
2. **Nothing survives `execvp` that isn't stdio.** Children close both ends
   of every pipe they hold, and the read end of the *next* pipe is marked
   `FD_CLOEXEC` — otherwise the writer's own copy would suppress the SIGPIPE
   it needs when its consumer exits, and `yes | head -1` would spin forever.

### Signals

The terminal delivers Ctrl+C to the whole foreground process group — shell
and children alike — so survival is a matter of choosing dispositions
explicitly on both sides:

- The parent installs a one-assignment `SIGINT` handler *without*
  `SA_RESTART`, so the prompt read is broken but the shell is not.
- Children restore `SIGINT`/`SIGQUIT` to `SIG_DFL` via `sigaction` before `execvp`.
  This is mandatory: **ignored dispositions survive `execve`**, so a shell
  that expressed its own survival as `SIG_IGN` would produce children that
  Ctrl+C cannot touch.
- `SIGPIPE` is left at its default on purpose — `yes | head -1` killing its
  writer is correct POSIX behavior, not a bug.

## Build

Compile with `make` or directly using `gcc`:

```sh
make
```

or:

```sh
gcc -std=c99 -Wall -Wextra -pedantic -O2 -o latv-sh latv_sh.c
```

Warning-free under the flags above. `_POSIX_C_SOURCE 200809L` is defined at
the top of the source, which strict-ISO C99 mode requires to expose `getline`,
`sigaction`, and related POSIX functions.

## Testing

An automated acceptance test suite is included in `tests/test_runner.py`:

```sh
make test
```

The test runner verifies:
- Built-ins `cd` and `pwd` executed in parent memory space
- Multi-stage pipeline data flow (`seq 1 5 | grep 3 | wc -l`)
- Deadlock prevention and `SIGPIPE` writer exit (`yes | head -1`)
- Input and output redirection round-trips (`<`, `>`)
- Single quotes, double quotes, escape sequences, and variable expansion (`$USER`, `$?`)
- POSIX exit code conventions (`127` for command not found, `2` for syntax error)
- Zero zombie (`<defunct>`) processes across 50 consecutive pipelines
- Graceful syntax error recovery without crashing
- Interactive `Ctrl+C` (`SIGINT`) child termination (reporting status `130`)

## Terminal examples

Built-ins run in the parent, so `cd` and `pwd` reflect the shell's process state:

```
latv-sh$ cd /tmp
latv-sh$ pwd
/tmp
```

Single and chained pipelines:

```
latv-sh$ echo hello | wc -w
1
latv-sh$ seq 1 5 | grep 3 | wc -l
1
latv-sh$ printf 'c\nb\na\n' | sort | head -1
a
latv-sh$ yes | head -1
y
```

Redirection, both directions:

```
latv-sh$ echo decree > /tmp/doom.txt
latv-sh$ cat < /tmp/doom.txt
decree
```

Quoting and expansion:

```
latv-sh$ echo "a b" '$HOME' $NOT_SET
a b $HOME
latv-sh$ false
latv-sh$ echo $?
1
```

Ctrl+C touches the running command, not the shell:

```
latv-sh$ sleep 100
^C
latv-sh$ echo $?
130
latv-sh$ echo still_here
still_here
```

Failures report and return — the shell never dies on user error:

```
latv-sh$ no_such_cmd
latv-sh: no_such_cmd: command not found
latv-sh$ echo $?
127
latv-sh$ echo hi >> /tmp/x
latv-sh: syntax error: '>>' redirection is not supported
latv-sh$
```

## Non-goals

Stated openly rather than left to be discovered:

- Job control — no `&`, no `fg`/`bg`, no Ctrl+Z (`SIGTSTP` + `tcsetpgrp`)
- Globbing (`*`, `?`), tilde expansion, `$()` command substitution
- `>>`, `2>`, `>&` and other fd redirects, heredocs
- Aliases, history, line editing (input is raw `getline`, not readline)
- Script entry (`latv-sh file`) and built-ins beyond `cd`, `pwd`, and `exit`
- Subshell syntax `( … )` and compound commands

## Design notes

A few deliberate deviations and decisions from full POSIX shell semantics, chosen for a
smaller, auditable core:

- `pwd` is implemented as a built-in via `getcwd(3)` so it always reports the shell's current working directory directly.
- Multi-stage pipelines where a stage lacks a command (e.g. `cmd | > file`) are caught as syntax errors before fork.
- Expansion results are not re-split into fields: `$X` containing spaces stays one argument.
- `exit abc` reports an error and keeps the shell running, where bash would terminate.
- A built-in combined with redirection (`cd /tmp > log`) applies the redirection in the parent and restores it afterwards, so the redirect works without swallowing the shell's stdout permanently.
- An empty unquoted expansion (`$NOT_SET`) disappears; a quoted one (`""`) is preserved as a real empty argument.

## License

[MIT](LICENSE)
