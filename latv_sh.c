/*
 * latv-sh — a minimal POSIX shell.
 *
 * Single translation unit, no dependencies beyond POSIX.1-2008. The design
 * trade-offs behind the process, signal, and descriptor handling are written
 * up in docs/phase1-architecture.md; comments here record only the invariants
 * that are easy to break and expensive to debug.
 *
 * Build:  gcc -std=c99 -Wall -Wextra -pedantic -O2 -o latv-sh latv_sh.c
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROMPT      "latv-sh$ "
#define ARGV_INIT   8
#define STAGES_INIT 4

/*
 * Set by the SIGINT handler, observed by the read loop. The handler does
 * nothing else: anything beyond a sig_atomic_t store is unsafe in signal
 * context and would risk corrupting stdio state we depend on.
 */
static volatile sig_atomic_t interrupted;

/* $? — the status the last command returned; expands inside the tokenizer. */
static int last_status;

struct buf {                    /* growable string used while lexing */
    char  *s;
    size_t len;
    size_t cap;
};

struct segment {                /* one stage of a pipeline */
    char **argv;                /* argc entries, always NULL-terminated */
    int    argc;
    char  *in_file;             /* '<' target, or NULL */
    char  *out_file;            /* '>' target, or NULL */
};

struct builtin {
    const char       *name;
    int             (*run)(const struct segment *s);
};

static void  shell_init(void);
static void  on_sigint(int signo);
static int   read_line(char **line, size_t *cap);

static struct segment *parse_line(char *line, size_t *nstage);
static int   tokenize(char *text, struct segment *s);
static void  buf_putc(struct buf *b, char c);
static void  append_var(struct buf *b, const char *name, size_t nlen);

static int   run_command(struct segment *stages, size_t nstage);
static pid_t spawn_stage(const struct segment *s, int in_fd, int out_fd);
static void  child_prepare(const struct segment *s, int in_fd, int out_fd);
static int   reap_stages(size_t nstage, pid_t last_pid);
static int   redirect_push(const struct segment *s, int saved[2]);
static void  redirect_pop(const int saved[2]);
static int   bi_cd(const struct segment *s);
static int   bi_pwd(const struct segment *s);
static int   bi_exit(const struct segment *s);

static void *xmalloc(size_t n);
static void *xrealloc(void *p, size_t n);
static void  segments_free(struct segment *stages, size_t nstage);

static const struct builtin builtins[] = {
    { "cd",   bi_cd   },
    { "pwd",  bi_pwd  },
    { "exit", bi_exit },
    { NULL,   NULL    }
};

/*
 * OOM in a shell is fatal whatever we do; dying here with a message keeps
 * every call site free of NULL checks that would drown the real logic.
 */
static void *
xmalloc(size_t n)
{
    void *p = malloc(n);

    if (p == NULL) {
        perror("latv-sh: malloc");
        exit(1);
    }
    return p;
}

static void *
xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);

    if (q == NULL) {
        perror("latv-sh: realloc");
        exit(1);
    }
    return q;
}

static void
buf_putc(struct buf *b, char c)
{
    if (b->len + 1 >= b->cap) {            /* room for c and its NUL */
        b->cap = b->cap ? b->cap * 2 : 32;
        b->s = xrealloc(b->s, b->cap);
    }
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

/* Append the value of $name (or $?) to b; unknown names expand to nothing. */
static void
append_var(struct buf *b, const char *name, size_t nlen)
{
    char nm[256];
    char num[32];
    const char *val;
    size_t i;

    if (nlen == 1 && name[0] == '?') {
        snprintf(num, sizeof num, "%d", last_status);
        for (i = 0; num[i] != '\0'; i++)
            buf_putc(b, num[i]);
        return;
    }

    if (nlen >= sizeof nm)          /* longer than any real variable name */
        return;
    for (i = 0; i < nlen; i++)
        nm[i] = name[i];
    nm[nlen] = '\0';

    val = getenv(nm);
    if (val != NULL)
        for (i = 0; val[i] != '\0'; i++)
            buf_putc(b, val[i]);
}

static void
on_sigint(int signo)
{
    (void)signo;
    interrupted = 1;                /* just enough to break the prompt read */
}

static void
shell_init(void)
{
    struct sigaction sa;
    int fd;

    /*
     * A handler, not SIG_IGN: ignored dispositions survive execve, so a
     * shell that ignored SIGINT would hand unkillable SIGINT to every child
     * it ever spawned. Handlers are reset by the kernel on exec — and the
     * child still resets explicitly, so latv-sh behaves even when it is
     * itself launched from a context where SIGINT arrives ignored.
     * No SA_RESTART: the point of the handler is to interrupt the read.
     */
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = SIG_IGN;        /* Ctrl+\ has no business core-dumping */
    sigaction(SIGQUIT, &sa, NULL);

    /*
     * pipe() and open() hand back the lowest free descriptor; if stdio was
     * closed before we started, a pipe end would land on 0/1/2 and silently
     * become the terminal. Pin the three standard slots to /dev/null.
     */
    for (fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
        if (fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
            int n = open("/dev/null", O_RDWR);

            if (n >= 0) {
                dup2(n, fd);
                if (n != fd)
                    close(n);
            }
        }
    }
}

/*
 * 0 — a line was read, 1 — EOF, -1 — unrecoverable read error.
 * Reprints the prompt after an interrupted read rather than treating Ctrl+C
 * at an empty prompt as a reason to die.
 */
static int
read_line(char **line, size_t *cap)
{
    for (;;) {
        ssize_t n;

        if (isatty(STDIN_FILENO)) {
            if (interrupted)
                interrupted = 0;
            fputs(PROMPT, stderr);
        }

        errno = 0;                  /* getline leaves it alone at EOF */
        n = getline(line, cap, stdin);
        if (n >= 0) {
            interrupted = 0;
            return 0;
        }
        if (errno == EINTR) {
            clearerr(stdin);
            continue;
        }
        if (ferror(stdin))
            return -1;

        if (isatty(STDIN_FILENO))
            fputc('\n', stderr);    /* leave the prompt line tidy on ^D */
        return 1;
    }
}

/* Lexer states: between tokens, inside a token, and the four quoting modes. */
enum { ST_OUT, ST_WORD, ST_SQUOTE, ST_DQUOTE, ST_ESC, ST_DQESC };

/*
 * Hand the pending token over: redirect targets take it, everything else
 * goes to argv. An empty *unquoted* token — a variable that expanded to
 * nothing — is dropped rather than passed as a zero-length argument, so
 * `$NOTHING cmd` still runs `cmd`. Quoted empties ("") are real arguments
 * and survive.
 */
static void
store_word(struct segment *s, struct buf *w, int *pending, int quoted, int *cap)
{
    char *tok;

    if (w->len == 0 && !quoted) {
        free(w->s);
        w->s = NULL;
        w->len = w->cap = 0;
        return;
    }
    if (w->s == NULL) {                 /* quoted "" — build an empty token */
        w->s = xmalloc(1);
        w->s[0] = '\0';
    }
    tok = w->s;
    w->s = NULL;
    w->len = w->cap = 0;

    if (*pending != 0) {
        char **slot = (*pending == '>') ? &s->out_file : &s->in_file;

        free(*slot);                    /* a second '>' keeps the last one */
        *slot = tok;
        *pending = 0;
        return;
    }
    if (s->argc + 2 > *cap) {           /* one slot for the token, one for NULL */
        *cap = *cap ? *cap * 2 : ARGV_INIT;
        s->argv = xrealloc(s->argv, *cap * sizeof *s->argv);
    }
    s->argv[s->argc++] = tok;
    s->argv[s->argc] = NULL;            /* execvp contract, kept live */
}

/*
 * Lex one pipeline stage into argv/argc plus optional redirect targets.
 * Returns 0, or -1 after printing a syntax error and leaving `s` empty
 * (parse_line frees the stages it had already completed).
 */
static int
tokenize(char *text, struct segment *s)
{
    struct buf w = { NULL, 0, 0 };
    char *p = text;
    const char *err = NULL;
    int state = ST_OUT;
    int quoted = 0;                     /* current token saw a quote */
    int digits = 0;                     /* current token is literal digits only */
    int pending = 0;                    /* '>' or '<' awaiting its filename */
    int cap = 0;
    int j;

    s->argv = NULL;
    s->argc = 0;
    s->in_file = NULL;
    s->out_file = NULL;

    for (;;) {
        char c = *p;

        if (state == ST_OUT) {
            if (c == '\0')
                goto end;
            if (c == ' ' || c == '\t') {
                p++;
                continue;
            }
            if (c == '&') {
                err = "'&' control operator is not supported";
                goto fail;
            }
            if (c == '>' || c == '<') {
                if (c == p[1]) {        /* >> or << — name them, don't mis-lex */
                    err = (c == '>') ? "'>>' redirection is not supported"
                                     : "'<<' heredocs are not supported";
                    goto fail;
                }
                if (pending != 0) {
                    err = (pending == '>') ? "expected file name after '>'"
                                           : "expected file name after '<'";
                    goto fail;
                }
                pending = c;
                p++;
                continue;
            }
            /* Anything else starts a token; re-dispatch through ST_WORD. */
            quoted = 0;
            digits = (c >= '0' && c <= '9');
            state = ST_WORD;
            continue;
        }

        if (state == ST_WORD) {
            if (c == '\0')
                goto end;
            if (c == ' ' || c == '\t') {
                store_word(s, &w, &pending, quoted, &cap);
                state = ST_OUT;
                p++;
                continue;
            }
            if (c == '\'') {
                quoted = 1;
                digits = 0;
                state = ST_SQUOTE;
                p++;
                continue;
            }
            if (c == '"') {
                quoted = 1;
                digits = 0;
                state = ST_DQUOTE;
                p++;
                continue;
            }
            if (c == '\\') {
                digits = 0;
                state = ST_ESC;
                p++;
                continue;
            }
            if (c == '&') {
                err = "'&' control operator is not supported";
                goto fail;
            }
            if (c == '>' || c == '<') {
                if (digits) {           /* `2>` — fd redirection is out of scope */
                    err = "file descriptor redirection is not supported";
                    goto fail;
                }
                if (c == p[1]) {
                    err = (c == '>') ? "'>>' redirection is not supported"
                                     : "'<<' heredocs are not supported";
                    goto fail;
                }
                store_word(s, &w, &pending, quoted, &cap);
                pending = c;
                state = ST_OUT;
                p++;
                continue;
            }
            if (c == '$') {
                char *nm = p + 1;
                size_t nlen = 0;

                if (*nm == '?') {
                    nm++;
                    nlen = 1;
                } else if ((*nm >= 'a' && *nm <= 'z') || (*nm >= 'A' && *nm <= 'Z') ||
                           *nm == '_') {
                    nm++;
                    while ((*nm >= 'a' && *nm <= 'z') || (*nm >= 'A' && *nm <= 'Z') ||
                           (*nm >= '0' && *nm <= '9') || *nm == '_')
                        nm++;
                    nlen = nm - (p + 1);
                }
                if (nlen == 0) {        /* a lone '$' is just a dollar sign */
                    buf_putc(&w, '$');
                    p++;
                } else {
                    append_var(&w, p + 1, nlen);
                    p = nm;
                }
                digits = 0;
                continue;
            }
            buf_putc(&w, c);
            if (c < '0' || c > '9')
                digits = 0;
            p++;
            continue;
        }

        if (state == ST_SQUOTE) {
            if (c == '\0') {
                err = "unterminated single quote";
                goto fail;
            }
            if (c == '\'') {
                state = ST_WORD;
                p++;
                continue;
            }
            buf_putc(&w, c);            /* single quotes: literal, no $ or \ */
            p++;
            continue;
        }

        if (state == ST_DQUOTE) {
            if (c == '\0') {
                err = "unterminated double quote";
                goto fail;
            }
            if (c == '"') {
                state = ST_WORD;
                p++;
                continue;
            }
            if (c == '\\') {
                state = ST_DQESC;
                p++;
                continue;
            }
            if (c == '$') {
                char *nm = p + 1;
                size_t nlen = 0;

                if (*nm == '?') {
                    nm++;
                    nlen = 1;
                } else if ((*nm >= 'a' && *nm <= 'z') || (*nm >= 'A' && *nm <= 'Z') ||
                           *nm == '_') {
                    nm++;
                    while ((*nm >= 'a' && *nm <= 'z') || (*nm >= 'A' && *nm <= 'Z') ||
                           (*nm >= '0' && *nm <= '9') || *nm == '_')
                        nm++;
                    nlen = nm - (p + 1);
                }
                if (nlen == 0) {
                    buf_putc(&w, '$');
                    p++;
                } else {
                    append_var(&w, p + 1, nlen);
                    p = nm;
                }
                continue;
            }
            buf_putc(&w, c);
            p++;
            continue;
        }

        if (state == ST_ESC) {
            if (c == '\0') {            /* trailing backslash is a literal */
                buf_putc(&w, '\\');
                state = ST_WORD;
                goto end;
            }
            buf_putc(&w, c);            /* backslash escapes anything outside quotes */
            state = ST_WORD;
            p++;
            continue;
        }

        /* ST_DQESC: inside "…" a backslash only quotes $ ` " \ itself. */
        if (c == '\0') {
            err = "unterminated double quote";
            goto fail;
        }
        if (c == '$' || c == '`' || c == '"' || c == '\\')
            buf_putc(&w, c);
        else {
            buf_putc(&w, '\\');
            buf_putc(&w, c);
        }
        state = ST_DQUOTE;
        p++;
        continue;
    }

end:
    if (state == ST_WORD)
        store_word(s, &w, &pending, quoted, &cap);
    if (pending != 0)
        err = (pending == '>') ? "expected file name after '>'"
                               : "expected file name after '<'";
    if (err != NULL)
        goto fail;
    return 0;

fail:
    fprintf(stderr, "latv-sh: syntax error: %s\n", err);
    free(w.s);
    for (j = 0; j < s->argc; j++)
        free(s->argv[j]);
    free(s->argv);
    free(s->in_file);
    free(s->out_file);
    s->argv = NULL;
    s->argc = 0;
    s->in_file = NULL;
    s->out_file = NULL;
    return -1;
}

/*
 * Split the line on unquoted '|' and tokenize each stage. Returns the stage
 * array (with *nstage possibly 0 for an empty line), or NULL after printing
 * a syntax error — the two are distinguished so an empty line does not
 * clobber $?.
 */
static struct segment *
parse_line(char *line, size_t *nstage)
{
    struct segment *stages = xmalloc(STAGES_INIT * sizeof *stages);
    size_t cap = STAGES_INIT;
    size_t n = 0;
    size_t i;
    char *p = line;
    int sq = 0, dq = 0, esc = 0;

    *nstage = 0;

    for (;;) {
        char *start = p;
        int hit_pipe = 0;

        for (; *p != '\0'; p++) {
            char c = *p;

            if (esc) {
                esc = 0;
            } else if (dq) {
                if (c == '\\')
                    esc = 1;
                else if (c == '"')
                    dq = 0;
            } else if (sq) {
                if (c == '\'')
                    sq = 0;
            } else if (c == '\\') {
                esc = 1;
            } else if (c == '\'') {
                sq = 1;
            } else if (c == '"') {
                dq = 1;
            } else if (c == '|') {
                *p = '\0';              /* stage boundary, in place */
                hit_pipe = 1;
                break;
            }
        }

        if (n == cap) {
            cap *= 2;
            stages = xrealloc(stages, cap * sizeof *stages);
        }
        if (tokenize(start, &stages[n]) < 0) {
            segments_free(stages, n);
            return NULL;
        }
        n++;

        if (!hit_pipe)
            break;
        p++;
    }

    for (i = 0; i < n; i++) {
        /*
         * In a pipeline of multiple stages, every stage must have a command.
         * A stage with redirections but no command (e.g. `cmd | > file`)
         * cannot be execvp'd and is a syntax error.
         */
        if (n > 1 && stages[i].argc == 0) {
            fprintf(stderr, "latv-sh: syntax error near '|'\n");
            segments_free(stages, n);
            return NULL;
        }
        if (stages[i].argc != 0 || stages[i].in_file || stages[i].out_file)
            continue;
        if (n > 1) {                    /* `a | | b`, `| a`, `a |` */
            fprintf(stderr, "latv-sh: syntax error near '|'\n");
            segments_free(stages, n);
            return NULL;
        }
        *nstage = 0;                    /* blank or spaces-only line */
        return stages;
    }

    *nstage = n;
    return stages;
}

/*
 * Built-ins run in the parent, so a redirect around one is applied here:
 * save the current descriptor, point it at the target, and let the caller
 * restore it. A failure part-way through rolls back what was already done —
 * the caller's stdio is never left altered when the command doesn't run.
 */
static int
redirect_push(const struct segment *s, int saved[2])
{
    int fd;

    saved[0] = saved[1] = -1;

    if (s->in_file != NULL) {
        fd = open(s->in_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "latv-sh: %s: %s\n", s->in_file, strerror(errno));
            return -1;
        }
        saved[0] = dup(STDIN_FILENO);
        if (saved[0] < 0) {
            perror("latv-sh: dup");
            close(fd);
            return -1;
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("latv-sh: dup2");
            close(fd);
            close(saved[0]);
            saved[0] = -1;
            return -1;
        }
        close(fd);
    }

    if (s->out_file != NULL) {
        fd = open(s->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            fprintf(stderr, "latv-sh: %s: %s\n", s->out_file, strerror(errno));
            redirect_pop(saved);
            return -1;
        }
        saved[1] = dup(STDOUT_FILENO);
        if (saved[1] < 0) {
            perror("latv-sh: dup");
            close(fd);
            redirect_pop(saved);
            return -1;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("latv-sh: dup2");
            close(fd);
            close(saved[1]);
            saved[1] = -1;
            redirect_pop(saved);
            return -1;
        }
        close(fd);
    }

    return 0;
}

static void
redirect_pop(const int saved[2])
{
    if (saved[0] != -1) {
        if (dup2(saved[0], STDIN_FILENO) < 0)
            perror("latv-sh: dup2");
        close(saved[0]);
    }
    if (saved[1] != -1) {
        if (dup2(saved[1], STDOUT_FILENO) < 0)
            perror("latv-sh: dup2");
        close(saved[1]);
    }
}

/*
 * Everything a stage does between fork and exec. Never returns: a failure
 * here exits, because falling through would resume the shell's REPL on this
 * cloned stack — a second shell, running a second copy of the command tree.
 */
static void
child_prepare(const struct segment *s, int in_fd, int out_fd)
{
    int fd;
    int e;

    /*
     * POSIX.1-2008 sigaction: Dispositions are chosen, never inherited.
     * Ignored signals survive execve, so if we simply kept whatever we were
     * given, a latv-sh started with SIGINT ignored would spawn children that
     * Ctrl+C cannot touch. SIGQUIT too: its default is a core dump.
     * Source: https://pubs.opengroup.org/onlinepubs/9699919799/functions/sigaction.html
     */
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    if (in_fd != -1 && dup2(in_fd, STDIN_FILENO) < 0) {
        perror("latv-sh: dup2");
        _exit(1);
    }
    if (out_fd != -1 && dup2(out_fd, STDOUT_FILENO) < 0) {
        perror("latv-sh: dup2");
        _exit(1);
    }
    /*
     * Both ends close here, including the one just duplicated from: execvp
     * carries every open descriptor into the new image, and a stray pipe
     * write end anywhere in the pipeline keeps its reader from ever seeing
     * EOF. Descriptors land on 0/1/2 only if stdio was closed at startup —
     * shell_init pins those slots, so the unconditional close is safe.
     */
    if (in_fd > STDERR_FILENO)
        close(in_fd);
    if (out_fd > STDERR_FILENO)
        close(out_fd);

    /* Redirections open first so a failure aborts before stdio is re-pointed. */
    if (s->in_file != NULL) {
        fd = open(s->in_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "latv-sh: %s: %s\n", s->in_file, strerror(errno));
            _exit(1);
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("latv-sh: dup2");
            _exit(1);
        }
        if (fd != STDIN_FILENO)
            close(fd);
    }
    if (s->out_file != NULL) {
        fd = open(s->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            fprintf(stderr, "latv-sh: %s: %s\n", s->out_file, strerror(errno));
            _exit(1);
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("latv-sh: dup2");
            _exit(1);
        }
        if (fd != STDOUT_FILENO)
            close(fd);
    }

    execvp(s->argv[0], s->argv);

    /* execvp only gets here on failure. 127 — no such command in PATH;
     * 126 — it exists but can't be executed (permissions, it's a directory). */
    e = errno;
    if (e == ENOENT)
        fprintf(stderr, "latv-sh: %s: command not found\n", s->argv[0]);
    else
        fprintf(stderr, "latv-sh: %s: %s\n", s->argv[0], strerror(e));
    _exit(e == ENOENT ? 127 : 126);
}

static pid_t
spawn_stage(const struct segment *s, int in_fd, int out_fd)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("latv-sh: fork");
        return -1;
    }
    if (pid == 0) {
        child_prepare(s, in_fd, out_fd);
        _exit(127);                     /* unreachable; never return to the loop */
    }
    return pid;
}

/*
 * Reap every child this command spawned. waitpid(-1) collects them in
 * arbitrary order, so the status of last_pid — the pipeline's final stage,
 * hence $? — is picked up whenever it turns up; the others are reaped and
 * discarded. EINTR means Ctrl+C landed while we waited: the children are
 * dying too, so retry rather than abandon them as zombies.
 */
static int
reap_stages(size_t nstage, pid_t last_pid)
{
    int status = 0;
    int result = 1;
    size_t k;

    for (k = 0; k < nstage; k++) {
        pid_t pid;

        while ((pid = waitpid(-1, &status, 0)) < 0 && errno == EINTR)
            ;
        if (pid < 0)                    /* ECHILD — nothing left to reap */
            break;
        if (pid == last_pid) {
            if (WIFEXITED(status))
                result = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                result = 128 + WTERMSIG(status);    /* Ctrl+C → 130, etc. */
        }
    }

    return result;
}

static int
bi_cd(const struct segment *s)
{
    const char *dir;

    if (s->argc > 2) {
        fprintf(stderr, "latv-sh: cd: too many arguments\n");
        return 1;
    }
    dir = (s->argc == 2) ? s->argv[1] : getenv("HOME");
    if (dir == NULL) {
        fprintf(stderr, "latv-sh: cd: HOME not set\n");
        return 1;
    }
    if (chdir(dir) < 0) {
        fprintf(stderr, "latv-sh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    return 0;
}

/*
 * POSIX.1-2008 getcwd: get current working directory.
 * Runs in the parent process so it accurately inspects the shell's own cwd.
 * Source: https://pubs.opengroup.org/onlinepubs/9699919799/functions/getcwd.html
 */
static int
bi_pwd(const struct segment *s)
{
    char cwd[4096];
    (void)s;

    if (getcwd(cwd, sizeof cwd) == NULL) {
        fprintf(stderr, "latv-sh: pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(cwd);
    fflush(stdout);
    return 0;
}

/*
 * Deliberately conservative: a mistyped argument reports and returns rather
 * than killing the shell. `exit 300` wraps to 44 like every other shell.
 */
static int
bi_exit(const struct segment *s)
{
    int status = last_status;
    char *end;
    long v;

    if (s->argc > 2) {
        fprintf(stderr, "latv-sh: exit: too many arguments\n");
        return 1;
    }
    if (s->argc == 2) {
        v = strtol(s->argv[1], &end, 10);
        if (end == s->argv[1] || *end != '\0') {
            fprintf(stderr, "latv-sh: exit: %s: numeric argument required\n",
                    s->argv[1]);
            return 1;
        }
        status = (int)(v & 0xFF);
    }
    exit(status);
}

static int
run_command(struct segment *stages, size_t nstage)
{
    const struct builtin *b;
    int in_fd = -1;
    size_t spawned = 0;
    size_t i;
    pid_t last = -1;

    /*
     * Built-ins change the shell itself — cd moves *our* working directory,
     * exit must end *this* process — so they run here, never in a child
     * whose changes evaporate when it exits.
     */
    if (nstage == 1) {
        struct segment *s = &stages[0];

        if (s->argc == 0) {             /* bare `> file` — make the file, move on */
            int saved[2];

            if (redirect_push(s, saved) < 0)
                return 1;
            redirect_pop(saved);
            return 0;
        }

        for (b = builtins; b->name != NULL; b++)
            if (strcmp(b->name, s->argv[0]) == 0)
                break;
        if (b->name != NULL) {
            int saved[2];
            int rc;

            if (s->in_file == NULL && s->out_file == NULL)
                return b->run(s);
            /*
             * `cd /tmp > log`: re-point the descriptor, run, point it back —
             * the redirect works without swallowing our stdout for the rest
             * of the session.
             */
            if (redirect_push(s, saved) < 0)
                return 1;
            rc = b->run(s);
            redirect_pop(saved);
            return rc;
        }
    }

    /* Flush stdio streams before forking so children don't duplicate buffered output */
    fflush(stdout);
    fflush(stderr);

    /*
     * Adjacent-pipe construction: each pipe is born immediately before the
     * fork that needs it, and the parent's ends are closed in that same
     * iteration. Holding a write end past this point is what leaves a
     * reader blocked in read() forever — closing here makes that bug
     * structurally unreachable rather than merely unlikely.
     */
    for (i = 0; i < nstage; i++) {
        int pfd[2] = { -1, -1 };
        pid_t pid;

        if (i + 1 < nstage) {
            if (pipe(pfd) < 0) {
                perror("latv-sh: pipe");
                break;
            }
            /*
             * Stage i is forked while both ends are still open here, so its
             * copy of the read end escapes child_prepare's explicit closes.
             * Left in place, the writer never sees SIGPIPE when its consumer
             * exits — `yes | head -1` would spin forever. exec drops it.
             */
            if (fcntl(pfd[0], F_SETFD, FD_CLOEXEC) < 0) {
                perror("latv-sh: fcntl");
                close(pfd[0]);
                close(pfd[1]);
                pfd[0] = pfd[1] = -1;
                break;
            }
        }
        pid = spawn_stage(&stages[i], in_fd, pfd[1]);

        if (in_fd != -1)
            close(in_fd);
        if (pfd[1] != -1)
            close(pfd[1]);
        if (pid < 0) {                  /* stage missing — don't start the rest */
            if (pfd[0] != -1)
                close(pfd[0]);
            break;
        }
        spawned++;
        last = pid;
        in_fd = pfd[0];
    }
    if (in_fd != -1)
        close(in_fd);                   /* last read end: nothing else reads it */

    if (spawned == 0)
        return 1;
    return reap_stages(spawned, last);
}

static void
segments_free(struct segment *stages, size_t nstage)
{
    size_t i;
    int j;

    if (stages == NULL)
        return;
    for (i = 0; i < nstage; i++) {
        for (j = 0; j < stages[i].argc; j++)
            free(stages[i].argv[j]);
        free(stages[i].argv);
        free(stages[i].in_file);
        free(stages[i].out_file);
    }
    free(stages);
}

int
main(void)
{
    char *line = NULL;
    size_t cap = 0;

    shell_init();

    for (;;) {
        struct segment *stages;
        size_t nstage = 0;
        size_t len;
        int rc = read_line(&line, &cap);

        if (rc != 0) {
            if (rc < 0)                 /* hard read error, not EOF */
                last_status = 1;
            break;
        }

        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }

        stages = parse_line(line, &nstage);
        if (stages == NULL) {
            last_status = 2;            /* syntax error — bash's convention too */
            continue;
        }
        if (nstage > 0)
            last_status = run_command(stages, nstage);
        segments_free(stages, nstage);
    }

    free(line);
    return last_status;
}


